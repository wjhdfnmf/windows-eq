#include "Ui.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace gui {

Id makeId(const char* s, int index)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
        h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    h = (h ^ static_cast<uint32_t>(index)) * 16777619u;
    return h ? h : 1;
}

// ------------------------------------------------------------------ numbers

std::wstring formatNumber(double v, NumKind kind)
{
    wchar_t b[64];
    switch (kind) {
    case NumKind::Freq:
        if (v < 100.0)
            swprintf(b, 64, L"%.1f Hz", v);
        else if (v < 1000.0)
            swprintf(b, 64, L"%.0f Hz", v);
        else if (v < 10000.0)
            swprintf(b, 64, L"%.2f kHz", v / 1000.0);
        else
            swprintf(b, 64, L"%.1f kHz", v / 1000.0);
        break;
    case NumKind::Db:
        swprintf(b, 64, L"%+.1f dB", std::fabs(v) < 0.05 ? 0.0 : v);
        break;
    case NumKind::Q:
        swprintf(b, 64, L"%.2f", v);
        break;
    case NumKind::Ms:
        swprintf(b, 64, v < 10.0 ? L"%.1f ms" : L"%.0f ms", v);
        break;
    case NumKind::Ratio:
        swprintf(b, 64, v < 10.0 ? L"%.1f : 1" : L"%.0f : 1", v);
        break;
    default:
        swprintf(b, 64, L"%.2f", v);
        break;
    }
    return b;
}

static std::wstring formatEditable(double v, NumKind kind)
{
    wchar_t b[64];
    switch (kind) {
    case NumKind::Freq: swprintf(b, 64, v < 1000.0 ? L"%.1f" : L"%.0f", v); break;
    case NumKind::Db: swprintf(b, 64, L"%.1f", v); break;
    default: swprintf(b, 64, L"%.3g", v); break;
    }
    return b;
}

bool parseNumber(const std::wstring& s, NumKind kind, double& out)
{
    std::wstring t;
    for (wchar_t c : s)
        t += (c == L',') ? L'.' : c;
    const wchar_t* p = t.c_str();
    wchar_t* end = nullptr;
    double v = wcstod(p, &end);
    if (end == p)
        return false;
    while (*end == L' ')
        ++end;
    if (kind == NumKind::Freq && (*end == L'k' || *end == L'K'))
        v *= 1000.0;
    if (!std::isfinite(v))
        return false;
    out = v;
    return true;
}

// ------------------------------------------------------------------ setup

bool Ui::init(ID2D1Factory* d2d, IDWriteFactory* dwrite)
{
    d2d_ = d2d;
    dw_ = dwrite;
    struct Spec {
        const wchar_t* family;
        float size;
        DWRITE_FONT_WEIGHT weight;
    };
    const Spec specs[] = {
        {L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL},     // Body
        {L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL},     // Small
        {L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},  // Bold
        {L"Segoe UI", 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},  // Title
        {L"Segoe UI", 10.5f, DWRITE_FONT_WEIGHT_BOLD},       // Caps
        {L"Segoe UI", 20.0f, DWRITE_FONT_WEIGHT_SEMI_LIGHT}, // Big
        {L"Segoe MDL2 Assets", 14.0f, DWRITE_FONT_WEIGHT_NORMAL}, // Icon
        {L"Segoe MDL2 Assets", 11.0f, DWRITE_FONT_WEIGHT_NORMAL}, // IconSmall
        {L"Segoe UI", 10.0f, DWRITE_FONT_WEIGHT_BOLD},       // Node
    };
    for (int i = 0; i < static_cast<int>(Font::Count); ++i) {
        if (FAILED(dw_->CreateTextFormat(specs[i].family, nullptr, specs[i].weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, specs[i].size, L"en-us", &fmt_[i])))
            return false;
        fmt_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt_[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (SUCCEEDED(dw_->CreateEllipsisTrimmingSign(fmt_[i].Get(), &ellipsis_[i]))) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            fmt_[i]->SetTrimming(&trim, ellipsis_[i].Get());
        }
    }
    D2D1_STROKE_STYLE_PROPERTIES dash = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DASH);
    d2d_->CreateStrokeStyle(dash, nullptr, 0, &dashed_);
    D2D1_STROKE_STYLE_PROPERTIES round = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    d2d_->CreateStrokeStyle(round, nullptr, 0, &round_);
    return true;
}

