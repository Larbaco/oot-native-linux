#!/usr/bin/env bash
# Local end-to-end check with a real dump. NEVER run in CI; the dump never leaves this machine.
#
#   make e2e DUMP=<OoT .nup> [NEG_DUMP=<another game's .nup>] [BUILD_DIR=<dir>] [RUN=1]
#            [PKG_DIR=<extracted release package> | OUT=<pipeline work dir holding a promoted release>]
#
# 1. check-dump in English and pt-BR: DUMP must be accepted, NEG_DUMP rejected (the dump is only read).
# 2. build into BUILD_DIR (re-using an existing build folder), then compare the shader counts with e2e-reference.env.
# 3. only with RUN=1: run the game for 90 s, isolated (xvfb + lavapipe, input devices masked, temporary XDG_DATA_HOME).
# DRY_RUN=1 prints every command and runs nothing except the dump checks.
set -euo pipefail
export LC_ALL=C.UTF-8
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
DUMP=${DUMP:-}; NEG_DUMP=${NEG_DUMP:-}; BUILD_DIR=${BUILD_DIR:-}; RUN=${RUN:-0}; DRY_RUN=${DRY_RUN:-0}
PKG_DIR=${PKG_DIR:-}; OUT=${OUT:-${OOT_RELEASE_WORK:-}}
E2E_TMP=${E2E_TMP:-${OUT:+$OUT/e2e-tmp}}
RUN_SECONDS=${RUN_SECONDS:-90}
fail() { printf 'e2e: FAIL: %s\n' "$*" >&2; exit 1; }
say() { printf 'e2e: %s\n' "$*"; }
[[ -n "$DUMP" && -f "$DUMP" ]] || fail "DUMP=<path to your OoT .nup> is required"
[[ -z "$NEG_DUMP" || -f "$NEG_DUMP" ]] || fail "NEG_DUMP is not a file: $NEG_DUMP"
[[ -n "$E2E_TMP" ]] || fail "set OUT=<work dir> (or E2E_TMP) for temporary data; never use / or /tmp for it"
case "$E2E_TMP" in /|/tmp|/tmp/*) fail "refusing E2E_TMP=$E2E_TMP" ;; esac
[[ -z "${CI:-}" && -z "${GITHUB_ACTIONS:-}" ]] || fail "e2e needs a real dump and never runs in CI"
mkdir -p "$E2E_TMP"

if [[ -z "$PKG_DIR" ]]; then   # newest promoted release under OUT
  tarball=$(ls -1 "$OUT"/v*/oot-native-linux-x86_64-v*.tar.xz 2>/dev/null | grep -v -- '-source' | tail -1 || true)
  [[ -n "$tarball" ]] || fail "PKG_DIR=<extracted package> or a promoted release under OUT is required"
  PKG_DIR="$E2E_TMP/package"
  if [[ ! -d "$PKG_DIR/.from-$(basename "$tarball")" ]]; then
    rm -rf "$PKG_DIR"; mkdir -p "$PKG_DIR"
    tar -xf "$tarball" -C "$PKG_DIR" --strip-components=1 --no-same-owner
    mkdir "$PKG_DIR/.from-$(basename "$tarball")"
  fi
fi
LAUNCHER="$PKG_DIR/oot-native-launcher"
[[ -x "$LAUNCHER" ]] || fail "no launcher in $PKG_DIR"

fingerprint() { stat -c '%s %Y' "$1"; }
check_accepts() {
  local lang=$1 before after out
  before=$(fingerprint "$DUMP")
  out=$("$LAUNCHER" --lang "$lang" --check "$DUMP" 2>&1) || fail "OoT dump rejected in $lang: $out"
  after=$(fingerprint "$DUMP")
  [[ "$before" == "$after" ]] || fail "the launcher modified the dump"
  say "PASS: dump accepted ($lang): $(head -1 <<<"$out")"
}
check_rejects() {
  local lang=$1 out rc=0
  out=$("$LAUNCHER" --lang "$lang" --check "$NEG_DUMP" 2>&1) || rc=$?
  (( rc == 1 )) || fail "negative dump was not rejected in $lang (rc=$rc): $out"
  say "PASS: negative dump rejected ($lang): $(head -1 <<<"$out")"
}
for lang in en pt-BR; do
  check_accepts "$lang"
  [[ -z "$NEG_DUMP" ]] || check_rejects "$lang"
done

