# Third-party notices

The release bundle and OoT Native components are distributed under
GPL-3.0-or-later as an aggregate. Compatible file-level MIT, Apache-2.0,
Apache-2.0 WITH LLVM-exception, and zlib licenses remain applicable to their
components. Full license texts are in `LICENSES/`; source is in the included
corresponding-source archive.

- `nxnative` is MIT, pinned to seedds/nxnative commit
  `c1df4ddaaedc3c18228065ea5b909688b79884e2`. The shader translator is compiled
  from that source tree after the project Switch 2 patch is applied. Ryujinx
  shader components are MIT. Their source snapshot and notices are in the
  source archive and `LICENSES/`.
- Translator NuGet dependencies: Gommon 2.8.1.2 (MIT), Humanizer 3.0.10 (MIT),
  Microsoft.IO.RecyclableMemoryStream 3.0.1 (MIT), MsgPack.Cli 1.0.1
  (Apache-2.0), and System.CodeDom 4.4.0 (MIT). Their upstream license texts
  and package copyright lines are in `LICENSES/`; Humanizer's copyright is from
  its pinned NuGet package metadata.
- `ampere-lower` is derived from Eden-NS2 and is GPL-3.0-or-later. Its dated
  modification notice and credits to Lichii, Eglantine-fairtech, and Orchid
  are in `builder/ampere-lower/NOTICE.md`.
- `zbic-unpack` is an isolated GPL-2.0-only subprocess from kinnay/zbic. Its
  source and license are in the corresponding-source archive; its license text
  is `LICENSES/zbic-GPL-2.0-only.txt`.
- `nxanalyze`, `nxlift`, and `nxrt` are distributed under GPL-3.0-or-later for
  this release aggregate; their source and compatible original file notices
  are in the corresponding-source archive. None links the ZBIC implementation.
- `oot-native-launcher` is the PyInstaller one-file GUI. Its bundled Python
  libraries and Tcl/Tk notices are retained in `LICENSES/`.
- LLVM 23.1.4 is Apache-2.0 WITH LLVM-exception. `ld.lld` is included; the
  complete upstream license and exception are in
  `LICENSES/LLVM-Apache-2.0-with-LLVM-exception.txt`.
- SDL3 3.4.18 is zlib-licensed and included as `builder/lib/libSDL3.so.0`.
- FFmpeg 7.1.1 (`libavcodec`, `libavformat`, `libavutil`) is configured without
  GPL and nonfree components, with only AV1 decode and WebM demux support.
  It is LGPL-2.1; full text is in `LICENSES/FFmpeg-LGPL-2.1.txt`.
- dav1d 1.5.1 is BSD-2-Clause; full text is in
  `LICENSES/dav1d-BSD-2-Clause.txt`.
- Exact source tarballs for both codec libraries are provided in the adjacent
  `*-thirdparty-source.tar.xz` archive.
- SPIRV-Tools is Apache-2.0. `spirv-opt` and `spirv-val` are included.
- SPIRV-Tools package copyright and any installed NOTICE are included in
  `LICENSES/`. `Spv.Generator` from Ryujinx is MIT; its original text is also
  included there.
- astc-encoder is Apache-2.0; bc7enc and the NUP reader are MIT-licensed.
- The embedded CPython 3.12 runtime is PSF-2.0. The one-file launcher uses
  PyInstaller 6.12.0; its bootloader license and exception are included.
  Tcl/Tk 8.6 license notices are included for the launcher GUI.
- CPython's unmodified `Doc/license.rst` is included for its embedded third-party
  code; separate copies of the expat MIT and libmpdec BSD-2-Clause sections are
  provided for `pyexpat`, `_elementtree`, and `_decimal`. The package build
  audits Python ELF dependencies and appends complete Ubuntu copyright texts
  for every non-glibc system library it links.
- The embedded .NET 10 runtime is MIT-licensed and carries Microsoft's
  `ThirdPartyNotices.txt`.
- Runtime Python modules are lz4 4.3.3, zstandard 0.23.0, fontTools 4.56.0,
  and Pillow 11.1.0. Their license texts are included.
- The bundled DejaVu Sans font is covered by its included Bitstream Vera
  license. System glibc, Vulkan loader/driver, display server, and audio
  libraries are provided by the operating system. X11-related libraries
  embedded in the launcher have separate copyright texts listed above.
- AMD FidelityFX FSR1 headers `ffx_a.h` and `ffx_fsr1.h` (opt-in present upscaler)
  are MIT-licensed, copyright 2021 Advanced Micro Devices, Inc. The full notice is
  in `LICENSES/FidelityFX-FSR1-MIT.txt` and alongside the headers under
  `windows/src/nxrt/vk/shaders/ffx/`.
- The FSR shader wrappers are derived from sostenesfreitas/ocarina-recomp at
  `f328427034dd3e8b2b2cc5e15ed13da6304d392b`, GPL-3.0-or-later; see
  `windows/docs/PROVENANCE.md`. Release binaries are GPL-3.0-or-later.
