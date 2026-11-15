// Vibe Wasapi (out_vibewasapi) - WASAPI output plug-in for Winamp / WACUP
//
// * Exclusive mode (event-driven, bit-perfect when volume = 100% and pan = 0)
//   with automatic format negotiation (16 / 24 / 24-in-32 / 32 / float).
// * Shared mode with Windows' built-in resampler (AUTOCONVERTPCM).
// * Optional fallback from exclusive to shared.
// * Device selection, buffer / period settings, software volume + pan.
// * Recovers from device removal / format change by reopening the endpoint.
//
// All IAudioClient calls happen on one MTA render thread; Winamp's threads
// only touch the ring buffer and send synchronous commands (pause / flush).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <atomic>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "out.h"
#include "resource.h"

#define PLUGIN_VERSION "0.1"
#define INI_SECTION L"out_vibewasapi"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
#ifndef AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
#define AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED AUDCLNT_ERR(0x019)
#endif

// GUIDs defined locally so we don't depend on uuid/ksuser import libs.
static const GUID kSubtypePCM   = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const GUID kSubtypeFloat = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const IID kIID_IMMDeviceEnumerator   = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const IID kIID_IAudioClient          = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const IID kIID_IAudioRenderClient    = {0xF294ACFC,0x3146,0x4483,{0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};
static const IID kIID_IAudioClock           = {0xCD63314F,0x3FBA,0x4A1B,{0x81,0x2C,0xEF,0x96,0x35,0x87,0x28,0xE7}};
static const PROPERTYKEY kPKEY_FriendlyName = {{0xA45C254E,0xDF1C,0x4EFD,{0x80,0x20,0x67,0xD1,0x46,0xA8,0x50,0xE0}},14};

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = NULL; } }

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct Settings {
    std::wstring deviceId;  // empty = system default
    int exclusive = 1;
    int fallback  = 1;
    int volume    = 1;      // apply Winamp volume/pan in software
    int ringMs    = 1000;   // our buffer
    int periodMs  = 0;      // device period / shared buffer, 0 = default
};

static Settings g_cfg;      // edited by the config dialog
static Settings g_act;      // snapshot used by the open stream
static std::wstring g_iniPath;

static void LoadSettings() {
    if (g_iniPath.empty()) return;
    const wchar_t* ini = g_iniPath.c_str();
    wchar_t buf[512] = {0};
    GetPrivateProfileStringW(INI_SECTION, L"device", L"", buf, 512, ini);
    g_cfg.deviceId  = buf;
    g_cfg.exclusive = GetPrivateProfileIntW(INI_SECTION, L"exclusive", 1, ini) != 0;
    g_cfg.fallback  = GetPrivateProfileIntW(INI_SECTION, L"fallback", 1, ini) != 0;
    g_cfg.volume    = GetPrivateProfileIntW(INI_SECTION, L"volume", 1, ini) != 0;
    g_cfg.ringMs    = GetPrivateProfileIntW(INI_SECTION, L"buffer_ms", 1000, ini);
    g_cfg.periodMs  = GetPrivateProfileIntW(INI_SECTION, L"period_ms", 0, ini);
    if (g_cfg.ringMs < 100) g_cfg.ringMs = 100;
    if (g_cfg.ringMs > 10000) g_cfg.ringMs = 10000;
    if (g_cfg.periodMs < 0) g_cfg.periodMs = 0;
    if (g_cfg.periodMs > 500) g_cfg.periodMs = 500;
}

static void WriteInt(const wchar_t* key, int v) {
    wchar_t b[32]; _snwprintf(b, 31, L"%d", v); b[31] = 0;
    WritePrivateProfileStringW(INI_SECTION, key, b, g_iniPath.c_str());
}

static void SaveSettings() {
    if (g_iniPath.empty()) return;
    WritePrivateProfileStringW(INI_SECTION, L"device", g_cfg.deviceId.c_str(), g_iniPath.c_str());
    WriteInt(L"exclusive", g_cfg.exclusive);
    WriteInt(L"fallback", g_cfg.fallback);
    WriteInt(L"volume", g_cfg.volume);
    WriteInt(L"buffer_ms", g_cfg.ringMs);
    WriteInt(L"period_ms", g_cfg.periodMs);
}

