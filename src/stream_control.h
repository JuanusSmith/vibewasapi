// stream_control.h - How Winamp's threads tell the render thread what they want.
//
// Requests are stored as the *desired state*, not as a queue of commands:
//   * Pause is one value: should playback be paused right now?
//   * Seeks are a counter: each new seek adds one.
//
// The render thread compares this with what it last did and catches up whenever it
// can. If it is busy reopening a device, nothing is lost: repeated pause clicks
// simply settle on the newest value, and a pending seek stays visible in the counter.
#pragma once

#include <atomic>

class StreamControl {
public:
    // Clears all requests. Called before a new stream starts.
    void reset() {
        wantPaused_.store(false);
        flushRequested_.store(0);
        flushCompleted_.store(0);
    }

    // Any thread: sets the desired pause state and returns the previous one.
    bool setPaused(bool paused) { return wantPaused_.exchange(paused); }

    // Any thread: reads the desired pause state.
    bool wantPaused() const { return wantPaused_.load(); }

    // Any thread: requests a seek and returns its number, for flushCompleted().
    unsigned requestFlush() { return flushRequested_.fetch_add(1) + 1; }

    // Render thread: the number of the newest requested seek.
    unsigned latestFlush() const { return flushRequested_.load(); }

    // Render thread: records that every seek up to `number` has been carried out.
    void markFlushCompleted(unsigned number) { flushCompleted_.store(number); }

    // Any thread: true once seek `number` has been carried out. The subtraction keeps
    // the comparison correct if the counter ever wraps past its maximum value.
    bool flushCompleted(unsigned number) const {
        return static_cast<int>(flushCompleted_.load() - number) >= 0;
    }

private:
    std::atomic<bool> wantPaused_{false};
    std::atomic<unsigned> flushRequested_{0};
    std::atomic<unsigned> flushCompleted_{0};
};
