// Checks the GUI spectrum analyzer's FFT, scaling and tilt with known sines.
#include "Analyzer.h"

#include <cmath>
#include <cstdio>
#include <vector>

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* what, double got, double want) {
        std::printf("  [%s] %s: %.2f (want %.2f)\n", ok ? " ok " : "FAIL", what, got, want);
        failures += ok ? 0 : 1;
    };
    const double fs = 48000.0;
    struct Case {
        double freq, amp, wantDb;
    } cases[] = {
        {1000.0, 1.0, 0.0},                             // tilt is 0 dB at 1 kHz
        {1000.0, 0.5, -6.02},                           // half amplitude
        {100.0, 1.0, 3.0 * std::log2(0.1)},             // -9.97 dB tilt
        {8000.0, 0.1, -20.0 + 3.0 * std::log2(8.0)},    // -11 dB
    };
    for (const Case& c : cases) {
        gui::Analyzer a;
        a.setSampleRate(fs);
        std::vector<float> buf(2 * 1024);
        long long n = 0;
        // Feed 1 s so the smoothing settles; post = pre.
        for (int block = 0; block < 47; ++block) {
            for (int i = 0; i < 1024; ++i, ++n) {
                const float v = static_cast<float>(c.amp * std::sin(2 * 3.14159265358979 * c.freq * n / fs));
                buf[2 * i] = buf[2 * i + 1] = v;
            }
            a.push(buf.data(), 1024);
            a.update(1024 / fs);
        }
        const float level = a.levelAt(a.post(), c.freq / 1.02, c.freq * 1.02);
        char what[64];
        std::snprintf(what, sizeof(what), "%.0f Hz sine at %.2f", c.freq, c.amp);
        // A tone between two bins reads low by the Hann window's scalloping
        // loss (at most 1.42 dB); it never reads high.
        check(level <= c.wantDb + 0.05 && level > c.wantDb - 1.45, what, level, c.wantDb);
        // Far from the tone the spectrum must be way down (window leakage).
        const float far = a.levelAt(a.post(), c.freq * 3.0, c.freq * 3.5);
        check(far < c.wantDb - 60, "  leakage 3x away", far, c.wantDb - 60);
        check(a.hasSignal(), "  hasSignal", a.hasSignal(), 1);
    }
    std::printf("%s\n", failures ? "analyzer test FAILED" : "analyzer test passed");
    return failures ? 1 : 0;
}
