# Maintainer release checklist

This document is for maintainers preparing a release, not a statement of the
current public release status. The completed checks and measurements below
describe the specific historical candidate identified in the validation record;
they do not certify the current `main` branch. Re-run checks for every
candidate.

## 1. Release blockers

- [x] Replace `assets/preview.png` with a current Game Mode screenshot of Ear
      Tuner.
- [x] Confirm the preview URL in `plugin.json` works without authentication
      (verified 2026-11-01 after publication).
- [x] Document the origin and redistribution rights for the bundled listening
      sample.
- [x] Accept and document that `ear-tuner` appears in the audio output selector
      for this release.
- [x] Select the `ear-tuner` output manually and verify it does not strand or
      silence audio.
- [x] Review the version, author, description, tags, repository URLs, and all
      user-facing text.
- [x] Confirm intended repository visibility with the maintainer before
      submission; publication was approved on 2026-11-01.
- [x] Verify Ear Tuner is unique or document how it differs from existing store
      plugins.

## 2. Automated validation

Run from a clean checkout at the candidate commit:

```bash
corepack enable
pnpm install --frozen-lockfile

python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt

pnpm run format:check
.venv/bin/python -m unittest discover -s src/tests -t src -v
pnpm test
pnpm run typecheck
pnpm run build
pnpm audit
pnpm run package:decky
```

- [x] Every command passes at the candidate commit.
- [x] Review every `pnpm audit` finding and record whether it affects shipped or
      build-only code.
- [x] CI and CodeQL pass for the published release tree.
- [x] `git status` is clean after the build and candidate status commit.

Audit review on 2026-10-25:

- `shell-quote` (`GHSA-pqg4-j6r4-53mv`) was present through the development-only
  `concurrently` package. Updating `concurrently` from 10.0.5 to 10.0.6 moved
  `shell-quote` from 1.9.0 to the patched 1.12.0 release.
- Updated the development-only Vite and Vitest toolchain to patched releases
  (Vite 8.3.5 and Vitest/coverage 4.1.12), and updated transitive Undici within
  the existing dependency ranges. The refreshed full audit reports zero
  advisories. Development tooling is excluded from the release archive; rerun
  both the full audit and `pnpm audit --prod` for the final candidate lockfile.

### Beta candidate validation (2026-11-01)

- Snapshot: superseded pre-publication beta candidate
- Version: `0.1.0-beta.1`
- Local validation passed: 106 Python tests, 50 frontend tests, 72 native tests,
  formatting, type checking, frontend build, packaging, and archive inspection.
- `pnpm audit --prod` reports no known vulnerabilities.
- The full `pnpm audit` reports four build-only findings inherited through
  `@decky/rollup`: three moderate findings in `glob`/`minimatch` paths and one
  low finding in `deepmerge`. Development tooling is excluded from the release
  archive.
- GitHub CI status for this commit still needs to be confirmed.
- The original sample licensing blocker was resolved by replacing the sample
  with an original recording owned by the project author. See
  `THIRD_PARTY_NOTICES.md`.

### Release-preparation validation (2026-11-01)

- Snapshot: release tree immediately before the public history reset. The file
  tree was unchanged by the reset.
- Local validation passed: 106 Python tests, 50 frontend tests, 72 native tests,
  formatting, type checking, frontend build, packaging, and archive inspection.
- `pnpm audit --prod` reports no known vulnerabilities. The full audit reports
  the same four build-only findings through `@decky/rollup` documented above.
- Archive: `release/ear-tuner-0.1.0-beta.1.tar.gz`
- SHA-256: `205b5533296bbc5c623f0a583bb388c52b5dbf8a401b37d1d22ddb0e9fc2ed1d`
- Anonymous access to the repository and preview image returns HTTP 200. GitHub
  CI and CodeQL pass for the published release tree.

## 3. Release archive inspection

Prepare the release as a draft and attach the archive, installer, checksum,
installation instructions, and known limitations before publishing. With release
immutability enabled, published assets and the release tag cannot be replaced;
publish a new version if a correction is needed.

- [x] `release/ear-tuner-0.1.0-beta.1.tar.gz` exists.
- [x] The generated SHA-256 file matches the archive.
- [x] Inspect the archive with `tar -tzf release/ear-tuner-0.1.0-beta.1.tar.gz`.
- [x] The archive contains `dist/index.js`, frontend assets, `main.py`, the
      `ear_tuner/` package, `service.sh`, `LICENSE`, `plugin.json`, and
      `native/build/decky_ear_tuner_backend`.
- [x] The archive contains no checkout metadata, caches, logs, test files,
      development symlinks, or obsolete policies.
- [x] Run `ldd` on the packaged backend and record its dynamic dependencies.
- [x] The BSD-3-Clause license is present in the repository and archive.

The packaged backend dynamically links `libpipewire-0.3`, `libstdc++`, `libm`,
`libgcc_s`, `libc`, and the system ELF loader.

## 4. Release archive installation

Install the archive on a Steam Deck with Decky Loader already installed. Do not
use the development symlink for release validation.

```bash
bash release/install-beta.sh \
      /absolute/path/to/release/ear-tuner-0.1.0-beta.1.tar.gz \
  EXPECTED_SHA256
```

- [x] Installation completes without errors.
- [ ] Decky loads the packaged plugin in Game Mode.
- [ ] Confirm the running frontend and backend came from the archive, not the
      development checkout.
- [x] Record the commit, archive filename, SHA-256, Steam Deck model, SteamOS
      version and channel, and Decky Loader version.

