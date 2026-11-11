#include "App.h"

#include "Presets.h"
#include "Storage.h"

#include "eqdsp/BiquadDesign.h"
#include "eqdsp/PresetParser.h"
#include "weq/Install.h"
#include "weq/Version.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace gui {

using eqdsp::Band;
using eqdsp::EqSettings;
using eqdsp::FilterType;

namespace {

constexpr double kFMin = 20.0, kFMax = 20000.0;
constexpr double kRanges[] = {6, 12, 18, 24};
constexpr float kColStep = 3.0f; // curve sample spacing (DIPs)
constexpr double kSpecLo = -100.0, kSpecHi = -10.0;

const D2D1_COLOR_F kBandColors[] = {
    rgb(0x4DA3FF), rgb(0xFF8A3D), rgb(0x3DDC97), rgb(0xFF5D8F), rgb(0xB07CFF),
    rgb(0xFFD43B), rgb(0x22D3EE), rgb(0xFF6B6B), rgb(0x9BE15D), rgb(0xF472B6),
};
D2D1_COLOR_F bandColor(int i) { return kBandColors[i % 10]; }

const std::vector<std::wstring> kTypeNames = {L"Peak",      L"Low shelf", L"High shelf", L"Low pass",
                                              L"High pass", L"Band pass", L"Notch",      L"All pass"};
const wchar_t* kTypeShort[] = {L"Peak", L"Low shelf", L"High shelf", L"Low pass",
                               L"High pass", L"Band pass", L"Notch", L"All pass"};

bool hasGain(FilterType t) { return t == FilterType::Peaking || t == FilterType::LowShelf || t == FilterType::HighShelf; }

bool sameSettings(const EqSettings& a, const EqSettings& b) { return std::memcmp(&a, &b, sizeof(EqSettings)) == 0; }

bool isCableName(const std::wstring& n)
{
    std::wstring l = n;
    for (auto& c : l)
        c = static_cast<wchar_t>(towlower(c));
    return l.find(L"cable") != std::wstring::npos || l.find(L"vb-audio") != std::wstring::npos;
}

ComPtr<ID2D1PathGeometry> makePath(ID2D1Factory* f, const std::vector<D2D1_POINT_2F>& pts, bool close)
{
    ComPtr<ID2D1PathGeometry> geo;
    if (pts.size() < 2 || FAILED(f->CreatePathGeometry(&geo)))
        return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure(pts[0], close ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts.data() + 1, static_cast<UINT32>(pts.size() - 1));
    sink->EndFigure(close ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return geo;
}

std::wstring fmt(const wchar_t* f, double v)
{
    wchar_t b[64];
    swprintf(b, 64, f, v);
    return b;
}

} // namespace

double App::rangeDb() const { return kRanges[std::clamp(rangeSel_, 0, 3)]; }

double App::idleFrameInterval(bool foreground) const
{
    // Moving content: the analyzer (while audio plays) and the meters. Rates
    // halve when the window is visible but not the active one.
    const bool spectrum = analyzerMode_ != 2 && analyzer_.hasSignal();
    const bool meters = meterDb_[0] > -90 || meterDb_[1] > -90 || limitFlash_ > 0;
    // Each presented frame has a fixed GPU cost that dominates on many
    // systems, so these are as low as still looks smooth.
    if (spectrum)
        return foreground ? 1.0 / 24.0 : 1.0 / 12.0;
    if (meters)
        return foreground ? 1.0 / 15.0 : 1.0 / 8.0;
    return 1.0 / 2.0; // nothing moving: status bar only
}

// ================================================================== lifecycle

bool App::init(HWND hwnd, bool route)
{
    hwnd_ = hwnd;
    route_ = route;
    analysisBuf_.resize(32768 * 2);

    analyzerMode_ = std::clamp(_wtoi(configGet(L"analyzer", L"0").c_str()), 0, 2);
    rangeSel_ = std::clamp(_wtoi(configGet(L"range", L"1").c_str()), 0, 3);
    limiter_ = configGet(L"limiter", L"1") != L"0";
    outputId_ = configGet(L"output");
    inputId_ = configGet(L"input");
    presetName_ = configGet(L"preset", L"Current EQ (eq.txt)");
    modified_ = configGet(L"modified", L"0") == L"1";

    // The live EQ is eq.txt (shared with weq-router); create it if missing.
    std::string text;
    if (readTextFile(currentPresetPath(), text)) {
        std::vector<std::string> warnings;
        EqSettings parsed{};
        if (eqdsp::parseEqualizerApoConfig(text, parsed, warnings))
            s_ = parsed;
    } else {
        s_ = EqSettings{};
        saveCurrentNow();
    }
    ownStamp_ = fileStamp(currentPresetPath());
    s_.bypass = 0;
    s_.limiterEnabled = 1;
    frameStart_ = s_;
    sel_ = s_.numBands > 0 ? 0 : -1;

    rebuildPresetList();
    refreshDevices(true);
    publish();
    if (route_)
        startRouter();
    return true;
}

void App::shutdown()
{
    if (saveIn_ >= 0)
        saveCurrentNow();
    stopRouter();
    configSet(L"analyzer", std::to_wstring(analyzerMode_));
    configSet(L"range", std::to_wstring(rangeSel_));
    configSet(L"limiter", limiter_ ? L"1" : L"0");
    configSet(L"preset", presetName_);
    configSet(L"modified", modified_ ? L"1" : L"0");
}

// ================================================================== routing

void App::refreshDevices(bool force)
{
    std::wstring err;
    auto renders = weq::listDevices(true, err);
    auto captures = weq::listDevices(false, err);

    defaultOutputName_.clear();
    defaultIsCable_ = false;
    for (const auto& d : renders)
        if (d.isDefault) {
            defaultOutputName_ = d.name;
            defaultIsCable_ = isCableName(d.name);
        }

    std::vector<weq::AudioDevice> outs, ins;
    for (const auto& d : renders)
        if (!isCableName(d.name))
            outs.push_back(d);
    ins = captures;
    cableInstalled_ = std::any_of(ins.begin(), ins.end(), [](const weq::AudioDevice& d) { return isCableName(d.name); });

    auto sameList = [](const std::vector<weq::AudioDevice>& a, const std::vector<weq::AudioDevice>& b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].id != b[i].id || a[i].name != b[i].name)
                return false;
        return true;
    };
    if (!force && sameList(outs, outputs_) && sameList(ins, inputs_))
        return;
    outputs_ = std::move(outs);
    inputs_ = std::move(ins);

    outputNames_.clear();
    for (const auto& d : outputs_)
        outputNames_.push_back(d.name);
    inputNames_.clear();
    for (const auto& d : inputs_)
        inputNames_.push_back(d.name);

    // Output: saved choice, else the Windows default if it is a real device,
    // else the first real device.
    outputSel_ = -1;
    for (size_t i = 0; i < outputs_.size(); ++i)
        if (outputs_[i].id == outputId_)
            outputSel_ = static_cast<int>(i);
    if (outputSel_ < 0) {
        for (size_t i = 0; i < outputs_.size(); ++i)
            if (outputs_[i].isDefault)
                outputSel_ = static_cast<int>(i);
        if (outputSel_ < 0 && !outputs_.empty())
            outputSel_ = 0;
        if (outputSel_ >= 0 && outputId_.empty())
            outputId_ = outputs_[outputSel_].id;
    }
    // Input: saved choice, else VB-Cable's "CABLE Output".
    inputSel_ = -1;
    for (size_t i = 0; i < inputs_.size(); ++i)
        if (inputs_[i].id == inputId_)
            inputSel_ = static_cast<int>(i);
    if (inputSel_ < 0) {
        for (size_t i = 0; i < inputs_.size(); ++i)
            if (inputs_[i].name.find(L"CABLE Output") != std::wstring::npos)
                inputSel_ = static_cast<int>(i);
        if (inputSel_ >= 0 && inputId_.empty())
            inputId_ = inputs_[inputSel_].id;
    }
}

void App::startRouter()
{
    if (outputId_.empty() || inputId_.empty()) {
        routerError_ = cableInstalled_ ? L"choose an input and an output device" : L"VB-Cable is not installed";
        retryIn_ = 3.0;
        return;
    }
    weq::RouterConfig cfg;
    cfg.captureId = inputId_;
    cfg.renderId = outputId_;
    publish();
    std::wstring err;
    if (router_.start(cfg, err)) {
        routerStarted_ = true;
        routerError_.clear();
        analyzer_.clear();
    } else {
        routerStarted_ = false;
        routerError_ = err;
        retryIn_ = 3.0;
    }
}

void App::stopRouter()
{
    router_.stop();
    routerStarted_ = false;
}

