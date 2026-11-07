# OoT Native runtime v0.1.4 test

This update replaces the Linux runtime and the packaged tools. **If you already have a build from v0.1.2 or v0.1.3, do not rebuild the game.** The runtime ABI is still 1, so your existing build keeps working: extract this package and use the same `OOT_NATIVE_HOME` mechanism as before.

What changed since v0.1.3:

- The default number of build jobs is now 90% of the available threads, adjustable with the jobs option (`--jobs N`, `OOT_NATIVE_JOBS=N`, or the Build jobs field in the launcher). It only matters if you build again.
- The release package is now reproducible: two independent builds of the same commit produce identical files.
- No change in game behavior.

1. Download and extract `oot-native-linux-x86_64-v0.1.4-2026-11-07.tar.xz`.
2. Check the archive against its sidecar: `sha256sum -c oot-native-linux-x86_64-v0.1.4-2026-11-07.tar.xz.sha256`.
3. Point the existing build at the extracted package and launch:

   ```sh
   OOT_NATIVE_HOME="/path/to/oot-native-linux-x86_64-v0.1.4-2026-11-07" \
     "/path/to/existing-build/out/play-oot-2.0.0" --fullscreen
   ```

   You can also open `oot-native-launcher` from the new package and select the same existing build folder. Do not run Build.

Movie decoding stays enabled by default. `NXRT_MOVIE_DECODE=0` selects the timed fallback and `NXRT_LOG_VERBOSE=1` restores per-call HLE logging; keep both unset for normal reports.

Please report whether the intro finishes and the game continues, plus the package version, your Linux distribution and version, GPU and driver. If it fails, send the complete terminal output and the runtime logs and crash reports from `~/.local/share/oot-native/2.0.0/logs/` (`*.log` and `*.crash.txt`). Do not send game dumps, `game.so`, save data, extracted files, or video from the game.
