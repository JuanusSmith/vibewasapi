// config_dialog.cpp - Implements the settings window.
//
// The window edits its own private copy of the settings and saves them to the ini
// file when the user clicks OK. A playing stream is unaffected; the next stream reads
// the new values when it opens.
#include "config_dialog.h"

#include <objbase.h>
#include <propidl.h>
#include <mmdeviceapi.h>
#include <cstdio>
#include <vector>

#include "resource.h"
#include "settings.h"
#include "wasapi_device.h"

namespace {

// Windows property that holds a device's display name, such as "Speakers (USB Audio)".
const PROPERTYKEY kPKEY_DeviceFriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

// Data that belongs to one open settings window.
struct DialogState {
    std::wstring iniPath;
    std::wstring statusText;
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
            IPropertyStore* properties = NULL;
            if (SUCCEEDED(device->GetId(&id)) && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(properties->GetValue(kPKEY_DeviceFriendlyName, &name)) && name.vt == VT_LPWSTR) {
                    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)name.pwszVal);
                    deviceIds.push_back(id);
                }
                PropVariantClear(&name);
            }
            if (properties) properties->Release();
            if (id) CoTaskMemFree(id);
            device->Release();
        }
        devices->Release();
    }
    enumerator->Release();
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

    wchar_t status[256];
    _snwprintf(status, 255, L"%s\nChanges apply from the next track.", state.statusText.c_str());
    status[255] = 0;
    SetDlgItemTextW(dialog, IDC_STATUS, status);
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
    if (message != WM_COMMAND || !state) return FALSE;

    switch (LOWORD(wParam)) {
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
                      const std::wstring& statusText) {
    DialogState state;
    state.iniPath = iniPath;
    state.statusText = statusText;
    state.settings = LoadSettings(iniPath);

    const HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), parent, DialogProc, (LPARAM)&state);
    if (SUCCEEDED(comResult)) CoUninitialize();
}
