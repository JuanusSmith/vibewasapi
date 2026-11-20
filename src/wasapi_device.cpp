// wasapi_device.cpp - Implements the device connection described in wasapi_device.h.
#include "wasapi_device.h"

#include <mmreg.h>
#include <cstdio>
#include <cstring>

// Some older Windows SDK and MinGW headers do not define these values.
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
#ifndef AUDCLNT_E_RESOURCES_INVALIDATED
#define AUDCLNT_E_RESOURCES_INVALIDATED AUDCLNT_ERR(0x026)
#endif
#ifndef AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
#define AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED AUDCLNT_ERR(0x019)
#endif
#ifndef E_NOTFOUND
#define E_NOTFOUND HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#endif

namespace {

// GUIDs are defined here so the project does not depend on extra import libraries.
const GUID kSubtypePCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID kIID_IAudioClock = {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};

template <class T>
void SafeRelease(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = NULL;
    }
}

// Tells Windows which speaker each channel feeds.
DWORD ChannelMask(int channels) {
    switch (channels) {
    case 1: return 0x4;    // Front centre
    case 2: return 0x3;    // Front left, front right
    case 3: return 0x7;    // Front left, front right, front centre
    case 4: return 0x33;   // Front left/right, back left/right
    case 5: return 0x37;   // Front left/right/centre, back left/right
    case 6: return 0x3F;   // 5.1
    case 7: return 0x13F;  // 6.1
    case 8: return 0x63F;  // 7.1
    }
    return 0;
}

// Builds the Windows description of an audio format.
void MakeWaveFormat(WAVEFORMATEXTENSIBLE& wave, const StreamFormat& format, const OutFormat& out) {
    memset(&wave, 0, sizeof(wave));
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = (WORD)format.channels;
    wave.Format.nSamplesPerSec = (DWORD)format.sampleRate;
    wave.Format.wBitsPerSample = (WORD)out.container;
    wave.Format.nBlockAlign = (WORD)(format.channels * out.container / 8);
    wave.Format.nAvgBytesPerSec = wave.Format.nSamplesPerSec * wave.Format.nBlockAlign;
    wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = (WORD)out.valid;
    wave.dwChannelMask = ChannelMask(format.channels);
    wave.SubFormat = out.isFloat ? kSubtypeFloat : kSubtypePCM;
}

// Works around a crash inside Windows itself.
//
// Windows loads audiokse.dll into our process to run exclusive-mode streams, and
// unloads it again when the last exclusive stream closes. That DLL starts a worker
// thread which can still be running when the DLL is unloaded; the thread then
// executes memory that no longer holds code, and the whole player crashes. Opening
// and closing exclusive streams quickly (fast track changes, seeks) makes this likely.
// It happens with every version of this plug-in and was seen on a Bluetooth device.
//
// "Pinning" the DLL tells Windows never to unload it from this process, so the
// thread always has valid code to run. The cost is a few hundred KB of memory that
// stays in use until the player exits. It only needs doing once.
void PinExclusiveModeDriver() {
    static volatile LONG pinned = 0;
    if (pinned) return;
    HMODULE module = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"audiokse.dll", &module))
        InterlockedExchange(&pinned, 1);
    // If the DLL is not loaded (other Windows versions or drivers), there is nothing to pin.
}

}  // namespace

HRESULT CreateDeviceEnumerator(IMMDeviceEnumerator** enumerator) {
    return CoCreateInstance(kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, kIID_IMMDeviceEnumerator,
                            (void**)enumerator);
}

bool IsTransientOpenError(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_DEVICE_IN_USE ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING || hr == AUDCLNT_E_ENDPOINT_CREATE_FAILED ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED || hr == E_NOTFOUND;
}

HRESULT WasapiDevice::activateClient() {
    return device_->Activate(kIID_IAudioClient, CLSCTX_ALL, NULL, (void**)&client_);
}

