# OoT Native runtime v0.1.5 test

This update replaces the Linux runtime and the packaged tools. **Rebuild the game with this package; do not reuse a build from v0.1.4 or earlier.** On Linux the runtime now exports only its `rt_*` and `adapter_*` symbols, and the release's ABI check marks this version as requiring a rebuild.

What changed since v0.1.4:

- Faster rendering in heavy scenes: GPU write-backs only write the memory pages that changed, a compute write-back that covers whole pages keeps the GPU copy current, the upload staging ring is larger, and the sync pass skips memory pools nobody wrote. In the forest this measured about 2.5x more presented frames per second (22.7 vs 8.9).
- Optional FSR1 upscaling and present color controls (both off by default, see below).
- Audio: with SDL's PipeWire driver, the runtime keeps one device period queued ahead, so requests no longer run dry and get padded with silence. `NXRT_AUDIO_RESERVE=0` turns this off.
- New diagnostic `NXRT_VK_ROBUST=1` (driver bounds checks on buffers and images; `NXRT_VK_ROBUST=0` forces them off). Leave it unset unless asked.
- The log no longer repeats the HDR paper-white query every frame.

1. Download and extract `oot-native-linux-x86_64-v0.1.5-2026-11-07.tar.xz`.
2. Check the archive against its sidecar: `sha256sum -c oot-native-linux-x86_64-v0.1.5-2026-11-07.tar.xz.sha256`.
3. Build again from the new package. Open `oot-native-launcher` from the extracted package, choose your own `.nup` dump and an empty build directory, and run Build (at least 45 GB free). Or from the command line:

   ```sh
   cd "/path/to/oot-native-linux-x86_64-v0.1.5-2026-11-07"
   ./oot-native-launcher --build "/path/to/your-dump.nup" --out "$HOME/Games/oot-native-build-0.1.5"
   "$HOME/Games/oot-native-build-0.1.5/out/play-oot-2.0.0" --fullscreen
   ```

   Once the new build works, you can delete the old build directory.

Optional present settings, read when the game starts (set them in front of the play command):

- `NXRT_FSR=1` uses FSR1 (EASU) to upscale when the window is larger than the game picture. `NXRT_FSR_SHARPNESS=0..1` adds the RCAS sharpening pass (default 0 = no RCAS; higher is sharper). If FSR1 cannot start on your GPU, the log says `FSR unavailable` and the standard path is used.
- `NXRT_COLOR_GAMMA` (default 1, above 0; above 1 brightens mid-tones), `NXRT_COLOR_BRIGHTNESS` (multiplier, default 1) and `NXRT_COLOR_SATURATION` (default 1; 0 is grayscale).

   ```sh
   NXRT_FSR=1 NXRT_FSR_SHARPNESS=0.2 NXRT_COLOR_SATURATION=1.1 \
     "$HOME/Games/oot-native-build-0.1.5/out/play-oot-2.0.0" --fullscreen
   ```

Movie decoding stays enabled by default. `NXRT_MOVIE_DECODE=0` selects the timed fallback and `NXRT_LOG_VERBOSE=1` restores per-call HLE logging; keep both unset for normal reports.

Please report whether the build finishes, whether the intro finishes and the game continues, which optional settings you used, plus the package version, your Linux distribution and version, GPU and driver. If it fails, send the complete terminal output and the runtime logs and crash reports from `~/.local/share/oot-native/2.0.0/logs/` (`*.log` and `*.crash.txt`). Do not send game dumps, `game.so`, save data, extracted files, the build directory, or video from the game.
