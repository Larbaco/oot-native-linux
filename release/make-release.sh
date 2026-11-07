#!/usr/bin/env bash
# Build, stage, scan, and archive the complete private v0.1.2 release.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${OOT_RELEASE_WORK:-${XDG_CACHE_HOME:-${TMPDIR:-/tmp}}/oot-release-work}
JOBS=${NXS2_JOBS:-8}
BRANCH=$(git -C "$ROOT" branch --show-current)
if [[ "$BRANCH" != "release/v0.1.1" ]]; then
  echo "make-release: expected branch release/v0.1.1, got $BRANCH" >&2
  exit 2
fi
if ! command -v docker >/dev/null; then
  echo "make-release: Docker is required for the Ubuntu 22.04 build" >&2
  exit 2
fi
if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || (( JOBS > 10 )); then
  echo "make-release: NXS2_JOBS must be between 1 and 10" >&2
  exit 2
fi
mkdir -p "$WORK"
ROOT=$(realpath -m "$ROOT")
WORK=$(realpath -m "$WORK")
if [[ "$WORK" == / || "$WORK" == "$ROOT" || "$WORK" == "$ROOT/"* ]]; then
  echo "make-release: refusing an unsafe workspace location: $WORK" >&2
  exit 2
fi

VERSION=v0.1.2
IMAGE=oot-native-release-builder:ubuntu22.04
SRC="$WORK/source-$VERSION"
RELEASE_DATE=${OOT_RELEASE_DATE:-$(date +%F)}
if [[ ! "$RELEASE_DATE" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
  echo "make-release: OOT_RELEASE_DATE must be YYYY-MM-DD" >&2
  exit 2
fi
PKG_NAME="oot-native-linux-x86_64-$VERSION-$RELEASE_DATE"
PKG="$WORK/$PKG_NAME"
SOURCE_NAME="$PKG_NAME-source"
SOURCE_STAGE="$WORK/source-stage/$SOURCE_NAME"
ART="$WORK/artifacts"
ATTEMPT_TAG="$(date -u +%Y%m%dT%H%M%SZ)-$(git -C "$ROOT" rev-parse --short HEAD)-$$"
SOURCE_TARBALL="$WORK/$SOURCE_NAME-candidate-$ATTEMPT_TAG.tar.xz"
PACKAGE_TARBALL="$WORK/$PKG_NAME-candidate-$ATTEMPT_TAG.tar.xz"
FINAL_SOURCE_TARBALL="$WORK/$SOURCE_NAME.tar.xz"
FINAL_PACKAGE_TARBALL="$WORK/$PKG_NAME.tar.xz"
NEGATIVE="$WORK/negative-$VERSION"
DOCKER_CONTEXT="$WORK/docker-context"
SMOKE="$WORK/package-smoke-$RELEASE_DATE"
SMOKE_IMAGE=oot-native-package-smoke:ubuntu22.04
SOURCE_DATE_EPOCH=$(git -C "$ROOT" show -s --format=%ct HEAD)
REFERENCE_PACKAGE=${OOT_REFERENCE_PACKAGE:-$WORK/oot-native-linux-x86_64-v0.1.0-2026-11-06}

wait_for_artifact_notice() {
  local kind=$1 gate="$WORK/.continue-after-$1"
  if [[ ${OOT_RELEASE_NOTICE_GATES:-0} == 1 ]]; then
    echo "make-release: paused after $kind artifact; create $gate to continue"
    while [[ ! -f "$gate" ]]; do sleep 1; done
    rm -f "$gate"
  fi
}

if [[ -e "$FINAL_SOURCE_TARBALL" || -e "$FINAL_PACKAGE_TARBALL" || \
      -e "$FINAL_SOURCE_TARBALL.sha256" || -e "$FINAL_PACKAGE_TARBALL.sha256" ]]; then
  echo "make-release: final archive name already exists; preserve or rename it before publishing" >&2
  exit 2
fi
if [[ ! -x "$REFERENCE_PACKAGE/builder/bin/nxrt" ]]; then
  echo "make-release: missing v0.1.0 reference package nxrt: $REFERENCE_PACKAGE/builder/bin/nxrt" >&2
  exit 2
fi

# Keep downloaded toolchains and Python source in WORK for incremental rebuilds;
# clear only outputs owned by this release invocation.
rm -rf "$SRC" "$PKG" "$WORK/source-stage" "$ART/upstream-source" "$WORK/launcher-venv" \
  "$WORK/pyinstaller-work" "$WORK/$SOURCE_NAME" "$NEGATIVE" "$DOCKER_CONTEXT" "$SMOKE"
rm -f "$SOURCE_TARBALL" "$PACKAGE_TARBALL" "$WORK/package-allowlist.txt" \
  "$WORK/glibc-baseline.txt" "$WORK/launcher-help.txt" "$WORK/build-result.txt" \
  "$WORK/setup.log" "$WORK/ctest-build.log" "$WORK/ctest.log" \
  "$WORK/python-tests.log" "$WORK/tool-tests.log" "$WORK/negative-check.log"
mkdir -p "$SRC" "$ART" "$WORK/source-stage"

echo "make-release: exporting committed source HEAD $(git -C "$ROOT" rev-parse --short HEAD) into private workspace"
# All release inputs come from git archive HEAD; worktree edits are preserved but excluded.
git -C "$ROOT" archive --format=tar HEAD | tar -xf - -C "$SRC"
SOURCE_ALLOWLIST="$WORK/source-allowlist.txt"
cp "$SRC/release/allowlist.txt" "$SOURCE_ALLOWLIST"
NXNATIVE_CACHE=${OOT_NXNATIVE_CACHE:-$WORK/upstream-nxnative-c1df4dd}
NXNATIVE_COMMIT=c1df4ddaaedc3c18228065ea5b909688b79884e2
if [[ -d "$NXNATIVE_CACHE/.git" ]] && \
   [[ "$(git -C "$NXNATIVE_CACHE" rev-parse HEAD 2>/dev/null || true)" == "$NXNATIVE_COMMIT" ]]; then
  echo "make-release: seeding the pinned nxnative checkout from the verified local object cache"
  nice -n 19 ionice -c3 git clone -q --no-hardlinks --no-checkout "$NXNATIVE_CACHE" "$SRC/nxnative"
  git -C "$SRC/nxnative" checkout -q --detach "$NXNATIVE_COMMIT"
fi

echo "make-release: building Ubuntu 22.04 release image"
mkdir -p "$DOCKER_CONTEXT"
install -m 0644 "$SRC/release/Dockerfile.ubuntu22.04" "$DOCKER_CONTEXT/Dockerfile.ubuntu22.04"
nice -n 19 ionice -c3 docker build --pull -t "$IMAGE" -f "$DOCKER_CONTEXT/Dockerfile.ubuntu22.04" "$DOCKER_CONTEXT"

echo "make-release: compiling tools and staging bundle in Ubuntu 22.04"
nice -n 19 ionice -c3 docker run --rm --cpus="$JOBS" -e NXS2_JOBS="$JOBS" \
  -e NXS2_RELEASE_PKG_NAME="$PKG_NAME" \
  -v "$SRC:/src" -v "$WORK:/out" "$IMAGE" /bin/bash /src/release/build-in-container.sh

test -x "$PKG/oot-native-launcher"
test -x "$PKG/builder/bin/nxrt"
test -x "$PKG/builder/bin/nxanalyze"
test -x "$PKG/builder/bin/nxlift"
test -x "$PKG/builder/bin/zbic-unpack"
test -x "$PKG/builder/ampere-lower/ampere-lower"
test -f "$PKG/builder/bin/dmsh_xlate.dll"
test -f "$PKG/builder/lib/libSDL3.so.0"

while IFS= read -r source; do
  [[ -z "$source" || "$source" == \#* ]] && continue
  if [[ ! -f "$SRC/$source" ]]; then
    echo "make-release: allowlisted source missing: $source" >&2
    exit 1
  fi
  mode=$(stat -c %a "$SRC/$source")
  install -D -m "$mode" "$SRC/$source" "$SOURCE_STAGE/$source"
done < "$SRC/release/allowlist.txt"
while IFS= read -r source; do
  [[ -z "$source" ]] && continue
  rel=${source#upstream/}
  if [[ ! -f "$ART/upstream-source/$rel" ]]; then
    echo "make-release: translator source missing: $source" >&2
    exit 1
  fi
  install -D -m 0644 "$ART/upstream-source/$rel" "$SOURCE_STAGE/$source"
  printf '%s\n' "$source" >> "$SOURCE_ALLOWLIST"
done < "$ART/upstream-source/manifest.txt"

echo "make-release: checking source staging"
python3 "$SOURCE_STAGE/release/release_check.py" "$SOURCE_STAGE" --allowlist "$SOURCE_ALLOWLIST" --mode source
if [[ -e "$SOURCE_STAGE/.git" ]]; then echo "make-release: source staging unexpectedly contains git history" >&2; exit 1; fi

echo "make-release: writing filtered corresponding-source archive"
nice -n 19 ionice -c3 tar --owner=0 --group=0 --numeric-owner --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
  --format=gnu -I 'xz -T8 -6' -cf "$SOURCE_TARBALL" -C "$WORK/source-stage" "$SOURCE_NAME"
python3 "$SOURCE_STAGE/release/release_check.py" "$SOURCE_TARBALL" --allowlist "$SOURCE_ALLOWLIST" --mode source
wait_for_artifact_notice source-candidate

install -m 0644 "$SOURCE_TARBALL" "$PKG/$SOURCE_NAME.tar.xz"
python3 - "$PKG" <<'PY'
import pathlib, sys
root=pathlib.Path(sys.argv[1])
entries=sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() or p.is_symlink())
(root/'PACKAGE-MANIFEST.txt').write_text('\n'.join(entries + ['PACKAGE-MANIFEST.txt', 'SHA256SUMS'])+'\n',encoding='utf-8')
PY
(
  cd "$PKG"
  find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)
cp "$PKG/PACKAGE-MANIFEST.txt" "$WORK/package-allowlist.txt"

echo "make-release: checking binary staging"
python3 "$SOURCE_STAGE/release/release_check.py" "$PKG" \
  --allowlist "$WORK/package-allowlist.txt" --mode package

echo "make-release: proving a forbidden synthetic asset is rejected"
cp -al "$PKG" "$NEGATIVE"
printf 'NSO0 synthetic negative fixture\n' > "$NEGATIVE/forbidden.nso"
if python3 "$SOURCE_STAGE/release/release_check.py" "$NEGATIVE" \
  --allowlist "$WORK/package-allowlist.txt" --mode package > "$WORK/negative-check.log" 2>&1; then
  echo "make-release: release_check accepted a forbidden fixture" >&2
  exit 1
else
  cat "$WORK/negative-check.log"
  echo "PASS: forbidden synthetic fixture rejected"
fi

echo "make-release: writing package tarball"
nice -n 19 ionice -c3 tar --owner=0 --group=0 --numeric-owner --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
  --format=gnu -I 'xz -T8 -6' -cf "$PACKAGE_TARBALL" -C "$WORK" "$PKG_NAME"
python3 "$SOURCE_STAGE/release/release_check.py" "$PACKAGE_TARBALL" \
  --allowlist "$WORK/package-allowlist.txt" --mode package
wait_for_artifact_notice package-candidate

echo "make-release: extracting the final archive for clean Ubuntu 22.04 package smoke tests"
mkdir -p "$SMOKE/extracted" "$SMOKE/embedded-elf" "$SMOKE/fixtures" "$SMOKE/state"
tar -xf "$PACKAGE_TARBALL" -C "$SMOKE/extracted"
test -d "$SMOKE/extracted/$PKG_NAME"
printf 'synthetic invalid input, not a NUP file\n' > "$SMOKE/fixtures/invalid.nup"
PYTHONPATH="$WORK/launcher-venv/lib/python3.12/site-packages" python3 - \
  "$SMOKE/embedded-elf" "$SMOKE/extracted/$PKG_NAME/oot-native-launcher" "$SMOKE/extracted/$PKG_NAME" <<'PY'
import pathlib, sys
from PyInstaller.archive.readers import CArchiveReader
out, exe, package = map(pathlib.Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
archive = CArchiveReader(str(exe))
external_libraries = set()
for name in archive.toc:
    if '.so' not in name.lower():
        continue
    data = archive.extract(name)
    if not isinstance(data, bytes) or data[:4] != bytes((127, 69, 76, 70)):
        continue
    library = pathlib.PurePosixPath(name).name
    if library.startswith('lib') and library not in {'libpython3.12.so.1.0', 'libtcl8.6.so', 'libtk8.6.so'}:
        external_libraries.add(library)
    relative = pathlib.PurePosixPath(name)
    if relative.is_absolute() or '..' in relative.parts:
        raise SystemExit(f'unsafe PyInstaller archive member {name!r}')
    target = out / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    target.chmod(0o644)
count = sum(1 for p in out.rglob('*') if p.is_file())
if not count:
    raise SystemExit('no embedded ELF libraries extracted from the PyInstaller launcher')
manifest = package/'LICENSES/PyInstaller-embedded-libraries.txt'
licensed = {line.split(' -> ',1)[0] for line in manifest.read_text().splitlines() if ' -> ' in line}
if external_libraries != licensed:
    raise SystemExit('PyInstaller shared-library license map mismatch: '
                     f'unlicensed={sorted(external_libraries-licensed)} '
                     f'not-embedded={sorted(licensed-external_libraries)}')
print(f'extracted {count} PyInstaller ELF files for ldd validation')
PY
echo "make-release: building clean Ubuntu 22.04 runtime-only smoke image"
nice -n 19 ionice -c3 docker build --pull -t "$SMOKE_IMAGE" -f "$SRC/release/Dockerfile.smoke.ubuntu22.04" "$SRC/release"
nice -n 19 ionice -c3 docker run --rm --network none --cpus=2 --tmpfs /doctor-space:rw,size=50g \
  -e OOT_REFERENCE_PACKAGE=/reference-package \
  -v "$SMOKE/extracted/$PKG_NAME:/pkg:ro" \
  -v "$REFERENCE_PACKAGE:/reference-package:ro" \
  -v "$SMOKE/embedded-elf:/embedded-elf:ro" \
  -v "$SMOKE/fixtures:/fixtures:ro" \
  -v "$SMOKE/state:/tmp/oot-native-smoke" \
  -v "$SRC/release:/release-src:ro" \
  -v "$SRC/release/package-smoke.sh:/smoke.sh:ro" \
  "$SMOKE_IMAGE" /bin/bash /smoke.sh | tee "$WORK/package-smoke.log"
grep -Fxq 'PASS: complete packaged smoke finished' "$WORK/package-smoke.log" || {
  echo 'make-release: complete package smoke did not reach its PASS gate' >&2
  exit 1
}

echo "make-release: publishing scanned candidate archives to their dated final names"
install -m 0644 "$SOURCE_TARBALL" "$FINAL_SOURCE_TARBALL"
cmp -s "$SOURCE_TARBALL" "$FINAL_SOURCE_TARBALL"
(cd "$WORK" && sha256sum "$(basename "$FINAL_SOURCE_TARBALL")" > "$(basename "$FINAL_SOURCE_TARBALL").sha256")
wait_for_artifact_notice source-final
install -m 0644 "$PACKAGE_TARBALL" "$FINAL_PACKAGE_TARBALL"
cmp -s "$PACKAGE_TARBALL" "$FINAL_PACKAGE_TARBALL"
(cd "$WORK" && sha256sum "$(basename "$FINAL_PACKAGE_TARBALL")" > "$(basename "$FINAL_PACKAGE_TARBALL").sha256")
wait_for_artifact_notice package-final

echo "make-release: final artifact evidence"
stat -c '%n %s bytes' "$FINAL_PACKAGE_TARBALL" "$FINAL_SOURCE_TARBALL"
sha256sum "$FINAL_PACKAGE_TARBALL" "$FINAL_SOURCE_TARBALL"
cat "$WORK/glibc-baseline.txt"
cat "$WORK/ctest.log"
tail -5 "$WORK/python-tests.log"
tail -5 "$WORK/tool-tests.log"
echo "make-release: staging=$PKG package=$FINAL_PACKAGE_TARBALL source=$FINAL_SOURCE_TARBALL"
