# SteamOS Preview test report

Maintainer/tester template: complete this report on a Steam Deck running the
SteamOS Preview update channel. The tester must be someone other than the plugin
author. Replace every pending field before sharing the completed report. This
template is not evidence that testing has passed; link the completed report in
the Decky Plugin Database pull request.

## Environment

- Tester: Pending
- Test date: Pending
- Steam Deck model: Pending
- SteamOS version: Pending
- SteamOS update channel: Preview
- Decky Loader version: Pending
- Ear Tuner commit: Pending
- Archive filename and SHA-256 (if applicable): Pending
- Installation method (release archive or linked checkout): Pending
- Headphones tested: Pending

## Installation

- [ ] Installed the candidate archive with `install-beta.sh` or built a linked
      checkout with `./install-decky.sh` from native Konsole without errors.
- [ ] Recorded the tested commit, installation method, and archive checksum when
      applicable.
- [ ] Reloaded Decky in Game Mode and confirmed Ear Tuner appears.

## Functional results

- [ ] Ear Tuner enables and disables cleanly.
- [ ] Every focus mode works with shared left/right settings.
- [ ] Every focus mode works with independent left/right settings.
- [ ] Every strength level works and either ear can be left unprocessed.
- [ ] Bundled listening sample plays and stops correctly.
- [ ] The listening sample plays and stops through the current Ear Tuner
      settings.
- [ ] Audio routes through headphones during games or media playback.
- [ ] Disconnecting headphones restores the expected output.
- [ ] Reconnecting headphones allows Ear Tuner to be enabled again.
- [ ] Settings survive a Decky reload.
- [ ] Settings survive a Steam Deck reboot.
- [ ] Repeated enable/disable cycles leave no stale virtual sink, process, or
      PipeWire links.
- [ ] Tested available wired, USB, and Bluetooth headphone paths and noted any
      paths not tested.
- [ ] Uninstall stops the backend and removes the plugin cleanly.

## Result

- Overall result: Pending
- Functionality-breaking issues: Pending
- Cosmetic issues: Pending
- Logs or supporting links: Pending
- Additional notes: Pending

For each failure, record the device, action, expected result, observed result,
and relevant logs. See [DEVELOPERS.md](DEVELOPERS.md) for log locations and
[DECKY_SUBMISSION.md](DECKY_SUBMISSION.md) for installation and validation
steps.
