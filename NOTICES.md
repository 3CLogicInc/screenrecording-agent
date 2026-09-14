# Third-party notices

This snapshot bundles the third-party components listed below. Each is used
under its own license; the 3CLogic-authored files in this repository are
licensed GPL-2.0-or-later (see `LICENSE` and the "License and source offer" section of `README.md`).

---

## libobs (OBS Studio)

- Location in this repository: `third_party/headers/obs/`
- Version: 31.0.3
- License: GNU General Public License, version 2 or (at your option) any later version
- Upstream: https://github.com/obsproject/obs-studio

```
Copyright (C) 2013-2024 by Lain Bailey <lain@obsproject.com>
Copyright (C) OBS Project contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
```

The full GPL-2.0 text is in `LICENSE`. The binary the agent links against is
built from the fork described in `README.md` under "License and source offer", which also states where to
obtain its corresponding source.

---

## obs-qsv11 (Intel Quick Sync encoder plugin, patched)

- Shipped binary: `obs-qsv11.dll` on Windows, built from
  https://github.com/3CLogicInc/obs-studio branch `release/31.0.3`
  (`plugins/obs-qsv11/`, tag `31.0.3-3clogic.1`); not part of this repository
- Origin: `plugins/obs-qsv11/` of OBS Studio 31.0.3, with a session-leak fix
  described in `plugins/obs-qsv11/README-3clogic.md` on that branch
- License: dual BSD / GPL-2.0, at the recipient's option (per the header of
  every source file)

```
Copyright(c) Oct. 2015 Intel Corporation.

This file is provided under a dual BSD/GPLv2 license.  When using or
redistributing this file, you may do so under either license.
```

The complete GPL and BSD grants, including the Intel contact address and the
BSD disclaimer, are reproduced verbatim at the top of each source file in that
directory. The GPL-2.0 text is in `LICENSE`.

---

## nlohmann/json

- Location in this repository: `third_party/headers/json.hpp`
- Version: 3.12.0
- License: MIT
- Upstream: https://github.com/nlohmann/json

```
SPDX-FileCopyrightText: 2013-2025 Niels Lohmann <https://nlohmann.me>
SPDX-License-Identifier: MIT
```

---

## spdlog

- Location in this repository: `third_party/headers/spdlog/`
- License: MIT
- Upstream: https://github.com/gabime/spdlog

```
Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
Distributed under the MIT License (http://opensource.org/licenses/MIT)
```

### fmt (vendored inside spdlog)

- Location in this repository: `third_party/headers/spdlog/fmt/bundled/`
- License: MIT, with the formatting library's optional object-form exception
  (see `third_party/headers/spdlog/fmt/bundled/fmt.license.rst`)
- Upstream: https://github.com/fmtlib/fmt

```
Copyright (c) 2012 - present, Victor Zverovich
```

---

## Boost.SML ([Boost].SML state machine library)

- Location in this repository: `third_party/headers/boost/sml.hpp`
- License: Boost Software License, Version 1.0
- Upstream: https://github.com/boost-ext/sml

```
Copyright (c) 2016-2024 Kris Jusiak (kris at jusiak dot net)

Distributed under the Boost Software License, Version 1.0.
```

---

## doctest (tests only)

- Location in this repository: `third_party/headers/doctest.h`
- License: MIT
- Upstream: https://github.com/doctest/doctest

```
Copyright (c) 2016-2023 Viktor Kirilov
```

doctest is used only by the test executables; it is not linked into
`screen_recording_agent`.

---

## MIT License (full text)

Applies to nlohmann/json, spdlog, fmt and doctest, each with its own copyright
line as quoted above.

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Boost Software License 1.0 (full text)

Applies to Boost.SML.

```
Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```
