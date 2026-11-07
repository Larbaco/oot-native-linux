#!/usr/bin/env bash
set -euo pipefail

PKG=/pkg
PY="$PKG/python/bin/python3"
BUILDER="$PKG/builder"
HOME=/tmp/oot-native-smoke/home
mkdir -p "$HOME" /tmp/oot-native-smoke/state/build
export HOME PATH=/usr/bin:/bin PYTHONHOME="$PKG/python" PYTHONDONTWRITEBYTECODE=1
export STD_FONT="$PKG/fonts/DejaVuSans.ttf" OOT_NATIVE_HOME="$PKG"
export NXS2_RELEASE_PACKAGE=1 NXS2_BIN="$BUILDER/bin" NXS2_LLD="$BUILDER/bin/ld.lld"
export NXS2_ZBIC_UNPACK="$BUILDER/bin/zbic-unpack" DOTNET_ROOT="$PKG/dotnet"
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
NXRT="$BUILDER/bin/nxrt"
REFERENCE_NXRT="$OOT_REFERENCE_PACKAGE/builder/bin/nxrt"
STATE=/tmp/oot-native-smoke/state

(
  cd "$PKG"
  sha256sum -c SHA256SUMS > /tmp/oot-native-smoke/state/package-checksums.log
)
echo "PASS: package internal SHA256SUMS ($(wc -l < /tmp/oot-native-smoke/state/package-checksums.log) files)"

# The Vulkan backend must be present, while its function-pointer slots stay out
# of nxrt's dynamic exports. The loader is opened with dlopen at runtime.
if nm -D --defined-only "$NXRT" | awk '{print $NF}' | grep -Eq '^vk[A-Z]'; then
  echo 'FAIL: nxrt exports a Vulkan API symbol' >&2
  nm -D --defined-only "$NXRT" | awk '{print $NF}' | grep -E '^vk[A-Z]' >&2 || true
  exit 1
fi
echo 'PASS: nxrt exports no vk[A-Z]* dynamic symbols'
if ! strings "$NXRT" | grep -F 'libvulkan.so.1' >/dev/null; then
  echo 'FAIL: nxrt has no libvulkan.so.1 loader reference' >&2; exit 1
fi
if ! strings "$NXRT" | grep -F '[vk] ' >/dev/null; then
  echo 'FAIL: nxrt internal Vulkan backend log strings are absent' >&2; exit 1
fi
echo 'PASS: nxrt contains libvulkan.so.1 and internal [vk] backend strings'

nxrt_backend_gate() {
  local label=$1 exe=$2 log_dir=$3 output rc
  set +e
  output=$("$exe" --gfx vulkan --log "$log_dir" 2>&1)
  rc=$?
  set -e
  printf '%s\n' "$output" > "$STATE/$label-backend.log"
  if (( rc != 0 )) || ! grep -Fq '[gfx] backend: vk' <<<"$output" || \
      ! grep -Fq 'no ExeFS folder given' <<<"$output"; then
    printf 'FAIL: %s nxrt did not select built-in Vulkan without loading a game (rc=%s):\n%s\n' \
      "$label" "$rc" "$output" >&2
    exit 1
  fi
  grep -F '[gfx] backend: vk' <<<"$output"
  echo "PASS: $label nxrt has Vulkan backend; no ExeFS/game was loaded"
}

# No ExeFS argument is supplied: nxrt initializes only its host logger/address
# space and reports backend selection. The reference is the released v0.1.0.
nxrt_backend_gate 'v0.1.2' "$NXRT" "$STATE/v012-log"
nxrt_backend_gate 'v0.1.0-reference' "$REFERENCE_NXRT" "$STATE/v010-log"

ldd_output=$(ldd "$NXRT" 2>&1) || { printf '%s\n' "$ldd_output" >&2; exit 1; }
if grep -q 'not found' <<<"$ldd_output"; then
  printf 'FAIL: unresolved nxrt dependencies:\n%s\n' "$ldd_output" >&2; exit 1
fi
printf '%s\n' "$ldd_output" > "$STATE/nxrt-ldd.log"
echo 'PASS: ldd resolved all nxrt dependencies'

