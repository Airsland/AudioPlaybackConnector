#pragma once

#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <wil/win32_helpers.h>

#include <chrono>
#include <cmath>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

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

// The A2DP Sink capture endpoint is not returned by the audio endpoint
// enumeration APIs on Windows 11, but it is registered in the MMDevices
// registry. Look it up by its friendly name (read-only) and return its
// short-form device id that IMMDeviceEnumerator::GetDevice accepts.
static std::wstring FindA2dpEndpointShortId()
{
	try
	{
		HKEY hRoot = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture", 0, KEY_READ, &hRoot) != ERROR_SUCCESS)
			return {};
		wil::unique_hkey rootKey(hRoot);

		for (DWORD i = 0; ; ++i)
		{
			wchar_t subKeyName[256] = {};
			DWORD nameSize = 256;
			if (RegEnumKeyExW(rootKey.get(), i, subKeyName, &nameSize, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
				break;

			std::wstring propsPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
			propsPath += subKeyName;
			propsPath += L"\\Properties";

			HKEY hProps = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, propsPath.c_str(), 0, KEY_READ, &hProps) != ERROR_SUCCESS)
				continue;
			wil::unique_hkey propsKey(hProps);

			wchar_t name[1024] = {};
			DWORD dataSize = static_cast<DWORD>(sizeof(name));
			DWORD type = 0;
			if (RegQueryValueExW(propsKey.get(), L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", nullptr, &type, reinterpret_cast<BYTE*>(name), &dataSize) == ERROR_SUCCESS && type == REG_SZ)
			{
				if (ContainsIgnoreCase(name, L"A2DP"))
				{
					AppendVolumeBoostLog((L"VolumeBoost: found A2DP endpoint: " + std::wstring(name)).c_str());
					return L"{0.0.1.00000000}." + std::wstring(subKeyName);
				}
			}
		}
	}
	CATCH_LOG();

	return {};
}

static float GainToDb(float gain)
{
	return 20.0f * std::log10f(gain);
}

// Volume boost for the Bluetooth A2DP Sink stream.
//
// Windows renders the incoming A2DP Sink stream through the hidden
// "A2DP SNK" capture endpoint, whose input gain is exposed via
// IAudioEndpointVolume with a range up to +30 dB. Raising this level makes
// the phone stream louder independently of the system master volume and of
// every other app's volume. This only controls the existing audio endpoint
// volume (the same control the volume mixer exposes), reads the device id
// dynamically from the registry, and never modifies the registry or any
// other system setting.
class VolumeBoost
{
public:
	void Start(std::wstring_view)
	{
		if (m_active || m_starting)
			return;

		m_stopped = false;
		m_starting = true;
		StartAsync();
	}

	void SetGain(float gain)
	{
		m_gain = gain;
		if (m_active)
			ApplyGain();
	}

	void Stop()
	{
		m_stopped = true;
		m_active = false;
		m_starting = false;
		RestoreOriginalLevel();
	}

	bool IsActive() const
	{
		return m_active;
	}

private:
	winrt::fire_and_forget StartAsync();
	bool ApplyGain();
	void RestoreOriginalLevel();

	float m_gain = 1.0f;
	float m_originalLevel = 0.0f;
	bool m_haveOriginalLevel = false;
	bool m_active = false;
	bool m_starting = false;
	bool m_stopped = false;
};

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

		if (ApplyGain())
		{
			m_starting = false;
			AppendVolumeBoostLog(L"VolumeBoost: started");
			NotifyVolumeBoostStatus(L"Volume boost", L"Volume boost enabled");
			return;
		}
	}

	m_starting = false;
	AppendVolumeBoostLog(L"VolumeBoost: all attempts failed");
	NotifyVolumeBoostStatus(L"Volume boost", L"Volume boost failed, see AudioPlaybackConnector.log");
	Stop();
}

bool VolumeBoost::ApplyGain()
{
	try
	{
		std::wstring deviceId = FindA2dpEndpointShortId();
		if (deviceId.empty())
		{
			AppendVolumeBoostLog(L"VolumeBoost: A2DP endpoint not found in registry");
			return false;
		}

		winrt::com_ptr<IMMDeviceEnumerator> enumerator;
		winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())));

		winrt::com_ptr<IMMDevice> device;
		winrt::check_hresult(enumerator->GetDevice(deviceId.c_str(), device.put()));

		winrt::com_ptr<IAudioEndpointVolume> volume;
		winrt::check_hresult(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, volume.put_void()));

		if (!m_haveOriginalLevel)
		{
			volume->GetMasterVolumeLevel(&m_originalLevel);
			m_haveOriginalLevel = true;
			AppendVolumeBoostLog((L"VolumeBoost: original level " + std::to_wstring(static_cast<double>(m_originalLevel)) + L" dB").c_str());
		}

		float rangeMin = 0.0f;
		float rangeMax = 0.0f;
		float increment = 0.0f;
		volume->GetVolumeRange(&rangeMin, &rangeMax, &increment);

		float target = GainToDb(m_gain);
		if (target < rangeMin)
			target = rangeMin;
		if (target > rangeMax)
			target = rangeMax;

		winrt::check_hresult(volume->SetMasterVolumeLevel(target, nullptr));
		AppendVolumeBoostLog((L"VolumeBoost: set level to " + std::to_wstring(static_cast<double>(target)) +
			L" dB (range " + std::to_wstring(static_cast<double>(rangeMin)) +
			L".." + std::to_wstring(static_cast<double>(rangeMax)) + L" dB)").c_str());
		m_active = true;
		return true;
	}
	catch (winrt::hresult_error const& ex)
	{
		LOG_CAUGHT_EXCEPTION();
		AppendVolumeBoostLog((L"VolumeBoost: apply failed: " + std::wstring(ex.message())).c_str());
		return false;
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		AppendVolumeBoostLog(L"VolumeBoost: apply failed");
		return false;
	}
}

void VolumeBoost::RestoreOriginalLevel()
{
	if (!m_haveOriginalLevel)
		return;

	try
	{
		std::wstring deviceId = FindA2dpEndpointShortId();
		if (deviceId.empty())
			return;

		winrt::com_ptr<IMMDeviceEnumerator> enumerator;
		winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put())));

		winrt::com_ptr<IMMDevice> device;
		if (FAILED(enumerator->GetDevice(deviceId.c_str(), device.put())))
			return;

		winrt::com_ptr<IAudioEndpointVolume> volume;
		if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, volume.put_void())))
			return;

		volume->SetMasterVolumeLevel(m_originalLevel, nullptr);
		AppendVolumeBoostLog(L"VolumeBoost: restored original level");
		m_haveOriginalLevel = false;
	}
	CATCH_LOG();
}
