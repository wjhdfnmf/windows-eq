#include "Install.h"

#include "Util.h"
#include "weq/Ids.h"
#include "weq/Status.h"

#include <windows.h>
#include <combaseapi.h>
#include <sddl.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace weq {

namespace {

// ------------------------------------------------------------------ registry helpers

constexpr wchar_t kAudioPolicyKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio";
constexpr wchar_t kDisableProtectedValue[] = L"DisableProtectedAudioDG";

std::wstring productDevicesKey() { return std::wstring(kProductRegKey) + L"\\Devices"; }
std::wstring backupKeyPath(const std::wstring& guid) { return productDevicesKey() + L"\\" + guid; }
std::wstring apoRegKeyPath() { return std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + kClsidApoString; }
std::wstring comRegKeyPath() { return std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidApoString; }

struct Key {
    HKEY h = nullptr;
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    ~Key() { close(); }
    void close()
    {
        if (h)
            RegCloseKey(h);
        h = nullptr;
    }
};

struct RegValue {
    DWORD type = REG_NONE;
    std::vector<BYTE> data;
};

bool keyExists(const std::wstring& path)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
        return false;
    RegCloseKey(h);
    return true;
}

// Opens/creates an HKLM key using SeRestorePrivilege so ACLs that only grant
// the Audiosrv service write access (MMDevices) don't get in the way. The key
// permissions themselves are left untouched.
LSTATUS openPrivileged(const std::wstring& path, Key& key, bool* created = nullptr)
{
    DWORD disp = 0;
    const LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, REG_OPTION_BACKUP_RESTORE,
                                       KEY_ALL_ACCESS, nullptr, &key.h, &disp);
    if (created)
        *created = st == ERROR_SUCCESS && disp == REG_CREATED_NEW_KEY;
    return st;
}

LSTATUS openNormal(const std::wstring& path, Key& key, bool create)
{
    if (create)
        return RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key.h, nullptr);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_ALL_ACCESS, &key.h);
}

bool readValue(HKEY key, const std::wstring& name, RegValue& out)
{
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    out.type = type;
    out.data.resize(size);
    return RegQueryValueExW(key, name.c_str(), nullptr, &type, out.data.data(), &size) == ERROR_SUCCESS;
}

LSTATUS writeValue(HKEY key, const std::wstring& name, const RegValue& v)
{
    return RegSetValueExW(key, name.c_str(), 0, v.type, v.data.data(), static_cast<DWORD>(v.data.size()));
}

