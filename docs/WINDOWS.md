# Windows desktop preview

This source-built prototype uses the existing browser UI, Python application
service, and portable DSP. It is not part of the submitted Decky release, an
installer, or a signed desktop distribution. Live Windows audio acceptance is
still required; cross-compilation alone does not verify driver behavior.

## Requirements and build

- Windows 10/11, x64, stereo headphones.
- [VB-CABLE](https://vb-audio.com/Cable/) installed separately using the
  vendor's instructions, including its requested reboot. Only driver
  installation needs administrator privileges. Ear Tuner does not bundle or
  install the driver.
- Python 3.12+, Node.js supported by the repository, pnpm 9, CMake, and Visual
  Studio Build Tools with **Desktop development with C++** and a Windows SDK.

In PowerShell at the repository root:

```powershell
pnpm install --frozen-lockfile
pnpm run typecheck
pnpm run build:web
cmake -S native -B native/build-current -A x64
cmake --build native/build-current --config Release --parallel
$env:PYTHONPATH = "src"
python -m ear_tuner.windows_desktop --list-devices
```

Stop if any build command fails. Device discovery prints JSON containing `uid`,
`name`, `direction`, and `virtual_cable`. Copy UIDs, not friendly names. Choose
the **capture** endpoint named **CABLE Output (VB-Audio Virtual Cable)** as
input and your physical headphones' **render** endpoint as output:

```powershell
python -m ear_tuner.windows_desktop --input '<CABLE Output UID>' --output '<headphone UID>'
```

Open <http://127.0.0.1:5173/>. `--port` changes only the browser server port;
the native control server uses loopback port 19285. Do not start another Ear
Tuner backend on that port.

## Route audio

1. Connect the selected headphones and start Ear Tuner.
2. In Windows Sound settings, set application/system **playback** to **CABLE
   Input (VB-Audio Virtual Cable)**. The names are intentionally opposite:
   playback goes into CABLE Input and Ear Tuner captures CABLE Output.
3. Keep Windows' **Listen to this device** disabled for CABLE Output. Do not
   route processed output back into any virtual cable or use speakers.
4. Tune using the browser UI. Disabling Ear Tuner bypasses DSP while forwarding
   audio; the native bridge must remain running.
5. Before stopping or disconnecting headphones, restore Windows playback to
   physical headphones. Press **Ctrl+C** in the launcher terminal to stop.
   Closing the browser tab does not stop the launcher.

Ear Tuner never changes system default devices or selects physical microphones.
Input selection requires VB-CABLE's original friendly name. Selecting VB-CABLE
as the processed output is rejected. Other virtual render devices are not
identified automatically: choose physical headphones explicitly.

Windows privacy settings may need **Microphone access** and **Let desktop apps
access your microphone** enabled for the virtual recording endpoint. No elevated
launcher is required.

## Runtime and troubleshooting

- Event-driven shared-mode WASAPI capture and render use stereo float32 at 48
  kHz. Windows' high-quality format conversion handles other supported device
  mix rates. Both endpoints must expose stereo capability.
- A bounded, preallocated FIFO runs the existing planar DSP. A separate,
  non-realtime COM thread uses `IAudioClockAdjustment` to correct independent
  device-clock drift within +/-0.5%; no clock adjustment or allocation occurs in
  the DSP/FIFO processing functions.
- The FIFO targets about 20 ms of queued audio in addition to Windows/driver
  buffering. This is not a measured end-to-end latency guarantee. Startup and
  discontinuities re-prime with silence; queue underruns/overflows are reported
  in diagnostics rather than replaying stale audio.
- Device invalidation, unsupported formats, stalled endpoints, and worker
  failures stop the backend. The desktop monitor retries the selected IDs and
  reapplies settings without switching endpoint IDs. Windows/driver routing
  inside a combined speaker/headphone endpoint remains Windows-controlled.
- Settings: `%LOCALAPPDATA%\Ear Tuner\settings.json`.
- Logs: `%LOCALAPPDATA%\Ear Tuner\logs\desktop.log` and `native.log`.
- A per-user Windows byte-range lock prevents concurrent desktop launchers.
  Readiness checks require the owned child PID, session, platform, and live
  audio.
- `http://127.0.0.1:19285/api/v1/diagnostics` reports readiness, FIFO occupancy,
  requested render rate, underruns, overflows, and capture discontinuities.
- If initialization fails, inspect the HRESULT in `native.log`; check that
  Windows Audio is running, both devices are active, recording access is
  allowed, and no application has taken either device exclusively.

## Validation

```powershell
ctest --test-dir native/build-current -C Release --output-on-failure --timeout 120
$env:PYTHONPATH = "src"
python -m unittest tests.test_windows_service tests.test_windows_desktop tests.test_desktop_server tests.test_application_service tests.test_backend_client -v
```

The Windows CI workflow builds with the Windows SDK and runs native Winsock,
portable DSP/FIFO, subprocess, browser API, and real singleton-lock tests. The
portable FIFO tests include ten simulated minutes each at -1000, 0, and +1000
ppm clock drift. They do not replace a real driver/audio test.

Before calling this Windows-ready, test on Windows with VB-CABLE and headphones:

- Device discovery, left/right identity, all focus modes, disabled DSP, and the
  browser listening sample.
- At least an hour of playback with diagnostics checked for growing occupancy,
  repeated underruns/overflows, glitches, or unexpected pitch changes.
- Supported device mix rates, measured latency, recording permission denial, and
  exclusive-device contention.
- Unplug/replug, sleep/resume, endpoint changes, backend crashes, startup
  failures, Ctrl+C, and a second launcher. Verify no orphan backend remains.
- Restore physical playback after exit and confirm normal audio is restored.
