// playback_status.h - Turns the facts about the current stream into plain-language
// text for the settings window.
//
// It answers the questions a listener actually has: which device is playing, is it
// exclusive or shared, is the sound bit-perfect, and if exclusive mode was asked for
// but not used, why not. These are pure functions (no Windows audio calls), so the
// unit tests can check every message.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "sample_convert.h"

// What the stream looks like once the device is open.
struct PlaybackStatus {
    std::wstring deviceName;           // For example "Headphones (U18)"
    bool exclusive = false;            // Exclusive mode is in use
    HRESULT exclusiveError = S_OK;     // Why exclusive mode failed before falling back, else S_OK
    int sampleRate = 44100;            // The source audio from Winamp
    int channels = 2;
    int sourceBits = 16;
    OutFormat out = {32, 32, true};    // What is sent to the device
    int latencyMs = 0;
    bool softwareVolume = false;       // Volume or pan is being applied right now
    bool paused = false;               // The listener has paused playback
};

// "44.1 kHz", "48 kHz", "176.4 kHz".
std::wstring DescribeRate(int sampleRate);

// "16-bit", "24-bit in 32", "32-bit float".
std::wstring DescribeSampleFormat(const OutFormat& format);

// Explains an error from opening a device, for example "another app is using the
// device in exclusive mode". `sampleRate` and `channels` describe the audio that was
// being opened; they are mentioned when the device rejects that format.
std::wstring DescribeOpenError(HRESULT hr, int sampleRate, int channels);

// Two or three lines describing the open stream.
std::wstring DescribePlayback(const PlaybackStatus& status);

// The exclusive-mode formats a device accepts at one sample rate (see
// ProbeExclusiveFormats in wasapi_device.h). An empty list means none.
struct RateSupport {
    int sampleRate = 0;
    std::vector<OutFormat> formats;
};

// One line per sample rate, or an explanation if the device could not be checked.
// `probeResult` is what ProbeExclusiveFormats returned.
std::wstring DescribeExclusiveFormats(HRESULT probeResult, const std::vector<RateSupport>& rates);
