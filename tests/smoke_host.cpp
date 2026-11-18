// smoke_host.cpp - Pretends to be Winamp so the finished DLL can be tried on a real device.
//
// Usage:  vibe_smoke_host.exe C:\path\to\out_vibewasapi.dll
//
// It plays digital silence only, so nothing is audible. It is not part of the automated
// tests because build servers have no sound card. The plug-in reads its settings from
// out_vibewasapi.ini beside the DLL; set exclusive=0 there to leave other apps undisturbed.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "out.h"

static int g_failures = 0;

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
    const int latency = module->Open(44100, 2, 16, 0, 0);
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
    if (module->Open(48000, 2, 24, 0, 0) < 0) {
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
        if (module->Open(44100, 2, 16, 0, 0) >= 0) ++opened;
        FeedSilence(module, 40);
        module->Flush(track * 1000);
        FeedSilence(module, 40);
        module->Close();
    }
    stop.store(true);
    interfaceThread.join();
    CHECK(opened == 25);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        printf("Usage: vibe_smoke_host.exe <path to out_vibewasapi.dll>\n");
        return 2;
    }

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
    TestConcurrentCalls(module);

    module->Quit();
    FreeLibrary(dll);

    printf(g_failures ? "%d check(s) failed.\n" : "All smoke tests passed.\n", g_failures);
    return g_failures ? 1 : 0;
}
