# macOS desktop (experimental)

This branch adds a standalone browser UI backed by the same settings,
application coordinator, control API, and C++ DSP as the Decky plugin. Decky and
PipeWire are not required. Windows is not implemented by this branch.

## Prerequisites

- macOS 12 or later, Xcode Command Line Tools, CMake, and Python 3.10 or later.
- Node.js and pnpm matching the [developer guide](DEVELOPERS.md).
- [BlackHole 2ch](https://existential.audio/blackhole/), installed separately.
  Ear Tuner neither installs nor redistributes the driver.
- Stereo headphones. Start at a low listening volume.

This is a source-checkout desktop host, not a signed/notarized application or
installer. Live audio validation with BlackHole is required before release.

## Build and launch

Install the project's normal development dependencies, then:

```bash
pnpm run build:macos
pnpm run start:macos --list-devices
```

The second command prints device names, persistent UIDs, channel counts, and
sample rates. Copy the UID of your **physical headphone output**, not BlackHole
or an aggregate/multi-output device. In Audio MIDI Setup, set BlackHole 2ch and
that output to the same sample rate (48,000 Hz is recommended).

```bash
pnpm run start:macos --output 'YOUR_PHYSICAL_OUTPUT_UID'
```

Select **BlackHole 2ch** as the macOS sound output (or as the output in just the
applications to process). Open <http://127.0.0.1:5173/>. Enable Ear Tuner and
adjust the shared controls. A different browser port can be passed with
`--port 5174`.

The equivalent launcher without pnpm is:

```bash
PYTHONPATH=src python3 -m ear_tuner.macos_desktop --output 'YOUR_PHYSICAL_OUTPUT_UID'
```

If macOS asks for audio-input/microphone permission, grant it to the launching
application (for example Terminal). Only the BlackHole input streams are
enabled; physical microphone streams are disabled. Re-launch after granting
permission if startup timed out.

## Routing and lifecycle

```text
Applications -> BlackHole 2ch -> shared Ear Tuner DSP -> physical headphones
Browser UI -> Python desktop service -> localhost native control API
```

The backend creates a process-private aggregate device with BlackHole first and
the physical output as clock source. Core Audio drift compensation synchronizes
BlackHole to that output. Only BlackHole input and physical output streams are
enabled; no processed audio is sent back to BlackHole. Processing uses bounded,
preallocated float32 stereo buffers, with no Python or HTTP work on the audio
thread. Unsupported formats or buffer sizes fail explicitly.

Ear Tuner **never changes the system default output**, sample rates of the
individual devices, or application routing. Do not create a multi-output device
that sends dry audio directly to the headphones: that would mix dry and
processed audio.

- Disable in the UI for dry passthrough while the backend remains running.
- Ctrl+C stops the web service and its owned native child and removes the
  private aggregate. **Select your physical output again in macOS Sound settings
  when you exit**, otherwise applications still routed to BlackHole will be
  inaudible.
- Disconnecting a device, changing sample rates, or losing audio callbacks
  causes the backend to exit with an error. The desktop health monitor retries
  the same selected UIDs; it does not switch to speakers or another device.
- To select different headphones, stop and relaunch with their UID.
- One desktop instance per user is allowed. Startup checks that native
  diagnostics match its child PID and random session before applying settings.

Settings are stored in `~/Library/Application Support/Ear Tuner/settings.json`.
Logs are in `~/Library/Logs/Ear Tuner/desktop.log` and `native.log`. Missing
BlackHole, unavailable devices, permissions, and control-port conflicts are
startup failures, not simulated audio success.

## Validation

```bash
cmake -S native -B native/build-current -DCMAKE_BUILD_TYPE=Release
cmake --build native/build-current
ctest --test-dir native/build-current --output-on-failure
PYTHONPATH=src .venv/bin/python -m unittest src.tests.test_macos_service src.tests.test_desktop_server -v
pnpm run typecheck
pnpm test
```

Native tests cover planar/interleaved stereo, channel isolation, invalid
buffers, partial frames, arithmetic-overflow layouts, latched errors, and shared
DSP/control behavior. The deterministic stress matrix compares callback output
with the portable processor across four sample rates and block sizes from one
frame through the 16,384-frame limit. Concurrent control updates are checked for
stereo isolation.

Python tests cover owned-process readiness, startup failure, cancellation,
shutdown, and desktop HTTP lifecycle. Controlled helper processes and real
localhost HTTP requests additionally exercise concurrent starts/stops, readiness
deadlines, forced termination, crash/restart recovery with saved controls, and
malformed/foreign diagnostics. Incomplete desktop request bodies time out with
HTTP 408 rather than blocking shutdown indefinitely while idle. These helper
processes simulate the native protocol; they do not exercise audio devices.

For sanitizer runs, see the [native guide](../native/README.md).
AddressSanitizer/UndefinedBehaviorSanitizer can exercise the callback tests;
ThreadSanitizer checks the concurrent control case, and RealtimeSanitizer checks
that callbacks avoid prohibited real-time operations. RealtimeSanitizer requires
a supporting Clang distribution; Apple's compiler may not provide its runtime.
Select that same compiler for both `CMAKE_CXX_COMPILER` and
`CMAKE_OBJCXX_COMPILER` when configuring an instrumented macOS build.

For real-device acceptance, test the listening sample and another application,
per-ear settings, dry passthrough, extended playback without drift/dropouts,
device disconnect/reconnect, sample-rate changes, permission denial, duplicate
launches, and Ctrl+C/SIGTERM cleanup. Confirm that no private aggregate remains
and that manually restoring the physical output restores normal audio.
