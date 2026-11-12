// WindowsEQ.exe - window host: DPI, Direct2D target, input, frame timing,
// tray icon, and the command-line modes (--tray, --uninstall, ...).

#include "App.h"
#include "CrashHandler.h"
#include "Storage.h"
#include "Ui.h"

#include "weq/Install.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wtsapi32.h>
#include <pdh.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gui;
namespace inst = weq::install;

namespace {

const UINT kMsgTray = WM_APP + 1;
const UINT kTimerId = 1;

struct Host {
    HWND hwnd = nullptr;
    ComPtr<ID2D1Factory1> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Bitmap1> target;
    // Default renderer: the proven Direct2D window target. The flip-model
    // renderer above (less GPU) is opt-in while a system crash seen during
    // its testing is unexplained (config "renderer=flip").
    bool useFlip = false;
    ComPtr<ID2D1HwndRenderTarget> hwndRt;
    Ui ui;
    App app;
    float dpi = 96;
    LARGE_INTEGER lastFrame{}, lastTick{}, qpf{};
    bool appReady = false;
    double interactUntil = 0; // seconds (QPC) until which to run at full rate
    bool showPerf = false;    // F12: frame timing overlay
    double perfFrameMs = 0, perfDrawMs = 0, perfFps = 0;
    double perfLastFrameMs = 0, perfLastDrawMs = 0;
    // Diagnostics query (registered message "WindowsEQ.Diagnostics"):
    // wParam 0 -> frames drawn so far; 1 -> flags (1 audio active,
    // 2 routing running, 4 window counts as seen).
    UINT msgDiagnostics = 0;
    LONG_PTR framesDrawn = 0;

    // tray
    NOTIFYICONDATAW nid{};
    bool trayAdded = false;
    unsigned trayKey = 0;
    HICON trayOn = nullptr, trayOff = nullptr;
    UINT taskbarCreated = 0;
    bool wasMaximized = false;
    bool hiddenMode = false;

    // Visibility: nothing is drawn while nobody can see the window.
    bool presentImmediately = false; // benchmarks: don't wait for vsync
    bool sessionLocked = false, displayOff = false;
    bool covered = false;
    double nextCoverCheck = 0;
    HPOWERNOTIFY displayNotify = nullptr;
} g;

// {6FE69556-704A-47A0-8F24-C28D936FDA47}: GUID_CONSOLE_DISPLAY_STATE
const GUID kDisplayStateGuid = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

bool isCloaked(HWND w)
{
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

// Can this window be seen through? Click-through windows: yes. Layered
// windows: only if they use per-pixel alpha (UpdateLayeredWindow) or a
// colour key or constant alpha below 255. Chromium/Electron apps (browsers,
// the Claude app, VS Code...) are layered but fully opaque, and must count.
bool seeThrough(HWND w)
{
    const LONG ex = GetWindowLongW(w, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT)
        return true;
    if (!(ex & WS_EX_LAYERED))
        return false;
    COLORREF key = 0;
    BYTE alpha = 0;
    DWORD flags = 0;
    if (!GetLayeredWindowAttributes(w, &key, &alpha, &flags))
        return true; // per-pixel alpha
    if (flags & LWA_COLORKEY)
        return true;
    return (flags & LWA_ALPHA) && alpha < 255;
}

// True when the client area is completely hidden behind other opaque
// top-level windows (or off every screen).
bool coveredByOtherWindows(HWND self)
{
    RECT rc;
    GetClientRect(self, &rc);
    MapWindowPoints(self, nullptr, reinterpret_cast<POINT*>(&rc), 2);
    const RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                      GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                      GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    RECT onScreen;
    if (!IntersectRect(&onScreen, &rc, &screen))
        return true;
    HRGN visible = CreateRectRgnIndirect(&onScreen);
    int kind = SIMPLEREGION;
    for (HWND w = GetWindow(self, GW_HWNDPREV); w && kind != NULLREGION; w = GetWindow(w, GW_HWNDPREV)) {
        if (!IsWindowVisible(w) || IsIconic(w) || isCloaked(w))
            continue;
        if (seeThrough(w))
            continue;
        RECT r;
        if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(w, &r))
            continue;
        if (IsRectEmpty(&r))
            continue;
        HRGN other = CreateRectRgnIndirect(&r);
        kind = CombineRgn(visible, visible, other, RGN_DIFF);
        DeleteObject(other);
    }
    DeleteObject(visible);
    return kind == NULLREGION;
}

// Can anyone see the window right now?
bool windowSeen(HWND hwnd)
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || g.sessionLocked || g.displayOff || isCloaked(hwnd))
        return false;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const double now = static_cast<double>(c.QuadPart) / g.qpf.QuadPart;
    if (now >= g.nextCoverCheck) {
        g.covered = coveredByOtherWindows(hwnd);
        g.nextCoverCheck = now + 0.25;
    }
    return !g.covered;
}

double nowSec()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / g.qpf.QuadPart;
}

