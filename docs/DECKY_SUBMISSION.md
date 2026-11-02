# Maintainer guide: Decky submission

This is a maintainer-facing release procedure, not an installation guide for
users or contributors. Track candidate readiness in
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md). Requirements can change; confirm
the current Decky Plugin Database rules and pull request template before
submission.

Use the Decky Plugin Database's **Plugin addition** pull request template. Do
not mark test or authorship declarations complete until they have been
independently verified.

Ear Tuner is in beta testing. Repository visibility, public release, and Decky
Plugin Database submission are separate decisions. Confirm the intended
visibility with the maintainer before publishing a release or submitting the
plugin. This guide is not a guarantee of acceptance.

## Backend declarations

- Custom backend other than Python: **Yes**. The shipped
  `decky_ear_tuner_backend` executable is written in C++.
- Third-party FOSS software with non-statically-linked dependencies: **Yes**.
  The executable uses the system PipeWire libraries.
- Custom binary with all dependencies statically linked: **No**.

Because the plugin ships a custom binary with dynamically linked PipeWire
dependencies, request a third-party test report from a Steam Deck on the SteamOS
Preview update channel before opening the database pull request.

## Next steps

1. Verify CI passes for the candidate commit and build its production archive on
   Linux or SteamOS.
2. Install the generated archive on a test Steam Deck and complete a local Game
   Mode smoke test before requesting independent testing.
3. Ask someone other than the plugin author to complete
   [STEAMOS_PREVIEW_TEST_REPORT.md](STEAMOS_PREVIEW_TEST_REPORT.md) on the
   SteamOS Preview channel. Provide the exact candidate archive and checksum.
4. Resolve any issues found during testing and rebuild the production archive.
5. Confirm `assets/preview.png` is a current screenshot of the actual Decky UI
   on Steam Deck and check the `plugin.json` image URL.
6. With maintainer approval, make the repository public for release/submission
   and verify the source and store preview image are anonymously accessible.
7. Only after the report passes, add the repository to the Decky Plugin Database
   as a submodule and open the plugin-addition pull request.

Pushing this repository does not submit the plugin to Decky. Database submodule
and store actions are separate and must not be performed until explicitly
authorized.

## Pre-submission checks

```bash
corepack enable
pnpm install --frozen-lockfile
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
pnpm run format:check
.venv/bin/python -m unittest discover -s src/tests -t src -v
pnpm test
pnpm run build
pnpm run package:decky
```

Run `pnpm audit` for the candidate lockfile and review any findings. Record
whether affected dependencies ship in the archive or are build-only; do not
assume an earlier audit result or patch availability is still current.

Inspect the archive under `release/` before submitting. It must include the
Decky bundle, `package.json`, `main.py`, the `ear_tuner/` Python package,
`service.sh`, the BSD-3-Clause `LICENSE`, and the native backend executable.

Packaging copies the Python package from `src/ear_tuner/` to root `ear_tuner/`
inside the archive. It also produces `install-beta.sh` and a SHA-256 checksum
beside the archive. Test the archive installation separately from the linked
development checkout.

## Release archive installation

With Decky Loader already installed, run from native Konsole:

```bash
bash release/install-beta.sh \
  /path/to/ear-tuner-0.1.0-beta.1.tar.gz \
  EXPECTED_SHA256
```

Replace the example archive path and checksum with the candidate release's
values. The installer accepts a local archive or HTTPS URL and refuses to
replace a development symlink. Use a separate test installation rather than
deleting a checkout or persisted settings to bypass that protection.

## Development checkout installation

Run this from Desktop Mode in Konsole on the test Deck:

```bash
git clone https://github.com/collinschupman/decky-ear-tuner-plugin.git
cd decky-ear-tuner-plugin
./install-decky.sh
```

The installer sets up missing development dependencies, builds the Decky
frontend and native PipeWire backend, runs the native tests, and creates the
development plugin link. Return to Game Mode and reload Ear Tuner in Decky
Settings after the installer completes. After pulling later changes, rerun
`./install-decky.sh` to rebuild and relink the checkout.

## Steam Deck validation

Record the installation method and test the following on the release candidate:

- Ear Tuner appears in Decky and can be enabled and disabled.
- Each focus mode works with shared and independent left/right settings.
- Audio routes correctly through headphones while games or media are playing.
- Headphone disconnect and reconnect move audio to the expected output.
- Settings survive a Decky reload and a Steam Deck reboot.
- The bundled listening sample plays and stops through the current Ear Tuner
  settings.
- Repeated enable/disable cycles leave no stale sink, backend, route watcher, or
  PipeWire links.
- Uninstall removes the plugin and stops the native backend cleanly.

Include the Deck model, SteamOS version and channel, tested commit, test date,
and any logs or failures in the independent test report. Start from
[STEAMOS_PREVIEW_TEST_REPORT.md](STEAMOS_PREVIEW_TEST_REPORT.md) and link the
completed report in the database pull request.

## Submission materials

- Confirm the committed store preview image URL in `plugin.json` is publicly
  accessible at the submitted commit.
- Keep the required `LICENSE` in the submitted commit and production archive.
- Explain that DeckSP provides general-purpose JamesDSP effects and per-game
  profiles, while Ear Tuner is an accessibility-focused workflow with
  independent left/right hearing profiles, the option to leave either ear
  unprocessed, and presets that bring forward low frequencies, voices, high
  pitches, or quiet sounds. Volume Boost and Virtual Surround Sound address
  overall loudness and spatialization rather than per-ear hearing differences.
- Link the requested third-party Preview-channel testing report in the database
  pull request.
