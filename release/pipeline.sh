#!/usr/bin/env bash
# Deterministic local release pipeline: no LLM, no prompts, fail closed.
#
#   release/pipeline.sh release        VERSION=x.y.z OUT=<work dir> PREV=<previous package .tar.xz | none>
#   release/pipeline.sh verify-repro   VERSION=x.y.z OUT=<work dir>
#   release/pipeline.sh verify-staging VERSION=x.y.z OUT=<work dir> STAGING=<package dir> [FROM=assemble|smoke]
#   release/pipeline.sh image-fp                     (print the image fingerprints to pin in pins.env)
#
# `make release` / `make verify-repro` call this. See release/PIPELINE.md.
# Every gate prints PASS or FAIL, writes <OUT>/work/v<VERSION>/logs/<gate>.log and aborts on the first FAIL.
# Final names are created only after every gate passed.
set -euo pipefail
export LC_ALL=C.UTF-8 TZ=UTC
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/.." && pwd)
CMD=${1:-release}

VERSION=${VERSION:-}
OUT=${OUT:-${OOT_RELEASE_WORK:-}}
PREV=${PREV:-}
PREV_ABI=${PREV_ABI:-}   # optional committed ABI baseline (release/abi-baseline-*.json) used instead of reading PREV
# Default: the one rule the builder and launcher share (90% of the usable threads); override with JOBS=N.
JOBS=${JOBS:-$(python3 -I -c "import sys; sys.path.insert(0, sys.argv[1]); import oot_native_jobs; print(oot_native_jobs.default_jobs())" "$ROOT")}
ALLOW_ABI_BREAK=${ALLOW_ABI_BREAK:-0}
MIN_FREE_GB=${MIN_FREE_GB:-25}
KEEP_WORK=${KEEP_WORK:-0}
STAGING=${STAGING:-}
FROM=${FROM:-assemble}

die() { printf 'pipeline: %s\n' "$*" >&2; exit 2; }
say() { printf 'pipeline: %s\n' "$*"; }

# ---------------------------------------------------------------- gate runner
LOGS=""; GATES=""
gate() {   # gate <name> <command...>: run in a subshell, log, record, abort on failure
  local name=$1; shift
  local log="$LOGS/$name.log" start=$SECONDS rc=0
  printf '[ .. ] %s\n' "$name"
  # The subshell must not sit in a conditional context (`|| ...`), or bash silently disables errexit inside it.
  set +e
  ( set -euo pipefail; "$@" ) > "$log" 2>&1
  rc=$?
  set -e
  local dur=$((SECONDS - start)) sum free_root free_out
  free_root=$(( $(df -B1 --output=avail / | tail -1) / 1073741824 )); free_out=$(( $(df -B1 --output=avail "$OUT" | tail -1) / 1073741824 ))
  sum=$(sha256sum "$log" | cut -d' ' -f1)
  if (( rc == 0 )); then
    printf '[PASS] %s (%ss)\n' "$name" "$dur"
    printf '{"name":"%s","status":"PASS","seconds":%d,"log":"%s.log","log_sha256":"%s","free_root_gb":%d,"free_out_gb":%d}\n' "$name" "$dur" "$name" "$sum" "$free_root" "$free_out" >> "$GATES"
  else
    printf '[FAIL] %s (rc=%s, %ss) -- %s\n' "$name" "$rc" "$dur" "$log" >&2
    tail -n 25 "$log" >&2 || true
    printf '{"name":"%s","status":"FAIL","seconds":%d,"rc":%d,"log":"%s.log","log_sha256":"%s"}\n' "$name" "$dur" "$rc" "$name" "$sum" >> "$GATES"
    printf 'pipeline: FAILED at gate %s; nothing was promoted\n' "$name" >&2
    exit 1
  fi
}

# ---------------------------------------------------------------- setup helpers
need_tools() {
  local t
  for t in docker git python3 xz tar sha256sum nm curl nice ionice df find sort install; do
    command -v "$t" >/dev/null || { echo "missing required tool: $t"; return 1; }
  done
  docker info >/dev/null 2>&1 || { echo "the docker daemon is not reachable"; return 1; }
}

