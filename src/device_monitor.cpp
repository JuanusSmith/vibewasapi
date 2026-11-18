// device_monitor.cpp - Implements the Windows device callbacks in device_monitor.h.
#include "device_monitor.h"

namespace {

// Interface IDs are defined here so the project needs no extra import libraries.
const IID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIID_IMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};

}  // namespace

HRESULT STDMETHODCALLTYPE DeviceMonitor::QueryInterface(REFIID iid, void** object) {
    if (!object) return E_POINTER;
    if (IsEqualIID(iid, kIID_IUnknown) || IsEqualIID(iid, kIID_IMMNotificationClient)) {
        *object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *object = NULL;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeviceMonitor::AddRef() {
    return references_.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE DeviceMonitor::Release() {
    const ULONG remaining = references_.fetch_sub(1) - 1;
    if (remaining == 0) delete this;
    return remaining;
}

void DeviceMonitor::signalDevicesChanged() {
    devicesChanged_.store(true);
    SetEvent(wakeEvent_);
}

HRESULT STDMETHODCALLTYPE DeviceMonitor::OnDeviceStateChanged(LPCWSTR, DWORD) {
    signalDevicesChanged();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceMonitor::OnDeviceAdded(LPCWSTR) {
    signalDevicesChanged();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceMonitor::OnDeviceRemoved(LPCWSTR) {
    signalDevicesChanged();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceMonitor::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) {
    // Windows reports each role separately. We play using the "multimedia" role only.
    if (flow == eRender && role == eMultimedia) {
        defaultChanged_.store(true);
        SetEvent(wakeEvent_);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceMonitor::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) {
    return S_OK;  // Property changes do not affect playback.
}
