# Building and running OoT Native from source

These steps start from a fresh clone. You need your own dump of the game. This repository does not contain it and cannot
provide it.

## 1. What you need

**Hardware**

- An x86-64 CPU with **AVX2 and FMA3**.
- **16 GB** of RAM.
- A GPU and driver with **Vulkan 1.3**. Mesa RADV, ANV, and the NVIDIA proprietary driver are all supported.
- About **20 GB** free for the toolchains (inside the clone) and about **15 GB** for one build folder.

**Your dump**

- One `.nup` file, or a folder, of **The Legend of Zelda: Ocarina of Time (Switch 2)**, title `0400486024DEC000`,
  **update 2.0.0**, made from your own console with your own tools.
- It must be decrypted plain files. This tool cannot read an encrypted dump and does not try to.
- `check-dump` rejects other versions and dumps with missing files.

**Software**

On Ubuntu 24.04 or Debian 13:

```sh
sudo apt install python3 python3-venv git cmake ninja-build curl zstd glslang-tools spirv-tools \
     build-essential pkg-config libx11-dev libxext-dev libwayland-dev libxkbcommon-dev \
     libpulse-dev libudev-dev libvulkan-dev mesa-vulkan-drivers
```

On Fedora:

```sh
sudo dnf install python3 git cmake ninja-build curl zstd glslang spirv-tools gcc-c++ pkgconf \
     libX11-devel libXext-devel wayland-devel libxkbcommon-devel pulseaudio-libs-devel \
     systemd-devel vulkan-loader-devel
```

On Arch:

```sh
sudo pacman -S python git cmake ninja curl zstd glslang spirv-tools base-devel \
     libx11 libxext wayland libxkbcommon libpulse vulkan-icd-loader
```

You also need the **.NET 10 SDK** (the SDK, not only the runtime), because `setup` compiles the shader translator from
source. Use your distro's `dotnet-sdk-10.0` package or Microsoft's `dotnet-install.sh --channel 10.0`. If you install it
outside `PATH`, set `DOTNET_ROOT`.

Python 3.12 or newer is required. The Python modules go in a virtual environment:

```sh
python3 -m venv .venv
.venv/bin/pip install lz4 zbic fonttools pillow zstandard
```

## 2. Check the machine

```sh
.venv/bin/python nxs2.py doctor
```

`doctor` lists each missing tool. Install those tools and run it again until it reports nothing missing.

## 3. Set up the toolchain (one time)

```sh
.venv/bin/python nxs2.py setup
```

`setup` does the following, all inside the clone:

