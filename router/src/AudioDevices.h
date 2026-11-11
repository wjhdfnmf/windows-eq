#pragma once

#include <windows.h>
#include <mmdeviceapi.h>

#include <string>
#include <vector>

namespace weq {

struct AudioDevice {
    int index = 0;
    std::wstring id;
    std::wstring name;        // "Speakers (Focusrite USB Audio)"
    bool isRender = true;
    bool isDefault = false;
};

// Active endpoints of one direction. COM must be initialised.
std::vector<AudioDevice> listDevices(bool render, std::wstring& error);

// Index, endpoint id, or unique case-insensitive name substring.
const AudioDevice* findDevice(const std::vector<AudioDevice>& devices, const std::wstring& selector, std::wstring& error);

// Activates the endpoint by id. Returns nullptr on failure (error filled).
IMMDevice* openDevice(const std::wstring& id, std::wstring& error);

std::wstring hrString(HRESULT hr);

} // namespace weq
