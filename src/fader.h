// fader.h - Short volume ramps that stop pause, seek and stop from clicking.
//
// Cutting a sound wave off mid-swing makes an audible click. Fading the level to zero
// over a few milliseconds first removes it. The Fader holds the current level (0.0 =
// silent, 1.0 = full) and moves it a little with every audio frame.
//
// When the level is steady at 1.0 the Fader reports isUnity(), and the audio is left
// completely untouched, so normal playback stays bit-perfect.
//
// This class is not thread-safe; only the render thread uses it.
#pragma once

#include "sample_convert.h"

class Fader {
public:
    // Jumps straight to `level` with no ramp.
    void setLevel(float level) {
        level_ = level;
        target_ = level;
        step_ = 0.f;
        remaining_ = 0;
    }

    // Moves smoothly from the current level to `target` over `frames` frames.
    void fadeTo(float target, long long frames) {
        if (frames <= 0) {
            setLevel(target);
            return;
        }
        target_ = target;
        remaining_ = frames;
        step_ = (target - level_) / (float)frames;
    }

    bool isFading() const { return remaining_ > 0; }
    bool isSilent() const { return !isFading() && level_ <= 0.f; }
    bool isUnity() const { return !isFading() && level_ >= 1.f; }

    // How many more frames of real audio should be read before the sound is silent.
    // Returns -1 when the level is not heading for silence, meaning "no limit".
    long long audibleFramesLeft() const {
        if (isSilent()) return 0;
        if (isFading() && target_ <= 0.f) return remaining_;
        return -1;
    }

    // Describes the gain for the next `frames` frames, then moves past them.
    GainRamp take(long long frames) {
        GainRamp ramp;
        ramp.start = level_;
        ramp.step = step_;
        ramp.frames = 0;
        if (remaining_ > 0 && frames > 0) {
            const long long used = frames < remaining_ ? frames : remaining_;
            ramp.frames = (size_t)used;
            remaining_ -= used;
            level_ = remaining_ > 0 ? level_ + step_ * (float)used : target_;
        }
        ramp.end = level_;  // Frames after the ramp keep this level
        return ramp;
    }

private:
    float level_ = 1.f;
    float target_ = 1.f;
    float step_ = 0.f;
    long long remaining_ = 0;
};
