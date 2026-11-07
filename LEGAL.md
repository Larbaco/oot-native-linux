# What this repository contains, and what it never contains

This project lets you run a game you own natively on your own Linux PC. It ships **tools only**. It does not contain, and must never
be made to contain, any part of the game.

## Never in this repository, never in a release

- The game's executable (`main`, `subsdk0`, `sdk`, `rtld`, the other ExeFS modules), in original, rewritten or recompiled form (the
  recompiled `game.so`, its objects and its `program.db` included).
- The dump itself (the `.nup` file) and the game's data (`RomFS`, `RomFS.img`, any file extracted from it).
- The game's shaders in any form: the shader containers from the game files, run-time shader dumps, lowered (Maxwell) programs,
  Vulkan SPIR-V, pipeline caches, or shader packages built from them.
- Console firmware files such as the system fonts.
- Keys, tickets, certificates, saves, screenshots of other people's content, or logs that name your files.

Every one of those is produced **on your machine, from your own dump, at build time**, and stays in your build folder. The build takes
only a dump you provide yourself and checks it is the version the profile is for. Never share a build folder: it holds the game.
Documentation may carry metadata about the game (IDs, sizes, hashes, symbol names), never its content.

## What is in the repository

- The build scripts and the game profile (`games/0400486024DEC000/`): tables that describe the game's identity and memory layout and
  how to treat each shader (hashes, slot kinds, formats). They contain no game code.
- `windows/`: the shared source tree (the directory name is historical; the Linux build uses it): the recompiler (`nxanalyze`,
  `nxlift`) and runtime (`nxrt`). Release modules that reproduce GPL-3.0-or-later behavior are distributed under GPL-3.0-or-later;
  individual files retain their original MIT headers and third-party components retain their licenses (see `windows/docs/PROVENANCE.md`).
- `host/`: the pinned-source integration used by setup. The public corresponding-source archive includes
  `host/nxnative-switch2.patch`, the MIT-licensed integration patch needed to reproduce the bundled translator, along with
  the exact patched translator source subset and its MIT notices.
- `ampere-lower/`: a standalone build of the Switch 2 to Maxwell shader lowering from the Eden-NS2 emulator
  (GPL-3.0-or-later; its own `LICENSE` and `NOTICE.md` are in that folder).
- Documentation (`docs/`).

## What the tool does not do

It decrypts nothing and bypasses nothing. It takes a dump that is already plain files, made by you from your own console with your own
tools, and needs no keys, tickets or certificates. If your dump is encrypted, this tool cannot read it and will not try.

## Licences

- `ampere-lower/`: GPL-3.0-or-later (Eden-NS2 code).
- `windows/src/nxload/third_party/zbic/`: GPL-2.0-only code from kinnay/zbic (zstd with Nintendo's ZBIC entropy-table coding), compiled
  only into the separate `zbic-unpack` subprocess. It is not linked into `nxload`, `nxanalyze`, `nxlift`, or `nxrt`; see the `LICENSE`
  and `README` in that folder.
- `host/`: MIT, with the nxnative and Ryujinx notices in `host/NOTICE.md`.
- The release as a whole, including the builder and binaries, is distributed under GPL-3.0-or-later. The full license is in `LICENSE`.
  Some files retain independent MIT headers; the project MIT notice is preserved in `LICENSES/MIT-project.txt`.
- Third-party components keep their own terms: LLVM (Apache-2.0 with LLVM exception), SDL3 (zlib), astc-encoder and SPIRV-Tools
  (Apache-2.0), bc7enc_rdo (MIT), nxnative/Ryujinx (MIT), and the isolated `zbic-unpack` (GPL-2.0-only).
  The binary package includes full third-party license texts and notices in `LICENSES/` and `THIRD-PARTY-NOTICES.md`.

## Trademarks and affiliation

The Legend of Zelda and Nintendo Switch are trademarks of Nintendo. This project is an independent, non-commercial fan project. It is
not affiliated with, endorsed by or supported by Nintendo or any other rights holder, and the names are used only to say which game it
is for.

If you fork this project, keep these rules: they are what makes it publishable.
