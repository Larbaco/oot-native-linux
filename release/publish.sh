#!/usr/bin/env bash
# Deterministic publication of a release that `make release` already promoted. No LLM, no prompts.
#
#   make publish VERSION=1.2.3 OUT=<work dir>            dry-run (default): runs every local check, builds the public commit in a
#                                                        scratch clone, `git push --dry-run`, prints what it would do
#   make publish VERSION=1.2.3 OUT=<work dir> PUBLISH=1  really publishes (commit, tag, release), then verifies it
#
# Inputs: OUT/v<VERSION>/ (manifest, three tarballs and their .sha256), release/notes/v<VERSION>.md (versioned notes with placeholders).
# Optional: STABLE=1 (not a pre-release, marked --latest), PUBLIC_REPO / BUILDER_REPO (derived from `git remote origin`),
# PUBLISH_AUTHOR_NAME / PUBLISH_AUTHOR_EMAIL (default: git config), ALLOW_NEW_IDENTITY=1.
# The author identity is never stored in the repository: it comes from git config or the environment and must equal the author of the
# previous public release commit.
set -euo pipefail
export LC_ALL=C.UTF-8 TZ=UTC
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/.." && pwd)
VERSION=${VERSION:-}; OUT=${OUT:-${OOT_RELEASE_WORK:-}}; PUBLISH=${PUBLISH:-0}; STABLE=${STABLE:-0}
ALLOW_NEW_IDENTITY=${ALLOW_NEW_IDENTITY:-0}
die() { printf 'publish: %s\n' "$*" >&2; exit 2; }
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || die "VERSION=x.y.z is required"
[[ -n "$OUT" && -d "$OUT" ]] || die "OUT=<work directory> is required"
OUT=$(realpath "$OUT")
case "$PUBLISH" in 0|1) ;; *) die "PUBLISH must be 0 or 1" ;; esac
REL="$OUT/v$VERSION"
PUB="$OUT/publish/v$VERSION"
LOGS="$PUB/logs"
TAG="v$VERSION"
rm -rf "$PUB"; mkdir -p "$LOGS"

step() {   # step <name> <command...>
  local name=$1; shift
  local log="$LOGS/$name.log" rc=0
  set +e; ( set -euo pipefail; "$@" ) > "$log" 2>&1; rc=$?; set -e
  if (( rc == 0 )); then printf '[PASS] %s\n' "$name"; sed 's/^/        /' "$log" | tail -n 4
  else printf '[FAIL] %s (rc=%s)\n' "$name" "$rc" >&2; cat "$log" >&2; printf 'publish: refusing to continue; nothing was published\n' >&2; exit 1; fi
}
chk() { python3 -I "$HERE/publish_check.py" "$@"; }
gh_status() { gh api -i "$1" 2>/dev/null | head -1 | awk '{print $2}' || true; }