// Exclusive mode talks directly to the hardware, so the hardware must accept our
// format exactly. We try the formats closest to the source first.
HRESULT WasapiDevice::initializeExclusive() {
    static const OutFormat from16[] = {{16, 16, false}, {24, 24, false}, {32, 24, false}, {32, 32, false}, {32, 32, true}};
    static const OutFormat from24[] = {{24, 24, false}, {32, 24, false}, {32, 32, false}, {32, 32, true}, {16, 16, false}};
    static const OutFormat from32[] = {{32, 32, false}, {32, 32, true}, {32, 24, false}, {24, 24, false}, {16, 16, false}};
    const OutFormat* candidates = format_.bitsPerSample == 24 ? from24 : format_.bitsPerSample == 32 ? from32 : from16;

    WAVEFORMATEXTENSIBLE wave;
    bool found = false;
    for (int i = 0; i < 5 && !found; ++i) {
        MakeWaveFormat(wave, format_, candidates[i]);
        if (client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&wave, NULL) == S_OK) {
            outFormat_ = candidates[i];
            found = true;
        }
    }
    if (!found) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    // WASAPI times are in units of 100 nanoseconds, so 1 ms = 10,000 units.
    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    client_->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    REFERENCE_TIME period = settings_.periodMs > 0 ? (REFERENCE_TIME)settings_.periodMs * 10000 : defaultPeriod;
    if (period < minimumPeriod) period = minimumPeriod;

    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     period, period, (WAVEFORMATEX*)&wave, NULL);

    // Some hardware needs a buffer of a particular size. Windows then reports the size
    // it wants, and the documented recovery is to start again with that size.
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 alignedFrames = 0;
        client_->GetBufferSize(&alignedFrames);
        SafeRelease(client_);
        period = (REFERENCE_TIME)(10000.0 * 1000.0 * alignedFrames / format_.sampleRate + 0.5);
        hr = activateClient();
        if (SUCCEEDED(hr)) {
            hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     period, period, (WAVEFORMATEX*)&wave, NULL);
        }
    }
    if (SUCCEEDED(hr)) {
        exclusive_ = true;
        PinExclusiveModeDriver();  // audiokse.dll is loaded now; keep it loaded
    }
    return hr;
}

// Shared mode goes through the Windows mixer, which accepts float audio at any
// sample rate and converts it for the hardware.
HRESULT WasapiDevice::initializeShared() {
    outFormat_ = {32, 32, true};
    WAVEFORMATEXTENSIBLE wave;
    MakeWaveFormat(wave, format_, outFormat_);

    const REFERENCE_TIME duration = (REFERENCE_TIME)(settings_.periodMs > 0 ? settings_.periodMs : 50) * 10000;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, (WAVEFORMATEX*)&wave, NULL);
    if (SUCCEEDED(hr)) exclusive_ = false;
    return hr;
}

HRESULT WasapiDevice::open(IMMDeviceEnumerator* enumerator, const Settings& settings, const StreamFormat& format) {
    close();
    settings_ = settings;
    format_ = format;

    // Run the setup steps in order, stopping at the first failure.
    HRESULT hr = S_OK;
    if (!settings_.deviceId.empty()) enumerator->GetDevice(settings_.deviceId.c_str(), &device_);

    // No chosen device, or it no longer exists in Windows: use the system default.
    if (!device_) hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device_);

    if (SUCCEEDED(hr)) {
        LPWSTR id = NULL;
        if (SUCCEEDED(device_->GetId(&id))) {
            deviceId_ = id;
            CoTaskMemFree(id);
        }
        hr = activateClient();
    }

    if (SUCCEEDED(hr)) {
        if (settings_.exclusive) {
            hr = initializeExclusive();
            if (FAILED(hr) && settings_.fallback) {
                // A client can only be initialized once, so shared mode needs a new one.
                SafeRelease(client_);
                hr = activateClient();
                if (SUCCEEDED(hr)) hr = initializeShared();
            }
        } else {
            hr = initializeShared();
        }
    }

    if (SUCCEEDED(hr)) {
        audioEvent_ = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!audioEvent_) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) hr = client_->SetEventHandle(audioEvent_);
    if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&bufferFrames_);
    if (SUCCEEDED(hr)) hr = client_->GetService(kIID_IAudioRenderClient, (void**)&render_);
    if (SUCCEEDED(hr)) hr = client_->GetService(kIID_IAudioClock, (void**)&clock_);
    if (SUCCEEDED(hr)) hr = clock_->GetFrequency(&clockFrequency_);

    if (FAILED(hr)) {
        close();
        return hr;
    }

    REFERENCE_TIME streamLatency = 0;
    client_->GetStreamLatency(&streamLatency);
    latencyMs_ = (int)((long long)bufferFrames_ * 1000 / format_.sampleRate + streamLatency / 10000);
    scratch_.resize((size_t)bufferFrames_ * format_.frameBytes());
    buildDescription();
    return S_OK;
}

void WasapiDevice::close() {
    if (client_) client_->Stop();
    SafeRelease(clock_);
    SafeRelease(render_);
    SafeRelease(client_);
    SafeRelease(device_);
    if (audioEvent_) {
        CloseHandle(audioEvent_);
        audioEvent_ = NULL;
    }
    bufferFrames_ = 0;
    clockFrequency_ = 0;
    deviceId_.clear();
}

