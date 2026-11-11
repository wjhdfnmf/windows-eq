// weq-router: system-wide EQ via a virtual audio cable.
//
// Apps play into the virtual cable (set as the Windows default output); this
// program captures the cable, applies the EQ and plays the result on the real
// output device. Works with any output, including ones whose driver refuses
// system-effect APOs.

#include "AudioDevices.h"
#include "Router.h"

#include "eqdsp/BiquadDesign.h"
#include "eqdsp/PresetParser.h"

#include <windows.h>
#include <objbase.h>

#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace weq;

namespace {

std::atomic<bool> g_quit{false};

BOOL WINAPI onCtrl(DWORD)
{
    g_quit.store(true);
    return TRUE;
}

void usage()
{
    wprintf(
        L"weq-router - system-wide EQ through a virtual audio cable\n"
        L"\n"
        L"  weq-router list\n"
        L"  weq-router run --to D [--from D | --from loopback:D] [--preset FILE] [--margin-ms N] [--seconds N]\n"
        L"\n"
        L"  --to       real output device (index from \"list\", or part of its name)\n"
        L"  --from     recording device to read; default: the VB-Audio \"CABLE Output\"\n"
        L"             loopback:D reads what is playing on output D instead (testing)\n"
        L"  --preset   Equalizer APO / AutoEQ text file; saving it applies changes live\n"
        L"             default: %%APPDATA%%\\WindowsEQ\\eq.txt (created if missing)\n"
        L"  --seconds  stop after N seconds (default: run until Ctrl+C)\n");
}

std::wstring lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(towlower(c));
    return s;
}

void printDevices()
{
    std::wstring err;
    for (int pass = 0; pass < 2; ++pass) {
        const bool render = pass == 0;
        wprintf(render ? L"Output devices (--to):\n" : L"\nRecording devices (--from):\n");
        for (const AudioDevice& d : listDevices(render, err))
            wprintf(L"  [%d]%s %s\n", d.index, d.isDefault ? L" (default)" : L"", d.name.c_str());
    }
    if (!err.empty())
        fwprintf(stderr, L"warning: %s\n", err.c_str());
}

// ------------------------------------------------------------------ preset file

std::wstring defaultPresetPath()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
    dir += L"\\WindowsEQ";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\eq.txt";
}

void ensurePresetExists(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    std::ofstream f(path, std::ios::binary);
    f << "# Windows EQ preset - Equalizer APO / AutoEQ \"ParametricEQ.txt\" format.\n"
         "# Save this file and the change is applied immediately (smoothly, no clicks).\n"
         "#\n"
         "# Examples:\n"
         "#   Preamp: -6 dB\n"
         "#   Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71\n"
         "#   Filter 2: ON PK Fc 3000 Hz Gain -2.5 dB Q 1.41\n"
         "#   Channel: L          (following filters only on the left channel)\n"
         "\n"
         "Preamp: 0 dB\n";
}

bool readFileUtf8(const std::wstring& path, std::string& out)
{
    // Share with editors that keep the file open.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 1 << 20)));
    DWORD read = 0;
    const BOOL ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    out.resize(read);
    return ok != FALSE;
}