void App::pollRouter(double dt)
{
    if (!route_)
        return;
    if (routerStarted_ && router_.failed()) {
        routerError_ = router_.failure();
        stopRouter();
        retryIn_ = 2.0;
    }
    if (!routerStarted_) {
        retryIn_ -= dt;
        if (retryIn_ <= 0) {
            refreshDevices(false);
            startRouter();
        }
    }
    devicePollIn_ -= dt;
    if (devicePollIn_ <= 0) {
        devicePollIn_ = 2.5;
        refreshDevices(false);
    }
}

double App::sampleRate() const
{
    const int r = const_cast<weq::Router&>(router_).stats().renderRate.load();
    return r > 0 ? r : 48000.0;
}

void App::pollAnalysis(double dt, bool visible)
{
    analyzer_.setSampleRate(sampleRate());
    analyzer_.setComputePre(analyzerMode_ == 0);
    const bool analyze = visible && analyzerMode_ != 2;
    for (;;) {
        const size_t n = router_.readAnalysis(analysisBuf_.data(), analysisBuf_.size() / 2);
        if (n == 0)
            break;
        if (analyze)
            analyzer_.push(analysisBuf_.data(), n);
    }
    if (analyze && demoSignal_)
        feedDemoSignal(dt);
    if (analyze)
        analyzer_.update(dt);

    auto& st = router_.stats();
    for (int c = 0; c < 2; ++c) {
        const float p = st.peakCh[c].exchange(0.0f);
        const float db = p > 1e-7f ? 20.0f * std::log10(p) : -120.0f;
        meterDb_[c] = std::max(db, meterDb_[c] - static_cast<float>(dt * 26.0)); // 26 dB/s fall
        if (db >= holdDb_[c] || holdAge_[c] > 1.6) {
            holdDb_[c] = std::max(db, meterDb_[c]);
            holdAge_[c] = 0;
        }
        holdAge_[c] += dt;
    }
    const double lim = st.limiterDb.load();
    if (lim < -0.1)
        limitFlash_ = 0.6;
    limitFlash_ = std::max(0.0, limitFlash_ - dt);
}

void App::feedDemoSignal(double dt)
{
    const double fs = sampleRate();
    if (!demoEqReady_) {
        demoEq_.prepare(fs, 1);
        demoEqReady_ = true;
    }
    EqSettings eqs = s_;
    eqs.bypass = bypass_ ? 1u : 0u;
    demoEq_.setSettings(eqs);
    const size_t n = std::min(static_cast<size_t>(dt * fs), analysisBuf_.size() / 2);
    std::vector<float> in(n), out(n);
    for (size_t i = 0; i < n; ++i) {
        // Roughly pink noise (two one-pole low-passes of white noise) plus a
        // bass line, so the display looks like music.
        demoRng_ = demoRng_ * 1664525u + 1013904223u;
        const float white = static_cast<float>(static_cast<int32_t>(demoRng_)) / 2147483648.0f;
        demoLp_[0] += (white - demoLp_[0]) * 0.02f;
        demoLp_[1] += (white - demoLp_[1]) * 0.3f;
        demoPhase_ += 2 * 3.14159265358979 * 82.4 / fs;
        in[i] = 0.25f * demoLp_[0] * 6 + 0.05f * demoLp_[1] + 0.12f * static_cast<float>(std::sin(demoPhase_));
    }
    if (demoPhase_ > 1e6)
        demoPhase_ = std::fmod(demoPhase_, 2 * 3.14159265358979);
    demoEq_.process(in.data(), out.data(), static_cast<int>(n));
    for (size_t i = 0; i < n; ++i) {
        analysisBuf_[2 * i] = in[i];
        analysisBuf_[2 * i + 1] = out[i];
    }
    analyzer_.push(analysisBuf_.data(), n);
}

// ================================================================== EQ model

void App::publish()
{
    EqSettings p = s_;
    p.bypass = bypass_ ? 1u : 0u;
    p.limiterEnabled = limiter_ ? 1u : 0u;
    router_.settings().publish(p);
}

void App::onSettingsChanged()
{
    publish();
    saveIn_ = 0.4;
    modified_ = true;
    presetSel_ = -1;
}

void App::undo()
{
    if (undo_.empty())
        return;
    redo_.push_back(s_);
    s_ = undo_.back();
    undo_.pop_back();
    frameStart_ = s_;
    sel_ = std::min(sel_, static_cast<int>(s_.numBands) - 1);
    onSettingsChanged();
}

void App::redo()
{
    if (redo_.empty())
        return;
    undo_.push_back(s_);
    s_ = redo_.back();
    redo_.pop_back();
    frameStart_ = s_;
    sel_ = std::min(sel_, static_cast<int>(s_.numBands) - 1);
    onSettingsChanged();
}

void App::saveCurrentNow()
{
    EqSettings out = s_;
    out.bypass = 0;
    std::string text = "# Windows EQ - live preset (edited by the Windows EQ app; weq-router reads it too)\n";
    text += eqdsp::formatEqualizerApoConfig(out);
    writeTextFileAtomic(currentPresetPath(), text);
    ownStamp_ = fileStamp(currentPresetPath());
    saveIn_ = -1;
}

void App::pollExternalEdits(double dt)
{
    if (saveIn_ >= 0) {
        saveIn_ -= dt;
        if (saveIn_ < 0)
            saveCurrentNow();
        return;
    }
    externalPollIn_ -= dt;
    if (externalPollIn_ > 0)
        return;
    externalPollIn_ = 0.5;
    const unsigned long long stamp = fileStamp(currentPresetPath());
    if (stamp == 0 || stamp == ownStamp_)
        return;
    ownStamp_ = stamp;
    std::string text;
    std::vector<std::string> warnings;
    EqSettings parsed{};
    if (readTextFile(currentPresetPath(), text) && eqdsp::parseEqualizerApoConfig(text, parsed, warnings)) {
        parsed.bypass = 0;
        parsed.limiterEnabled = 1;
        if (!sameSettings(parsed, s_)) {
            undo_.push_back(s_);
            redo_.clear();
            s_ = parsed;
            frameStart_ = s_;
            sel_ = std::min(sel_, static_cast<int>(s_.numBands) - 1);
            publish();
            presetName_ = L"eq.txt (edited outside the app)";
            modified_ = false;
            presetSel_ = -1;
        }
    }
}

void App::addBand(FilterType type, double f, double g, double q)
{
    if (s_.numBands >= static_cast<uint32_t>(eqdsp::kMaxBands))
        return;
    Band& b = s_.bands[s_.numBands];
    b = Band{};
    b.type = type;
    b.frequencyHz = f;
    b.gainDb = g;
    b.q = q;
    sel_ = static_cast<int>(s_.numBands);
    ++s_.numBands;
}

void App::deleteBand(int index)
{
    if (index < 0 || index >= static_cast<int>(s_.numBands))
        return;
    for (uint32_t i = static_cast<uint32_t>(index); i + 1 < s_.numBands; ++i)
        s_.bands[i] = s_.bands[i + 1];
    --s_.numBands;
    s_.bands[s_.numBands] = Band{};
    if (sel_ >= static_cast<int>(s_.numBands))
        sel_ = static_cast<int>(s_.numBands) - 1;
}

// ================================================================== presets

void App::rebuildPresetList()
{
    userPresets_ = listUserPresets();
    presetItems_.clear();
    for (const auto& n : userPresets_)
        presetItems_.push_back(n);
    for (const auto& b : builtinPresets())
        presetItems_.push_back(L"Built-in · " + b.name);
    syncPresetSelection();
}

void App::syncPresetSelection()
{
    presetSel_ = -1;
    if (modified_)
        return;
    for (size_t i = 0; i < presetItems_.size(); ++i)
        if (presetItems_[i] == presetName_)
            presetSel_ = static_cast<int>(i);
}

void App::applyPreset(int index)
{
    if (index < 0 || index >= static_cast<int>(presetItems_.size()))
        return;
    EqSettings next{};
    const int nUser = static_cast<int>(userPresets_.size());
    if (index < nUser) {
        std::string text;
        std::vector<std::string> warnings;
        if (!readTextFile(userPresetPath(userPresets_[index]), text) ||
            !eqdsp::parseEqualizerApoConfig(text, next, warnings)) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
    } else {
        next = builtinPresets()[index - nUser].settings;
    }
    next.bypass = 0;
    next.limiterEnabled = 1;
    s_ = next;
    sel_ = s_.numBands > 0 ? 0 : -1;
    publish();
    saveIn_ = 0.05;
    presetName_ = presetItems_[index];
    modified_ = false;
    presetSel_ = index;
    frameStart_ = s_; // the undo snapshot is taken by the caller's gesture
}

