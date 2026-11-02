# Third-party notices

Ear Tuner's frontend bundle includes code from the following projects:

- `@decky/api` 1.1.3, Copyright SteamDeckHomebrew Team, licensed under the GNU
  Lesser General Public License version 2.1.
- `@decky/ui` 4.12.1, Copyright SteamDeckHomebrew Team, licensed under the GNU
  Lesser General Public License version 2.1.

The release archive includes the applicable license text at
`licenses/LGPL-2.1.txt`. Source for these libraries is available from the
[Decky Loader API](https://github.com/SteamDeckHomebrew/loader-api) and
[Decky Frontend Library](https://github.com/SteamDeckHomebrew/decky-frontend-lib)
repositories.

The bundled `deckySounds.wav` listening sample is an original recording created
by project author Collin Schupman. The author owns the recording and permits its
redistribution as part of Ear Tuner under the project's BSD-3-Clause license. It
is a 24-bit, 48 kHz stereo WAV file.

Ear Tuner's native backend dynamically links the system-provided PipeWire,
C/C++, math, and compiler runtime libraries. Those libraries are not copied into
the release archive.
