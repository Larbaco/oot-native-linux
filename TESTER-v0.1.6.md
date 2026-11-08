# OoT Native runtime v0.1.6 test

This release requires a fresh game build. **Rebuild with this package; do not reuse a build from v0.1.5 or earlier.** `PushComputeConstants` and `PushComputeConstantsDeferred` changed from stubs to implemented host calls, which changes the build-time host-call registry. The shader cache version also changed for the `image_offset` pass: affected shaders are invalidated and translated again automatically when needed, so no manual shader translation is required.

What changed:

- GPU write-back now adopts complete-page results, writes only differing data, merges CPU writes that happen after recording, and preserves CPU-written pages during large landings. Exposure read-and-reset uses the v3 conversion path. Uploads have a barrier before queue submission, and dirty-page lookup uses an ordered index.
- A report queue and upload barrier were added. P3 descriptors and `PushComputeConstants` are implemented. `NXRT_VK_PUSH_CC=0` disables pushed compute constants.
- ISA lowering now covers `TLD4.PTP`, `SUATOM.P`, `TEX.F16` and `F2FP`.
- When the built-in NV12 stand-in is active, the translated movie compute chain is skipped. This fixes a stall right after the first movie frame on some setups. `NXRT_MOVIE_CHAIN_SKIP=0` restores the previous behaviour.

For diagnosis, the new runtime switches include `NXRT_VK_PUSH_CC=0`, `NXRT_VK_UPLOAD_BARRIER=0`, `NXRT_VK_WB_MERGE=0`, `NXRT_VK_ADOPT_STRICT=0` and `NXRT_VK_EXPO_FIX=0`. Leave them unset for normal use; change one at a time when isolating a report.

1. Download and extract `oot-native-linux-x86_64-v0.1.6-2026-11-08.tar.xz`.
2. Check the archive: `sha256sum -c oot-native-linux-x86_64-v0.1.6-2026-11-08.tar.xz.sha256`.
3. Rebuild your game with the package using the launcher or your usual build procedure. Keep your dump, build output and save data private.
4. Check that the intro passes the first movie frame. In the log, confirm `[nvn] frame` lines and presents continue for about 8 seconds after that frame. Send the log if it stalls. `NXRT_SKIP_COMPUTE` is no longer needed in this version.
5. Report the package version, whether the build completes, the runtime logs and the exact settings changed. Do not send dumps, built game files or save data.
