// position_tracker.h - Turns the sound card's clock into a position in the song.
//
// The device clock counts every frame played, including silence we insert when
// the decoder falls behind. To show the correct song time we need to subtract
// silence that played before real audio.
//
// Silence is held as "pending" until more real audio follows it. Trailing silence
// at the end of a track therefore never counts against the song position.
//
// This class is not thread-safe; only the render thread uses it.
#pragma once

class PositionTracker {
public:
    // Starts counting from `songFrames`. This is used after a seek (with 0) and
    // after reopening a device, because a new device's clock starts again at zero.
    void restartAt(long long songFrames) {
        frameOffset_ = songFrames;
        committedSilence_ = 0;
        pendingSilence_ = 0;
        submittedAudio_ = 0;
    }

    // Records one buffer sent to the device: `audioFrames` of music followed by
    // `silenceFrames` of padding.
    void onBufferFilled(long long audioFrames, long long silenceFrames) {
        if (audioFrames > 0) {
            committedSilence_ += pendingSilence_;
            pendingSilence_ = 0;
            submittedAudio_ += audioFrames;
        }
        pendingSilence_ += silenceFrames;
    }

    // Converts the device clock's position into frames of the song actually heard.
    long long playedFrames(long long deviceFrames) const {
        long long played = deviceFrames - committedSilence_;
        if (played < 0) played = 0;
        if (played > submittedAudio_) played = submittedAudio_;
        return frameOffset_ + played;
    }

    // Song position of the end of the audio handed to the device so far.
    long long submittedFrames() const { return frameOffset_ + submittedAudio_; }

private:
    long long frameOffset_ = 0;       // Song position when this device clock started
    long long committedSilence_ = 0;  // Silence that played before real audio
    long long pendingSilence_ = 0;    // Silence not yet followed by real audio
    long long submittedAudio_ = 0;    // Real audio frames sent to this device
};
