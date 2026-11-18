// Winamp output plug-in interface (compatible with the Winamp 5.x SDK out.h).
// Works with Winamp 2.x/5.x and WACUP.
#pragma once
#include <windows.h>
#include <stdint.h>

#define OUT_VER 0x10

typedef struct {
    int version;            // OUT_VER
    char *description;      // shown in Preferences > Output
    intptr_t id;            // unique-ish id
    HWND hMainWindow;       // filled in by Winamp
    HINSTANCE hDllInstance; // filled in by Winamp

    void (*Config)(HWND hwndParent);
    void (*About)(HWND hwndParent);

    void (*Init)();
    void (*Quit)();

    // returns >= 0 (max latency in ms) on success, < 0 on failure
    int (*Open)(int samplerate, int numchannels, int bitspersamp, int bufferlenms, int prebufferms);
    void (*Close)();
    int (*Write)(char *buf, int len);   // 0 on success
    int (*CanWrite)();                  // bytes that can be written right now
    int (*IsPlaying)();                 // nonzero while buffered audio is still playing
    int (*Pause)(int pause);            // returns previous pause state
    void (*SetVolume)(int volume);      // 0..255, -666 = re-apply current
    void (*SetPan)(int pan);            // -128..128
    void (*Flush)(int t);               // drop buffers, output time becomes t (ms)
    int (*GetOutputTime)();             // ms actually heard
    int (*GetWrittenTime)();            // ms written by the input plug-in
} Out_Module;
