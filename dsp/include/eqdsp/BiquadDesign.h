#pragma once

#include "eqdsp/Types.h"

namespace eqdsp {

// Normalised biquad coefficients (a0 == 1):
//   H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    bool isIdentity() const {
        return b0 == 1.0 && b1 == 0.0 && b2 == 0.0 && a1 == 0.0 && a2 == 0.0;
    }
};

// RBJ "Audio EQ Cookbook" designs (the same math AutoEQ / Equalizer APO
// presets assume). Disabled bands, 0 dB peaking/shelves and invalid input all
// return exact identity. Parameters are clamped to safe ranges.
// Real-time safe: no allocation, no locks.
BiquadCoeffs designBiquad(const Band& band, double sampleRate);

// Magnitude response of a single biquad, in dB.
double biquadMagnitudeDb(const BiquadCoeffs& c, double freqHz, double sampleRate);

// Total response of all bands that apply to `channel`, in dB. Includes the
// preamp when includePreamp is true. Returns 0 dB when bypassed.
double responseDb(const EqSettings& s, int channel, double freqHz, double sampleRate,
                  bool includePreamp = true);

// Largest boost (dB, filters only, no preamp) over 10 Hz .. Nyquist across the
// first numChannels channels. Dynamic bands that can boost count at full boost.
double maxBoostDb(const EqSettings& s, int numChannels, double sampleRate);

// Preamp (<= 0 dB) that cancels the largest boost so the EQ cannot clip.
double autoPreampDb(const EqSettings& s, int numChannels, double sampleRate);

} // namespace eqdsp
