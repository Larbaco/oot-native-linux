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
