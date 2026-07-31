#pragma once

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <propsys.h>
#include <propidl.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cwchar>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Render.h>

void NotifyVolumeBoostStatus(const wchar_t* title, const wchar_t* message);

// Simple diagnostic logger. Writes to AudioPlaybackConnector.log next to the
// exe so failures can be reported without a debugger.
static void AppendVolumeBoostLog(const wchar_t* line)
{
	try
	{
		wchar_t exePath[MAX_PATH] = {};
		const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return;

		wchar_t* slash = wcsrchr(exePath, L'\\');
		if (!slash)
			return;
		const size_t remaining = MAX_PATH - static_cast<size_t>(slash + 1 - exePath);
		wcscpy_s(slash + 1, remaining, L"AudioPlaybackConnector.log");

		wil::unique_hfile hFile(CreateFileW(exePath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!hFile)
			return;

		std::wstring entry(line);
		entry += L"\r\n";
		DWORD written = 0;
		WriteFile(hFile.get(), entry.c_str(), static_cast<DWORD>(entry.size() * sizeof(wchar_t)), &written, nullptr);
	}
	catch (...)
	{
	}
}

// Software gain stage for the Bluetooth A2DP Sink stream.
//
// Windows renders the incoming A2DP Sink stream to the default playback
// device at a fixed level that is often too quiet, and the phone's own
// volume slider does not affect it (AVRCP absolute volume is ignored).
// AudioPlaybackConnection itself exposes no volume API, so to make this
// stream louder independently of the system master volume we:
//   1. capture the "A2DP SNK" recording endpoint,
//   2. run it through an AudioGraph with a gain > 1.0,
//   3. play the boosted result on the default output device,
//   4. mute the system's original rendering of the same stream so the
//      user does not hear it twice.

class VolumeBoost
{
public:
	void Start(std::wstring_view deviceName)
	{
		if (m_active || m_starting)
			return;

		m_deviceName = std::wstring(deviceName);
		m_stopped = false;
		m_starting = true;
		StartAsync();
	}

	void SetGain(float gain)
	{
		if (m_gain == gain)
			return;

		m_gain = gain;
		if (m_active && m_outputNode)
		{
			try
			{
				m_outputNode.OutgoingGain(m_gain);
			}
			CATCH_LOG();
		}
	}

	void Stop()
	{
		m_stopped = true;
		m_active = false;
		m_starting = false;

		try
		{
			if (m_graph)
				m_graph.Stop();
		}
		CATCH_LOG();

		m_inputNode = nullptr;
		m_outputNode = nullptr;
		m_graph = nullptr;

		UnmuteOriginalSessions();
	}

	bool IsActive() const
	{
		return m_active;
	}

private:
	winrt::fire_and_forget StartAsync();
	winrt::Windows::Foundation::IAsyncOperation<bool> TryStartAsync();
	void MuteOriginalSessions();
	void UnmuteOriginalSessions();

	winrt::Windows::Media::Audio::AudioGraph m_graph{ nullptr };
	winrt::Windows::Media::Audio::AudioDeviceInputNode m_inputNode{ nullptr };
	winrt::Windows::Media::Audio::AudioDeviceOutputNode m_outputNode{ nullptr };
	std::vector<winrt::com_ptr<IAudioSessionControl>> m_mutedSessions;
	std::wstring m_deviceName;
	float m_gain = 1.0f;
	bool m_active = false;
	bool m_starting = false;
	bool m_stopped = false;
};

static bool ContainsIgnoreCase(std::wstring_view haystack, std::wstring_view needle)
{
	if (needle.empty())
		return true;
	if (haystack.size() < needle.size())
		return false;

	for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
	{
		if (_wcsnicmp(haystack.data() + i, needle.data(), needle.size()) == 0)
			return true;
	}
	return false;
}

static std::wstring GetDeviceId(IMMDevice* device)
{
	LPWSTR id = nullptr;
	if (FAILED(device->GetId(&id)))
		return {};

	std::wstring result(id ? id : L"");
	CoTaskMemFree(id);
	return result;
}

// Fallback: the A2DP Sink recording endpoint may be hidden from the Sound
// settings UI (Windows 11) and only becomes active while audio is streaming,
// so search ALL capture endpoints (any device state) through the low-level
// Core Audio API.
static std::wstring FindCaptureEndpointByName(std::wstring_view deviceName)
{
	std::wstring fallback;

	try
	{
		winrt::com_ptr<IMMDeviceEnumerator> enumerator;
		winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())));

		winrt::com_ptr<IMMDeviceCollection> collection;
		winrt::check_hresult(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED, collection.put()));

		UINT count = 0;
		winrt::check_hresult(collection->GetCount(&count));
		AppendVolumeBoostLog((L"VolumeBoost: WASAPI enumerated " + std::to_wstring(count) + L" capture endpoints").c_str());

		for (UINT i = 0; i < count; ++i)
		{
			winrt::com_ptr<IMMDevice> device;
			winrt::check_hresult(collection->Item(i, device.put()));

			DWORD state = 0;
			device->GetState(&state);

			std::wstring name;
			winrt::com_ptr<IPropertyStore> store;
			if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.put())))
			{
				PROPVARIANT var;
				PropVariantInit(&var);
				if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal)
					name = var.pwszVal;
				PropVariantClear(&var);
			}

			AppendVolumeBoostLog((L"VolumeBoost: WASAPI endpoint state=" + std::to_wstring(state) + L" name=" + name).c_str());

			if (!ContainsIgnoreCase(name, L"A2DP"))
				continue;

			if (ContainsIgnoreCase(name, deviceName))
			{
				AppendVolumeBoostLog((L"VolumeBoost: found endpoint by WASAPI (exact): " + name).c_str());
				return GetDeviceId(device.get());
			}
			if (fallback.empty())
				fallback = GetDeviceId(device.get());
		}
		if (!fallback.empty())
			AppendVolumeBoostLog(L"VolumeBoost: found endpoint by WASAPI (fallback name match)");
	}
	CATCH_LOG();

	return fallback;
}