// ---------------------------------------------------------------------------
// Ring buffer (input-format bytes)
// ---------------------------------------------------------------------------
struct Ring {
    std::vector<unsigned char> b;
    size_t rd = 0, used = 0;
    void alloc(size_t n) { b.assign(n ? n : 1, 0); rd = used = 0; }
    void clear() { rd = used = 0; }
    size_t cap() const { return b.size(); }
    size_t freeBytes() const { return b.size() - used; }
    size_t write(const unsigned char* p, size_t n) {
        if (n > freeBytes()) n = freeBytes();
        size_t wr = (rd + used) % cap();
        size_t first = n < cap() - wr ? n : cap() - wr;
        memcpy(&b[wr], p, first);
        if (n > first) memcpy(&b[0], p + first, n - first);
        used += n;
        return n;
    }
    size_t read(unsigned char* p, size_t n) {
        if (n > used) n = used;
        size_t first = n < cap() - rd ? n : cap() - rd;
        memcpy(p, &b[rd], first);
        if (n > first) memcpy(p + first, &b[0], n - first);
        rd = (rd + n) % cap();
        used -= n;
        return n;
    }
};

// ---------------------------------------------------------------------------
// Sample conversion
// ---------------------------------------------------------------------------
struct OutFormat { int container; int valid; bool isFloat; };

static inline int32_t ReadSample(const unsigned char* p, int bps) {
    switch (bps) {
    case 8:  return (int32_t)(((uint32_t)p[0] ^ 0x80u) << 24);
    case 16: return (int32_t)(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 24));
    case 24: return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24));
    default: return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    }
}

static inline void WriteIntSample(unsigned char* p, int32_t s, const OutFormat& of) {
    uint32_t u = (uint32_t)s;
    switch (of.container) {
    case 16: p[0] = (unsigned char)(u >> 16); p[1] = (unsigned char)(u >> 24); break;
    case 24: p[0] = (unsigned char)(u >> 8);  p[1] = (unsigned char)(u >> 16); p[2] = (unsigned char)(u >> 24); break;
    default:
        if (of.valid == 24) u &= 0xFFFFFF00u;
        p[0] = (unsigned char)u; p[1] = (unsigned char)(u >> 8);
        p[2] = (unsigned char)(u >> 16); p[3] = (unsigned char)(u >> 24);
        break;
    }
}

static void ConvertFrames(const unsigned char* in, unsigned char* out, size_t frames,
                          int inBps, int nch, const OutFormat& of, const float* gains) {
    const int inB = inBps / 8, outB = of.container / 8;
    const double kScale = 1.0 / 2147483648.0;
    for (size_t f = 0; f < frames; ++f) {
        for (int c = 0; c < nch; ++c) {
            int32_t s = ReadSample(in, inBps);
            in += inB;
            if (gains) {
                double x = (double)s * gains[c];
                if (of.isFloat) {
                    float v = (float)(x * kScale);
                    memcpy(out, &v, 4);
                } else {
                    if (x > 2147483647.0) x = 2147483647.0;
                    if (x < -2147483648.0) x = -2147483648.0;
                    WriteIntSample(out, (int32_t)lrint(x), of);
                }
            } else if (of.isFloat) {
                float v = (float)(s * kScale);
                memcpy(out, &v, 4);
            } else {
                WriteIntSample(out, s, of);  // lossless when container >= input
            }
            out += outB;
        }
    }
}

// ---------------------------------------------------------------------------
// Shared state (Winamp threads <-> render thread)
// ---------------------------------------------------------------------------
enum { CMD_NONE = 0, CMD_PAUSE, CMD_UNPAUSE, CMD_FLUSH };

