#include "eqdsp/PresetParser.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>

namespace eqdsp {

namespace {

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\xEF' ||
                          s.front() == '\xBB' || s.front() == '\xBF')) // also strips a UTF-8 BOM
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool iequal(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

std::vector<std::string_view> tokens(std::string_view s)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        const size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t')
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::optional<double> number(std::string_view s)
{
    if (!s.empty() && s.front() == '+')
        s.remove_prefix(1);
    double v = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size() || !std::isfinite(v))
        return std::nullopt;
    return v;
}

// "-6.2" or "-6.2dB" (unit glued on) followed optionally by a unit token.
std::optional<double> numberWithUnit(const std::vector<std::string_view>& t, size_t& i, std::string_view unit)
{
    if (i >= t.size())
        return std::nullopt;
    std::string_view s = t[i];
    if (s.size() > unit.size() && iequal(s.substr(s.size() - unit.size()), unit))
        s.remove_suffix(unit.size());
    const auto v = number(s);
    if (!v)
        return std::nullopt;
    ++i;
    if (i < t.size() && iequal(t[i], unit))
        ++i;
    return v;
}

void warn(std::vector<std::string>& w, int line, const std::string& msg)
{
    w.push_back("line " + std::to_string(line) + ": " + msg);
}

// Equalizer APO channel names (WAVE order): L R C SUB RL RR SL SR, or 1-based numbers.
std::optional<uint32_t> channelBit(std::string_view name)
{
    static constexpr std::string_view names[] = {"L", "R", "C", "SUB", "RL", "RR", "SL", "SR"};
    for (uint32_t i = 0; i < 8; ++i)
        if (iequal(name, names[i]))
            return 1u << i;
    if (iequal(name, "LFE"))
        return 1u << 3;
    if (const auto n = number(name); n && *n >= 1 && *n <= kMaxChannels && *n == std::floor(*n))
        return 1u << (static_cast<uint32_t>(*n) - 1);
    return std::nullopt;
}

struct TypeInfo {
    std::string_view name;
    FilterType type;
    bool hasGain;
    bool needsQ;     // false: default Q used when absent
    double defaultQ;
};

constexpr double kButterworthQ = 0.70710678118654752;

constexpr TypeInfo kTypes[] = {
    {"PK", FilterType::Peaking, true, true, 1.0},
    {"PEQ", FilterType::Peaking, true, true, 1.0},
    {"LS", FilterType::LowShelf, true, false, kButterworthQ},
    {"LSC", FilterType::LowShelf, true, false, kButterworthQ},
    {"HS", FilterType::HighShelf, true, false, kButterworthQ},
    {"HSC", FilterType::HighShelf, true, false, kButterworthQ},
    {"LP", FilterType::LowPass, false, false, kButterworthQ},
    {"LPQ", FilterType::LowPass, false, false, kButterworthQ},
    {"HP", FilterType::HighPass, false, false, kButterworthQ},
    {"HPQ", FilterType::HighPass, false, false, kButterworthQ},
    {"BP", FilterType::BandPass, false, true, 1.0},
    {"NO", FilterType::Notch, false, true, 1.0},
    {"AP", FilterType::AllPass, false, true, kButterworthQ},
};

const char* typeName(FilterType t)
{
    switch (t) {
    case FilterType::Peaking: return "PK";
    case FilterType::LowShelf: return "LSC";
    case FilterType::HighShelf: return "HSC";
    case FilterType::LowPass: return "LPQ";
    case FilterType::HighPass: return "HPQ";
    case FilterType::BandPass: return "BP";
    case FilterType::Notch: return "NO";
    case FilterType::AllPass: return "AP";
    }
    return "PK";
}

