# Licensing and written offer for source

## The files in this repository

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

## The license of the resulting binary

`screen_recording_agent` links libobs, which is GPL-2.0-or-later, and libobs in
turn links FFmpeg configured with `--enable-gpl --enable-version3` and x264.
`--enable-version3` pulls in LGPL-3.0/GPL-3.0-only components, so the only
license that covers the combined work is **GPLv3**: the binaries 3CLogic
distributes are distributed under the GNU General Public License version 3 or
later, while the source in this repository remains GPL-2.0-or-later and can be
combined with a GPLv2-only libobs build if you prefer.

## How to obtain the corresponding source

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