static CRITICAL_SECTION g_ringCs, g_cmdCs;
static Ring g_ring;
static int g_srate = 44100, g_nch = 2, g_bps = 16, g_frameBytes = 4;
static HANDLE g_thread = NULL, g_cmdEvent = NULL, g_ackEvent = NULL, g_readyEvent = NULL;
static volatile LONG g_cmd = CMD_NONE, g_quit = 0;
static std::atomic<long long> g_played(0), g_submitted(0), g_written(0);
static std::atomic<int> g_baseMs(0), g_failed(0), g_paused(0), g_volume(255), g_pan(0);
static HRESULT g_initHr = E_FAIL;
static int g_latencyMs = 0;
static wchar_t g_modeDesc[160] = L"Not playing";

// render-thread-only state
static IMMDevice* r_dev = NULL;
static IAudioClient* r_client = NULL;
static IAudioRenderClient* r_render = NULL;
static IAudioClock* r_clock = NULL;
static HANDLE r_audioEvent = NULL;
static UINT32 r_bufFrames = 0;
static UINT64 r_clockFreq = 0;
static bool r_exclusive = false, r_paused = false;
static OutFormat r_of = {32, 32, true};
static long long r_committedSilence = 0, r_pendingSilence = 0, r_submittedReal = 0, r_frameOffset = 0;
static std::vector<unsigned char> r_scratch;
static DWORD r_lastRetry = 0;
static int r_retries = 0;

static DWORD ChannelMask(int nch) {
    switch (nch) {
    case 1: return 0x4;      // FC
    case 2: return 0x3;      // FL FR
    case 3: return 0x7;      // FL FR FC
    case 4: return 0x33;     // FL FR BL BR
    case 5: return 0x37;     // FL FR FC BL BR
    case 6: return 0x3F;     // 5.1
    case 7: return 0x13F;    // 6.1
    case 8: return 0x63F;    // 7.1
    }
    return 0;
}

static void MakeFormat(WAVEFORMATEXTENSIBLE& w, const OutFormat& of) {
    memset(&w, 0, sizeof(w));
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = (WORD)g_nch;
    w.Format.nSamplesPerSec = (DWORD)g_srate;
    w.Format.wBitsPerSample = (WORD)of.container;
    w.Format.nBlockAlign = (WORD)(g_nch * of.container / 8);
    w.Format.nAvgBytesPerSec = w.Format.nSamplesPerSec * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = (WORD)of.valid;
    w.dwChannelMask = ChannelMask(g_nch);
    w.SubFormat = of.isFloat ? kSubtypeFloat : kSubtypePCM;
}

static HRESULT ActivateClient() {
    return r_dev->Activate(kIID_IAudioClient, CLSCTX_ALL, NULL, (void**)&r_client);
}

static HRESULT InitExclusive() {
    // Candidate device formats, best first, per input bit depth.
    static const OutFormat c16[] = {{16,16,false},{24,24,false},{32,24,false},{32,32,false},{32,32,true}};
    static const OutFormat c24[] = {{24,24,false},{32,24,false},{32,32,false},{32,32,true},{16,16,false}};
    static const OutFormat c32[] = {{32,32,false},{32,32,true},{32,24,false},{24,24,false},{16,16,false}};
    const OutFormat* list = g_bps == 24 ? c24 : g_bps == 32 ? c32 : c16;

    WAVEFORMATEXTENSIBLE wf;
    bool found = false;
    for (int i = 0; i < 5; ++i) {
        MakeFormat(wf, list[i]);
        if (r_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&wf, NULL) == S_OK) {
            r_of = list[i];
            found = true;
            break;
        }
    }
    if (!found) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    REFERENCE_TIME def = 0, mn = 0;
    r_client->GetDevicePeriod(&def, &mn);
    REFERENCE_TIME period = g_act.periodMs > 0 ? (REFERENCE_TIME)g_act.periodMs * 10000 : def;
    if (period < mn) period = mn;

    HRESULT hr = r_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                      period, period, (WAVEFORMATEX*)&wf, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames = 0;
        r_client->GetBufferSize(&frames);
        SafeRelease(r_client);
        period = (REFERENCE_TIME)(10000.0 * 1000.0 * frames / g_srate + 0.5);
        hr = ActivateClient();
        if (SUCCEEDED(hr))
            hr = r_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                      period, period, (WAVEFORMATEX*)&wf, NULL);
    }
    if (SUCCEEDED(hr)) r_exclusive = true;
    return hr;
}