void Ui::setTarget(ID2D1RenderTarget* rt)
{
    rt_ = rt;
    brush_.Reset();
    rt_->CreateSolidColorBrush(col::text, &brush_);
}

void Ui::releaseTarget()
{
    brush_.Reset();
    grad_.Reset();
    rt_ = nullptr;
}

ID2D1LinearGradientBrush* Ui::verticalGradient(D2D1_COLOR_F top, D2D1_COLOR_F bottom, float y0, float y1)
{
    auto same = [](D2D1_COLOR_F a, D2D1_COLOR_F b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
    if (!grad_ || !same(top, gradTop_) || !same(bottom, gradBottom_)) {
        grad_.Reset();
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0.0f, top}, {1.0f, bottom}};
        if (FAILED(rt_->CreateGradientStopCollection(gs, 2, &stops)) ||
            FAILED(rt_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, y0), D2D1::Point2F(0, y1)), stops.Get(), &grad_)))
            return nullptr;
        gradTop_ = top;
        gradBottom_ = bottom;
    }
    grad_->SetStartPoint(D2D1::Point2F(0, y0));
    grad_->SetEndPoint(D2D1::Point2F(0, y1));
    return grad_.Get();
}

// ------------------------------------------------------------------ frame

void Ui::beginFrame(float width, float height, double dtSec)
{
    w_ = width;
    h_ = height;
    dt_ = dtSec;
    time_ += dtSec;
    cursor_ = IDC_ARROW;
    tooltip_.clear();

    // A click outside the field being edited commits it (the owning widget
    // picks the value up this frame via pendingCommitId_).
    if (editId_ && in.pressed && !editRect_.contains(in.mx, in.my)) {
        pendingCommitId_ = editId_;
        pendingCommitBuf_ = editBuf_;
        editId_ = 0;
    }

    // The popup is modal: it consumes clicks and the wheel.
    if (popupId_) {
        if (in.keyEsc) {
            popupId_ = 0;
            in.keyEsc = false;
        } else if (in.pressed || in.rpressed) {
            if (popupRect_.contains(in.mx, in.my)) {
                const int idx = popupScroll_ + static_cast<int>((in.my - popupRect_.y - 4) / kItemH);
                if (idx >= 0 && idx < static_cast<int>(popupItems_.size())) {
                    resultId_ = popupId_;
                    resultIndex_ = idx;
                }
            }
            popupId_ = 0;
            in.pressed = in.rpressed = in.doubleClick = false;
        } else if (in.wheel != 0 && popupRect_.contains(in.mx, in.my)) {
            const int maxScroll = std::max(0, static_cast<int>(popupItems_.size()) - popupVisible_);
            popupScroll_ = std::clamp(popupScroll_ - static_cast<int>(in.wheel), 0, maxScroll);
            in.wheel = 0;
        }
    }
}