// Input arrived: redraw now and stay at full frame rate for a moment.
void poke()
{
    g.interactUntil = nowSec() + 0.5;
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

float toDip(int px) { return px * 96.0f / g.dpi; }

// ------------------------------------------------------------------ icon

// Straight-alpha BGRA, top-down: a rounded square with three EQ bars.
std::vector<uint32_t> iconPixels(int size, bool bypassed)
{
    std::vector<uint32_t> px(static_cast<size_t>(size) * size);
    const float s = static_cast<float>(size);
    const float radius = s * 0.22f;
    const float barW = std::max(1.0f, s * 0.13f), gap = s * 0.09f;
    const float heights[3] = {0.38f, 0.66f, 0.5f};
    const float x0 = (s - (3 * barW + 2 * gap)) / 2;
    const uint32_t bg = bypassed ? 0xFFB547u : 0x4DA3FFu;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float dx = std::max({radius - fx, 0.0f, fx - (s - radius)});
            const float dy = std::max({radius - fy, 0.0f, fy - (s - radius)});
            const float d = std::sqrt(dx * dx + dy * dy) - radius;
            const float a = std::clamp(0.5f - d, 0.0f, 1.0f);
            uint32_t rgbv = bg;
            for (int i = 0; i < 3; ++i) {
                const float bx = x0 + i * (barW + gap);
                const float top = s * 0.78f - s * heights[i];
                if (fx >= bx && fx < bx + barW && fy >= top && fy < s * 0.78f)
                    rgbv = 0x0B1420u;
            }
            const uint32_t A = static_cast<uint32_t>(a * 255.0f + 0.5f);
            px[static_cast<size_t>(y) * size + x] = (A << 24) | rgbv;
        }
    }
    return px;
}

HICON createIcon(int size, bool bypassed)
{
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color)
        return LoadIconW(nullptr, IDI_APPLICATION);
    const auto px = iconPixels(size, bypassed);
    std::memcpy(bits, px.data(), px.size() * 4);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

// Writes a multi-size .ico (used once to produce gui/res/app.ico).
int writeIco(const std::wstring& path)
{
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 256};
    std::vector<std::vector<uint8_t>> images;
    for (int s : sizes) {
        std::vector<uint8_t> img(sizeof(BITMAPINFOHEADER));
        auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(img.data());
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = s;
        bih->biHeight = s * 2;
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        const auto px = iconPixels(s, false);
        for (int y = s - 1; y >= 0; --y) // bottom-up
            for (int x = 0; x < s; ++x) {
                const uint32_t v = px[static_cast<size_t>(y) * s + x];
                img.push_back(static_cast<uint8_t>(v & 0xFF));
                img.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                img.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
                img.push_back(static_cast<uint8_t>(v >> 24));
            }
        img.resize(img.size() + static_cast<size_t>((s + 31) / 32 * 4) * s, 0); // AND mask
        images.push_back(std::move(img));
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return 2;
    const uint16_t header[3] = {0, 1, static_cast<uint16_t>(images.size())};
    fwrite(header, 2, 3, f);
    uint32_t offset = 6 + 16 * static_cast<uint32_t>(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        uint8_t e[16] = {};
        e[0] = static_cast<uint8_t>(sizes[i] >= 256 ? 0 : sizes[i]);
        e[1] = e[0];
        e[4] = 1;  // planes
        e[6] = 32; // bpp
        const uint32_t len = static_cast<uint32_t>(images[i].size());
        std::memcpy(e + 8, &len, 4);
        std::memcpy(e + 12, &offset, 4);
        fwrite(e, 1, 16, f);
        offset += len;
    }
    for (const auto& img : images)
        fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    return 0;
}

// ------------------------------------------------------------------ tray

void trayUpdate(bool force)
{
    if (!g.trayAdded || !g.appReady)
        return;
    const unsigned key = g.app.stateKey();
    if (!force && key == g.trayKey)
        return;
    g.trayKey = key;
    g.nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
    g.nid.hIcon = g.app.bypassed() ? g.trayOff : g.trayOn;
    wcsncpy_s(g.nid.szTip, g.app.trayTip().c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

void trayAdd()
{
    const int sz = GetSystemMetricsForDpi(SM_CXSMICON, static_cast<UINT>(g.dpi));
    if (!g.trayOn) {
        g.trayOn = createIcon(sz, false);
        g.trayOff = createIcon(sz, true);
    }
    g.nid = NOTIFYICONDATAW{};
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g.nid.uCallbackMessage = kMsgTray;
    g.nid.hIcon = g.trayOn;
    wcsncpy_s(g.nid.szTip, L"Windows EQ", _TRUNCATE);
    g.trayAdded = Shell_NotifyIconW(NIM_ADD, &g.nid) != FALSE;
    g.nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
    trayUpdate(true);
}

void trayRemove()
{
    if (g.trayAdded)
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
    g.trayAdded = false;
}

void trayBalloon(const wchar_t* title, const wchar_t* text)
{
    if (!g.trayAdded)
        return;
    NOTIFYICONDATAW n = g.nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(n.szInfo, text, _TRUNCATE);
    n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    n.hBalloonIcon = g.trayOn;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ------------------------------------------------------------------ rendering

// Two renderers:
//  - default: ID2D1HwndRenderTarget, the proven Direct2D window target;
//  - opt-in "low-GPU" (config renderer=flip): Direct3D 11 + DXGI flip-model
//    swap chain + Direct2D 1.1 device context. The compositor uses its back
//    buffer directly, without a full-window copy per frame (less than half
//    the GPU in measurements). A system crash (DPC watchdog, inconclusive
//    dump) happened while it was being stress-tested, so it isn't the
//    default until that's understood.

// config.ini "renderer=flip" opts in; WEQ_RENDERER=flip|legacy overrides (benchmarks).
bool flipRendererConfigured()
{
    wchar_t env[16];
    if (GetEnvironmentVariableW(L"WEQ_RENDERER", env, 16) > 0)
        return _wcsicmp(env, L"flip") == 0;
    return configGet(L"renderer", L"legacy") == L"flip";
}

ID2D1RenderTarget* activeTarget()
{
    return g.useFlip ? static_cast<ID2D1RenderTarget*>(g.dc.Get()) : static_cast<ID2D1RenderTarget*>(g.hwndRt.Get());
}

void createLegacyTarget()
{
    if (g.hwndRt)
        return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(1, rc.right - rc.left), std::max<LONG>(1, rc.bottom - rc.top));
    if (SUCCEEDED(g.d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), g.dpi, g.dpi),
            D2D1::HwndRenderTargetProperties(g.hwnd, size,
                                             g.presentImmediately ? D2D1_PRESENT_OPTIONS_IMMEDIATELY
                                                                  : D2D1_PRESENT_OPTIONS_NONE),
            &g.hwndRt))) {
        g.hwndRt->SetDpi(g.dpi, g.dpi);
        g.ui.setTarget(g.hwndRt.Get());
    }
}

