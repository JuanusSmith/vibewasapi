// smoke_host.cpp - Pretends to be Winamp so the finished DLL can be tried on a real device.
//
// Usage:  vibe_smoke_host.exe C:\path\to\out_vibewasapi.dll [sample rate]
//
// It plays digital silence only, so nothing is audible. It is not part of the automated
// tests because build servers have no sound card. The plug-in reads its settings from
// out_vibewasapi.ini beside the DLL; set exclusive=0 there to leave other apps undisturbed.
//
// In exclusive mode the plug-in does not resample, so the tests can only open at a rate
// the device accepts. Pass that rate (for example 48000) to run every test at it; the
// format-change test then switches bit depth instead of sample rate.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "out.h"

static int g_failures = 0;

// The sample rate most tests open at, and the format the format-change test switches to.
static int g_rate = 44100;
static int g_otherRate = 96000;
static int g_otherBits = 24;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (condition) {                                                     \
            printf("  ok: %s\n", #condition);                                \
        } else {                                                             \
            printf("  FAILED line %d: %s\n", __LINE__, #condition);          \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// Feeds silence to the plug-in for about `durationMs`, as a decoder would.
static void FeedSilence(Out_Module* module, int durationMs) {
    std::vector<char> silence(4096, 0);
    const DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < durationMs) {
        if (module->CanWrite() >= (int)silence.size()) module->Write(silence.data(), (int)silence.size());
        else Sleep(5);
    }
}

static void TestPlaybackControls(Out_Module* module) {
    printf("Playback, pause, resume and seek:\n");
    const int latency = module->Open(g_rate, 2, 16, 0, 0);
    CHECK(latency >= 0);
    if (latency < 0) return;

    FeedSilence(module, 1500);
    const int playing = module->GetOutputTime();
    CHECK(playing > 500);

    module->Pause(1);
    Sleep(300);  // Give the pause and the last position update time to settle
    const int pausedAt = module->GetOutputTime();
    Sleep(700);
    CHECK(module->GetOutputTime() == pausedAt);

    CHECK(module->Pause(0) == 1);
    FeedSilence(module, 1000);
    CHECK(module->GetOutputTime() > pausedAt);

    module->Flush(60000);
    CHECK(module->GetOutputTime() >= 60000);
    CHECK(module->GetWrittenTime() == 60000);
    FeedSilence(module, 800);
    CHECK(module->GetOutputTime() > 60000);

    // Seeking while paused must work too.
    module->Pause(1);
    module->Flush(30000);
    CHECK(module->GetOutputTime() == 30000);
    module->Pause(0);
    FeedSilence(module, 800);
    CHECK(module->GetOutputTime() > 30000);
    module->Close();
}

static void TestEndOfTrackDrains(Out_Module* module) {
    printf("End of track:\n");
    if (module->Open(g_rate, 2, 24, 0, 0) < 0) {
        CHECK(!"Open failed");
        return;
    }
    FeedSilence(module, 500);

    const DWORD start = GetTickCount();
    while (module->IsPlaying() && GetTickCount() - start < 5000) Sleep(10);
    CHECK(!module->IsPlaying());
    module->Close();
}

// Calls the plug-in from a second thread while tracks open and close, as Winamp can.
static void TestConcurrentCalls(Out_Module* module) {
    printf("Calls from several threads during track changes:\n");
    std::atomic<bool> stop{false};
    std::thread interfaceThread([&] {
        int pause = 0;
        while (!stop.load()) {
            module->Pause(pause);
            pause = !pause;
            module->GetOutputTime();
            module->GetWrittenTime();
            module->IsPlaying();
            module->CanWrite();
            module->SetVolume(200);
            module->SetPan(0);
            Sleep(1);
        }
    });

    int opened = 0;
    for (int track = 0; track < 25; ++track) {
        if (module->Open(g_rate, 2, 16, 0, 0) >= 0) ++opened;
        FeedSilence(module, 40);
        module->Flush(track * 1000);
        FeedSilence(module, 40);
        module->Close();
    }
    stop.store(true);
    interfaceThread.join();
    CHECK(opened == 25);
}

// Waits for Winamp's end-of-track signal, as an input plug-in does after its last Write.
static bool WaitForTrackEnd(Out_Module* module, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (module->IsPlaying()) {
        if (GetTickCount() - start > timeoutMs) return false;
        Sleep(5);
    }
    return true;
}

static void TestGaplessTrackChange(Out_Module* module) {
    printf("Gapless track change (same format):\n");
    if (module->Open(g_rate, 2, 16, 0, 0) < 0) {
        CHECK(!"Open failed");
        return;
    }
    FeedSilence(module, 1200);
    CHECK(WaitForTrackEnd(module, 5000));
    module->Close();  // Lingers: the last audio keeps playing

    // The next track continues the same stream, so it opens almost instantly.
    const DWORD openStart = GetTickCount();
    const int latency = module->Open(g_rate, 2, 16, 0, 0);
    const DWORD openMs = GetTickCount() - openStart;
    printf("  (next track opened in %lu ms)\n", openMs);
    CHECK(latency >= 0);
    CHECK(openMs < 50);
    CHECK(module->GetOutputTime() < 100);  // The new track starts at its beginning

    FeedSilence(module, 1200);
    const int elapsed = module->GetOutputTime();
    CHECK(elapsed > 200 && elapsed < 1300);

    // Stopping mid-track fades out and stops promptly.
    const DWORD closeStart = GetTickCount();
    module->Close();
    const DWORD closeMs = GetTickCount() - closeStart;
    printf("  (mid-track stop took %lu ms)\n", closeMs);
    CHECK(closeMs < 500);
}

static void TestFormatChangeAfterTrackEnd(Out_Module* module) {
    printf("Track change to a different format:\n");
    if (module->Open(g_rate, 2, 16, 0, 0) < 0) {
        CHECK(!"Open failed");
        return;
    }
    FeedSilence(module, 800);
    CHECK(WaitForTrackEnd(module, 5000));
    module->Close();

    // The new format needs a new stream; it must still open successfully.
    CHECK(module->Open(g_otherRate, 2, g_otherBits, 0, 0) >= 0);
    FeedSilence(module, 800);
    CHECK(module->GetOutputTime() > 200);
    module->Close();
}

static void TestLingerEndsWithoutNextTrack(Out_Module* module) {
    printf("Last track in the playlist:\n");
    if (module->Open(g_rate, 2, 16, 0, 0) < 0) {
        CHECK(!"Open failed");
        return;
    }
    FeedSilence(module, 500);
    CHECK(WaitForTrackEnd(module, 5000));
    module->Close();
    Sleep(3500);  // Long enough for the lingering stream to release the device

    // A later track starts a fresh stream normally.
    CHECK(module->Open(g_rate, 2, 16, 0, 0) >= 0);
    FeedSilence(module, 800);
    CHECK(module->GetOutputTime() > 200);
    module->Close();
}

int wmain(int argc, wchar_t** argv) {
    // Print each line straight away, so a crash still shows how far the tests got.
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 2 && argc != 3) {
        printf("Usage: vibe_smoke_host.exe <path to out_vibewasapi.dll> [sample rate]\n");
        return 2;
    }
    if (argc == 3) {
        g_rate = _wtoi(argv[2]);
        if (g_rate < 8000 || g_rate > 768000) {
            printf("The sample rate must be between 8000 and 768000.\n");
            return 2;
        }
        g_otherRate = g_rate;  // Same rate, so the device can still open it in exclusive mode
        g_otherBits = 32;      // A different bit depth still needs a new stream
    }
    printf("Testing at %d Hz.\n", g_rate);

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) {
        printf("Could not load the DLL (error %lu).\n", GetLastError());
        return 2;
    }
    typedef Out_Module* (*GetModuleFn)();
    GetModuleFn getModule = (GetModuleFn)(void*)GetProcAddress(dll, "winampGetOutModule");
    if (!getModule) {
        printf("The DLL does not export winampGetOutModule.\n");
        return 2;
    }

    Out_Module* module = getModule();
    printf("Loaded: %s\n", module->description);
    module->hDllInstance = dll;
    module->hMainWindow = NULL;
    module->Init();

    TestPlaybackControls(module);
    TestEndOfTrackDrains(module);
    TestGaplessTrackChange(module);
    TestFormatChangeAfterTrackEnd(module);
    TestLingerEndsWithoutNextTrack(module);
    TestConcurrentCalls(module);

    module->Quit();
    FreeLibrary(dll);

    printf(g_failures ? "%d check(s) failed.\n" : "All smoke tests passed.\n", g_failures);
    return g_failures ? 1 : 0;
}