static HRESULT InitShared() {
    // Hand Windows float at the source rate; the engine resamples/remaps as needed.
    r_of = {32, 32, true};
    WAVEFORMATEXTENSIBLE wf;
    MakeFormat(wf, r_of);
    REFERENCE_TIME dur = (REFERENCE_TIME)(g_act.periodMs > 0 ? g_act.periodMs : 50) * 10000;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT hr = r_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, (WAVEFORMATEX*)&wf, NULL);
    if (SUCCEEDED(hr)) r_exclusive = false;
    return hr;
}

static void ReleaseDevice() {
    if (r_client) r_client->Stop();
    SafeRelease(r_clock);
    SafeRelease(r_render);
    SafeRelease(r_client);
    SafeRelease(r_dev);
    if (r_audioEvent) { CloseHandle(r_audioEvent); r_audioEvent = NULL; }
    r_bufFrames = 0;
}

static HRESULT InitDevice() {
    IMMDeviceEnumerator* en = NULL;
    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) return hr;
    if (!g_act.deviceId.empty()) en->GetDevice(g_act.deviceId.c_str(), &r_dev);
    if (!r_dev) hr = en->GetDefaultAudioEndpoint(eRender, eMultimedia, &r_dev);
    en->Release();
    if (FAILED(hr) || !r_dev) return FAILED(hr) ? hr : E_FAIL;

    hr = ActivateClient();
    if (FAILED(hr)) return hr;

    if (g_act.exclusive) {
        hr = InitExclusive();
        if (FAILED(hr) && g_act.fallback) {
            SafeRelease(r_client);
            hr = ActivateClient();
            if (SUCCEEDED(hr)) hr = InitShared();
        }
    } else {
        hr = InitShared();
    }
    if (FAILED(hr)) return hr;

    r_audioEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!r_audioEvent) return E_FAIL;
    if (FAILED(hr = r_client->SetEventHandle(r_audioEvent))) return hr;
    if (FAILED(hr = r_client->GetBufferSize(&r_bufFrames))) return hr;
    if (FAILED(hr = r_client->GetService(kIID_IAudioRenderClient, (void**)&r_render))) return hr;
    if (FAILED(hr = r_client->GetService(kIID_IAudioClock, (void**)&r_clock))) return hr;
    r_clock->GetFrequency(&r_clockFreq);

    REFERENCE_TIME lat = 0;
    r_client->GetStreamLatency(&lat);
    g_latencyMs = (int)((long long)r_bufFrames * 1000 / g_srate + lat / 10000);
    r_scratch.resize((size_t)r_bufFrames * g_frameBytes);

    wchar_t fmt[48];
    if (r_of.isFloat) _snwprintf(fmt, 47, L"32-bit float");
    else if (r_of.container != r_of.valid) _snwprintf(fmt, 47, L"%d-bit in %d", r_of.valid, r_of.container);
    else _snwprintf(fmt, 47, L"%d-bit", r_of.valid);
    fmt[47] = 0;
    _snwprintf(g_modeDesc, 159, L"Now: %s, %s, %d Hz, %d ch, %u-frame buffer (%d ms)",
               r_exclusive ? L"exclusive" : L"shared", fmt, g_srate, g_nch, r_bufFrames, g_latencyMs);
    g_modeDesc[159] = 0;
    return S_OK;
}

static bool ComputeGains(float* g) {
    if (!g_act.volume) return false;
    int v = g_volume.load(), p = g_pan.load();
    if (v >= 255 && p == 0) return false;          // bit-perfect path
    float m = v <= 0 ? 0.f : (float)(v / 255.0) * (float)(v / 255.0);  // ~perceptual taper
    float l = 1.f, r = 1.f;
    if (g_nch == 2) {
        if (p > 0) l = (128 - p) / 128.f;
        else if (p < 0) r = (128 + p) / 128.f;
    }
    for (int c = 0; c < g_nch; ++c) g[c] = m * (g_nch == 2 ? (c == 0 ? l : r) : 1.f);
    return true;
}

