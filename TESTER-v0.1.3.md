# OoT Native Linux v0.1.3 tester notes

Please test with your existing build from v0.1.2. **Do not rebuild the game.**
The runtime and its packaged tools changed; the build output and `game.so` ABI
are intended to remain compatible.

The release package now includes a bundled FFmpeg/libdav1d AV1 decoder. Movie
decoding is enabled by default. To check the fallback behavior, launch the
runtime with `NXRT_MOVIE_DECODE=0`; the intro should still advance on timing
with no video pixels. `NXRT_LOG_VERBOSE=1` restores per-call HLE logging if a
diagnostic log is needed. Keep it unset for normal reports.

Please report whether the intro finishes and the game continues, plus the
package version, SteamOS version, GPU and driver, and a sanitized runtime log.
Do not send game dumps, `game.so`, save data, extracted files, or video from the
game.
