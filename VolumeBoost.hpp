#pragma once

#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cwchar>
#include <string>

// Volume gain for the Bluetooth A2DP Sink stream.
//
// Windows exposes the incoming phone stream through a capture endpoint
// ("Microphone (xxx A2DP SNK)") whose level can be raised above the 0 dB
// the volume mixer allows. The endpoint is not returned by the audio
// endpoint enumeration APIs, so we look it up by its friendly name in the
// MMDevices registry (read-only, no hard-coded device id) and adjust its
// IAudioEndpointVolume level by the selected amount in dB, relative to the
// level it had when the gain was first enabled.
class VolumeBoost
{
public:
	void Start(float gainDb);
	void SetGain(float gainDb);
	void Stop();
	bool IsActive() const;

private:
	bool ApplyGain();
	void RestoreOriginalLevel();

	float m_gainDb = 0.0f;
	float m_originalLevel = 0.0f;
	bool m_haveOriginalLevel = false;
	bool m_active = false;
};

static bool ContainsIgnoreCase(const wchar_t* text, const wchar_t* needle)
{
	if (!text || !needle)
		return false;

	const size_t textLength = wcslen(text);
	const size_t needleLength = wcslen(needle);
	if (needleLength == 0 || needleLength > textLength)
		return false;

	for (size_t i = 0; i + needleLength <= textLength; ++i)
	{
		if (_wcsnicmp(text + i, needle, needleLength) == 0)
			return true;
	}
	return false;
}

// Find the A2DP Sink capture endpoint id. The endpoint is hidden from the
// audio endpoint enumeration APIs on Windows 11, but it is registered in the
// MMDevices registry under a sub-key named after the endpoint GUID, which is
// exactly what IMMDeviceEnumerator::GetDevice expects in short form.
static std::wstring FindA2dpEndpointShortId()
{
	HKEY hRoot = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture", 0, KEY_READ, &hRoot) != ERROR_SUCCESS)
		return {};

	for (DWORD i = 0;; ++i)
	{
		wchar_t subKeyName[256] = {};
		DWORD nameSize = 256;
		if (RegEnumKeyExW(hRoot, i, subKeyName, &nameSize, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
			break;

		std::wstring propsPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
		propsPath += subKeyName;
		propsPath += L"\\Properties";

		HKEY hProps = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, propsPath.c_str(), 0, KEY_READ, &hProps) != ERROR_SUCCESS)
			continue;

		wchar_t name[1024] = {};
		DWORD dataSize = static_cast<DWORD>(sizeof(name));
		DWORD type = 0;
		const LONG result = RegQueryValueExW(hProps, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", nullptr, &type, reinterpret_cast<BYTE*>(name), &dataSize);
		RegCloseKey(hProps);

		if (result == ERROR_SUCCESS && type == REG_SZ && ContainsIgnoreCase(name, L"A2DP"))
			return L"{0.0.1.00000000}." + std::wstring(subKeyName);
	}

	RegCloseKey(hRoot);
	return {};
}

void VolumeBoost::Start(float gainDb)
{
	m_gainDb = gainDb;
	ApplyGain();
}

void VolumeBoost::SetGain(float gainDb)
{
	m_gainDb = gainDb;
	if (m_active)
		ApplyGain();
}

void VolumeBoost::Stop()
{
	m_active = false;
	RestoreOriginalLevel();
}

bool VolumeBoost::IsActive() const
{
	return m_active;
}

bool VolumeBoost::ApplyGain()
{
	std::wstring deviceId = FindA2dpEndpointShortId();
	if (deviceId.empty())
		return false;

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
	}

	float rangeMin = 0.0f;
	float rangeMax = 0.0f;
	float increment = 0.0f;
	volume->GetVolumeRange(&rangeMin, &rangeMax, &increment);

	float target = m_originalLevel + m_gainDb;
	if (target < rangeMin)
		target = rangeMin;
	if (target > rangeMax)
		target = rangeMax;

	winrt::check_hresult(volume->SetMasterVolumeLevel(target, nullptr));
	m_active = true;
	return true;
}

void VolumeBoost::RestoreOriginalLevel()
{
	if (!m_haveOriginalLevel)
		return;

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
	m_haveOriginalLevel = false;
}
