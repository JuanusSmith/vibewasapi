// sample_convert.cpp - Implements the conversions described in sample_convert.h.
//
// Every input sample is first widened to a signed 32-bit value, with the audio in
// the top bits. That gives all formats one common representation, so each output
// format only needs to know how to store a 32-bit sample.
#include "sample_convert.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

// Reads one little-endian sample and left-aligns it in 32 bits.
inline int32_t ReadSample(const unsigned char* p, int bps) {
    switch (bps) {
    case 8:  // 8-bit WAV audio is unsigned, so flip the top bit to make it signed.
        return (int32_t)(((uint32_t)p[0] ^ 0x80u) << 24);
    case 16:
        return (int32_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 24));
    case 24:
        return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
    default:
        return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    }
}

// Stores the most significant bits of a left-aligned sample in the device format.
inline void WriteIntSample(unsigned char* p, int32_t sample, const OutFormat& format) {
    uint32_t u = (uint32_t)sample;
    switch (format.container) {
    case 16:
        p[0] = (unsigned char)(u >> 16);
        p[1] = (unsigned char)(u >> 24);
        break;
    case 24:
        p[0] = (unsigned char)(u >> 8);
        p[1] = (unsigned char)(u >> 16);
        p[2] = (unsigned char)(u >> 24);
        break;
    default:
        // In 24-in-32 the lowest byte is unused padding and must be zero.
        if (format.valid == 24) u &= 0xFFFFFF00u;
        p[0] = (unsigned char)u;
        p[1] = (unsigned char)(u >> 8);
        p[2] = (unsigned char)(u >> 16);
        p[3] = (unsigned char)(u >> 24);
        break;
    }
}

}  // namespace

void ConvertFrames(const unsigned char* in, unsigned char* out, size_t frames,
                   int inBps, int channels, const OutFormat& format, const float* gains,
                   const GainRamp* ramp) {
    const int inBytes = inBps / 8;
    const int outBytes = format.container / 8;
    const double kToFloat = 1.0 / 2147483648.0;  // Maps 32-bit integers to -1.0..1.0
    const bool scale = gains || ramp;

    for (size_t frame = 0; frame < frames; ++frame) {
        // The fade level for this frame; 1.0 when there is no fade.
        double fade = 1.0;
        if (ramp) fade = frame < ramp->frames ? ramp->start + ramp->step * (double)frame : ramp->end;

        for (int channel = 0; channel < channels; ++channel) {
            int32_t sample = ReadSample(in, inBps);
            in += inBytes;

            if (scale) {
                const double gain = (gains ? gains[channel] : 1.0) * fade;
                double scaled = (double)sample * gain;
                if (format.isFloat) {
                    float value = (float)(scaled * kToFloat);
                    memcpy(out, &value, 4);
                } else {
                    // Limit the result so a large value cannot wrap around into a loud click.
                    if (scaled > 2147483647.0) scaled = 2147483647.0;
                    if (scaled < -2147483648.0) scaled = -2147483648.0;
                    WriteIntSample(out, (int32_t)lrint(scaled), format);
                }
            } else if (format.isFloat) {
                float value = (float)(sample * kToFloat);
                memcpy(out, &value, 4);
            } else {
                WriteIntSample(out, sample, format);  // Lossless when the container is wide enough
            }
            out += outBytes;
        }
    }
}

bool ComputeGains(int volume, int pan, int channels, float* gains) {
    if (volume >= 255 && pan == 0) return false;  // Keep the bit-perfect path

    // Squaring the volume gives a curve that sounds more even to the ear.
    const float level = volume <= 0 ? 0.f : (float)(volume / 255.0) * (float)(volume / 255.0);

    // Pan only applies to stereo: turning toward one side lowers the other side.
    float left = 1.f, right = 1.f;
    if (channels == 2) {
        if (pan > 0) left = (128 - pan) / 128.f;
        else if (pan < 0) right = (128 + pan) / 128.f;
    }

    for (int channel = 0; channel < channels; ++channel) {
        const float balance = channels == 2 ? (channel == 0 ? left : right) : 1.f;
        gains[channel] = level * balance;
    }
    return true;
}
