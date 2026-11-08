# OoT Native provenance and licensing

The release combines files that retain their original licenses. The package-level
license is GPL-3.0-or-later; individual MIT, Apache-2.0 WITH LLVM-exception, zlib,
BSD, and other compatible notices remain attached to their files. See the
release's `THIRD-PARTY-NOTICES.md` for redistributed components.

## Runtime and builder

- `nxnative` is MIT-licensed and supplies the native host/runtime foundation.
  The release source archive includes the ported files and the exact patched
  translator/Ryujinx source snapshot used for the shipped build.
- LLVM is Apache-2.0 WITH LLVM-exception and supplies the static recompiler's
  backend and related libraries.
- `ampere-lower` derives from Eden-NS2, GPL-3.0-or-later. Its lowering behavior
  also reflects study of Lichii (zortos293), Eglantine-fairtech, and Orchid.
  The project is distributed under GPL-3.0-or-later and retains upstream notices.
- The code that translates guest shader formats is built from permissively
  licensed Ryujinx components; corresponding source and notices accompany the
  distributed translator build.
- The NUP reader is MIT-licensed. ZBIC decompression is GPL-2.0 and remains
  isolated in the separate `zbic-unpack` process (including its stream mode for
  shader containers); it is not linked into `nxload`, `nxanalyze`, `nxlift`, or
  `nxrt`. The package is an aggregate under GPL-3.0-or-later; the helper remains
  separately licensed under GPL-2.0-only with its source and license included.

## Clean-room and content boundary

The project reads a user-provided dump locally. It does not distribute the game,
game executable code, RomFS assets, firmware, keys, saves, or game-derived shader
data. Release tests use synthetic instruction encodings and synthetic fixtures.
Internal research notes and other-game profiles are excluded from release source
and packages by `release/allowlist.txt`.

## Present FSR1

- AMD FidelityFX FSR1 headers `ffx_a.h` and `ffx_fsr1.h`: MIT, copyright
  2021 Advanced Micro Devices, Inc.; copied from sostenesfreitas/ocarina-recomp
  commit `f328427034dd3e8b2b2cc5e15ed13da6304d392b`, destination
  `src/nxrt/vk/shaders/ffx/`. The MIT notice is retained there and in
  `LICENSES/FidelityFX-FSR1-MIT.txt`.
- The EASU and RCAS shader wrappers derive from sostenesfreitas/ocarina-recomp
  commit `f328427034dd3e8b2b2cc5e15ed13da6304d392b`, GPL-3.0-or-later;
  destination `src/nxrt/vk/shaders/fsr_easu.comp` and `fsr_rcas.comp`.
  The release binaries are GPL-3.0-or-later.

## Ampere shader lowering

- The SUATOM lowering for Ampere opcode `0x593` adapts the field validation,
  operand mapping, and Maxwell SUATOM encoding from zortos293/Lichii commit
  [`d5b258210436254fc734c8a646bcd60eee96ba1e`](https://github.com/zortos293/Lichii/commit/d5b258210436254fc734c8a646bcd60eee96ba1e),
  GPL-3.0 (compatible with this project's GPL-3.0-or-later license). Destination:
  `ampere-lower/shader_recompiler/frontend/ampere/lower_to_maxwell.h`.
- The ISBEWR register-shadow rule follows zortos293/Lichii commit
  [`b3284778`](https://github.com/zortos293/Lichii/commit/b3284778) and
  Djihads80/Eglantine commit
  [`582ecc45`](https://github.com/Djihads80/Eglantine/commit/582ecc45),
  GPL-3.0-or-later: ISBEWR's data register is an operand at bits 32-39 and must
  be included in high-register shadowing. This rule was already present in the
  base branch; this change makes it directly testable and adds a synthetic
  regression. Destination: `ampere-lower/shader_recompiler/frontend/ampere/lower_to_maxwell.h`.

## Ports from the upstream project (MIT)
## Ports from the upstream project (MIT)

Small, attributed ports of MIT-licensed work from the upstream project credited in the top-level `README.md`, under the licence in `LICENSES/MIT-project.txt`, re-applied by hand to this tree (upstream commit hashes and issues below):

- `56be9d26`: the Linux dynamic-export list (`windows/src/nxrt/plat/linux/nxrt.dynlist`) and its test (`tests/check_exports.cmake`), for issue #30 (found by apocal26).
- `1762d91` (linux-beta 3): the PipeWire audio reserve in `windows/src/nxrt/front/audio_sdl.c` and the `hle/` audio callbacks (diagnosed by Quantumenis).
- `77d1060`: the `NXRT_VK_ROBUST` robust-access switch in `windows/src/nxrt/vk/vk_dev.c` (only that hunk; the default is unchanged).
