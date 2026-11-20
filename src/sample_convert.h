// sample_convert.h - Converts audio from Winamp's format to the device's format.
//
// These are pure functions: they touch no Windows audio APIs and no shared
// state, which makes them safe on the real-time thread and easy to unit test.
#pragma once

#include <cstddef>

// Describes how one sample is stored in the device's buffer.
struct OutFormat {
    int container;  // Bits of storage per sample (16, 24 or 32)
    int valid;      // Bits that carry audio; 24-in-32 has container 32, valid 24
    bool isFloat;   // true = 32-bit float, false = signed integer
};

// A gain that changes smoothly across a run of frames, used for fades (see fader.h).
// Frame number f gets `start + step * f` for the first `frames` frames, then `end`.
struct GainRamp {
    float start;
    float step;
    size_t frames;
    float end;
};

// Converts `frames` frames from `in` to `out`. A frame is one sample for each
// channel. `inBps` is the input bit depth (8, 16, 24 or 32).
//
// `gains` holds one multiplier per channel, and `ramp` adds a fade on top. Pass NULL
// for both to skip volume processing; integer output is then bit-perfect when the
// device format can hold the input.
void ConvertFrames(const unsigned char* in, unsigned char* out, size_t frames,
                   int inBps, int channels, const OutFormat& format, const float* gains,
                   const GainRamp* ramp = NULL);

// Calculates per-channel gains from Winamp's volume (0..255) and pan (-128..128).
// `gains` must have room for `channels` values. Returns false when volume is full
// and pan is centred, meaning the audio should pass through untouched.
bool ComputeGains(int volume, int pan, int channels, float* gains);
