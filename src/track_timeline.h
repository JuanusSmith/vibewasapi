// track_timeline.h - Keeps the song time Winamp sees: the position shown in its time
// display and seek bar, and how far ahead the decoder has written.
//
// Two complications make this more than "frames divided by sample rate":
//
//   * Seeks. After a seek to 1:30, frame counting restarts at zero, and every time
//     reported adds 1:30 (the "seek base").
//
//     A seek takes a few milliseconds to carry out, because the render thread first
//     fades the old audio out. Until it is done, the engine's played-frame count still
//     belongs to the audio *before* the seek. Adding that old count to the new seek
//     base would briefly show a time far past the target (seeking to 1:30 after
//     14 seconds of playback would flash 1:44). So between seek() and finishSeek(),
//     the reported time is simply the seek target.
//
//   * Gapless track changes. The device stream keeps running from one track into the
//     next, so the render engine's frame counts belong to the *stream*, not to a track.
//     When a track begins on a running stream, the timeline remembers the stream frame
//     where it starts. Until playback reaches that frame, the listener is still hearing
//     the previous track, so the new track's time stays at 0:00.
//
// It also remembers whether IsPlaying has told Winamp the track has ended, which is
// what Close uses to tell a natural track end (linger for gapless) from Stop.
//
// All values are atomic, so any of Winamp's threads may call any method. The plug-in's
// lifecycle lock keeps the multi-step changes (starts and seeks) from overlapping.
#pragma once

#include <atomic>

class TrackTimeline {
public:
    // A fresh stream opened for a new track: everything starts at zero.
    void startStream(int sampleRate) {
        sampleRate_.store(sampleRate > 0 ? sampleRate : 1);
        streamFramesWritten_.store(0);
        resetTrack(0, 0);
    }

    // A new track continues on the running stream (gapless). It begins straight after
    // the audio already written.
    void startGaplessTrack() { resetTrack(streamFramesWritten_.load(), 0); }

    // A seek to `timeMs`. The ring and the engine's counts restart from zero, so this
    // does too. Call finishSeek() once the engine has carried the seek out.
    void seek(int timeMs) {
        // Raise the flag before changing the seek base, so a GetOutputTime call made
        // in between never pairs the new base with the old played-frame count.
        seekPending_.store(true);
        streamFramesWritten_.store(0);
        resetTrack(0, timeMs);
    }

    // The engine has carried out the seek and its played-frame count has restarted
    // from zero, so outputTimeMs() can go back to counting from the seek base.
    void finishSeek() { seekPending_.store(false); }

    // Records `frames` of new audio written by the decoder.
    void onWritten(long long frames) {
        if (frames <= 0) return;
        streamFramesWritten_.fetch_add(frames);
        trackFramesWritten_.fetch_add(frames);
        endReported_.store(false);  // More audio means this is not the end yet
    }

    // IsPlaying has told Winamp that this track has finished.
    void markEndReported() { endReported_.store(true); }
    bool endReported() const { return endReported_.load(); }

    // The song time being heard now, given the engine's count of stream frames played.
    int outputTimeMs(long long streamFramesPlayed) const {
        // Mid-seek, the played count is left over from before the seek (see the top
        // of this file), so report the target the listener is about to hear instead.
        if (seekPending_.load()) return seekBaseMs_.load();

        long long trackFrames = streamFramesPlayed - trackStartFrame_.load();
        if (trackFrames < 0) trackFrames = 0;  // Still hearing the previous track
        return seekBaseMs_.load() + toMs(trackFrames);
    }

    // The song time of the newest audio the decoder has written.
    int writtenTimeMs() const { return seekBaseMs_.load() + toMs(trackFramesWritten_.load()); }

private:
    void resetTrack(long long startFrame, int seekBaseMs) {
        trackStartFrame_.store(startFrame);
        trackFramesWritten_.store(0);
        seekBaseMs_.store(seekBaseMs);
        endReported_.store(false);
    }

    int toMs(long long frames) const { return (int)(frames * 1000 / sampleRate_.load()); }

    std::atomic<int> sampleRate_{44100};
    std::atomic<long long> streamFramesWritten_{0};  // Across gapless track changes
    std::atomic<long long> trackStartFrame_{0};      // Stream frame where this track begins
    std::atomic<long long> trackFramesWritten_{0};   // Since this track began or was seeked
    std::atomic<int> seekBaseMs_{0};                 // Song time of the most recent seek
    std::atomic<bool> endReported_{false};
    std::atomic<bool> seekPending_{false};           // Between seek() and finishSeek()
};