void Ui::endFrame()
{
    if (popupId_)
        drawPopup();

    if (!tooltip_.empty() && in.mx >= 0 && !in.down) {
        const float tw = textWidth(tooltip_, Font::Small) + 16;
        Rect r{in.mx + 14, in.my + 20, tw, 24};
        if (r.r() > w_ - 4)
            r.x = w_ - 4 - r.w;
        if (r.b() > h_ - 4)
            r.y = in.my - 30;
        shadow(r, 5, 6, 0.4f);
        fill(r, rgb(0x262B36), 5);
        stroke(r, col::borderHi, 5);
        text(tooltip_, r.inset(8, 0), col::text, Font::Small);
    }

    if (in.released && active_ && !in.down)
        active_ = 0;
    // A pending commit nobody claimed is dropped.
    pendingCommitId_ = 0;
    resultId_ = 0;

    in.pressed = in.released = in.doubleClick = in.rpressed = false;
    in.wheel = 0;
    in.chars.clear();
    in.keyEnter = in.keyEsc = in.keyBack = in.keyDelete = in.keyTab = false;
    in.keyUndo = in.keyRedo = in.keyB = false;

    // Drop text layouts not used for ~2 s (values that changed, closed popups).
    if (++frameNo_ % 64 == 0 || layouts_.size() > 4000) {
        for (auto it = layouts_.begin(); it != layouts_.end();)
            it = (frameNo_ - it->second.lastUsed > 48) ? layouts_.erase(it) : std::next(it);
    }
}

// ------------------------------------------------------------------ drawing

ID2D1SolidColorBrush* Ui::brush(D2D1_COLOR_F c)
{
    brush_->SetColor(c);
    return brush_.Get();
}

void Ui::fill(Rect r, D2D1_COLOR_F c, float radius)
{
    if (radius > 0)
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush(c));
    else
        rt_->FillRectangle(r.d2d(), brush(c));
}

void Ui::stroke(Rect r, D2D1_COLOR_F c, float radius, float width)
{
    const Rect s = r.inset(width * 0.5f);
    if (radius > 0)
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(s.d2d(), radius, radius), brush(c), width);
    else
        rt_->DrawRectangle(s.d2d(), brush(c), width);
}

void Ui::line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width, ID2D1StrokeStyle* style)
{
    rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(c), width, style);
}

void Ui::circle(float cx, float cy, float radius, D2D1_COLOR_F c)
{
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(c));
}

void Ui::circleStroke(float cx, float cy, float radius, D2D1_COLOR_F c, float width)
{
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(c), width);
}

IDWriteTextLayout* Ui::layout(const std::wstring& s, Font f, float w, float h, Align a)
{
    // Key: box size to 1/8 DIP, font, alignment, then the text itself.
    std::wstring key;
    key.reserve(s.size() + 24);
    key += std::to_wstring(static_cast<long>(w * 8)) + L'x' + std::to_wstring(static_cast<long>(h * 8));
    key += static_cast<wchar_t>(L'a' + static_cast<int>(f));
    key += static_cast<wchar_t>(L'0' + static_cast<int>(a));
    key += L'|';
    key += s;
    auto it = layouts_.find(key);
    if (it != layouts_.end()) {
        it->second.lastUsed = frameNo_;
        return it->second.layout.Get();
    }
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(dw_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), format(f), std::max(w, 0.0f),
                                     std::max(h, 0.0f), &lay)))
        return nullptr;
    lay->SetTextAlignment(a == Align::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                          : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                               : DWRITE_TEXT_ALIGNMENT_TRAILING);
    CachedLayout& entry = layouts_[key];
    entry.layout = lay;
    entry.lastUsed = frameNo_;
    return lay.Get();
}