// Wraps the swap chain's current back buffer as the device context's target.
bool bindBackBuffer()
{
    ComPtr<IDXGISurface> surface;
    if (FAILED(g.swap->GetBuffer(0, IID_PPV_ARGS(&surface))))
        return false;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), g.dpi, g.dpi);
    if (FAILED(g.dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &g.target)))
        return false;
    g.dc->SetTarget(g.target.Get());
    g.dc->SetDpi(g.dpi, g.dpi);
    return true;
}

void createFlipTarget()
{
    if (g.dc)
        return;
    if (!g.d2dDevice) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                       static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &g.d3d, nullptr,
                                       nullptr);
        if (FAILED(hr)) // no usable GPU (remote desktop, broken driver): software rendering
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &g.d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(hr) || FAILED(g.d3d.As(&dxgi)) || FAILED(g.d2d->CreateDevice(dxgi.Get(), &g.d2dDevice)))
            return;
    }
    if (FAILED(g.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.dc)))
        return;
    if (!g.swap) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(g.d3d.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            g.dc.Reset();
            return;
        }
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
        sd.Height = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_NONE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(factory->CreateSwapChainForHwnd(g.d3d.Get(), g.hwnd, &sd, nullptr, nullptr, &g.swap))) {
            g.dc.Reset();
            return;
        }
        factory->MakeWindowAssociation(g.hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    if (!bindBackBuffer()) {
        g.dc.Reset();
        return;
    }
    g.ui.setTarget(g.dc.Get());
}

void createTarget()
{
    if (g.useFlip)
        createFlipTarget();
    else
        createLegacyTarget();
}

// Drops the render target (and with `everything`, the swap chain and the
// GPU device too); the next frame recreates what is missing.
void discardTarget(bool everything = false)
{
    g.ui.releaseTarget();
    g.hwndRt.Reset();
    if (g.dc)
        g.dc->SetTarget(nullptr);
    g.target.Reset();
    g.dc.Reset();
    if (everything) {
        g.swap.Reset();
        g.d2dDevice.Reset();
        g.d3d.Reset();
    }
}

void resizeTarget()
{
    if (g.hwndRt) {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        g.hwndRt->Resize(D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left), static_cast<UINT32>(rc.bottom - rc.top)));
        return;
    }
    if (!g.swap)
        return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0)
        return; // minimized: keep the old buffers
    if (g.dc)
        g.dc->SetTarget(nullptr);
    g.target.Reset();
    if (FAILED(g.swap->ResizeBuffers(0, static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top),
                                     DXGI_FORMAT_UNKNOWN, 0))) {
        discardTarget(true);
        return;
    }
    if (g.dc && !bindBackBuffer())
        discardTarget();
}

void render()
{
    createTarget();
    ID2D1RenderTarget* rt = activeTarget();
    if (!rt)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = g.lastFrame.QuadPart ? static_cast<double>(now.QuadPart - g.lastFrame.QuadPart) / g.qpf.QuadPart : 0.016;
    g.lastFrame = now;
    g.lastTick = now;
    dt = std::clamp(dt, 0.001, 0.25);

    g.ui.in.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    g.ui.in.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    const D2D1_SIZE_F size = rt->GetSize();
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g.ui.beginFrame(size.width, size.height, dt);
    bool frameFailed = false;
    if (g.appReady) {
        // Never let a UI bug take audio routing down with it: log and carry on.
        try {
            g.app.frame(g.ui, dt);
        } catch (const std::exception& e) {
            logError(L"frame", e.what());
            frameFailed = true;
        } catch (...) {
            logError(L"frame", "unknown exception");
            frameFailed = true;
        }
    }
    if (g.showPerf) {
        wchar_t b[128];
        swprintf(b, 128, L"%.0f fps   frame %.2f ms   draw %.2f ms", g.perfFps, g.perfFrameMs, g.perfDrawMs);
        const Rect r{size.width / 2 - 140, 4, 280, 20};
        g.ui.fill(r, rgb(0x000000, 0.75f), 4);
        g.ui.text(b, r, col::good, Font::Small, Align::Center);
    }
    g.ui.endFrame();
    LARGE_INTEGER mid;
    QueryPerformanceCounter(&mid);
    HRESULT hr = rt->EndDraw();
    if (SUCCEEDED(hr))
        ++g.framesDrawn;
    if (SUCCEEDED(hr) && g.useFlip) { // the window target presents in EndDraw
        // Vsync (interval 1) normally; benchmarks present immediately.
        const HRESULT pr = g.swap->Present(g.presentImmediately ? 0 : 1, 0);
        if (pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET)
            hr = D2DERR_RECREATE_TARGET;
    }
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    const double frameMs = static_cast<double>(mid.QuadPart - now.QuadPart) * 1000.0 / g.qpf.QuadPart;
    const double drawMs = static_cast<double>(end.QuadPart - mid.QuadPart) * 1000.0 / g.qpf.QuadPart;
    g.perfLastFrameMs = frameMs;
    g.perfLastDrawMs = drawMs;
    g.perfFrameMs += (frameMs - g.perfFrameMs) * 0.1;
    g.perfDrawMs += (drawMs - g.perfDrawMs) * 0.1;
    g.perfFps += (1.0 / dt - g.perfFps) * 0.1;
    // Lost GPU device: recreate everything. A frame that threw may have left
    // clips pushed: start from a fresh device context.
    if (hr == D2DERR_RECREATE_TARGET)
        discardTarget(true);
    else if (frameFailed || FAILED(hr))
        discardTarget();
    SetCursor(LoadCursorW(nullptr, g.ui.cursor()));
    trayUpdate(false);
}