void App::saveUserPreset(const std::wstring& rawName)
{
    const std::wstring name = sanitizeFileName(rawName);
    if (name.empty())
        return;
    EqSettings out = s_;
    out.bypass = 0;
    std::string text = "# Windows EQ preset\n" + eqdsp::formatEqualizerApoConfig(out);
    if (!writeTextFileAtomic(userPresetPath(name), text)) {
        MessageBoxW(hwnd_, L"Could not write the preset file.", L"Windows EQ", MB_ICONERROR);
        return;
    }
    presetName_ = name;
    modified_ = false;
    rebuildPresetList();
}

void App::runDeferred()
{
    if (inDeferred_ || deferred_ == Deferred::None)
        return;
    inDeferred_ = true;
    const Deferred what = deferred_;
    deferred_ = Deferred::None;

    switch (what) {
    case Deferred::Import: {
        const std::wstring path = openPresetDialog(hwnd_);
        if (!path.empty()) {
            std::string text;
            std::vector<std::string> warnings;
            EqSettings next{};
            if (readTextFile(path, text) && eqdsp::parseEqualizerApoConfig(text, next, warnings)) {
                undo_.push_back(s_);
                redo_.clear();
                next.bypass = 0;
                next.limiterEnabled = 1;
                s_ = next;
                frameStart_ = s_;
                sel_ = s_.numBands > 0 ? 0 : -1;
                publish();
                saveCurrentNow();
                presetName_ = fileStem(path);
                modified_ = true; // imported, not yet saved as a preset
                presetSel_ = -1;
                if (!warnings.empty()) {
                    std::wstring msg = L"Imported with " + std::to_wstring(warnings.size()) +
                                       L" line(s) skipped:\n\n";
                    for (size_t i = 0; i < warnings.size() && i < 8; ++i) {
                        const std::string& w = warnings[i];
                        msg += std::wstring(w.begin(), w.end()) + L"\n";
                    }
                    MessageBoxW(hwnd_, msg.c_str(), L"Windows EQ", MB_ICONINFORMATION);
                }
            } else {
                MessageBoxW(hwnd_, L"That file doesn't contain any Equalizer APO / AutoEQ filter lines.",
                            L"Windows EQ", MB_ICONWARNING);
            }
        }
        break;
    }
    case Deferred::Export: {
        const std::wstring path = savePresetDialog(hwnd_, presetName_);
        if (!path.empty()) {
            EqSettings out = s_;
            out.bypass = 0;
            if (!writeTextFileAtomic(path, eqdsp::formatEqualizerApoConfig(out)))
                MessageBoxW(hwnd_, L"Could not write the file.", L"Windows EQ", MB_ICONERROR);
        }
        break;
    }
    case Deferred::DeletePreset: {
        const std::wstring msg = L"Delete the preset \"" + pendingName_ + L"\"?";
        if (MessageBoxW(hwnd_, msg.c_str(), L"Windows EQ", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            DeleteFileW(userPresetPath(pendingName_).c_str());
            if (presetName_ == pendingName_)
                modified_ = true;
            rebuildPresetList();
        }
        break;
    }
    case Deferred::ConfirmOverwrite: {
        const std::wstring msg = L"A preset named \"" + pendingName_ + L"\" already exists. Replace it?";
        if (MessageBoxW(hwnd_, msg.c_str(), L"Windows EQ", MB_YESNO | MB_ICONQUESTION) == IDYES)
            saveUserPreset(pendingName_);
        break;
    }
    case Deferred::OpenSoundSettings:
        ShellExecuteW(hwnd_, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case Deferred::SettingsMenu:
        if (showMenu(settingsMenuPt_, false) == kMenuQuit)
            PostMessageW(hwnd_, kMsgQuitAsk, 0, 0);
        break;
    default:
        break;
    }
    inDeferred_ = false;
}

// ================================================================== frame

void App::tick(double dt, bool visible)
{
    pollRouter(dt);
    pollAnalysis(dt, visible);
    pollExternalEdits(dt);
}

void App::setBypassed(bool b)
{
    bypass_ = b;
    publish();
}

std::wstring App::trayTip() const
{
    std::wstring s = L"Windows EQ — ";
    auto& st = const_cast<weq::Router&>(router_).stats();
    if (!routerStarted_)
        s += L"stopped";
    else if (!st.running.load())
        s += L"starting";
    else
        s += bypass_ ? L"bypassed" : L"on";
    std::wstring preset = presetName_;
    const std::wstring prefix = L"Built-in · ";
    if (preset.rfind(prefix, 0) == 0)
        preset = preset.substr(prefix.size());
    s += L"\n" + preset + (modified_ ? L" (modified)" : L"");
    return s.substr(0, 127); // NOTIFYICONDATA tip limit
}

unsigned App::stateKey() const
{
    auto& st = const_cast<weq::Router&>(router_).stats();
    unsigned k = (bypass_ ? 1u : 0u) | (routerStarted_ ? 2u : 0u) | (st.running.load() ? 4u : 0u) |
                 (modified_ ? 8u : 0u);
    for (wchar_t c : presetName_)
        k = k * 31u + c;
    return k;
}

int App::showMenu(POINT pt, bool fromTray)
{
    enum { kOpen = 100, kEq, kAutostart, kCloseToTray, kSound, kAbout, kUninstall, kQuit, kPreset0 = 1000 };
    HMENU menu = CreatePopupMenu();
    HMENU presets = CreatePopupMenu();
    for (size_t i = 0; i < presetItems_.size() && i < 500; ++i)
        AppendMenuW(presets, MF_STRING | (static_cast<int>(i) == presetSel_ && !modified_ ? MF_CHECKED : 0),
                    kPreset0 + i, presetItems_[i].c_str());
    if (fromTray) {
        AppendMenuW(menu, MF_STRING, kOpen, L"Open Windows EQ");
        SetMenuDefaultItem(menu, kOpen, FALSE);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING | (bypass_ ? 0 : MF_CHECKED), kEq, L"EQ on\tB");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(presets), L"Preset");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (weq::install::autostartEnabled() ? MF_CHECKED : 0), kAutostart,
                L"Start with Windows (in the tray)");
    AppendMenuW(menu, MF_STRING | (configGet(L"closeToTray", L"1") != L"0" ? MF_CHECKED : 0), kCloseToTray,
                L"Keep running in the tray when closed");
    AppendMenuW(menu, MF_STRING, kSound, L"Windows Sound settings…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kAbout, L"About Windows EQ");
    if (weq::install::isInstalled())
        AppendMenuW(menu, MF_STRING, kUninstall, L"Uninstall…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuit, L"Quit Windows EQ");

    SetForegroundWindow(hwnd_); // so the menu closes when clicking elsewhere
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu); // also destroys the submenu

    int result = 0;
    if (cmd == kOpen)
        result = kMenuOpen;
    else if (cmd == kQuit)
        result = kMenuQuit;
    else if (cmd == kEq)
        setBypassed(!bypass_);
    else if (cmd == kAutostart) {
        const bool on = !weq::install::autostartEnabled();
        // Prefer the installed copy, so sign-in launch survives deleting a build folder.
        const std::wstring exe = weq::install::isInstalled() ? weq::install::installedExePath()
                                                             : weq::install::currentExePath();
        if (!weq::install::setAutostart(on, exe))
            MessageBoxW(hwnd_, L"Could not change the sign-in setting.", L"Windows EQ", MB_ICONWARNING);
    } else if (cmd == kCloseToTray) {
        configSet(L"closeToTray", configGet(L"closeToTray", L"1") != L"0" ? L"0" : L"1");
    } else if (cmd == kSound) {
        ShellExecuteW(hwnd_, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == kAbout) {
        const std::wstring msg = std::wstring(L"Windows EQ ") + WEQ_VERSION_WSTR +
                                 L"\n\nSystem-wide parametric EQ: Windows audio → VB-Cable → EQ → "
                                 L"your output device.\n\nSettings and presets: " +
                                 appDataDir() + L"\n\nOpen source under the MIT License.";
        MessageBoxW(hwnd_, msg.c_str(), L"About Windows EQ", MB_ICONINFORMATION);
    } else if (cmd == kUninstall) {
        ShellExecuteW(nullptr, L"open", weq::install::installedExePath().c_str(), L"--uninstall", nullptr,
                      SW_SHOWNORMAL);
    } else if (cmd >= kPreset0 && cmd < kPreset0 + static_cast<int>(presetItems_.size())) {
        undo_.push_back(s_);
        redo_.clear();
        applyPreset(cmd - kPreset0);
    }
    return result;
}

void App::frame(Ui& ui, double dt)
{
    tick(dt, true);

    frameStart_ = s_;
    if (ui.in.pressed || ui.in.rpressed) {
        gestureBase_ = s_;
        gesture_ = true;
    }

    // Keyboard shortcuts (not while typing into a field).
    if (!ui.editing()) {
        if (ui.in.keyUndo)
            undo();
        if (ui.in.keyRedo)
            redo();
        if (ui.in.keyB) {
            bypass_ = !bypass_;
            publish();
        }
        if (ui.in.keyDelete && sel_ >= 0) {
            undo_.push_back(s_);
            redo_.clear();
            deleteBand(sel_);
            frameStart_ = s_;
            onSettingsChanged();
        }
    }
    frameStart_ = s_;

    const float W = ui.width(), H = ui.height();
    ui.fill(Rect{0, 0, W, H}, col::bg);
    // A minimized window can still get a paint with a 0x0 client area; below
    // the minimum size the layout would go negative, so draw nothing.
    if (W < kMinLayoutW || H < kMinLayoutH)
        return;

    const float pad = 16;
    const Rect top{0, 0, W, 64};
    const Rect status{0, H - 30, W, 30};
    const Rect tool{pad, top.b(), W - 2 * pad, 44};
    const Rect band{pad, status.y - 128, W - 2 * pad, 120};
    float mainY = tool.b() + 4;
    const std::wstring banner = bannerText();
    Rect bannerR{pad, mainY, W - 2 * pad, 0};
    if (!banner.empty()) {
        bannerR.h = 38;
        mainY = bannerR.b() + 8;
    }
    const float sideW = 132;
    const Rect graph{pad, mainY, W - 2 * pad - sideW - 12, band.y - 10 - mainY};
    const Rect side{graph.r() + 12, mainY, sideW, graph.h};

    if (!(benchSkip_ & 8)) {
        drawTopBar(ui, top);
        drawToolbar(ui, tool);
    }
    if (!banner.empty())
        drawBanner(ui, bannerR);
    if (!(benchSkip_ & 1))
        drawGraph(ui, graph);
    if (!(benchSkip_ & 2))
        drawSide(ui, side);
    if (!(benchSkip_ & 4))
        drawBandPanel(ui, band);
    if (!(benchSkip_ & 8))
        drawStatusBar(ui, status);

    // ---- undo bookkeeping: a mouse gesture is one step; other changes
    // (wheel, typing) coalesce for 0.8 s.
    if (!sameSettings(frameStart_, s_)) {
        if (!gesture_) {
            if (ui.time() - lastLooseChange_ > 0.8) {
                undo_.push_back(frameStart_);
                redo_.clear();
            }
            lastLooseChange_ = ui.time();
        }
        onSettingsChanged();
    }
    if (gesture_ && !ui.in.down) {
        if (!sameSettings(gestureBase_, s_)) {
            undo_.push_back(gestureBase_);
            redo_.clear();
        }
        gesture_ = false;
    }
    if (undo_.size() > 200)
        undo_.erase(undo_.begin());
}

std::wstring App::bannerText() const
{
    if (!route_)
        return L"";
    if (!cableInstalled_)
        return L"VB-Cable isn't installed. Install it from vb-audio.com/Cable, then choose \"CABLE Input\" as the "
               L"Windows output.";
    if (!routerError_.empty() && !routerStarted_)
        return L"Audio routing stopped: " + routerError_ + L". Retrying…";
    if (!defaultIsCable_)
        return L"Windows is playing to \"" + defaultOutputName_ +
               L"\" directly, so the EQ isn't in the path. Make \"CABLE Input\" the default output.";
    return L"";
}

// ================================================================== top bar

void App::drawTopBar(Ui& ui, Rect r)
{
    ui.fill(r, col::panel);
    ui.line(r.x, r.b() - 0.5f, r.r(), r.b() - 0.5f, col::border);

    // Logo: three bars of an EQ.
    const Rect logo{16, r.cy() - 16, 32, 32};
    ui.fill(logo, col::accent, 8);
    const float bars[3] = {0.45f, 0.8f, 0.6f};
    for (int i = 0; i < 3; ++i) {
        const float bh = 20 * bars[i];
        ui.fill(Rect{logo.x + 7 + i * 7.0f, logo.cy() + 10 - bh, 4, bh}, rgb(0x0B1420), 2);
    }
    ui.text(L"Windows EQ", Rect{58, r.y + 12, 200, 22}, col::text, Font::Title);
    ui.text(L"System-wide parametric equalizer", Rect{58, r.y + 34, 260, 16}, col::textDim, Font::Small);

    // A/B: the most important control when judging an EQ.
    const Rect ab{r.r() - 16 - 168, r.cy() - 18, 168, 36};
    {
        const Id id = makeId("ab");
        const bool hover = ui.over(ab);
        if (hover && ui.in.pressed)
            ui.setActive(id);
        if (ui.active() == id && ui.in.released) {
            if (hover) {
                bypass_ = !bypass_;
                publish();
            }
            ui.setActive(0);
        }
        if (!bypass_) {
            ui.fill(ab, hover ? col::accentHi : col::accent, 18);
            ui.text(L"", Rect{ab.x + 14, ab.y, 20, ab.h}, rgb(0x0B1420), Font::Icon, Align::Center);
            ui.text(L"EQ ON", Rect{ab.x + 40, ab.y, ab.w - 52, ab.h}, rgb(0x0B1420), Font::Bold, Align::Center);
        } else {
            ui.fill(ab, hover ? rgb(0x3A2E1A) : rgb(0x2C2416), 18);
            ui.stroke(ab, col::warn, 18, 1.5f);
            ui.text(L"", Rect{ab.x + 14, ab.y, 20, ab.h}, col::warn, Font::Icon, Align::Center);
            ui.text(L"BYPASSED", Rect{ab.x + 40, ab.y, ab.w - 52, ab.h}, col::warn, Font::Bold, Align::Center);
        }
        if (hover)
            ui.tooltip(L"Switch the EQ off and on to compare (shortcut: B)");
    }

    // Settings menu (sign-in launch, tray behaviour, about, uninstall, quit).
    const Rect gear{ab.x - 46, r.cy() - 18, 36, 36};
    if (ui.iconButton(makeId("settings"), gear, L"", L"Settings")) {
        const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
        POINT pt{MulDiv(static_cast<int>(gear.x), dpi, 96), MulDiv(static_cast<int>(gear.b() + 4), dpi, 96)};
        if (hwnd_)
            ClientToScreen(hwnd_, &pt);
        settingsMenuPt_ = pt;
        deferred_ = Deferred::SettingsMenu;
    }

    // Routing.
    float x = gear.x - 18;
    const float outW = std::clamp((x - 340) * 0.55f, 160.0f, 300.0f);
    const float inW = std::clamp((x - 340) * 0.45f - 70, 130.0f, 240.0f);
    const Rect outR{x - outW, r.cy() - 6, outW, 30};
    ui.text(L"OUTPUT", Rect{outR.x, r.y + 8, outR.w, 14}, col::textFaint, Font::Caps);
    int o = outputSel_;
    if (ui.combo(makeId("out"), outR, outputNames_, o, L"No output device") && o >= 0) {
        outputSel_ = o;
        outputId_ = outputs_[o].id;
        configSet(L"output", outputId_);
        stopRouter();
        startRouter();
    }
    const Rect arrow{outR.x - 30, outR.y, 22, outR.h};
    ui.text(L"", arrow, col::textFaint, Font::IconSmall, Align::Center);
    const Rect inR{arrow.x - 8 - inW, outR.y, inW, 30};
    ui.text(L"INPUT", Rect{inR.x, r.y + 8, inR.w, 14}, col::textFaint, Font::Caps);
    int i = inputSel_;
    if (ui.combo(makeId("in"), inR, inputNames_, i, L"No input") && i >= 0) {
        inputSel_ = i;
        inputId_ = inputs_[i].id;
        configSet(L"input", inputId_);
        stopRouter();
        startRouter();
    }
}

// ================================================================== toolbar

void App::drawToolbar(Ui& ui, Rect r)
{
    const float y = r.y + 7, h = 30;
    float x = r.x;

    ui.text(L"PRESET", Rect{x, y, 52, h}, col::textFaint, Font::Caps);
    x += 54;
    const Rect presetR{x, y, 270, h};
    if (saveAsOpen_) {
        bool cancelled = false;
        if (ui.textField(makeId("saveas"), presetR, saveAsName_, true, cancelled, L"Preset name…")) {
            const std::wstring name = sanitizeFileName(saveAsName_);
            saveAsOpen_ = false;
            if (!name.empty()) {
                if (std::find(userPresets_.begin(), userPresets_.end(), name) != userPresets_.end()) {
                    pendingName_ = name;
                    deferred_ = Deferred::ConfirmOverwrite;
                } else {
                    saveUserPreset(name);
                }
            }
        } else if (cancelled) {
            saveAsOpen_ = false;
        }
    } else {
        int sel = presetSel_;
        const std::wstring placeholder = presetName_ + (modified_ ? L"  • modified" : L"");
        if (ui.combo(makeId("preset"), presetR, presetItems_, sel, placeholder)) {
            undo_.push_back(s_);
            redo_.clear();
            applyPreset(sel);
        }
    }
    x = presetR.r() + 6;

    const bool isUser = !modified_ && presetSel_ >= 0 && presetSel_ < static_cast<int>(userPresets_.size());
    const bool canOverwrite = std::find(userPresets_.begin(), userPresets_.end(), presetName_) != userPresets_.end();
    if (ui.iconButton(makeId("save"), Rect{x, y, h, h}, L"",
                      canOverwrite ? L"Save to \"" + presetName_ + L"\"" : L"Save as a new preset")) {
        if (canOverwrite)
            saveUserPreset(presetName_);
        else {
            saveAsOpen_ = true;
            saveAsName_ = modified_ ? L"" : presetName_;
        }
    }
    x += h + 2;
    if (ui.iconButton(makeId("saveas_btn"), Rect{x, y, h, h}, L"", L"Save as a new preset…")) {
        saveAsOpen_ = true;
        saveAsName_.clear();
    }
    x += h + 2;
    if (ui.iconButton(makeId("delpreset"), Rect{x, y, h, h}, L"", L"Delete this preset", isUser)) {
        pendingName_ = userPresets_[presetSel_];
        deferred_ = Deferred::DeletePreset;
    }
    x += h + 10;
    ui.line(x, y + 6, x, y + h - 6, col::border);
    x += 10;
    if (ui.button(makeId("import"), Rect{x, y, 92, h}, L"Import", Btn::Ghost, true, L""))
        deferred_ = Deferred::Import;
    if (ui.over(Rect{x, y, 92, h}))
        ui.tooltip(L"Import an AutoEQ ParametricEQ.txt or Equalizer APO config");
    x += 94;
    if (ui.button(makeId("export"), Rect{x, y, 92, h}, L"Export", Btn::Ghost, true, L""))
        deferred_ = Deferred::Export;
    x += 100;
    ui.line(x, y + 6, x, y + h - 6, col::border);
    x += 10;
    if (ui.iconButton(makeId("undo"), Rect{x, y, h, h}, L"", L"Undo (Ctrl+Z)", !undo_.empty()))
        undo();
    x += h + 2;
    if (ui.iconButton(makeId("redo"), Rect{x, y, h, h}, L"", L"Redo (Ctrl+Y)", !redo_.empty()))
        redo();

    // Right side: view options.
    float rx = r.r();
    const Rect rangeR{rx - 104, y, 104, h};
    static const std::vector<std::wstring> ranges = {L"±6 dB", L"±12 dB", L"±18 dB", L"±24 dB"};
    int rs = rangeSel_;
    if (ui.combo(makeId("range"), rangeR, ranges, rs))
        rangeSel_ = rs;
    rx = rangeR.x - 8;
    static const std::vector<std::wstring> modes = {L"Before + after", L"After EQ only", L"Off"};
    const Rect anaR{rx - 196, y, 196, h};
    int am = analyzerMode_;
    if (ui.combo(makeId("analyzer"), anaR, modes, am, L"", L"Analyzer: ")) {
        analyzerMode_ = am;
        analyzer_.clear();
    }
    rx = anaR.x - 8;
    if (ui.button(makeId("reset"), Rect{rx - 78, y, 78, h}, L"Reset", Btn::Ghost, s_.numBands > 0 || s_.preampDb != 0,
                  L"")) {
        s_ = EqSettings{};
        sel_ = -1;
        presetName_ = L"Flat";
    }
    if (ui.over(Rect{rx - 78, y, 78, h}))
        ui.tooltip(L"Remove all bands and set the preamp to 0 dB");
}

void App::drawBanner(Ui& ui, Rect r)
{
    const bool error = !routerError_.empty() && !routerStarted_;
    const D2D1_COLOR_F c = error ? col::bad : col::warn;
    ui.fill(r, withAlpha(c, 0.12f), 8);
    ui.stroke(r, withAlpha(c, 0.45f), 8);
    ui.text(L"", Rect{r.x + 10, r.y, 22, r.h}, c, Font::Icon, Align::Center);
    const bool soundBtn = cableInstalled_ && !defaultIsCable_ && !error;
    ui.text(bannerText(), Rect{r.x + 40, r.y, r.w - (soundBtn ? 210 : 50), r.h}, col::text, Font::Body);
    if (soundBtn &&
        ui.button(makeId("soundsettings"), Rect{r.r() - 172, r.y + 5, 162, r.h - 10}, L"Open Sound settings"))
        deferred_ = Deferred::OpenSoundSettings;
}

// ================================================================== graph

void App::updateCurves(Rect g)
{
    if (!(g.w >= 2.0f)) { // also catches NaN
        curveL_.assign(2, 0.0f);
        curveR_.assign(2, 0.0f);
        bandCurves_.assign(s_.numBands, std::vector<float>(2, 0.0f));
        curveW_ = -1;
        return;
    }
    const double fs = sampleRate();
    if (curveX_ == g.x && curveW_ == g.w && curveFs_ == fs && sameSettings(curveKey_, s_))
        return;
    curveKey_ = s_;
    curveFs_ = fs;
    curveX_ = g.x;
    curveW_ = g.w;
    const int n = static_cast<int>(g.w / kColStep) + 2;
    curveL_.assign(n, 0.0f);
    curveR_.assign(n, 0.0f);
    bandCurves_.assign(s_.numBands, std::vector<float>(n, 0.0f));
    std::vector<eqdsp::BiquadCoeffs> coeffs(s_.numBands);
    for (uint32_t b = 0; b < s_.numBands; ++b) {
        Band band = s_.bands[b];
        band.enabled = 1;
        coeffs[b] = eqdsp::designBiquad(band, fs);
    }
    stereoDiffers_ = false;
    for (uint32_t b = 0; b < s_.numBands; ++b)
        if (s_.bands[b].enabled && ((s_.bands[b].channelMask & 3u) != 3u))
            stereoDiffers_ = true;
    const double ratio = std::log(kFMax / kFMin);
    for (int i = 0; i < n; ++i) {
        const double f = kFMin * std::exp(ratio * std::min(1.0, static_cast<double>(i * kColStep / g.w)));
        double l = 0, rr = 0;
        for (uint32_t b = 0; b < s_.numBands; ++b) {
            const double db = eqdsp::biquadMagnitudeDb(coeffs[b], f, fs);
            bandCurves_[b][i] = static_cast<float>(db);
            if (!s_.bands[b].enabled)
                continue;
            if (s_.bands[b].channelMask & 1u)
                l += db;
            if (s_.bands[b].channelMask & 2u)
                rr += db;
        }
        curveL_[i] = static_cast<float>(l);
        curveR_[i] = static_cast<float>(rr);
    }
}

void App::drawGraph(Ui& ui, Rect g)
{
    ui.fill(g, col::panel, 10);
    ui.stroke(g, col::border, 10);
    const Rect plot = g.inset(1);
    // Aliased (pixel-snapped) clip: an anti-aliased clip can make Direct2D
    // render the whole graph through an extra offscreen layer every frame.
    ui.rt()->PushAxisAlignedClip(plot.d2d(), D2D1_ANTIALIAS_MODE_ALIASED);

    const double range = rangeDb();
    const float padY = 18;
    const double ratio = std::log(kFMax / kFMin);
    auto xOf = [&](double f) { return static_cast<float>(g.x + std::log(f / kFMin) / ratio * g.w); };
    auto fOf = [&](float x) { return kFMin * std::exp(ratio * (x - g.x) / g.w); };
    const float halfH = g.h / 2 - padY;
    auto yOf = [&](double db) { return static_cast<float>(g.cy() - db / range * halfH); };
    auto specY = [&](double db) {
        return static_cast<float>(g.b() - (db - kSpecLo) / (kSpecHi - kSpecLo) * (g.h - 8));
    };

    // ---- grid
    for (int dec = 10; dec <= 10000; dec *= 10) {
        for (int m = 1; m <= 9; ++m) {
            const double f = dec * m;
            if (f < kFMin || f > kFMax)
                continue;
            const float x = xOf(f);
            ui.line(x, g.y, x, g.b(), m == 1 ? col::gridMajor : col::grid, 1);
        }
    }
    const double step = range <= 6 ? 2 : range <= 12 ? 3 : 6;
    for (double db = -range; db <= range + 1e-9; db += step) {
        const float y = yOf(db);
        ui.line(g.x, y, g.r(), y, std::fabs(db) < 1e-9 ? col::borderHi : col::grid, std::fabs(db) < 1e-9 ? 1.2f : 1.0f);
        ui.text(std::fabs(db) < 1e-9 ? L"0" : fmt(L"%+.0f", db), Rect{g.x + 6, y - 9, 30, 18}, col::textFaint,
                    Font::Small);
    }
    const struct {
        double f;
        const wchar_t* t;
    } labels[] = {{50, L"50"},   {100, L"100"}, {200, L"200"}, {500, L"500"},  {1000, L"1k"},
                  {2000, L"2k"}, {5000, L"5k"}, {10000, L"10k"}, {20000, L"20k"}};
    for (const auto& l : labels) {
        const float x = xOf(l.f);
        if (x > g.r() - 12)
            ui.text(l.t, Rect{g.r() - 46, g.b() - 20, 40, 16}, col::textFaint, Font::Small, Align::Right);
        else
            ui.text(l.t, Rect{x - 20, g.b() - 20, 40, 16}, col::textFaint, Font::Small, Align::Center);
    }

    // ---- spectrum analyzer
    if (analyzerMode_ != 2 && routerStarted_) {
        auto spectrumPts = [&](const std::vector<float>& spec, bool closed) {
            std::vector<D2D1_POINT_2F> pts;
            const float stepX = 3.0f;
            for (float x = g.x; x <= g.r() + 0.1f; x += stepX) {
                const double f0 = fOf(x - stepX * 0.5f), f1 = fOf(x + stepX * 0.5f);
                const float lv = analyzer_.levelAt(spec, f0, f1);
                pts.push_back(D2D1::Point2F(x, std::clamp(specY(lv), g.y, g.b())));
            }
            if (closed) {
                pts.push_back(D2D1::Point2F(g.r(), g.b()));
                pts.push_back(D2D1::Point2F(g.x, g.b()));
            }
            return pts;
        };
        if (analyzerMode_ == 0) {
            auto geo = makePath(ui.factory(), spectrumPts(analyzer_.pre(), false), false);
            if (geo)
                ui.rt()->DrawGeometry(geo.Get(), ui.brush(rgb(0xA9B1C2, 0.45f)), 1.0f);
        }
        auto postPts = spectrumPts(analyzer_.post(), false);
        auto outline = makePath(ui.factory(), postPts, false);
        postPts.push_back(D2D1::Point2F(g.r(), g.b()));
        postPts.push_back(D2D1::Point2F(g.x, g.b()));
        auto post = makePath(ui.factory(), postPts, true);
        if (post) {
            ID2D1LinearGradientBrush* grad = ui.verticalGradient(withAlpha(col::accent, 0.30f),
                                                                 withAlpha(col::accent, 0.03f), g.y + g.h * 0.2f, g.b());
            if (grad)
                ui.rt()->FillGeometry(post.Get(), grad);
        }
        if (outline)
            ui.rt()->DrawGeometry(outline.Get(), ui.brush(withAlpha(col::accent, 0.55f)), 1.0f);
        // Legend.
        float lx = g.r() - 12;
        auto legend = [&](const std::wstring& t, D2D1_COLOR_F c) {
            const float tw = ui.textWidth(t, Font::Small);
            lx -= tw;
            ui.text(t, Rect{lx, g.y + 10, tw + 2, 16}, col::textDim, Font::Small);
            lx -= 18;
            ui.fill(Rect{lx + 4, g.y + 16, 10, 4}, c, 2);
            lx -= 10;
        };
        legend(L"After EQ", col::accent);
        if (analyzerMode_ == 0)
            legend(L"Before EQ", rgb(0xA9B1C2, 0.8f));
        if (!analyzer_.hasSignal())
            ui.text(L"No audio playing — start some music to see its spectrum",
                    Rect{g.x, g.y + g.h * 0.72f, g.w, 20}, col::textFaint, Font::Small, Align::Center);
    }

    // ---- EQ curves
    updateCurves(g);
    const int n = static_cast<int>(curveL_.size());
    auto curvePts = [&](const std::vector<float>& c, double offset) {
        std::vector<D2D1_POINT_2F> pts;
        pts.reserve(n);
        for (int i = 0; i < n; ++i)
            pts.push_back(D2D1::Point2F(std::min(g.x + i * kColStep, g.r()),
                                        std::clamp(yOf(c[i] + offset), g.y - 4, g.b() + 4)));
        return pts;
    };
    const float y0 = yOf(0);
    for (uint32_t b = 0; b < s_.numBands; ++b) {
        const bool selected = static_cast<int>(b) == sel_;
        if (!s_.bands[b].enabled && !selected)
            continue;
        auto pts = curvePts(bandCurves_[b], 0);
        const D2D1_COLOR_F c = bandColor(static_cast<int>(b));
        if (selected) {
            auto area = pts;
            area.push_back(D2D1::Point2F(g.r(), y0));
            area.push_back(D2D1::Point2F(g.x, y0));
            auto geo = makePath(ui.factory(), area, true);
            if (geo)
                ui.rt()->FillGeometry(geo.Get(), ui.brush(withAlpha(c, 0.16f)));
        }
        auto geo = makePath(ui.factory(), pts, false);
        if (geo)
            ui.rt()->DrawGeometry(geo.Get(), ui.brush(withAlpha(c, selected ? 0.9f : 0.35f)), selected ? 1.5f : 1.0f,
                                  s_.bands[b].enabled ? nullptr : ui.dashed());
    }
    const float curveAlpha = bypass_ ? 0.35f : 1.0f;
    if (stereoDiffers_) {
        auto geo = makePath(ui.factory(), curvePts(curveR_, 0), false);
        if (geo)
            ui.rt()->DrawGeometry(geo.Get(), ui.brush(rgb(0xFFB547, 0.8f * curveAlpha)), 1.6f, ui.dashed());
    }
    {
        auto geo = makePath(ui.factory(), curvePts(curveL_, 0), false);
        if (geo) {
            ui.rt()->DrawGeometry(geo.Get(), ui.brush(withAlpha(col::accent, 0.2f * curveAlpha)), 4.5f);
            ui.rt()->DrawGeometry(geo.Get(), ui.brush(rgb(0xF2F5FA, curveAlpha)), 2.2f, ui.roundCap());
        }
    }

    // ---- nodes + interaction
    const Id graphId = makeId("graph");
    // Nodes sit on the band's own curve at its frequency: peaks at their
    // gain, shelves half-way, pass filters at their corner.
    auto nodePos = [&](int b) {
        const Band& band = s_.bands[b];
        const float x = xOf(std::clamp(band.frequencyHz, kFMin, kFMax));
        double db = 0.0;
        if (band.type == FilterType::Peaking)
            db = band.gainDb;
        else if (band.type == FilterType::LowShelf || band.type == FilterType::HighShelf)
            db = band.gainDb * 0.5;
        else if (band.type == FilterType::LowPass || band.type == FilterType::HighPass) {
            Band on = band;
            on.enabled = 1;
            db = eqdsp::biquadMagnitudeDb(eqdsp::designBiquad(on, sampleRate()), band.frequencyHz, sampleRate());
        }
        return D2D1::Point2F(x, std::clamp(yOf(db), g.y + 8, g.b() - 8));
    };
    int hoverNode = -1;
    if (ui.over(g) && (ui.active() == 0 || ui.active() == graphId)) {
        float best = 13.0f;
        for (int b = static_cast<int>(s_.numBands) - 1; b >= 0; --b) {
            const auto p = nodePos(b);
            const float d = std::hypot(p.x - ui.in.mx, p.y - ui.in.my);
            if (d < best) {
                best = d;
                hoverNode = b;
            }
        }
    }
    if (dragNode_ >= 0)
        hoverNode = dragNode_;

    if (ui.over(g) && ui.active() == 0) {
        if (ui.in.doubleClick) {
            if (hoverNode >= 0) {
                if (hasGain(s_.bands[hoverNode].type))
                    s_.bands[hoverNode].gainDb = 0;
            } else {
                const double f = std::clamp(fOf(ui.in.mx), 20.0, 20000.0);
                const double db = std::clamp(std::round((g.cy() - ui.in.my) / halfH * range * 10.0) / 10.0, -range, range);
                addBand(FilterType::Peaking, std::round(f), db, 1.41);
            }
            ui.in.pressed = false;
        } else if (ui.in.pressed && hoverNode >= 0) {
            sel_ = hoverNode;
            dragNode_ = hoverNode;
            dragX_ = ui.in.mx;
            dragY_ = ui.in.my;
            dragF_ = s_.bands[hoverNode].frequencyHz;
            dragG_ = s_.bands[hoverNode].gainDb;
            ui.setActive(graphId);
        } else if (ui.in.rpressed && hoverNode >= 0) {
            deleteBand(hoverNode);
            hoverNode = -1;
        }
        if (hoverNode >= 0 && ui.in.wheel != 0) {
            Band& b = s_.bands[hoverNode];
            b.q = std::clamp(b.q * std::pow(1.12, ui.in.wheel * (ui.in.shift ? 0.25 : 1.0)), 0.1, 24.0);
            sel_ = hoverNode;
            ui.in.wheel = 0;
        }
    }
    if (dragNode_ >= 0 && ui.active() == graphId) {
        if (ui.in.down) {
            const double k = ui.in.shift ? 0.15 : 1.0;
            Band& b = s_.bands[dragNode_];
            const double nf = dragF_ * std::exp(ratio * (ui.in.mx - dragX_) / g.w * k);
            b.frequencyHz = std::clamp(std::round(nf * 10.0) / 10.0, 10.0, std::min(22000.0, sampleRate() * 0.49));
            if (hasGain(b.type)) {
                // Shelf nodes sit at half the gain, so they move at half speed.
                const double perNode = b.type == FilterType::Peaking ? 1.0 : 2.0;
                const double ng = dragG_ - (ui.in.my - dragY_) / halfH * range * k * perNode;
                b.gainDb = std::clamp(std::round(ng * 10.0) / 10.0, -range, range);
            }
            ui.setCursor(IDC_SIZEALL);
        } else {
            dragNode_ = -1;
            ui.setActive(0);
        }
    }

    for (int b = 0; b < static_cast<int>(s_.numBands); ++b) {
        const auto p = nodePos(b);
        const Band& band = s_.bands[b];
        const D2D1_COLOR_F c = bandColor(b);
        const bool selected = b == sel_, hot = b == hoverNode;
        const float rad = selected || hot ? 9.5f : 8.0f;
        if (selected)
            ui.circle(p.x, p.y, rad + 5, withAlpha(c, 0.18f));
        if (band.enabled) {
            ui.circle(p.x, p.y, rad, c);
            ui.text(std::to_wstring(b + 1), Rect{p.x - 10, p.y - 9, 20, 18}, rgb(0x0B1420), Font::Node, Align::Center);
        } else {
            ui.circle(p.x, p.y, rad, col::panel);
            ui.circleStroke(p.x, p.y, rad - 0.75f, withAlpha(c, 0.7f), 1.5f);
            ui.text(std::to_wstring(b + 1), Rect{p.x - 10, p.y - 9, 20, 18}, withAlpha(c, 0.8f), Font::Node,
                    Align::Center);
        }
        if (selected || hot)
            ui.circleStroke(p.x, p.y, rad + 0.5f, col::text, 1.5f);
    }
    if (hoverNode >= 0) {
        const Band& b = s_.bands[hoverNode];
        std::wstring tip = std::to_wstring(hoverNode + 1) + L"  " + kTypeShort[static_cast<int>(b.type)] + L"   " +
                           formatNumber(b.frequencyHz, NumKind::Freq);
        if (hasGain(b.type))
            tip += L"   " + formatNumber(b.gainDb, NumKind::Db);
        tip += L"   Q " + formatNumber(b.q, NumKind::Q);
        if (!b.enabled)
            tip += L"   (off)";
        ui.tooltip(tip);
        if (dragNode_ < 0)
            ui.setCursor(IDC_SIZEALL);
    }

    if (bypass_) {
        const Rect br{g.cx() - 190, g.y + 12, 380, 30};
        ui.fill(br, rgb(0x2C2416, 0.92f), 15);
        ui.stroke(br, col::warn, 15);
        ui.text(L"BYPASSED — you're hearing the original sound", br, col::warn, Font::Bold, Align::Center);
    }
    if (s_.numBands == 0 && !bypass_)
        ui.text(L"Double-click anywhere on the graph to add a band, or pick a preset above",
                Rect{g.x, g.cy() - 34, g.w, 20}, col::textDim, Font::Body, Align::Center);
    ui.text(L"Double-click: add band  ·  Drag: move  ·  Scroll on a band: width (Q)  ·  "
            L"Right-click: remove  ·  Shift: fine",
            Rect{g.x + 40, g.b() - 40, g.w - 80, 16}, col::textFaint, Font::Small, Align::Center);

    ui.rt()->PopAxisAlignedClip();
}

// ================================================================== side column

void App::drawSide(Ui& ui, Rect r)
{
    ui.fill(r, col::panel, 10);
    ui.stroke(r, col::border, 10);

    // Preamp.
    ui.text(L"PREAMP", Rect{r.x, r.y + 10, r.w, 14}, col::textFaint, Font::Caps, Align::Center);
    ui.text(formatNumber(s_.preampDb, NumKind::Db), Rect{r.x, r.y + 26, r.w, 20}, col::text, Font::Bold,
            Align::Center);
    const float sliderH = std::max(90.0f, r.h * 0.42f);
    const Rect slider{r.x + 8, r.y + 50, 48, sliderH};
    double pre = s_.preampDb;
    if (ui.vslider(makeId("preamp"), slider, pre, -30.0, 12.0, 0.0))
        s_.preampDb = pre;
    if (ui.over(slider))
        ui.tooltip(L"Preamp: drag or scroll, double-click for 0 dB");
    // Scale labels next to the slider.
    {
        const float top = slider.y + 8, bottom = slider.b() - 8;
        for (double t : {12.0, 0.0, -12.0, -24.0}) {
            const float ty = static_cast<float>(bottom - (t + 30.0) / 42.0 * (bottom - top));
            ui.text(t == 0 ? L"0" : fmt(L"%+.0f", t), Rect{slider.r() - 4, ty - 8, 30, 16}, col::textFaint,
                    Font::Small);
        }
    }
    const double boost = eqdsp::maxBoostDb(s_, 2, sampleRate());
    const bool clipRisk = boost + s_.preampDb > 0.05;
    if (ui.button(makeId("autogain"), Rect{r.x + 10, slider.b() + 6, r.w - 20, 26}, L"Auto",
                  clipRisk ? Btn::Primary : Btn::Normal))
        s_.preampDb = std::floor(eqdsp::autoPreampDb(s_, 2, sampleRate()) * 10.0) / 10.0;
    if (ui.over(Rect{r.x + 10, slider.b() + 6, r.w - 20, 26}))
        ui.tooltip(clipRisk ? L"The EQ boosts more than the preamp removes: click to prevent clipping"
                            : L"Set the preamp to exactly cancel the largest boost");

    // Output meters.
    const float my = slider.b() + 44;
    ui.text(L"OUTPUT", Rect{r.x, my, r.w, 14}, col::textFaint, Font::Caps, Align::Center);
    const float mTop = my + 22, mBottom = r.b() - 58;
    if (mBottom - mTop > 40) {
        auto mY = [&](double db) {
            return static_cast<float>(mBottom - (std::clamp(db, -60.0, 0.0) + 60.0) / 60.0 * (mBottom - mTop));
        };
        const float bw = 12;
        const float x0 = r.cx() - bw - 3;
        for (int c = 0; c < 2; ++c) {
            const float x = x0 + c * (bw + 6);
            ui.fill(Rect{x, mTop, bw, mBottom - mTop}, col::field, 3);
            const double lv = meterDb_[c];
            if (lv > -60) {
                // Green up to -12 dB, amber to -3 dB, red above.
                const float yl = mY(lv);
                auto seg = [&](double lo, double hi, D2D1_COLOR_F cc) {
                    if (lv <= lo)
                        return;
                    const float ya = mY(std::min(lv, hi)), yb = mY(lo);
                    ui.fill(Rect{x, ya, bw, yb - ya}, cc, 0);
                };
                seg(-60, -12, col::good);
                seg(-12, -3, col::warn);
                seg(-3, 0, col::bad);
                (void)yl;
            }
            if (holdDb_[c] > -60)
                ui.fill(Rect{x, mY(holdDb_[c]) - 1, bw, 2}, col::text, 0);
            ui.text(c == 0 ? L"L" : L"R", Rect{x - 4, mBottom + 2, bw + 8, 14}, col::textFaint, Font::Small,
                    Align::Center);
        }
        for (double t : {0.0, -12.0, -24.0, -36.0, -48.0}) {
            const float ty = mY(t);
            ui.text(fmt(L"%.0f", t), Rect{r.x + 4, ty - 7, x0 - r.x - 8, 14}, col::textFaint, Font::Small, Align::Right);
        }
        const double peak = std::max(holdDb_[0], holdDb_[1]);
        ui.text(peak > -100 ? fmt(L"%.1f", peak) : L"–", Rect{r.x, mBottom + 16, r.w, 14},
                peak > -0.5 ? col::bad : col::textDim, Font::Small, Align::Center);
    }
    // Safety limiter.
    bool lim = limiter_;
    const Rect limR{r.x + 14, r.b() - 30, r.w - 20, 22};
    if (ui.toggleSwitch(makeId("limiter"), limR, lim, limitFlash_ > 0 ? L"LIMIT" : L"Limiter")) {
        limiter_ = lim;
        publish();
    }
    if (limitFlash_ > 0)
        ui.circle(limR.r() - 6, limR.cy(), 4, col::bad);
    if (ui.over(limR))
        ui.tooltip(L"Safety limiter: catches peaks above 0 dBFS so a boost can never clip");
}

// ================================================================== band panel

void App::drawBandPanel(Ui& ui, Rect r)
{
    ui.fill(r, col::panel, 10);
    ui.stroke(r, col::border, 10);

    // Row 1: band chips.
    const float cy = r.y + 12, ch = 28;
    ui.text(L"BANDS", Rect{r.x + 14, cy, 50, ch}, col::textFaint, Font::Caps);
    const int nb = static_cast<int>(s_.numBands);
    float x = r.x + 66;
    const float avail = r.r() - 120 - x;
    const float chipW = std::clamp(avail / std::max(1, nb) - 4, 30.0f, 46.0f);
    for (int b = 0; b < nb; ++b) {
        const Rect chip{x, cy, chipW, ch};
        const Id id = makeId("chip", b);
        const bool hover = ui.over(chip);
        if (hover && ui.in.pressed) {
            sel_ = b;
            ui.in.pressed = false;
        }
        if (hover && ui.in.rpressed) {
            s_.bands[b].enabled = !s_.bands[b].enabled;
        }
        (void)id;
        const D2D1_COLOR_F c = bandColor(b);
        const bool on = s_.bands[b].enabled != 0;
        ui.fill(chip, b == sel_ ? col::panel2 : hover ? col::fieldHover : col::field, 6);
        ui.stroke(chip, b == sel_ ? c : col::border, 6, b == sel_ ? 1.5f : 1.0f);
        if (on)
            ui.circle(chip.x + 11, chip.cy(), 4, c);
        else
            ui.circleStroke(chip.x + 11, chip.cy(), 3.5f, c, 1.2f);
        ui.text(std::to_wstring(b + 1), Rect{chip.x + 16, chip.y, chip.w - 18, ch}, on ? col::text : col::textFaint,
                Font::Bold, Align::Center);
        if (hover)
            ui.tooltip(std::wstring(kTypeShort[static_cast<int>(s_.bands[b].type)]) + L" " +
                       formatNumber(s_.bands[b].frequencyHz, NumKind::Freq) + L"  ·  right-click: on/off");
        x += chipW + 4;
    }
    const Rect addR{x + 2, cy, 64, ch};
    if (ui.button(makeId("addband"), addR, L"Add", Btn::Ghost, nb < eqdsp::kMaxBands, L"")) {
        // Next band goes an octave and a half above the highest one (or 1 kHz).
        double f = 1000.0;
        if (nb > 0) {
            double hi = 0;
            for (int b = 0; b < nb; ++b)
                hi = std::max(hi, s_.bands[b].frequencyHz);
            f = hi * 2.8 < 16000 ? hi * 2.8 : 1000.0;
        }
        addBand(FilterType::Peaking, std::round(f), 0.0, 1.41);
    }
    const std::wstring count = std::to_wstring(nb) + L" / " + std::to_wstring(eqdsp::kMaxBands);
    ui.text(count, Rect{r.r() - 110, cy, 96, ch}, col::textFaint, Font::Small, Align::Right);

    ui.line(r.x + 12, r.y + 50, r.r() - 12, r.y + 50, col::border);

    // Row 2: editor for the selected band.
    const float ly = r.y + 58, fy = r.y + 76, fh = 32;
    if (sel_ < 0 || sel_ >= nb) {
        ui.text(nb == 0 ? L"No bands yet. Double-click the graph or press Add."
                        : L"Select a band above or click its node on the graph to edit it.",
                Rect{r.x + 16, fy, r.w - 32, fh}, col::textDim, Font::Body);
        return;
    }
    Band& b = s_.bands[sel_];
    const D2D1_COLOR_F c = bandColor(sel_);
    float ex = r.x + 16;
    ui.circle(ex + 12, fy + fh / 2, 12, c);
    ui.text(std::to_wstring(sel_ + 1), Rect{ex, fy, 24, fh}, rgb(0x0B1420), Font::Bold, Align::Center);
    ex += 36;

    bool on = b.enabled != 0;
    ui.text(L"ON", Rect{ex, ly, 50, 14}, col::textFaint, Font::Caps);
    if (ui.toggleSwitch(makeId("bandon"), Rect{ex, fy, 40, fh}, on, L""))
        b.enabled = on ? 1u : 0u;
    ex += 54;

    auto caption = [&](const wchar_t* t, float w) { ui.text(t, Rect{ex, ly, w, 14}, col::textFaint, Font::Caps); };
    caption(L"TYPE", 150);
    int type = static_cast<int>(b.type);
    if (ui.combo(makeId("type"), Rect{ex, fy, 150, fh}, kTypeNames, type)) {
        b.type = static_cast<FilterType>(type);
        if (!hasGain(b.type))
            b.gainDb = 0;
        if (b.type == FilterType::LowShelf || b.type == FilterType::HighShelf || b.type == FilterType::LowPass ||
            b.type == FilterType::HighPass)
            b.q = 0.71;
    }
    ex += 162;

    caption(L"FREQUENCY", 130);
    NumberSpec fspec{NumKind::Freq, 10.0, std::min(22000.0, sampleRate() * 0.49), true, 1.0 / 40.0, 1.0 / 12.0};
    double f = b.frequencyHz;
    if (ui.numberField(makeId("freq"), Rect{ex, fy, 130, fh}, f, fspec))
        b.frequencyHz = std::round(f * 10.0) / 10.0;
    ex += 142;

    caption(L"GAIN", 116);
    NumberSpec gspec{NumKind::Db, -24.0, 24.0, false, 0.1, 0.5};
    double gdb = b.gainDb;
    if (ui.numberField(makeId("gain"), Rect{ex, fy, 116, fh}, gdb, gspec, hasGain(b.type)))
        b.gainDb = std::round(gdb * 10.0) / 10.0;
    ex += 128;

    caption(L"Q", 100);
    NumberSpec qspec{NumKind::Q, 0.1, 24.0, true, 1.0 / 60.0, 1.0 / 8.0};
    double q = b.q;
    if (ui.numberField(makeId("q"), Rect{ex, fy, 100, fh}, q, qspec))
        b.q = std::round(q * 1000.0) / 1000.0;
    ex += 112;

    caption(L"CHANNELS", 130);
    static const std::vector<std::wstring> chans = {L"Both", L"Left only", L"Right only"};
    int cs = (b.channelMask & 3u) == 3u ? 0 : (b.channelMask & 1u) ? 1 : 2;
    if (ui.combo(makeId("chans"), Rect{ex, fy, 130, fh}, chans, cs))
        b.channelMask = cs == 0 ? eqdsp::kAllChannels : cs == 1 ? 1u : 2u;
    ex += 142;

    const Rect delR{r.r() - 16 - 104, fy, 104, fh};
    if (delR.x > ex) {
        if (ui.button(makeId("delband"), delR, L"Remove", Btn::Danger, true, L""))
            deleteBand(sel_);
    }
}

// ================================================================== status bar

void App::drawStatusBar(Ui& ui, Rect r)
{
    ui.fill(r, col::panel);
    ui.line(r.x, r.y + 0.5f, r.r(), r.y + 0.5f, col::border);
    auto& st = router_.stats();
    D2D1_COLOR_F dot = col::bad;
    std::wstring state;
    if (routerStarted_ && st.running.load()) {
        dot = col::good;
        state = L"Running";
    } else if (routerStarted_) {
        dot = col::warn;
        state = L"Starting…";
    } else if (!route_) {
        dot = col::textFaint;
        state = L"Preview (audio routing off)";
    } else {
        state = L"Stopped";
    }
    ui.circle(r.x + 18, r.cy(), 4.5f, dot);
    std::wstring route = state;
    if (inputSel_ >= 0 && outputSel_ >= 0)
        route += L"   ·   " + inputs_[inputSel_].name + L"  →  EQ  →  " + outputs_[outputSel_].name;
    if (routerStarted_ && st.renderRate.load() > 0)
        route += fmt(L"   ·   %.1f kHz", st.renderRate.load() / 1000.0);
    ui.text(route, Rect{r.x + 30, r.y, r.w * 0.6f, r.h}, col::textDim, Font::Small);

    if (routerStarted_) {
        wchar_t b[200];
        swprintf(b, 200, L"Latency %.0f ms     Drift %+.1f ppm     CPU %.2f%%     Dropouts %llu", st.latencyMs.load(),
                 st.ppm.load(), st.cpuPercent.load(), st.underruns.load());
        ui.text(b, Rect{r.r() - 420, r.y, 404, r.h}, col::textDim, Font::Small, Align::Right);
    }
}

} // namespace gui