// Windows EQ extension, stored as a comment so Equalizer APO ignores it:
//   # WindowsEQ Dynamic 3: Mode Above Threshold -30 dB Ratio 2 Range -6 dB Attack 5 ms Release 120 ms
// It follows its Filter line; the number is that filter's position.
void parseDynamicLine(std::string_view rest, EqSettings& out, std::vector<std::string>& warnings, int lineNo)
{
    const size_t colon = rest.find(':');
    if (colon == std::string_view::npos)
        return;
    const std::vector<std::string_view> key = tokens(rest.substr(0, colon));
    if (key.size() < 2 || !iequal(key[1], "Dynamic"))
        return; // some other (future) Windows EQ extension
    if (out.numBands == 0) {
        warn(warnings, lineNo, "Dynamic settings before any filter; ignored");
        return;
    }
    uint32_t index = out.numBands - 1; // normally: the filter just above
    if (key.size() >= 3)
        if (const auto n = number(key[2]); n && *n >= 1 && *n <= out.numBands && *n == std::floor(*n))
            index = static_cast<uint32_t>(*n) - 1;
    Band& b = out.bands[index];
    if (!supportsDynamics(b.type)) {
        warn(warnings, lineNo, "Dynamic settings only apply to peak and shelf filters; ignored");
        return;
    }
    const std::vector<std::string_view> t = tokens(rest.substr(colon + 1));
    Band d = b;
    d.dynamic = 1;
    size_t i = 0;
    bool bad = false;
    while (i < t.size() && !bad) {
        const std::string_view k = t[i++];
        if (iequal(k, "Mode")) {
            bad = i >= t.size() || !(iequal(t[i], "Above") || iequal(t[i], "Below"));
            if (!bad)
                d.dynMode = iequal(t[i++], "Below") ? DynMode::Below : DynMode::Above;
        } else if (iequal(k, "Off")) {
            d.dynamic = 0;
        } else if (iequal(k, "Threshold")) {
            const auto v = numberWithUnit(t, i, "dB");
            bad = !v;
            if (!bad) d.thresholdDb = std::clamp(*v, -100.0, 0.0);
        } else if (iequal(k, "Ratio")) {
            std::string_view s = i < t.size() ? t[i] : std::string_view{};
            if (s.size() > 2 && s.substr(s.size() - 2) == ":1")
                s.remove_suffix(2);
            const auto v = number(s);
            bad = !v || *v < 1.0;
            if (!bad) { d.ratio = std::min(*v, 100.0); ++i; }
        } else if (iequal(k, "Range")) {
            const auto v = numberWithUnit(t, i, "dB");
            bad = !v;
            if (!bad) d.rangeDb = std::clamp(*v, -30.0, 30.0);
        } else if (iequal(k, "Attack")) {
            const auto v = numberWithUnit(t, i, "ms");
            bad = !v || *v <= 0;
            if (!bad) d.attackMs = std::min(*v, 1000.0);
        } else if (iequal(k, "Release")) {
            const auto v = numberWithUnit(t, i, "ms");
            bad = !v || *v <= 0;
            if (!bad) d.releaseMs = std::min(*v, 5000.0);
        } else {
            warn(warnings, lineNo, "Dynamic: unexpected \"" + std::string(k) + "\"; rest of line ignored");
            break;
        }
    }
    if (bad) {
        warn(warnings, lineNo, "Dynamic: malformed value; dynamic settings ignored");
        return;
    }
    b = d;
}

} // namespace

