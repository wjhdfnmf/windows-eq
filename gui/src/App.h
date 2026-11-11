#pragma once

#include "Analyzer.h"
#include "Ui.h"

#include "AudioDevices.h"
#include "Router.h"
#include "eqdsp/Equalizer.h"
#include "eqdsp/Types.h"

#include <string>
#include <vector>

namespace gui {

class App {
public:
    // route = false: preview only (screenshots); no audio devices are opened.
    bool init(HWND hwnd, bool route = true);
    void shutdown();
    // One UI frame: logic, layout and drawing (inside BeginDraw/EndDraw).
    void frame(Ui& ui, double dtSec);
    // Modal work (dialogs, message boxes) requested during frame(); runs
    // after EndDraw so a nested paint cannot re-enter the frame.
    void runDeferred();
    // How often to redraw when the user isn't interacting: fast enough for
    // the analyzer while audio plays, slow when idle.
    double idleFrameInterval(bool foreground) const;
    // Routing, autosave and file watching; called by frame(), and on a timer
    // while the window is hidden in the tray (visible = false skips the FFT).
    void tick(double dtSec, bool visible);

    // ---- for the tray icon and menus
    bool bypassed() const { return bypass_; }
    void setBypassed(bool b);
    std::wstring trayTip() const;
    unsigned stateKey() const; // changes whenever the tray icon/tip should update
    bool audioStopsOnQuit() const { return defaultIsCable_; }
    // Diagnostics: is audio reaching the analyzer, is routing running?
    bool audioActive() const { return analyzer_.hasSignal() || meterDb_[0] > -90 || meterDb_[1] > -90; }
    bool routingRunning() const { return routerStarted_ && const_cast<weq::Router&>(router_).stats().running.load(); }
    // Shows the tray/settings menu at a screen point and handles what it
    // can; returns kMenuOpen / kMenuQuit for the host, else 0.
    static constexpr int kMenuOpen = 1, kMenuQuit = 2;
    // Smallest client area (DIPs) the layout supports; smaller frames draw nothing.
    static constexpr float kMinLayoutW = 640.0f, kMinLayoutH = 420.0f;
    // Posted to the main window: quit, asking first if audio would stop.
    static constexpr UINT kMsgQuitAsk = WM_APP + 22;
    // Posted to the main window: the renderer setting changed.
    static constexpr UINT kMsgRendererChanged = WM_APP + 23;
    int showMenu(POINT screenPt, bool fromTray);

    // ---- benchmarks / previews
    // Feeds the analyzer synthetic music-like noise (run through the current
    // EQ for the "after" trace) instead of the router's audio.
    void setDemoSignal(bool on) { demoSignal_ = on; }
    void setAnalyzerMode(int mode) { analyzerMode_ = mode; }
    // Benchmark: skip drawing sections (bit 0 graph, 1 side, 2 bands, 3 bars).
    void setBenchSkip(unsigned mask) { benchSkip_ = mask; }
    unsigned long long settingsChanges() const { return settingsChanges_; }
    unsigned long long curveRebuilds() const { return curveRebuilds_; }

private:
    enum class Deferred { None, Import, Export, DeletePreset, ConfirmOverwrite, OpenSoundSettings, SettingsMenu };
    POINT settingsMenuPt_{};

    // ---- routing
    void refreshDevices(bool force);
    void startRouter();
    void stopRouter();
    void pollRouter(double dt);
    void pollAnalysis(double dt, bool visible);
    double sampleRate() const;

    // ---- EQ model
    void publish();
    void onSettingsChanged();
    void undo();
    void redo();
    void saveCurrentNow();
    void pollExternalEdits(double dt);
    void addBand(eqdsp::FilterType type, double f, double g, double q);
    void deleteBand(int index);

    // ---- presets
    void rebuildPresetList();
    void applyPreset(int comboIndex);
    void saveUserPreset(const std::wstring& name);
    void syncPresetSelection();

