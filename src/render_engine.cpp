// render_engine.cpp - The real-time loop described in render_engine.h.
//
// The loop sleeps until something needs attention, then handles it in this order:
//   1. Requests from Winamp (pause, resume, seek, stop), using fades where audible.
//   2. Device changes reported by Windows.
//   3. Moving audio to the device, or reconnecting if there is no device.
//   4. Ending a gapless linger once the last audio has played.
//
// Device recovery has one simple rule: when a device is lost, remember the song
// position and keep trying until a device is available. While paused, no attempt is
// made until the user resumes, because nothing needs to play until then.
//
// Pause, seek and stop share one sequence, called "settling":
//   a. Fade the level down to silence over kFadeMs while audio keeps playing.
//   b. Wait until the faded audio has actually left the speakers.
//   c. Stop and clear the device, then apply what was asked for.
//   d. If playback continues, fade back in from silence.
#include "render_engine.h"

#include <cstdio>

namespace {

const DWORD kLoopTimeoutMs = 100;       // Longest sleep; also how often device health is checked
const DWORD kRetryIntervalMs = 1000;    // Delay between reconnect attempts
const DWORD kStartTimeoutMs = 5000;     // Longest time Winamp waits for a device to open
const DWORD kFlushTimeoutMs = 2000;     // Longest time a seek waits for the render thread
const int kStartupAttempts = 15;        // With the delay below, retries for about 3 seconds
const DWORD kStartupRetryDelayMs = 200;

const int kFadeMs = 15;                 // Length of each fade; short enough to feel instant
const DWORD kSettleSlackMs = 250;       // Extra time allowed for a fade to finish playing
const DWORD kLingerGraceMs = 2000;      // How long to wait for the next track after the last audio

// MMCSS is the Windows service that gives audio threads priority over other work.
// It is loaded at run time so a system without it can still play audio.
typedef HANDLE(WINAPI* AvSetMmThreadCharacteristicsFn)(LPCWSTR, LPDWORD);
typedef BOOL(WINAPI* AvRevertMmThreadCharacteristicsFn)(HANDLE);

}  // namespace

bool RenderEngine::initialize() {
    wake_ = CreateEventW(NULL, FALSE, FALSE, NULL);
    ready_ = CreateEventW(NULL, TRUE, FALSE, NULL);
    flushAck_ = CreateEventW(NULL, FALSE, FALSE, NULL);
    return wake_ && ready_ && flushAck_;
}

void RenderEngine::shutdown() {
    stop(false);
    if (wake_) CloseHandle(wake_);
    if (ready_) CloseHandle(ready_);
    if (flushAck_) CloseHandle(flushAck_);
    wake_ = ready_ = flushAck_ = NULL;
}

HRESULT RenderEngine::start(const StreamParams& params) {
    stop(false);
    if (!wake_ || !ready_ || !flushAck_) return E_FAIL;

    params_ = params;
    control_.reset();
    tracker_.restartAt(0);
    fader_.setLevel(1.f);  // A new stream starts at full level, so its first notes are untouched
    fadeFrames_ = (long long)params_.format.sampleRate * kFadeMs / 1000;
    played_.store(0);
    submitted_.store(0);
    paused_ = false;
    settling_ = false;
    retryNow_ = false;
    seenFlush_ = 0;
    quit_.store(false);
    fadeOnQuit_.store(false);
    lingerState_.store(kActive);
    startResult_.store(E_PENDING);
    ResetEvent(ready_);
    ResetEvent(flushAck_);

    thread_ = CreateThread(NULL, 0, threadEntry, this, 0, NULL);
    if (!thread_) return HRESULT_FROM_WIN32(GetLastError());

    WaitForSingleObject(ready_, kStartTimeoutMs);
    const HRESULT hr = startResult_.load();
    if (FAILED(hr)) {
        stop(false);
        wchar_t text[64];
        _snwprintf(text, 63, L"Open failed (0x%08lX)", (unsigned long)hr);
        text[63] = 0;
        setStatusText(text);
    }
    return hr;
}

