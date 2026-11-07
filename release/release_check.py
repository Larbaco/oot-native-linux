#!/usr/bin/env python3
"""Fail-closed scanner for the source and binary release trees."""
from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tarfile

GAME_SUFFIXES = {'.nup', '.nsp', '.nsz', '.xci', '.nca', '.nso', '.nro', '.npdm', '.bfsha', '.bnsh', '.bntx', '.bfres', '.zs', '.szs', '.sarc', '.spv', '.ctl', '.db', '.img', '.nacp', '.tik', '.cert', '.sav', '.ppm', '.wav', '.ogg', '.opus', '.bwav', '.bfttf', '.otf', '.png', '.jpg', '.jpeg', '.mp4', '.gif', '.ico', '.icns'}
SOURCE_BINARY_SUFFIXES = {'.exe', '.dll', '.pdb', '.so', '.o', '.a', '.bin', '.ttf'}
PACKAGE_BINARY_SUFFIXES = {'.exe', '.pdb'}
DENY_NAMES = {'game.so', 'program.db', 'prod.keys', 'title.keys'}
PERSONAL = (b'/' + b'home/', b'/' + b'mnt/', b'jo' + b'gos/', b'lar' + b'baco', b'thia' + b'goo', b'@' + b'gmail.com', b'192.' + b'168.', b'c:\\users\\')
# Public author contact details belong to the corresponding upstream notices and
# Python runtime sources, including CPython modules packed inside the PyInstaller
# executable. These paths are exempted only for email addresses; owner identifiers
# and machine paths remain scanned everywhere.
PUBLIC_AUTHOR_EMAIL_PATHS = (
    'LICENSES/', 'THIRD-PARTY-NOTICES.md', 'release/THIRD-PARTY-NOTICES.md',
    'dotnet/ThirdPartyNotices.txt', 'LICENSES/dotnet-ThirdPartyNotices.txt',
    # Public upstream attribution strings in the pinned CPython shared library
    # and the exact third-party runtime files which carry their authors' contact
    # details. Keep this list path-specific; personal identifiers stay scanned.
    'python/lib/libpython3.12.so.1.0',
    'python/lib/python3.12/site-packages/zstandard-0.23.0.dist-info/METADATA',
    'python/lib/python3.12/site-packages/lz4-4.3.3.dist-info/METADATA',
    'python/lib/python3.12/site-packages/PIL/IcoImagePlugin.py',
    'python/lib/python3.12/site-packages/PIL/ImageMorph.py',
)
# CPython's canonical RFC1918 example is not user data. Keep that single
# standard-library literal intact while scanning all other private IPv4 values.
PYTHON_PRIVATE_RANGE_EXAMPLES = {
    'python/lib/python3.12/ipaddress.py',
    'python/lib/python3.12/urllib/request.py',
}
OTHER_TITLE = b'0400a3a0' + b'1ee4c000'
MAGIC = (b'NSO0', b'NRO0', b'NCA3', b'PFS0', b'HFS0', b'BNTX', b'FRES', b'SARC', b'Yaz0', b'BFSH', b'FSHA', b'RIFF', b'\x03\x02#\x07')
HEX_DUMP = re.compile(rb'^[ \t]*(?:0x)?[0-9a-fA-F]{2,16}(?:[ \t,]+(?:0x)?[0-9a-fA-F]{2,16}){23,},?[ \t]*$')
# Upstream codec implementations contain fixed algorithm lookup tables, not captured game data.
HEX_TABLE_SOURCES = {
    'windows/src/nxrt/vk/third_party/bc7enc/bc7enc.cpp',
    'windows/src/nxrt/vk/third_party/astcenc/astcenc_quantization.cpp',
}


def _scan_one(name: str, data: bytes, allowed: set[str], mode: str) -> list[str]:
    errors: list[str] = []
    p = pathlib.PurePosixPath(name)
    binary_suffixes = SOURCE_BINARY_SUFFIXES if mode == 'source' else PACKAGE_BINARY_SUFFIXES
    if name not in allowed:
        errors.append(f'{name}: not in allowlist')
    if p.name.lower() in {x.lower() for x in DENY_NAMES} or p.suffix.lower() in GAME_SUFFIXES | binary_suffixes:
        errors.append(f'{name}: forbidden filename/type')
    if name.startswith('LICENSES/') and p.suffix.lower() == '.pyc':
        errors.append(f'{name}: compiled Python cache is not a license text')
    lowered_name = name.lower().encode('utf-8', 'replace')
    lower = data.lower()
    if OTHER_TITLE in lowered_name or OTHER_TITLE in lower:
        errors.append(f'{name}: other-game title/profile identifier')
    if any(part.lower() in {'exefs', 'romfs', 'extract', 'logs'} for part in pathlib.PurePosixPath(name).parts):
        errors.append(f'{name}: forbidden dump/build-data path')
    license_notice_name = p.name.lower().startswith(('license', 'notice', 'copying'))
    python_stdlib = name.startswith('python/lib/python3.12/') and not name.startswith('python/lib/python3.12/site-packages/')
    embedded_python_launcher = name == 'oot-native-launcher' and data.startswith(b'\x7fELF')
    exempt_public_email = (
        name.startswith(PUBLIC_AUTHOR_EMAIL_PATHS) or license_notice_name or
        python_stdlib or embedded_python_launcher
    )
    exempt_stdlib_private_range = name in PYTHON_PRIVATE_RANGE_EXAMPLES
    personal_tokens = tuple(token for token in PERSONAL if not (
        (token == b'@' + b'gmail.com' and exempt_public_email) or
        (token == b'192.' + b'168.' and exempt_stdlib_private_range)
    ))
    if any(token in lowered_name or token in lower for token in personal_tokens):
        errors.append(f'{name}: personal path, identifier, or address')
    is_text = b'\0' not in data[:4096]
    is_native_executable = mode == 'package' and (data.startswith(b'\x7fELF') or data.startswith(b'MZ'))
    if not is_text and not is_native_executable:
        for marker in MAGIC:
            if data.startswith(marker):
                errors.append(f'{name}: forbidden content signature {marker!r}')
                break
    if name not in HEX_TABLE_SOURCES and any(HEX_DUMP.fullmatch(line) for line in data.splitlines()):
        errors.append(f'{name}: looks like a large byte dump')
    if p.suffix.lower() == '.tsv' and len(data) > 1_000_000:
        errors.append(f'{name}: TSV exceeds 1 MB')
    return errors


