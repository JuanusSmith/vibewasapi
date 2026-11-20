// wasapi_device.h - One open connection to a Windows audio output device.
//
// WASAPI is the Windows audio API. This class hides its setup details: choosing the
// endpoint, negotiating a sample format, and moving audio into the device's buffer.
// It makes no decisions about *when* to open or reopen; render_engine.cpp does that.
//
// The file has two parts:
//   * Free functions that ask Windows about devices (names, supported formats). The
//     settings window uses these from its own thread; they keep no shared state.
//   * The WasapiDevice class, whose methods must only be called on the render thread.
#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string>
#include <vector>

#include "fader.h"
#include "playback_status.h"
#include "position_tracker.h"
#include "sample_convert.h"
#include "settings.h"
#include "spsc_ring.h"

// The format of the audio Winamp is sending us.
struct StreamFormat {
    int sampleRate = 44100;
    int channels = 2;
    int bitsPerSample = 16;
    int frameBytes() const { return channels * bitsPerSample / 8; }

    bool operator==(const StreamFormat& other) const {
        return sampleRate == other.sampleRate && channels == other.channels &&
               bitsPerSample == other.bitsPerSample;
    }
    bool operator!=(const StreamFormat& other) const { return !(*this == other); }
};

// Creates the Windows object used to find audio devices.
HRESULT CreateDeviceEnumerator(IMMDeviceEnumerator** enumerator);

// True for errors that often clear by themselves, such as a device that another
// stream released a moment ago.
bool IsTransientOpenError(HRESULT hr);

// The name Windows shows for a device, such as "Speakers (USB Audio)", or "" if unknown.
std::wstring DeviceFriendlyName(IMMDevice* device);

// The sample rates the settings window checks, from CD quality up to 192 kHz.
extern const int kProbeRates[];
extern const int kProbeRateCount;

// Asks a device which formats it accepts in exclusive mode at each of kProbeRates,
// with `channels` channels. An empty `deviceId` means the Windows default device.
// Nothing is played. Returns an error if the device can't be checked at all, for
// example because it is in exclusive use right now.
HRESULT ProbeExclusiveFormats(IMMDeviceEnumerator* enumerator, const std::wstring& deviceId,
                              int channels, std::vector<RateSupport>& rates);

class WasapiDevice {
public:
    ~WasapiDevice() { close(); }

    // Connects to the configured device. On failure the object is left closed.
    HRESULT open(IMMDeviceEnumerator* enumerator, const Settings& settings, const StreamFormat& format);

    // Releases the device. Safe to call when already closed.
    void close();

    bool isOpen() const { return client_ != NULL; }

    // Windows signals this event whenever the device is ready for more audio.
    HANDLE audioEvent() const { return audioEvent_; }

    int latencyMs() const { return latencyMs_; }

    // Facts about the open stream for the settings window (see playback_status.h).
    // softwareVolume is left false; the engine fills it in, since volume changes live.
    const PlaybackStatus& status() const { return status_; }

    HRESULT start();
    void stop();
    void reset();

    // Fills all free space in the device buffer from the ring, padding with silence
    // if the ring runs short. `gains` may be NULL for bit-perfect output. `fader`
    // applies any fade in progress; once it has faded to silence, no more audio is
    // taken from the ring, so that audio is still there when playback resumes.
    HRESULT fillAvailable(SpscRing& ring, PositionTracker& tracker, const float* gains, Fader& fader);

    // Asks the device a harmless question. A failure means the device has gone away.
    HRESULT checkAlive();

    // Reads the device clock, expressed in frames of the source audio.
    HRESULT readDeviceFrames(long long* frames);

    // True if this device is still the Windows default output device.
    bool isSystemDefault(IMMDeviceEnumerator* enumerator) const;

private:
    HRESULT activateClient();
    HRESULT initializeExclusive();
    HRESULT initializeShared();
    HRESULT fill(UINT32 frames, SpscRing& ring, PositionTracker& tracker, const float* gains, Fader& fader);
    void buildStatus(HRESULT exclusiveError);

    IMMDevice* device_ = NULL;
    IAudioClient* client_ = NULL;
    IAudioRenderClient* render_ = NULL;
    IAudioClock* clock_ = NULL;
    HANDLE audioEvent_ = NULL;

    Settings settings_;
    StreamFormat format_;
    OutFormat outFormat_ = {32, 32, true};
    std::wstring deviceId_;
    PlaybackStatus status_;
    DitherState dither_;                  // Kept across buffers so the noise stays random
    std::vector<unsigned char> scratch_;  // Holds source audio while it is converted
    UINT32 bufferFrames_ = 0;
    UINT64 clockFrequency_ = 0;
    int latencyMs_ = 0;
    bool exclusive_ = false;
};
