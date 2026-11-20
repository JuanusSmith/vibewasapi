// config_dialog.cpp - Implements the settings window.
//
// The window edits its own private copy of the settings and saves them to the ini
// file when the user clicks OK. A playing stream is unaffected; the next stream reads
// the new values when it opens.
//
// It also shows two kinds of information:
//   * "Now playing": what the current stream is doing, refreshed twice a second.
//   * The exclusive-mode formats the selected device accepts, checked when the window
//     opens and whenever a different device is chosen.
#include "config_dialog.h"

#include <objbase.h>
#include <mmdeviceapi.h>
#include <vector>

#include "playback_status.h"
#include "resource.h"
#include "settings.h"
#include "wasapi_device.h"

namespace {

const UINT_PTR kStatusTimer = 1;
const UINT kStatusRefreshMs = 500;

// Data that belongs to one open settings window.
struct DialogState {
    std::wstring iniPath;
    StatusProvider statusProvider = NULL;
    std::wstring shownStatus;  // Avoids redrawing (and flickering) when nothing changed
    Settings settings;
    std::vector<std::wstring> deviceIds;  // Same order as the entries in the device list
};

// Fills the device list. The first entry is always "System default device".
void FillDeviceList(HWND combo, std::vector<std::wstring>& deviceIds) {
    deviceIds.clear();
    deviceIds.push_back(L"");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"System default device");

    IMMDeviceEnumerator* enumerator = NULL;
    if (FAILED(CreateDeviceEnumerator(&enumerator))) return;

    IMMDeviceCollection* devices = NULL;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = NULL;
            if (FAILED(devices->Item(i, &device))) continue;

            LPWSTR id = NULL;
            const std::wstring name = DeviceFriendlyName(device);
            if (SUCCEEDED(device->GetId(&id)) && !name.empty()) {
                SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
                deviceIds.push_back(id);
            }
            if (id) CoTaskMemFree(id);
            device->Release();
        }
        devices->Release();
    }
    enumerator->Release();
}

// Shows the latest playback status, if it has changed.
void RefreshStatus(HWND dialog, DialogState& state) {
    const std::wstring status = state.statusProvider ? state.statusProvider() : L"";
    if (status == state.shownStatus) return;
    state.shownStatus = status;
    SetDlgItemTextW(dialog, IDC_STATUS, status.c_str());
}

// Lists the exclusive-mode formats of the device chosen in the device list.
void RefreshFormats(HWND dialog, DialogState& state) {
    const int selected = (int)SendDlgItemMessageW(dialog, IDC_DEVICE, CB_GETCURSEL, 0, 0);
    const std::wstring deviceId =
        (selected > 0 && selected < (int)state.deviceIds.size()) ? state.deviceIds[selected] : L"";

    // Checking a Bluetooth device can take a moment, so show that something is happening.
    SetDlgItemTextW(dialog, IDC_FORMATS, L"Checking...");
    UpdateWindow(GetDlgItem(dialog, IDC_FORMATS));  // Draw it now; the check below blocks
    HCURSOR previous = SetCursor(LoadCursor(NULL, IDC_WAIT));

    std::wstring text;
    IMMDeviceEnumerator* enumerator = NULL;
    HRESULT hr = CreateDeviceEnumerator(&enumerator);
    std::vector<RateSupport> rates;
    if (SUCCEEDED(hr)) {
        hr = ProbeExclusiveFormats(enumerator, deviceId, 2, rates);
        enumerator->Release();
    }
    text = DescribeExclusiveFormats(hr, rates);

    SetCursor(previous);
    SetDlgItemTextW(dialog, IDC_FORMATS, text.c_str());
}

// Copies the settings into the window's controls.
void PopulateControls(HWND dialog, DialogState& state) {
    HWND combo = GetDlgItem(dialog, IDC_DEVICE);
    FillDeviceList(combo, state.deviceIds);

    int selected = 0;
    for (size_t i = 1; i < state.deviceIds.size(); ++i) {
        if (state.deviceIds[i] == state.settings.deviceId) selected = (int)i;
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);

    CheckDlgButton(dialog, IDC_EXCLUSIVE, state.settings.exclusive ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_FALLBACK, state.settings.fallback ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_VOLUME, state.settings.volume ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(dialog, IDC_RINGMS, state.settings.ringMs, FALSE);
    SetDlgItemInt(dialog, IDC_PERIODMS, state.settings.periodMs, FALSE);

    // The fallback option only means something when exclusive mode is requested.
    EnableWindow(GetDlgItem(dialog, IDC_FALLBACK), state.settings.exclusive);

    RefreshStatus(dialog, state);
    RefreshFormats(dialog, state);
    SetTimer(dialog, kStatusTimer, kStatusRefreshMs, NULL);
}

// Reads the window's controls and saves the result.
void SaveControls(HWND dialog, DialogState& state) {
    Settings& settings = state.settings;
    const int selected = (int)SendDlgItemMessageW(dialog, IDC_DEVICE, CB_GETCURSEL, 0, 0);
    settings.deviceId = (selected > 0 && selected < (int)state.deviceIds.size()) ? state.deviceIds[selected] : L"";
    settings.exclusive = IsDlgButtonChecked(dialog, IDC_EXCLUSIVE) == BST_CHECKED;
    settings.fallback = IsDlgButtonChecked(dialog, IDC_FALLBACK) == BST_CHECKED;
    settings.volume = IsDlgButtonChecked(dialog, IDC_VOLUME) == BST_CHECKED;
    settings.ringMs = (int)GetDlgItemInt(dialog, IDC_RINGMS, NULL, FALSE);
    settings.periodMs = (int)GetDlgItemInt(dialog, IDC_PERIODMS, NULL, FALSE);
    SaveSettings(state.iniPath, ClampSettings(settings));
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    // Windows passes our state pointer once; it is stored with the window for later messages.
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        PopulateControls(dialog, *reinterpret_cast<DialogState*>(lParam));
        return TRUE;
    }

    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!state) return FALSE;

    if (message == WM_TIMER && wParam == kStatusTimer) {
        RefreshStatus(dialog, *state);
        return TRUE;
    }
    if (message == WM_DESTROY) {
        KillTimer(dialog, kStatusTimer);
        return FALSE;
    }
    if (message != WM_COMMAND) return FALSE;

    switch (LOWORD(wParam)) {
    case IDC_DEVICE:
        if (HIWORD(wParam) == CBN_SELCHANGE) RefreshFormats(dialog, *state);
        return TRUE;
    case IDC_EXCLUSIVE:
        EnableWindow(GetDlgItem(dialog, IDC_FALLBACK), IsDlgButtonChecked(dialog, IDC_EXCLUSIVE) == BST_CHECKED);
        return TRUE;
    case IDOK:
        SaveControls(dialog, *state);
        EndDialog(dialog, IDOK);
        return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

void ShowConfigDialog(HINSTANCE instance, HWND parent, const std::wstring& iniPath,
                      StatusProvider statusProvider) {
    DialogState state;
    state.iniPath = iniPath;
    state.statusProvider = statusProvider;
    state.settings = LoadSettings(iniPath);

    const HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), parent, DialogProc, (LPARAM)&state);
    if (SUCCEEDED(comResult)) CoUninitialize();
}
