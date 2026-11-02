# Maintainer release checklist

Use this checklist for the commit submitted to the Decky Plugin Database. Decky
testing occurs during pull request review; it is not a prerequisite for opening
the pull request.

## Candidate validation

- [x] Version, metadata, user-facing text, license, notices, and preview image
      are ready.
- [x] Repository and `plugin.json` preview URL are publicly accessible.
- [x] Formatting, Python tests, frontend tests, type checking, build, native
      tests, packaging, and archive inspection pass.
- [x] CI and CodeQL pass for the published release tree.
- [x] Production dependency audit reports no known vulnerabilities; remaining
      audit findings are confined to build-only dependencies.
- [x] The packaged plugin loads in Game Mode, enables cleanly, plays the bundled
      sample, applies its focus modes, and leaves backend diagnostics healthy.

## Candidate artifact

- Version: `0.1.0-beta.1`
- Archive: `release/ear-tuner-0.1.0-beta.1.tar.gz`
- SHA-256: `205b5533296bbc5c623f0a583bb388c52b5dbf8a401b37d1d22ddb0e9fc2ed1d`
- Native dependencies: `libpipewire-0.3`, `libstdc++`, `libm`, `libgcc_s`,
  `libc`, and the system ELF loader.
- Known cosmetic limitation: an `ear-tuner` entry may appear in the SteamOS
  audio output selector while the plugin is running.

## Plugin Database submission

- [ ] Fork the Decky Plugin Database and add this repository under `plugins/` as
      a submodule pinned to the candidate commit.
- [ ] Open a **Plugin addition** pull request.
- [x] Declare custom backend other than Python: **Yes**.
- [x] Declare third-party FOSS with dynamically linked dependencies: **Yes**.
- [x] Declare custom binary with all dependencies statically linked: **No**.
- [ ] Leave third-party testing boxes unchecked until a tester comments on the
      pull request.
- [ ] Address review feedback and any functionality-breaking issues found by
      SteamOS Preview testing.

Do not move the submitted submodule to another commit without rebuilding and
retesting that commit.