# A constructor in this linked probe calls libvulkan before nxrt main. --help
# is intentionally an invalid nxrt argument; return 2 plus usage proves the
# process reached main without loading ExeFS or any game data.
cc -shared -fPIC -o "$STATE/vulkan-loader-probe.so" /release-src/vulkan-loader-probe.c -lvulkan
set +e
probe_output=$(LD_PRELOAD="$STATE/vulkan-loader-probe.so" "$NXRT" --help 2>&1)
probe_rc=$?
set -e
printf '%s\n' "$probe_output" > "$STATE/vulkan-loader-probe.log"
if (( probe_rc != 2 )) || ! grep -Fq 'usage: nxrt' <<<"$probe_output"; then
  printf 'FAIL: Vulkan LD_PRELOAD probe did not reach nxrt argument handling (rc=%s):\n%s\n' \
    "$probe_rc" "$probe_output" >&2
  exit 1
fi
echo 'PASS: Vulkan LD_PRELOAD constructor probe completed without a crash (nxrt usage rc=2)'

for license in \
  Python-libmpdec-BSD-2-Clause.txt Python-expat-MIT.txt Python-stdlib-third-party-licenses.rst \
  Python-linked-libsqlite3-0-copyright.txt Python-linked-libffi8-copyright.txt \
  Python-linked-libncursesw6-copyright.txt Python-linked-libssl3-copyright.txt \
  Python-linked-zlib1g-copyright.txt Python-linked-libbz2-1.0-copyright.txt \
  Python-linked-liblzma5-copyright.txt; do
  test -s "$PKG/LICENSES/$license" || { echo "FAIL: missing Python runtime license $license" >&2; exit 1; }
done
for soname in libsqlite3.so.0 libffi.so.8 libncursesw.so.6 libssl.so.3 libcrypto.so.3 \
  libz.so.1 libbz2.so.1.0 liblzma.so.5; do
  grep -Fq "\`$soname\`" "$PKG/THIRD-PARTY-NOTICES.md" || {
    echo "FAIL: missing Python runtime notice mapping for $soname" >&2; exit 1;
  }
done
echo 'PASS: CPython embedded and dynamically linked library license notices are present'

"$PKG/oot-native-launcher" --help > /tmp/oot-native-smoke/state/launcher-help.txt
grep -q -- '--check' /tmp/oot-native-smoke/state/launcher-help.txt
echo 'PASS: packaged launcher --help'

"$PY" "$BUILDER/nxs2.py" doctor --release --json --out /doctor-space/build > /tmp/oot-native-smoke/state/doctor.json
"$PY" - <<'PY'
import json, pathlib
report=json.loads(pathlib.Path('/tmp/oot-native-smoke/state/doctor.json').read_text())
if not report.get('ok'):
    raise SystemExit('release doctor failed: '+json.dumps(report,ensure_ascii=False))
print('PASS: packaged release doctor: '+', '.join(x['name'] for x in report['checks']))
PY

"$PY" - <<'PY'
import importlib, pathlib, sys
root=pathlib.Path('/pkg/builder')
sys.path[:0]=[str(root),str(root/'tools'),str(root/'windows/src/nxshader')]
modules=(
    'nxs2','nxs2_linux','oot_native_i18n','nup_dump','third_party.nup','third_party.nup.format',
    'third_party.nup.read','tools.s2_pkg','tools.s2_ctl_scan','tools.s2_dumps',
    'tools.nso_tool','tools.make_ext_font','tools.epd_shaders','glslc2_extract',
    'tools.lower_corpus','nxshader','nxs_front','nxs_refl','nxs_spv',
)
for name in modules:
    importlib.import_module(name)
for name in ('lz4','zstandard','fontTools','PIL'):
    importlib.import_module(name)
print('PASS: imported packaged builder modules and runtime dependencies ('+str(len(modules)+4)+')')
import nxs2,nxs2_linux
nxs2_linux.preflight(nxs2)
print('PASS: package build preflight, including dmsh_xlate --source-sha and all packaged tools')
PY

