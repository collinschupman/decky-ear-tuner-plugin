# Backend (C++)

This guide is for contributors working on Ear Tuner's native audio engine. It
covers the DSP, PipeWire runtime, diagnostics, and advanced testing. For general
project setup, start with the [developer guide](../docs/DEVELOPERS.md).

Run the commands below from the repository root. Native implementation and tests
live in `src/native/`, reusable DSP lives in `src/dsp/`, and
`native/CMakeLists.txt` remains the build entrypoint.

The production Linux backend is a paired PipeWire `pw_stream` smart filter with
planar left/right audio. PipeWire invokes its DSP callbacks in the graph's
real-time cycle, and a fixed-size lock-free stereo ring buffer transfers audio
between the ingress and output streams. Independent zero-lookahead peak limiters
cap fully focused output at `-1 dBFS`, after output gain, with immediate
attenuation and a 100 ms release. An ear set to None skips limiting; globally
disabled processing and None for both ears fade to the dry signal over about 20
ms. Dry audio still follows the native API's output-gain control.

Both streams leave the sample rate unspecified so PipeWire can negotiate it from
the graph rather than requesting 48 kHz. DSP uses the negotiated ingress
format's rate, including subsequent format changes, so crossover frequencies and
dynamics timing match the samples being processed. Ingress honors each plane's
chunk offset, including wraparound, and clamps chunk sizes to the mapped
capacity before processing.

The focused path uses four-channel wide dynamic range compression (WDRC). Three
residual crossovers split each ear at 250 Hz, 1 kHz, and 4 kHz, and the bands
reconstruct exactly at unity gain. Each band uses a 2:1 compression ratio with
approximately 5 ms attack and 100 ms release. Mode and strength select the
maximum low-level gain in each band; louder signals receive progressively less
gain above the shared compression knee.

Build and test:

```bash
cmake -S native -B native/build-current -DCMAKE_BUILD_TYPE=Release
cmake --build native/build-current -j2
ctest --test-dir native/build-current --output-on-failure
```

## Check real-time safety

