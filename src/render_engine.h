// render_engine.h - The background thread that keeps audio flowing to the device.
//
// The plug-in has two sides:
//   * Winamp's threads call the plug-in's functions and put decoded audio in a ring.
//   * This engine's single real-time thread moves that audio to the sound device.
//
// All WASAPI calls for playback happen on the engine thread. Other threads only leave
// requests (see stream_control.h) and read published values, so device trouble can
// never freeze Winamp's interface.
//
// Two features shape the engine's behaviour:
//   * Fades: pause, seek and stop first fade the sound out over a few milliseconds,
//     and playback fades back in afterwards, so the waveform is never cut off.
//   * Gapless playback: at the end of a track the engine can "linger", playing the
//     last of the audio with the device still open. If the next track has the same
//     format, it simply continues on the same stream with no gap.
#pragma once

#include <windows.h>
#include <atomic>
#include <string>

#include "device_monitor.h"
#include "fader.h"
#include "position_tracker.h"
#include "settings.h"
#include "spsc_ring.h"
#include "stream_control.h"
#include "wasapi_device.h"

// Everything the engine needs to play one stream.
struct StreamParams {
    Settings settings;
    StreamFormat format;
    SpscRing* ring = NULL;  // Owned by the caller; must outlive the stream
};

class RenderEngine {
public:
    // Creates the signalling events. Call once when the plug-in loads.
    bool initialize();

    // Destroys the signalling events. Call once when the plug-in unloads.
    void shutdown();

    // Starts the render thread and waits for the device to open.
    HRESULT start(const StreamParams& params);

    // Stops the render thread and releases the device. With `fadeOut`, audible
    // playback fades out first, which takes a few tens of milliseconds. Safe to call
    // when stopped.
    void stop(bool fadeOut);

    // Requests pause or resume without waiting. Returns the previous pause state.
    bool setPaused(bool paused);

    // Resets the song position after a seek. Waits briefly for the thread to fade out
    // the old audio and catch up.
    void flush();

    // Gapless playback (see the top of this file).
    // beginLinger: the current track has ended; keep the device open and play out the
    //   remaining audio. Returns false if lingering is not possible right now.
    // resumeFromLinger: the next track continues on this stream. Returns false if the
    //   linger has already ended, in which case a fresh start is needed.
    // isLingering: true from beginLinger until the next start or resumeFromLinger.
    bool beginLinger();
    bool resumeFromLinger();
    bool isLingering() const { return lingerState_.load() != kActive; }
    bool lingerActive() const { return lingerState_.load() == kLingering; }

    void setVolume(int volume) { volume_.store(volume); }
    void setPan(int pan) { pan_.store(pan); }

    // Song positions, in frames since the stream started or was last seeked,
    // published by the render thread.
    long long playedFrames() const { return played_.load(); }
    long long submittedFrames() const { return submitted_.load(); }

    int latencyMs() const { return latencyMs_.load(); }

    // The status shown in the settings window, safe to call from any thread. While a
    // stream is open it describes the stream (see playback_status.h), including
    // whether volume is being applied in software right now.
    std::wstring statusText() const;

    // Replaces the status with a message such as "Not playing".
    void setStatusText(const std::wstring& text);

private:
    // States for gapless lingering. They only move forward until the next start:
    // Active -> Lingering -> (back to Active, or on to Expired).
    enum { kActive = 0, kLingering = 1, kExpired = 2 };

    static DWORD WINAPI threadEntry(LPVOID self);
    void run();
    void mainLoop();

    HRESULT openAndStart();
    HRESULT fillDevice();

    bool reconcile();
    void settle(bool flushPending, bool wantPaused);
    void startWithFadeIn();
    void resumeDevice();
    bool heardAllAudio();
    long long heardFrames();

    void handleDeviceEvents();
    void handleLinger();
    void onDeviceLost();
    void tryReopen();
    void publishPosition();
    void setPlaybackStatus(const PlaybackStatus& playback, bool volumeOption);

    // Signalling events. They last as long as the plug-in, so a late call from Winamp
    // can never use a handle that has already been closed.
    HANDLE wake_ = NULL;      // Wakes the render thread to look at new requests
    HANDLE ready_ = NULL;     // Signalled when the first device open has finished
    HANDLE flushAck_ = NULL;  // Signalled when a seek has been carried out

    HANDLE thread_ = NULL;
    std::atomic<bool> quit_{false};
    std::atomic<bool> fadeOnQuit_{false};
    std::atomic<long> startResult_{E_PENDING};
    std::atomic<int> lingerState_{kActive};
    std::atomic<DWORD> lingerStartTick_{0};
    StreamControl control_;

    // Values shared with Winamp's threads.
    std::atomic<long long> played_{0};
    std::atomic<long long> submitted_{0};
    std::atomic<int> volume_{255};
    std::atomic<int> pan_{0};
    std::atomic<int> latencyMs_{0};

    // The settings window's status. When `showPlayback_` is true the status describes
    // `playback_`; otherwise it is the message in `status_`. Guarded by statusLock_.
    mutable SRWLOCK statusLock_ = SRWLOCK_INIT;
    std::wstring status_ = L"Not playing";
    PlaybackStatus playback_;
    bool showPlayback_ = false;
    bool playbackVolumeOption_ = false;  // The "apply volume in software" setting of that stream

    // Used only by the render thread while a stream is running.
    StreamParams params_;
    WasapiDevice device_;
    PositionTracker tracker_;
    Fader fader_;
    IMMDeviceEnumerator* enumerator_ = NULL;
    DeviceMonitor* monitor_ = NULL;
    bool paused_ = false;      // The device is actually paused (not just requested)
    bool settling_ = false;    // A fade-out is in progress before pausing, seeking or stopping
    DWORD settleStartTick_ = 0;
    bool retryNow_ = false;
    DWORD lastRetryTick_ = 0;
    unsigned seenFlush_ = 0;
    long long fadeFrames_ = 0;
};
