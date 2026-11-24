# macOS desktop (experimental)

This branch adds a standalone browser UI backed by the same settings,
application coordinator, control API, and C++ DSP as the Decky plugin. Decky,
PipeWire, and third-party audio drivers are not required. Windows is not
implemented by this branch.

## Prerequisites

- macOS 14.2 or later, Xcode Command Line Tools, CMake, and Python 3.10 or
  later.
- Node.js and pnpm matching the [developer guide](DEVELOPERS.md).
- Stereo headphones or another stereo output. Start at a low listening volume.

This is a source-checkout desktop host, not a signed/notarized application or
installer. Live system-audio validation is required before release.

## Build and launch

Install the project's normal development dependencies, then:

```bash
pnpm run build:macos
pnpm run start:macos --list-devices
pnpm run start:macos
```

The device list reports names, persistent UIDs, channel counts, and sample
rates. Ear Tuner captures audio sent to the current macOS default output and
plays the processed result through that same output. Use macOS Sound settings to
choose the output you want to listen through; Ear Tuner does not change it. Open
<http://127.0.0.1:5173/> and enable Ear Tuner. A different browser port can be
passed with `--port 5174`.

The equivalent launcher without pnpm is:

```bash
cd src && PYTHONPATH=. python3 -m ear_tuner.macos_desktop
```

Approve macOS's system-audio capture prompt for the launching application (for
example, Terminal). The Core Audio process tap is device-specific: audio routed
to a different per-app output is not included.

## Routing and lifecycle

```text
Applications -> current default output's Core Audio process tap
             -> shared Ear Tuner DSP -> that same output
Browser UI -> Python desktop service -> localhost native control API
```

The backend creates a process-private aggregate device with a tap on the current
default output and that output as its clock source. While the tap is active,
Core Audio mutes the original tapped output to avoid mixing dry and processed
audio. The processed signal is sent to the same endpoint. Ear Tuner never
changes the system default output, application routing, or the output device's
sample rate.

When the macOS default output changes, the backend exits and the desktop health
monitor restarts it against the new endpoint. Disconnecting the output, changing
its sample rate, losing callbacks, or denying capture permission causes an
explicit startup/runtime failure; the service does not silently switch to
another output. Stop with Ctrl+C. The private tap and aggregate are removed, and
normal audio resumes through the same macOS output.

- Disable in the UI for dry passthrough while the backend remains running.
- Use **Stop Audio** in the desktop UI to stop the backend and release the
  system-audio tap. **Start Audio** starts it again with the saved settings. The
  desktop host and browser page remain open.
- To capture app audio routed somewhere other than the macOS default output,
  change that app's output route back to the default output.
- Only one desktop instance per user is allowed. Startup checks that native
  diagnostics match its child PID and random session before applying settings.

Settings are stored in `~/Library/Application Support/Ear Tuner/settings.json`.
Logs are in `~/Library/Logs/Ear Tuner/desktop.log` and `native.log`. Missing
devices, unsupported output formats, permissions, and control-port conflicts are
startup failures, not simulated audio success.

## Validation

```bash
cmake -S native -B native/build-current -DCMAKE_BUILD_TYPE=Release
cmake --build native/build-current
ctest --test-dir native/build-current --output-on-failure
(cd src && PYTHONPATH=. ../.venv/bin/python -m unittest tests.test_macos_service tests.test_desktop_server -v)
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
malformed/foreign diagnostics. These helper processes simulate the native
protocol; they do not exercise audio devices.

For sanitizer runs, see the [native guide](../native/README.md).
AddressSanitizer/UndefinedBehaviorSanitizer can exercise the callback tests;
ThreadSanitizer checks the concurrent control case, and RealtimeSanitizer checks
that callbacks avoid prohibited real-time operations. RealtimeSanitizer requires
a supporting Clang distribution; Apple's compiler may not provide its runtime.
Select that same compiler for both `CMAKE_CXX_COMPILER` and
`CMAKE_OBJCXX_COMPILER` when configuring an instrumented macOS build.

For real-device acceptance, test the listening sample and another application,
per-ear settings, dry passthrough, extended playback without dropouts, default
output changes, device disconnect/reconnect, permission denial, duplicate
launches, and Ctrl+C/SIGTERM cleanup. Confirm that no private aggregate remains
and that normal output resumes when Ear Tuner stops.
