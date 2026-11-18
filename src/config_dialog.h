// config_dialog.h - The settings window opened from Winamp's Preferences.
#pragma once

#include <windows.h>
#include <string>

// Shows the settings window and blocks until the user closes it. `statusText` is the
// current playback description, displayed at the bottom of the window.
void ShowConfigDialog(HINSTANCE instance, HWND parent, const std::wstring& iniPath,
                      const std::wstring& statusText);
