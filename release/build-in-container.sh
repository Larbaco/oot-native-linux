#!/usr/bin/env bash
# Build-only entrypoint for Docker Ubuntu 22.04. The host make-release script
# mounts this checkout at /src and an NVMe work directory at /out.
set -euo pipefail

ROOT=/src
OUT=/out
PKG_NAME=${NXS2_RELEASE_PKG_NAME:-oot-native-linux-x86_64-v0.1.0}
PKG="$OUT/$PKG_NAME"
ART="$OUT/artifacts"
JOBS=${NXS2_JOBS:-8}
if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || (( JOBS > 12 )); then
  echo "NXS2_JOBS must be between 1 and 12" >&2
  exit 2
fi
require_root_space() {
  local root=${OOT_HOST_ROOT:-/host-root} available_kb
  [[ -d "$root" ]] || root=/
  available_kb=$(df -Pk "$root" | awk 'NR==2 {print $4}')
  df -h "$root"
  if [[ ! "$available_kb" =~ ^[0-9]+$ ]] || (( available_kb < 12582912 )); then
    echo "release build: stopping; host / must retain at least 12 GiB free" >&2
    exit 2
  fi
}
PYTHON_VERSION=3.12.12
MESON_VERSION=1.5.2
DAV1D_VERSION=1.5.1
FFMPEG_VERSION=7.1.1
DAV1D_SHA256=401813f1f89fa8fd4295805aa5284d9aed9bc7fc1fdbe554af4292f64cbabe21
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
mkdir -p "$OUT/tmp" "$OUT/toolchains" "$ART" "$PKG/builder/bin" "$PKG/builder/lib" "$PKG/builder/ampere-lower"
if ! command -v meson >/dev/null; then
  require_root_space
  nice -n 19 ionice -c3 /opt/release-venv/bin/python -m pip install --disable-pip-version-check --no-cache-dir "meson==$MESON_VERSION"