// Fill `frames` frames of the device buffer from the ring, padding with silence.
static HRESULT Fill(UINT32 frames) {
    if (!frames) return S_OK;
    BYTE* data = NULL;
    HRESULT hr = r_render->GetBuffer(frames, &data);
    if (FAILED(hr)) return hr;

    size_t got;
    EnterCriticalSection(&g_ringCs);
    got = g_ring.read(r_scratch.data(), (size_t)frames * g_frameBytes);
    LeaveCriticalSection(&g_ringCs);

    size_t n = got / g_frameBytes;
    size_t outFrameBytes = (size_t)g_nch * r_of.container / 8;
    if (n) {
        float gains[8];
        ConvertFrames(r_scratch.data(), data, n, g_bps, g_nch, r_of, ComputeGains(gains) ? gains : NULL);
    }
    if (n < frames) memset(data + n * outFrameBytes, 0, (frames - n) * outFrameBytes);
    hr = r_render->ReleaseBuffer(frames, 0);

    // Track silence inserted *before* real audio so the clock maps to song time.
    if (n) { r_committedSilence += r_pendingSilence; r_pendingSilence = 0; r_submittedReal += (long long)n; }
    r_pendingSilence += (long long)(frames - n);
    return hr;
}

static HRESULT FillAvailable() {
    UINT32 pad = 0;
    if (!r_exclusive) {
        HRESULT hr = r_client->GetCurrentPadding(&pad);
        if (FAILED(hr)) return hr;
    }
    return Fill(r_bufFrames - pad);
}

static void ResetCounters() {
    r_committedSilence = r_pendingSilence = r_submittedReal = 0;
}

static void PublishPosition() {
    if (g_cmd != CMD_NONE) return;  // a flush is pending; don't publish stale values
    long long played = 0;
    if (r_clock && r_clockFreq) {
        UINT64 pos = 0;
        if (SUCCEEDED(r_clock->GetPosition(&pos, NULL))) {
            long long dev = (long long)((double)pos * g_srate / (double)r_clockFreq);
            played = dev - r_committedSilence;
            if (played < 0) played = 0;
            if (played > r_submittedReal) played = r_submittedReal;
        }
    }
    g_played = r_frameOffset + played;
    g_submitted = r_frameOffset + r_submittedReal;
}

static void ProcessCommand() {
    LONG cmd = InterlockedExchange(&g_cmd, CMD_NONE);
    if (cmd == CMD_NONE) return;
    switch (cmd) {
    case CMD_PAUSE:
        r_paused = true;
        if (r_client) r_client->Stop();
        break;
    case CMD_UNPAUSE:
        r_paused = false;
        if (r_client) r_client->Start();
        break;
    case CMD_FLUSH:
        r_frameOffset = 0;
        ResetCounters();
        if (r_client) {
            r_client->Stop();
            r_client->Reset();
            FillAvailable();
            if (!r_paused) r_client->Start();
        }
        g_played = 0;
        g_submitted = 0;
        break;
    }
    SetEvent(g_ackEvent);
}

static void HandleDeviceError() {
    // Device unplugged, default changed, format changed, etc. Keep song time continuous.
    r_frameOffset = g_played.load();
    ResetCounters();
    ReleaseDevice();
    r_lastRetry = GetTickCount();
    _snwprintf(g_modeDesc, 159, L"Device lost - retrying...");
}

static void TryReopen() {
    r_lastRetry = GetTickCount();
    if (SUCCEEDED(InitDevice()) && SUCCEEDED(FillAvailable()) && (r_paused || SUCCEEDED(r_client->Start()))) {
        r_retries = 0;
        return;
    }
    ReleaseDevice();
    if (++r_retries >= 5) {
        g_failed = 1;  // give up: Write() will discard so playback can finish
        _snwprintf(g_modeDesc, 159, L"Device unavailable");
    }
}

typedef HANDLE (WINAPI* AvSetFn)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI* AvRevertFn)(HANDLE);

