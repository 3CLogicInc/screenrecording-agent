# screenrecording-agent

`screen_recording_agent` is a headless desktop recorder for Windows and macOS.
It captures one or more displays and the machine's audio through
[libobs](https://github.com/obsproject/obs-studio), muxes the result to an MP4
file on local disk, and takes its orders over a local IPC channel — a Unix
domain socket on macOS, a named pipe on Windows — speaking the small JSON
command protocol defined in `include/ipc/protocol.hpp` (start, stop, pause and
resume video/audio, configure, status, shutdown, plus unsolicited
recording-started/stopped/failed events). It runs in the user's desktop session
because screen capture requires one, holds no network connections of its own,
and writes its recordings and logs to fixed per-platform directories listed in
`include/constants.hpp`.

## License

GPL-2.0-or-later for the sources here; the linked binary is distributed under
GPLv3. See `LICENSE`, the "License and source offer" section below, and `NOTICES.md`.

## Building

Requirements:

- CMake 3.20 or newer
- C++17 toolchain: Apple Clang (Xcode command line tools) on macOS, MSVC x64 on
  Windows
- libobs 31.0.3, built from https://github.com/3CLogicInc/obs-studio branch
  `release/31.0.3` (upstream tag 31.0.3 plus two 3CLogic fixes), at
  the `31.0.3-3clogic.N` tag named under "License and source offer" below. All other
  dependencies (libobs headers, nlohmann/json, spdlog, Boost.SML, doctest) are
  bundled under `third_party/`.

`LIBOBS_ROOT` is required on both platforms and points at the directory holding
the built libobs; configure fails with a message naming the expected layout if
it is missing or empty.

### macOS (Apple Silicon, macOS 13 or newer)

`LIBOBS_ROOT` must contain `Frameworks/libobs.framework`.

```
cmake -S . -B build \
  -DLIBOBS_ROOT=/path/to/obs-runtime \
  -DSCREENREC_VERSION=1.0.0
cmake --build build -j
```

The binary is ad-hoc signed after linking so that the macOS Screen Recording
permission grant survives rebuilds. At runtime the OBS framework and plugin
tree must be reachable: either copy `Frameworks/`, `PlugIns/` and
`obs-ffmpeg-mux` next to the binary, or keep `LIBOBS_ROOT` in place (it is
baked into the build rpath).

### Windows (x64)

`LIBOBS_ROOT` must contain `obs.lib` and the OBS runtime DLLs.

```
cmake -S . -B build -A x64 ^
  -DLIBOBS_ROOT=C:\path\to\obs-runtime ^
  -DSCREENREC_VERSION=1.0.0
cmake --build build --config Release
```

Copy the OBS DLLs, the `data` directory and the `obs-plugins` directory next to
`screen_recording_agent.exe` before running it. To rebuild the patched Quick
Sync encoder plugin, follow `plugins/obs-qsv11/README-3clogic.md` on the
`release/31.0.3` branch of https://github.com/3CLogicInc/obs-studio.

## Tests

Tests are built by default (`-DBUILD_TESTS=OFF` to skip). They are pure logic —
no libobs, no display, no privileges — so they run anywhere:

```
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Nine suites cover the IPC framing and protocol contract, the recording state
machine, encoder selection, monitor layout, output dimensions, pause/resume
transitions, stop-reason classification, and the configuration defaults
(including that `config/config.example.json` agrees with the compiled
defaults).

## Configuration

`config/config.example.json` shows the settings the agent reads: the recording
profile (video and audio encoding parameters, layout, maximum duration, output
format), storage limits, and file-logging levels and rotation. Per-recording
parameters are normally delivered over IPC at start time and override the file.

## Release policy

This repository is published as a snapshot, not as a development history. Each
tag mirrors one 3CLogic release and contains the exact agent sources shipped in
that release, with no intermediate commits. Tags are the unit of comparison and
are what the THIRD-PARTY-NOTICES installed with the product point at.

Each tag is published together with a `release/vX.Y.Z` branch holding the same
commit, based on `main` as it stood at publish time, so that `main` can be
brought forward by a pull request. `main` may therefore lag the newest tag;
when it does, the tag is authoritative.

## License and source offer

### The files in this repository

Every file in this repository authored by 3CLogic Inc. is licensed under the
GNU General Public License, version 2 or (at your option) any later version
(GPL-2.0-or-later). The full GPL-2.0 text is in `LICENSE`.

```
Copyright (C) 2026 3CLogic Inc.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
details.

You should have received a copy of the GNU General Public License along with
this program; if not, see <https://www.gnu.org/licenses/>.
```

Third-party components bundled here keep their own licenses; see `NOTICES.md`.

### The license of the resulting binary

`screen_recording_agent` links libobs, which is GPL-2.0-or-later, and libobs in
turn links FFmpeg configured with `--enable-gpl --enable-version3` and x264.
`--enable-version3` pulls in LGPL-3.0/GPL-3.0-only components, so the only
license that covers the combined work is **GPLv3**: the binaries 3CLogic
distributes are distributed under the GNU General Public License version 3 or
later, while the source in this repository remains GPL-2.0-or-later and can be
combined with a GPLv2-only libobs build if you prefer.

### How to obtain the corresponding source

### libobs and obs-qsv11

The Windows `obs.dll` and `obs-qsv11.dll` shipped with the agent are built from
OBS Studio 31.0.3 plus two 3CLogic fixes: `libobs/obs.c` (mixes_mutex lifetime;
upstream issue obsproject/obs-studio#11693) and `plugins/obs-qsv11/` (close the
Intel VPL session on encoder teardown). Their complete corresponding source is:

- Repository: https://github.com/3CLogicInc/obs-studio
- Branch: `release/31.0.3` (upstream tag 31.0.3 plus those two commits)
- Tag: `31.0.3-3clogic.1`

The patch notes and the standalone build recipe for obs-qsv11 are in
`plugins/obs-qsv11/README-3clogic.md` on that branch. obs-qsv11 is available
under either the BSD license or GPL-2.0, at your option.

The macOS `libobs.framework` is unmodified upstream libobs 31.0.3
(https://github.com/obsproject/obs-studio, tag `31.0.3`).

### FFmpeg, x264 and the other prebuilt dependencies

The FFmpeg (7.1, built with `--enable-gpl --enable-version3 --enable-libx264`),
x264 (core 164), and remaining runtime libraries shipped alongside libobs are the
unmodified prebuilt dependencies published by the OBS Project as obs-deps
release **2024-09-12**: https://github.com/obsproject/obs-deps/releases/tag/2024-09-12.
That release carries the build scripts and pins the exact upstream source
revisions. 3CLogic will also supply those source archives directly on request
to any recipient of the binaries, for at least three years from the release
date, at no charge beyond the cost of the medium.

### This repository

https://github.com/3CLogicInc/screenrecording-agent

Each release tag here is a source snapshot of the agent as shipped in the
corresponding 3CLogic release.