LSTATUS writeString(HKEY key, const std::wstring& name, const std::wstring& value)
{
    return RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

LSTATUS writeMultiString(HKEY key, const std::wstring& name, const std::vector<std::wstring>& values)
{
    std::wstring blob;
    for (const std::wstring& v : values) {
        blob += v;
        blob.push_back(L'\0');
    }
    blob.push_back(L'\0');
    return RegSetValueExW(key, name.c_str(), 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(blob.data()),
                          static_cast<DWORD>(blob.size() * sizeof(wchar_t)));
}

LSTATUS writeDword(HKEY key, const std::wstring& name, DWORD value)
{
    return RegSetValueExW(key, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

std::wstring readString(HKEY key, const std::wstring& name)
{
    RegValue v;
    if (!readValue(key, name, v) || (v.type != REG_SZ && v.type != REG_EXPAND_SZ) || v.data.size() < sizeof(wchar_t))
        return {};
    const auto* p = reinterpret_cast<const wchar_t*>(v.data.data());
    return std::wstring(p, wcsnlen(p, v.data.size() / sizeof(wchar_t)));
}

std::vector<std::wstring> readMultiString(HKEY key, const std::wstring& name)
{
    std::vector<std::wstring> out;
    RegValue v;
    if (!readValue(key, name, v) || v.type != REG_MULTI_SZ)
        return out;
    const auto* p = reinterpret_cast<const wchar_t*>(v.data.data());
    const size_t n = v.data.size() / sizeof(wchar_t);
    size_t i = 0;
    while (i < n && p[i]) {
        const size_t len = wcsnlen(p + i, n - i);
        out.emplace_back(p + i, len);
        i += len + 1;
    }
    return out;
}

bool readDword(HKEY key, const std::wstring& name, DWORD& out)
{
    RegValue v;
    if (!readValue(key, name, v) || v.type != REG_DWORD || v.data.size() != sizeof(DWORD))
        return false;
    out = *reinterpret_cast<const DWORD*>(v.data.data());
    return true;
}

// Deletes the key behind an open handle (needs DELETE access, which the
// backup/restore open grants). Used for FxProperties keys we created.
bool deleteOpenKey(HKEY key)
{
    using NtDeleteKeyFn = LONG(NTAPI*)(HANDLE);
    static const auto fn = reinterpret_cast<NtDeleteKeyFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeleteKey"));
    return fn && fn(key) >= 0;
}

bool keyIsEmpty(HKEY key)
{
    DWORD subkeys = 0, values = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr,
                         nullptr, nullptr) != ERROR_SUCCESS)
        return false;
    return subkeys == 0 && values == 0;
}

bool fail(const std::wstring& what, LSTATUS st)
{
    fwprintf(stderr, L"error: %s: %s\n", what.c_str(), errorText(static_cast<DWORD>(st)).c_str());
    return false;
}

// ------------------------------------------------------------------ preconditions

bool requireAdmin()
{
    if (!isElevated()) {
        fwprintf(stderr, L"error: this command changes system audio settings and must be run from an\n"
                         L"       Administrator terminal (right-click Terminal > Run as administrator).\n");
        return false;
    }
    if (!enablePrivilege(SE_BACKUP_NAME) || !enablePrivilege(SE_RESTORE_NAME)) {
        fwprintf(stderr, L"error: could not enable backup/restore privileges\n");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ core files / registration

bool callDllExport(const std::wstring& dllPath, const char* exportName)
{
    HMODULE mod = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        fail(L"load " + dllPath, static_cast<LSTATUS>(GetLastError()));
        return false;
    }
    using Fn = HRESULT(STDAPICALLTYPE*)();
    const auto fn = reinterpret_cast<Fn>(GetProcAddress(mod, exportName));
    HRESULT hr = E_NOTIMPL;
    if (fn)
        hr = fn();
    FreeLibrary(mod);
    if (FAILED(hr)) {
        fwprintf(stderr, L"error: %hs failed: %s\n", exportName, hrText(hr).c_str());
        return false;
    }
    return true;
}

bool copyDll()
{
    const std::wstring src = exeDirectory() + L"\\" + kApoDllName;
    const std::wstring dir = installDir();
    const std::wstring dst = installedDllPath();

    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"error: %s not found next to weq-setup.exe\n", src.c_str());
        return false;
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return fail(L"create " + dir, static_cast<LSTATUS>(GetLastError()));

    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        // Still loaded somewhere: move the old file aside and delete it at reboot.
        const std::wstring aside = dst + L"." + std::to_wstring(GetTickCount64()) + L".old";
        if (!MoveFileExW(dst.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING) ||
            !CopyFileW(src.c_str(), dst.c_str(), FALSE))
            return fail(L"copy APO to " + dst, static_cast<LSTATUS>(GetLastError()));
        MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    wprintf(L"  copied     %s\n", dst.c_str());
    return true;
}

bool prepareDataDir()
{
    // SYSTEM/Administrators full, LOCAL SERVICE (audiodg) modify, Users read.
    const std::wstring dir = dataDir();
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;LS)(A;OICI;0x1200a9;;;BU)",
            SDDL_REVISION_1, &sd, nullptr))
        return fail(L"build data directory ACL", static_cast<LSTATUS>(GetLastError()));

    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    bool ok = true;
    if (!CreateDirectoryW(dir.c_str(), &sa)) {
        if (GetLastError() == ERROR_ALREADY_EXISTS)
            ok = SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION, sd) != FALSE;
        else
            ok = false;
    }
    const DWORD err = GetLastError();
    LocalFree(sd);
    if (!ok)
        return fail(L"prepare " + dir, static_cast<LSTATUS>(err));
    wprintf(L"  log dir    %s\n", dir.c_str());
    return true;
}

