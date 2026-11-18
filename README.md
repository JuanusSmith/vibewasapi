# Vibe Wasapi (out_vibewasapi.dll) — WASAPI output plug-in for Winamp / WACUP

## Features
- **Exclusive mode** (event-driven). It is bit-perfect when volume is 100% and pan is centred. The plug-in tries the source format first (16 / 24 / 24-in-32 / 32 / float) and uses the next best format the device accepts.
- **Shared mode** uses the Windows mixer and resampler (AUTOCONVERTPCM), so any sample rate plays.
- If exclusive mode fails, for example because the device is busy or the format is unsupported, it can fall back to shared mode automatically.
- Lets you pick the output device, and set the buffer length and device period.
- Applies Winamp volume and pan in software. You can turn this off for strict bit-perfect playback.
- Uses MMCSS "Pro Audio" thread priority.
- Seek and pause positions are accurate because it reads the WASAPI clock (IAudioClock).
- If the device is unplugged or the endpoint changes, it reopens the device automatically, retrying up to 5 times.
- If the device disconnects while paused, such as Bluetooth headphones turning off, it reopens the device when you unpause and resumes from the same position.

## Install
1. Copy `out_vibewasapi.dll` into the `Plugins` folder of Winamp or WACUP:
   - x86 build → Winamp and standard WACUP
   - x64 build → 64-bit WACUP only
2. Go to Preferences → Plug-ins → Output, select **Vibe Wasapi**, then click Configure.

Settings are saved in `winamp.ini` under `[out_vibewasapi]`. They take effect from the next track.

## Build
**MinGW-w64** (Linux or MSYS2):
```
make        # 32-bit  → build/x86/out_vibewasapi.dll
make x64    # 64-bit  → build/x64/out_vibewasapi.dll
```
**Visual Studio** (2019 or later, Developer prompt):
```
cmake -B build -A Win32
cmake --build build --config Release
```
Use `-A x64` for a 64-bit build. Both builds link the C runtime statically, so no redistributable is needed.

## How it works
- Winamp's threads only write to a lock-protected ring buffer. They send Pause and Flush as synchronous commands.
- One MTA render thread makes every IAudioClient call. It wakes on the WASAPI event, converts audio from the ring buffer into the device format, and fills any gap with silence.
- The plug-in records how much silence went out before real audio. That lets it turn the device clock position into song time exactly, so `GetOutputTime` and `IsPlaying` (used for end-of-track drain) stay correct even after underruns.

## Ideas for future versions
- Reopen the device immediately when settings change, instead of waiting for the next track.
- Add TPDF dither when the device format has fewer bits than the source (rare: only when a device accepts 16-bit and nothing else).
- Add an option for 24-bit and higher output in shared mode when the device can't use exclusive mode.
- Add a unicode description (OUT_VER_U) for newer WACUP builds.

## License

MIT. See [LICENSE](LICENSE).
