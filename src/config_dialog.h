// config_dialog.h - The settings window opened from Winamp's Preferences.
#pragma once

#include <windows.h>
#include <string>

// Returns the current playback description. The window calls it twice a second, so
// the status stays up to date while the window is open.
typedef std::wstring (*StatusProvider)();

// Shows the settings window and blocks until the user closes it.
void ShowConfigDialog(HINSTANCE instance, HWND parent, const std::wstring& iniPath,
                      StatusProvider statusProvider);