bool setDisableProtectedAudioDG()
{
    Key product;
    LSTATUS st = openNormal(kProductRegKey, product, true);
    if (st != ERROR_SUCCESS)
        return fail(L"create HKLM\\" + std::wstring(kProductRegKey), st);

    Key audio;
    st = openPrivileged(kAudioPolicyKey, audio);
    if (st != ERROR_SUCCESS)
        return fail(L"open HKLM\\" + std::wstring(kAudioPolicyKey), st);

    DWORD dummy = 0;
    if (!readDword(product.h, L"OrigDisableProtectedAudioDGSaved", dummy)) {
        RegValue orig;
        if (readValue(audio.h, kDisableProtectedValue, orig))
            writeValue(product.h, L"OrigDisableProtectedAudioDG", orig);
        writeDword(product.h, L"OrigDisableProtectedAudioDGSaved", 1);
    }

    st = writeDword(audio.h, kDisableProtectedValue, 1);
    if (st != ERROR_SUCCESS)
        return fail(L"set DisableProtectedAudioDG", st);
    wprintf(L"  policy     DisableProtectedAudioDG = 1 (allows the unsigned APO to load)\n");
    return true;
}

void restoreDisableProtectedAudioDG()
{
    Key product;
    if (openNormal(kProductRegKey, product, false) != ERROR_SUCCESS)
        return;
    DWORD saved = 0;
    if (!readDword(product.h, L"OrigDisableProtectedAudioDGSaved", saved))
        return;
    Key audio;
    if (openPrivileged(kAudioPolicyKey, audio) != ERROR_SUCCESS)
        return;
    RegValue orig;
    if (readValue(product.h, L"OrigDisableProtectedAudioDG", orig))
        writeValue(audio.h, kDisableProtectedValue, orig);
    else
        RegDeleteValueW(audio.h, kDisableProtectedValue);
    wprintf(L"  policy     DisableProtectedAudioDG restored\n");
}

bool installCore()
{
    if (!copyDll() || !prepareDataDir())
        return false;
    if (!callDllExport(installedDllPath(), "DllRegisterServer"))
        return false;
    wprintf(L"  registered COM class + APO %s\n", kClsidApoString);
    if (!setDisableProtectedAudioDG())
        return false;

    Key product;
    if (openNormal(kProductRegKey, product, true) == ERROR_SUCCESS) {
        writeString(product.h, L"InstallDir", installDir());
        writeString(product.h, L"Version", std::to_wstring(kApoVersionMajor) + L"." + std::to_wstring(kApoVersionMinor));
    }
    return true;
}

// ------------------------------------------------------------------ per-device FX values

bool restoreDevice(const std::wstring& guid, bool verbose)
{
    Key backup;
    if (openNormal(backupKeyPath(guid), backup, false) != ERROR_SUCCESS)
        return true; // nothing to restore

    const std::vector<std::wstring> touched = readMultiString(backup.h, L"Touched");
    DWORD fxCreated = 0;
    readDword(backup.h, L"FxKeyCreated", fxCreated);

    const std::wstring fxPath = renderKeyPath(guid) + L"\\FxProperties";
    if (keyExists(fxPath)) {
        Key fx;
        LSTATUS st = openPrivileged(fxPath, fx);
        if (st != ERROR_SUCCESS)
            return fail(L"open " + fxPath, st);

        Key original;
        const bool haveOriginal = RegOpenKeyExW(backup.h, L"Original", 0, KEY_READ, &original.h) == ERROR_SUCCESS;
        for (const std::wstring& name : touched) {
            RegValue v;
            if (haveOriginal && readValue(original.h, name, v)) {
                st = writeValue(fx.h, name, v);
                if (st != ERROR_SUCCESS)
                    return fail(L"restore " + name, st);
            } else {
                RegDeleteValueW(fx.h, name.c_str());
            }
        }
        if (fxCreated && keyIsEmpty(fx.h))
            deleteOpenKey(fx.h);
    }

    backup.close();
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, backupKeyPath(guid).c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, backupKeyPath(guid).c_str());
    if (verbose)
        wprintf(L"  restored   original effects on %s\n", guid.c_str());
    return true;
}

const std::wstring& currentClsid(const RenderDevice& dev, FxSlot slot)
{
    return slot == FxSlot::SFX ? dev.sfx : slot == FxSlot::MFX ? dev.mfx : dev.efx;
}

