# Developer guide

Want to help build Ear Tuner? You can work on the UI, settings, or audio
processing without setting up a Steam Deck first. Desktop development is useful,
but it doesn't replace testing audio on a supported device.

## Browser preview

The preview needs Node.js supported by `package.json`, pnpm 9.15.9 through
Corepack, and Python 3.10 or newer. Clone the repository and install
dependencies:

```bash
git clone https://github.com/collinschupman/decky-ear-tuner-plugin.git
cd decky-ear-tuner-plugin
corepack enable
pnpm install --frozen-lockfile
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
```

Start the UI and its local Python API:

```bash
source .venv/bin/activate
pnpm run dev
```

Open the address printed by Vite, usually <http://127.0.0.1:5173/>. Stop both
processes with Ctrl+C.

**The preview uses the real backend, not a mock.** On Linux, it restores saved
settings and may start or restart the audio service. Don't run it alongside the
installed plugin or another Ear Tuner instance. On macOS, you can inspect the
UI, but audio processing isn't available and settings changes may fail.

## Project map

| Area                              | Source                |
| --------------------------------- | --------------------- |
| Decky UI                          | `src/frontend/decky/` |
| Settings and application behavior | `src/ear_tuner/`      |
| Reusable DSP                      | `src/dsp/`            |
| Native core and platform adapters | `src/native/`         |

Decky loads root `main.py`, `plugin.json`, and `service.sh`. Keep Decky RPC
methods in `main.py` and application behavior in `src/ear_tuner/`. WirePlumber
owns audio routing; `service.sh` manages the native backend lifecycle. See the
[architecture guide](ARCHITECTURE.md) for runtime details and the
[native backend guide](../native/README.md) for DSP, diagnostics, and advanced
native testing.

## Tests and contributions

Run the checks relevant to your changes and add regression tests where useful.
The [contributing guide](../.github/CONTRIBUTING.md) has the setup and canonical
pre-review checks. Desktop tests don't replace Steam Deck audio testing.

## Test on Steam Deck

From the checkout in Desktop Mode, open native Konsole and run:

```bash
./install-decky.sh
```

The installer builds the frontend and native backend, runs native tests, and
links the checkout into Decky. It may install missing dependencies or Decky
Loader, ask for administrator access, and temporarily disable the SteamOS
read-only filesystem while installing packages. Return to Game Mode and reload
Ear Tuner through Decky Settings.

Use `./update-decky.sh` to update and rebuild a clean checkout, or
`./uninstall-decky.sh` to stop processing and remove the development link.
Uninstall preserves settings unless you explicitly pass `--purge-settings`.

For service issues, use native Konsole:

```bash
bash service.sh status
tail -f ~/homebrew/logs/ear-tuner/plugin.log
tail -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ear-tuner-backend.log"
```

Reload the plugin through Decky, not by restarting `plugin_loader.service`;
Decky's unload hook stops the backend and removes its audio nodes. For release
packaging and device validation, see the
[release checklist](RELEASE_CHECKLIST.md) and
[Decky submission guide](DECKY_SUBMISSION.md).
