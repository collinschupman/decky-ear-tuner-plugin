# Architecture

Ear Tuner is a Steam Deck Decky Loader plugin with a native PipeWire audio
runtime. The repository separates reusable audio processing from product
behavior, host integration, and user interfaces.

## Source modules

```text
src/dsp/
  Reusable DSP algorithms, policies, and DSP-focused tests.

src/native/core/ear_tuner/
  Platform-independent Ear Tuner behavior, control state, and DSP composition.

src/native/core/http/
  Platform-independent parsing of the versioned control request.

src/native/platform/posix/http/
  POSIX socket transport for the native control API.

src/native/platform/linux/pipewire/
  PipeWire smart-filter streams and real-time audio transfer.

src/native/platform/linux/main.cpp
  Linux/PipeWire runtime startup and shutdown.

src/ear_tuner/
  Python settings validation, application coordination, and external adapters.

main.py
  Root Decky entrypoint, Python import setup, lifecycle, and RPC.

service.sh
  Native runtime lifecycle and readiness checks.

src/frontend/decky/
  Production Decky UI shipped with the plugin.

src/frontend/test-web/
  Browser adapters for the shared production UI.
```

The intended dependency direction is:

```text
dsp
  ^
  |
ear_tuner_core
  ^
  |
native platform adapter
  ^
  |
Decky Python backend and service lifecycle
```

The `src/dsp/` and `src/native/core/` modules must remain independent of
PipeWire, POSIX sockets, Decky APIs, filesystem state, and plugin persistence.
Platform adapters compose the portable core with each host's audio and transport
APIs.

## Runtime flow

1. The Decky UI calls `main.py` through `@decky/api`.
2. `main.py` persists settings and starts or stops `service.sh`.
3. `service.sh` launches the native PipeWire runtime without changing the
   default sink or moving application streams.
4. The native runtime publishes a paired WirePlumber smart filter: an
   application-facing `Audio/Sink` stream and an automatically scheduled
   `Stream/Output/Audio` stream. A lock-free stereo buffer carries processed
   audio between them.
5. WirePlumber owns both external routes, so applications pass through Ear Tuner
   while the selected physical output remains visible and follows normal system
   policy. The plugin watches the effective default sink and restarts the filter
   when that output changes, then reapplies the persisted DSP state. The native
   runtime exposes the versioned local HTTP control API.
6. Disabling Ear Tuner ramps to the dry signal while keeping the smart filter
   active. Disabling or reloading the Decky plugin, explicit stop, and uninstall
   stop the backend, which removes its filter nodes without changing the
   selected physical output.

Settings application, recovery, and explicit lifecycle operations are serialized
on the coordinator's event loop. Stop and uninstall prevent later health polls
from restarting the service. Failed control updates leave persisted settings
unchanged; the UI restores the last acknowledged settings and displays an
explicit failure message. Frontend writes are ordered so a delayed response
cannot overwrite a newer change. The desktop preview dispatches all plugin calls
to one shared event loop, including calls from separate HTTP threads.

## Build targets

- `ear_tuner_dsp`: header-only CMake interface target for reusable DSP.
- `test_dsp`: tests owned by the DSP module under `src/dsp/`.
- `ear_tuner_core`: portable Ear Tuner control and DSP composition library.
- `test_core`: platform-agnostic control, parsing, and DSP composition tests.
- `ear_tuner_posix_http`: POSIX socket HTTP control transport.
- `ear_tuner_linux_pipewire`: Linux PipeWire smart-filter adapter.
- `decky_ear_tuner_backend`: Linux/PipeWire runtime executable.

The portable DSP/core targets and tests do not require PipeWire. The POSIX HTTP
transport is separate from the core, and the Linux runtime is only enabled when
the PipeWire development package is available.

## Production boundary

The Decky UI and compiled native runtime ship with the plugin. The reviewed
release backend is tracked at `native/build/decky_ear_tuner_backend` because the
Deckbrew builder does not provide PipeWire development libraries; all other
native build output remains ignored. The Python application package ships with
the plugin; frontend source, C++ source, tests, and development tooling do not
ship in the production archive.

A production archive contains:

```text
LICENSE
package.json
plugin.json
main.py
service.sh
ear_tuner/
dist/index.js
dist/assets/
native/build/decky_ear_tuner_backend
```

Decky reads `package.json` to classify `dist/index.js` as an ES module. Omitting
that file makes Loader evaluate the bundle as a legacy script.

C++ source, tests, and development tooling remain repository-only. Build a
production archive with `pnpm run package:decky`. Packaging requires the Linux
backend target to build successfully before it can use an executable; an
existing tracked binary is not a fallback for missing PipeWire build
dependencies.

The archive layout differs from the checkout: packaging copies `src/ear_tuner/`
to the archive's root `ear_tuner/`. The root Python entrypoint supports both
layouts. The service prefers the development executable under
`native/build-current/` when present and otherwise uses the release executable.

See [DEVELOPERS.md](DEVELOPERS.md) for setup and checks, and the
[native backend guide](../native/README.md) for the control API and DSP details.
