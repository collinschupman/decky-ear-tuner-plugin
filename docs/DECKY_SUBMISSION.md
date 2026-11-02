# Maintainer guide: Decky submission

This is a maintainer-facing release procedure, not an installation guide for
users or contributors. Track candidate readiness in
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md). Requirements can change; confirm
the current Decky Plugin Database rules and pull request template before
submission.

Use the Decky Plugin Database's **Plugin addition** pull request template. This
guide is not a guarantee of acceptance.

## Backend declarations

- Custom backend other than Python: **Yes**. The shipped
  `decky_ear_tuner_backend` executable is written in C++.
- Third-party FOSS software with non-statically-linked dependencies: **Yes**.
  The executable uses the system PipeWire libraries.
- Custom binary with all dependencies statically linked: **No**.

Because the plugin ships a custom binary with dynamically linked PipeWire
dependencies, Decky's review process requires third-party testing on the SteamOS
Preview channel before acceptance.

## Next steps

1. Verify CI passes for the candidate commit and perform a local Game Mode smoke
   test.
2. Confirm the repository and the preview URL in `plugin.json` are public.
3. Fork the Decky Plugin Database, add this repository under `plugins/` as a
   submodule pinned to the candidate commit, and open a plugin-addition pull
   request.
4. Complete the backend declarations above. Leave Decky's third-party testing
   checkbox unchecked until a tester comments on the pull request.
5. During review, address reviewer feedback and any functionality-breaking
   issues reported by a SteamOS Preview tester.

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
- Do not check Decky's third-party testing box yourself. A tester or reviewer
  completes that part of the pull request review.