ULONGLONG lastWrite(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (static_cast<ULONGLONG>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
}

bool loadPreset(const std::wstring& path, eqdsp::EqSettings& s, bool verbose)
{
    std::string text;
    if (!readFileUtf8(path, text)) {
        fwprintf(stderr, L"\nwarning: cannot read preset %s\n", path.c_str());
        return false;
    }
    std::vector<std::string> warnings;
    if (!eqdsp::parseEqualizerApoConfig(text, s, warnings)) {
        fwprintf(stderr, L"\nwarning: preset has no usable lines; keeping the previous EQ\n");
        return false;
    }
    for (const std::string& w : warnings)
        fwprintf(stderr, L"\n  preset %hs", w.c_str());
    if (!warnings.empty())
        fwprintf(stderr, L"\n");
    if (verbose) {
        const double boost = eqdsp::maxBoostDb(s, 2, 48000);
        wprintf(L"\nPreset: %u filter(s), preamp %+.1f dB, max boost %+.1f dB%s\n", s.numBands, s.preampDb, boost,
                boost + s.preampDb > 0.05 ? L"  (limiter will catch any clipping; lower the preamp to avoid it)" : L"");
    }
    return true;
}

// ------------------------------------------------------------------ status

double toDb(float v) { return v > 1e-9f ? 20.0 * std::log10(v) : -180.0; }

void printStatus(RouterStats& st)
{
    const float peak = st.peak.exchange(0.0f);
    if (!st.running.load()) {
        wprintf(L"\r  buffering...                                                                       ");
        return;
    }
    wprintf(L"\r  latency ~%4.1f ms (buffer %4.1f/%4.1f) | drift %+7.1f ppm | cpu %4.2f%% | peak %6.1f dBFS%s | underruns %llu   ",
            st.latencyMs.load(), st.fillMs.load(), st.targetMs.load(), st.ppm.load(), st.cpuPercent.load(), toDb(peak),
            st.limiterDb.load() < -0.05 ? L" LIMITING" : L"", st.underruns.load());
    fflush(stdout);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        fwprintf(stderr, L"error: COM initialisation failed\n");
        return 1;
    }

    if (argc < 2) {
        usage();
        return 2;
    }
    const std::wstring cmd = lower(argv[1]);
    if (cmd == L"list") {
        printDevices();
        return 0;
    }
    if (cmd != L"run") {
        usage();
        return cmd == L"help" || cmd == L"--help" ? 0 : 2;
    }

    std::wstring toSel, fromSel, presetPath = defaultPresetPath();
    double marginMs = weq::RouterConfig{}.marginMs, seconds = 0.0;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = lower(argv[i]);
        const bool hasValue = i + 1 < argc;
        if (a == L"--to" && hasValue) toSel = argv[++i];
        else if (a == L"--from" && hasValue) fromSel = argv[++i];
        else if (a == L"--preset" && hasValue) presetPath = argv[++i];
        else if (a == L"--margin-ms" && hasValue) marginMs = _wtof(argv[++i]);
        else if (a == L"--seconds" && hasValue) seconds = _wtof(argv[++i]);
        else {
            fwprintf(stderr, L"unknown argument: %s\n\n", argv[i]);
            usage();
            return 2;
        }
    }

    // ---- resolve devices
    std::wstring err;
    const std::vector<AudioDevice> outputs = listDevices(true, err);
    const std::vector<AudioDevice> inputs = listDevices(false, err);

    if (toSel.empty()) {
        fwprintf(stderr, L"error: choose the real output with --to\n\n");
        printDevices();
        return 2;
    }
    const AudioDevice* to = findDevice(outputs, toSel, err);
    if (!to) {
        fwprintf(stderr, L"error: --to: %s\n\n", err.c_str());
        printDevices();
        return 2;
    }
    if (lower(to->name).find(L"cable input") != std::wstring::npos) {
        fwprintf(stderr, L"error: --to is the virtual cable itself; that would feed the output back into the input.\n"
                         L"       Pick your real speakers / headphones / interface.\n");
        return 2;
    }

    RouterConfig cfg;
    cfg.renderId = to->id;
    cfg.marginMs = marginMs;
    std::wstring fromName;
    if (lower(fromSel).rfind(L"loopback:", 0) == 0) {
        const AudioDevice* src = findDevice(outputs, fromSel.substr(9), err);
        if (!src) {
            fwprintf(stderr, L"error: --from loopback: %s\n", err.c_str());
            return 2;
        }
        if (src->id == to->id) {
            fwprintf(stderr, L"error: loopback source and output are the same device (feedback)\n");
            return 2;
        }
        cfg.captureId = src->id;
        cfg.loopback = true;
        fromName = L"loopback of " + src->name;
    } else {
        const AudioDevice* src = nullptr;
        if (fromSel.empty()) {
            for (const AudioDevice& d : inputs)
                if (lower(d.name).find(L"cable output") != std::wstring::npos)
                    src = &d;
            if (!src) {
                fwprintf(stderr,
                         L"error: no virtual cable found. Install VB-Audio Virtual Cable (https://vb-audio.com/Cable/),\n"
                         L"       or name a recording device with --from.\n");
                return 2;
            }
        } else if (!(src = findDevice(inputs, fromSel, err))) {
            fwprintf(stderr, L"error: --from: %s\n", err.c_str());
            return 2;
        }
        cfg.captureId = src->id;
        fromName = src->name;
    }

    // ---- preset
    ensurePresetExists(presetPath);
    eqdsp::EqSettings settings;
    settings.limiterEnabled = 1;
    loadPreset(presetPath, settings, true);
    ULONGLONG presetStamp = lastWrite(presetPath);
    wprintf(L"Preset file: %s (edit and save to change the EQ live)\n", presetPath.c_str());

    SetConsoleCtrlHandler(onCtrl, TRUE);

    Router router;
    const ULONGLONG startTick = GetTickCount64();
    int attempts = 0;
    while (!g_quit.load()) {
        router.settings().publish(settings);
        if (!router.start(cfg, err)) {
            fwprintf(stderr, L"\nerror: %s\n", err.c_str());
            if (++attempts > 3 && seconds > 0)
                return 1;
            fwprintf(stderr, L"retrying in 2 s (Ctrl+C to quit)...\n");
            Sleep(2000);
            continue;
        }
        attempts = 0;
        RouterStats& st = router.stats();
        wprintf(L"\n%s  (%d Hz, %d ch)\n  -> EQ -> %s  (%d Hz, %d ch)\n", fromName.c_str(), st.captureRate.load(),
                st.captureChannels.load(), to->name.c_str(), st.renderRate.load(), st.renderChannels.load());
        wprintf(L"  capture packet %d frames, output buffer %d frames, buffer target %.1f ms\n",
                st.capturePeriodFrames.load(), st.renderBufferFrames.load(), st.targetMs.load());

        ULONGLONG lastPrint = 0, lastPoll = 0;
        while (!g_quit.load() && !router.failed()) {
            Sleep(50);
            const ULONGLONG now = GetTickCount64();
            if (seconds > 0 && now - startTick >= static_cast<ULONGLONG>(seconds * 1000))
                g_quit.store(true);
            if (now - lastPoll >= 300) {
                lastPoll = now;
                const ULONGLONG stamp = lastWrite(presetPath);
                if (stamp != presetStamp) {
                    presetStamp = stamp;
                    Sleep(50); // let the editor finish writing
                    eqdsp::EqSettings next = settings;
                    if (loadPreset(presetPath, next, true)) {
                        next.limiterEnabled = settings.limiterEnabled;
                        settings = next;
                        router.settings().publish(settings);
                    }
                }
            }
            if (now - lastPrint >= 1000) {
                lastPrint = now;
                printStatus(st);
            }
        }

        if (router.failed()) {
            fwprintf(stderr, L"\n\naudio stream stopped: %s\nrestarting in 2 s...\n", router.failure().c_str());
            router.stop();
            Sleep(2000);
            continue;
        }

        const RouterStats& fin = router.stats();
        const double el = (GetTickCount64() - startTick) / 1000.0;
        wprintf(L"\n\nCounters over %.1f s: captured %llu frames in %llu packets (%.0f/s), synthesized %llu (%.0f/s), "
                L"rendered %llu (%.0f/s)",
                el, fin.capturedFrames.load(), fin.capturePackets.load(), fin.capturedFrames.load() / el,
                fin.synthesizedFrames.load(), fin.synthesizedFrames.load() / el, fin.renderedFrames.load(),
                fin.renderedFrames.load() / el);
        wprintf(L"\nOutput device clock vs system clock: %+.1f ppm (measured over %.1f s)",
                fin.renderClockPpm.load(), fin.renderClockSpanSec.load());
        wprintf(L"\n\nSummary: drift %+.1f ppm, buffer %.1f ms (target %.1f), underruns %llu, overrun frames %llu, "
                L"resyncs %llu, discontinuities %llu, silence trimmed %.1f ms / padded %.1f ms, cpu %.2f%%\n",
                fin.ppm.load(), fin.fillMs.load(), fin.targetMs.load(), fin.underruns.load(), fin.overrunFrames.load(),
                fin.resyncs.load(), fin.discontinuities.load(),
                fin.silenceTrimmedFrames.load() * 1000.0 / std::max(1, fin.captureRate.load()),
                fin.silencePaddedFrames.load() * 1000.0 / std::max(1, fin.captureRate.load()), fin.cpuPercent.load());
        router.stop();

        wchar_t tracePath[MAX_PATH];
        if (GetEnvironmentVariableW(L"WEQ_ROUTER_TRACE", tracePath, MAX_PATH)) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, tracePath, L"w") == 0 && f) {
                std::fprintf(f, "t,fifo,accrued,resampler,fill,smoothed,ppm,avail,padding,buffering\n");
                const auto& tr = router.trace();
                const double t0 = tr.empty() ? 0 : tr.front().t;
                for (const auto& r : tr)
                    std::fprintf(f, "%.5f,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%u,%d\n", r.t - t0, r.fifo, r.accrued,
                                 r.resampler, r.fill, r.smoothed, r.ppm, r.avail, r.padding, r.buffering ? 1 : 0);
                std::fclose(f);
                wprintf(L"trace written to %s\n", tracePath);
            }
        }
    }

    CoUninitialize();
    return 0;
}