bool installDevice(const RenderDevice& dev, const std::vector<FxSlot>& slots)
{
    // Re-installing: always put the originals back first, so the backup is
    // taken fresh and covers exactly the values this version writes.
    const DeviceInstallState state = deviceInstallState(dev);
    if (state.installed && !restoreDevice(dev.guid, true))
        return false;

    const std::wstring fxPath = renderKeyPath(dev.guid) + L"\\FxProperties";
    std::vector<std::wstring> touched = {fxAssociationValueName()};
    for (FxSlot slot : slots) {
        touched.push_back(fxClsidValueName(slot));
        touched.push_back(fxModesValueName(slot));
    }

    Key fx;
    bool created = false;
    LSTATUS st = openPrivileged(fxPath, fx, &created);
    if (st != ERROR_SUCCESS)
        return fail(L"open/create " + fxPath, st);

    // Back up the original values once (never overwrite an existing backup,
    // which would capture our own values instead of the driver's).
    Key backup;
    const bool haveBackup = openNormal(backupKeyPath(dev.guid), backup, false) == ERROR_SUCCESS;
    if (!haveBackup) {
        st = openNormal(backupKeyPath(dev.guid), backup, true);
        if (st != ERROR_SUCCESS)
            return fail(L"create backup key", st);
        Key original;
        st = RegCreateKeyExW(backup.h, L"Original", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &original.h, nullptr);
        if (st != ERROR_SUCCESS)
            return fail(L"create backup key", st);
        for (const std::wstring& name : touched) {
            RegValue v;
            if (readValue(fx.h, name, v))
                writeValue(original.h, name, v);
        }
        writeMultiString(backup.h, L"Touched", touched);
        writeDword(backup.h, L"FxKeyCreated", created ? 1 : 0);
        writeString(backup.h, L"Name", dev.displayName());
    }
    writeString(backup.h, L"Slot", slotsName(slots));

    // PKEY_FX_Association binds the effects to the endpoint's KS topology.
    // Driver INFs always set it to KSNODETYPE_ANY; without it Windows ignores
    // the effect CLSIDs on devices that shipped with no effects of their own.
    {
        RegValue existing;
        if (!readValue(fx.h, fxAssociationValueName(), existing)) {
            st = writeString(fx.h, fxAssociationValueName(), kKsNodeTypeAny);
            if (st != ERROR_SUCCESS)
                return fail(L"write PKEY_FX_Association", st);
            wprintf(L"  device     %s: PKEY_FX_Association = KSNODETYPE_ANY\n", dev.displayName().c_str());
        }
    }

    for (FxSlot slot : slots) {
        // EFX runs after all mixing and also sees RAW-mode streams (pro-audio
        // drivers often expose only RAW), so advertise both modes there.
        st = writeString(fx.h, fxClsidValueName(slot), kClsidApoString);
        if (st == ERROR_SUCCESS)
            st = slot == FxSlot::EFX ? writeMultiString(fx.h, fxModesValueName(slot), {kModeDefault, kModeRaw})
                                     : writeMultiString(fx.h, fxModesValueName(slot), {kModeDefault});
        if (st != ERROR_SUCCESS)
            return fail(L"write effect values", st);

        const std::wstring& previous = currentClsid(dev, slot);
        const bool replaced = !previous.empty() && !iequals(previous, kClsidApoString);
        wprintf(L"  device     %s: %s = Windows EQ%s\n", dev.displayName().c_str(), slotName(slot),
                replaced ? (L" (replaces " + previous + L", backed up)").c_str() : L"");
    }
    return true;
}

std::vector<std::wstring> installedDeviceGuids()
{
    std::vector<std::wstring> out;
    Key devs;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, productDevicesKey().c_str(), 0, KEY_READ, &devs.h) != ERROR_SUCCESS)
        return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(devs.h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        out.emplace_back(name, len);
    }
    return out;
}

bool stopAudio(AudioServiceStopper& audio)
{
    wprintf(L"  stopping   audio services (AudioEndpointBuilder + Windows Audio)...\n");
    std::wstring err;
    if (!audio.stop(err)) {
        fwprintf(stderr, L"error: %s\n", err.c_str());
        return false;
    }
    return true;
}