void Ui::text(const std::wstring& s, Rect r, D2D1_COLOR_F c, Font f, Align a)
{
    if (s.empty() || r.w <= 0 || skipText)
        return;
    if (IDWriteTextLayout* lay = layout(s, f, r.w, r.h, a))
        rt_->DrawTextLayout(D2D1::Point2F(r.x, r.y), lay, brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Ui::textWidth(const std::wstring& s, Font f)
{
    IDWriteTextLayout* lay = layout(s, f, 4000.0f, 100.0f, Align::Left);
    if (!lay)
        return 0;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Ui::shadow(Rect r, float radius, float spread, float alpha)
{
    const int steps = 5;
    for (int i = steps; i >= 1; --i) {
        const float d = spread * i / steps;
        fill(Rect{r.x - d * 0.6f, r.y - d * 0.3f + 2, r.w + d * 1.2f, r.h + d * 1.2f},
             rgb(0x000000, alpha / steps), radius + d);
    }
}

// ------------------------------------------------------------------ widgets

bool Ui::button(Id id, Rect r, const std::wstring& label, Btn style, bool enabled, const wchar_t* icon)
{
    const bool hover = enabled && over(r) && (active_ == 0 || active_ == id);
    bool clicked = false;
    if (hover && in.pressed)
        active_ = id;
    if (active_ == id && in.released) {
        clicked = hover;
        active_ = 0;
    }
    const bool pressing = active_ == id && in.down;

    D2D1_COLOR_F bgc = col::field, fg = col::text, bc = col::border;
    switch (style) {
    case Btn::Primary:
        bgc = pressing ? rgb(0x3C8BE0) : hover ? col::accentHi : col::accent;
        fg = rgb(0x0B1420);
        bc = bgc;
        break;
    case Btn::Ghost:
        bgc = pressing ? col::fieldHover : hover ? col::field : rgb(0, 0);
        bc = rgb(0, 0);
        break;
    case Btn::Danger:
        bgc = pressing ? rgb(0x5A2A31) : hover ? rgb(0x4A252B) : col::field;
        fg = hover ? rgb(0xFF8A95) : col::text;
        break;
    default:
        bgc = pressing ? col::panel2 : hover ? col::fieldHover : col::field;
        bc = hover ? col::borderHi : col::border;
        break;
    }
    if (!enabled) {
        fg = col::textFaint;
        bgc = style == Btn::Ghost ? rgb(0, 0) : col::panel2;
    }
    fill(r, bgc, 6);
    if (bc.a > 0)
        stroke(r, bc, 6);

    if (icon && !label.empty()) {
        const float tw = textWidth(label, Font::Body);
        const float total = 16 + 6 + tw;
        const float x0 = r.cx() - total / 2;
        text(icon, Rect{x0, r.y, 16, r.h}, fg, Font::Icon, Align::Center);
        text(label, Rect{x0 + 22, r.y, tw + 2, r.h}, fg, Font::Body);
    } else if (icon) {
        text(icon, r, fg, Font::Icon, Align::Center);
    } else {
        text(label, r.inset(8, 0), fg, style == Btn::Primary ? Font::Bold : Font::Body, Align::Center);
    }
    return clicked;
}

bool Ui::iconButton(Id id, Rect r, const wchar_t* glyph, const std::wstring& tip, bool enabled, bool activeLook)
{
    const bool hover = enabled && over(r) && (active_ == 0 || active_ == id);
    bool clicked = false;
    if (hover && in.pressed)
        active_ = id;
    if (active_ == id && in.released) {
        clicked = hover;
        active_ = 0;
    }
    const bool pressing = active_ == id && in.down;
    if (activeLook)
        fill(r, withAlpha(col::accent, pressing ? 0.35f : hover ? 0.28f : 0.2f), 6);
    else if (pressing || hover)
        fill(r, pressing ? col::panel2 : col::fieldHover, 6);
    text(glyph, r, !enabled ? col::textFaint : activeLook ? col::accentHi : hover ? col::text : col::textDim,
         Font::Icon, Align::Center);
    if (hover && !tip.empty())
        tooltip(tip);
    return clicked;
}

bool Ui::toggleSwitch(Id id, Rect r, bool& value, const std::wstring& label)
{
    const bool hover = over(r) && (active_ == 0 || active_ == id);
    bool changed = false;
    if (hover && in.pressed)
        active_ = id;
    if (active_ == id && in.released) {
        if (hover) {
            value = !value;
            changed = true;
        }
        active_ = 0;
    }
    const Rect pill{r.x, r.cy() - 9, 34, 18};
    fill(pill, value ? col::accent : (hover ? col::borderHi : col::border), 9);
    const float kx = value ? pill.r() - 9 : pill.x + 9;
    circle(kx, pill.cy(), 6.5f, value ? rgb(0x0B1420) : col::textDim);
    if (!label.empty())
        text(label, Rect{pill.r() + 8, r.y, r.w - pill.w - 8, r.h}, hover ? col::text : col::textDim, Font::Body);
    return changed;
}

bool Ui::combo(Id id, Rect r, const std::vector<std::wstring>& items, int& selected, const std::wstring& placeholder,
               const std::wstring& prefix)
{
    bool changed = false;
    if (resultId_ == id) {
        if (resultIndex_ != selected) {
            selected = resultIndex_;
            changed = true;
        }
        resultId_ = 0;
    }
    const bool open = popupId_ == id;
    const bool hover = over(r) && active_ == 0;
    if (hover && in.pressed && !items.empty()) {
        popupId_ = id;
        popupItems_ = items;
        popupSel_ = selected;
        float wMax = r.w;
        for (const auto& it : items)
            wMax = std::max(wMax, textWidth(it, Font::Body) + 44);
        wMax = std::min(wMax, std::max(r.w, w_ - r.x - 8));
        popupVisible_ = std::min(static_cast<int>(items.size()), 12);
        const float ph = popupVisible_ * kItemH + 8;
        float py = r.b() + 4;
        if (py + ph > h_ - 8 && r.y - 4 - ph > 8)
            py = r.y - 4 - ph;
        popupRect_ = Rect{r.x, py, wMax, ph};
        popupScroll_ = std::clamp(selected - popupVisible_ / 2, 0,
                                  std::max(0, static_cast<int>(items.size()) - popupVisible_));
        in.pressed = false;
    }
    fill(r, open ? col::fieldHover : hover ? col::fieldHover : col::field, 6);
    stroke(r, open ? col::accent : hover ? col::borderHi : col::border, 6);
    const std::wstring shown =
        selected >= 0 && selected < static_cast<int>(items.size()) ? prefix + items[selected] : placeholder;
    text(shown, Rect{r.x + 10, r.y, r.w - 34, r.h}, selected >= 0 ? col::text : col::textFaint, Font::Body);
    text(L"", Rect{r.r() - 26, r.y, 18, r.h}, col::textDim, Font::IconSmall, Align::Center);
    return changed;
}

void Ui::drawPopup()
{
    const Rect r = popupRect_;
    shadow(r, 8, 14, 0.5f);
    fill(r, rgb(0x20242E), 8);
    stroke(r, col::borderHi, 8);
    rt_->PushAxisAlignedClip(r.inset(1).d2d(), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < popupVisible_; ++i) {
        const int idx = popupScroll_ + i;
        if (idx >= static_cast<int>(popupItems_.size()))
            break;
        const Rect ir{r.x + 4, r.y + 4 + i * kItemH, r.w - 8, kItemH};
        const bool hover = ir.contains(in.mx, in.my);
        if (hover)
            fill(ir, withAlpha(col::accent, 0.18f), 5);
        if (idx == popupSel_)
            text(L"", Rect{ir.x + 4, ir.y, 18, ir.h}, col::accent, Font::IconSmall, Align::Center);
        text(popupItems_[idx], Rect{ir.x + 26, ir.y, ir.w - 30, ir.h}, hover ? col::text : col::textDim, Font::Body);
    }
    rt_->PopAxisAlignedClip();
    const int total = static_cast<int>(popupItems_.size());
    if (total > popupVisible_) {
        const float trackH = r.h - 12;
        const float th = trackH * popupVisible_ / total;
        const float ty = r.y + 6 + trackH * popupScroll_ / total;
        fill(Rect{r.r() - 6, ty, 3, th}, col::borderHi, 1.5f);
    }
}

void Ui::editKeys(std::wstring& buf, bool& commit, bool& cancel)
{
    if (!in.chars.empty()) {
        if (editSelectAll_) {
            buf.clear();
            editSelectAll_ = false;
        }
        for (wchar_t c : in.chars)
            if (c >= 32 && buf.size() < 120)
                buf += c;
    }
    if (in.keyBack) {
        if (editSelectAll_)
            buf.clear();
        else if (!buf.empty())
            buf.pop_back();
        editSelectAll_ = false;
    }
    commit = in.keyEnter || in.keyTab;
    cancel = in.keyEsc;
}

bool Ui::numberField(Id id, Rect r, double& value, const NumberSpec& spec, bool enabled)
{
    bool changed = false;
    auto clampV = [&](double v) { return std::clamp(v, spec.lo, spec.hi); };

    // Commit from a click elsewhere.
    if (pendingCommitId_ == id) {
        double v;
        if (parseNumber(pendingCommitBuf_, spec.kind, v) && clampV(v) != value) {
            value = clampV(v);
            changed = true;
        }
        pendingCommitId_ = 0;
    }

    const bool isEditing = editId_ == id;
    const bool hover = enabled && over(r) && (active_ == 0 || active_ == id);

    if (isEditing) {
        bool commit = false, cancel = false;
        editKeys(editBuf_, commit, cancel);
        if (commit) {
            double v;
            if (parseNumber(editBuf_, spec.kind, v) && clampV(v) != value) {
                value = clampV(v);
                changed = true;
            }
            editId_ = 0;
        } else if (cancel) {
            editId_ = 0;
        }
    } else if (enabled) {
        if (hover && in.pressed) {
            active_ = id;
            pressX_ = in.mx;
            pressY_ = in.my;
            pressValue_ = value;
            dragged_ = false;
        }
        if (active_ == id && in.down) {
            const float dy = in.my - pressY_;
            if (!dragged_ && std::fabs(dy) > 3)
                dragged_ = true;
            if (dragged_) {
                const double k = in.shift ? 0.1 : 1.0;
                const double nv = spec.logScale ? pressValue_ * std::pow(2.0, -dy * spec.dragPerPx * k)
                                                : pressValue_ - dy * spec.dragPerPx * k;
                if (clampV(nv) != value) {
                    value = clampV(nv);
                    changed = true;
                }
            }
            setCursor(IDC_SIZENS);
        }
        if (active_ == id && in.released) {
            if (!dragged_ && hover) {
                editId_ = id;
                editRect_ = r;
                editBuf_ = formatEditable(value, spec.kind);
                editSelectAll_ = true;
            }
            active_ = 0;
        }
        if (hover && in.wheel != 0 && active_ == 0) {
            const double k = in.shift ? 0.1 : 1.0;
            const double nv = spec.logScale ? value * std::pow(2.0, in.wheel * spec.wheelStep * k)
                                            : value + in.wheel * spec.wheelStep * k;
            value = clampV(nv);
            changed = true;
            in.wheel = 0;
        }
        if (hover)
            setCursor(IDC_SIZENS);
    }

    const bool editingNow = editId_ == id;
    fill(r, editingNow ? col::bg : hover || active_ == id ? col::fieldHover : col::field, 6);
    stroke(r, editingNow ? col::accent : hover ? col::borderHi : col::border, 6);
    if (editingNow) {
        const Rect tr = r.inset(10, 0);
        if (editSelectAll_ && !editBuf_.empty()) {
            const float tw = textWidth(editBuf_, Font::Body);
            fill(Rect{tr.x - 2, r.cy() - 9, tw + 4, 18}, withAlpha(col::accent, 0.35f), 3);
        }
        text(editBuf_, tr, col::text, Font::Body);
        if (std::fmod(time_, 1.0) < 0.55) {
            const float cx = tr.x + textWidth(editBuf_, Font::Body) + 1;
            line(cx, r.cy() - 8, cx, r.cy() + 8, col::text, 1);
        }
        setCursor(IDC_IBEAM);
    } else {
        text(formatNumber(value, spec.kind), r.inset(10, 0), enabled ? col::text : col::textFaint, Font::Body,
             Align::Center);
    }
    return changed;
}

bool Ui::vslider(Id id, Rect r, double& value, double lo, double hi, double def)
{
    bool changed = false;
    const bool hover = over(r) && (active_ == 0 || active_ == id);
    const float trackX = r.cx();
    const float top = r.y + 8, bottom = r.b() - 8;
    auto toY = [&](double v) { return static_cast<float>(bottom - (v - lo) / (hi - lo) * (bottom - top)); };

    if (hover && in.doubleClick) {
        value = def;
        changed = true;
        in.doubleClick = false;
        in.pressed = false;
    }
    if (hover && in.pressed) {
        active_ = id;
        pressY_ = in.my;
        pressValue_ = value;
    }
    if (active_ == id && in.down) {
        const double k = in.shift ? 0.1 : 1.0;
        const double nv = std::clamp(pressValue_ - (in.my - pressY_) / (bottom - top) * (hi - lo) * k, lo, hi);
        const double snapped = std::round(nv * 10.0) / 10.0;
        if (snapped != value) {
            value = snapped;
            changed = true;
        }
        setCursor(IDC_SIZENS);
    }
    if (hover && in.wheel != 0 && active_ == 0) {
        value = std::clamp(std::round((value + in.wheel * (in.shift ? 0.1 : 0.5)) * 10.0) / 10.0, lo, hi);
        changed = true;
        in.wheel = 0;
    }

    fill(Rect{trackX - 2, top, 4, bottom - top}, col::field, 2);
    const float y0 = toY(std::clamp(0.0, lo, hi)), yv = toY(value);
    fill(Rect{trackX - 2, std::min(y0, yv), 4, std::fabs(yv - y0)}, col::accent, 2);
    for (double t = std::ceil(lo / 6) * 6; t <= hi; t += 6) {
        const float ty = toY(t);
        line(trackX + 8, ty, trackX + (t == 0 ? 16 : 12), ty, t == 0 ? col::textDim : col::textFaint, 1);
    }
    const Rect thumb{trackX - 11, yv - 6, 22, 12};
    fill(thumb, hover || active_ == id ? col::text : rgb(0xC9CED8), 4);
    line(thumb.x + 5, yv, thumb.r() - 5, yv, col::bg, 1.5f);
    return changed;
}

bool Ui::textField(Id id, Rect r, std::wstring& value, bool focus, bool& cancelled, const std::wstring& placeholder)
{
    cancelled = false;
    bool committed = false;
    if (focus && editId_ != id) {
        editId_ = id;
        editRect_ = r;
        editBuf_ = value;
        editSelectAll_ = false;
    }
    if (pendingCommitId_ == id) {
        value = pendingCommitBuf_;
        pendingCommitId_ = 0;
        cancelled = true; // clicking away abandons the dialog-like field
    }
    if (editId_ == id) {
        editRect_ = r;
        bool commit = false, cancel = false;
        editKeys(editBuf_, commit, cancel);
        value = editBuf_;
        if (commit && !editBuf_.empty()) {
            committed = true;
            editId_ = 0;
        } else if (cancel) {
            cancelled = true;
            editId_ = 0;
        }
    }
    const bool editingNow = editId_ == id;
    fill(r, col::bg, 6);
    stroke(r, editingNow ? col::accent : col::border, 6);
    const Rect tr = r.inset(10, 0);
    if (value.empty())
        text(placeholder, tr, col::textFaint, Font::Body);
    else
        text(value, tr, col::text, Font::Body);
    if (editingNow && std::fmod(time_, 1.0) < 0.55) {
        const float cx = tr.x + (value.empty() ? 0 : textWidth(value, Font::Body)) + 1;
        line(cx, r.cy() - 8, cx, r.cy() + 8, col::text, 1);
    }
    if (over(r))
        setCursor(IDC_IBEAM);
    return committed;
}

} // namespace gui
