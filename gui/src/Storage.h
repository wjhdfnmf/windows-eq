#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace gui {

// %APPDATA%\WindowsEQ (created on demand) and the files in it.
std::wstring appDataDir();
std::wstring currentPresetPath(); // eq.txt: the live EQ, shared with weq-router
std::wstring presetsDir();        // user presets (*.txt)
std::wstring configPath();        // config.ini

bool readTextFile(const std::wstring& path, std::string& utf8);
bool writeTextFileAtomic(const std::wstring& path, const std::string& utf8);
unsigned long long fileStamp(const std::wstring& path); // last-write time, 0 if missing

std::vector<std::wstring> listUserPresets(); // names without ".txt", sorted
std::wstring userPresetPath(const std::wstring& name);
std::wstring sanitizeFileName(const std::wstring& name);

std::wstring configGet(const wchar_t* key, const std::wstring& def = L"");
void configSet(const wchar_t* key, const std::wstring& value);

// Common file dialogs (Equalizer APO / AutoEQ .txt). Empty on cancel.
std::wstring openPresetDialog(HWND owner);
std::wstring savePresetDialog(HWND owner, const std::wstring& suggestedName);

std::wstring fileStem(const std::wstring& path);

} // namespace gui
