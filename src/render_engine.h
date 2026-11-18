// render_engine.h - The background thread that keeps audio flowing to the device.
//
// The plug-in has two sides:
//   * Winamp's threads call the plug-in's functions and put decoded audio in a ring.
//   * This engine's single real-time thread moves that audio to the sound device.
//
// All WASAPI calls for playback happen on the engine thread. Other threads only leave
// requests (see stream_control.h) and read published values, so device trouble can
// never freeze Winamp's interface.
#pragma once

#include <windows.h>
#include <atomic>
#include <string>

#include "device_monitor.h"
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

    // Stops the render thread and releases the device. Safe to call when stopped.
    void stop();

    // Requests pause or resume without waiting. Returns the previous pause state.
    bool setPaused(bool paused);

    // Resets the song position after a seek. Waits briefly for the thread to catch up.
    void flush();

    void setVolume(int volume) { volume_.store(volume); }
    void setPan(int pan) { pan_.store(pan); }

    // Song positions, in frames, published by the render thread.
    long long playedFrames() const { return played_.load(); }
    long long submittedFrames() const { return submitted_.load(); }

    int latencyMs() const { return latencyMs_.load(); }

    // Thread-safe access to the status text shown in the settings window.
    std::wstring statusText() const;
    void setStatusText(const std::wstring& text);

private:
    static DWORD WINAPI threadEntry(LPVOID self);
    void run();
    void mainLoop();

    HRESULT openAndStart();
    HRESULT fillDevice();

    void applyRequests();
    void pauseDevice();
    void resumeDevice();
    void applyFlush();

    void handleDeviceEvents();
    void onDeviceLost();
    void tryReopen();
    void publishPosition();

    // Signalling events. They last as long as the plug-in, so a late call from Winamp
    // can never use a handle that has already been closed.
    HANDLE wake_ = NULL;      // Wakes the render thread to look at new requests
    HANDLE ready_ = NULL;     // Signalled when the first device open has finished
    HANDLE flushAck_ = NULL;  // Signalled when a seek has been carried out

    HANDLE thread_ = NULL;
    std::atomic<bool> quit_{false};
    std::atomic<long> startResult_{E_PENDING};
    StreamControl control_;

    // Values shared with Winamp's threads.
    std::atomic<long long> played_{0};
    std::atomic<long long> submitted_{0};
    std::atomic<int> volume_{255};
    std::atomic<int> pan_{0};
    std::atomic<int> latencyMs_{0};

    mutable SRWLOCK statusLock_ = SRWLOCK_INIT;
    std::wstring status_ = L"Not playing";

    // Used only by the render thread while a stream is running.
    StreamParams params_;
    WasapiDevice device_;
    PositionTracker tracker_;
    IMMDeviceEnumerator* enumerator_ = NULL;
    DeviceMonitor* monitor_ = NULL;
    bool paused_ = false;
    bool retryNow_ = false;
    DWORD lastRetryTick_ = 0;
    unsigned seenFlush_ = 0;
};