init_paths() {   # init_paths <run name>
  [[ -n "$OUT" ]] || die "OUT=<work directory on a large disk> is required (or OOT_RELEASE_WORK)"
  mkdir -p "$OUT"
  OUT=$(realpath -m "$OUT")
  case "$OUT" in /|/tmp|/tmp/*|"$ROOT"|"$ROOT"/*) die "refusing an unsafe OUT: $OUT" ;; esac
  [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || die "VERSION must look like 1.2.3 or 1.2.3-rc1 (got '$VERSION')"
  [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a whole number of 1 or more"
  CACHE="$OUT/cache"; DL="$CACHE/downloads"
  RUNNAME=$1
  RUN="$OUT/work/$RUNNAME"
  SRC="$RUN/src"; RO="$RUN/out"; LOGS="$RUN/logs"; W="$RUN"
  GATES="$LOGS/gates.jsonl"
}

load_pins() { set -a; . "$HERE/pins.env"; set +a; }

record_context() {   # facts that identify this release; also feeds the manifest
  COMMIT=$(git -C "$ROOT" rev-parse HEAD)
  BRANCH=$(git -C "$ROOT" branch --show-current)
  SOURCE_DATE_EPOCH=$(git -C "$ROOT" show -s --format=%ct HEAD)
  RELEASE_DATE=$(date -u -d "@$SOURCE_DATE_EPOCH" +%F)
  SOURCE_DATE_UTC=$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)
  PKG_NAME="oot-native-linux-x86_64-v$VERSION-$RELEASE_DATE"
  export SOURCE_DATE_EPOCH
  PKG="$RO/$PKG_NAME"
  derive_paths
}
derive_paths() {   # everything below must be computable from W/PKG_NAME alone: gates run in subshells and cannot hand variables back
  SOURCE_STAGE="$W/source-stage/$PKG_NAME-source"
  SOURCE_ALLOWLIST="$W/source-allowlist.txt"
  SOURCE_TARBALL="$W/$PKG_NAME-source.tar.xz"
  THIRDPARTY_STAGE="$W/thirdparty-stage/$PKG_NAME-thirdparty-source"
  THIRDPARTY_TARBALL="$W/$PKG_NAME-thirdparty-source.tar.xz"
  PACKAGE_TARBALL="$W/$PKG_NAME.tar.xz"
  SMOKE="$W/smoke"
}

# ---------------------------------------------------------------- stage a: preconditions
pre_clean_tree() {
  if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=all)" ]]; then
    git -C "$ROOT" status --short
    echo "the git tree is not clean; commit or remove the changes above"
    return 1
  fi
  echo "tree clean at $COMMIT ($BRANCH)"
  echo "version=$VERSION source_date_epoch=$SOURCE_DATE_EPOCH ($SOURCE_DATE_UTC) release_date=$RELEASE_DATE"
}
pre_space() {
  local avail_gb
  avail_gb=$(( $(df -B1 --output=avail "$OUT" | tail -1) / 1073741824 ))
  echo "free space under $OUT: ${avail_gb} GB (minimum ${MIN_FREE_GB} GB)"
  (( avail_gb >= MIN_FREE_GB )) || { echo "not enough free space"; return 1; }
}
pre_names() {
  [[ ! -e "$FINAL" || -z "$(ls -A "$FINAL" 2>/dev/null)" ]] || { echo "final directory $FINAL already exists and is not empty; move it away first"; return 1; }
  case "$PREV" in
    '') echo "PREV=<previous release package .tar.xz> is required (or PREV=none with ALLOW_ABI_BREAK=1 for a first release)"; return 1 ;;
    none) [[ "$ALLOW_ABI_BREAK" == 1 ]] || { echo "PREV=none needs ALLOW_ABI_BREAK=1: there is no ABI baseline"; return 1; } ;;
    *) [[ -f "$PREV" ]] || { echo "PREV is not a file: $PREV"; return 1; } ;;
  esac
  [[ -z "$PREV_ABI" || -f "$PREV_ABI" ]] || { echo "PREV_ABI is not a file: $PREV_ABI"; return 1; }
}

# ---------------------------------------------------------------- stage c: pinned inputs
pins_downloads() { python3 -I "$HERE/release_pins.py" fetch --cache "$DL"; }
pins_nxnative() {
  if [[ ! -d "$CACHE/nxnative/.git" ]]; then
    nice -n 19 ionice -c3 git clone --quiet "$NXNATIVE_URL" "$CACHE/nxnative"
  fi
  if ! git -C "$CACHE/nxnative" cat-file -e "$NXNATIVE_COMMIT^{commit}" 2>/dev/null; then
    git -C "$CACHE/nxnative" fetch --quiet origin "$NXNATIVE_COMMIT"
  fi
  git -C "$CACHE/nxnative" checkout --quiet --detach --force "$NXNATIVE_COMMIT"
  git -C "$CACHE/nxnative" clean --quiet -fdx
  local head; head=$(git -C "$CACHE/nxnative" rev-parse HEAD)
  [[ "$head" == "$NXNATIVE_COMMIT" ]] || { echo "nxnative is at $head, pinned $NXNATIVE_COMMIT"; return 1; }
  [[ -z "$(git -C "$CACHE/nxnative" status --porcelain)" ]] || { echo "nxnative cache is dirty"; return 1; }
  echo "nxnative at pinned $head"
}

build_image() {   # build_image <kind: builder|smoke> -> sets IMAGE_ID, IMAGE_FP
  local kind=$1 dockerfile ctx tag key
  if [[ $kind == builder ]]; then dockerfile=Dockerfile.ubuntu22.04; else dockerfile=Dockerfile.smoke.ubuntu22.04; fi
  key=$(cat "$HERE/$dockerfile" "$HERE/requirements-build.lock" | { cat; echo "$UBUNTU_BASE_DIGEST $UBUNTU_SNAPSHOT $DL_DOTNET_SDK_SHA256"; } | sha256sum | cut -c1-12)
  tag="oot-release-$kind:$key"
  if ! docker image inspect "$tag" >/dev/null 2>&1; then
    ctx="$OUT/work/docker-ctx-$kind"
    rm -rf "$ctx"; mkdir -p "$ctx"
    cp "$HERE/$dockerfile" "$ctx/Dockerfile"
    cp "$HERE/requirements-build.lock" "$ctx/"
    if [[ $kind == builder ]]; then ln "$DL/$DL_DOTNET_SDK_FILE" "$ctx/$DL_DOTNET_SDK_FILE" 2>/dev/null || cp "$DL/$DL_DOTNET_SDK_FILE" "$ctx/"; fi
    nice -n 19 ionice -c3 docker build -t "$tag" \
      --build-arg "UBUNTU_BASE_DIGEST=$UBUNTU_BASE_DIGEST" --build-arg "UBUNTU_SNAPSHOT=$UBUNTU_SNAPSHOT" \
      --build-arg "DOTNET_SDK_FILE=$DL_DOTNET_SDK_FILE" --build-arg "DOTNET_SDK_SHA256=$DL_DOTNET_SDK_SHA256" "$ctx"
    rm -rf "$ctx"
  fi
  IMAGE_ID=$(docker image inspect --format '{{.Id}}' "$tag")
  IMAGE_FP=$(docker run --rm "$IMAGE_ID" sha256sum /etc/release-image-contents.txt | cut -d' ' -f1)
  echo "$kind image $tag id=$IMAGE_ID contents-fingerprint=$IMAGE_FP"
}
images_gate() {
  build_image builder; BUILDER_ID=$IMAGE_ID; BUILDER_FP_NOW=$IMAGE_FP
  build_image smoke; SMOKE_ID=$IMAGE_ID; SMOKE_FP_NOW=$IMAGE_FP
  printf '%s\n' "$BUILDER_ID" > "$RUN/builder-image.id"; printf '%s\n' "$SMOKE_ID" > "$RUN/smoke-image.id"
  printf '%s\n' "$BUILDER_FP_NOW" > "$RUN/builder-image.fp"; printf '%s\n' "$SMOKE_FP_NOW" > "$RUN/smoke-image.fp"
  [[ -n "$BUILDER_FP" && -n "$SMOKE_FP" ]] || { echo "BUILDER_FP/SMOKE_FP are not pinned in release/pins.env"; return 1; }
  [[ "$BUILDER_FP_NOW" == "$BUILDER_FP" ]] || { echo "builder image contents differ from the pin ($BUILDER_FP)"; return 1; }
  [[ "$SMOKE_FP_NOW" == "$SMOKE_FP" ]] || { echo "smoke image contents differ from the pin ($SMOKE_FP)"; return 1; }
  echo "both images match their pinned contents"
}

export_source() {
  rm -rf "$SRC"; mkdir -p "$SRC"
  git -C "$ROOT" archive --format=tar HEAD | tar -xf - -C "$SRC" --no-same-owner
  nice -n 19 ionice -c3 git clone --quiet --no-hardlinks --no-checkout "$CACHE/nxnative" "$SRC/nxnative"
  git -C "$SRC/nxnative" checkout --quiet --detach "$NXNATIVE_COMMIT"
  [[ "$(git -C "$SRC/nxnative" rev-parse HEAD)" == "$NXNATIVE_COMMIT" ]] || { echo "exported nxnative is not at the pin"; return 1; }
  echo "exported $COMMIT and nxnative $NXNATIVE_COMMIT"
}

# ---------------------------------------------------------------- containers
drun() {   # drun <phase>
  nice -n 19 ionice -c3 docker run --rm --cpus="$JOBS" \
    -e NXS2_JOBS="$JOBS" -e NXS2_RELEASE_PKG_NAME="$PKG_NAME" -e NXS2_RELEASE_VERSION="$VERSION" -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
    -e HOME=/out/home -e NUGET_PACKAGES=/out/nuget -e TMPDIR=/tmp \
    -v "$SRC:/src" -v "$RO:/out" -v "$CACHE:/cache" -v "$RO/container-tmp:/tmp" "$BUILDER_ID" \
    /bin/bash /src/release/build-in-container.sh "$1"
}
phase_gate() { drun "$1"; }

tests_python_gate() {
  drun tests-python
  grep -E '^Ran [0-9]+ tests' "$RO/python-tests.log" "$RO/tool-tests.log"
  grep -Eq '^OK' "$RO/python-tests.log" && grep -Eq '^OK' "$RO/tool-tests.log"
}

# ---------------------------------------------------------------- stage d: assembly
assemble_source() {
  rm -rf "$W/source-stage"; mkdir -p "$W/source-stage"
  cp "$SRC/release/allowlist.txt" "$SOURCE_ALLOWLIST"
  local source mode rel
  while IFS= read -r source; do
    [[ -z "$source" || "$source" == \#* ]] && continue
    [[ -f "$SRC/$source" ]] || { echo "allowlisted source missing: $source"; return 1; }
    mode=$(stat -c %a "$SRC/$source")
    install -D -m "$mode" "$SRC/$source" "$SOURCE_STAGE/$source"
  done < "$SRC/release/allowlist.txt"
  while IFS= read -r source; do
    [[ -z "$source" ]] && continue
    rel=${source#upstream/}
    [[ -f "$RO/artifacts/upstream-source/$rel" ]] || { echo "translator source missing: $source"; return 1; }
    install -D -m 0644 "$RO/artifacts/upstream-source/$rel" "$SOURCE_STAGE/$source"
    printf '%s\n' "$source" >> "$SOURCE_ALLOWLIST"
  done < "$RO/artifacts/upstream-source/manifest.txt"
  [[ ! -e "$SOURCE_STAGE/.git" ]] || { echo "source staging contains git history"; return 1; }
  python3 -I "$SRC/release/render_docs.py" render-tree "$SOURCE_STAGE" --version "$VERSION" --pkg-name "$PKG_NAME"
  echo "source staging: $(find "$SOURCE_STAGE" -type f | wc -l) files"
}
docs_check_source() { python3 -I "$SRC/release/render_docs.py" check "$SOURCE_STAGE" --version "$VERSION" --pkg-name "$PKG_NAME" --repo-root "$SRC" --allow-template release/README-RELEASE.md; }
docs_check_package() { python3 -I "$SRC/release/render_docs.py" check "$PKG" --version "$VERSION" --pkg-name "$PKG_NAME"; }
release_check_source_dir() { python3 -I "$SRC/release/release_check.py" "$SOURCE_STAGE" --allowlist "$SOURCE_ALLOWLIST" --mode source; }
make_tar() {   # make_tar <tarball> <parent dir> <member>
  nice -n 19 ionice -c3 tar --owner=0 --group=0 --numeric-owner --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
    --format=gnu -I 'xz -T8 -6' -cf "$1" -C "$2" "$3"
}
source_tarball() {
  rm -f "$SOURCE_TARBALL"
  make_tar "$SOURCE_TARBALL" "$W/source-stage" "$PKG_NAME-source"
  python3 -I "$SRC/release/release_check.py" "$SOURCE_TARBALL" --allowlist "$SOURCE_ALLOWLIST" --mode source
}
# The exact FFmpeg/dav1d source tarballs behind builder/lib/libav*, libdav1d (LGPL-2.1 / BSD-2-Clause source offer).
thirdparty_source() {
  [[ -n "$W" && -n "$THIRDPARTY_STAGE" ]] || { echo "paths are not initialised"; return 1; }
  rm -rf "$W/thirdparty-stage" "$THIRDPARTY_TARBALL"; mkdir -p "$THIRDPARTY_STAGE"
  cp "$DL/$DL_FFMPEG_FILE" "$DL/$DL_DAV1D_FILE" "$THIRDPARTY_STAGE/"
  ( cd "$THIRDPARTY_STAGE" && echo "$DL_FFMPEG_SHA256  $DL_FFMPEG_FILE" | sha256sum -c - && echo "$DL_DAV1D_SHA256  $DL_DAV1D_FILE" | sha256sum -c - )
  cat > "$THIRDPARTY_STAGE/README.txt" <<README
OoT Native v$VERSION third-party codec corresponding source
FFmpeg 7.1.1: LGPL-2.1, SHA-256 $DL_FFMPEG_SHA256
dav1d 1.5.1: BSD-2-Clause, SHA-256 $DL_DAV1D_SHA256
These are the exact source tarballs used to build the packaged codec libraries.
README
  chmod 0644 "$THIRDPARTY_STAGE/README.txt" "$THIRDPARTY_STAGE/$DL_FFMPEG_FILE" "$THIRDPARTY_STAGE/$DL_DAV1D_FILE"
  printf '%s\n' README.txt "$DL_FFMPEG_FILE" "$DL_DAV1D_FILE" > "$W/thirdparty-allowlist.txt"
  make_tar "$THIRDPARTY_TARBALL" "$W/thirdparty-stage" "$PKG_NAME-thirdparty-source"
  python3 -I "$SRC/release/release_check.py" "$THIRDPARTY_TARBALL" --allowlist "$W/thirdparty-allowlist.txt" --mode source
  # the archive must hold exactly the pinned bytes
  rm -rf "$W/thirdparty-verify"; mkdir -p "$W/thirdparty-verify"
  tar -xf "$THIRDPARTY_TARBALL" -C "$W/thirdparty-verify" --no-same-owner
  ( cd "$W/thirdparty-verify/$PKG_NAME-thirdparty-source" && echo "$DL_FFMPEG_SHA256  $DL_FFMPEG_FILE" | sha256sum -c - && echo "$DL_DAV1D_SHA256  $DL_DAV1D_FILE" | sha256sum -c - )
  rm -rf "$W/thirdparty-verify"
}
package_manifest() {
  local required=(oot-native-launcher builder/bin/nxrt builder/bin/nxrt-movie-decode builder/bin/nxanalyze builder/bin/nxlift builder/bin/zbic-unpack builder/ampere-lower/ampere-lower builder/bin/dmsh_xlate.dll builder/lib/libSDL3.so.0 builder/lib/libavcodec.so.61 builder/lib/libdav1d.so.7 THIRD-PARTY-SOURCE.md)
  local f
  for f in "${required[@]}"; do [[ -f "$PKG/$f" ]] || { echo "staging lacks $f"; return 1; }; done
  [[ -x "$PKG/oot-native-launcher" && -x "$PKG/builder/bin/nxrt" ]] || { echo "launcher or nxrt is not executable"; return 1; }
  rm -f "$PKG"/*-source.tar.xz "$PKG/PACKAGE-MANIFEST.txt" "$PKG/SHA256SUMS"
  install -m 0644 "$SOURCE_TARBALL" "$PKG/$PKG_NAME-source.tar.xz"
  install -m 0644 "$THIRDPARTY_TARBALL" "$PKG/$PKG_NAME-thirdparty-source.tar.xz"
  python3 -I - "$PKG" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
entries = sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() or p.is_symlink())
(root / 'PACKAGE-MANIFEST.txt').write_text('\n'.join(entries + ['PACKAGE-MANIFEST.txt', 'SHA256SUMS']) + '\n', encoding='utf-8')
PY
  ( cd "$PKG" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS )
  # The v0.1.1 package once shipped absolute paths in SHA256SUMS: every entry must be ./relative and verify from inside the package.
  if awk '{print $2}' "$PKG/SHA256SUMS" | grep -v '^\./' | grep -q .; then echo "SHA256SUMS contains a non-relative path"; return 1; fi
  if grep -q -F -e "$RO" -e "$W" "$PKG/SHA256SUMS"; then echo "SHA256SUMS leaks a build path"; return 1; fi
  ( cd "$PKG" && sha256sum --quiet -c SHA256SUMS )
  cp "$PKG/PACKAGE-MANIFEST.txt" "$W/package-allowlist.txt"
  echo "SHA256SUMS: $(wc -l < "$PKG/SHA256SUMS") relative entries, self-verifying"
}
release_check_package_dir() { python3 -I "$SRC/release/release_check.py" "$PKG" --allowlist "$W/package-allowlist.txt" --mode package; }
negative_forbidden() {   # the scanner must reject a synthetic game-file fixture
  rm -rf "$W/negative-fixture"; cp -al "$PKG" "$W/negative-fixture"
  printf 'NSO0 synthetic negative fixture\n' > "$W/negative-fixture/forbidden.nso"
  if python3 -I "$SRC/release/release_check.py" "$W/negative-fixture" --allowlist "$W/package-allowlist.txt" --mode package; then
    echo "release_check accepted a forbidden fixture"; rm -rf "$W/negative-fixture"; return 1
  fi
  rm -rf "$W/negative-fixture"
  echo "PASS: forbidden synthetic fixture rejected"
}
package_tarball() {
  rm -f "$PACKAGE_TARBALL"
  make_tar "$PACKAGE_TARBALL" "$(dirname "$PKG")" "$PKG_NAME"
  python3 -I "$SRC/release/release_check.py" "$PACKAGE_TARBALL" --allowlist "$W/package-allowlist.txt" --mode package
}

# ---------------------------------------------------------------- stage d (smoke) and stage e (ABI)
smoke_prepare() {
  rm -rf "$SMOKE"; mkdir -p "$SMOKE/extracted" "$SMOKE/embedded-elf" "$SMOKE/fixtures" "$SMOKE/state" "$SMOKE/reference"
  tar -xf "$PACKAGE_TARBALL" -C "$SMOKE/extracted" --no-same-owner
  [[ -d "$SMOKE/extracted/$PKG_NAME" ]]
  printf 'synthetic invalid input, not a NUP file\n' > "$SMOKE/fixtures/invalid.nup"
  if [[ "$PREV" != none ]]; then
    tar -xf "$PREV" -C "$SMOKE/reference" --no-same-owner --wildcards '*/builder/bin/nxrt' '*/builder/lib/*'
    [[ -x "$(find "$SMOKE/reference" -mindepth 1 -maxdepth 1 -type d | head -1)/builder/bin/nxrt" ]] || { echo "previous release package has no builder/bin/nxrt"; return 1; }
  fi
}
smoke_extract_elf() {
  # Reads the PyInstaller archive with the same Python/PyInstaller that built it (inside the builder image).
  docker run --rm -v "$RO:/out:ro" -v "$SMOKE:/smoke" -v "$SRC/release:/release-src:ro" "$BUILDER_ID" \
    env PYTHONPATH=/out/launcher-venv/lib/python3.12/site-packages PYTHONHOME="/smoke/extracted/$PKG_NAME/python" \
    "/smoke/extracted/$PKG_NAME/python/bin/python3" /release-src/extract_launcher_elf.py /smoke/embedded-elf \
    "/smoke/extracted/$PKG_NAME/oot-native-launcher" "/smoke/extracted/$PKG_NAME"
}
smoke_run() {
  local ref_args=() REFERENCE_DIR
  REFERENCE_DIR=$(find "$SMOKE/reference" -mindepth 1 -maxdepth 1 -type d | head -1)
  if [[ "$PREV" != none ]]; then ref_args=(-e OOT_REFERENCE_PACKAGE=/reference-package -v "$REFERENCE_DIR:/reference-package:ro"); fi
  nice -n 19 ionice -c3 docker run --rm --network none --cpus=2 --tmpfs /doctor-space:rw,size=50g \
    -e OOT_RELEASE_LABEL=candidate "${ref_args[@]}" \
    -v "$SMOKE/extracted/$PKG_NAME:/pkg:ro" \
    -v "$SMOKE/embedded-elf:/embedded-elf:ro" \
    -v "$SMOKE/fixtures:/fixtures:ro" \
    -v "$SMOKE/state:/tmp/oot-native-smoke" \
    -v "$SRC/release:/release-src:ro" \
    -v "$SRC/release/package-smoke.sh:/smoke.sh:ro" \
    "$SMOKE_ID" /bin/bash /smoke.sh
}
smoke_evidence() {
  local noref=()
  [[ "$PREV" == none ]] && noref=(--allow-no-reference)
  python3 -I "$HERE/release_manifest.py" check-smoke "$LOGS/smoke-run.log" --out "$W/smoke-gates.json" "${noref[@]}"
  python3 -I -c "import json;[print(g['status'],g['id']) for g in json.load(open('$W/smoke-gates.json'))]"
}
ctest_evidence() {
  python3 -I "$HERE/release_manifest.py" check-ctest "$RO/ctest.log" --out "$W/ctest.json"
  cat "$W/ctest.json"
}
abi_gate() {
  local nxrt="$SMOKE/extracted/$PKG_NAME/builder/bin/nxrt"
  python3 -I "$HERE/release_abi.py" describe --nxrt "$nxrt" --abi-info "$SMOKE/state/state/abi-info.txt" \
    --source "$SOURCE_TARBALL" --out "$W/release-abi.json"
  if [[ "$PREV" == none ]]; then
    python3 -I - "$W/release-abi.json" "$W/abi-result.json" "$ALLOW_ABI_BREAK" <<'PY'
import json, sys
new = json.load(open(sys.argv[1]))
json.dump({'requires_rebuild': True, 'reasons': ['no previous release: no ABI baseline'], 'previous': None,
           'new': {'origin': new['origin'], 'abi_version': new['abi_version'], 'registry_sha256': new['registry']['sha256'],
                   'exports_sha256': new['exports']['sha256'], 'registry_count': new['registry']['count'], 'exports_count': new['exports']['count']},
           'exports_added': [], 'exports_removed': [], 'registry_added': [], 'registry_removed': [], 'registry_changed': [],
           'abi_break_allowed': sys.argv[3] == '1'}, open(sys.argv[2], 'w'), indent=1)
PY
    echo "requires_rebuild=true (no baseline)"
    return 0
  fi
  local allow=(); [[ "$ALLOW_ABI_BREAK" == 1 ]] && allow=(--allow-break)
  local baseline="$PREV"
  if [[ -n "$PREV_ABI" ]]; then
    # The committed baseline is the comparison reference; the previous package, which we also have, must agree with it.
    python3 -I "$HERE/release_abi.py" baseline --package "$PREV" --out "$W/abi-prev-from-package.json"
    python3 -I "$HERE/release_abi.py" compare --prev "$PREV_ABI" --new "$W/abi-prev-from-package.json" --out "$W/abi-baseline-crosscheck.json" \
      || { echo "the committed baseline $PREV_ABI disagrees with the previous package"; return 1; }
    echo "committed baseline agrees with the previous package"
    baseline="$PREV_ABI"
  fi
  python3 -I "$HERE/release_abi.py" compare --prev "$baseline" --new "$W/release-abi.json" --out "$W/abi-result.json" "${allow[@]}"
}

# ---------------------------------------------------------------- manifest and promotion
write_context() {
  python3 -I - "$W/context.json" <<PY
import json, os, sys
e = os.environ
pins = {k: v for k, v in e.items() if k.startswith(('DL_',)) and k.endswith('_SHA256')}
ctx = {
  'version': '$VERSION', 'commit': '$COMMIT', 'branch': '$BRANCH', 'source_date_epoch': int('$SOURCE_DATE_EPOCH'),
  'source_date_utc': '$SOURCE_DATE_UTC', 'release_date': '$RELEASE_DATE',
  'builder_image': {'id': '$BUILDER_ID', 'contents_fingerprint': '$BUILDER_FP_NOW', 'base_digest': e['UBUNTU_BASE_DIGEST'], 'apt_snapshot': e['UBUNTU_SNAPSHOT']},
  'smoke_image': {'id': '$SMOKE_ID', 'contents_fingerprint': '$SMOKE_FP_NOW', 'base_digest': e['UBUNTU_BASE_DIGEST'], 'apt_snapshot': e['UBUNTU_SNAPSHOT']},
  'pinned_inputs': {'nxnative_commit': e['NXNATIVE_COMMIT'], 'downloads_sha256': dict(sorted(pins.items())),
                    'pins_env_sha256': '$(sha256sum "$HERE/pins.env" | cut -d' ' -f1)'},
  'abi_baseline': '$([[ "$PREV" == none ]] && echo none || basename "${PREV_ABI:-$PREV}")',
}
json.dump(ctx, open(sys.argv[1], 'w'), indent=1, sort_keys=True)
PY
}
manifest_gate() {
  python3 -I "$HERE/release_manifest.py" manifest --context "$W/context.json" --gates "$GATES" \
    --smoke-gates "$W/smoke-gates.json" --ctest "$W/ctest.json" --abi-result "$W/abi-result.json" \
    --abi-desc "$W/release-abi.json" --abi-file "$W/release-abi.json" --package "$PACKAGE_TARBALL" \
    --source "$SOURCE_TARBALL" --thirdparty "$THIRDPARTY_TARBALL" --staging "$PKG" --out "$W/release-manifest.json"
  python3 -I -m json.tool "$W/release-manifest.json" >/dev/null
}
promote() {
  [[ ! -e "$FINAL" || -z "$(ls -A "$FINAL")" ]] || { echo "$FINAL appeared while the pipeline ran"; return 1; }
  local tmp="$OUT/.promote-v$VERSION-$$"
  rm -rf "$tmp"; mkdir -p "$tmp"
  cp "$PACKAGE_TARBALL" "$SOURCE_TARBALL" "$THIRDPARTY_TARBALL" "$tmp/"
  cp "$W/release-abi.json" "$tmp/$PKG_NAME.release-abi.json"
  ( cd "$tmp" && sha256sum "$PKG_NAME.tar.xz" > "$PKG_NAME.tar.xz.sha256" && sha256sum "$PKG_NAME-source.tar.xz" > "$PKG_NAME-source.tar.xz.sha256" \
      && sha256sum "$PKG_NAME-thirdparty-source.tar.xz" > "$PKG_NAME-thirdparty-source.tar.xz.sha256" \
      && sha256sum --quiet -c "$PKG_NAME.tar.xz.sha256" "$PKG_NAME-source.tar.xz.sha256" "$PKG_NAME-thirdparty-source.tar.xz.sha256" )
  cmp -s "$PACKAGE_TARBALL" "$tmp/$PKG_NAME.tar.xz"
  # the manifest names the ABI sidecar by its final name
  sed -i "s#\"file\": \"release-abi.json\"#\"file\": \"$PKG_NAME.release-abi.json\"#" "$W/release-manifest.json"
  cp "$W/release-manifest.json" "$tmp/release-manifest.json"
  mkdir -p "$FINAL"
  mv "$tmp"/* "$FINAL/"
  rmdir "$tmp"
  echo "promoted to $FINAL"
}

# ---------------------------------------------------------------- command: release
all_assembly_gates() {   # shared by release, verify-staging --from assemble
  gate source-stage assemble_source
  gate docs-version-source docs_check_source
  gate release-check-source-dir release_check_source_dir
  gate source-tarball source_tarball
  gate thirdparty-source thirdparty_source
  gate docs-version-package docs_check_package
  gate package-manifest package_manifest
  gate release-check-package-dir release_check_package_dir
  gate negative-forbidden-fixture negative_forbidden
  gate package-tarball package_tarball
}
smoke_gates_all() {
  gate smoke-prepare smoke_prepare
  gate smoke-extract-elf smoke_extract_elf
  gate smoke-run smoke_run
  gate smoke-evidence smoke_evidence
}

cmd_release() {
  init_paths "v$VERSION"
  FINAL="$OUT/v$VERSION"
  rm -rf "$RUN"; mkdir -p "$LOGS"; : > "$GATES"
  gate need-tools need_tools
  load_pins
  record_context
  gate pre-clean-tree pre_clean_tree
  gate pre-free-space pre_space
  gate pre-final-names pre_names
  gate pins-downloads pins_downloads
  gate pins-nxnative pins_nxnative
  gate images images_gate
  BUILDER_ID=$(<"$RUN/builder-image.id"); SMOKE_ID=$(<"$RUN/smoke-image.id")
  BUILDER_FP_NOW=$(<"$RUN/builder-image.fp"); SMOKE_FP_NOW=$(<"$RUN/smoke-image.fp")
  mkdir -p "$RO/home" "$RO/nuget" "$RO/container-tmp"; chmod 1777 "$RO/container-tmp"
  gate export-source export_source
  gate container-prep phase_gate prep
  gate tests-python-tools tests_python_gate
  gate container-build phase_gate build
  gate ctest phase_gate ctest
  gate tests-native phase_gate tests-native
  gate ctest-evidence ctest_evidence
  gate container-stage phase_gate stage
  all_assembly_gates
  smoke_gates_all
  gate abi abi_gate
  write_context
  gate manifest manifest_gate
  gate promote promote
  cp -r "$LOGS" "$FINAL/gate-logs"
  say "release v$VERSION promoted to $FINAL"
  python3 -I -c "import json;m=json.load(open('$FINAL/release-manifest.json'));print('requires_rebuild =',str(m['requires_rebuild']).lower())"
  if [[ "$KEEP_WORK" != 1 ]]; then rm -rf "$RUN/src" "$RUN/out" "$RUN/smoke" "$RUN/source-stage"; fi
}

# ---------------------------------------------------------------- command: verify-staging (negative tests)
cmd_verify_staging() {
  [[ -d "$STAGING" ]] || die "STAGING=<package directory> is required"
  init_paths "v$VERSION"
  [[ -f "$LOGS/gates.jsonl" ]] || die "no kept run for v$VERSION under $OUT/work (run the release first with KEEP_WORK=1)"
  load_pins
  record_context
  local tag; tag="negative-$(date -u +%H%M%S)"
  W="$RUN/$tag"; LOGS="$W/logs"; GATES="$LOGS/gates.jsonl"
  mkdir -p "$LOGS"; : > "$GATES"
  FINAL="$W/final-not-created"
  BUILDER_ID=$(<"$RUN/builder-image.id"); SMOKE_ID=$(<"$RUN/smoke-image.id")
  PKG=$(realpath "$STAGING")
  PKG_NAME=$(basename "$PKG")
  derive_paths   # W and PKG_NAME changed: every derived path follows
  if [[ "$FROM" == assemble ]]; then
    all_assembly_gates
  else
    # FROM=smoke: the staging is taken as assembled (checksums are rewritten so only the fault under test can fail)
    gate source-stage assemble_source
    gate source-tarball source_tarball
    gate thirdparty-source thirdparty_source
    gate package-manifest package_manifest
    gate package-tarball package_tarball
  fi
  smoke_gates_all
  say "verify-staging unexpectedly passed every gate"
}

# ---------------------------------------------------------------- command: verify-repro
repro_one() {   # repro_one <a|b>
  init_paths "repro-$1"
  FINAL="$OUT/repro/v$VERSION"
  rm -rf "$RUN"; mkdir -p "$LOGS"; : > "$GATES"
  record_context
  BUILDER_ID=$(<"$OUT/work/repro-images/builder.id"); SMOKE_ID=$(<"$OUT/work/repro-images/smoke.id")
  mkdir -p "$RO/home" "$RO/nuget" "$RO/container-tmp"; chmod 1777 "$RO/container-tmp"
  say "== repro build $1 =="
  gate export-source export_source
  gate container-prep phase_gate prep
  gate container-build phase_gate build
  gate container-stage phase_gate stage
  PREV=none
  gate source-stage assemble_source
  gate source-tarball source_tarball
  gate thirdparty-source thirdparty_source
  gate package-manifest package_manifest
  gate package-tarball package_tarball
}
cmd_verify_repro() {
  init_paths "repro-images"
  rm -rf "$RUN"; mkdir -p "$LOGS"; : > "$GATES"
  gate need-tools need_tools
  load_pins
  record_context
  gate pre-clean-tree pre_clean_tree
  gate pre-free-space pre_space
  gate pins-downloads pins_downloads
  gate pins-nxnative pins_nxnative
  gate images images_gate
  mkdir -p "$OUT/work/repro-images"
  cp "$RUN/builder-image.id" "$OUT/work/repro-images/builder.id"; cp "$RUN/smoke-image.id" "$OUT/work/repro-images/smoke.id"
  ( repro_one a )
  ( repro_one b )
  python3 -I "$HERE/repro_report.py" --a "$OUT/work/repro-a" --b "$OUT/work/repro-b" --version "$VERSION" \
    --pkg-name "$PKG_NAME" --out "$OUT/repro/v$VERSION" --epoch "$SOURCE_DATE_EPOCH"
  if [[ "$KEEP_WORK" != 1 ]]; then rm -rf "$OUT/work/repro-a/src" "$OUT/work/repro-b/src" "$OUT/work/repro-a/out" "$OUT/work/repro-b/out"; fi
}

case "$CMD" in
  release) cmd_release ;;
  verify-staging) cmd_verify_staging ;;
  verify-repro) cmd_verify_repro ;;
  image-fp)
    init_paths "image-fp"; rm -rf "$RUN"; mkdir -p "$LOGS"; : > "$GATES"
    load_pins; BUILDER_FP=x SMOKE_FP=x
    pins_downloads; build_image builder; echo "BUILDER_FP=$IMAGE_FP"; build_image smoke; echo "SMOKE_FP=$IMAGE_FP" ;;
  *) die "unknown command '$CMD' (release | verify-repro | verify-staging | image-fp)" ;;
esac
