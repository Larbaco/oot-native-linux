# The host: nxnative with the Switch 2 changes

`nxs2.py setup` fetches [nxnative](https://github.com/seedds/nxnative) (MIT) at the commit this patch set is made
against and applies what is here. Nothing of nxnative is copied into this repository.

| File | What it is |
|---|---|
| `nxnative-switch2.patch` | `git diff` of the Switch 2 port against upstream commit **c1df4dd** ("Mac: game input hides the pointer over the picture until the mouse moves"). 34 files: the Horizon host (`dm/host`: SVC, OS, offline and "oe" services the Switch 2 SDK 20.5 calls), the NVN layer (`dm/gfx/nvn`: NVN2 entry points, texture layouts, the 0xe0 Switch 2 shader control block, the shader-map key rule), the texture deswizzle paths (the Switch 2 in-GOB order through `s2tex`, this project's MIT module: new files `dm/gfx/tex/s2tex.c`, `dm/gfx/tex/s2tex.h`, `dm/gfx/msl/s2tex.metal`, which the two test Makefiles also build), the shader chain (`dm/gfx/tools/dmsh_build.py`: the NVN2 compute driver bank, the `texh`/`imgh` facts), the Ryujinx translator sources (`dm/third_party/ryujinx/...`: two decoder and memory-instruction changes the lowered Switch 2 programs need), the iOS gamepad (`DmGamepad.m`: the `DM_TOUCH_AUTORUN` option) and `dmsh_chain.lock` (the fingerprints of the rebuilt translator below). Since the Link Play change (46 files) and internet play (54 files): the co-op module as new files `dm/host/coop.h`, `coop_int.h` and `hle_coop_{ldn,vnet,mpnet,plat,disc,auth}.c` (this project's own MIT code, shared with the Windows port), its routing in `hle_offline.c`, `hle_net.c` and `hle_misc.c`, the launcher's Link Play switch with internet play, the room code and the relay address under it (`dm/games/settings.tsv`; the last two are the launcher's new `text` kind, launcher.json schema 2: `dm/games/settings.py`, `docs/launcher.schema.json`, `docs/frontends.md`, `dm/ios/Sources/LauncherRow.swift` and `LauncherSettings.swift`, `dm/mac/launcher.c`, `dm/tools/tests/test_launcher_settings.py`), the local network keys in `dm/ios/project.yml` and `dm/mac/pack_app.py` (which also writes the text rows' saved-choice rule), and their rows in `docs/env_switches.tsv` ([docs/link-play-review.md](../docs/link-play-review.md)). |

## Rebuilding the translator yourself

With the .NET 10 **SDK** installed, in the patched checkout:

```bash
cd nxnative/dm/gfx/tools/dmsh_xlate && dotnet publish -c Release -o ../dmsh_xlate_bin
cd .. && python3 tool_lock.py --update-lock
```

The lock then names your build; nothing else changes.

## Xcode versions

nxnative's lock records Xcode 27.2's Metal compiler. The build sets `DM_ALLOW_OTHER_METAL=1` (nxnative's own escape
hatch for another compiler), reports the compiler it used, and keeps that build's libraries in cache entries keyed by
that compiler. The port was built and played with Xcode 27.0's compiler.

## Updating to a newer nxnative

The patch is against c1df4dd. A newer upstream may conflict: `git apply --3way` in the checkout, resolve, rebuild the
host (`nxs2.py build` does), and if `dmsh_chain.py` or `dmsh_build.py` changed, the chain version changes too and every
shader library is compiled again (half an hour or so).

## In this fork

This fork builds only the Linux game. It keeps `nxnative-switch2.patch` unchanged, with the pinned commit, so that upstream's patch updates merge
cleanly: the patch still carries the Mac, iPhone and Link Play parts, which `nxs2.py` no longer uses. The Mac copy of the translator
(`host/dmsh_xlate_bin/`), the prebuilt Linux copy (`host/windows/dmsh_xlate_bin/`; `setup` now builds the translator from source) and the Windows iPhone-builder patches (`host/windows/build_app.patch`, `fork-extras.patch`) were deleted.
