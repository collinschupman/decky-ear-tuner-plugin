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

Ear Tuner's native backend dynamically links system-provided PipeWire, C, and
math libraries. Linux CMake builds normally also link the system C++ and
compiler runtimes. Those shared libraries are not copied into the archive.

The tracked submission-feedback executable was cross-compiled using Zig 0.17.1
and includes compiled LLVM libc++, libc++abi, and unwind support under the
[Apache License 2.0 with LLVM Exceptions](https://llvm.org/LICENSE.txt). The
LLVM object-code exception applies to these embedded compiled portions. Sources
and license details are available in the
[LLVM project](https://github.com/llvm/llvm-project). Rebuilding with the Linux
CMake toolchain may change which runtime libraries are linked.

Zig runtime support is available from the
[Zig project](https://github.com/ziglang/zig) under the following license:

```text
The MIT License (Expat)

Copyright (c) Zig contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