bool startAudio(AudioServiceStopper& audio)
{
    wprintf(L"  starting   audio services...\n");
    std::wstring err;
    if (!audio.restart(err)) {
        fwprintf(stderr, L"error: %s\n", err.c_str());
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ status helpers

std::wstring modeName(const GUID& g)
{
    wchar_t s[40];
    if (StringFromGUID2(g, s, 40) == 0)
        return L"?";
    if (g == GUID{})
        return L"(none)";
    if (iequals(s, kModeDefault))
        return L"Default";
    if (iequals(s, kModeRaw))
        return L"Raw";
    return s;
}

void printLogTail(int lines)
{
    const std::wstring path = dataDir() + L"\\" + kLogFileName;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        wprintf(L"\nAPO log: %s (not created yet: the APO has not been loaded)\n", path.c_str());
        return;
    }
    std::vector<std::string> all;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        all.push_back(line);
    }
    wprintf(L"\nAPO log (last %d lines of %s):\n", lines, path.c_str());
    const size_t start = all.size() > static_cast<size_t>(lines) ? all.size() - lines : 0;
    for (size_t i = start; i < all.size(); ++i) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, all[i].c_str(), static_cast<int>(all[i].size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, all[i].c_str(), static_cast<int>(all[i].size()), w.data(), n);
        wprintf(L"  %s\n", w.c_str());
    }
}

} // namespace

// ------------------------------------------------------------------ public

DeviceInstallState deviceInstallState(const RenderDevice& dev)
{
    DeviceInstallState s;
    Key backup;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupKeyPath(dev.guid).c_str(), 0, KEY_READ, &backup.h) != ERROR_SUCCESS)
        return s;
    s.installed = true;
    s.slot = readString(backup.h, L"Slot");
    std::vector<FxSlot> slots;
    if (parseSlots(s.slot, slots)) {
        s.active = true;
        for (FxSlot slot : slots)
            s.active = s.active && iequals(currentClsid(dev, slot), kClsidApoString);
    }
    return s;
}

int cmdInstall(const RenderDevice& dev, const std::vector<FxSlot>& slots)
{
    if (!requireAdmin())
        return 1;

    wprintf(L"Installing Windows EQ on %s as %s\n", dev.displayName().c_str(), slotsName(slots).c_str());
    if (dev.sysFxDisabled)
        wprintf(L"  warning    \"Enable audio enhancements\" is OFF for this device; Windows will not load\n"
                L"             any APO until it is turned on (Sound settings > device > Advanced).\n");

    AudioServiceStopper audio;
    if (!stopAudio(audio))
        return 1;

    const bool ok = installCore() && installDevice(dev, slots);
    const bool started = startAudio(audio);
    if (!ok) {
        fwprintf(stderr, L"\nInstall failed. Run \"weq-setup uninstall --all\" to roll back.\n");
        return 1;
    }
    if (!started)
        return 1;

    wprintf(L"\nDone. Play some audio on this device, then run:  weq-setup status\n");
    return 0;
}

int cmdUninstallDevice(const RenderDevice& dev)
{
    if (!requireAdmin())
        return 1;
    if (!deviceInstallState(dev).installed) {
        wprintf(L"Windows EQ is not installed on %s\n", dev.displayName().c_str());
        return 0;
    }
    wprintf(L"Removing Windows EQ from %s\n", dev.displayName().c_str());
    AudioServiceStopper audio;
    if (!stopAudio(audio))
        return 1;
    const bool ok = restoreDevice(dev.guid, true);
    const bool started = startAudio(audio);
    if (ok && installedDeviceGuids().empty())
        wprintf(L"\nNo devices use Windows EQ any more. \"weq-setup uninstall --all\" removes it completely.\n");
    return ok && started ? 0 : 1;
}

int cmdUninstallAll()
{
    if (!requireAdmin())
        return 1;
    wprintf(L"Removing Windows EQ completely\n");
    AudioServiceStopper audio;
    if (!stopAudio(audio))
        return 1;

    bool ok = true;
    for (const std::wstring& guid : installedDeviceGuids())
        ok = restoreDevice(guid, true) && ok;

    const std::wstring dll = installedDllPath();
    if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES)
        callDllExport(dll, "DllUnregisterServer");
    // Belt and braces in case the DLL was missing or failed.
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, apoRegKeyPath().c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, apoRegKeyPath().c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, comRegKeyPath().c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, comRegKeyPath().c_str());
    wprintf(L"  unregistered APO\n");

    restoreDisableProtectedAudioDG();

    if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(dll.c_str()))
        MoveFileExW(dll.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    RemoveDirectoryW(installDir().c_str());
    wprintf(L"  removed    %s\n", installDir().c_str());

    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kProductRegKey);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kProductRegKey);

    const bool started = startAudio(audio);
    wprintf(L"\nLogs were kept in %s\n", dataDir().c_str());
    return ok && started ? 0 : 1;
}