static winrt::Windows::Devices::Enumeration::DeviceInformation FindCaptureInputDevice(std::wstring_view deviceName)
{
	winrt::Windows::Devices::Enumeration::DeviceInformation fallback{ nullptr };

	try
	{
		auto devices = winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(
			winrt::Windows::Media::Devices::MediaDevice::GetAudioCaptureSelector()).get();
		AppendVolumeBoostLog((L"VolumeBoost: WinRT enumerated " + std::to_wstring(devices.Size()) + L" capture endpoints").c_str());

		for (auto const& device : devices)
		{
			std::wstring name(device.Name());
			AppendVolumeBoostLog((L"VolumeBoost: WinRT endpoint name=" + name).c_str());
			if (!ContainsIgnoreCase(name, L"A2DP"))
				continue;

			if (ContainsIgnoreCase(name, deviceName))
			{
				AppendVolumeBoostLog((L"VolumeBoost: found endpoint by WinRT: " + name).c_str());
				return device;
			}
			if (!fallback)
				fallback = device;
		}

		if (fallback)
		{
			AppendVolumeBoostLog(L"VolumeBoost: found endpoint by WinRT (fallback name match)");
			return fallback;
		}
	}
	CATCH_LOG();

	// Fallback: the endpoint may be hidden from the WinRT device list
	// (Windows 11 hides it from the Sound settings UI), so search active
	// capture endpoints through the low-level Core Audio API and resolve the
	// id back to a DeviceInformation object.
	std::wstring id = FindCaptureEndpointByName(deviceName);
	if (!id.empty())
	{
		try
		{
			AppendVolumeBoostLog((L"VolumeBoost: found endpoint by WASAPI fallback, id: " + id).c_str());
			return winrt::Windows::Devices::Enumeration::DeviceInformation::CreateFromIdAsync(
				winrt::hstring(id.c_str())).get();
		}
		CATCH_LOG();
	}

	AppendVolumeBoostLog(L"VolumeBoost: no A2DP capture endpoint found");
	return nullptr;
}

