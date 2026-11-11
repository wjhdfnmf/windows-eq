// weq-setup: installs / removes the Windows EQ APO on playback devices and
// reports whether it is loaded and processing audio.

#include "Devices.h"
#include "Install.h"
#include "Util.h"
#include "weq/Ids.h"

#include <windows.h>
#include <objbase.h>

#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace weq;

namespace {

void usage()
{
    wprintf(
        L"weq-setup - Windows EQ installer and diagnostics\n"
        L"\n"
        L"  weq-setup list                          playback devices and their current effects\n"
        L"  weq-setup status                        install state + live APO activity\n"
        L"  weq-setup install   [--device D] [--slot S]   (admin)\n"
        L"  weq-setup uninstall [--device D]        restore one device's original effects (admin)\n"
        L"  weq-setup uninstall --all               remove Windows EQ completely (admin)\n"
        L"  weq-setup restart-audio                 restart the Windows Audio service (admin)\n"
        L"\n"
        L"  D = index from \"list\", device GUID, or part of the name. Default: the default device.\n"
        L"  Slots: mfx = after the per-mode mix (default, one instance for all apps),\n"
        L"         sfx = per application stream, efx = endpoint (after all mixing).\n"
        L"         Several at once: --slot sfx,mfx\n");
}

std::wstring clsidLabel(const std::wstring& clsid)
{
    if (clsid.empty())
        return L"-";
    if (iequals(clsid, kClsidApoString))
        return L"Windows EQ";
    return clsid;
}

int cmdList(const std::vector<RenderDevice>& devices)
{
    for (const RenderDevice& d : devices) {
        const DeviceInstallState s = deviceInstallState(d);
        wprintf(L"[%d]%s %s\n", d.index, d.isDefault ? L" (default)" : L"", d.displayName().c_str());
        wprintf(L"     id   %s\n", d.id.c_str());
        if (!d.hasFxKey)
            wprintf(L"     fx   none (no driver effects)\n");
        else
            wprintf(L"     fx   SFX %s | MFX %s | EFX %s%s\n", clsidLabel(d.sfx).c_str(), clsidLabel(d.mfx).c_str(),
                    clsidLabel(d.efx).c_str(),
                    (!d.lfx.empty() || !d.gfx.empty()) ? L" | legacy LFX/GFX present" : L"");
        if (d.sysFxDisabled)
            wprintf(L"     !    audio enhancements are disabled for this device\n");
        if (s.installed)
            wprintf(L"     eq   installed as %s%s\n", s.slot.c_str(), s.active ? L"" : L" (but overwritten)");
    }
    if (devices.empty())
        wprintf(L"no active playback devices\n");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    if (argc < 2) {
        usage();
        return 2;
    }

    const std::wstring cmd = toLower(argv[1]);
    std::wstring deviceSel, slotStr = L"mfx";
    bool all = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = toLower(argv[i]);
        if ((a == L"--device" || a == L"-d") && i + 1 < argc)
            deviceSel = argv[++i];
        else if (a == L"--slot" && i + 1 < argc)
            slotStr = argv[++i];
        else if (a == L"--all")
            all = true;
        else {
            fwprintf(stderr, L"unknown argument: %s\n\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (cmd == L"help" || cmd == L"--help" || cmd == L"-h" || cmd == L"/?") {
        usage();
        return 0;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        fwprintf(stderr, L"error: COM initialisation failed\n");
        return 1;
    }

    int rc = 0;
    std::wstring err;
    const std::vector<RenderDevice> devices = listRenderDevices(err);
    if (!err.empty())
        fwprintf(stderr, L"warning: %s\n", err.c_str());

    if (cmd == L"list") {
        rc = cmdList(devices);
    } else if (cmd == L"status") {
        rc = cmdStatus(devices);
    } else if (cmd == L"install") {
        std::vector<FxSlot> slots;
        if (!parseSlots(slotStr, slots)) {
            fwprintf(stderr, L"error: --slot must be mfx, sfx, efx or a comma list such as sfx,mfx\n");
            rc = 2;
        } else if (const RenderDevice* d = selectDevice(devices, deviceSel, err)) {
            rc = cmdInstall(*d, slots);
        } else {
            fwprintf(stderr, L"error: %s\n", err.c_str());
            rc = 1;
        }
    } else if (cmd == L"uninstall") {
        if (all) {
            rc = cmdUninstallAll();
        } else if (const RenderDevice* d = selectDevice(devices, deviceSel, err)) {
            rc = cmdUninstallDevice(*d);
        } else {
            fwprintf(stderr, L"error: %s\n", err.c_str());
            rc = 1;
        }
    } else if (cmd == L"restart-audio") {
        rc = cmdRestartAudio();
    } else {
        fwprintf(stderr, L"unknown command: %s\n\n", argv[1]);
        usage();
        rc = 2;
    }

    CoUninitialize();
    return rc;
}