// Runs fn, logging (instead of crashing on) any C++ exception.
template <class Fn>
void guarded(const wchar_t* where, Fn&& fn)
{
    try {
        fn();
    } catch (const std::exception& e) {
        logError(where, e.what());
    } catch (...) {
        logError(where, "unknown exception");
    }
}

// ------------------------------------------------------------------ window state

void saveWindowPlacement()
{
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g.hwnd, &wp))
        return;
    const bool maxed = IsWindowVisible(g.hwnd) ? wp.showCmd == SW_SHOWMAXIMIZED : g.wasMaximized;
    wchar_t b[128];
    swprintf(b, 128, L"%ld %ld %ld %ld %u", wp.rcNormalPosition.left, wp.rcNormalPosition.top,
             wp.rcNormalPosition.right, wp.rcNormalPosition.bottom, maxed ? 1u : 0u);
    configSet(L"window", b);
}

// Positions the (still hidden) window from the saved placement.
void restoreWindowPlacement()
{
    const std::wstring s = configGet(L"window");
    long l, t, r, bt;
    unsigned maxed;
    if (swscanf_s(s.c_str(), L"%ld %ld %ld %ld %u", &l, &t, &r, &bt, &maxed) != 5 || r - l < 200 || bt - t < 200)
        return;
    RECT rc{l, t, r, bt};
    if (!MonitorFromRect(&rc, MONITOR_DEFAULTTONULL))
        return; // that monitor is gone
    WINDOWPLACEMENT wp{sizeof(wp)};
    wp.rcNormalPosition = rc;
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(g.hwnd, &wp);
    g.wasMaximized = maxed != 0;
}

void setHiddenMode(bool hidden)
{
    g.hiddenMode = hidden;
    // Hidden: tick routing 4x per second; visible: 15 ms frame clock.
    SetTimer(g.hwnd, kTimerId, hidden ? 250 : 15, nullptr);
}