### Previously validated candidate (2026-10-25)

- Snapshot: superseded pre-publication candidate
- Archive: `ear-tuner-0.1.0.tar.gz`
- SHA-256: `66477d4fce402c57127b445cf6b0f213ea6a9328f8e1ed481b940c8fbaf19f09`
- Automated validation passed on 2026-10-25, including 105 Python tests, 50
  frontend tests, and 70 native tests.
- Hardware: Steam Deck OLED (`Galileo`)
- SteamOS: `3.9.5` Stable, build `20261023.100`
- Decky Loader: `v3.2.10`
- Archive checksum and contents were verified on 2026-10-25. Every installed
  file matches the archive SHA-256 manifest exactly. The running backend comes
  from the packaged plugin directory, exposes both PipeWire nodes, and returns
  `OK` from its control API.

Historical provisional archive installation on 2026-10-11:

- Archive: `ear-tuner-0.1.0.tar.gz`
- SHA-256: `6b17fba17450c13b9de63d6ac8d5eec86bd350cbad6266566006e2de42cc02aa`
- Source: provisional pre-publication tree with uncommitted release changes;
  this is not the final candidate.
- Hardware: Steam Deck OLED (`Galileo`)
- SteamOS: `3.9.2` Preview, build `20260925.101`
- Decky Loader: `v3.2.10`
- Installed frontend, sample, metadata, and native backend hashes match the
  archive. The running backend executable is under the packaged plugin directory
  and its control API is healthy.

The candidate snapshots above predate later changes to `main`. Treat them as
historical installation evidence only; do not use their checksums or test
results as release approval for a later commit.

## 5. Game Mode functional tests

- [ ] Enable and disable Ear Tuner.
- [ ] Test every focus mode and strength level.
- [ ] Test shared left/right settings.
- [ ] Test independent left/right settings.
- [ ] Test either ear left unprocessed.
- [ ] Switch an active focus mode to None for both ears and verify dry audio.
- [ ] Verify a None ear remains unchanged during loud peaks in the other ear.
- [x] Play and stop the bundled sample.
- [ ] Compare processed and unprocessed sample playback.
- [ ] Test real game and media audio for at least 15 to 30 minutes.
- [ ] Test volume buttons, rapid volume changes, mute, and unmute.
- [ ] Suspend and resume while audio is active.
- [ ] Reload Decky and verify settings persist.
- [ ] Reboot the Deck and verify settings and audio recover.
- [ ] Repeat enable and disable cycles and inspect for stale processes, sinks,
      or PipeWire links.
- [x] Record the visible `ear-tuner` output entry as a known cosmetic
      limitation.

## 6. Output and hotplug matrix

- [ ] Built-in speakers.
- [ ] 3.5 mm wired headphones.
- [ ] Bluetooth headphones.
- [ ] USB audio, if available.
- [ ] Dock or HDMI audio, if available.
- [ ] Disconnect and reconnect every tested output during playback.
- [ ] Switch outputs while Ear Tuner is enabled.
- [ ] Disable or change settings during an output switch and verify the latest
      acknowledged settings win after recovery.
- [ ] Audio follows the selected physical output without silence or duplicate
      playback.
- [ ] Record unavailable paths as not tested rather than passed.

## 7. Failure and cleanup tests

- [ ] Kill the native backend and confirm recovery or a clear disabled state.
- [ ] Reject a settings update and verify the UI shows the last acknowledged
      settings and a visible error, rather than reporting an unconfirmed
      Disable.
- [ ] Reload or restart Decky while processing is active.
- [ ] Restart PipeWire and WirePlumber and verify recovery.
- [ ] Uninstall while Ear Tuner is enabled.
- [ ] Uninstall removes the plugin, backend process, PID file, nodes, and links.
- [ ] Ordinary audio works immediately after uninstall.
- [ ] Preserve backend, Decky, and WirePlumber logs for every failure.

## 8. Independent Preview testing

Ear Tuner ships a dynamically linked custom C++ backend, so the release needs
independent testing on the SteamOS Preview channel.

- [ ] Find a tester other than the plugin author.
- [ ] Tester uses the SteamOS Preview update channel.
- [ ] Provide the exact candidate archive and SHA-256.
- [ ] Tester completes `docs/STEAMOS_PREVIEW_TEST_REPORT.md`.
- [ ] Resolve every functionality-breaking issue.
- [ ] Rebuild and repeat testing after any release-candidate change.
- [ ] Publish or link the completed report in the submission pull request.

## 9. Decky Plugin Database submission

- [x] Recheck the current Plugin Database plugin-addition template and rules.
- [ ] Verify the plugin works on the SteamOS Stable update channel.
- [ ] Verify the plugin works on the SteamOS Beta update channel.
- [x] Declare custom non-Python backend: **Yes**.
- [x] Declare third-party dynamically linked FOSS dependencies: **Yes**.
- [x] Declare fully static custom binary: **No**.
- [x] Explain that the backend dynamically links SteamOS's system PipeWire
      libraries.
- [x] Confirm all library licenses are satisfied and required notices are
      attached.
- [ ] Optionally test two other ready Plugin Database pull requests and retain
      links to the feedback comments.
- [x] Make the source repository and preview image publicly accessible.
- [ ] Add the repository as the required Plugin Database submodule.
- [ ] Open the plugin-addition pull request at the independently tested commit.
- [ ] Include the Preview test report, preview image, license, concise plugin
      differentiator, archive checksum, and known output-selector limitation.
- [ ] Do not change the candidate commit without rebuilding and retesting.
