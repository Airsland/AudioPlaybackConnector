#pragma once

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <propsys.h>
#include <propidl.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Media.Render.h>

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
// settings UI (Windows 11), so also search active capture endpoints through
// the low-level Core Audio API.
static std::wstring FindCaptureEndpointByName(std::wstring_view deviceName)
{
	std::wstring fallback;

	try
	{
		winrt::com_ptr<IMMDeviceEnumerator> enumerator;
		winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())));

		winrt::com_ptr<IMMDeviceCollection> collection;
		winrt::check_hresult(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.put()));

		UINT count = 0;
		winrt::check_hresult(collection->GetCount(&count));

		for (UINT i = 0; i < count; ++i)
		{
			winrt::com_ptr<IMMDevice> device;
			winrt::check_hresult(collection->Item(i, device.put()));

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

			if (!ContainsIgnoreCase(name, L"A2DP"))
				continue;

			if (ContainsIgnoreCase(name, deviceName))
				return GetDeviceId(device.get());
			if (fallback.empty())
				fallback = GetDeviceId(device.get());
		}
	}
	CATCH_LOG();

	return fallback;
}

static std::wstring FindCaptureEndpoint(std::wstring_view deviceName)
{
	std::wstring fallback;

	try
	{
		auto devices = winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(
			winrt::Windows::Media::Devices::MediaDevice::GetAudioCaptureSelector()).get();

		for (auto const& device : devices)
		{
			std::wstring name(device.Name());
			if (!ContainsIgnoreCase(name, L"A2DP"))
				continue;

			if (ContainsIgnoreCase(name, deviceName))
				return std::wstring(device.Id());
			if (fallback.empty())
				fallback = std::wstring(device.Id());
		}

		if (!fallback.empty())
			return fallback;
	}
	CATCH_LOG();

	return FindCaptureEndpointByName(deviceName);
}

winrt::fire_and_forget VolumeBoost::StartAsync()
{
	try
	{
		auto captureId = FindCaptureEndpoint(m_deviceName);
		if (captureId.empty())
		{
			OutputDebugStringW(L"VolumeBoost: A2DP SNK capture endpoint not found.\n");
			Stop();
			return;
		}

		// Mute the system's original rendering of the stream before the
		// audio graph creates its own output session, so we never mute the
		// boosted copy. Only sessions that already exist (the connection's
		// stream session or our own process sessions) are muted here.
		MuteOriginalSessions();
		if (m_stopped)
			return;

		winrt::Windows::Media::Audio::AudioGraphSettings settings(winrt::Windows::Media::Render::AudioRenderCategory::Media);
		auto createResult = co_await winrt::Windows::Media::Audio::AudioGraph::CreateAsync(settings);
		if (m_stopped)
			return;
		if (createResult.Status() != winrt::Windows::Media::Audio::AudioGraphCreationStatus::Success)
		{
			OutputDebugStringW(L"VolumeBoost: AudioGraph creation failed.\n");
			Stop();
			return;
		}
		m_graph = createResult.Graph();

		winrt::hstring captureIdHstr(captureId.c_str());
		auto inputResult = co_await m_graph.CreateDeviceInputNodeAsync(captureIdHstr);
		if (m_stopped)
			return;
		if (inputResult.Status() != winrt::Windows::Media::Audio::AudioDeviceNodeCreationStatus::Success)
		{
			OutputDebugStringW(L"VolumeBoost: input node creation failed.\n");
			Stop();
			return;
		}
		m_inputNode = inputResult.DeviceInputNode();

		auto outputResult = co_await m_graph.CreateDeviceOutputNodeAsync();
		if (m_stopped)
			return;
		if (outputResult.Status() != winrt::Windows::Media::Audio::AudioDeviceNodeCreationStatus::Success)
		{
			OutputDebugStringW(L"VolumeBoost: output node creation failed.\n");
			Stop();
			return;
		}
		m_outputNode = outputResult.DeviceOutputNode();

		m_graph.Connect(m_inputNode, m_outputNode);
		m_outputNode.OutgoingGain(m_gain);
		m_graph.Start();

		m_active = true;
		m_starting = false;
		OutputDebugStringW(L"VolumeBoost: started.\n");
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		OutputDebugStringW(ex.message().c_str());
		Stop();
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		Stop();
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
				m_mutedSessions.push_back(std::move(control));
		}
	}
	CATCH_LOG();
	m_mutedSessions.clear();
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