expect_invalid_dump() {
  local label=$1 expected=$2 output rc
  shift 2
  set +e
  output=$("$@" 2>&1)
  rc=$?
  set -e
  if (( rc == 0 )) || ! grep -Fq "$expected" <<<"$output"; then
    printf 'FAIL: %s did not clearly reject the synthetic invalid .nup (rc=%s):\n%s\n' "$label" "$rc" "$output" >&2
    exit 1
  fi
  printf 'PASS: %s rejects a synthetic invalid .nup\n' "$label"
}

expect_invalid_dump '--lang en' 'Dump rejected: This file is not a valid .nup' \
  env -u OOT_NATIVE_LANG LANG=pt_BR.UTF-8 "$PKG/oot-native-launcher" --lang en --check /fixtures/invalid.nup
expect_invalid_dump '--lang pt-BR' 'Dump rejeitado: Este arquivo não é um dump .nup válido' \
  env -u OOT_NATIVE_LANG LANG=en_US.UTF-8 "$PKG/oot-native-launcher" --lang pt-BR --check /fixtures/invalid.nup
expect_invalid_dump 'OOT_NATIVE_LANG=en' 'Dump rejected: This file is not a valid .nup' \
  env OOT_NATIVE_LANG=en LANG=pt_BR.UTF-8 "$PKG/oot-native-launcher" --check /fixtures/invalid.nup
expect_invalid_dump 'OOT_NATIVE_LANG=pt-BR' 'Dump rejeitado: Este arquivo não é um dump .nup válido' \
  env OOT_NATIVE_LANG=pt-BR LANG=en_US.UTF-8 "$PKG/oot-native-launcher" --check /fixtures/invalid.nup

for lang in en pt-BR; do
  set +e
  OOT_NATIVE_LANG=$lang "$PY" "$BUILDER/nxs2.py" --game 0400486024DEC000 check-dump /fixtures/invalid.nup --json \
    > "/tmp/oot-native-smoke/state/check-dump-$lang.json"
  check_rc=$?
  set -e
  if (( check_rc != 1 )); then
    echo "FAIL: check-dump returned $check_rc for an invalid synthetic input in $lang; expected 1" >&2
    exit 1
  fi
done
"$PY" - <<'PY'
import json, pathlib
root=pathlib.Path('/tmp/oot-native-smoke/state')
reports=[json.loads((root/f'check-dump-{lang}.json').read_text()) for lang in ('en','pt-BR')]
codes=[r.get('code') for r in reports]
if codes != ['invalid_format','invalid_format']:
    raise SystemExit(f'check-dump JSON code changed by language: {codes}')
print('PASS: check-dump --json returns the same stable code in English and pt-BR')
PY