step() { if [[ "$DRY_RUN" == 1 ]]; then printf 'e2e: [dry-run] %s\n' "$*"; else "$@"; fi; }
[[ -n "$BUILD_DIR" ]] || { BUILD_DIR="$E2E_TMP/build"; say "BUILD_DIR not given; using $BUILD_DIR"; }
case "$BUILD_DIR" in /|/tmp|/tmp/*) fail "refusing BUILD_DIR=$BUILD_DIR" ;; esac

say "build: $LAUNCHER --lang en --build <dump> --out $BUILD_DIR   (re-uses the folder when it is already a launcher build)"
BUILD_LOG="$E2E_TMP/build-$(date -u +%Y%m%dT%H%M%SZ).log"
if [[ "$DRY_RUN" == 1 ]]; then step "$LAUNCHER" --lang en --build "$DUMP" --out "$BUILD_DIR"
else "$LAUNCHER" --lang en --build "$DUMP" --out "$BUILD_DIR" 2>&1 | tee "$BUILD_LOG"; fi

shader_report() {
  local log
  log=$(ls -1 "$BUILD_LOG" "$BUILD_DIR"/logs/*.log "$BUILD_DIR"/build*.log 2>/dev/null | head -50 | xargs grep -h -E 'shaders [0-9]+ .*ok [0-9]+, translation failed [0-9]+|spirv-val .*[0-9]+ pass, [0-9]+ fail' 2>/dev/null || true)
  [[ -n "$log" ]] || fail "no shader summary found under $BUILD_DIR (was the build completed?)"
  python3 -I - "$HERE/e2e-reference.env" <<PY
import re, sys
ref = dict(l.strip().split('=', 1) for l in open(sys.argv[1]) if l.strip() and not l.startswith('#'))
text = '''$log'''
m = re.search(r'shaders (\d+) .*?ok (\d+), translation failed (\d+)', text)
v = re.search(r'(\d+) pass, (\d+) fail', text)
if not m or not v: raise SystemExit('e2e: FAIL: cannot parse the shader summary')
got = {'E2E_SHADERS_TOTAL': m.group(1), 'E2E_SHADERS_OK': m.group(2), 'E2E_SHADERS_TRANSLATION_FAILED': m.group(3), 'E2E_SHADERS_SPIRV_FAILED': v.group(2)}
bad = {k: (got[k], ref[k]) for k in got if got[k] != ref[k]}
print('e2e: shader counts', got)
if bad: raise SystemExit(f'e2e: FAIL: shader counts differ from the reference (got, reference): {bad}')
print('e2e: PASS: shader counts match the reference')
PY
}
if [[ "$DRY_RUN" == 1 ]]; then say "[dry-run] shader counts would be compared with $HERE/e2e-reference.env"; else shader_report; fi

if [[ "$RUN" != 1 ]]; then say "RUN=1 not set: the game is not started"; exit 0; fi

version=$(cat "$BUILD_DIR/version.txt" 2>/dev/null || echo 2.0.0)
game="$BUILD_DIR/out/play-oot-$version"
xdg="$E2E_TMP/run-xdg-$$"
runlog="$E2E_TMP/run-$(date -u +%Y%m%dT%H%M%SZ).log"
lvp=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)
mask_args=(--dev-bind / / --tmpfs /dev/input)
cmd=(flock -o /tmp/oot-gpu.lock
     env HEADLESS=1 OOT_NATIVE_HOME="$PKG_DIR" XDG_DATA_HOME="$xdg" VK_DRIVER_FILES="$lvp" VK_ICD_FILENAMES="$lvp" SDL_AUDIODRIVER=dummy
     bwrap "${mask_args[@]}" xvfb-run -a timeout "$RUN_SECONDS" "$game" --silent)
say "isolated ${RUN_SECONDS}s run: ${cmd[*]}"
if [[ "$DRY_RUN" == 1 ]]; then say "[dry-run] not started"; exit 0; fi
mkdir -p "$xdg"
rc=0; "${cmd[@]}" > "$runlog" 2>&1 || rc=$?
(( rc == 124 )) || fail "the run ended with rc=$rc instead of the expected timeout 124 (log $runlog)"
grep -q 'FATAL' "$runlog" && fail "FATAL in the run log $runlog"
say "PASS: ran ${RUN_SECONDS}s and was stopped by the timeout (log $runlog); temporary XDG_DATA_HOME $xdg"