Audio callbacks must not allocate or free heap memory, lock mutexes, or perform
blocking I/O. The optional
[RealtimeSanitizer](https://clang.llvm.org/docs/RealtimeSanitizer.html) build
checks the DSP entrypoint and both PipeWire process callbacks, including calls
made underneath them.

Use a Clang toolchain with `-fsanitize=realtime` support and the matching
compiler-rt runtime. Linux CI uses Clang 21. Configuration fails if the
compiler, annotation, or runtime is unavailable rather than silently skipping
the checks.

```bash
cmake -S native -B native/build-current/rtsan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++-21 \
  -DEAR_TUNER_ENABLE_RTSAN=ON
cmake --build native/build-current/rtsan -j2
ctest --test-dir native/build-current/rtsan --output-on-failure
```

These tests cover first-use processing, mode and strength changes, bypass,
sample-rate and tuning changes, and different block sizes. A positive-control
test deliberately allocates in a marked callback and requires the expected
sanitizer report, so a disabled guard can't pass unnoticed.

To check the actual callbacks, run the instrumented backend on a Linux host with
a live PipeWire session:

```bash
target_node="$(pactl get-default-sink)" &&
  EAR_TUNER_TARGET_NODE="$target_node" \
    ./native/build-current/rtsan/decky_ear_tuner_backend
```

Don't run it alongside the installed plugin or another Ear Tuner instance.
Verify the target is the intended physical output, check routing, exercise
processing and output changes, and stop with Ctrl+C. A sanitizer violation
terminates this testing instance.

The nested build directory keeps the instrumented executable separate from the
one selected by `service.sh`. Normal builds and release packaging leave RTSan
off. Don't ship the instrumented executable. RTSan checks executed paths and
intercepted unsafe operations; passing tests are a regression guard, not proof
that every path is allocation-free or always meets its timing deadline.

### Profile DSP performance

The opt-in `bench_dsp` target uses Catch2 benchmarks to time each DSP module,
`EarTunerProcessor::Process`, and the full `ProcessAudioBlock` entrypoint. Each
result is the time to process one block at 48 kHz, so compare it with the
block's real-time budget: 1333 us for 64 frames, 5333 us for 256 frames, and
21333 us for 1024 frames. Mono modules process one channel. The processor and
block benchmarks process both channels.

```bash
cmake -S native -B native/build-current/bench -DCMAKE_BUILD_TYPE=Release \
  -DEAR_TUNER_BUILD_BENCHMARKS=ON
cmake --build native/build-current/bench --target bench_dsp -j2
./native/build-current/bench/bench_dsp
```

Run one group by passing its tag: `[modules]`, `[processor]`, or `[block]`. Use
`--benchmark-samples` to change the sample count. Use `-r json` or `-r xml` for
machine-readable output. Benchmark in a Release build on the target hardware,
such as a Steam Deck, because results on a development machine won't match
on-device timing.

Targets:

- `ear_tuner_dsp` and `test_dsp`: reusable DSP and its tests.
- `ear_tuner_core`: portable Ear Tuner state, parsing, and DSP composition.
- `test_core`: platform-agnostic native unit tests; does not require PipeWire.
- `ear_tuner_posix_http`: POSIX socket control API transport.
- `test_http_server`: POSIX HTTP transport tests.
- `ear_tuner_linux_pipewire`: Linux PipeWire smart-filter adapter.
- `test_pipewire_filter`: Linux PipeWire adapter tests.
- `decky_ear_tuner_backend`: production native DSP and HTTP control server.

The portable core and tests can be built on non-Linux hosts. PipeWire and the
runtime executable are enabled only on Linux when PipeWire development files are
available. The POSIX HTTP transport is isolated from the core so another
platform can provide its own transport or runtime adapter.

Use the repository service script rather than launching the filter alone. The
script starts the backend without changing the default output. The backend
publishes a paired WirePlumber smart filter, and WirePlumber routes applications
through it to the selected physical output. The Decky backend watches the
effective default sink and restarts the filter when devices change so the
processed stream follows the new output:

```bash
bash service.sh start
```

Do not run it alongside another Ear Tuner instance. Check and stop the service
when finished:

```bash
bash service.sh status
bash service.sh stop
```

Control endpoint:

```bash
curl "http://127.0.0.1:19285/api/v1/control?left=-3&right=1"
```

Runtime audio diagnostics are available without changing controls:

```bash
curl "http://127.0.0.1:19285/api/v1/diagnostics"
bash service.sh status
```

The JSON reports ingress and output callback counts, lifetime maximum callback
times in nanoseconds, callback deadline misses, and ring-buffer underruns and
overflows. `service.sh status` also lists the backend threads' Linux scheduler
class and real-time priority. `FF` or `RR` with a nonzero `RTPRIO` indicates
real-time scheduling; `TS` is ordinary time-sharing scheduling. Counters start
at backend launch, so compare snapshots before and after the workload under
test. Startup routing can produce transient ring-buffer counters; increases
during steady playback are the important signal.

On a Steam Deck, capture `bash service.sh status` after routing has settled, run
a CPU-heavy game scene, then capture status again. Callback counts should
increase without new deadline misses, underruns, or overflows. Compare
`maximum_nanoseconds` with the callback deadline (`frames / sample_rate`); for
example, 256 frames at 48 kHz is about 5.33 ms. The callback telemetry measures
Ear Tuner's ingress and output callbacks, not the rest of the PipeWire graph.

One Steam Deck game-session measurement on 2026-10-28 recorded 36,788 callbacks
per direction, no deadline misses, and maximum callback times of 639,080 ns
ingress and 88,797 ns output. Two ring-buffer underruns occurred during startup;
none increased during steady gameplay. This is one observed run, not a
performance guarantee across games, devices, or PipeWire quantum settings.

The control contract is versioned independently from the plugin release. New
callers use `/api/v1/control`; the original root endpoint remains as a temporary
compatibility alias. Unknown API versions are rejected.

The control server accepts fragmented headers within a 4 KiB request limit. Each
accepted connection has a 500 ms total read/write deadline so an idle client
cannot indefinitely block later controls. Shutdown interrupts listener and
client waits without closing their file descriptors from another thread.

Temporary profile overrides can be sent to the same endpoint. They remain in
memory until the native backend restarts:

- `tune_low_0` through `tune_low_3`: Low-mode base gain for each of the four
  bands
- `tune_voice_0` through `tune_voice_3`: Voices-mode base gain for each band
- `tune_high_0` through `tune_high_3`: High-pitch base gain for each band
- `tune_quiet_gain`: Quiet Sounds base gain applied to all four bands
- `tune_compression_threshold`: maximum WDRC knee as linear full scale
- `tune_quiet_threshold`: compatibility alias for the compression threshold

Base band gains are clamped to `-12` through `12` dB, Quiet Sounds base gain to
`0` through `12` dB per level, and the knee to `0.01` through `1.0`. The default
knee is `0.125`, approximately `-18 dBFS`. Each channel automatically lowers its
effective knee when needed for its selected gain and strength. The default
strength multipliers are `1`, `2.25`, and `3.5` for Weak, Medium, and Strong;
the effective knee depends on the largest gain in that channel's selected curve.

## Research behind the processing

Ear Tuner draws on hearing-aid research on multichannel wide dynamic range
compression (WDRC). Studies report improved audibility for some listeners, with
tradeoffs depending on settings and listening conditions. Ear Tuner does not use
an audiogram or calibrated sound levels; its profiles are user-controlled
starting points, not clinical fittings.

- Davies-Venn et al. (2009),
  [four-channel WDRC and speech audibility](https://pmc.ncbi.nlm.nih.gov/articles/PMC2981704/).
- Alexander and Masterson (2015),
  [channel count and release time](https://pmc.ncbi.nlm.nih.gov/articles/PMC4336616/).
- Keidser et al. (2011),
  [NAL-NL2 prescription procedure](https://pmc.ncbi.nlm.nih.gov/articles/PMC4627149/).