fi
if ! command -v nasm >/dev/null; then
  require_root_space
  nice -n 19 ionice -c3 apt-get update
  require_root_space
  nice -n 19 ionice -c3 apt-get install -y --no-install-recommends nasm
  apt-get clean
  rm -rf /var/lib/apt/lists/*
fi

# Pinned, LGPL/BSD codec stack. Keep the exact upstream tarballs for the
# adjacent third-party-source archive assembled by make-release.sh.
CODEC_SRC="$OUT/codec-source"
mkdir -p "$CODEC_SRC"
DAV1D_TARBALL="$CODEC_SRC/dav1d-$DAV1D_VERSION.tar.xz"
FFMPEG_TARBALL="$CODEC_SRC/ffmpeg-$FFMPEG_VERSION.tar.xz"
[[ -s "$DAV1D_TARBALL" ]] || curl -fL --retry 3 "https://download.videolan.org/pub/videolan/dav1d/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.xz" -o "$DAV1D_TARBALL"
[[ -s "$FFMPEG_TARBALL" ]] || curl -fL --retry 3 "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" -o "$FFMPEG_TARBALL"
echo "$DAV1D_SHA256  $DAV1D_TARBALL" | sha256sum -c -
echo "$FFMPEG_SHA256  $FFMPEG_TARBALL" | sha256sum -c -
DAV1D_SRC="$OUT/tmp/dav1d-$DAV1D_VERSION"
FFMPEG_SRC="$OUT/tmp/ffmpeg-$FFMPEG_VERSION"
CODEC_PREFIX="$OUT/toolchains/codec"
FFMPEG_CONFIG_STAMP="$CODEC_PREFIX/.ffmpeg-$FFMPEG_VERSION-libdav1d-decoder-configured"
export PKG_CONFIG_PATH="$CODEC_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$CODEC_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [[ ! -f "$CODEC_PREFIX/lib/pkgconfig/dav1d.pc" ]]; then
  tar -xf "$DAV1D_TARBALL" -C "$OUT/tmp"
  if [[ ! -f "$OUT/tmp/dav1d-build/build.ninja" ]]; then rm -rf "$OUT/tmp/dav1d-build"; fi
  require_root_space
  nice -n 19 ionice -c3 meson setup "$OUT/tmp/dav1d-build" "$DAV1D_SRC" --prefix="$CODEC_PREFIX" --libdir=lib -Denable_tools=false -Denable_tests=false
  require_root_space
  nice -n 19 ionice -c3 ninja -C "$OUT/tmp/dav1d-build" -j "$JOBS"
  nice -n 19 ionice -c3 ninja -C "$OUT/tmp/dav1d-build" install
fi
FFMPEG_LIBDIR=
if [[ -f "$CODEC_PREFIX/lib/pkgconfig/libavcodec.pc" ]]; then
  FFMPEG_LIBDIR=$(pkg-config --variable=libdir libavcodec 2>/dev/null || true)
fi
if [[ ! -f "$FFMPEG_CONFIG_STAMP" || ! -f "$CODEC_PREFIX/lib/pkgconfig/libavcodec.pc" || "$FFMPEG_LIBDIR" != "$CODEC_PREFIX/lib" ]]; then
  rm -rf "$FFMPEG_SRC"
  rm -f "$FFMPEG_CONFIG_STAMP"
  tar -xf "$FFMPEG_TARBALL" -C "$OUT/tmp"
  require_root_space
  (cd "$FFMPEG_SRC" && nice -n 19 ionice -c3 ./configure \
    --prefix="$CODEC_PREFIX" --libdir="$CODEC_PREFIX/lib" --pkgconfigdir="$CODEC_PREFIX/lib/pkgconfig" \
    --disable-gpl --disable-nonfree --disable-programs --disable-doc --disable-everything \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-postproc \
    --enable-shared --disable-static --enable-avcodec --enable-avformat --enable-avutil \
    --enable-decoder=libdav1d --enable-parser=av1 --enable-demuxer=matroska --enable-protocol=file \
    --enable-libdav1d --extra-cflags="-I$CODEC_PREFIX/include" \
    --extra-ldflags="-L$CODEC_PREFIX/lib")
  require_root_space
  nice -n 19 ionice -c3 make -C "$FFMPEG_SRC" -j "$JOBS"
  nice -n 19 ionice -c3 make -C "$FFMPEG_SRC" install
  touch "$FFMPEG_CONFIG_STAMP"
fi
PYTHON_TARBALL="$OUT/tmp/Python-$PYTHON_VERSION.tgz"
PYTHON_SOURCE="$OUT/tmp/Python-$PYTHON_VERSION"
if [[ ! -x "$PKG/python/bin/python3.12" ]]; then
  curl -fL --retry 3 "https://www.python.org/ftp/python/$PYTHON_VERSION/Python-$PYTHON_VERSION.tgz" -o "$PYTHON_TARBALL"
  tar -xzf "$PYTHON_TARBALL" -C "$OUT/tmp"
  require_root_space
  (cd "$PYTHON_SOURCE" && \
    CFLAGS="-O2 -ffile-prefix-map=$PYTHON_SOURCE=." \
    LDFLAGS='-Wl,-rpath,$ORIGIN/../lib' \
    ./configure --prefix="$PKG/python" --enable-shared --with-ensurepip=install --enable-loadable-sqlite-extensions && \
    make -j"$JOBS" && make install)
  patchelf --set-rpath '$ORIGIN/../lib' "$PKG/python/bin/python3.12"
  ln -sf python3.12 "$PKG/python/bin/python3"
fi
# The builder never imports dbm/gdbm. These optional CPython extensions pull in
# host gdbm shared libraries, so leave them out of the embedded runtime.
find "$PKG/python/lib/python3.12/lib-dynload" -maxdepth 1 -type f \
  \( -name '_dbm*.so' -o -name '_gdbm*.so' \) -delete
PYTHON="$PKG/python/bin/python3"
SITE="$PKG/python/lib/python3.12/site-packages"
mkdir -p "$SITE"
"$PYTHON" -m pip install --disable-pip-version-check --no-compile --no-warn-script-location --target "$SITE" -r "$ROOT/release/requirements-runtime.txt"

export PATH="$PKG/python/bin:/opt/dotnet:/opt/release-venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export DOTNET_ROOT=/opt/dotnet DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
export NXNATIVE="$ROOT/nxnative" NXL_TOOLCHAINS="$OUT/toolchains" NXS2_BIN="$PKG/builder/bin"
export NXS2_ZBIC_UNPACK="$PKG/builder/bin/zbic-unpack" NXS2_JOBS="$JOBS" PYTHONDONTWRITEBYTECODE=1 PYTHONUTF8=1
export NXS2_RELEASE_PACKAGE=1
export NXS2_CMAKE_ARGS="-DNXRT_ENABLE_FFMPEG=ON"

cd "$ROOT"
require_root_space
nice -n 19 ionice -c3 "$PYTHON" nxs2.py setup 2>&1 | tee "$OUT/setup.log"
BUILD="$ROOT/windows/build/linux-0400486024DEC000"
require_root_space
nice -n 19 ionice -c3 cmake --build "$BUILD" -j "$JOBS" 2>&1 | tee "$OUT/ctest-build.log"
export LD_LIBRARY_PATH="$NXL_TOOLCHAINS/llvm-23.1.4/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
require_root_space
nice -n 19 ionice -c3 ctest --test-dir "$BUILD" --output-on-failure --parallel "$JOBS" 2>&1 | tee "$OUT/ctest.log"

LLVM="$OUT/toolchains/llvm-23.1.4"
SDL="$OUT/toolchains/SDL3-3.4.18"
for name in nxanalyze nxlift nxrt nxrt-movie-decode zbic-unpack; do
  install -m 0755 "$BUILD/bin/$name" "$PKG/builder/bin/$name"
done
install -m 0755 "$LLVM/bin/ld.lld" "$PKG/builder/bin/ld.lld"
install -m 0755 "$(command -v spirv-opt)" "$PKG/builder/bin/spirv-opt"
install -m 0755 "$(command -v spirv-val)" "$PKG/builder/bin/spirv-val"
install -m 0644 "$BUILD/bin/libSDL3.so.0" "$PKG/builder/lib/libSDL3.so.0"
install -m 0644 "$CODEC_PREFIX"/lib/libdav1d.so.* "$PKG/builder/lib/"
install -m 0644 "$CODEC_PREFIX"/lib/libavutil.so.* "$PKG/builder/lib/"
install -m 0644 "$CODEC_PREFIX"/lib/libavcodec.so.* "$PKG/builder/lib/"
install -m 0644 "$CODEC_PREFIX"/lib/libavformat.so.* "$PKG/builder/lib/"
for codec_lib in "$PKG"/builder/lib/libdav1d.so.* "$PKG"/builder/lib/libavutil.so.* "$PKG"/builder/lib/libavcodec.so.* "$PKG"/builder/lib/libavformat.so.*; do
  patchelf --set-rpath '$ORIGIN' "$codec_lib"
done
patchelf --set-rpath '$ORIGIN/../lib' "$PKG/builder/bin/nxrt-movie-decode"
install -m 0755 "$ROOT/ampere-lower/ampere-lower" "$PKG/builder/ampere-lower/ampere-lower"
patchelf --set-rpath '$ORIGIN/../lib' "$PKG/builder/bin/nxrt"
install -D -m 0644 "$FFMPEG_SRC/COPYING.LGPLv2.1" "$PKG/LICENSES/FFmpeg-LGPL-2.1.txt"
install -D -m 0644 "$DAV1D_SRC/COPYING" "$PKG/LICENSES/dav1d-BSD-2-Clause.txt"

require_root_space
"$PYTHON" -m unittest discover -s tests -v 2>&1 | tee "$OUT/python-tests.log"
"$PYTHON" -m unittest discover -s tools/tests -v 2>&1 | tee "$OUT/tool-tests.log"
"$PYTHON" - <<'PY'
import os, subprocess, zstandard
raw=b"synthetic ZBIC frame for the isolated helper test"*9
frame=zstandard.ZstdCompressor(write_content_size=True).compress(raw)
encoded=b"ZBIC"+frame[4:]
p=subprocess.run([os.environ["NXS2_ZBIC_UNPACK"],"--stream"],input=encoded,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=False)
if p.returncode or p.stdout!=raw: raise SystemExit(p.stderr.decode("utf-8","replace") or "synthetic ZBIC helper round trip failed")
print("PASS: synthetic ZBIC frame round-tripped through zbic-unpack subprocess")
PY

while IFS= read -r source; do
  [[ -z "$source" || "$source" == \#* ]] && continue
  install -D -m 0644 "$ROOT/$source" "$PKG/builder/$source"
done < "$ROOT/release/package-files.txt"

install -D -m 0644 "$ROOT/release/GPL-3.0-or-later.txt" "$PKG/LICENSE"
install -D -m 0644 "$ROOT/release/GPL-3.0-or-later.txt" "$PKG/LICENSES/GPL-3.0-or-later.txt"
install -D -m 0644 "$ROOT/LICENSES/MIT-project.txt" "$PKG/LICENSES/MIT-project.txt"
install -D -m 0644 "$ROOT/release/MIT-license.txt" "$PKG/LICENSES/MIT-license.txt"
install -D -m 0644 "$ROOT/ampere-lower/LICENSE" "$PKG/LICENSES/ampere-lower-GPL-3.0-or-later.txt"
install -D -m 0644 "$ROOT/third_party/nup/LICENSE" "$PKG/LICENSES/nup-MIT.txt"
install -D -m 0644 "$ROOT/windows/src/nxload/third_party/zbic/LICENSE" "$PKG/LICENSES/zbic-GPL-2.0-only.txt"
install -D -m 0644 /usr/share/common-licenses/Apache-2.0 "$PKG/LICENSES/Apache-2.0.txt"
install -D -m 0644 "$ROOT/windows/src/nxrt/vk/third_party/astcenc/LICENSE.txt" "$PKG/LICENSES/astc-encoder-Apache-2.0.txt"
install -D -m 0644 "$ROOT/windows/src/nxrt/vk/third_party/bc7enc/LICENSE" "$PKG/LICENSES/bc7enc-MIT.txt"
install -D -m 0644 "$ROOT/windows/src/nxrt/third_party/nxnative-LICENSE" "$PKG/LICENSES/nxnative-MIT.txt"
install -D -m 0644 "$ROOT/release/MIT-license.txt" "$PKG/LICENSES/Gommon-MIT.txt"
install -D -m 0644 "$ROOT/release/MIT-license.txt" "$PKG/LICENSES/System.CodeDom-MIT.txt"
NUGET_ROOT=${NUGET_PACKAGES:-$HOME/.nuget/packages}
if [[ -f "$NUGET_ROOT/gommon/2.8.1.2/content/LICENSE" ]]; then
  cp -f "$NUGET_ROOT/gommon/2.8.1.2/content/LICENSE" "$PKG/LICENSES/Gommon-MIT.txt"
fi
if [[ -f "$NUGET_ROOT/system.codedom/4.4.0/LICENSE.TXT" ]]; then
  cp -f "$NUGET_ROOT/system.codedom/4.4.0/LICENSE.TXT" "$PKG/LICENSES/System.CodeDom-MIT.txt"
fi
if grep -q 'respective authors' "$PKG/LICENSES/Gommon-MIT.txt" "$PKG/LICENSES/System.CodeDom-MIT.txt"; then
  echo 'missing real upstream license text for a translator NuGet dependency' >&2
  exit 1
fi
# NuGet packages declare their licenses and copyright in the package metadata;
# upstream license texts below come from pinned source or the package metadata.
# Humanizer 3.0.10's nuspec copyright is reproduced in the MIT text header.
curl -fsSL "https://raw.githubusercontent.com/Microsoft/Microsoft.IO.RecyclableMemoryStream/e29a28387da9018fa9605a1dcb3f7a0435aa9974/LICENSE" \
  -o "$PKG/LICENSES/RecyclableMemoryStream-MIT.txt"
{
  printf '%s\n\n' 'MsgPack.Cli 1.0.1' 'Copyright 2010-2016 FUJIWARA, Yusuke, all rights reserved.'
  cat /usr/share/common-licenses/Apache-2.0
} > "$PKG/LICENSES/MsgPack.Cli-Apache-2.0.txt"
python3 - "$ROOT/release/MIT-license.txt" "$PKG/LICENSES/Humanizer-MIT.txt" <<'PY'
import pathlib, sys
source, target = map(pathlib.Path, sys.argv[1:])
text = source.read_text(encoding='utf-8')
placeholder = ('Copyright (c) the respective authors and contributors identified in\n'
               'THIRD-PARTY-NOTICES.md and in the corresponding-source archive.')
if placeholder not in text:
    raise SystemExit('expected MIT license template changed; refusing a placeholder license')
text = text.replace(placeholder, 'Copyright (c) .NET Foundation and Contributors', 1)
target.write_text(text, encoding='utf-8')
PY
install -D -m 0644 "$ROOT/release/README-RELEASE.md" "$PKG/README-RELEASE.md"
install -D -m 0644 "$ROOT/README.md" "$PKG/README.md"
install -D -m 0644 "$ROOT/release/THIRD-PARTY-NOTICES.md" "$PKG/THIRD-PARTY-NOTICES.md"
install -D -m 0644 "$ROOT/release/THIRD-PARTY-SOURCE.md" "$PKG/THIRD-PARTY-SOURCE.md"
install -D -m 0644 "$ROOT/TESTER-v0.1.3.md" "$PKG/TESTER-v0.1.3.md"
install -D -m 0644 /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf "$PKG/fonts/DejaVuSans.ttf"
install -D -m 0644 /usr/share/doc/fonts-dejavu-core/copyright "$PKG/LICENSES/DejaVu-font-license.txt"

for name in dmsh_xlate.dll dmsh_xlate.deps.json dmsh_xlate.runtimeconfig.json Ryujinx.Common.dll Ryujinx.Graphics.Shader.dll Spv.Generator.dll Gommon.dll Humanizer.dll Microsoft.IO.RecyclableMemoryStream.dll MsgPack.dll System.CodeDom.dll; do
  install -m 0644 "$ROOT/nxnative/dm/gfx/tools/dmsh_xlate_bin/$name" "$PKG/builder/bin/$name"
done
cp -a "$ROOT/nxnative/LICENSE" "$PKG/LICENSES/nxnative-upstream-MIT.txt"
cp -a "$ROOT/nxnative/dm/third_party/ryujinx/LICENSE.txt" "$PKG/LICENSES/Ryujinx-MIT.txt"
cp -a "$ROOT/nxnative/dm/third_party/ryujinx/src/Spv.Generator/LICENSE" "$PKG/LICENSES/Spv.Generator-MIT.txt"
cp -a "$SDL/share/licenses/SDL3/LICENSE.txt" "$PKG/LICENSES/SDL3-zlib.txt"
install -D -m 0644 "$ROOT/release/LLVM-LICENSE.txt" "$PKG/LICENSES/LLVM-Apache-2.0-with-LLVM-exception.txt"
cp -a /usr/share/doc/spirv-tools/copyright "$PKG/LICENSES/SPIRV-Tools-copyright.txt"
if [[ -f /usr/share/doc/spirv-tools/NOTICE ]]; then
  cp -a /usr/share/doc/spirv-tools/NOTICE "$PKG/LICENSES/SPIRV-Tools-NOTICE.txt"
fi
cp -a "$OUT/tmp/Python-$PYTHON_VERSION/LICENSE" "$PKG/LICENSES/Python-PSF-2.0.txt"
cp -a "$OUT/tmp/Python-$PYTHON_VERSION/Doc/license.rst" "$PKG/LICENSES/Python-stdlib-third-party-licenses.rst"
"$PYTHON" - "$OUT/tmp/Python-$PYTHON_VERSION/Doc/license.rst" "$PKG/LICENSES" <<'PY'
import pathlib, sys
source, out = map(pathlib.Path, sys.argv[1:])
text = source.read_text(encoding='utf-8')
sections = (
    ('expat', 'expat\n-----\n', 'libffi\n------\n', 'Python-expat-MIT.txt'),
    ('libmpdec', 'libmpdec\n--------\n', 'W3C C14N test suite\n-------------------\n', 'Python-libmpdec-BSD-2-Clause.txt'),
)
for name, start_marker, end_marker, filename in sections:
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    excerpt = text[start:end].rstrip() + '\n'
    if 'Copyright' not in excerpt or 'Permission' not in excerpt and 'Redistribution' not in excerpt:
        raise SystemExit(f'CPython license section unexpectedly incomplete: {name}')
    (out / filename).write_text(excerpt, encoding='utf-8')
PY
cp -a /usr/share/doc/tcl8.6/copyright "$PKG/LICENSES/Tcl-8.6-copyright.txt"
cp -a /usr/share/doc/tk8.6/copyright "$PKG/LICENSES/Tk-8.6-copyright.txt"
/usr/local/bin/dotnet-install.sh --runtime dotnet --channel 10.0 --install-dir "$PKG/dotnet" --no-path
# This optional EventPipe/LTTng adapter targets liblttng-ust.so.0, which is not
# shipped in Ubuntu 22.04. The builder uses no LTTng tracing provider.
find "$PKG/dotnet/shared/Microsoft.NETCore.App" -maxdepth 2 -type f \
  -name 'libcoreclrtraceptprovider.so' -delete
cp -a "$PKG/dotnet/LICENSE.txt" "$PKG/LICENSES/dotnet-MIT.txt"
cp -a "$PKG/dotnet/ThirdPartyNotices.txt" "$PKG/LICENSES/dotnet-ThirdPartyNotices.txt"

# The one-file Tk launcher carries these Ubuntu shared libraries inside its
# PyInstaller archive. Preserve each installed package's complete copyright
# file and list the library-to-license mapping in the package notices.
declare -A EMBEDDED_LIB_PACKAGES=(
  [libX11.so.6]=libx11-6 [libXau.so.6]=libxau6 [libXdmcp.so.6]=libxdmcp6
  [libXext.so.6]=libxext6 [libXft.so.2]=libxft2 [libXrender.so.1]=libxrender1
  [libXss.so.1]=libxss1 [libbrotlicommon.so.1]=libbrotli1 [libbrotlidec.so.1]=libbrotli1
  [libbsd.so.0]=libbsd0 [libbz2.so.1.0]=libbz2-1.0 [libcrypto.so.3]=libssl3
  [libexpat.so.1]=libexpat1 [libfontconfig.so.1]=libfontconfig1 [libfreetype.so.6]=libfreetype6
  [liblzma.so.5]=liblzma5 [libmd.so.0]=libmd0 [libpng16.so.16]=libpng16-16
  [libuuid.so.1]=libuuid1 [libz.so.1]=zlib1g
)
for library in "${!EMBEDDED_LIB_PACKAGES[@]}"; do
  package=${EMBEDDED_LIB_PACKAGES[$library]}
  copyright="/usr/share/doc/$package/copyright"
  [[ -f "$copyright" ]] || { echo "missing copyright text for PyInstaller library $library ($package)" >&2; exit 1; }
  install -D -m 0644 "$copyright" "$PKG/LICENSES/PyInstaller-$package-copyright.txt"
done
{
  printf '%s\n' 'PyInstaller-bundled shared libraries and their complete Ubuntu copyright texts:'
  for library in "${!EMBEDDED_LIB_PACKAGES[@]}"; do
    printf '%s -> LICENSES/PyInstaller-%s-copyright.txt\n' "$library" "${EMBEDDED_LIB_PACKAGES[$library]}"
  done | sort
} > "$PKG/LICENSES/PyInstaller-embedded-libraries.txt"
{
  printf '\n## Shared libraries embedded in the GUI launcher\n\n'
  printf 'The PyInstaller one-file executable contains X11, freetype, fontconfig, PNG, expat, Brotli, bzip2, xz, zlib, UUID, BSD/libmd, OpenSSL, and Tcl/Tk libraries. Complete package copyright texts are provided in `LICENSES/`:\n\n'
  for library in "${!EMBEDDED_LIB_PACKAGES[@]}"; do
    printf -- '- `%s` (%s): `LICENSES/PyInstaller-%s-copyright.txt`.\n' "$library" "${EMBEDDED_LIB_PACKAGES[$library]}" "${EMBEDDED_LIB_PACKAGES[$library]}"
  done | sort
} >> "$PKG/THIRD-PARTY-NOTICES.md"

python3 - "$PKG/LICENSES" "$PYTHON" <<'PY'
import importlib.metadata as md
import pathlib
import shutil
import sys
out = pathlib.Path(sys.argv[1])
py = sys.argv[2]
site = pathlib.Path(py).resolve().parent.parent / "lib/python3.12/site-packages"
distributions = {d.metadata["Name"].lower(): d for d in md.distributions(path=[str(site)])}
for name in ("lz4", "zstandard", "fonttools", "Pillow"):
    dist = distributions[name.lower()]
    for item in dist.files or ():
        rel = pathlib.PurePosixPath(str(item))
        if "license" not in rel.name.lower() or len(rel.parts) > 4:
            continue
        src = pathlib.Path(dist.locate_file(item))
        if src.is_file():
            shutil.copy2(src, out / f"Python-{name}-{rel.name}")
PY

"$PYTHON" - "$PKG/THIRD-PARTY-NOTICES.md" <<'PY'
import importlib.metadata as md
import pathlib
import platform
out = pathlib.Path(__import__('sys').argv[1])
with out.open('a', encoding='utf-8') as f:
    f.write("\n## Versions in this build\n\n")
    f.write(f"- Embedded builder Python: CPython {platform.python_version()} (PSF-2.0).\n")
for name in ("lz4", "zstandard", "fonttools", "Pillow"):
    d = md.distribution(name)
    with out.open('a', encoding='utf-8') as f:
        f.write(f"- Python package `{d.metadata['Name']}` {d.version}; its license text is in LICENSES/.\n")
with out.open('a', encoding='utf-8') as f:
    f.write("- The one-file GUI launcher is built with PyInstaller 6.12.0 and its installed Python dependencies; their licenses are in LICENSES/.\n")
    f.write("- System-provided glibc, Vulkan loader/driver, X11/Wayland, and audio libraries are not redistributed by this package.\n")
PY

python3 -m venv "$OUT/launcher-venv"
"$OUT/launcher-venv/bin/python" -m pip install --disable-pip-version-check --no-cache-dir pyinstaller==6.12.0
python3 - "$OUT/launcher-venv" "$PKG/LICENSES" "$PKG/THIRD-PARTY-NOTICES.md" <<'PY'
import importlib.metadata as md
import pathlib, shutil, sys
venv, out, notices = map(pathlib.Path, sys.argv[1:])
site = next((venv / 'lib').glob('python*/site-packages'))
selected = []
for dist in md.distributions(path=[str(site)]):
    name = dist.metadata.get('Name', 'unknown')
    version = dist.version
    selected.append(f'- PyInstaller environment dependency `{name}` {version} ({dist.metadata.get("License", "license in LICENSES/")}).')
    for item in dist.files or ():
        rel = pathlib.PurePosixPath(str(item))
        if not any(token in rel.name.lower() for token in ('license', 'copying', 'notice', 'exception')) or len(rel.parts) > 4:
            continue
        src = pathlib.Path(dist.locate_file(item))
        if src.is_file(): shutil.copy2(src, out / f'PyInstaller-{name}-{rel.name}')