void RenderEngine::stop(bool fadeOut) {
    if (!thread_) return;
    fadeOnQuit_.store(fadeOut);
    quit_.store(true);
    SetEvent(wake_);
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
    thread_ = NULL;
    lingerState_.store(kActive);
    setStatusText(L"Not playing");
}

bool RenderEngine::setPaused(bool paused) {
    const bool previous = control_.setPaused(paused);
    if (wake_) SetEvent(wake_);
    return previous;
}

void RenderEngine::flush() {
    const unsigned number = control_.requestFlush();
    SetEvent(wake_);

    // The request cannot be lost, so a timeout here only means it finishes a little later.
    const DWORD startTick = GetTickCount();
    while (!control_.flushCompleted(number)) {
        const DWORD elapsed = GetTickCount() - startTick;
        if (elapsed >= kFlushTimeoutMs) break;
        WaitForSingleObject(flushAck_, kFlushTimeoutMs - elapsed);
    }
    played_.store(0);
    submitted_.store(0);
}

bool RenderEngine::beginLinger() {
    // A paused track is not ending naturally, so it is stopped instead.
    if (!thread_ || control_.wantPaused()) return false;
    lingerStartTick_.store(GetTickCount());
    int expected = kActive;
    const bool started = lingerState_.compare_exchange_strong(expected, kLingering);
    if (started) SetEvent(wake_);
    return started;
}

bool RenderEngine::resumeFromLinger() {
    // Only one of this call and the render thread's expiry can win the exchange.
    int expected = kLingering;
    return lingerState_.compare_exchange_strong(expected, kActive);
}

std::wstring RenderEngine::statusText() const {
    AcquireSRWLockShared(&statusLock_);
    std::wstring copy = status_;
    ReleaseSRWLockShared(&statusLock_);
    return copy;
}

void RenderEngine::setStatusText(const std::wstring& text) {
    AcquireSRWLockExclusive(&statusLock_);
    status_ = text;
    ReleaseSRWLockExclusive(&statusLock_);
}

DWORD WINAPI RenderEngine::threadEntry(LPVOID self) {
    static_cast<RenderEngine*>(self)->run();
    return 0;
}

void RenderEngine::run() {
    const HRESULT comResult = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // Ask Windows for professional-audio scheduling, or the highest thread priority.
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    HANDLE mmcssTask = NULL;
    AvRevertMmThreadCharacteristicsFn avRevert = NULL;
    if (avrt) {
        AvSetMmThreadCharacteristicsFn avSet =
            (AvSetMmThreadCharacteristicsFn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
        avRevert = (AvRevertMmThreadCharacteristicsFn)(void*)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
        DWORD taskIndex = 0;
        if (avSet) mmcssTask = avSet(L"Pro Audio", &taskIndex);
    }
    if (!mmcssTask) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    HRESULT hr = CreateDeviceEnumerator(&enumerator_);
    if (SUCCEEDED(hr)) {
        // Playback still works without notifications; recovery just relies on retries.
        monitor_ = new DeviceMonitor(wake_);
        if (FAILED(enumerator_->RegisterEndpointNotificationCallback(monitor_))) {
            monitor_->Release();
            monitor_ = NULL;
        }

        // A device can be briefly busy right after the previous stream releases it,
        // for example when one track stops and the next starts immediately.
        hr = E_FAIL;
        for (int attempt = 0; attempt < kStartupAttempts && !quit_.load(); ++attempt) {
            hr = openAndStart();
            if (SUCCEEDED(hr) || !IsTransientOpenError(hr)) break;
            Sleep(kStartupRetryDelayMs);
        }
    }

    startResult_.store(hr);
    SetEvent(ready_);
    if (SUCCEEDED(hr)) mainLoop();

    // Stop notifications before releasing anything they could touch.
    device_.close();
    if (monitor_) {
        enumerator_->UnregisterEndpointNotificationCallback(monitor_);
        monitor_->Release();
        monitor_ = NULL;
    }
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = NULL;
    }

    if (mmcssTask && avRevert) avRevert(mmcssTask);
    if (avrt) FreeLibrary(avrt);
    if (SUCCEEDED(comResult)) CoUninitialize();
}

