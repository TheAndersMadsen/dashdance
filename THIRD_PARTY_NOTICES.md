# Licences and third-party notices

Dashdance is distributed under the GNU General Public License, version 2 or (at your option) any later version
(GPL-2.0-or-later). The full text is in [LICENSE](LICENSE). This page lists what the project is built from, the licence
each part carries, and the evidence for it, so the licensing can be checked rather than taken on trust. It was compiled
from the upstream repositories on 2026-09-14. It is a factual record, not legal advice.

## The project itself

| Source | What Dashdance uses | Licence | Evidence |
|---|---|---|---|
| [Hero88go/melee-unlocked](https://github.com/Hero88go/melee-unlocked) | The fork's base: the recompiler, runtime and HLE layers (pinned in `tools/port_source_pins.json`) | GPL-2.0-or-later | Its README, section "License": "GPL-2.0-or-later." |
| [Dolphin](https://github.com/dolphin-emu/dolphin) | Ports: GX register layouts and shader generation (`gx_regs.h`, `gx_shader.cpp`), AX audio (`ax_ucode.cpp`), memory-card layout | GPL-2.0-or-later | Ported files carry `SPDX-License-Identifier: GPL-2.0-or-later` (for example `VideoCommon/PixelShaderGen.cpp`, `VideoCommon/BPMemory.h`, `Core/HW/DSPHLE/UCodes/AX.cpp`). Its COPYING says the repository as a whole "is compatible with the GPLv3 license". |
| [Slippi Ishiiruka](https://github.com/project-slippi/Ishiiruka) | Ports: the Slippi EXI device, netplay, matchmaking, user record, savestates and playback (`exi_slippi.*`, `slippi_net.*`, `slippi_online.*`, `slippi_playback.*`); SlippiLib (`port/third_party/slippilib`, from `Externals/SlippiLib`); the Sys files in `port/slippi_sys` (from `Data/Sys`) | GPL-2.0-or-later | Its Readme: "licensed under the terms of the GNU General Public License, version 2 or later (GPLv2+)". Source headers such as `EXI_DeviceSlippi.cpp` and `SlippiNetplay.cpp`: "Licensed under GPLv2+". |
| [Slippi Dolphin](https://github.com/project-slippi/dolphin) | The playback Sys files in `port/slippi_sys_playback` | GPL-2.0-or-later, per file | Same COPYING and SPDX scheme as Dolphin. |
| [slippi-rust-extensions](https://github.com/project-slippi/slippi-rust-extensions) | A C++ port of the jukebox (`jukebox.cpp`); the Discord presence follows [pull request 36](https://github.com/project-slippi/slippi-rust-extensions/pull/36), which is not merged | GPL-2.0, no version stated | The repository ships the GPLv2 text as `LICENSE`; neither its README nor any crate's `Cargo.toml` states a version. Section 9 of GPLv2: "If the Program does not specify a version number of this License, you may choose any version ever published by the Free Software Foundation." |

## Components under other licences

| Component | Where | Licence | In the Apple apps? |
|---|---|---|---|
| [iPhone Duo Skills](https://github.com/mirzaaghazadeh/iphone-duo-skills/tree/b8b9d15b4dc987d0c35940265e07f4128ba79361) | `.agents/skills/iphone-duo-*` and `.agents/reference`; development guidance only | MIT; upstream `LICENSE` says "MIT License" and "Copyright (c) 2026 Navid Mirzaaghazadeh"; full notice below | No |
| [SDL 3](https://github.com/libsdl-org/SDL) | Built from Aurora's vendored copy | zlib | Yes |
| [Aurora](https://github.com/encounter/aurora) (commit 749d6ee) | Build tooling that provides SDL | MIT | Its SDL only |
| [ENet](https://github.com/lsalzman/enet) | `port/third_party/enet` | MIT, see its `LICENSE` | Yes |
| [nlohmann/json](https://github.com/nlohmann/json) 3.4.0 | `port/third_party/nlohmann/json.hpp` | MIT, notice in the header | Yes |
| [hps_decode](https://github.com/DarylPinto/hps_decode) 0.3.0 | The HPS/DSP-ADPCM decoding that `jukebox.cpp` ports, via slippi-rust-extensions | MIT | Yes (as a port) |
| [VirtualFriend](https://github.com/agg23/virtualfriend) | The on-screen controller layout and touch model in `window_sdl.cpp` are adapted from it | MIT | Yes (as an adaptation) |
| [ControllerOverlays](https://github.com/datkat21/ControllerOverlays) | The GameCube controller artwork in `port/app/art/controller` | GPL-3.0 | Yes |
| [Dear ImGui](https://github.com/ocornut/imgui) | `port/third_party/imgui` | MIT, see its `LICENSE.txt` | No, Windows build only |
| [NVIDIA Streamline](https://github.com/NVIDIAGameWorks/Streamline) | `port/third_party/streamline` | MIT, see its `license.txt` | No, Windows build only |

**ControllerOverlays** ships the GPLv3 text without a version statement; section 14 of GPLv3 then lets recipients choose
any published version. Builds that include the artwork are distributed under GPL-3.0, which the rest of the project's
GPL-2.0-or-later licence permits.

**NVIDIA Streamline** is MIT, except the NSight Perf SDK files its `license.txt` names (`sl_nvperf.h`, `sl_nvperf.dll`),
which fall under NVIDIA's separate NSight Perf SDK licence. Dashdance uses neither, so `sl_nvperf.h`, which came in with
the upstream fork, was removed.

## Not in this repository

- **doldecomp/melee.** The build reads a checkout of [doldecomp/melee](https://github.com/doldecomp/melee) for function names
  and generates an animation interpreter from its `fobj.c` and `spline.c` (`tools/generate_fobj_host.py`). That repository
  publishes no licence. The generated code is created in your build folder and compiled into your own build; it is not
  committed here.
- **Super Smash Bros. Melee.** Nintendo and HAL Laboratory own the game. This repository contains none of its code or data;
  every build is made from a disc image the player supplies. The files in `port/slippi_sys` are Slippi's own code list,
  code handler and patches, as distributed in Slippi Ishiiruka.
- **Trademarks.** Slippi is a trademark of its authors. The Dashdance name, icon and mark are original to this project.

## AI-assisted development

Most of this project was written with AI coding assistants, and most commits carry an AI co-author trailer. On
copyright, the U.S. Copyright Office's
[Part 2 report on copyrightability](https://www.copyright.gov/ai/Copyright-and-Artificial-Intelligence-Part-2-Copyrightability-Report.pdf)
(January 2025) concludes that "Copyright does not extend to purely AI-generated material, or material where there is
insufficient human control over the expressive elements", and that "Copyright protects the original expression in a work
created by a human author, even if the work also includes AI-generated material." Its
[registration guidance](https://www.copyright.gov/ai/ai_policy_guidance.pdf) (88 Fed. Reg. 16190, March 16, 2023) adds
that a human may select, arrange or modify AI-generated material in a way that is protected.

What that means here:

- The licence covers every part of Dashdance that is protected by copyright. That includes the upstream code above, which
  people wrote, and the human selection, arrangement and modification in this project.
- Parts that are purely AI-generated may not be protected at all. Nobody, this project included, can restrict their use
  with a licence.
- That does not make the project public domain. Code derived from the GPL sources above stays under the GPL, and anyone
  who distributes Dashdance or a modified version must follow the GPL for it.
- Anthropic's [commercial terms](https://www.anthropic.com/legal/commercial-terms) assign to the customer whatever rights
  Anthropic has in outputs; they do not create copyright where the law recognises none.

## Notice texts

The MIT and zlib licences require their notices to be kept. ENet, Dear ImGui, nlohmann/json and Streamline keep theirs in
`port/third_party`. The others, which are not otherwise present in this repository:

### VirtualFriend

```
The MIT License (MIT)

Copyright (c) 2024 Adam Gastineau

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

### hps_decode

```
MIT License

Copyright (c) 2023 Daryl Pinto

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

### Aurora

```
The MIT License

Copyright (c) 2022 Luke Street

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

### SDL

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:
  
1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## iPhone Duo Skills — MIT notice

```text
MIT License

Copyright (c) 2026 Navid Mirzaaghazadeh

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

---

This project is not affiliated with, endorsed by, or sponsored by Apple Inc.
"Apple", "iPhone", "iPhone Duo", "iOS", "Xcode", "SwiftUI" and "UIKit" are
trademarks of Apple Inc. All technical guidance here is original writing derived
from Apple's publicly published developer material, which is linked in
reference/sources.md.
```
