# Contributing

## Maintainer and access

Collin Schupman (@collinschupman) is the sole maintainer and decides which
changes are merged and released. Contributions are welcome through pull requests
from forks. The [CODEOWNERS](CODEOWNERS) file identifies the maintainer for all
files; GitHub repository permissions control who can push or merge changes.

## Branches

`main` contains the current beta development code. A passing CI run alone does
not establish Steam Deck release readiness. Use short-lived branches for changes
and merge them into `main` through a pull request:

- `feature/<short-name>` for new functionality
- `fix/<short-name>` for bug fixes

Delete short-lived branches after they are merged.

## Development flow

Fork the repository on GitHub and clone your fork. Configure the original
repository as the upstream remote:

```bash
git clone https://github.com/YOUR-USERNAME/decky-ear-tuner-plugin.git
cd decky-ear-tuner-plugin
git remote add upstream https://github.com/collinschupman/decky-ear-tuner-plugin.git
```

Start new work from the latest upstream `main` branch:

```bash
git fetch upstream
git switch main
git merge --ff-only upstream/main
git switch -c feature/descriptive-name
```

Keep commits focused, push your branch to your fork, and open a pull request
against this repository's `main` branch:

```bash
git push -u origin feature/descriptive-name
```

The maintainer reviews and merges accepted changes and creates Semantic
Versioning tags when preparing releases.

## Required checks

Install dependencies as described in the
[developer guide](../docs/DEVELOPERS.md), then run the checks used by CI before
requesting review:

```bash
pnpm install --frozen-lockfile
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
pnpm run format:check
.venv/bin/python -m unittest discover -s src/tests -t src -v
pnpm test
pnpm run package:decky
```

`pnpm run package:decky` builds and tests the native backend and requires Linux
or SteamOS; it cannot complete on macOS. If you cannot run it, say so in your
pull request. Packaging rebuilds the tracked release executable in place, so
review `git status` and don't include binary changes unless intentionally
updating the release artifact. CI also runs RealtimeSanitizer checks for native
audio callbacks. CodeQL scans C++, JavaScript/TypeScript, and Python on pull
requests and `main`, and weekly for newly detected issues. GitHub Actions must
be pinned to full commit SHAs; Dependabot maintains those pins. Routine minor
and patch dependency updates are grouped, while major updates remain separate
for review.

See the [developer guide](../docs/DEVELOPERS.md) for setup and source layout.
Release packaging and real-device validation are maintainer tasks; see the
[release checklist](../docs/RELEASE_CHECKLIST.md).

## Pull requests

Keep pull requests small enough to review, explain user-visible and DSP behavior
changes, and include focused tests. Avoid mixing unrelated refactors with a
behavioral change. Update documentation when commands, settings, APIs, or audio
behavior change.

Report security vulnerabilities privately using the
[security policy](../SECURITY.md), not public issues or discussions.
