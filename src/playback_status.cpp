// playback_status.cpp - Implements the descriptions in playback_status.h.
#include "playback_status.h"

#include <audioclient.h>
#include <cstdarg>
#include <cstdio>

// Some older Windows SDK and MinGW headers do not define these values.
#ifndef AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED
#define AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED AUDCLNT_ERR(0x00e)
#endif
#ifndef E_NOTFOUND
#define E_NOTFOUND HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#endif

namespace {

std::wstring Format(const wchar_t* pattern, ...) {
    wchar_t text[512];
    va_list args;
    va_start(args, pattern);
    _vsnwprintf(text, 511, pattern, args);
    va_end(args);
    text[511] = 0;
    return text;
}

std::wstring DescribeChannels(int channels) {
    if (channels == 1) return L"mono";
    if (channels == 2) return L"stereo";
    return Format(L"%d channels", channels);
}

// The audio is bit-perfect when every sample reaches the device unchanged.
bool IsBitPerfect(const PlaybackStatus& status) {
    if (!status.exclusive || status.softwareVolume) return false;
    // Float holds 24-bit integers exactly, but not 32-bit ones.
    if (status.out.isFloat) return status.sourceBits <= 24;
    return status.sourceBits <= status.out.valid;
}

}  // namespace

std::wstring DescribeRate(int sampleRate) {
    if (sampleRate % 1000 == 0) return Format(L"%d kHz", sampleRate / 1000);
    return Format(L"%d.%d kHz", sampleRate / 1000, (sampleRate % 1000) / 100);
}

std::wstring DescribeSampleFormat(const OutFormat& format) {
    if (format.isFloat) return L"32-bit float";
    if (format.container != format.valid) return Format(L"%d-bit in %d", format.valid, format.container);
    return Format(L"%d-bit", format.valid);
}

std::wstring DescribeOpenError(HRESULT hr, int sampleRate, int channels) {
    switch (hr) {
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return Format(L"the device doesn't accept %s %s audio in exclusive mode.",
                      DescribeRate(sampleRate).c_str(), DescribeChannels(channels).c_str());
    case AUDCLNT_E_DEVICE_IN_USE:
        return L"another app is using the device in exclusive mode.";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        return L"Windows sound settings don't allow exclusive mode for this device.";
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case E_NOTFOUND:
        return L"the device was disconnected.";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return L"the Windows audio service isn't running.";
    }
    return Format(L"Windows reported error 0x%08lX.", (unsigned long)hr);
}

std::wstring DescribePlayback(const PlaybackStatus& status) {
    const std::wstring name = status.deviceName.empty() ? std::wstring(L"Output device") : status.deviceName;
    std::wstring text = Format(L"%s: %s mode, %s, %s, %s, %d ms latency.", name.c_str(),
                               status.exclusive ? L"exclusive" : L"shared",
                               DescribeRate(status.sampleRate).c_str(),
                               DescribeSampleFormat(status.out).c_str(),
                               DescribeChannels(status.channels).c_str(), status.latencyMs);
    if (status.paused) text += L" Paused.";

    // The second line says what happens to the sound on the way.
    text += L"\n";
    const bool dither = NeedsDither(status.sourceBits, status.out, status.softwareVolume);
    if (IsBitPerfect(status)) {
        text += L"Bit-perfect: the audio reaches the device unchanged.";
    } else if (!status.exclusive) {
        text += L"Windows mixes the audio with other apps and converts it for the device.";
        if (status.softwareVolume) text += L" Volume is applied in software.";
    } else if (!status.out.isFloat && status.sourceBits > status.out.valid) {
        text += Format(L"The %d-bit source is reduced to %d-bit, with dither.", status.sourceBits, status.out.valid);
        if (status.softwareVolume) text += L" Volume is applied in software.";
    } else if (status.softwareVolume) {
        text += dither ? L"Not bit-perfect: volume is applied in software, with dither."
                       : L"Not bit-perfect: volume is applied in software.";
    } else {
        text += L"Not bit-perfect: the 32-bit source is converted to 32-bit float.";
    }

    if (status.exclusiveError != S_OK) {
        text += L"\nExclusive mode wasn't used: ";
        text += DescribeOpenError(status.exclusiveError, status.sampleRate, status.channels);
    }
    return text;
}

std::wstring DescribeExclusiveFormats(HRESULT probeResult, const std::vector<RateSupport>& rates) {
    if (probeResult == AUDCLNT_E_DEVICE_IN_USE) {
        return L"The device is in exclusive use right now, possibly by this player, so it can't "
               L"be checked. Stop playback and open this window again.";
    }
    if (probeResult == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
        return L"Windows doesn't allow exclusive mode for this device. To allow it, open Sound "
               L"settings, choose the device, and turn on \"Allow applications to take exclusive "
               L"control of this device\".";
    }
    if (FAILED(probeResult)) {
        return L"This device couldn't be checked: " + DescribeOpenError(probeResult, 0, 2);
    }

    std::wstring text;
    bool any = false;
    for (size_t i = 0; i < rates.size(); ++i) {
        if (i) text += L"\r\n";  // The format list is an edit box, which needs \r\n line breaks
        text += DescribeRate(rates[i].sampleRate) + L": ";
        if (rates[i].formats.empty()) {
            text += L"not accepted";
        } else {
            any = true;
            for (size_t f = 0; f < rates[i].formats.size(); ++f) {
                if (f) text += L", ";
                text += DescribeSampleFormat(rates[i].formats[f]);
            }
        }
    }
    if (!any) text += L"\r\nNo common format works in exclusive mode; use shared mode with this device.";
    return text;
}
