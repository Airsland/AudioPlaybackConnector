#pragma once

#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <cwchar>
#include <string>

// Volume control for the Bluetooth A2DP Sink stream.
//
// Windows exposes the incoming phone stream through a capture endpoint
// ("Microphone (xxx A2DP SNK)") whose level can be raised above the 0 dB
// the volume mixer allows. The endpoint is not returned by the audio
// endpoint enumeration APIs, so we look it up by its friendly name in the
// MMDevices registry (read-only, no hard-coded device id) and write the
// selected level directly through IAudioEndpointVolume.
//
// The written value is absolute: the level chosen in the UI is exactly what
// gets written, independent of the level the endpoint happened to have
// before. The neutral default is 0 dB, and Reset() always writes 0 dB so a
// previous session can never leave a non-zero level behind.
class VolumeBoost
{
public:
	// Write the given level (in dB) directly to the A2DP endpoint.
	static void SetLevel(float levelDb);

	// Reset the endpoint to 0 dB (neutral). Called on disconnect and exit.
	static void Reset();
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

static bool ApplyLevel(float levelDb)
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

	float rangeMin = 0.0f;
	float rangeMax = 0.0f;
	float increment = 0.0f;
	volume->GetVolumeRange(&rangeMin, &rangeMax, &increment);

	float target = levelDb;
	if (target < rangeMin)
		target = rangeMin;
	if (target > rangeMax)
		target = rangeMax;

	winrt::check_hresult(volume->SetMasterVolumeLevel(target, nullptr));
	return true;
}

void VolumeBoost::SetLevel(float levelDb)
{
	ApplyLevel(levelDb);
}

void VolumeBoost::Reset()
{
	ApplyLevel(0.0f);
}