void RenderEngine::mainLoop() {
    for (;;) {
        HANDLE handles[2] = {wake_, device_.audioEvent()};
        const DWORD handleCount = device_.isOpen() ? 2 : 1;
        const DWORD woke = WaitForMultipleObjects(handleCount, handles, FALSE, kLoopTimeoutMs);

        if (reconcile()) break;  // True when it is time to stop
        handleDeviceEvents();

        if (device_.isOpen()) {
            if (woke == WAIT_OBJECT_0 + 1 && !paused_) {
                // The device asked for more audio.
                if (FAILED(fillDevice())) {
                    onDeviceLost();
                    continue;
                }
            } else if (woke == WAIT_TIMEOUT && FAILED(device_.checkAlive())) {
                // A dead device may never signal again, so a quiet period triggers a check.
                onDeviceLost();
                continue;
            }
            publishPosition();
        } else if (!paused_ && lingerState_.load() == kActive &&
                   (retryNow_ || GetTickCount() - lastRetryTick_ >= kRetryIntervalMs)) {
            tryReopen();
        }

        handleLinger();
    }
}

// Opens the device, primes its buffer so playback starts cleanly, and starts it.
HRESULT RenderEngine::openAndStart() {
    HRESULT hr = device_.open(enumerator_, params_.settings, params_.format);
    if (SUCCEEDED(hr)) hr = fillDevice();
    if (SUCCEEDED(hr) && !paused_) hr = device_.start();
    if (FAILED(hr)) {
        device_.close();
        return hr;
    }
    latencyMs_.store(device_.latencyMs());
    setStatusText(device_.description());
    return S_OK;
}

HRESULT RenderEngine::fillDevice() {
    float gains[8];
    const bool useGains = params_.settings.volume &&
                          ComputeGains(volume_.load(), pan_.load(), params_.format.channels, gains);
    return device_.fillAvailable(*params_.ring, tracker_, useGains ? gains : NULL, fader_);
}

// Brings the device in line with what Winamp most recently asked for.
// Returns true when the render thread should exit.
bool RenderEngine::reconcile() {
    const bool quitting = quit_.load();
    const bool wantPaused = control_.wantPaused();
    const bool flushPending = control_.latestFlush() != seenFlush_;

    // Resuming needs no fade-out first; it fades in instead.
    if (!quitting && !flushPending && !wantPaused) {
        if (settling_) {
            // The pause was cancelled while fading out, so fade back up from here.
            settling_ = false;
            fader_.fadeTo(1.f, fadeFrames_);
        }
        if (paused_) resumeDevice();
        return false;
    }
    if (!quitting && !flushPending && paused_) return false;  // Already paused; nothing to do

    // Something needs the current sound to stop. If it is audible, fade it out first.
    const bool audible = device_.isOpen() && !paused_;
    const bool fadeAllowed = !quitting || fadeOnQuit_.load();
    if (audible && fadeAllowed) {
        if (!settling_) {
            settling_ = true;
            settleStartTick_ = GetTickCount();
            fader_.fadeTo(0.f, fadeFrames_);
        }
        const DWORD limit = (DWORD)kFadeMs + (DWORD)device_.latencyMs() + kSettleSlackMs;
        const bool timedOut = GetTickCount() - settleStartTick_ >= limit;
        if (!(fader_.isSilent() && heardAllAudio()) && !timedOut) return false;  // Still fading
    }
    settling_ = false;

    if (quitting) return true;
    settle(flushPending, wantPaused);
    return false;
}

// Applies a pause and/or seek once the sound is silent.
void RenderEngine::settle(bool flushPending, bool wantPaused) {
    // Clear the device's buffer so stale audio cannot play later. Clearing also resets
    // the device clock, so the position count restarts from what was actually heard.
    if (device_.isOpen() && !paused_) {
        const long long heard = heardFrames();
        device_.stop();
        device_.reset();
        tracker_.restartAt(heard);
        played_.store(heard);
        submitted_.store(tracker_.submittedFrames());
    }

    if (flushPending) {
        // Drop the old audio and restart the song position at zero.
        // (GetOutputTime in plugin.cpp adds the seek target to this position.)
        params_.ring->applyDiscard();
        tracker_.restartAt(0);
        played_.store(0);
        submitted_.store(0);
        seenFlush_ = control_.latestFlush();
        control_.markFlushCompleted(seenFlush_);
        SetEvent(flushAck_);
    }

    paused_ = wantPaused;
    if (paused_) fader_.setLevel(0.f);  // Resume will fade in from silence
    else if (device_.isOpen()) startWithFadeIn();
}