void showMainWindow()
{
    if (!IsWindowVisible(g.hwnd))
        ShowWindow(g.hwnd, g.wasMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    else if (IsIconic(g.hwnd))
        ShowWindow(g.hwnd, SW_RESTORE);
    SetForegroundWindow(g.hwnd);
    setHiddenMode(false);
    g.lastFrame.QuadPart = 0;
    poke();
}

void hideToTray()
{
    g.wasMaximized = IsZoomed(g.hwnd) != FALSE;
    saveWindowPlacement();
    ShowWindow(g.hwnd, SW_HIDE);
    setHiddenMode(true);
    if (configGet(L"trayHintShown", L"0") != L"1") {
        trayBalloon(L"Windows EQ is still running",
                    L"The EQ stays on. Click the tray icon to open it; right-click it to quit.");
        configSet(L"trayHintShown", L"1");
    }
}

void quitApp(bool ask)
{
    if (ask && g.appReady && g.app.audioStopsOnQuit()) {
        const int r = MessageBoxW(
            IsWindowVisible(g.hwnd) ? g.hwnd : nullptr,
            L"Windows is sending its audio to \"CABLE Input\", so you won't hear anything until Windows EQ runs "
            L"again or you choose another output in Sound settings.\n\nQuit anyway?",
            L"Quit Windows EQ", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (r != IDYES)
            return;
    }
    if (IsWindowVisible(g.hwnd))
        saveWindowPlacement();
    DestroyWindow(g.hwnd);
}

// ------------------------------------------------------------------ window proc

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Input& in = g.ui.in;
    if (msg == g.msgDiagnostics && g.msgDiagnostics) {
        if (wp == 0)
            return g.framesDrawn;
        LRESULT flags = 0;
        if (g.appReady && g.app.audioActive())
            flags |= 1;
        if (g.appReady && g.app.routingRunning())
            flags |= 2;
        if (!g.hiddenMode)
            flags |= 4;
        if (GetForegroundWindow() == hwnd)
            flags |= 8;
        return flags;
    }
    if (msg == g.taskbarCreated && g.taskbarCreated) { // Explorer restarted
        g.trayAdded = false;
        trayAdd();
        return 0;
    }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        // Minimized windows have a 0x0 client area: nothing to draw.
        if (!IsIconic(hwnd))
            render();
        if (g.appReady)
            guarded(L"deferred", [] { g.app.runDeferred(); });
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER: {
        if (!windowSeen(hwnd)) {
            // Hidden, minimized, covered, on another desktop, screen locked or
            // display off: keep routing alive without drawing anything.
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double dt = g.lastTick.QuadPart
                                  ? static_cast<double>(now.QuadPart - g.lastTick.QuadPart) / g.qpf.QuadPart
                                  : 0.25;
            g.lastTick = now;
            if (g.appReady) {
                guarded(L"tick", [dt] { g.app.tick(std::clamp(dt, 0.0, 1.0), false); });
                trayUpdate(false);
            }
            if (!g.hiddenMode)
                setHiddenMode(true);
            return 0;
        }
        if (g.hiddenMode) {
            // Seen again: back to the frame clock and redraw right away.
            setHiddenMode(false);
            g.lastFrame.QuadPart = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        const double now = nowSec();
        const double last = g.lastFrame.QuadPart ? static_cast<double>(g.lastFrame.QuadPart) / g.qpf.QuadPart : 0;
        const bool interacting = now < g.interactUntil || g.ui.in.down || g.ui.editing();
        const bool foreground = GetForegroundWindow() == hwnd;
        const double interval = interacting ? 0.0 : (g.appReady ? g.app.idleFrameInterval(foreground) : 0.1);
        if (now - last >= interval - 0.004)
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SIZE:
        resizeTarget();
        if (wp != SIZE_MINIMIZED && g.hiddenMode && IsWindowVisible(hwnd))
            setHiddenMode(false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        g.dpi = static_cast<float>(HIWORD(wp));
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        discardTarget(); // rebinds the back buffer at the new DPI on the next frame
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        const float s = g.dpi / 96.0f;
        mmi->ptMinTrackSize.x = static_cast<LONG>(1040 * s);
        mmi->ptMinTrackSize.y = static_cast<LONG>(640 * s);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g.ui.cursor()));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        in.mx = toDip(GET_X_LPARAM(lp));
        in.my = toDip(GET_Y_LPARAM(lp));
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&t);
        poke();
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!in.down) {
            in.mx = -1;
            in.my = -1;
        }
        poke();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        in.mx = toDip(GET_X_LPARAM(lp));
        in.my = toDip(GET_Y_LPARAM(lp));
        in.down = true;
        in.pressed = true;
        if (msg == WM_LBUTTONDBLCLK)
            in.doubleClick = true;
        SetCapture(hwnd);
        SetFocus(hwnd);
        poke();
        return 0;
    case WM_LBUTTONUP:
        in.down = false;
        in.released = true;
        ReleaseCapture();
        poke();
        return 0;
    case WM_CAPTURECHANGED:
        if (in.down) {
            in.down = false;
            in.released = true;
        }
        return 0;
    case WM_RBUTTONDOWN:
        in.mx = toDip(GET_X_LPARAM(lp));
        in.my = toDip(GET_Y_LPARAM(lp));
        in.rpressed = true;
        poke();
        return 0;
    case WM_MOUSEWHEEL:
        in.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        poke();
        return 0;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (wp) {
        case VK_RETURN: in.keyEnter = true; break;
        case VK_ESCAPE: in.keyEsc = true; break;
        case VK_BACK: in.keyBack = true; break;
        case VK_DELETE: in.keyDelete = true; break;
        case VK_TAB: in.keyTab = true; break;
        case 'Z':
            if (ctrl)
                (shift ? in.keyRedo : in.keyUndo) = true;
            break;
        case 'Y':
            if (ctrl)
                in.keyRedo = true;
            break;
        case 'B':
            if (!ctrl)
                in.keyB = true;
            break;
        case VK_F12:
            g.showPerf = !g.showPerf;
            break;
        default: break;
        }
        poke();
        return 0;
    }
    case WM_CHAR:
        if (wp >= 32 && wp != 127)
            in.chars += static_cast<wchar_t>(wp);
        poke();
        return 0;

    case kMsgTray:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            showMainWindow();
            break;
        case WM_CONTEXTMENU: {
            if (!g.appReady)
                break;
            const POINT pt{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
            int r = 0;
            guarded(L"tray menu", [&] { r = g.app.showMenu(pt, true); });
            if (r == App::kMenuOpen)
                showMainWindow();
            else if (r == App::kMenuQuit)
                quitApp(true);
            trayUpdate(true);
            break;
        }
        default:
            break;
        }
        return 0;
    case inst::kMsgShow:
        showMainWindow();
        return 0;
    case inst::kMsgQuit: // from setup / uninstall: no questions
        quitApp(false);
        return 0;
    case App::kMsgQuitAsk:
        quitApp(true);
        return 0;
    case App::kMsgRendererChanged:
        discardTarget(true);
        g.useFlip = flipRendererConfigured();
        g.lastFrame.QuadPart = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CLOSE:
        if (configGet(L"closeToTray", L"1") != L"0" && g.trayAdded)
            hideToTray();
        else
            quitApp(true);
        return 0;
    case WM_ACTIVATE:
    case WM_WINDOWPOSCHANGED:
    case WM_DISPLAYCHANGE:
        g.nextCoverCheck = 0; // re-check visibility on the next tick
        break;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK)
            g.sessionLocked = true;
        else if (wp == WTS_SESSION_UNLOCK) {
            g.sessionLocked = false;
            g.nextCoverCheck = 0;
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            const auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
            if (s && IsEqualGUID(s->PowerSetting, kDisplayStateGuid) && s->DataLength >= sizeof(DWORD)) {
                g.displayOff = *reinterpret_cast<const DWORD*>(s->Data) == 0; // 0 off, 1 on, 2 dimmed
                g.nextCoverCheck = 0;
            }
        }
        return TRUE;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp && g.appReady) {
            saveWindowPlacement();
            g.app.shutdown();
            g.appReady = false;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        WTSUnRegisterSessionNotification(hwnd);
        if (g.displayNotify)
            UnregisterPowerSettingNotification(g.displayNotify);
        trayRemove();
        if (g.appReady)
            g.app.shutdown();
        g.appReady = false;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ modes

// Self-test: WindowsEQ.exe --render-png out.png [width height [seconds [mx my]]]
// renders the real UI off-screen for a while (router running, analyzer fed)
// and saves the last frame. Used to check the layout without a desktop.
int renderToPng(const std::wstring& path, int w, int h, double seconds, float mx, float my, bool route)
{
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return 2;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)))
        return 2;
    ComPtr<ID2D1RenderTarget> rt;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    if (FAILED(g.d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt)))
        return 2;
    g.ui.setTarget(rt.Get());
    g.appReady = g.app.init(nullptr, route);
    if (!route)
        g.app.setDemoSignal(true); // previews show a live spectrum and live dynamics
    const int frames = std::max(1, static_cast<int>(seconds * 60));
    for (int i = 0; i < frames; ++i) {
        g.ui.in.mx = mx;
        g.ui.in.my = my;
        rt->BeginDraw();
        g.ui.beginFrame(static_cast<float>(w), static_cast<float>(h), 1.0 / 60);
        g.app.frame(g.ui, 1.0 / 60);
        g.ui.endFrame();
        rt->EndDraw();
        Sleep(16);
    }
    g.app.shutdown();
    g.ui.releaseTarget();

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(bmp.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return 3;
    return 0;
}

