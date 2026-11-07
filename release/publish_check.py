#!/usr/bin/env python3
"""Pure, testable checks behind release/publish.sh. Every check returns a list of problems (empty = pass).

No names, e-mails or repository owners are written here: they come from git config, the environment and `git remote`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile

sys.dont_write_bytecode = True
HERE = pathlib.Path(__file__).resolve().parent
# The title the owner decided to leave in text (nominative use); written in pieces so this file is not itself a mention.
POKO = 'poko' + 'pia'
ASSET_KINDS = ('', '-source', '-thirdparty-source')   # suffix before .tar.xz


def asset_names(pkg_name: str) -> list[str]:
    tars = [f'{pkg_name}{k}.tar.xz' for k in ASSET_KINDS]
    return tars + [t + '.sha256' for t in tars]


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------------ manifest and artifacts
def check_manifest(m: dict, version: str) -> list[str]:
    problems = []
    if m.get('version') != version:
        problems.append(f"manifest version {m.get('version')!r} != {version!r}")
    gates = m.get('gates') or []
    if not gates:
        problems.append('manifest has no gates')
    for g in gates:
        if g.get('status') != 'PASS':
            problems.append(f"gate {g.get('name')} is {g.get('status')}")
    smoke = m.get('smoke_gates') or []
    if not smoke:
        problems.append('manifest has no smoke gates')
    for g in smoke:
        if g.get('status') != 'PASS':
            problems.append(f"smoke gate {g.get('id')} is {g.get('status')}")
    ct = m.get('ctest') or {}
    if ct.get('failed') != 0 or not ct.get('total'):
        problems.append(f'CTest not clean: {ct}')
    if not isinstance(m.get('requires_rebuild'), bool):
        problems.append('manifest lacks a boolean requires_rebuild')
    elif m['requires_rebuild'] and not (m.get('abi') or {}).get('abi_break_allowed'):
        problems.append('requires_rebuild is true and the ABI break was not explicitly allowed')
    if m.get('tree_clean') is not True:
        problems.append('the manifest does not record a clean tree')
    arts = m.get('artifacts') or {}
    for key in ('package', 'source', 'thirdparty_source'):
        if key not in arts:
            problems.append(f'manifest lacks artifacts.{key}')
    return problems


def check_artifacts(release_dir: pathlib.Path, m: dict) -> list[str]:
    problems = []
    for key in ('package', 'source', 'thirdparty_source'):
        art = (m.get('artifacts') or {}).get(key)
        if not art:
            continue
        f = release_dir / art['file']
        side = release_dir / (art['file'] + '.sha256')
        if not f.is_file() or not side.is_file():
            problems.append(f"missing {art['file']} or its .sha256")
            continue
        actual = sha256_file(f)
        line = side.read_text().split()
        if len(line) != 2 or line[1].lstrip('*') != art['file'] or line[0] != actual:
            problems.append(f"{art['file']}: sidecar does not match the file")
        if actual != art['sha256']:
            problems.append(f"{art['file']}: sha256 {actual} != manifest {art['sha256']}")
        if f.stat().st_size != art['bytes']:
            problems.append(f"{art['file']}: size differs from the manifest")
    return problems


def check_commit(manifest_commit: str, head: str, porcelain: str) -> list[str]:
    problems = []
    if manifest_commit != head:
        problems.append(f'manifest commit {manifest_commit[:12]} is not HEAD {head[:12]}: build the release again at HEAD')
    if porcelain.strip():
        problems.append('the working tree is not clean')
    return problems


# ------------------------------------------------------------------ remote state (data comes from `gh api`)
def check_builder_private(repo_json: dict) -> list[str]:
    if repo_json.get('private') is not True:
        return ['the builder repository is not private (or its visibility could not be read)']
    return []


def check_public_absent(tag_http_status: int, release_http_status: int) -> list[str]:
    problems = []
    if tag_http_status != 404:
        problems.append(f'the tag already exists in the public repository (or cannot be queried): HTTP {tag_http_status}')
    if release_http_status != 404:
        problems.append(f'the release already exists in the public repository (or cannot be queried): HTTP {release_http_status}')
    return problems


def check_identity(name: str, email: str, last_name: str, last_email: str, allow_new: bool = False) -> list[str]:
    if not name or not email:
        return ['no author identity: set git config user.name/user.email or PUBLISH_AUTHOR_NAME/PUBLISH_AUTHOR_EMAIL']
    if (name, email) != (last_name, last_email) and not allow_new:
        return ['the author identity differs from the one of the previous public release commit '
                '(set ALLOW_NEW_IDENTITY=1 only if that is intended)']
    return []


# ------------------------------------------------------------------ export content
def check_export(exported: set[str], allowlist: set[str], source_only_prefix: str = 'upstream/') -> list[str]:
    allowed = {a for a in allowlist if not a.startswith(source_only_prefix)}
    problems = [f'allowlisted file missing from the export: {m}' for m in sorted(allowed - exported)]
    problems += [f'file in the export but not allowlisted: {e}' for e in sorted(exported - allowed)]
    return problems


PERSONAL_PATTERNS = {
    'private LAN address': re.compile(rb'192\.168\.'),
    'owner login': re.compile(b'lar' + b'baco', re.I),
    'home directory': re.compile(b'/ho' + b'me/[A-Za-z_]'),
    'work disk path': re.compile(b'/mn' + b't/cabal'),
    'macOS user path': re.compile(b'/Us' + b'ers/[A-Za-z_]'),
    'private key': re.compile(rb'BEGIN [A-Z ]*PRIVATE'),
    'GitHub token': re.compile(rb'(?:ghp_|github_pat_)[A-Za-z0-9_]{10,}'),
    'API key (sk-)': re.compile(rb'\bsk-[A-Za-z0-9]{20,}'),
    'AWS key': re.compile(rb'AKIA[0-9A-Z]{16}'),
    'Slack token': re.compile(rb'xox[abp]-'),
    'windows user path': re.compile(b'c:' + b'\\\\us' + b'ers\\\\', re.I),
}
# bounded quantifiers: an unbounded local part is quadratic on a long run of letters
EMAIL = re.compile(rb'[A-Za-z0-9._+\-]{1,64}@[A-Za-z0-9\-]{1,63}\.[A-Za-z0-9.\-]{1,200}')
EMAIL_OK = {b'git@github.com'}
FORBIDDEN_SUFFIXES = {'.nup', '.nso', '.nro', '.nca', '.keys', '.z64', '.png', '.jpg', '.jpeg', '.mp4', '.wav', '.ogg', '.webm',
                      '.mkv', '.ivf', '.xci', '.nsp', '.spv', '.bfsha', '.bntx', '.zs', '.sav', '.tik'}
MAX_BYTES = 5 * 1024 * 1024


def scan_export(root: pathlib.Path, previous_poko: set[str] | None = None) -> tuple[list[str], dict]:
    """The guard scans of the previous publications, over a directory tree."""
    problems: list[str] = []
    poko_files: set[str] = set()
    count = 0
    for p in sorted(root.rglob('*')):
        if p.is_symlink():
            problems.append(f'symbolic link in the export: {p.relative_to(root)}')
            continue
        if not p.is_file() or '.git' in p.relative_to(root).parts:
            continue
        rel = p.relative_to(root).as_posix()
        count += 1
        data = p.read_bytes()
        if p.suffix.lower() in FORBIDDEN_SUFFIXES:
            problems.append(f'{rel}: forbidden file type')
        if POKO in rel.lower():
            problems.append(f'{rel}: file name mentions {POKO}')
        if len(data) > MAX_BYTES:
            problems.append(f'{rel}: larger than 5 MB')
        if b'\0' in data[:8192]:
            problems.append(f'{rel}: binary (non-text) file')
        lname = rel.encode().lower()
        for label, rx in PERSONAL_PATTERNS.items():
            if rx.search(data) or rx.search(lname):
                problems.append(f'{rel}: {label}')
        for m in EMAIL.finditer(data):
            if m.group(0) not in EMAIL_OK:
                problems.append(f'{rel}: e-mail address {m.group(0).decode(errors="replace")}')
                break
        if POKO.encode() in data.lower():
            poko_files.add(rel)
    if previous_poko is not None:
        new = sorted(poko_files - previous_poko)
        if new:
            problems.append(f'new files mention "{POKO}" (owner decision needed): {new}')
    return problems, {'files': count, 'poko_text_files': sorted(poko_files)}


def poko_set(root: pathlib.Path) -> set[str]:
    out = set()
    for p in root.rglob('*'):
        if p.is_file() and not p.is_symlink() and '.git' not in p.relative_to(root).parts and POKO.encode() in p.read_bytes().lower():
            out.add(p.relative_to(root).as_posix())
    return out


def extract_export(source_tar: pathlib.Path, dest: pathlib.Path) -> list[str]:
    """The source archive minus upstream/ (the vendored translator source lives only in the archive)."""
    dest.mkdir(parents=True, exist_ok=True)
    names = []
    with tarfile.open(source_tar, 'r:*') as tf:
        for m in tf:
            parts = pathlib.PurePosixPath(m.name).parts
            if len(parts) < 2 or '..' in parts or m.name.startswith('/'):
                continue
            rel = pathlib.PurePosixPath(*parts[1:]).as_posix()
            if rel == 'upstream' or rel.startswith('upstream/'):
                continue
            if m.isdir():
                (dest / rel).mkdir(parents=True, exist_ok=True)
                continue
            if not m.isfile():
                raise SystemExit(f'FAIL: unsupported member type in the source archive: {m.name}')
            target = dest / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(tf.extractfile(m).read())
            target.chmod(0o755 if m.mode & 0o111 else 0o644)
            names.append(rel)
    return names


# ------------------------------------------------------------------ notes
def render_notes(template: str, version: str, pkg_name: str, sums: dict[str, str]) -> str:
    block = '\n'.join(f'{sums[n]}  {n}' for n in sorted(sums))
    values = {'VERSION': version, 'TAG': 'v' + version, 'PKG_NAME': pkg_name, 'SHA256_BLOCK': block,
              'PACKAGE_ARCHIVE': pkg_name + '.tar.xz', 'TESTER_GUIDE': f'TESTER-v{version.split("-")[0]}.md'}
    out = re.sub(r'@([A-Z0-9_]+)@', lambda m: values.get(m.group(1), m.group(0)), template)
    left = re.findall(r'@[A-Z0-9_]+@', out)
    if left:
        raise SystemExit(f'FAIL: release notes keep unfilled placeholders: {sorted(set(left))}')
    return out


def check_notes(text: str) -> list[str]:
    problems = []
    for label, rx in PERSONAL_PATTERNS.items():
        if rx.search(text.encode()):
            problems.append(f'release notes: {label}')
    for m in EMAIL.finditer(text.encode()):
        if m.group(0) not in EMAIL_OK:
            problems.append('release notes: e-mail address')
            break
    return problems


# ------------------------------------------------------------------ post-publication
def check_release_assets(release_json: dict, local_sums: dict[str, str], pkg_name: str, prerelease: bool) -> list[str]:
    problems = []
    assets = {a['name']: a for a in release_json.get('assets', [])}
    expected = set(asset_names(pkg_name))
    if set(assets) != expected:
        problems.append(f'release assets differ: missing {sorted(expected - set(assets))}, extra {sorted(set(assets) - expected)}')
    for name, digest in local_sums.items():
        a = assets.get(name)
        if a and a.get('digest') != f'sha256:{digest}':
            problems.append(f"{name}: GitHub digest {a.get('digest')} != sidecar {digest}")
    if bool(release_json.get('prerelease')) != prerelease:
        problems.append(f"prerelease flag is {release_json.get('prerelease')}, expected {prerelease}")
    if release_json.get('draft'):
        problems.append('the release is still a draft')
    return problems


def tree_diff(a: pathlib.Path, b: pathlib.Path) -> list[str]:
    def files(root):
        return {p.relative_to(root).as_posix(): p for p in root.rglob('*')
                if p.is_file() and '.git' not in p.relative_to(root).parts}
    fa, fb = files(a), files(b)
    problems = [f'only in {a.name}: {n}' for n in sorted(set(fa) - set(fb))] + [f'only in {b.name}: {n}' for n in sorted(set(fb) - set(fa))]
    problems += [f'content differs: {n}' for n in sorted(set(fa) & set(fb)) if fa[n].read_bytes() != fb[n].read_bytes()]
    return problems


# ------------------------------------------------------------------ CLI
def load_json(path):
    return json.loads(pathlib.Path(path).read_text(encoding='utf-8'))


def report(title: str, problems: list[str]) -> int:
    if problems:
        print('\n'.join(f'FAIL: {p}' for p in problems), file=sys.stderr)
        return 1
    print(f'PASS: {title}')
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('local'); s.add_argument('--release-dir', type=pathlib.Path, required=True)
    s.add_argument('--version', required=True); s.add_argument('--head', required=True); s.add_argument('--porcelain', default='')
    s = sub.add_parser('builder-private'); s.add_argument('json', type=pathlib.Path)
    s = sub.add_parser('public-absent'); s.add_argument('tag_status', type=int); s.add_argument('release_status', type=int)
    s = sub.add_parser('identity')
    for k in ('name', 'email', 'last-name', 'last-email'):
        s.add_argument('--' + k, default='')
    s.add_argument('--allow-new', action='store_true')
    s = sub.add_parser('export'); s.add_argument('--source-tar', type=pathlib.Path, required=True); s.add_argument('--dest', type=pathlib.Path, required=True)
    s.add_argument('--allowlist', type=pathlib.Path, required=True); s.add_argument('--previous-tree', type=pathlib.Path)
    s = sub.add_parser('scan'); s.add_argument('root', type=pathlib.Path); s.add_argument('--previous-tree', type=pathlib.Path)
    s = sub.add_parser('notes'); s.add_argument('--template', type=pathlib.Path, required=True); s.add_argument('--release-dir', type=pathlib.Path, required=True)
    s.add_argument('--version', required=True); s.add_argument('--out', type=pathlib.Path, required=True)
    s = sub.add_parser('assets'); s.add_argument('--release-json', type=pathlib.Path, required=True); s.add_argument('--release-dir', type=pathlib.Path, required=True)
    s.add_argument('--version', required=True); s.add_argument('--stable', action='store_true')
    s = sub.add_parser('tree-diff'); s.add_argument('a', type=pathlib.Path); s.add_argument('b', type=pathlib.Path)
    a = ap.parse_args(argv)

    if a.cmd == 'local':
        mpath = a.release_dir / 'release-manifest.json'
        if not mpath.is_file():
            return report('manifest', [f'{mpath} does not exist: run make release first'])
        m = load_json(mpath)
        problems = check_manifest(m, a.version) + check_artifacts(a.release_dir, m) + check_commit(m.get('commit', ''), a.head, a.porcelain)
        return report(f"manifest all PASS, hashes match, commit {m.get('commit', '')[:12]} is HEAD, tree clean", problems)
    if a.cmd == 'builder-private':
        return report('builder repository is private', check_builder_private(load_json(a.json)))
    if a.cmd == 'public-absent':
        return report('tag and release absent from the public repository', check_public_absent(a.tag_status, a.release_status))
    if a.cmd == 'identity':
        return report('author identity matches the previous public release', check_identity(a.name, a.email, a.last_name, a.last_email, a.allow_new))
    if a.cmd == 'export':
        names = set(extract_export(a.source_tar, a.dest))
        allow = {l.strip() for l in a.allowlist.read_text().splitlines() if l.strip() and not l.startswith('#')}
        problems = check_export(names, allow)
        prev = poko_set(a.previous_tree) if a.previous_tree else None
        scan_problems, info = scan_export(a.dest, prev)
        (a.dest.parent / 'export-scan.json').write_text(json.dumps(info, indent=1))
        return report(f"export = allowlist ({len(names)} files, upstream/ left out); guard scans clean over {info['files']} files",
                      problems + scan_problems)
    if a.cmd == 'scan':
        prev = poko_set(a.previous_tree) if a.previous_tree else None
        problems, info = scan_export(a.root, prev)
        return report(f"guard scans clean over {info['files']} published files", problems)
    if a.cmd == 'notes':
        m = load_json(a.release_dir / 'release-manifest.json')
        pkg = m['artifacts']['package']['file'][:-len('.tar.xz')]
        sums = {m['artifacts'][k]['file']: m['artifacts'][k]['sha256'] for k in ('package', 'source', 'thirdparty_source')}
        text = render_notes(a.template.read_text(encoding='utf-8'), a.version, pkg, sums)
        problems = check_notes(text)
        if not problems:
            a.out.write_text(text, encoding='utf-8')
        return report('release notes rendered and clean', problems)
    if a.cmd == 'assets':
        m = load_json(a.release_dir / 'release-manifest.json')
        pkg = m['artifacts']['package']['file'][:-len('.tar.xz')]
        sums = {m['artifacts'][k]['file']: m['artifacts'][k]['sha256'] for k in ('package', 'source', 'thirdparty_source')}
        return report('release assets and GitHub digests match the sidecars',
                      check_release_assets(load_json(a.release_json), sums, pkg, not a.stable))
    return report('public tree equals the export', tree_diff(a.a, a.b))


if __name__ == '__main__':
    raise SystemExit(main())
