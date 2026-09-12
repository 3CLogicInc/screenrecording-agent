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
GPLv3. See `LICENSE`, `COPYING.note.md` and `NOTICES.md`.

## Building

Requirements:

- CMake 3.20 or newer
- C++17 toolchain: Apple Clang (Xcode command line tools) on macOS, MSVC x64 on
  Windows
- libobs 31.0.3, built from https://github.com/3CLogicInc/obs-studio branch
  `release/31.0.3` (upstream tag 31.0.3 plus two 3CLogic fixes), at
  the `31.0.3-3clogic.N` tag named in this release's COPYING.note.md. All other
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
that release, with no intermediate commits. Tags are the unit of comparison;
there is no ongoing branch history to follow.

## Source offer

- This repository: https://github.com/3CLogicInc/screenrecording-agent
- libobs fork used to build the shipped `obs.dll`:
  https://github.com/3CLogicInc/obs-studio (branch `release/31.0.3`,
  tag `31.0.3-3clogic.N` as named in COPYING.note.md)
- FFmpeg, x264 and the other prebuilt OBS dependencies: OBS Project's obs-deps
  release 2024-09-12, https://github.com/obsproject/obs-deps

See `COPYING.note.md` for the full written offer.