bool parseEqualizerApoConfig(std::string_view text, EqSettings& out, std::vector<std::string>& warnings)
{
    out = EqSettings{};
    out.numBands = 0;
    uint32_t channelMask = kAllChannels;
    bool sawSomething = false, sawContent = false;
    int lineNo = 0;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++lineNo;

        // "# WindowsEQ ..." comments carry Windows EQ's own settings.
        if (const std::string_view raw = trim(line); !raw.empty() && raw.front() == '#') {
            const std::string_view rest = trim(raw.substr(1));
            if (rest.size() >= 9 && iequal(rest.substr(0, 9), "WindowsEQ")) {
                parseDynamicLine(rest, out, warnings, lineNo);
                continue;
            }
        }

        if (const size_t hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;
        sawContent = true;

        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            warn(warnings, lineNo, "ignored (no \"Key:\")");
            continue;
        }
        const std::string_view key = trim(line.substr(0, colon));
        const std::vector<std::string_view> t = tokens(line.substr(colon + 1));
        const std::vector<std::string_view> keyTok = tokens(key);
        const std::string_view keyword = keyTok.empty() ? std::string_view{} : keyTok[0];

        if (iequal(keyword, "Preamp")) {
            size_t i = 0;
            const auto v = numberWithUnit(t, i, "dB");
            if (!v) {
                warn(warnings, lineNo, "Preamp: expected a number in dB");
                continue;
            }
            out.preampDb += *v; // Equalizer APO accumulates multiple Preamp lines
            sawSomething = true;
            continue;
        }

        if (iequal(keyword, "Channel")) {
            if (t.empty()) {
                warn(warnings, lineNo, "Channel: missing channel list");
                continue;
            }
            uint32_t mask = 0;
            bool ok = true;
            for (const std::string_view c : t) {
                if (iequal(c, "all")) {
                    mask = kAllChannels;
                    continue;
                }
                const auto bit = channelBit(c);
                if (!bit) {
                    warn(warnings, lineNo, "Channel: unknown channel \"" + std::string(c) + "\"");
                    ok = false;
                    break;
                }
                mask |= *bit;
            }
            if (ok)
                channelMask = mask;
            sawSomething = true;
            continue;
        }

        if (!iequal(keyword, "Filter")) {
            warn(warnings, lineNo, "\"" + std::string(key) + "\" is not supported; ignored");
            continue;
        }

        // ---- Filter
        size_t i = 0;
        if (i >= t.size() || !(iequal(t[i], "ON") || iequal(t[i], "OFF"))) {
            warn(warnings, lineNo, "Filter: expected ON or OFF");
            continue;
        }
        const bool enabled = iequal(t[i++], "ON");
        if (i >= t.size()) {
            warn(warnings, lineNo, "Filter: missing type");
            continue;
        }
        const TypeInfo* info = nullptr;
        for (const TypeInfo& ti : kTypes)
            if (iequal(t[i], ti.name))
                info = &ti;
        if (!info) {
            warn(warnings, lineNo, "Filter: unsupported type \"" + std::string(t[i]) + "\"");
            continue;
        }
        ++i;

        // Optional shelf slope token ("LS 12dB" etc.)
        if (i < t.size() && (iequal(t[i], "12dB") || iequal(t[i], "6dB"))) {
            if (iequal(t[i], "6dB"))
                warn(warnings, lineNo, "6 dB/oct shelf approximated by a 12 dB/oct (second-order) shelf");
            ++i;
        }

        Band b;
        b.type = info->type;
        b.enabled = enabled ? 1u : 0u;
        b.channelMask = channelMask;
        b.q = info->defaultQ;
        bool haveFc = false, haveQ = false, bad = false;

        while (i < t.size() && !bad) {
            if (iequal(t[i], "Fc")) {
                ++i;
                const auto v = numberWithUnit(t, i, "Hz");
                bad = !v || *v <= 0;
                if (!bad) { b.frequencyHz = *v; haveFc = true; }
            } else if (iequal(t[i], "Gain")) {
                ++i;
                const auto v = numberWithUnit(t, i, "dB");
                bad = !v;
                if (!bad) b.gainDb = *v;
            } else if (iequal(t[i], "Q")) {
                ++i;
                const auto v = i < t.size() ? number(t[i]) : std::nullopt;
                bad = !v || *v <= 0;
                if (!bad) { b.q = *v; haveQ = true; ++i; }
            } else if (iequal(t[i], "BW")) {
                ++i;
                if (i < t.size() && iequal(t[i], "Oct"))
                    ++i;
                const auto v = i < t.size() ? number(t[i]) : std::nullopt;
                bad = !v || *v <= 0;
                if (!bad) {
                    const double p = std::pow(2.0, *v);
                    b.q = std::sqrt(p) / (p - 1.0);
                    haveQ = true;
                    ++i;
                }
            } else {
                warn(warnings, lineNo, "Filter: unexpected \"" + std::string(t[i]) + "\"; rest of line ignored");
                break;
            }
        }
        if (bad) {
            warn(warnings, lineNo, "Filter: malformed value; filter skipped");
            continue;
        }
        if (!haveFc) {
            warn(warnings, lineNo, "Filter: missing Fc; filter skipped");
            continue;
        }
        if (info->needsQ && !haveQ)
            warn(warnings, lineNo, "Filter: no Q given; using " + std::to_string(info->defaultQ).substr(0, 4));
        if (!info->hasGain)
            b.gainDb = 0.0;

        if (out.numBands >= static_cast<uint32_t>(kMaxBands)) {
            warn(warnings, lineNo, "more than " + std::to_string(kMaxBands) + " filters; extra filter ignored");
            continue;
        }
        out.bands[out.numBands++] = b;
        sawSomething = true;
    }

    return sawSomething || !sawContent;
}

std::string formatEqualizerApoConfig(const EqSettings& s)
{
    std::string out;
    char buf[256];
    std::snprintf(buf, sizeof buf, "Preamp: %.2f dB\n", s.preampDb);
    out += buf;

    uint32_t mask = kAllChannels;
    const int n = std::min<int>(static_cast<int>(s.numBands), kMaxBands);
    for (int i = 0; i < n; ++i) {
        const Band& b = s.bands[i];
        if (b.channelMask != mask) {
            mask = b.channelMask;
            if (mask == kAllChannels) {
                out += "Channel: all\n";
            } else {
                out += "Channel:";
                for (int c = 0; c < kMaxChannels; ++c)
                    if (mask & (1u << c)) {
                        std::snprintf(buf, sizeof buf, " %d", c + 1);
                        out += buf;
                    }
                out += "\n";
            }
        }
        const bool gainType = b.type == FilterType::Peaking || b.type == FilterType::LowShelf ||
                              b.type == FilterType::HighShelf;
        if (gainType)
            std::snprintf(buf, sizeof buf, "Filter %d: %s %s Fc %.6g Hz Gain %.4g dB Q %.6g\n", i + 1,
                          b.enabled ? "ON" : "OFF", typeName(b.type), b.frequencyHz, b.gainDb, b.q);
        else
            std::snprintf(buf, sizeof buf, "Filter %d: %s %s Fc %.6g Hz Q %.6g\n", i + 1, b.enabled ? "ON" : "OFF",
                          typeName(b.type), b.frequencyHz, b.q);
        out += buf;
        if (b.dynamic && gainType) {
            std::snprintf(buf, sizeof buf,
                          "# WindowsEQ Dynamic %d: Mode %s Threshold %.4g dB Ratio %.4g Range %.4g dB Attack %.4g ms "
                          "Release %.4g ms\n",
                          i + 1, b.dynMode == DynMode::Below ? "Below" : "Above", b.thresholdDb, b.ratio, b.rangeDb,
                          b.attackMs, b.releaseMs);
            out += buf;
        }
    }
    return out;
}

} // namespace eqdsp