// Benchmark: WindowsEQ.exe --bench out.txt [frames]
// Opens a test window (no audio devices; set WEQ_DATA_DIR for demo settings),
// feeds the analyzer synthetic audio, renders frames as fast as possible and
// reports CPU time per frame for a few configurations. With vsync off, the
// achieved frame rate is limited by whichever of CPU or GPU is slower.
int runBench(HINSTANCE inst, const std::wstring& outPath, int frames)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WindowsEQ.Bench";
    RegisterClassExW(&wc);
    const UINT sysDpi = GetDpiForSystem();
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Windows EQ benchmark", WS_OVERLAPPEDWINDOW, 40, 40,
                             MulDiv(1320, sysDpi, 96), MulDiv(820, sysDpi, 96), nullptr, nullptr, inst, nullptr);
    if (!g.hwnd)
        return 2;
    g.dpi = static_cast<float>(GetDpiForWindow(g.hwnd));
    ShowWindow(g.hwnd, SW_SHOWNORMAL);
    g.presentImmediately = true;
    g.appReady = g.app.init(g.hwnd, false);
    g.app.setDemoSignal(true);

    auto pump = [] {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
    };
    struct Result {
        const wchar_t* name;
        double fps, frameMs, endDrawMs, cpuPct;
        unsigned long long settingsChanges, curveRebuilds;
    };
    std::vector<Result> results;
    auto runCase = [&](const wchar_t* name, int analyzerMode, bool skipText, unsigned skipMask = 0) {
        g.app.setAnalyzerMode(analyzerMode);
        g.ui.skipText = skipText;
        g.app.setBenchSkip(skipMask);
        for (int i = 0; i < 40; ++i) { // warm up (fills the analyzer too)
            pump();
            render();
        }
        FILETIME c, e, k0, u0, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        double sumFrame = 0, sumEnd = 0;
        const auto changes0 = g.app.settingsChanges(), rebuilds0 = g.app.curveRebuilds();
        for (int i = 0; i < frames; ++i) {
            pump();
            render();
            sumFrame += g.perfLastFrameMs;
            sumEnd += g.perfLastDrawMs;
        }
        QueryPerformanceCounter(&t1);
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
        auto ft = [](FILETIME f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const double wall = static_cast<double>(t1.QuadPart - t0.QuadPart) / g.qpf.QuadPart;
        const double cpu = (ft(k1) - ft(k0) + ft(u1) - ft(u0)) * 1e-7;
        results.push_back({name, frames / wall, sumFrame / frames, sumEnd / frames, cpu / wall * 100.0,
                           g.app.settingsChanges() - changes0, g.app.curveRebuilds() - rebuilds0});
    };
    runCase(L"full UI, analyzer before+after", 0, false);
    runCase(L"analyzer off", 2, false);
    runCase(L"background only", 2, true, 15);
    runCase(L"graph only (analyzer off)", 2, false, 14);
    runCase(L"side column only", 2, false, 13);
    runCase(L"band panel only", 2, false, 11);
    runCase(L"top/tool/status bars only", 2, false, 7);

    // Paced runs (vsync on, like the real app), measuring this process's
    // GPU 3D-engine utilisation with Windows' own performance counter.
    struct Paced {
        const wchar_t* name;
        double fps, gpuPct, cpuPct;
    };
    std::vector<Paced> paced;
    discardTarget();
    g.presentImmediately = false;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    wchar_t path[256];
    swprintf(path, 256, L"\\GPU Engine(pid_%lu_*engtype_3D)\\Utilization Percentage", GetCurrentProcessId());
    const bool pdhOk = PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS &&
                       PdhAddEnglishCounterW(query, path, 0, &counter) == ERROR_SUCCESS;
    auto pacedCase = [&](const wchar_t* name, double fps, int analyzerMode, unsigned skipMask) {
        g.app.setAnalyzerMode(analyzerMode);
        g.ui.skipText = false;
        g.app.setBenchSkip(skipMask);
        for (int i = 0; i < 30; ++i) {
            pump();
            render();
        }
        FILETIME c, e, k0, u0, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
        if (pdhOk)
            PdhCollectQueryData(query);
        const double start = nowSec();
        int n = 0;
        while (nowSec() - start < 5.0) {
            const double due = start + n / fps;
            while (nowSec() < due) {
                pump();
                Sleep(1);
            }
            pump();
            render();
            ++n;
        }
        const double wall = nowSec() - start;
        double gpu = -1;
        if (pdhOk && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            DWORD size = 0, count = 0;
            PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
            std::vector<BYTE> buf(size);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
            if (size && PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                gpu = 0;
                for (DWORD i = 0; i < count; ++i)
                    gpu += items[i].FmtValue.doubleValue;
            }
        }
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
        auto ft = [](FILETIME f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const double cpu = (ft(k1) - ft(k0) + ft(u1) - ft(u0)) * 1e-7;
        paced.push_back({name, n / wall, gpu, cpu / wall * 100.0});
    };
    // The app's real rates: analyzer 24 fps (active window) / 12 fps (inactive),
    // meters only 15 fps, idle 2 fps.
    pacedCase(L"24 fps, analyzer (active window)", 24, 0, 0);
    pacedCase(L"12 fps, analyzer (inactive window)", 12, 0, 0);
    pacedCase(L"15 fps, analyzer off (meters)", 15, 2, 0);
    pacedCase(L"2 fps, idle", 2, 0, 0);
    pacedCase(L"24 fps, background only", 24, 2, 15);
    if (query)
        PdhCloseQuery(query);

    FILE* f = nullptr;
    if (_wfopen_s(&f, outPath.c_str(), L"w") == 0 && f) {
        fwprintf(f, L"Windows EQ benchmark, %.0f DPI, renderer: %s\n\nUnpaced (vsync off), %d frames per case:\n",
                 g.dpi, g.useFlip ? L"flip model (low-GPU)" : L"window target (default)", frames);
        fwprintf(f, L"%-34s %8s %10s %10s %8s %9s %9s\n", L"case", L"fps", L"frame ms", L"EndDraw", L"CPU %",
                 L"changes", L"rebuilds");
        for (const Result& r : results)
            fwprintf(f, L"%-34s %8.0f %10.2f %10.2f %8.1f %9llu %9llu\n", r.name, r.fps, r.frameMs, r.endDrawMs,
                     r.cpuPct, r.settingsChanges, r.curveRebuilds);
        fwprintf(f, L"\nPaced (vsync on), 5 s per case:\n%-34s %8s %8s %8s\n", L"case", L"fps", L"GPU %", L"CPU %");
        for (const Paced& p : paced)
            fwprintf(f, L"%-34s %8.1f %8.2f %8.2f\n", p.name, p.fps, p.gpuPct, p.cpuPct);
        fclose(f);
    }
    g.app.shutdown();
    g.appReady = false;
    DestroyWindow(g.hwnd);
    return 0;
}

int runUninstall(bool quiet)
{
    BOOL deleteSettings = FALSE;
    if (!quiet) {
        TASKDIALOG_BUTTON buttons[] = {{IDOK, L"Uninstall"}, {IDCANCEL, L"Cancel"}};
        TASKDIALOGCONFIG tdc{};
        tdc.cbSize = sizeof(tdc);
        tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        tdc.pszWindowTitle = L"Uninstall Windows EQ";
        tdc.hMainIcon = createIcon(32, false);
        tdc.dwFlags |= TDF_USE_HICON_MAIN;
        tdc.pszMainInstruction = L"Uninstall Windows EQ?";
        tdc.pszContent =
            L"This removes the app, its Start menu shortcut and its sign-in launch.\n\n"
            L"VB-Cable is not removed. Windows will keep sending audio to \"CABLE Input\", so choose your "
            L"speakers or headphones in Sound settings afterwards.";
        tdc.pButtons = buttons;
        tdc.cButtons = 2;
        tdc.nDefaultButton = IDCANCEL;
        tdc.pszVerificationText = L"Also delete my presets and settings";
        int button = 0;
        if (FAILED(TaskDialogIndirect(&tdc, &button, nullptr, &deleteSettings)) || button != IDOK)
            return 1;
    }
    if (!inst::closeRunningInstance(8000)) {
        if (!quiet)
            MessageBoxW(nullptr, L"Windows EQ is still running and didn't close. Quit it from the tray and try again.",
                        L"Uninstall Windows EQ", MB_ICONWARNING);
        return 2;
    }
    inst::setAutostart(false, L"");
    DeleteFileW(inst::startMenuShortcutPath().c_str());
    inst::unregisterUninstall();
    if (deleteSettings)
        inst::deleteDirectoryAfterExit(appDataDir());
    if (inst::runningFromInstallDir()) {
        inst::deleteDirectoryAfterExit(inst::installDir());
    } else {
        DeleteFileW(inst::installedExePath().c_str());
        RemoveDirectoryW(inst::installDir().c_str());
    }
    if (!quiet) {
        TASKDIALOG_BUTTON buttons[] = {{100, L"Open Sound settings"}, {IDCLOSE, L"Close"}};
        TASKDIALOGCONFIG tdc{};
        tdc.cbSize = sizeof(tdc);
        tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        tdc.pszWindowTitle = L"Uninstall Windows EQ";
        tdc.pszMainIcon = TD_INFORMATION_ICON;
        tdc.pszMainInstruction = L"Windows EQ has been removed";
        tdc.pszContent = L"To hear audio again, choose your speakers or headphones as the output in Sound settings.";
        tdc.pButtons = buttons;
        tdc.cButtons = 2;
        tdc.nDefaultButton = 100;
        int button = 0;
        if (SUCCEEDED(TaskDialogIndirect(&tdc, &button, nullptr, nullptr)) && button == 100)
            ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 0;
}

bool hasArg(int argc, wchar_t** argv, const wchar_t* a)
{
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], a) == 0)
            return true;
    return false;
}