static DWORD WINAPI RenderThread(LPVOID) {
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    HANDLE mmcss = NULL;
    AvRevertFn avRevert = NULL;
    if (avrt) {
        AvSetFn avSet = (AvSetFn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
        avRevert = (AvRevertFn)(void*)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
        DWORD idx = 0;
        if (avSet) mmcss = avSet(L"Pro Audio", &idx);
    }
    if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    r_paused = false;
    r_frameOffset = 0;
    r_retries = 0;
    ResetCounters();

    HRESULT hr = InitDevice();
    if (SUCCEEDED(hr)) hr = FillAvailable();
    if (SUCCEEDED(hr)) hr = r_client->Start();
    g_initHr = hr;
    SetEvent(g_readyEvent);

    if (SUCCEEDED(hr)) {
        while (!g_quit) {
            HANDLE hs[2] = {g_cmdEvent, r_audioEvent};
            DWORD n = r_audioEvent ? 2 : 1;
            DWORD w = WaitForMultipleObjects(n, hs, FALSE, 100);
            if (g_quit) break;
            ProcessCommand();
            if (r_client) {
                if (w == WAIT_OBJECT_0 + 1 && !r_paused) {
                    hr = FillAvailable();
                    if (FAILED(hr)) { HandleDeviceError(); continue; }
                }
                PublishPosition();
            } else if (!g_failed && GetTickCount() - r_lastRetry > 1000) {
                TryReopen();
            }
        }
    }

    ReleaseDevice();
    if (mmcss && avRevert) avRevert(mmcss);
    if (avrt) FreeLibrary(avrt);
    if (SUCCEEDED(coHr)) CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// Out_Module implementation
// ---------------------------------------------------------------------------
static void Config(HWND parent);
static void About(HWND parent);
static void Init();
static void Quit();
static int Open(int, int, int, int, int);
static void Close();
static int Write(char*, int);
static int CanWrite();
static int IsPlaying();
static int Pause(int);
static void SetVolume(int);
static void SetPan(int);
static void Flush(int);
static int GetOutputTime();
static int GetWrittenTime();

static char g_desc[] = "Vibe Wasapi v" PLUGIN_VERSION " (out_vibewasapi.dll)";

static Out_Module g_mod = {
    OUT_VER, g_desc, 0x56574153 /* VWAS */, NULL, NULL,
    Config, About, Init, Quit, Open, Close, Write, CanWrite, IsPlaying,
    Pause, SetVolume, SetPan, Flush, GetOutputTime, GetWrittenTime
};

extern "C" __declspec(dllexport) Out_Module* winampGetOutModule() { return &g_mod; }

static void SendCommand(LONG cmd) {
    EnterCriticalSection(&g_cmdCs);
    ResetEvent(g_ackEvent);
    InterlockedExchange(&g_cmd, cmd);
    SetEvent(g_cmdEvent);
    WaitForSingleObject(g_ackEvent, 2000);
    LeaveCriticalSection(&g_cmdCs);
}

static void Init() {
    InitializeCriticalSection(&g_ringCs);
    InitializeCriticalSection(&g_cmdCs);

    // Resolve the ini path here, on Winamp's main thread (Open() may run on a
    // decode thread, and SendMessage from there can deadlock).
    if (g_mod.hMainWindow) {
        wchar_t* w = (wchar_t*)SendMessageW(g_mod.hMainWindow, WM_USER, 0, 1334 /*IPC_GETINIFILEW*/);
        if (w && (ULONG_PTR)w > 0xFFFF && w[0]) g_iniPath = w;
        if (g_iniPath.empty()) {
            char* a = (char*)SendMessageA(g_mod.hMainWindow, WM_USER, 0, 334 /*IPC_GETINIFILE*/);
            if (a && (ULONG_PTR)a > 0xFFFF && a[0]) {
                wchar_t tmp[MAX_PATH * 2] = {0};
                MultiByteToWideChar(CP_ACP, 0, a, -1, tmp, MAX_PATH * 2);
                g_iniPath = tmp;
            }
        }
    }
    if (g_iniPath.empty()) {
        wchar_t p[MAX_PATH] = {0};
        GetModuleFileNameW(g_mod.hDllInstance, p, MAX_PATH);
        wchar_t* slash = wcsrchr(p, L'\\');
        if (slash) { wcscpy(slash + 1, L"out_vibewasapi.ini"); g_iniPath = p; }
    }
    LoadSettings();
}

static void Quit() {
    Close();
    DeleteCriticalSection(&g_cmdCs);
    DeleteCriticalSection(&g_ringCs);
}

static int Open(int samplerate, int numchannels, int bitspersamp, int, int) {
    Close();
    if (samplerate <= 0 || numchannels < 1 || numchannels > 8) return -1;
    if (bitspersamp != 8 && bitspersamp != 16 && bitspersamp != 24 && bitspersamp != 32) return -1;

    LoadSettings();
    g_act = g_cfg;
    g_srate = samplerate;
    g_nch = numchannels;
    g_bps = bitspersamp;
    g_frameBytes = numchannels * bitspersamp / 8;

    size_t ringBytes = (size_t)((long long)g_act.ringMs * samplerate / 1000) * g_frameBytes;
    EnterCriticalSection(&g_ringCs);
    g_ring.alloc(ringBytes);
    LeaveCriticalSection(&g_ringCs);

    g_played = 0; g_submitted = 0; g_written = 0;
    g_baseMs = 0; g_failed = 0; g_paused = 0;
    g_cmd = CMD_NONE; g_quit = 0;
    g_initHr = E_PENDING;

    g_cmdEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_ackEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_readyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, RenderThread, NULL, 0, NULL);
    if (!g_thread) { Close(); return -1; }

    WaitForSingleObject(g_readyEvent, 5000);
    if (FAILED(g_initHr)) {
        Close();
        _snwprintf(g_modeDesc, 159, L"Open failed (0x%08lX)", (unsigned long)g_initHr);
        return -1;
    }
    return g_latencyMs;
}

static void Close() {
    if (g_thread) {
        InterlockedExchange(&g_quit, 1);
        SetEvent(g_cmdEvent);
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_cmdEvent) { CloseHandle(g_cmdEvent); g_cmdEvent = NULL; }
    if (g_ackEvent) { CloseHandle(g_ackEvent); g_ackEvent = NULL; }
    if (g_readyEvent) { CloseHandle(g_readyEvent); g_readyEvent = NULL; }
}

static int Write(char* buf, int len) {
    if (!g_thread || len < 0) return 1;
    size_t n = (size_t)len - (size_t)len % g_frameBytes;
    if (g_failed) { g_written += (long long)(n / g_frameBytes); return 0; }
    EnterCriticalSection(&g_ringCs);
    n = g_ring.write((const unsigned char*)buf, n);
    LeaveCriticalSection(&g_ringCs);
    g_written += (long long)(n / g_frameBytes);
    return 0;
}

static int CanWrite() {
    if (!g_thread) return 0;
    if (g_failed) return 65536 - 65536 % g_frameBytes;
    EnterCriticalSection(&g_ringCs);
    size_t f = g_ring.freeBytes();
    LeaveCriticalSection(&g_ringCs);
    f -= f % g_frameBytes;
    return f > 0x7FFFFFFF ? 0x7FFFFFFF : (int)f;
}

static int IsPlaying() {
    if (!g_thread || g_failed) return 0;
    EnterCriticalSection(&g_ringCs);
    size_t used = g_ring.used;
    LeaveCriticalSection(&g_ringCs);
    return used > 0 || g_played.load() < g_submitted.load();
}

static int Pause(int pause) {
    int prev = g_paused.load();
    pause = pause ? 1 : 0;
    g_paused = pause;
    if (g_thread && pause != prev) SendCommand(pause ? CMD_PAUSE : CMD_UNPAUSE);
    return prev;
}

static void SetVolume(int v) {
    if (v == -666) return;  // "re-apply current" - nothing cached elsewhere
    g_volume = v < 0 ? 0 : v > 255 ? 255 : v;
}

static void SetPan(int p) { g_pan = p < -128 ? -128 : p > 128 ? 128 : p; }

static void Flush(int t) {
    if (!g_thread) return;
    EnterCriticalSection(&g_ringCs);
    g_ring.clear();
    g_written = 0;
    g_baseMs = t;
    g_played = 0;
    g_submitted = 0;
    LeaveCriticalSection(&g_ringCs);
    SendCommand(CMD_FLUSH);
}

static int GetOutputTime() {
    if (!g_thread) return 0;
    return g_baseMs.load() + (int)(g_played.load() * 1000 / g_srate);
}

static int GetWrittenTime() {
    if (!g_thread) return 0;
    return g_baseMs.load() + (int)(g_written.load() * 1000 / g_srate);
}

// ---------------------------------------------------------------------------
// Config / About
// ---------------------------------------------------------------------------
static std::vector<std::wstring> s_devIds;

static void FillDeviceList(HWND combo) {
    s_devIds.clear();
    s_devIds.push_back(L"");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"System default device");

    IMMDeviceEnumerator* en = NULL;
    if (FAILED(CoCreateInstance(kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en)))
        return;
    IMMDeviceCollection* col = NULL;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* d = NULL;
            if (FAILED(col->Item(i, &d))) continue;
            LPWSTR id = NULL;
            IPropertyStore* ps = NULL;
            if (SUCCEEDED(d->GetId(&id)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(kPKEY_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)pv.pwszVal);
                    s_devIds.push_back(id);
                }
                PropVariantClear(&pv);
            }
            SafeRelease(ps);
            if (id) CoTaskMemFree(id);
            d->Release();
        }
        col->Release();
    }
    en->Release();
}

