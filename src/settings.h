// settings.h - User-configurable options and how they are stored on disk.
//
// Settings live in the [out_vibewasapi] section of winamp.ini. There is
// deliberately no global "current settings" object: Winamp can open a stream on
// its decoding thread while the settings window is open on its main thread, so
// each caller works on its own private copy. Windows serializes the ini access.
#pragma once

#include <string>

struct Settings {
    std::wstring deviceId;  // Windows endpoint ID; empty means "system default device"
    int exclusive = 1;      // 1 = ask for exclusive (bit-perfect) device access
    int fallback  = 1;      // 1 = use shared mode if exclusive mode is refused
    int volume    = 1;      // 1 = apply Winamp's volume and pan in software
    int ringMs    = 1000;   // Length of our own audio buffer, in milliseconds
    int periodMs  = 0;      // Device period / shared buffer in ms; 0 = device default
};

// Allowed ranges for the numeric options.
enum {
    kMinRingMs = 100,
    kMaxRingMs = 10000,
    kMinPeriodMs = 0,
    kMaxPeriodMs = 500
};

// Returns a copy of `settings` with every numeric option forced into its allowed
// range, so a hand-edited ini file cannot produce an unusable configuration.
Settings ClampSettings(Settings settings);

// Reads settings from the ini file. An empty path returns the defaults.
Settings LoadSettings(const std::wstring& iniPath);

// Writes settings to the ini file. An empty path does nothing.
void SaveSettings(const std::wstring& iniPath, const Settings& settings);