int cmdRestartAudio()
{
    if (!requireAdmin())
        return 1;
    AudioServiceStopper audio;
    if (!stopAudio(audio))
        return 1;
    return startAudio(audio) ? 0 : 1;
}

int cmdStatus(const std::vector<RenderDevice>& devices)
{
    // Install state
    wprintf(L"Install\n");
    const bool dllPresent = GetFileAttributesW(installedDllPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    wprintf(L"  APO DLL        %s%s\n", installedDllPath().c_str(), dllPresent ? L"" : L"  (missing)");
    wprintf(L"  APO registered %s\n", keyExists(apoRegKeyPath()) && keyExists(comRegKeyPath()) ? L"yes" : L"no");
    {
        Key audio;
        DWORD v = 0;
        const bool have = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kAudioPolicyKey, 0, KEY_READ, &audio.h) == ERROR_SUCCESS &&
                          readDword(audio.h, kDisableProtectedValue, v);
        wprintf(L"  DisableProtectedAudioDG %s\n", have ? (v ? L"1" : L"0") : L"(not set)");
    }

    wprintf(L"\nDevices\n");
    for (const RenderDevice& d : devices) {
        const DeviceInstallState s = deviceInstallState(d);
        if (!s.installed) {
            wprintf(L"  [%d] %-45s not installed\n", d.index, d.displayName().c_str());
            continue;
        }
        wprintf(L"  [%d] %-45s installed as %s%s%s\n", d.index, d.displayName().c_str(), s.slot.c_str(),
                s.active ? L"" : L"  (WARNING: slot no longer points at Windows EQ - driver update? reinstall)",
                d.sysFxDisabled ? L"  (WARNING: audio enhancements disabled)" : L"");
    }

    // Live instances
    wprintf(L"\nLive APO instances (sampled over 1 s)\n");
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kStatusMappingName);
    if (!mapping) {
        wprintf(L"  none: the APO is not loaded in audiodg.exe (%s)\n", errorText(GetLastError()).c_str());
        printLogTail(10);
        return 0;
    }
    const auto* block = static_cast<const StatusBlock*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(StatusBlock)));
    if (!block || block->magic != kStatusMagic) {
        wprintf(L"  status block unavailable\n");
        if (block)
            UnmapViewOfFile(block);
        CloseHandle(mapping);
        return 0;
    }

    uint64_t before[kMaxStatusInstances];
    for (int i = 0; i < kMaxStatusInstances; ++i)
        before[i] = block->instances[i].framesProcessed;
    Sleep(1000);

    std::map<std::wstring, std::wstring> names;
    for (const RenderDevice& d : devices)
        names[toLower(d.id)] = d.displayName();

    int shown = 0;
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < kMaxStatusInstances; ++i) {
        const InstanceStatus& s = block->instances[i];
        if (s.state <= 0)
            continue;
        ++shown;
        const auto it = names.find(toLower(s.endpointId));
        const std::wstring dev = it != names.end() ? it->second : std::wstring(s.endpointId);
        const uint64_t perSec = s.framesProcessed - before[i];
        wprintf(L"  %s\n", dev.c_str());
        wprintf(L"      mode %s, %s", modeName(s.processingMode).c_str(),
                s.state == kInstanceLocked ? L"streaming" : (s.discoveryOnly ? L"discovery only" : L"idle"));
        if (s.state == kInstanceLocked)
            wprintf(L", %u ch @ %u Hz, %llu frames/s", s.channels, s.sampleRate,
                    static_cast<unsigned long long>(perSec));
        if (s.lastProcessTick)
            wprintf(L", last audio %.1f s ago", (now - s.lastProcessTick) / 1000.0);
        wprintf(L"  (audiodg pid %u)\n", s.processId);
    }
    if (!shown)
        wprintf(L"  none\n");

    UnmapViewOfFile(block);
    CloseHandle(mapping);
    printLogTail(10);
    return 0;
}

} // namespace weq