// Primes the device with audio that rises from silence, then starts it.
void RenderEngine::startWithFadeIn() {
    fader_.setLevel(0.f);
    fader_.fadeTo(1.f, fadeFrames_);
    HRESULT hr = fillDevice();
    if (SUCCEEDED(hr)) hr = device_.start();
    if (FAILED(hr)) onDeviceLost();
}

void RenderEngine::resumeDevice() {
    paused_ = false;

    // A device that vanished during the pause is first noticed here.
    if (device_.isOpen()) startWithFadeIn();

    // Reconnect right away instead of waiting for the next retry interval.
    if (!device_.isOpen()) retryNow_ = true;
}

// True once every frame of real audio given to the device has been played.
bool RenderEngine::heardAllAudio() {
    long long deviceFrames = 0;
    if (FAILED(device_.readDeviceFrames(&deviceFrames))) return true;  // A dead device plays nothing
    return tracker_.playedFrames(deviceFrames) >= tracker_.submittedFrames();
}

// The song position actually heard so far.
long long RenderEngine::heardFrames() {
    long long deviceFrames = 0;
    if (FAILED(device_.readDeviceFrames(&deviceFrames))) return played_.load();
    return tracker_.playedFrames(deviceFrames);
}

// Reacts to the flags raised by DeviceMonitor.
void RenderEngine::handleDeviceEvents() {
    if (!monitor_) return;

    if (monitor_->consumeDevicesChanged()) {
        if (!device_.isOpen()) retryNow_ = true;
        else if (FAILED(device_.checkAlive())) onDeviceLost();
    }

    // A default-device change stays pending during a pause and is handled on resume.
    if (!paused_ && monitor_->consumeDefaultChanged() && params_.settings.deviceId.empty()) {
        if (device_.isOpen() && !device_.isSystemDefault(enumerator_)) onDeviceLost();
        if (!device_.isOpen()) retryNow_ = true;
    }
}

// Ends a gapless linger when the last audio has played and no next track has arrived.
void RenderEngine::handleLinger() {
    if (lingerState_.load() != kLingering) return;

    const bool finished = !device_.isOpen() ||
                          (params_.ring->pendingBytes() == 0 && heardAllAudio() &&
                           GetTickCount() - lingerStartTick_.load() >= kLingerGraceMs);
    if (!finished) return;

    int expected = kLingering;
    if (lingerState_.compare_exchange_strong(expected, kExpired)) {
        device_.close();  // Release the device, so exclusive mode lets other apps play
        setStatusText(L"Not playing");
    }
}

// Closes the device but keeps the song position, so playback can continue later.
void RenderEngine::onDeviceLost() {
    tracker_.restartAt(played_.load());
    device_.close();
    settling_ = false;
    lastRetryTick_ = GetTickCount();
    setStatusText(L"Device lost - waiting for it to return...");
}

void RenderEngine::tryReopen() {
    retryNow_ = false;
    lastRetryTick_ = GetTickCount();

    // Audio returns mid-song, so ease it in rather than starting abruptly.
    fader_.setLevel(0.f);
    fader_.fadeTo(1.f, fadeFrames_);
    openAndStart();  // On failure the loop tries again after the retry interval.
}

// Shares the current song position with Winamp's threads.
void RenderEngine::publishPosition() {
    // A seek is waiting to be applied, so the current numbers are out of date.
    if (control_.latestFlush() != seenFlush_) return;

    // A dead device cannot report its clock. Keep the last good position.
    long long deviceFrames = 0;
    if (FAILED(device_.readDeviceFrames(&deviceFrames))) return;

    played_.store(tracker_.playedFrames(deviceFrames));
    submitted_.store(tracker_.submittedFrames());
}
