// CPU benchmark: how much of one core the EQ needs in real time.

#include "eqdsp/BiquadDesign.h"
#include "eqdsp/Equalizer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace eqdsp;

static void run(double fs, int channels, int bands, bool ramping)
{
    EqSettings s;
    s.numBands = static_cast<uint32_t>(bands);
    for (int i = 0; i < bands; ++i) {
        s.bands[i].type = FilterType::Peaking;
        s.bands[i].frequencyHz = 30.0 * std::pow(600.0, i / std::max(1.0, bands - 1.0));
        s.bands[i].gainDb = (i % 2 ? 3.0 : -3.0);
        s.bands[i].q = 1.4;
    }

    Equalizer eq;
    eq.prepare(fs, channels);
    eq.setSettings(s, true);

    const int block = 480; // 10 ms at 48 kHz, typical shared-mode period
    std::vector<float> audio(static_cast<size_t>(block) * channels);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> d(-0.5f, 0.5f);
    for (float& x : audio) x = d(rng);
    std::vector<float> output(audio.size());

    const double seconds = 20.0;
    const int iterations = static_cast<int>(seconds * fs / block);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (ramping && i % 2 == 0) {
            // Simulate a user dragging a band continuously.
            s.bands[0].gainDb = 6.0 * std::sin(i * 0.01);
            eq.setSettings(s);
        }
        eq.process(audio.data(), output.data(), block);
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    std::printf("%6.0f Hz  %d ch  %2d bands  %-8s  %7.3f%% of one core  (%.0fx realtime, %.1f ns/frame)\n",
                fs, channels, bands, ramping ? "dragging" : "steady",
                100.0 * elapsed / seconds, seconds / elapsed,
                1e9 * elapsed / (seconds * fs));
}

int main()
{
    std::printf("eqdsp benchmark (20 s of audio per row)\n\n");
    run(48000, 2, 10, false);
    run(48000, 2, 32, false);
    run(48000, 2, 10, true);
    run(48000, 8, 10, false);
    run(96000, 2, 10, false);
    run(192000, 2, 32, false);
    return 0;
}