boot = site / 'PyInstaller' / 'bootloader'
for item in boot.rglob('*'):
    if item.is_file() and any(token in item.name.lower() for token in ('license', 'copying', 'exception')):
        shutil.copy2(item, out / f'PyInstaller-bootloader-{item.name}')
with notices.open('a', encoding='utf-8') as f:
    f.write('\n### PyInstaller environment dependencies\n\n' + '\n'.join(sorted(selected)) + '\n')
PY
mkdir -p "$OUT/pyinstaller-work"
"$OUT/launcher-venv/bin/pyinstaller" --noconfirm --clean --onefile --name oot-native-launcher \
  --paths "$ROOT" --distpath "$PKG" --workpath "$OUT/pyinstaller-work" --specpath "$OUT" "$ROOT/launcher/oot_native_launcher.py"
chmod 0755 "$PKG/oot-native-launcher"
"$PKG/oot-native-launcher" --help > "$OUT/launcher-help.txt"

/usr/bin/python3 - "$PKG" "$LLVM/bin/llvm-strip" <<'PY'
import pathlib, re, shutil, subprocess, sys
root=pathlib.Path(sys.argv[1])
py=root/'python'
lib=py/'lib/python3.12'

# Keep only runtime Python files. Tests, IDE assets, installers, headers,
# static archives, caches, and Tk bindings are build-time material; the GUI
# launcher is already a one-file PyInstaller executable.
for rel in ('test','idlelib','turtledemo','ensurepip','lib2to3','tkinter','venv','config-3.12-x86_64-linux-gnu'):
    target=lib/rel
    if target.is_dir(): shutil.rmtree(target)
    elif target.exists(): target.unlink()