HRESULT WasapiDevice::start() {
    if (!client_) return E_FAIL;
    const HRESULT hr = client_->Start();
    return hr == AUDCLNT_E_NOT_STOPPED ? S_OK : hr;  // Already running is not a problem.
}

void WasapiDevice::stop() {
    if (client_) client_->Stop();
}

void WasapiDevice::reset() {
    if (client_) client_->Reset();
}

HRESULT WasapiDevice::fill(UINT32 frames, SpscRing& ring, PositionTracker& tracker, const float* gains, Fader& fader) {
    if (!frames) return S_OK;

    BYTE* deviceBuffer = NULL;
    HRESULT hr = render_->GetBuffer(frames, &deviceBuffer);
    if (FAILED(hr)) return hr;

    // During a fade-out, take only as much audio as the fade still needs.
    size_t wantedFrames = frames;
    const long long audibleLeft = fader.audibleFramesLeft();
    if (audibleLeft >= 0 && (long long)wantedFrames > audibleLeft) wantedFrames = (size_t)audibleLeft;

    const size_t bytesRead = wantedFrames ? ring.read(scratch_.data(), wantedFrames * format_.frameBytes()) : 0;
    const size_t audioFrames = bytesRead / format_.frameBytes();
    const size_t outFrameBytes = (size_t)format_.channels * outFormat_.container / 8;

    if (audioFrames) {
        // A steady full level means no fade, which keeps the bit-perfect path.
        GainRamp ramp;
        const bool fading = !fader.isUnity();
        if (fading) ramp = fader.take((long long)audioFrames);
        ConvertFrames(scratch_.data(), deviceBuffer, audioFrames, format_.bitsPerSample,
                      format_.channels, outFormat_, gains, fading ? &ramp : NULL);
    }
    if (audioFrames < frames) {
        memset(deviceBuffer + audioFrames * outFrameBytes, 0, (frames - audioFrames) * outFrameBytes);
    }

    hr = render_->ReleaseBuffer(frames, 0);
    tracker.onBufferFilled((long long)audioFrames, (long long)(frames - audioFrames));
    return hr;
}

HRESULT WasapiDevice::fillAvailable(SpscRing& ring, PositionTracker& tracker, const float* gains, Fader& fader) {
    // In exclusive event-driven mode, Windows hands us the entire buffer on every event.
    // In shared mode, part of the buffer may still be waiting to play.
    UINT32 queuedFrames = 0;
    if (!exclusive_) {
        const HRESULT hr = client_->GetCurrentPadding(&queuedFrames);
        if (FAILED(hr)) return hr;
    }
    return fill(bufferFrames_ - queuedFrames, ring, tracker, gains, fader);
}

HRESULT WasapiDevice::checkAlive() {
    if (!client_) return E_FAIL;
    UINT32 queuedFrames = 0;
    return client_->GetCurrentPadding(&queuedFrames);
}

HRESULT WasapiDevice::readDeviceFrames(long long* frames) {
    if (!clock_ || !clockFrequency_) return E_FAIL;
    UINT64 position = 0;
    const HRESULT hr = clock_->GetPosition(&position, NULL);
    if (FAILED(hr)) return hr;
    *frames = (long long)((double)position * format_.sampleRate / (double)clockFrequency_);
    return S_OK;
}

bool WasapiDevice::isSystemDefault(IMMDeviceEnumerator* enumerator) const {
    IMMDevice* defaultDevice = NULL;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice))) return false;

    bool same = false;
    LPWSTR id = NULL;
    if (SUCCEEDED(defaultDevice->GetId(&id))) {
        same = deviceId_ == id;
        CoTaskMemFree(id);
    }
    defaultDevice->Release();
    return same;
}

// Builds the text shown in the settings window.
void WasapiDevice::buildDescription() {
    wchar_t sampleText[48];
    if (outFormat_.isFloat) _snwprintf(sampleText, 47, L"32-bit float");
    else if (outFormat_.container != outFormat_.valid) _snwprintf(sampleText, 47, L"%d-bit in %d", outFormat_.valid, outFormat_.container);
    else _snwprintf(sampleText, 47, L"%d-bit", outFormat_.valid);
    sampleText[47] = 0;

    wchar_t text[160];
    _snwprintf(text, 159, L"Now: %s, %s, %d Hz, %d ch, %u-frame buffer (%d ms)",
               exclusive_ ? L"exclusive" : L"shared", sampleText, format_.sampleRate,
               format_.channels, bufferFrames_, latencyMs_);
    text[159] = 0;
    description_ = text;
}