bool initGraphics()
{
    return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                       reinterpret_cast<void**>(g.d2d.GetAddressOf()))) &&
           SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(g.dwrite.GetAddressOf()))) &&
           g.ui.init(g.d2d.Get(), g.dwrite.Get());
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QueryPerformanceFrequency(&g.qpf);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    struct ArgsFree {
        wchar_t** a;
        ~ArgsFree() { LocalFree(a); }
    } argsFree{argv};

    if (hasArg(argc, argv, L"--uninstall"))
        return runUninstall(hasArg(argc, argv, L"--quiet"));
    if (argc >= 3 && _wcsicmp(argv[1], L"--write-icon") == 0)
        return writeIco(argv[2]);
    if (argc >= 3 && _wcsicmp(argv[1], L"--render-png") == 0) {
        if (!initGraphics())
            return 2;
        const int w = argc >= 5 ? _wtoi(argv[3]) : 1320, h = argc >= 5 ? _wtoi(argv[4]) : 820;
        const double secs = argc >= 6 ? _wtof(argv[5]) : 1.5;
        const float mx = argc >= 8 ? static_cast<float>(_wtof(argv[6])) : -1.0f;
        const float my = argc >= 8 ? static_cast<float>(_wtof(argv[7])) : -1.0f;
        return renderToPng(argv[2], w, h, secs, mx, my, !hasArg(argc, argv, L"--demo"));
    }
    g.useFlip = flipRendererConfigured();
    if (argc >= 3 && _wcsicmp(argv[1], L"--bench") == 0) {
        if (!initGraphics())
            return 2;
        return runBench(inst, argv[2], argc >= 4 ? std::max(50, _wtoi(argv[3])) : 300);
    }
    const bool startInTray = hasArg(argc, argv, L"--tray");
    installCrashHandler();

    // Diagnostics self-test: an exception escaping a window procedure, the
    // same path as a real UI crash. Expect crash.log + a .dmp in crashes\.
    if (hasArg(argc, argv, L"--crash-test")) {
        WNDCLASSW tc{};
        tc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            if (m == WM_APP)
                throw std::runtime_error("crash test (--crash-test)");
            return DefWindowProcW(h, m, w, l);
        };
        tc.hInstance = inst;
        tc.lpszClassName = L"WindowsEQ.CrashTest";
        RegisterClassW(&tc);
        HWND t = CreateWindowW(tc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        PostMessageW(t, WM_APP, 0, 0);
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0)
            DispatchMessageW(&m);
        return 0;
    }

    // One instance: show the existing one (unless this is a silent sign-in start).
    if (HWND existing = FindWindowW(inst::kMainWindowClass, nullptr)) {
        if (!startInTray) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, inst::kMsgShow, 0, 0);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    if (!initGraphics()) {
        MessageBoxW(nullptr, L"Direct2D is not available on this system.", L"Windows EQ", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon)
        wc.hIcon = createIcon(32, false);
    wc.hIconSm = createIcon(GetSystemMetrics(SM_CXSMICON), false);
    wc.lpszClassName = inst::kMainWindowClass;
    wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x10, 0x14));
    RegisterClassExW(&wc);

    // Initial size in DIPs, scaled for the primary monitor's DPI.
    const UINT sysDpi = GetDpiForSystem();
    const int w = MulDiv(1320, sysDpi, 96), h = MulDiv(820, sysDpi, 96);
    g.hwnd = CreateWindowExW(0, inst::kMainWindowClass, L"Windows EQ", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, w, h, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd)
        return 1;
    g.dpi = static_cast<float>(GetDpiForWindow(g.hwnd));
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g.msgDiagnostics = RegisterWindowMessageW(L"WindowsEQ.Diagnostics");
    WTSRegisterSessionNotification(g.hwnd, NOTIFY_FOR_THIS_SESSION);
    g.displayNotify = RegisterPowerSettingNotification(g.hwnd, &kDisplayStateGuid, DEVICE_NOTIFY_WINDOW_HANDLE);

    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(g.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark))))
        DwmSetWindowAttribute(g.hwnd, 19, &dark, sizeof(dark)); // Windows 10 before 20H1

    g.appReady = g.app.init(g.hwnd);
    trayAdd();
    restoreWindowPlacement();
    if (startInTray) {
        setHiddenMode(true);
    } else {
        ShowWindow(g.hwnd, g.wasMaximized ? SW_SHOWMAXIMIZED
                           : (show == SW_SHOWMINIMIZED || show == SW_MINIMIZE || show == SW_SHOWMINNOACTIVE)
                               ? show
                               : SW_SHOWNORMAL);
        UpdateWindow(g.hwnd);
        setHiddenMode(false);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
