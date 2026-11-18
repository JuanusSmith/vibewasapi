// device_monitor.h - Listens for Windows audio device changes.
//
// Windows calls this object when a device is plugged in, removed, or chosen as the
// new system default. This is what lets playback react as soon as Bluetooth
// headphones return, instead of repeatedly guessing whether they are back.
//
// Windows delivers these callbacks on its own thread and forbids them from
// blocking or making audio API calls. So each callback only raises a flag and wakes
// the render thread, which does the real work.
#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <atomic>

class DeviceMonitor : public IMMNotificationClient {
public:
    // `wakeEvent` is signalled after each change. It must outlive this object.
    explicit DeviceMonitor(HANDLE wakeEvent) : wakeEvent_(wakeEvent) {}

    // Render thread: returns true once for each batch of device changes.
    bool consumeDevicesChanged() { return devicesChanged_.exchange(false); }

    // Render thread: returns true once after the system default device changes.
    bool consumeDefaultChanged() { return defaultChanged_.exchange(false); }

    // IUnknown: standard COM reference counting.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IMMNotificationClient: the callbacks Windows makes.
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR deviceId) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) override;

private:
    void signalDevicesChanged();

    HANDLE wakeEvent_;
    std::atomic<ULONG> references_{1};
    std::atomic<bool> devicesChanged_{false};
    std::atomic<bool> defaultChanged_{false};
};