for t in git gh python3 sha256sum tar; do command -v "$t" >/dev/null || die "missing tool: $t"; done
ORIGIN_URL=$(git -C "$ROOT" remote get-url origin) || die "no origin remote"
BUILDER_REPO=${BUILDER_REPO:-$(sed -E 's#^(git@github\.com:|https://github\.com/)##; s#\.git$##' <<<"$ORIGIN_URL")}
if [[ -z "${PUBLIC_REPO:-}" ]]; then
  [[ "$BUILDER_REPO" == */*-builder ]] || die "cannot derive the public repository from '$BUILDER_REPO': set PUBLIC_REPO=owner/name"
  PUBLIC_REPO="${BUILDER_REPO%-builder}-linux"
fi
[[ "$PUBLIC_REPO" != "$BUILDER_REPO" ]] || die "the public and builder repositories are the same"
NOTES_TEMPLATE="$ROOT/release/notes/$TAG.md"
AUTHOR_NAME=${PUBLISH_AUTHOR_NAME:-$(git -C "$ROOT" config user.name || true)}
AUTHOR_EMAIL=${PUBLISH_AUTHOR_EMAIL:-$(git -C "$ROOT" config user.email || true)}

printf 'publish: %s  mode=%s  builder=%s  public=%s\n' "$TAG" "$([[ $PUBLISH == 1 ]] && echo PUBLISH || echo DRY-RUN)" "$BUILDER_REPO" "$PUBLIC_REPO"

# ----------------------------------------------------------------- local preconditions
local_checks() {
  [[ -f "$NOTES_TEMPLATE" ]] || { echo "missing versioned release notes: release/notes/$TAG.md"; return 1; }
  chk local --release-dir "$REL" --version "$VERSION" --head "$(git -C "$ROOT" rev-parse HEAD)" \
      --porcelain "$(git -C "$ROOT" status --porcelain --untracked-files=all)"
}
step local-preconditions local_checks
PKG_NAME=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['artifacts']['package']['file'][:-len('.tar.xz')])" "$REL/release-manifest.json")
SOURCE_DATE_EPOCH=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['source_date_epoch'])" "$REL/release-manifest.json")
MANIFEST_COMMIT=$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['commit'])" "$REL/release-manifest.json")

# ----------------------------------------------------------------- remote preconditions
remote_checks() {
  gh api "repos/$BUILDER_REPO" > "$PUB/builder.json"
  chk builder-private "$PUB/builder.json"
  gh api "repos/$PUBLIC_REPO" --jq '{private: .private, default_branch: .default_branch}' > "$PUB/public.json"
  python3 -I -c "import json,sys;d=json.load(open(sys.argv[1]));assert d['private'] is False, 'the public repository is not public';print('public repository is public, default branch', d['default_branch'])" "$PUB/public.json"
  local tag_status rel_status
  tag_status=$(gh_status "repos/$PUBLIC_REPO/git/ref/tags/$TAG"); rel_status=$(gh_status "repos/$PUBLIC_REPO/releases/tags/$TAG")
  chk public-absent "${tag_status:-0}" "${rel_status:-0}"
  gh api "repos/$PUBLIC_REPO/commits/main" --jq '[.sha,.commit.author.name,.commit.author.email]|@tsv' > "$PUB/public-main.tsv"
  IFS=$'\t' read -r PUB_MAIN LAST_NAME LAST_EMAIL < "$PUB/public-main.tsv"
  echo "public main is $PUB_MAIN"
  local allow=(); [[ "$ALLOW_NEW_IDENTITY" == 1 ]] && allow=(--allow-new)
  chk identity --name "$AUTHOR_NAME" --email "$AUTHOR_EMAIL" --last-name "$LAST_NAME" --last-email "$LAST_EMAIL" "${allow[@]}"
  git ls-remote --tags "https://github.com/$PUBLIC_REPO.git" | sort > "$PUB/tags-before.txt"
}
step remote-preconditions remote_checks
PUB_MAIN=$(cut -f1 "$PUB/public-main.tsv")

# ----------------------------------------------------------------- export, scans, notes
CLONE="$PUB/repo"; EXPORT="$PUB/export"
prepare_export() {
  gh repo clone "$PUBLIC_REPO" "$CLONE" -- --quiet
  [[ "$(git -C "$CLONE" rev-parse HEAD)" == "$PUB_MAIN" ]] || { echo "the clone is not at the public main seen by the API"; return 1; }
  # The allowlist of the commit being released (the manifest commit is HEAD, checked above).
  git -C "$ROOT" show "$MANIFEST_COMMIT:release/allowlist.txt" > "$PUB/allowlist.txt"
  chk export --source-tar "$REL/$PKG_NAME-source.tar.xz" --dest "$EXPORT" --allowlist "$PUB/allowlist.txt" --previous-tree "$CLONE"
  # the same scanner the release pipeline uses
  python3 -I "$ROOT/release/release_check.py" "$EXPORT" --allowlist "$PUB/allowlist.txt" --mode source
}
step export-and-scans prepare_export
render_notes() { chk notes --template "$NOTES_TEMPLATE" --release-dir "$REL" --version "$VERSION" --out "$PUB/notes.md"; }
step release-notes render_notes

# ----------------------------------------------------------------- the public commit (deterministic: parent, tree, identity and date are fixed)
export GIT_AUTHOR_NAME="$AUTHOR_NAME" GIT_AUTHOR_EMAIL="$AUTHOR_EMAIL" GIT_COMMITTER_NAME="$AUTHOR_NAME" GIT_COMMITTER_EMAIL="$AUTHOR_EMAIL"
export GIT_AUTHOR_DATE="@$SOURCE_DATE_EPOCH +0000" GIT_COMMITTER_DATE="@$SOURCE_DATE_EPOCH +0000"
build_commit() {
  git -C "$CLONE" rm -rq --ignore-unmatch .
  find "$CLONE" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
  cp -a "$EXPORT"/. "$CLONE"/
  git -C "$CLONE" add -A
  git -C "$CLONE" -c commit.gpgsign=false commit -q -m "OoT Native $TAG"
  git -C "$CLONE" -c tag.gpgsign=false tag -a "$TAG" -m "OoT Native $TAG"
  [[ "$(git -C "$CLONE" rev-parse HEAD^)" == "$PUB_MAIN" ]] || { echo "the new commit is not a child of the public main"; return 1; }
  [[ "$(git -C "$CLONE" rev-list --count "$PUB_MAIN..HEAD")" == 1 ]] || { echo "more than one new commit"; return 1; }
  git -C "$CLONE" diff --stat "$PUB_MAIN" HEAD | tail -n 1
  echo "new public commit $(git -C "$CLONE" rev-parse HEAD) (tree $(git -C "$CLONE" rev-parse 'HEAD^{tree}')), tag object $(git -C "$CLONE" rev-parse "$TAG")"
  chk tree-diff "$EXPORT" "$CLONE"
  git -C "$CLONE" push --dry-run --quiet origin "HEAD:refs/heads/main" "refs/tags/$TAG" 2>&1 && echo "git push --dry-run accepted (fast-forward, no force)"
}
step public-commit build_commit

# ----------------------------------------------------------------- the plan
ASSETS=()
for n in "$PKG_NAME.tar.xz" "$PKG_NAME-source.tar.xz" "$PKG_NAME-thirdparty-source.tar.xz"; do ASSETS+=("$REL/$n" "$REL/$n.sha256"); done
RELEASE_ARGS=(release create "$TAG" -R "$PUBLIC_REPO" --verify-tag --notes-file "$PUB/notes.md")
if [[ "$STABLE" == 1 ]]; then RELEASE_ARGS+=(--title "OoT Native $TAG" --latest); else RELEASE_ARGS+=(--title "OoT Native $TAG (pre-release)" --prerelease); fi
cat <<PLAN

PLAN ($TAG)
  public repo     : $PUBLIC_REPO  (main $PUB_MAIN -> $(git -C "$CLONE" rev-parse HEAD))
  author          : identity from git config/environment (equal to the previous public release commit)
  commit message  : OoT Native $TAG   (single commit, no builder history; export = source archive minus upstream/)
  tag             : annotated $TAG
  release         : $([[ $STABLE == 1 ]] && echo "stable, --latest" || echo "pre-release") with ${#ASSETS[@]} assets: $(for a in "${ASSETS[@]}"; do basename "$a"; done | tr '\n' ' ')
  notes           : $PUB/notes.md (from release/notes/$TAG.md)
  commands        : git push origin main ; git push origin $TAG ; gh ${RELEASE_ARGS[*]} <assets>
PLAN

if [[ "$PUBLISH" != 1 ]]; then
  printf '\npublish: DRY-RUN complete; every check passed and nothing was published. Re-run with PUBLISH=1 to publish.\n'
  exit 0
fi

# ----------------------------------------------------------------- publish
recheck_and_push() {
  # nothing may have moved since the checks
  [[ "$(gh api "repos/$PUBLIC_REPO/commits/main" --jq .sha)" == "$PUB_MAIN" ]] || { echo "public main moved during the run"; return 1; }
  chk public-absent "$(gh_status "repos/$PUBLIC_REPO/git/ref/tags/$TAG")" "$(gh_status "repos/$PUBLIC_REPO/releases/tags/$TAG")"
  gh api "repos/$BUILDER_REPO" > "$PUB/builder.json"; chk builder-private "$PUB/builder.json"
  git -C "$CLONE" push origin "HEAD:refs/heads/main"
  git -C "$CLONE" push origin "refs/tags/$TAG"
  gh "${RELEASE_ARGS[@]}" "${ASSETS[@]}"
}
step push-and-release recheck_and_push

# ----------------------------------------------------------------- post-verification
post_verify() {
  local newsha; newsha=$(git -C "$CLONE" rev-parse HEAD)
  [[ "$(gh api "repos/$PUBLIC_REPO/commits/main" --jq .sha)" == "$newsha" ]] || { echo "public main is not the new commit"; return 1; }
  gh api "repos/$PUBLIC_REPO/releases/tags/$TAG" > "$PUB/release.json"
  chk assets --release-json "$PUB/release.json" --release-dir "$REL" --version "$VERSION" $([[ $STABLE == 1 ]] && echo --stable)
  # the sidecars attached to the release equal the local ones
  mkdir -p "$PUB/downloaded"
  gh release download "$TAG" -R "$PUBLIC_REPO" -D "$PUB/downloaded" --pattern '*.sha256'
  for f in "$PUB"/downloaded/*.sha256; do cmp "$f" "$REL/$(basename "$f")"; done
  # the tag is annotated and points at the new commit; the older tags did not move
  git ls-remote --tags "https://github.com/$PUBLIC_REPO.git" | sort > "$PUB/tags-after.txt"
  diff <(grep -v "refs/tags/$TAG" "$PUB/tags-after.txt") "$PUB/tags-before.txt" > /dev/null || { echo "an existing tag changed"; diff "$PUB/tags-before.txt" "$PUB/tags-after.txt" || true; return 1; }
  grep -q "refs/tags/$TAG\^{}" "$PUB/tags-after.txt" || { echo "the tag is not annotated"; return 1; }
  [[ "$(awk -v t="refs/tags/$TAG^{}" '$2==t {print $1}' "$PUB/tags-after.txt")" == "$newsha" ]] || { echo "the tag does not point at the new commit"; return 1; }
  # the published main is exactly the export, and the guard scans pass on what was published
  gh repo clone "$PUBLIC_REPO" "$PUB/published" -- --quiet
  [[ "$(git -C "$PUB/published" rev-parse HEAD)" == "$newsha" ]]
  chk tree-diff "$EXPORT" "$PUB/published"
  chk scan "$PUB/published"
  gh api "repos/$BUILDER_REPO" > "$PUB/builder.json"; chk builder-private "$PUB/builder.json"
  echo "published: https://github.com/$PUBLIC_REPO/releases/tag/$TAG"
}
step post-verification post_verify
printf '\npublish: %s published and verified.\n' "$TAG"