winrt::fire_and_forget VolumeBoost::StartAsync()
{
	for (int attempt = 1; attempt <= 15 && !m_stopped; ++attempt)
	{
		if (attempt > 1)
		{
			AppendVolumeBoostLog((L"VolumeBoost: retrying (" + std::to_wstring(attempt) + L")...").c_str());
			co_await winrt::resume_after(std::chrono::seconds(3));
			if (m_stopped)
				return;
		}

		try
		{
			if (co_await TryStartAsync())
			{
				m_active = true;
				m_starting = false;
				AppendVolumeBoostLog(L"VolumeBoost: started");
				NotifyVolumeBoostStatus(L"Volume boost", L"Volume boost enabled");
				return;
			}
		}
		catch (winrt::hresult_error const& ex)
		{
			LOG_CAUGHT_EXCEPTION();
			AppendVolumeBoostLog((L"VolumeBoost: attempt threw: " + std::wstring(ex.message())).c_str());
		}
	}

	m_starting = false;
	AppendVolumeBoostLog(L"VolumeBoost: all attempts failed");
	NotifyVolumeBoostStatus(L"Volume boost", L"Volume boost failed, see AudioPlaybackConnector.log");
	Stop();
}

winrt::Windows::Foundation::IAsyncOperation<bool> VolumeBoost::TryStartAsync()
{
	try
	{
		auto inputDevice = FindCaptureInputDevice(m_deviceName);
		if (!inputDevice)
		{
			AppendVolumeBoostLog(L"VolumeBoost: A2DP SNK capture endpoint not found");
			co_return false;
		}

		// Reset any leftover nodes from a previous failed attempt.
		m_inputNode = nullptr;
		m_outputNode = nullptr;
		m_graph = nullptr;

		// Mute the system's original rendering of the stream before the
		// audio graph creates its own output session, so we never mute the
		// boosted copy. Only sessions that already exist (the connection's
		// stream session or our own process sessions) are muted here.
		MuteOriginalSessions();
		if (m_stopped)
			co_return false;

		winrt::Windows::Media::Audio::AudioGraphSettings settings(winrt::Windows::Media::Render::AudioRenderCategory::Media);
		auto createResult = co_await winrt::Windows::Media::Audio::AudioGraph::CreateAsync(settings);
		if (m_stopped)
			co_return false;
		if (createResult.Status() != winrt::Windows::Media::Audio::AudioGraphCreationStatus::Success)
		{
			AppendVolumeBoostLog(L"VolumeBoost: AudioGraph creation failed");
			co_return false;
		}
		m_graph = createResult.Graph();

		auto inputFormat = winrt::Windows::Media::MediaProperties::AudioEncodingProperties::CreatePcm(44100, 2, 16);
		auto inputResult = co_await m_graph.CreateDeviceInputNodeAsync(
			winrt::Windows::Media::Capture::MediaCategory::Media,
			inputFormat,
			inputDevice);
		if (inputResult.Status() != winrt::Windows::Media::Audio::AudioDeviceNodeCreationStatus::Success)
		{
			AppendVolumeBoostLog((L"VolumeBoost: input node failed at 44100, hr=" + std::to_wstring(static_cast<int32_t>(inputResult.ExtendedError()))).c_str());
			inputFormat = winrt::Windows::Media::MediaProperties::AudioEncodingProperties::CreatePcm(48000, 2, 16);
			inputResult = co_await m_graph.CreateDeviceInputNodeAsync(
				winrt::Windows::Media::Capture::MediaCategory::Media,
				inputFormat,
				inputDevice);
		}
		if (m_stopped)
			co_return false;
		if (inputResult.Status() != winrt::Windows::Media::Audio::AudioDeviceNodeCreationStatus::Success)
		{
			AppendVolumeBoostLog((L"VolumeBoost: input node failed at 48000, hr=" + std::to_wstring(static_cast<int32_t>(inputResult.ExtendedError()))).c_str());
			co_return false;
		}
		m_inputNode = inputResult.DeviceInputNode();

		auto outputResult = co_await m_graph.CreateDeviceOutputNodeAsync();
		if (m_stopped)
			co_return false;
		if (outputResult.Status() != winrt::Windows::Media::Audio::AudioDeviceNodeCreationStatus::Success)
		{
			AppendVolumeBoostLog((L"VolumeBoost: output node failed, hr=" + std::to_wstring(static_cast<int32_t>(outputResult.ExtendedError()))).c_str());
			co_return false;
		}
		m_outputNode = outputResult.DeviceOutputNode();

		m_inputNode.AddOutgoingConnection(m_outputNode);
		m_outputNode.OutgoingGain(m_gain);
		m_graph.Start();

		co_return true;
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		AppendVolumeBoostLog((L"VolumeBoost: TryStartAsync threw: " + std::wstring(ex.message())).c_str());
		co_return false;
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		AppendVolumeBoostLog(L"VolumeBoost: TryStartAsync threw");
		co_return false;
	}
}

