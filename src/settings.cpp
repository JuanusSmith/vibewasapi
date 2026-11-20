// settings.cpp - Loads and saves the options described in settings.h.
#include "settings.h"

#include <windows.h>
#include <cstdio>

namespace {

const wchar_t kIniSection[] = L"out_vibewasapi";

int ClampInt(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

void WriteInt(const std::wstring& iniPath, const wchar_t* key, int value) {
    wchar_t text[32];
    _snwprintf(text, 31, L"%d", value);
    text[31] = 0;
    WritePrivateProfileStringW(kIniSection, key, text, iniPath.c_str());
}

}  // namespace

Settings ClampSettings(Settings settings) {
    settings.ringMs = ClampInt(settings.ringMs, kMinRingMs, kMaxRingMs);
    settings.periodMs = ClampInt(settings.periodMs, kMinPeriodMs, kMaxPeriodMs);
    return settings;
}

Settings LoadSettings(const std::wstring& iniPath) {
    Settings settings;
    if (iniPath.empty()) return settings;

    const wchar_t* ini = iniPath.c_str();
    wchar_t device[512] = {0};
    GetPrivateProfileStringW(kIniSection, L"device", L"", device, 512, ini);
    settings.deviceId = device;
    settings.exclusive = GetPrivateProfileIntW(kIniSection, L"exclusive", 1, ini) != 0;
    settings.fallback = GetPrivateProfileIntW(kIniSection, L"fallback", 1, ini) != 0;
    settings.volume = GetPrivateProfileIntW(kIniSection, L"volume", 1, ini) != 0;
    settings.ringMs = GetPrivateProfileIntW(kIniSection, L"buffer_ms", 1000, ini);
    settings.periodMs = GetPrivateProfileIntW(kIniSection, L"period_ms", 0, ini);
    return ClampSettings(settings);
}

void SaveSettings(const std::wstring& iniPath, const Settings& settings) {
    if (iniPath.empty()) return;
    WritePrivateProfileStringW(kIniSection, L"device", settings.deviceId.c_str(), iniPath.c_str());
    // On/off options are stored as 1 or 0, as in earlier versions.
    WriteInt(iniPath, L"exclusive", settings.exclusive ? 1 : 0);
    WriteInt(iniPath, L"fallback", settings.fallback ? 1 : 0);
    WriteInt(iniPath, L"volume", settings.volume ? 1 : 0);
    WriteInt(iniPath, L"buffer_ms", settings.ringMs);
    WriteInt(iniPath, L"period_ms", settings.periodMs);
}