if (py/'include').exists(): shutil.rmtree(py/'include')
for pattern in ('pip*','idle*','2to3*','pydoc*','python3.12-config','python3-config'):
    for p in (py/'bin').glob(pattern):
        if p.is_file() or p.is_symlink(): p.unlink()
for p in (lib/'site-packages').glob('pip*'):
    if p.is_dir(): shutil.rmtree(p)
    elif p.exists(): p.unlink()
for p in list(lib.rglob('*')):
    if p.is_dir() and p.name=='__pycache__': shutil.rmtree(p)
    elif p.is_file() and p.suffix=='.pyc': p.unlink()
for p in lib.glob('config-*'):
    if p.is_dir(): shutil.rmtree(p)
    elif p.exists(): p.unlink()

# License notices, Python standard-library modules, and upstream metadata remain
# byte-for-byte intact. The release scanner exempts public author emails only in
# those paths and continues to reject owner-specific paths and identifiers.

# A NuGet assembly's CodeView record contains a build-machine PDB path.
# Normalize the debug-only path; the portable IL stays byte-for-byte intact.
marker=b'C:'+bytes((92,))+b'Users'+bytes((92,))
for p in (root/'builder/bin').glob('*.dll'):
    data=bytearray(p.read_bytes())
    lower=bytes(data).lower()
    needle=marker.lower()
    pos=0
    changed=False
    while True:
        pos=lower.find(needle,pos)
        if pos<0: break
        end=data.find(0,pos)
        if end<0: raise SystemExit(f'unterminated managed debug path: {p.name}')
        replacement=b'oot-native.pdb'
        if len(replacement)>end-pos: raise SystemExit(f'short managed debug path: {p.name}')
        data[pos:end]=replacement+b'\0'*(end-pos-len(replacement))
        lower=bytes(data).lower()
        pos+=len(replacement)
        changed=True
    if changed: p.write_bytes(data)

