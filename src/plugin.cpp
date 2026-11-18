// plugin.cpp - The functions Winamp calls. This is the plug-in's front door.
//
// Winamp loads this DLL, calls winampGetOutModule() to get a table of functions, and
// then calls those functions as tracks play. A typical track looks like this:
//
//   Open -> Write / CanWrite (repeatedly) -> IsPlaying (until drained) -> Close
//
// Thread safety is the main concern here, because Winamp may call these functions
// from its interface thread and its decoding thread at the same moment:
//
//   * g_lifecycleLock makes Open, Close and Flush run one at a time.
//   * g_streamLock protects the ring buffer and stream format while they are replaced.
//     Audio calls share it; Open and Close hold it exclusively only for a moment, and
//     never while waiting for a device, so the interface stays responsive.
//   * Everything else uses atomic values, which are safe to read from any thread.
#include <windows.h>
#include <atomic>
#include <string>

#include "config_dialog.h"
#include "out.h"
#include "render_engine.h"
#include "settings.h"
#include "spsc_ring.h"
#include "version.h"

namespace {

CRITICAL_SECTION g_lifecycleLock;
SRWLOCK g_streamLock = SRWLOCK_INIT;

RenderEngine g_engine;
SpscRing g_ring;                         // Guarded by g_streamLock when replaced
StreamFormat g_format;                   // Guarded by g_streamLock
std::atomic<bool> g_streamOpen{false};
std::atomic<int> g_sampleRate{44100};    // Atomic copy for the position functions
std::atomic<long long> g_writtenFrames{0};
std::atomic<int> g_seekBaseMs{0};        // Song time of the most recent seek

std::wstring g_iniPath;                  // Set once in Init, then only read

// Plug-in functions must be declared before the table that points to them.
void Config(HWND parent);
void About(HWND parent);
void Init();
void Quit();
int Open(int sampleRate, int channels, int bitsPerSample, int bufferLengthMs, int prebufferMs);
void Close();
int Write(char* buffer, int length);
int CanWrite();
int IsPlaying();
int Pause(int pause);
void SetVolume(int volume);
void SetPan(int pan);
void Flush(int timeMs);
int GetOutputTime();
int GetWrittenTime();

char g_description[] = "Vibe Wasapi v" VIBE_VERSION_TEXT;

// The table of functions handed to Winamp.
Out_Module g_module = {
    OUT_VER, g_description, 0x56574153 /* "VWAS" */, NULL, NULL,
    Config, About, Init, Quit, Open, Close, Write, CanWrite, IsPlaying,
    Pause, SetVolume, SetPan, Flush, GetOutputTime, GetWrittenTime
};

// Finds winamp.ini. This must run on Winamp's main thread, which is why it is done in
// Init: asking the main window from a decoding thread could deadlock.
std::wstring FindIniPath() {
    const UINT kWmWinampIpc = WM_USER;
    const LPARAM kIpcGetIniFile = 334;
    const LPARAM kIpcGetIniFileW = 1334;

    if (g_module.hMainWindow) {
        // Winamp returns a small number instead of a pointer if it doesn't support a request.
        wchar_t* wide = (wchar_t*)SendMessageW(g_module.hMainWindow, kWmWinampIpc, 0, kIpcGetIniFileW);
        if (wide && (ULONG_PTR)wide > 0xFFFF && wide[0]) return wide;

        char* narrow = (char*)SendMessageA(g_module.hMainWindow, kWmWinampIpc, 0, kIpcGetIniFile);
        if (narrow && (ULONG_PTR)narrow > 0xFFFF && narrow[0]) {
            wchar_t converted[MAX_PATH * 2] = {0};
            MultiByteToWideChar(CP_ACP, 0, narrow, -1, converted, MAX_PATH * 2);
            return converted;
        }
    }

    // Not running inside Winamp: keep the settings in a file next to the DLL.
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(g_module.hDllInstance, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return L"";
    wcscpy(slash + 1, L"out_vibewasapi.ini");
    return path;
}

// Stops the current stream. The caller must hold g_lifecycleLock.
void CloseStreamLocked() {
    AcquireSRWLockExclusive(&g_streamLock);
    g_streamOpen.store(false);
    ReleaseSRWLockExclusive(&g_streamLock);
    g_engine.stop();
}

void Config(HWND parent) {
    ShowConfigDialog(g_module.hDllInstance, parent, g_iniPath, g_engine.statusText());
}

void About(HWND parent) {
    MessageBoxA(parent,
                "Vibe Wasapi v" VIBE_VERSION_TEXT "\n\n"
                "Exclusive (bit-perfect) and shared-mode WASAPI output\n"
                "for Winamp and WACUP.",
                "About Vibe Wasapi", MB_OK | MB_ICONINFORMATION);
}

void Init() {
    InitializeCriticalSection(&g_lifecycleLock);
    g_engine.initialize();
    g_iniPath = FindIniPath();
}

void Quit() {
    Close();
    g_engine.shutdown();
    DeleteCriticalSection(&g_lifecycleLock);
}

// Starts a new stream. Returns the output latency in ms, or -1 on failure.
int Open(int sampleRate, int channels, int bitsPerSample, int, int) {
    if (sampleRate <= 0 || channels < 1 || channels > 8) return -1;
    if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) return -1;

    EnterCriticalSection(&g_lifecycleLock);
    CloseStreamLocked();

    StreamParams params;
    params.settings = LoadSettings(g_iniPath);
    params.format.sampleRate = sampleRate;
    params.format.channels = channels;
    params.format.bitsPerSample = bitsPerSample;
    params.ring = &g_ring;

    // Size the ring as a whole number of frames so a frame never splits across a write.
    long long ringFrames = (long long)params.settings.ringMs * sampleRate / 1000;
    if (ringFrames < 1) ringFrames = 1;

    AcquireSRWLockExclusive(&g_streamLock);
    g_format = params.format;
    g_ring.allocate((size_t)ringFrames * params.format.frameBytes());
    ReleaseSRWLockExclusive(&g_streamLock);

    g_sampleRate.store(sampleRate);
    g_writtenFrames.store(0);
    g_seekBaseMs.store(0);

    // This can take a few seconds if the device is busy. Only the lifecycle lock is
    // held, so Pause, volume changes and position updates are not blocked.
    const HRESULT hr = g_engine.start(params);
    if (SUCCEEDED(hr)) g_streamOpen.store(true);
    LeaveCriticalSection(&g_lifecycleLock);

    return SUCCEEDED(hr) ? g_engine.latencyMs() : -1;
}

void Close() {
    EnterCriticalSection(&g_lifecycleLock);
    CloseStreamLocked();
    LeaveCriticalSection(&g_lifecycleLock);
}

// Accepts decoded audio from Winamp. Returns 0 on success.
int Write(char* buffer, int length) {
    if (length < 0) return 1;

    int result = 1;
    AcquireSRWLockShared(&g_streamLock);
    if (g_streamOpen.load()) {
        const size_t frameBytes = (size_t)g_format.frameBytes();
        const size_t wholeFrames = (size_t)length - (size_t)length % frameBytes;
        const size_t written = g_ring.write((const unsigned char*)buffer, wholeFrames);
        g_writtenFrames.fetch_add((long long)(written / frameBytes));
        result = 0;
    }
    ReleaseSRWLockShared(&g_streamLock);
    return result;
}

// Tells Winamp how many bytes it may write right now.
int CanWrite() {
    int result = 0;
    AcquireSRWLockShared(&g_streamLock);
    if (g_streamOpen.load()) {
        size_t room = g_ring.freeBytes();
        room -= room % (size_t)g_format.frameBytes();
        result = room > 0x7FFFFFFF ? 0x7FFFFFFF : (int)room;
    }
    ReleaseSRWLockShared(&g_streamLock);
    return result;
}

// Returns nonzero while audio is still waiting to be heard.
int IsPlaying() {
    int result = 0;
    AcquireSRWLockShared(&g_streamLock);
    if (g_streamOpen.load()) {
        result = g_ring.pendingBytes() > 0 || g_engine.playedFrames() < g_engine.submittedFrames();
    }
    ReleaseSRWLockShared(&g_streamLock);
    return result;
}

// Pauses or resumes, and returns the previous pause state.
int Pause(int pause) {
    return g_engine.setPaused(pause != 0) ? 1 : 0;
}

void SetVolume(int volume) {
    if (volume == -666) return;  // Winamp's "re-apply the current volume" request
    g_engine.setVolume(volume < 0 ? 0 : volume > 255 ? 255 : volume);
}

void SetPan(int pan) {
    g_engine.setPan(pan < -128 ? -128 : pan > 128 ? 128 : pan);
}

// Handles a seek: drops queued audio and makes `timeMs` the new song position.
void Flush(int timeMs) {
    EnterCriticalSection(&g_lifecycleLock);
    if (g_streamOpen.load()) {
        g_ring.requestDiscard();
        g_writtenFrames.store(0);
        g_seekBaseMs.store(timeMs);
        g_engine.flush();
    }
    LeaveCriticalSection(&g_lifecycleLock);
}

// The song time, in ms, that the listener is hearing now.
int GetOutputTime() {
    if (!g_streamOpen.load()) return 0;
    return g_seekBaseMs.load() + (int)(g_engine.playedFrames() * 1000 / g_sampleRate.load());
}

// The song time, in ms, of the newest audio Winamp has written.
int GetWrittenTime() {
    if (!g_streamOpen.load()) return 0;
    return g_seekBaseMs.load() + (int)(g_writtenFrames.load() * 1000 / g_sampleRate.load());
}

}  // namespace

// The one function Winamp looks for when it loads an output plug-in.
extern "C" __declspec(dllexport) Out_Module* winampGetOutModule() {
    return &g_module;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