def scan_items(items: dict[str, bytes], allowed: set[str], mode: str = 'source') -> list[str]:
    if mode not in {'source', 'package'}:
        raise ValueError(f'unknown release-check mode: {mode}')
    errors: list[str] = []
    for name, data in items.items():
        errors.extend(_scan_one(name, data, allowed, mode))
    for missing in sorted(allowed - items.keys()):
        errors.append(f'{missing}: allowlisted file is missing')
    return errors


def iter_tree(root: pathlib.Path):
    for p in root.rglob('*'):
        if p.is_symlink():
            relative = p.relative_to(root).as_posix()
            target = pathlib.PurePosixPath(p.readlink().as_posix())
            resolved = pathlib.PurePosixPath(relative).parent.joinpath(target)
            stack = []
            for part in resolved.parts:
                if part in ('', '.'):
                    continue
                if part == '..':
                    if not stack:
                        raise ValueError(f'unsafe symlink target: {relative} -> {target}')
                    stack.pop()
                else:
                    stack.append(part)
            yield relative, b'SYMLINK:' + target.as_posix().encode('utf-8')
        elif p.is_file():
            yield p.relative_to(root).as_posix(), p.read_bytes()


def iter_tar(path: pathlib.Path):
    with tarfile.open(path, 'r:*') as tf:
        members = tf.getmembers()
        for member in members:
            if member.uid != 0 or member.gid != 0 or member.uname not in ('', 'root') or member.gname not in ('', 'root'):
                raise ValueError(
                    f'non-root tar metadata: {member.name}: '
                    f'uid={member.uid} gid={member.gid} uname={member.uname!r} gname={member.gname!r}'
                )
        files = [m for m in members if m.isfile()]
        roots = {pathlib.PurePosixPath(m.name).parts[0] for m in files if pathlib.PurePosixPath(m.name).parts}
        strip_root = next(iter(roots)) if len(roots) == 1 else None
        for member in members:
            if member.name.startswith('/') or '..' in pathlib.PurePosixPath(member.name).parts:
                raise ValueError(f'unsafe archive path: {member.name}')
            if member.isdir(): continue
            if member.issym():
                target = pathlib.PurePosixPath(member.linkname)
                if target.is_absolute(): raise ValueError(f'unsafe absolute symlink: {member.name}')
                resolved = pathlib.PurePosixPath(member.name).parent.joinpath(target)
                stack = []
                for part in resolved.parts:
                    if part in ('', '.'): continue
                    if part == '..':
                        if not stack: raise ValueError(f'unsafe symlink target: {member.name} -> {target}')
                        stack.pop()
                    else: stack.append(part)
                name = member.name
                if strip_root:
                    parts = pathlib.PurePosixPath(name).parts
                    name = pathlib.PurePosixPath(*parts[1:]).as_posix()
                if name: yield name, b'SYMLINK:' + member.linkname.encode('utf-8')
                continue
            if not member.isfile(): raise ValueError(f'unsupported archive member type: {member.name}')
            f = tf.extractfile(member)
            if f is None: raise ValueError(f'cannot read archive member: {member.name}')
            name = member.name
            if strip_root:
                parts = pathlib.PurePosixPath(name).parts
                name = pathlib.PurePosixPath(*parts[1:]).as_posix()
            if name: yield name, f.read()


def load_allowlist(path: pathlib.Path) -> set[str]:
    return {line.strip() for line in path.read_text(encoding='utf-8').splitlines() if line.strip() and not line.lstrip().startswith('#')}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('target', type=pathlib.Path, help='staging directory or tar archive')
    ap.add_argument('--allowlist', required=True, type=pathlib.Path)
    ap.add_argument('--mode', choices=('source', 'package'), default='source')
    args = ap.parse_args()
    try:
        allowed = load_allowlist(args.allowlist)
        items = iter_tree(args.target) if args.target.is_dir() else iter_tar(args.target)
        errors = []
        seen = set()
        for name, data in items:
            seen.add(name)
            errors.extend(_scan_one(name, data, allowed, args.mode))
        errors.extend(f'{missing}: allowlisted file is missing' for missing in sorted(allowed - seen))
    except (OSError, ValueError, tarfile.TarError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr); return 2
    if errors:
        print('\n'.join(f'FAIL: {e}' for e in errors), file=sys.stderr); return 1
    print(f'PASS: {len(seen)} files match allowlist and content checks')
    return 0


if __name__ == '__main__': raise SystemExit(main())