static INT_PTR CALLBACK ConfigProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND combo = GetDlgItem(hwnd, IDC_DEVICE);
        FillDeviceList(combo);
        int sel = 0;
        for (size_t i = 1; i < s_devIds.size(); ++i)
            if (s_devIds[i] == g_cfg.deviceId) sel = (int)i;
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
        CheckDlgButton(hwnd, IDC_EXCLUSIVE, g_cfg.exclusive ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_FALLBACK, g_cfg.fallback ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_VOLUME, g_cfg.volume ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hwnd, IDC_RINGMS, g_cfg.ringMs, FALSE);
        SetDlgItemInt(hwnd, IDC_PERIODMS, g_cfg.periodMs, FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_FALLBACK), g_cfg.exclusive);
        wchar_t status[256];
        _snwprintf(status, 255, L"%s\nChanges apply from the next track.", g_modeDesc);
        status[255] = 0;
        SetDlgItemTextW(hwnd, IDC_STATUS, status);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_EXCLUSIVE:
            EnableWindow(GetDlgItem(hwnd, IDC_FALLBACK), IsDlgButtonChecked(hwnd, IDC_EXCLUSIVE) == BST_CHECKED);
            return TRUE;
        case IDOK: {
            int sel = (int)SendDlgItemMessageW(hwnd, IDC_DEVICE, CB_GETCURSEL, 0, 0);
            g_cfg.deviceId = (sel > 0 && sel < (int)s_devIds.size()) ? s_devIds[sel] : L"";
            g_cfg.exclusive = IsDlgButtonChecked(hwnd, IDC_EXCLUSIVE) == BST_CHECKED;
            g_cfg.fallback = IsDlgButtonChecked(hwnd, IDC_FALLBACK) == BST_CHECKED;
            g_cfg.volume = IsDlgButtonChecked(hwnd, IDC_VOLUME) == BST_CHECKED;
            int r = (int)GetDlgItemInt(hwnd, IDC_RINGMS, NULL, FALSE);
            int p = (int)GetDlgItemInt(hwnd, IDC_PERIODMS, NULL, FALSE);
            g_cfg.ringMs = r < 100 ? 100 : r > 10000 ? 10000 : r;
            g_cfg.periodMs = p > 500 ? 500 : p;
            SaveSettings();
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void Config(HWND parent) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DialogBoxParamW(g_mod.hDllInstance, MAKEINTRESOURCEW(IDD_CONFIG), parent, ConfigProc, 0);
    if (SUCCEEDED(hr)) CoUninitialize();
}

static void About(HWND parent) {
    MessageBoxA(parent,
                "Vibe Wasapi v" PLUGIN_VERSION "\n\n"
                "Exclusive (bit-perfect) and shared-mode WASAPI output\n"
                "for Winamp and WACUP.",
                "About Vibe Wasapi", MB_OK | MB_ICONINFORMATION);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