void VolumeBoost::MuteOriginalSessions()
{
	UnmuteOriginalSessions();

	try
	{
		winrt::com_ptr<IMMDeviceEnumerator> enumerator;
		winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())));

		winrt::com_ptr<IMMDevice> device;
		winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()));

		winrt::com_ptr<IAudioSessionManager2> sessionManager;
		winrt::check_hresult(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, sessionManager.put_void()));

		winrt::com_ptr<IAudioSessionEnumerator> sessionEnumerator;
		winrt::check_hresult(sessionManager->GetSessionEnumerator(sessionEnumerator.put()));

		int count = 0;
		winrt::check_hresult(sessionEnumerator->GetCount(&count));

		const DWORD ourPid = GetCurrentProcessId();
		for (int i = 0; i < count; ++i)
		{
			winrt::com_ptr<IAudioSessionControl> control;
			if (FAILED(sessionEnumerator->GetSession(i, control.put())))
				continue;

			std::wstring displayName;
			LPWSTR name = nullptr;
			if (SUCCEEDED(control->GetDisplayName(&name)) && name)
			{
				displayName = name;
				CoTaskMemFree(name);
			}

			DWORD pid = 0;
			auto control2 = control.try_as<IAudioSessionControl2>();
			if (control2)
				control2->GetProcessId(&pid);

			const bool isOurProcess = (pid == ourPid);
			const bool isDeviceStream = ContainsIgnoreCase(displayName, m_deviceName) || ContainsIgnoreCase(displayName, L"A2DP");
			if (!isOurProcess && !isDeviceStream)
				continue;

			auto volume = control.try_as<ISimpleAudioVolume>();
			if (volume && SUCCEEDED(volume->SetMute(TRUE, nullptr)))
			{
				float level = 0.0f;
				BOOL isMuted = FALSE;
				volume->GetMasterVolume(&level);
				volume->GetMute(&isMuted);
				AppendVolumeBoostLog((L"VolumeBoost: session pid=" + std::to_wstring(pid) +
					L" vol=" + std::to_wstring(static_cast<double>(level)) +
					L" muted=" + std::to_wstring(isMuted) +
					L" name=" + displayName).c_str());
				m_mutedSessions.push_back(std::move(control));
			}
		}

		AppendVolumeBoostLog((L"VolumeBoost: muted " + std::to_wstring(m_mutedSessions.size()) + L" session(s)").c_str());
	}
	CATCH_LOG();
}

void VolumeBoost::UnmuteOriginalSessions()
{
	for (auto& control : m_mutedSessions)
	{
		auto volume = control.try_as<ISimpleAudioVolume>();
		if (volume)
			volume->SetMute(FALSE, nullptr);
	}
	m_mutedSessions.clear();
}