1. Downloads LLVM 23.1.4 (about 1.2 GB) into `toolchains/` and builds SDL3.
2. Clones [nxnative](https://github.com/seedds/nxnative) at the pinned commit into `nxnative/` and applies
   `host/nxnative-switch2.patch`.
3. Builds the `dmsh_xlate` shader translator with `dotnet publish`.
4. Builds `ampere-lower`, `nxanalyze`, `nxlift`, `nxrt` and `zbic-unpack`.

Logs are written next to `nxs2.py` as `setup-*.log`. Running `setup` again is safe. It skips work that is already done.

### Where the shader translator is

The repository carries no prebuilt translator. `setup` publishes the one it built to `$NXS2_BIN` (default `bin/` next to `nxs2.py`) and keeps a copy
in `nxnative/dm/gfx/tools/dmsh_xlate_bin/`; `nxs2.py` passes that folder to `nxshader.py` with `--xlate`. The developer tools that run `nxshader.py`
on their own (`tools/ndsr_validate.py`, the `nxshader.spirv_val_sample` test) look in the same places, in this order: `$NXS2_BIN`, `bin/`,
`nxnative/dm/gfx/tools/dmsh_xlate_bin/`, and, in a packaged runtime, `installer/host/windows/dmsh_xlate_bin/`. Without a `dmsh_xlate.dll` in any of
them they stop with a message naming the folders they tried. To use a translator built elsewhere, pass `--xlate <folder>` to `nxshader.py` or set
`NXS2_BIN`.

A shader cache is tied to the translator that made it: `nxshader.py` derives the cache's chain id from the translator's source hash and the hashes of
its DLLs, so a translator built from a different source or toolchain has a different chain and does not reuse an older cache.

## 4. Validate your dump

```sh
.venv/bin/python nxs2.py check-dump "/path/to/your dump.nup"
```

This reads only the dump's table of contents and the build IDs. Nothing is extracted or modified.

## 5. Build

```sh
nice -n 19 .venv/bin/python nxs2.py build "/path/to/your dump.nup" --out ~/Games/oot_native
```

The first build takes about 30 minutes on 8 threads. The game build uses 90% of the available threads by default (`oot_native_jobs.py`); use `-j N` or `OOT_NATIVE_JOBS=N` to change it
(`--full-speed` uses every available thread), and `taskset -c 0-7` to pin the build to specific CPUs, which also lowers the
thread count the default is computed from. `NXS2_JOBS=<n>` only limits the tool compile in `nxs2.py setup`. Later builds redo only what changed. Use `-v` to see every line the tools print. Full logs are
in `<build>/logs/`.

The build folder holds data derived from the game (`game.so`, the RomFS image, translated shaders).
**Keep it on your machine. Do not upload, share or commit it.**

## 6. Run

```sh
~/Games/oot_native/out/play-oot-2.0.0 --window 1600x900
```

The launcher accepts these options: `--fullscreen`, `--window WxH`, `--silent`, `--gamemode`. You can also run
`.venv/bin/python nxs2.py run ~/Games/oot_native`. Saves and runtime logs are written to
`~/.local/share/oot-native/2.0.0/`. On the first start the shaders are translated as the game asks for them and then cached.

**Optional GUI:** `python3 launcher/oot_native_launcher.py` lets you pick a dump and an empty build folder, validates the
dump, and runs the same build.

## Running without a screen (testing)

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy DM_AUDIO=0 \
  NXRT_VK_SHOT_EVERY=900 NXRT_VK_SHOT_DIR=shots \
  xvfb-run -a ~/Games/oot_native/out/play-oot-2.0.0
```

The screenshots in `shots/` show the game. Do not publish them.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `doctor` reports `.NET 10 SDK` missing | Install the SDK, not only the runtime, or set `DOTNET_ROOT`. |
| `check-dump` reports a build ID mismatch | The dump is not update 2.0.0. Only that version is supported. |
| `check-dump` reports a missing RomFS file | The dump is incomplete. Dump the game again. |
| Black screen with a plain RomFS folder | Use the build's `extract/RomFS.img`. The launcher does this by default. |
| `VK_ERROR_DEVICE_LOST` after some minutes | This is a known, rare GPU hang. Update your driver. The last committed save is kept. |
| Saves on NFS, eCryptfs or FUSE | These are supported through a fallback commit that uses a marker file. It is slower but safe. |

## Building only the native tools

If you want to build the tools without the driver script:

```sh
cmake -S windows -B windows/build/linux-0400486024DEC000 -G Ninja -DNXRT_GAME=0400486024DEC000 \
      -DNXW_LLVM_DIR=<LLVM 23 prefix> -DNXRT_SDL3_DIR=<SDL3 prefix> -DNXRT_VK_INCLUDE=<Vulkan headers>/include
cmake --build windows/build/linux-0400486024DEC000
```

### Keeping the toolchains outside the clone (development builds)

`setup` puts LLVM, SDL3 and the Vulkan headers in `toolchains/` next to `nxs2.py`. A development checkout that has several build folders or worktrees can keep
one copy elsewhere and point every build at it with `NXL_TOOLCHAINS=<dir>`. A build folder must never point into another project's or another
build job's work area: if that folder is cleaned, the compiler and the SDL3 library (`nxrt` has the SDL3 `lib` folder in its `RUNPATH`) disappear with it.

Fetch the toolchains once, without running the rest of `setup` (this is what `setup` runs first; it is skipped for anything already there):

```sh
NXL_TOOLCHAINS=~/dev/toolchains python3 -I -c "import sys; sys.path.insert(0, '.'); import nxs2, nxs2_linux as L; L.fetch_toolchain(nxs2)"
```

Then (re)configure and build the folder against it, with the same options `setup` uses (`-DNXRT_ENABLE_FFMPEG=ON` builds the movie decoder; drop it if FFmpeg's
development packages are not installed). Reconfiguring onto a different compiler path rebuilds everything once:

```sh
T=~/dev/toolchains
rm -rf windows/build/linux-0400486024DEC000/CMakeCache.txt windows/build/linux-0400486024DEC000/CMakeFiles   # only when moving an existing folder
PATH=$T/llvm-23.1.4/bin:$PATH cmake -S windows -B windows/build/linux-0400486024DEC000 -G Ninja -DNXRT_GAME=0400486024DEC000 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DNXW_LLVM_DIR=$T/llvm-23.1.4 -DNXRT_SDL3_DIR=$T/SDL3-3.4.18 \
      -DNXRT_VK_INCLUDE=$T/vulkan-headers/include -DNXRT_ENABLE_FFMPEG=ON
PATH=$T/llvm-23.1.4/bin:$PATH cmake --build windows/build/linux-0400486024DEC000
LD_LIBRARY_PATH=$T/llvm-23.1.4/lib NXS2_ZBIC_UNPACK=$PWD/windows/build/linux-0400486024DEC000/bin/zbic-unpack ctest --test-dir windows/build/linux-0400486024DEC000
```

Without a dump, `ctest` should report every test passed and three skipped (`nxanalyze.dump`, `s2tex.corpus`, `nxshader.spirv_val_sample`: they need a dump or
corpus). Check that nothing points outside your folders: `grep -c '<some other work area>' windows/build/*/CMakeCache.txt` and `readelf -d <build>/bin/nxrt | grep RUNPATH`.

### Running a build from a dump you already extracted

`nxrt` and `nxload` refuse a ZBIC-compressed `main`: it must be normalized first by the separate `zbic-unpack` tool (`nxs2.py build` does this in the build's
private ExeFS copy). If you run `nxrt` by hand on an ExeFS folder taken straight from a dump, copy the folder and run `zbic-unpack <copy>/main` (it works in place);
never unpack the only copy you have.
