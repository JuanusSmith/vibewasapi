# Vibe Wasapi (out_vibewasapi.dll) — WASAPI output plug-in for Winamp / WACUP

## Features
- **Exclusive mode** (event-driven). It is bit-perfect when volume is 100% and pan is centred. The plug-in tries the source format first (16 / 24 / 24-in-32 / 32 / float) and uses the next best format the device accepts.
- **Shared mode** uses the Windows mixer and resampler (AUTOCONVERTPCM), so any sample rate plays.
- If exclusive mode fails, for example because the device is busy or the format is unsupported, it can fall back to shared mode automatically.
- Lets you pick the output device, and set the buffer length and device period.
- Applies Winamp volume and pan in software. You can turn this off for strict bit-perfect playback.
- Uses MMCSS "Pro Audio" thread priority.
- Seek and pause positions are accurate because it reads the WASAPI clock (IAudioClock).
- **Automatic device recovery.** If the device disconnects, playback waits and continues from the same position as soon as the device returns, however long that takes.
- If the device disconnects while paused, such as Bluetooth headphones turning off, it reconnects when you unpause.
- When set to **System default device**, playback follows Windows as the default output changes.
- **Gapless playback.** When the next track has the same sample rate, channels and bit depth, it continues on the same open stream with no gap or click. Other tracks still play their ending in full before the device reopens.
- **Click-free pause, seek and stop.** The sound fades out over 15 ms before stopping and fades back in on resume. Normal playback is untouched, so it stays bit-perfect.
- **Dither.** When audio has to lose detail, such as a 24-bit track on a device that only accepts 16-bit, or volume applied in software, it adds TPDF dither, so quiet passages and fade-outs stay clean. Bit-perfect playback is never touched.
- **Clear status in the settings window.** The Now playing box shows the device, exclusive or shared mode, the format, and whether playback is bit-perfect or dithered. It updates live. If exclusive mode was requested but not used, it says why. A second box lists the formats the selected device accepts in exclusive mode.

## Install
1. Copy `out_vibewasapi.dll` into the `Plugins` folder of Winamp or WACUP:
   - x86 build → Winamp and standard WACUP
   - x64 build → 64-bit WACUP only
2. Go to Preferences → Plug-ins → Output, select **Vibe Wasapi**, then click Configure.

Settings are saved in `winamp.ini` under `[out_vibewasapi]`. They take effect from the next track.

## Build
**Visual Studio** (2019 or later, Developer prompt):
```
cmake -B build -A Win32
cmake --build build --config Release
```
Use `-A x64` for a 64-bit build. Both builds link the C runtime statically, so no redistributable is needed.

**MinGW-w64** (Linux or MSYS2):
```
make        # 32-bit  → build/x86/out_vibewasapi.dll
make x64    # 64-bit  → build/x64/out_vibewasapi.dll
```

## Test
The automated tests need no sound card. After a Visual Studio build:
```
ctest --test-dir build -C Release --output-on-failure
```
GitHub runs the same build and tests for both architectures on every push to `main`, every version tag, and every pull request.

`vibe_smoke_host.exe` is a manual test that loads the finished DLL and plays silence on a real device:
```
build\Release\vibe_smoke_host.exe build\Release\out_vibewasapi.dll
```
In exclusive mode the plug-in doesn't resample, so the device must accept the test's sample rate. Add a rate the device supports, for example `48000`, to run every test at that rate.

## How it works
Winamp's threads and the audio thread are kept apart, so a slow device can't freeze the player and a busy player can't interrupt the sound.

1. Winamp's decoder writes audio into a lock-free ring buffer.
2. Pause and seek requests are recorded as the desired state, so the audio thread can't miss one, even while it is reconnecting a device.
3. One MTA render thread makes every playback call to WASAPI. It wakes on the device's event, converts audio from the ring buffer into the device format, and fills any gap with silence.
4. A device monitor receives Windows notifications when devices appear, disappear, or become the default. The render thread reconnects accordingly.
5. The plug-in records how much silence went out before real audio. That lets it turn the device clock position into song time exactly, so `GetOutputTime` and `IsPlaying` (used for end-of-track drain) stay correct even after underruns.
6. Pause, seek and stop first fade the sound to silence and wait until the faded audio has played. Only then is the device stopped and cleared, and playback fades back in if it continues.
7. For gapless playback, `IsPlaying` reports the end of a track while about 500 ms of audio is still queued, so Winamp starts the next track early. The stream keeps playing that audio after `Close`. If the next `Open` has the same format and settings, the new track's audio follows straight on. Otherwise the old track finishes first and a fresh stream starts.

### Source files
| File | Purpose |
|---|---|
| `src/plugin.cpp` | The functions Winamp calls, and how they stay thread-safe |
| `src/render_engine.*` | The real-time audio thread, fades, gapless handoff and device recovery |
| `src/fader.h` | The short volume ramps used for click-free pause, seek and stop |
| `src/wasapi_device.*` | One open connection to a Windows audio device |
| `src/device_monitor.*` | Windows notifications about device changes |
| `src/spsc_ring.h` | Lock-free audio buffer between the decoder and audio thread |
| `src/stream_control.h` | Pause and seek requests between threads |
| `src/position_tracker.h` | Converts the device clock into song time |
| `src/track_timeline.h` | The song time Winamp sees, across seeks and gapless track changes |
| `src/sample_convert.*` | Audio format conversion, volume, pan and dither |
| `src/playback_status.*` | The plain-language status and format list in the settings window |
| `src/settings.*` | Loading and saving options |
| `src/config_dialog.*` | The settings window |
| `src/version.h` | The version number used throughout the plug-in |
| `tests/` | Automated tests and the manual smoke test |

## Ideas for future versions
- Reopen the device immediately when settings change, instead of waiting for the next track.
- Add an option for 24-bit and higher output in shared mode when the device can't use exclusive mode.

## License

MIT. See [LICENSE](LICENSE).
