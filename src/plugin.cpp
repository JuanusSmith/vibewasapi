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
//     The song-time counters live in TrackTimeline (track_timeline.h).
//
// Gapless playback works like this:
//   1. Near the end of a track, IsPlaying reports "finished" while about
//      kGaplessLeadMs of audio is still queued, so Winamp starts the next track early.
//   2. Close then leaves the stream running ("lingering") to play that last audio.
//   3. If the next Open has the same format and settings, it continues the same
//      stream, and the new track's audio follows the old one with no gap.
//   4. Otherwise Open waits for the old audio to finish, then starts afresh.
// A Close that arrives mid-track (the user pressed Stop or chose another track)
// fades out and stops immediately instead.
#include <windows.h>
#include <atomic>
#include <string>

#include "config_dialog.h"
#include "out.h"
#include "render_engine.h"
#include "settings.h"
#include "spsc_ring.h"
#include "track_timeline.h"
#include "version.h"

namespace {

// How much audio may still be queued when IsPlaying reports the end of a track. This
// is the time Winamp has to open the next track before a gap could be heard.
const int kGaplessLeadMs = 500;

// Extra time Open allows, beyond the queued audio, for a lingering track to finish.
const DWORD kTailWaitSlackMs = 500;

CRITICAL_SECTION g_lifecycleLock;
SRWLOCK g_streamLock = SRWLOCK_INIT;

RenderEngine g_engine;
SpscRing g_ring;                         // Guarded by g_streamLock when replaced
StreamFormat g_format;                   // Guarded by g_streamLock
Settings g_streamSettings;               // Settings of the running stream; guarded by g_lifecycleLock
std::atomic<bool> g_streamOpen{false};   // Winamp may use the stream (Write, IsPlaying...)
TrackTimeline g_timeline;                // Song time and end-of-track state

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

// Frames still to be heard: audio waiting in the ring plus audio inside the device.
// The caller must hold g_streamLock (shared) or g_lifecycleLock.
long long QueuedFrames() {
    const long long inRing = (long long)(g_ring.pendingBytes() / (size_t)g_format.frameBytes());
    const long long inDevice = g_engine.submittedFrames() - g_engine.playedFrames();
    return inRing + (inDevice > 0 ? inDevice : 0);
}

// Stops Winamp's access to the stream. The caller must hold g_lifecycleLock.
void DetachStreamLocked() {
    AcquireSRWLockExclusive(&g_streamLock);
    g_streamOpen.store(false);
    ReleaseSRWLockExclusive(&g_streamLock);
}

// Stops the current stream, with a short fade if it is audible.
// The caller must hold g_lifecycleLock.
void CloseStreamLocked(bool fadeOut) {
    DetachStreamLocked();
    g_engine.stop(fadeOut);
}

// Waits for a lingering track's last audio to finish playing.
// The caller must hold g_lifecycleLock.
void WaitForLingeringTail() {
    const DWORD limit = (DWORD)kGaplessLeadMs + (DWORD)g_engine.latencyMs() + kTailWaitSlackMs;
    const DWORD startTick = GetTickCount();
    while (g_engine.lingerActive() && QueuedFrames() > 0 && GetTickCount() - startTick < limit) {
        Sleep(5);
    }
}

void Config(HWND parent) {
    ShowConfigDialog(g_module.hDllInstance, parent, g_iniPath, [] { return g_engine.statusText(); });
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
    // Winamp is closing, so stop at once, even if a track is lingering.
    EnterCriticalSection(&g_lifecycleLock);
    CloseStreamLocked(false);
    LeaveCriticalSection(&g_lifecycleLock);
    g_engine.shutdown();
    DeleteCriticalSection(&g_lifecycleLock);
}

// Starts a new track. Returns the output latency in ms, or -1 on failure.
int Open(int sampleRate, int channels, int bitsPerSample, int, int) {
    if (sampleRate <= 0 || channels < 1 || channels > 8) return -1;
    if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) return -1;

    StreamParams params;
    params.settings = LoadSettings(g_iniPath);
    params.format.sampleRate = sampleRate;
    params.format.channels = channels;
    params.format.bitsPerSample = bitsPerSample;
    params.ring = &g_ring;

    EnterCriticalSection(&g_lifecycleLock);

    // Gapless: continue the previous track's stream if nothing about it would change.
    if (g_engine.isLingering()) {
        const bool compatible = params.format == g_format && params.settings == g_streamSettings;
        if (compatible && g_engine.resumeFromLinger()) {
            g_timeline.startGaplessTrack();
            g_streamOpen.store(true);
            LeaveCriticalSection(&g_lifecycleLock);
            return g_engine.latencyMs();
        }

        // A different format needs a new stream, but let the last track finish first.
        WaitForLingeringTail();
    }
    CloseStreamLocked(false);

    // Size the ring as a whole number of frames so a frame never splits across a write.
    long long ringFrames = (long long)params.settings.ringMs * sampleRate / 1000;
    if (ringFrames < 1) ringFrames = 1;

    AcquireSRWLockExclusive(&g_streamLock);
    g_format = params.format;
    g_ring.allocate((size_t)ringFrames * params.format.frameBytes());
    ReleaseSRWLockExclusive(&g_streamLock);

    g_streamSettings = params.settings;
    g_timeline.startStream(sampleRate);

    // This can take a few seconds if the device is busy. Only the lifecycle lock is
    // held, so Pause, volume changes and position updates are not blocked.
    const HRESULT hr = g_engine.start(params);
    if (SUCCEEDED(hr)) g_streamOpen.store(true);
    LeaveCriticalSection(&g_lifecycleLock);

    return SUCCEEDED(hr) ? g_engine.latencyMs() : -1;
}

// Ends a track. See the gapless notes at the top of this file.
void Close() {
    EnterCriticalSection(&g_lifecycleLock);
    if (g_streamOpen.load()) {
        if (g_timeline.endReported() && g_engine.beginLinger()) DetachStreamLocked();
        else CloseStreamLocked(true);
    }
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
        const long long written = (long long)(g_ring.write((const unsigned char*)buffer, wholeFrames) / frameBytes);
        g_timeline.onWritten(written);
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

// Returns nonzero while the track is still playing. Winamp calls this after the last
// Write to learn when the track has ended. Reporting the end slightly early is what
// makes gapless playback possible; the remaining audio still plays in full.
int IsPlaying() {
    int result = 0;
    AcquireSRWLockShared(&g_streamLock);
    if (g_streamOpen.load()) {
        const long long leadFrames = (long long)g_format.sampleRate * kGaplessLeadMs / 1000;
        result = QueuedFrames() > leadFrames;
        if (!result) g_timeline.markEndReported();
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
        g_timeline.seek(timeMs);
        g_engine.flush();
    }
    LeaveCriticalSection(&g_lifecycleLock);
}

// The song time, in ms, that the listener is hearing now.
int GetOutputTime() {
    if (!g_streamOpen.load()) return 0;
    return g_timeline.outputTimeMs(g_engine.playedFrames());
}

// The song time, in ms, of the newest audio Winamp has written.
int GetWrittenTime() {
    if (!g_streamOpen.load()) return 0;
    return g_timeline.writtenTimeMs();
}

}  // namespace

// The one function Winamp looks for when it loads an output plug-in.
extern "C" __declspec(dllexport) Out_Module* winampGetOutModule() {
    return &g_module;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