# LLVM's release lld embeds build-root strings in diagnostic literals. Strip
# DWARF first, then terminate each remaining absolute path in-place; this
# changes diagnostics only and keeps the ELF size and code offsets unchanged.
lld=root/'builder/bin/ld.lld'
subprocess.run([sys.argv[2],'--strip-debug',str(lld)],check=True)
path_pattern=re.compile(rb"""(?i)/(?:home|mnt)/[^\x00 \t\r\n"'(),:]*""")
def scrub_path(match):
    original=match.group(0)
    replacement=b'<build-path>'
    if len(original)<len(replacement): return b'X'*len(original)
    return replacement+b'\0'*(len(original)-len(replacement))
lld.write_bytes(path_pattern.sub(scrub_path,lld.read_bytes()))
with (root/'THIRD-PARTY-NOTICES.md').open('a',encoding='utf-8') as f:
    f.write('- The embedded Python runtime omits build and test material; its standard-library modules and all third-party license and notice texts are preserved verbatim.\n')
    f.write('- Debug and diagnostic build-root paths in bundled tool binaries are normalized; executable code and license texts are unchanged.\n')
print('trimmed Python build/test material and normalized non-runtime examples')
PY

find "$PKG/LICENSES" -type f -name '*.pyc' -delete

# Audit every non-glibc direct dependency of the embedded Python runtime and
# preserve the complete copyright text from its Ubuntu 22.04 runtime package.
# Wheels' private shared objects are already inside python/ and covered by
# their upstream package notices; system dependencies are mapped by SONAME.
declare -A PYTHON_SYSTEM_LIB_PACKAGES=(
  [libbz2.so.1.0]=libbz2-1.0 [libcrypt.so.1]=libcrypt1 [libcrypto.so.3]=libssl3
  [libffi.so.8]=libffi8 [liblzma.so.5]=liblzma5 [libncursesw.so.6]=libncursesw6
  [libnsl.so.2]=libnsl2 [libpanelw.so.6]=libtinfo6 [libreadline.so.8]=libreadline8
  [libsqlite3.so.0]=libsqlite3-0 [libssl.so.3]=libssl3 [libtcl8.6.so]=tcl8.6
  [libtinfo.so.6]=libtinfo6 [libtirpc.so.3]=libtirpc3 [libtk8.6.so]=tk8.6
  [libuuid.so.1]=libuuid1 [libz.so.1]=zlib1g
)
declare -A PYTHON_NOTICE_FILES=()
while IFS= read -r -d '' elf; do
  [[ "$(od -An -N4 -tx1 "$elf" | tr -d ' \n')" == 7f454c46 ]] || continue
  while IFS= read -r soname; do
    [[ -n "$soname" ]] || continue
    if [[ "$soname" =~ ^(libc|libm|libpthread|libdl|librt|libresolv|libutil|libanl)\.so ]] || \
       [[ "$soname" =~ ^ld-linux.*\.so ]] || \
       find "$PKG/python" -type f -name "$soname" -print -quit | grep -q .; then
      continue
    fi
    package=${PYTHON_SYSTEM_LIB_PACKAGES[$soname]:-}
    if [[ -z "$package" ]]; then
      echo "unmapped external Python runtime dependency: $soname ($elf)" >&2
      exit 1
    fi
    copyright="/usr/share/doc/$package/copyright"
    [[ -f "$copyright" ]] || { echo "missing Python runtime library copyright text: $copyright" >&2; exit 1; }
    target="LICENSES/Python-linked-$package-copyright.txt"
    if [[ -z "${PYTHON_NOTICE_FILES[$soname]:-}" ]]; then
      install -D -m 0644 "$copyright" "$PKG/$target"
      PYTHON_NOTICE_FILES[$soname]="$target"
    fi
  done < <(readelf -d "$elf" 2>/dev/null | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
done < <(find "$PKG/python" -type f -print0)
{
  printf '\n## CPython runtime dependencies\n\n'
  printf 'The embedded CPython 3.12 runtime uses the exact third-party notices from its source and its Ubuntu 22.04 linked libraries.\n'
  printf 'Embedded libmpdec (BSD-2-Clause) and expat (MIT) acknowledgements for `_decimal`, `pyexpat`, and `_elementtree` are in `LICENSES/Python-libmpdec-BSD-2-Clause.txt` and `LICENSES/Python-expat-MIT.txt`.\n\n'
  printf 'System shared libraries linked by Python runtime modules and their complete copyright texts:\n\n'
  for soname in "${!PYTHON_NOTICE_FILES[@]}"; do
    printf -- '- `%s`: `%s`.\n' "$soname" "${PYTHON_NOTICE_FILES[$soname]}"
  done | sort
  printf '\nCPython third-party acknowledgements are in `LICENSES/Python-stdlib-third-party-licenses.rst`; system library copyright texts are copied verbatim from Ubuntu package documentation.\n'
} >> "$PKG/THIRD-PARTY-NOTICES.md"
printf 'audited %d direct external shared-library dependencies of embedded Python\n' "${#PYTHON_NOTICE_FILES[@]}"

/usr/bin/python3 - "$PKG/README-RELEASE.md" "$PKG" <<'PY'
import pathlib, re, subprocess, sys
readme, root = map(pathlib.Path, sys.argv[1:])
glibc_versions=[]
glibcxx_versions=[]
for p in root.rglob('*'):
    if not p.is_file(): continue
    try: data=p.read_bytes()[:4]
    except OSError: continue
    if data == b'\x7fELF':
        out=subprocess.run(['objdump','-T',str(p)],capture_output=True,text=True,check=False).stdout
        glibc_versions.extend(re.findall(r'GLIBC_(\d+(?:\.\d+)+)',out))
        glibcxx_versions.extend(re.findall(r'GLIBCXX_(\d+(?:\.\d+)+)',out))
if not glibc_versions: raise SystemExit('no GLIBC symbol versions found in packaged ELF files')
if not glibcxx_versions: raise SystemExit('no GLIBCXX symbol versions found in packaged ELF files')
def key(v): return tuple(map(int,v.split('.')))
floor=max(glibc_versions,key=key)
glibcxx_floor=max(glibcxx_versions,key=key)
text=readme.read_text(encoding='utf-8').replace('@GLIBC_BASELINE@',floor).replace('@GLIBCXX_BASELINE@',glibcxx_floor)
readme.write_text(text,encoding='utf-8')
pathlib.Path('/out/glibc-baseline.txt').write_text(f'GLIBC_{floor}\nGLIBCXX_{glibcxx_floor}\n')
print(f'measured ABI floors: GLIBC_{floor}, GLIBCXX_{glibcxx_floor}')
PY

# The translator source snapshot is an exact file list from the pinned checkout;
# unrelated iOS, co-op, and other-title patch material is excluded.
mkdir -p "$ART/upstream-source"
git -C "$ROOT/nxnative" ls-files -z -- dm/gfx/tools/dmsh_xlate dm/third_party/ryujinx LICENSE > "$ART/upstream-source/files.zlist"
/usr/bin/python3 - "$ROOT/nxnative" "$ART/upstream-source" <<'PY'
import pathlib, shutil, sys
root, out = map(pathlib.Path, sys.argv[1:])
raw=(out/'files.zlist').read_bytes().split(b'\0')
paths=[]
for part in raw:
    if not part: continue
    rel=part.decode()
    if pathlib.PurePosixPath(rel).suffix.lower() in {'.dll','.exe','.pdb','.spv','.png','.jpg','.jpeg','.bmp','.bin'} or pathlib.PurePosixPath(rel).name.lower() in {'readme.md','readme.txt'}:
        continue
    src=root/rel
    if not src.is_file(): continue
    dst=out/'nxnative'/rel
    dst.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(src,dst)
    paths.append('upstream/nxnative/'+rel)
(out/'manifest.txt').write_text('\n'.join(sorted(paths))+'\n',encoding='utf-8')
(out/'nxnative/UPSTREAM-SOURCE.md').write_text(
    '# Translator source snapshot\n\n'
    'This is the exact translator source snapshot built for the release. It derives from '
    'seedds/nxnative commit c1df4ddaaedc3c18228065ea5b909688b79884e2 and includes the '
    'patched `dm/gfx/tools/dmsh_xlate` project and its patched Ryujinx source dependencies '
    'under `dm/third_party/ryujinx`. One generic Windows profile example was normalized in a comment to `%APPDATA%`; '
    'no executable code was changed. Rebuild with the .NET 10 SDK: `dotnet publish -c Release '
    '-o ../dmsh_xlate_bin -maxcpucount:4 -p:UseAppHost=false`.\n',encoding='utf-8')
(out/'manifest.txt').open('a').write('upstream/nxnative/UPSTREAM-SOURCE.md\n')
PY
rm -f "$ART/upstream-source/files.zlist"

/usr/bin/python3 - "$PKG" "$OUT/package-allowlist.txt" <<'PY'
import pathlib, sys
root, manifest=map(pathlib.Path,sys.argv[1:])
files=sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file())
manifest.write_text('\n'.join(files)+'\n',encoding='utf-8')
print(f'package staging: {len(files)} files before source archive and checksums')
PY

printf 'Ubuntu 22.04 container build complete\n' > "$OUT/build-result.txt"
