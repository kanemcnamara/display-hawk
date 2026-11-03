# Third-party notices

Display Hawk's own code is under the MIT licence (`LICENSE`). It is built on, and its installers ship,
the software listed here, each under its own licence. Full licence texts are in `LICENSES/`, and the
vendored source code is in `third_party/` (see `third_party/README.md`).

## Compiled into Display Hawk

### Spout2 (Windows only)

SpoutDX and its support files, release 2.007.018, from <https://github.com/leadedge/Spout2>.
Licence: BSD-2-Clause.

```
Copyright (c) 2020, Lynn Jarvis
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Syphon Framework (macOS only)

Core and Metal server/client from <https://github.com/Syphon/Syphon-Framework>, commit
f4761677a45b8034a3c2069ec0f3d2553da81fba, with two small local patches (listed in
`third_party/README.md`). Licence: BSD-3-Clause.

```
Copyright 2010 bangnoise (Tom Butterworth) & vade (Anton Marini).
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

* Neither the name of the Syphon Project nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Shipped with the installers (dynamically linked)

### Qt 6.8.3

The Qt Company Ltd and other contributors, <https://www.qt.io>. Display Hawk uses Qt under the
**GNU Lesser General Public License v3** (`LICENSES/LGPL-3.0.txt`, which builds on
`LICENSES/GPL-3.0.txt`). The modules shipped are Qt Core, Gui, Widgets, Network, Multimedia,
Shader Tools, OpenGL, OpenGL Widgets and Svg, plus their platform, style, image-format, icon-engine,
TLS, network-information and multimedia plugins.

Qt is linked dynamically and is not modified. Under the LGPL you may replace the Qt libraries
shipped with Display Hawk (the `Qt6*.dll` files and plugin folders on Windows, and the frameworks
and plugins inside `Display Hawk.app/Contents` on macOS) with your own build of the same or a
compatible Qt version. Display Hawk's complete source code is available, so you can also rebuild
it against a modified Qt. The Qt 6.8.3 source code is at
<https://download.qt.io/archive/qt/6.8/6.8.3/>.

Qt itself contains third-party code. The parts in the modules above are listed below with their
licences. Qt's own attribution list, with the full notices, is at
<https://doc.qt.io/qt-6.8/licenses-used-in-qt.html>.

| Component (inside Qt) | Licence |
| --- | --- |
| FreeType 2.13.3 | FreeType License (FTL) |
| HarfBuzz 10.4.0 | MIT |
| libjpeg-turbo 3.1.0 | IJG and BSD-3-Clause |
| libpng 1.6.47 | libpng License |
| zlib 1.3.1 | zlib License |
| PCRE2 10.45 (and sljit) | BSD-3-Clause with PCRE2 binary exception, BSD-2-Clause |
| double-conversion 3.3.0, easing equations, RFC 6234, libpsl, AGLFN | BSD-3-Clause |
| TinyCBOR 0.6.1, md4c 0.5.2, Vulkan / D3D12 memory allocators, OpenGL headers, WebGradients | MIT |
| Public Suffix List data | MPL-2.0 (`LICENSES/MPL-2.0.txt`) |
| Unicode Character Database 34, Unicode CLDR 46.1 | Unicode License v3 |
| tika-mimetypes | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |
| BLAKE2, SHA-1, SHA-3/Keccak, MD4, MD5, SipHash | CC0-1.0 / public domain |
| ICC sRGB colour profile | ICC License |
| Wintab headers (Windows) | LCS/Telegraphics License |
| X SVG (in Qt Svg) | HPND (sell variant) |
| glslang 14.2.0 (in Qt Shader Tools) | BSD-3-Clause, MIT (Khronos), Apache-2.0, GPL-3.0-or-later with the Bison exception (applies only to its generated parser, which may be used under any licence) |
| SPIRV-Cross (in Qt Shader Tools) | Apache-2.0, MIT (Khronos) |
| Resonance Audio, with Eigen 3.4.0 and PFFFT (in Qt Multimedia) | Apache-2.0; MPL-2.0 and BSD-3-Clause; BSD-3-Clause |

### FFmpeg

The FFmpeg project, <https://ffmpeg.org>. The libraries are libavcodec 61, libavformat 61,
libavutil 59, libswresample 5 and libswscale 8 (FFmpeg 7.x), in the LGPL build that The Qt Company
provides for the Qt Multimedia FFmpeg backend. Licence: **GNU Lesser General Public License v2.1
or later** (`LICENSES/LGPL-2.1.txt`). They are dynamically linked, unmodified and replaceable. The
FFmpeg source code is at <https://ffmpeg.org/download.html>, and Qt's build configuration is in the
Qt Multimedia source (see the Qt source link above).

### Mesa llvmpipe software OpenGL (Windows only, `opengl32sw.dll`)

A software OpenGL fallback that Qt ships for machines without a working GPU driver. It is used only
with the OpenGL backend. Licence: MIT. Mesa: <https://mesa3d.org>.

### Microsoft runtime components (Windows only)

`D3Dcompiler_47.dll` and the Visual C++ Redistributable (`vc_redist.x64.exe`) are redistributed
under Microsoft's licence terms for redistributable code. They are not covered by Display Hawk's
licence.

## Used at runtime but not shipped

### NDI®

Display Hawk loads the NDI runtime that is installed on the machine (NDI Tools or the NDI Runtime,
from <https://ndi.video>). No NDI code is shipped. `src/media/Ndi.cpp` declares the matching
structure layouts of the NDI SDK's public C API (`Processing.NDI.structs.h`, whose headers are under
the MIT licence) so that the runtime can be called. NDI® is a registered trademark of Vizrt NDI AB.

## Formats implemented from public specifications

Display Hawk's own code reads and writes these formats. No code from their reference
implementations is included.

- **HAP** video codec, from Vidvox's specification:
  <https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md>
- **Snappy** compression, the format used inside HAP frames: <https://github.com/google/snappy>
- **ISF** (Interactive Shader Format), <https://isf.video>. The bundled example shaders in
  `assets/isf/` were written for Display Hawk.
- **Art-Net™** (Art-Net™ Designed by and Copyright Artistic Licence Engineering Ltd), **sACN**
  (ANSI E1.31), **OSC**, **MIDI** and **SMPTE / MIDI timecode**.