gui_title=$(env -u OOT_NATIVE_LANG -u LC_ALL -u LANGUAGE LANG=pt_BR.UTF-8 \
  HOME="$HOME/gui-default" XDG_CONFIG_HOME="$HOME/gui-config" \
  xvfb-run -a bash -c '
    "$1" >/tmp/oot-native-smoke/state/gui-default.log 2>&1 & app=$!
    trap "kill $app 2>/dev/null || true; wait $app 2>/dev/null || true" EXIT
    for _ in $(seq 1 60); do
      tree=$(xwininfo -root -tree 2>/dev/null || true)
      if grep -Fq "OoT Native for Linux" <<<"$tree"; then
        grep -F "OoT Native for Linux" <<<"$tree" | head -1
        exit 0
      fi
      sleep 0.25
    done
    cat /tmp/oot-native-smoke/state/gui-default.log >&2
    echo "GUI window did not open with its English title under pt_BR locale" >&2
    exit 1
  ' _ "$PKG/oot-native-launcher")
echo "PASS: launcher GUI defaults to English with LANG=pt_BR.UTF-8 ($gui_title)"

# Synthetic ZBIC file mode: exercise the same helper the release build invokes.
"$PY" - <<'PY'
import pathlib,struct,subprocess,sys,zstandard
sys.path.insert(0,'/pkg/builder/tools')
import nso_tool
segments=(b'synthetic text segment'*90,b'synthetic rodata segment'*70,b'synthetic data segment'*55)
buf=bytearray(0x100); buf[:4]=b'NSO0'; struct.pack_into('<I',buf,0x0c,0x87)
for i,raw in enumerate(segments):
    frame=zstandard.ZstdCompressor(write_content_size=True).compress(raw)
    stored=b'ZBIC'+frame[4:]
    struct.pack_into('<III',buf,0x10+0x10*i,len(buf),0x1000*i,len(raw))
    struct.pack_into('<I',buf,0x60+4*i,len(stored)); buf.extend(stored)
path=pathlib.Path('/tmp/oot-native-smoke/state/synthetic-main.nso'); path.write_bytes(buf)
r=subprocess.run(['/pkg/builder/bin/zbic-unpack',str(path)],capture_output=True,text=True)
if r.returncode: raise SystemExit('zbic-unpack file mode failed: '+r.stderr)
flags=struct.unpack_from('<I',path.read_bytes(),0x0c)[0]
if flags & 0x87: raise SystemExit(f'zbic-unpack left ZBIC flags set: {flags:#x}')
decoded,_,_=nso_tool.read_nso(path)
for name,expected in zip(('text','rodata','data'),segments):
    if decoded[name][1] != expected: raise SystemExit(f'synthetic {name} segment changed')
print('PASS: zbic-unpack normalizes a synthetic NSO in file mode and all segments round-trip')
PY

# manylinux wheels keep private dependencies beside extension modules in .libs
# directories and resolve them through $ORIGIN at import time. Give ldd the same
# packaged library locations so every ELF is checked against the bundle contents.
PACKAGE_LIBRARY_DIRS=$(
  while IFS= read -r -d '' elf; do
    magic=$(od -An -N4 -tx1 "$elf" | tr -d ' \n')
    [[ "$magic" == 7f454c46 ]] && dirname "$elf"
  done < <(find "$PKG" -type f -print0) | sort -u | paste -sd:
)
export LD_LIBRARY_PATH="/embedded-elf${PACKAGE_LIBRARY_DIRS:+:$PACKAGE_LIBRARY_DIRS}"
checked=0
while IFS= read -r -d '' elf; do
  magic=$(od -An -N4 -tx1 "$elf" | tr -d ' \n')
  [[ "$magic" == 7f454c46 ]] || continue
  output=$(ldd "$elf" 2>&1) || {
    if ! grep -q 'not a dynamic executable' <<<"$output"; then
      printf 'FAIL: ldd failed for %s:\n%s\n' "$elf" "$output" >&2; exit 1
    fi
  }
  if grep -q 'not found' <<<"$output"; then
    printf 'FAIL: unresolved ELF dependencies for %s:\n%s\n' "$elf" "$output" >&2; exit 1
  fi
  checked=$((checked+1))
done < <(find "$PKG" /embedded-elf -type f -print0)
if (( checked == 0 )); then echo 'FAIL: no ELF files checked' >&2; exit 1; fi
echo "PASS: ldd resolved all $checked package and PyInstaller-embedded ELF files"

# The release sets invariant globalization, so the packaged translator must
# still start even when the runtime image has no system ICU shared libraries.
find /lib /usr/lib \( -type f -o -type l \) -name 'libicu*.so*' -delete
if find /lib /usr/lib \( -type f -o -type l \) -name 'libicu*.so*' -print -quit | grep -q .; then
  echo 'FAIL: ICU libraries remain in the no-ICU smoke environment' >&2
  exit 1
fi
if command -v ldconfig >/dev/null 2>&1; then
  ldconfig
  if ldconfig -p | grep -q 'libicu'; then
    echo 'FAIL: ICU libraries remain in the no-ICU loader cache' >&2
    exit 1
  fi
fi
source_sha=$("$DOTNET_ROOT/dotnet" "$BUILDER/bin/dmsh_xlate.dll" --source-sha)
if [[ ! "$source_sha" =~ ^[0-9a-f]{16}$ ]]; then
  echo "FAIL: packaged translator did not start without system ICU (source SHA: $source_sha)" >&2
  exit 1
fi
echo "PASS: packaged dmsh_xlate starts without system ICU (source SHA $source_sha)"
echo 'PASS: complete packaged smoke finished'
