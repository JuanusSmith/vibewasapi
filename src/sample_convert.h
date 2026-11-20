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

// Dither: what it is and why it is here.
//
// When audio must be stored with fewer bits than it has (a 24-bit track on a device
// that only takes 16-bit, or any track whose volume is being changed in software),
// the extra detail has to be rounded away. Plain rounding makes the error follow the
// music, which adds faint harsh distortion to quiet passages and fade-outs. Adding a
// tiny amount of random noise first (about one step of the smallest bit) turns that
// error into a steady, much less noticeable hiss far below the music. The noise here
// is "TPDF" (triangular probability density function): the sum of two random numbers,
// the standard choice because it fully removes the distortion.
//
// The state is just a random-number generator carried from one call to the next.
struct DitherState {
    unsigned int seed = 0x9E3779B9u;  // Any non-zero starting value works
};

// True when converting `inBps`-bit audio to `format` loses detail, so it needs dither.
// `scaling` means volume, pan or a fade is being applied. Float output and 32-bit
// integer output have more precision than any source, so they never need it.
bool NeedsDither(int inBps, const OutFormat& format, bool scaling);

// Converts `frames` frames from `in` to `out`. A frame is one sample for each
// channel. `inBps` is the input bit depth (8, 16, 24 or 32).
//
// `gains` holds one multiplier per channel, and `ramp` adds a fade on top. Pass NULL
// for both to skip volume processing; integer output is then bit-perfect when the
// device format can hold the input.
//
// When NeedsDither() is true, the result is rounded to the device's bit depth, with
// TPDF dither if `dither` is given. Bit-perfect conversions are never touched.
void ConvertFrames(const unsigned char* in, unsigned char* out, size_t frames,
                   int inBps, int channels, const OutFormat& format, const float* gains,
                   const GainRamp* ramp = NULL, DitherState* dither = NULL);

// Calculates per-channel gains from Winamp's volume (0..255) and pan (-128..128).
// `gains` must have room for `channels` values. Returns false when volume is full
// and pan is centred, meaning the audio should pass through untouched.
bool ComputeGains(int volume, int pan, int channels, float* gains);
