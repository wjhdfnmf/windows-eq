#pragma once

// A small immediate-mode UI toolkit on Direct2D / DirectWrite. All
// coordinates are DIPs. Widgets are drawn and hit-tested in the same call;
// popups (dropdown lists) and tooltips are drawn last, on top.

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gui {

using Microsoft::WRL::ComPtr;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect inset(float d) const { return inset(d, d); }
    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

constexpr D2D1_COLOR_F rgb(uint32_t hex, float a = 1.0f)
{
    return {((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a};
}
inline D2D1_COLOR_F withAlpha(D2D1_COLOR_F c, float a)
{
    c.a = a;
    return c;
}

namespace col {
constexpr D2D1_COLOR_F bg = rgb(0x0E1014);
constexpr D2D1_COLOR_F panel = rgb(0x161920);
constexpr D2D1_COLOR_F panel2 = rgb(0x1C2029);
constexpr D2D1_COLOR_F field = rgb(0x222733);
constexpr D2D1_COLOR_F fieldHover = rgb(0x2A303D);
constexpr D2D1_COLOR_F border = rgb(0x2B313D);
constexpr D2D1_COLOR_F borderHi = rgb(0x3D4555);
constexpr D2D1_COLOR_F text = rgb(0xE7EAF0);
constexpr D2D1_COLOR_F textDim = rgb(0x9AA2B1);
constexpr D2D1_COLOR_F textFaint = rgb(0x5F6776);
constexpr D2D1_COLOR_F accent = rgb(0x4DA3FF);
constexpr D2D1_COLOR_F accentHi = rgb(0x73B8FF);
constexpr D2D1_COLOR_F good = rgb(0x3DDC97);
constexpr D2D1_COLOR_F warn = rgb(0xFFB547);
constexpr D2D1_COLOR_F bad = rgb(0xFF5D6C);
constexpr D2D1_COLOR_F grid = rgb(0x232833);
constexpr D2D1_COLOR_F gridMajor = rgb(0x2E3442);
} // namespace col

enum class Font { Body, Small, Bold, Title, Caps, Big, Icon, IconSmall, Node, Count };
enum class Align { Left, Center, Right };
enum class Btn { Normal, Primary, Ghost, Danger };

using Id = uint32_t;
Id makeId(const char* s, int index = 0);

struct Input {
    float mx = -1, my = -1;
    bool down = false;
    bool pressed = false, released = false, doubleClick = false, rpressed = false;
    float wheel = 0; // notches, positive = away from the user
    std::wstring chars;
    bool keyEnter = false, keyEsc = false, keyBack = false, keyDelete = false, keyTab = false;
    bool keyUndo = false, keyRedo = false, keyB = false;
    bool shift = false, ctrl = false;
};

enum class NumKind { Freq, Db, Q, Plain };
struct NumberSpec {
    NumKind kind = NumKind::Plain;
    double lo = 0, hi = 1;
    bool logScale = false;
    double dragPerPx = 0.01; // linear: units per px; log: octaves per px
    double wheelStep = 0.1;  // linear: units per notch; log: octaves per notch
};

std::wstring formatNumber(double v, NumKind kind);
bool parseNumber(const std::wstring& s, NumKind kind, double& out);

class Ui {
public:
    bool init(ID2D1Factory* d2d, IDWriteFactory* dwrite);
    void setTarget(ID2D1RenderTarget* rt);
    void releaseTarget();

    void beginFrame(float width, float height, double dtSec);
    void endFrame();

    Input in;

    // ---- drawing
    ID2D1RenderTarget* rt() const { return rt_; }
    ID2D1Factory* factory() const { return d2d_; }
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c);
    // One cached two-stop vertical gradient (recreated only when colours change).
    ID2D1LinearGradientBrush* verticalGradient(D2D1_COLOR_F top, D2D1_COLOR_F bottom, float y0, float y1);
    ID2D1StrokeStyle* dashed() const { return dashed_.Get(); }
    ID2D1StrokeStyle* roundCap() const { return round_.Get(); }
    void fill(Rect r, D2D1_COLOR_F c, float radius = 0);
    void stroke(Rect r, D2D1_COLOR_F c, float radius = 0, float width = 1);
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width = 1, ID2D1StrokeStyle* style = nullptr);
    void circle(float cx, float cy, float radius, D2D1_COLOR_F c);
    void circleStroke(float cx, float cy, float radius, D2D1_COLOR_F c, float width = 1);
    void text(const std::wstring& s, Rect r, D2D1_COLOR_F c, Font f = Font::Body, Align a = Align::Left);
    float textWidth(const std::wstring& s, Font f);
    void shadow(Rect r, float radius, float spread = 10.0f, float alpha = 0.35f);

    // ---- widgets (return true when the value changed / the button was clicked)
    bool button(Id id, Rect r, const std::wstring& label, Btn style = Btn::Normal, bool enabled = true,
                const wchar_t* icon = nullptr);
    bool iconButton(Id id, Rect r, const wchar_t* glyph, const std::wstring& tip, bool enabled = true,
                    bool activeLook = false);
    bool toggleSwitch(Id id, Rect r, bool& value, const std::wstring& label);
    bool combo(Id id, Rect r, const std::vector<std::wstring>& items, int& selected, const std::wstring& placeholder = L"",
               const std::wstring& prefix = L"");
    bool numberField(Id id, Rect r, double& value, const NumberSpec& spec, bool enabled = true);
    bool vslider(Id id, Rect r, double& value, double lo, double hi, double def);
    // Returns true when the user pressed Enter (commit); Esc sets cancelled.
    bool textField(Id id, Rect r, std::wstring& value, bool focus, bool& cancelled, const std::wstring& placeholder);

    void tooltip(const std::wstring& s) { tooltip_ = s; }
    void setCursor(LPCWSTR idc) { cursor_ = idc; }
    LPCWSTR cursor() const { return cursor_; }

    // ---- helpers for custom widgets
    bool blocked() const { return popupId_ != 0; }       // a popup owns the mouse
    bool over(Rect r) const { return !blocked() && r.contains(in.mx, in.my); }
    bool editing() const { return editId_ != 0; }
    Id active() const { return active_; }
    void setActive(Id id) { active_ = id; }
    double time() const { return time_; }
    float width() const { return w_; }
    float height() const { return h_; }

private:
    IDWriteTextFormat* format(Font f) { return fmt_[static_cast<int>(f)].Get(); }
    void drawPopup();
    void editKeys(std::wstring& buf, bool& commit, bool& cancel);

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    ID2D1RenderTarget* rt_ = nullptr;
    ComPtr<IDWriteTextFormat> fmt_[static_cast<int>(Font::Count)];
    ComPtr<IDWriteInlineObject> ellipsis_[static_cast<int>(Font::Count)];
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1LinearGradientBrush> grad_;
    D2D1_COLOR_F gradTop_{}, gradBottom_{};
    ComPtr<ID2D1StrokeStyle> dashed_, round_;

    float w_ = 0, h_ = 0;
    double time_ = 0, dt_ = 0;
    Id active_ = 0;
    LPCWSTR cursor_ = IDC_ARROW;
    std::wstring tooltip_;

    // drag state for number fields / sliders
    float pressX_ = 0, pressY_ = 0;
    double pressValue_ = 0;
    bool dragged_ = false;

    // text editing
    Id editId_ = 0;
    Rect editRect_;
    std::wstring editBuf_;
    bool editSelectAll_ = false;
    Id pendingCommitId_ = 0;
    std::wstring pendingCommitBuf_;

    // dropdown popup
    Id popupId_ = 0;
    Rect popupRect_;
    std::vector<std::wstring> popupItems_;
    int popupSel_ = -1;
    int popupScroll_ = 0;
    int popupVisible_ = 0;
    Id resultId_ = 0;
    int resultIndex_ = -1;
    static constexpr float kItemH = 28.0f;
};

} // namespace gui
