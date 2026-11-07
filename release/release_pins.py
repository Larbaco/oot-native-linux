#!/usr/bin/env python3
"""Read release/pins.env and fetch/verify the pinned downloads. Fails closed: a missing, truncated or
substituted file is deleted and reported, never used."""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys

PINS = pathlib.Path(__file__).resolve().parent / 'pins.env'


def load(path: pathlib.Path = PINS) -> dict[str, str]:
    out: dict[str, str] = {}
    for n, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        m = re.fullmatch(r'([A-Z][A-Z0-9_]*)=(\S*)', line)
        if not m:
            raise ValueError(f'{path}:{n}: not KEY=VALUE without spaces: {line!r}')
        if m.group(1) in out:
            raise ValueError(f'{path}:{n}: duplicate key {m.group(1)}')
        out[m.group(1)] = m.group(2)
    return out


def downloads(pins: dict[str, str]) -> list[tuple[str, str, str]]:
    """[(file, url, sha256)] for every DL_<KEY>_FILE."""
    items = []
    for key in sorted(k for k in pins if k.startswith('DL_') and k.endswith('_FILE')):
        base = key[:-5]
        url, sha = pins.get(base + '_URL', ''), pins.get(base + '_SHA256', '')
        if not url or not re.fullmatch(r'[0-9a-f]{64}', sha):
            raise ValueError(f'{base}: needs _URL and a 64-hex _SHA256 in pins.env')
        if not url.startswith('https://') or '/' in pins[key]:
            raise ValueError(f'{base}: https URL and a plain file name are required')
        items.append((pins[key], url, sha))
    return items


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def ensure(cache: pathlib.Path, pins: dict[str, str]) -> dict[str, str]:
    cache.mkdir(parents=True, exist_ok=True)
    result = {}
    for name, url, sha in downloads(pins):
        target = cache / name
        if target.is_file() and sha256_file(target) != sha:
            print(f'FAIL: cached {name} does not match its pinned SHA-256; removing it', file=sys.stderr)
            target.unlink()
        if not target.is_file():
            part = cache / (name + '.part')
            print(f'fetching {name}')
            subprocess.run(['curl', '-fL', '--retry', '3', '--proto', '=https', '-o', str(part), url], check=True)
            if sha256_file(part) != sha:
                got = sha256_file(part)
                part.unlink()
                raise SystemExit(f'FAIL: {name} downloaded from {url} has SHA-256 {got}, pinned {sha}')
            part.rename(target)
        result[name] = sha
        print(f'ok   {sha}  {name}')
    return result


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=('fetch', 'get'))
    ap.add_argument('--cache', type=pathlib.Path)
    ap.add_argument('key', nargs='?')
    args = ap.parse_args(argv)
    pins = load()
    if args.cmd == 'get':
        print(pins[args.key])
        return 0
    ensure(args.cache, pins)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