    // ---- drawing
    void drawTopBar(Ui& ui, Rect r);
    void drawToolbar(Ui& ui, Rect r);
    void drawBanner(Ui& ui, Rect r);
    void drawGraph(Ui& ui, Rect r);
    void drawSide(Ui& ui, Rect r);
    void drawBandPanel(Ui& ui, Rect r);
    void drawStatusBar(Ui& ui, Rect r);
    std::wstring bannerText() const;
    void updateCurves(Rect g);

    HWND hwnd_ = nullptr;

    // demo signal (benchmarks, previews)
    bool demoSignal_ = false;
    unsigned benchSkip_ = 0;
    unsigned long long settingsChanges_ = 0, curveRebuilds_ = 0;
    eqdsp::Equalizer demoEq_;
    bool demoEqReady_ = false;
    uint32_t demoRng_ = 12345;
    float demoLp_[2] = {0, 0};
    double demoPhase_ = 0;
    void feedDemoSignal(double dt);

    // routing
    bool route_ = true;
    weq::Router router_;
    bool routerStarted_ = false;
    double retryIn_ = 0;
    std::wstring routerError_;
    std::vector<weq::AudioDevice> outputs_, inputs_;
    std::vector<std::wstring> outputNames_, inputNames_;
    std::wstring outputId_, inputId_;
    int outputSel_ = -1, inputSel_ = -1;
    std::wstring defaultOutputName_;
    bool defaultIsCable_ = true, cableInstalled_ = true;
    double devicePollIn_ = 0;

    // EQ model
    eqdsp::EqSettings s_{};
    bool bypass_ = false;
    bool limiter_ = true;
    int sel_ = -1;
    eqdsp::EqSettings frameStart_{}, gestureBase_{};
    bool gesture_ = false;
    double lastLooseChange_ = -10;
    std::vector<eqdsp::EqSettings> undo_, redo_;
    double saveIn_ = -1;
    unsigned long long ownStamp_ = 0;
    double externalPollIn_ = 0;

    // presets
    std::vector<std::wstring> userPresets_;
    std::vector<std::wstring> presetItems_;
    int presetSel_ = -1;
    std::wstring presetName_ = L"Flat";
    bool modified_ = false;
    bool saveAsOpen_ = false;
    std::wstring saveAsName_;
    std::wstring pendingName_;
    Deferred deferred_ = Deferred::None;
    bool inDeferred_ = false;

    // view
    int analyzerMode_ = 0; // 0 pre+post, 1 post, 2 off
    int rangeSel_ = 1;     // index into kRanges
    double rangeDb() const;
    double cachedMaxBoost();
    eqdsp::EqSettings boostKey_{};
    double boostFs_ = 0, boostDb_ = 0;
    Analyzer analyzer_;
    std::vector<float> analysisBuf_;

    // graph interaction
    int dragNode_ = -1;
    float dragX_ = 0, dragY_ = 0;
    double dragF_ = 0, dragG_ = 0;

    // curve cache (dB per sample column)
    eqdsp::EqSettings curveKey_{};
    double curveFs_ = 0;
    float curveX_ = -1, curveW_ = -1;
    std::vector<float> curveL_, curveR_;
    std::vector<std::vector<float>> bandCurves_;
    bool stereoDiffers_ = false;
    // Cached curve paths, rebuilt when the curves above or the graph's
    // geometry change (fills are built lazily for the selected band).
    struct CurveGeo {
        ComPtr<ID2D1PathGeometry> line, fill;
    };
    std::vector<CurveGeo> bandGeo_;
    ComPtr<ID2D1PathGeometry> geoL_, geoR_;
    bool geoValid_ = false;
    float geoY_ = -1, geoH_ = -1;
    double geoRange_ = 0;

    // meters
    float meterDb_[2] = {-120, -120}, holdDb_[2] = {-120, -120};
    double holdAge_[2] = {0, 0};
    double limitFlash_ = 0;
};

} // namespace gui
