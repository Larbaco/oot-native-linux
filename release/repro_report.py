#!/usr/bin/env python3
"""Compare two independent pipeline builds of the same commit and report what differs.
Reproducibility is reported, never enforced: this exits 0 whether or not the builds are bit-identical."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib

HINTS = (
    ('python/lib/libpython', 'CPython build: object/link order or embedded build metadata'),
    ('python/', 'embedded CPython tree (build-time metadata such as _sysconfigdata, pyc, or link output)'),
    ('oot-native-launcher', 'PyInstaller one-file archive (bundled library copies, zlib stream, bytecode)'),
    ('dotnet/', 'pinned .NET runtime tarball (should be identical; a difference means the pin is not enforced)'),
    ('builder/bin/dmsh_xlate', 'managed assemblies: NuGet restore or compiler metadata'),
    ('builder/bin/', 'native tools built by clang/ninja'),
    ('LICENSES/', 'license text copied from the image or a package metadata file'),
)


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def sums(path: pathlib.Path) -> dict[str, str]:
    out = {}
    for line in path.read_text().splitlines():
        digest, name = line.split(None, 1)
        out[name.lstrip('*')] = digest
    return out


def hint(name: str) -> str:
    n = name[2:] if name.startswith('./') else name
    return next((h for p, h in HINTS if n.startswith(p)), 'unclassified')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--a', type=pathlib.Path, required=True)
    ap.add_argument('--b', type=pathlib.Path, required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--pkg-name', required=True)
    ap.add_argument('--out', type=pathlib.Path, required=True)
    ap.add_argument('--epoch', required=True)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    result = {'version': a.version, 'source_date_epoch': int(a.epoch), 'artifacts': {}, 'differing_files': [], 'identical': True}
    for label, suffix in (('package', '.tar.xz'), ('source', '-source.tar.xz')):
        fa, fb = a.a / (a.pkg_name + suffix), a.b / (a.pkg_name + suffix)
        sa, sb = sha256_file(fa), sha256_file(fb)
        result['artifacts'][label] = {'build_a': sa, 'build_b': sb, 'bytes_a': fa.stat().st_size, 'bytes_b': fb.stat().st_size, 'identical': sa == sb}
        result['identical'] &= sa == sb
    ma, mb = sums(a.a / 'out' / a.pkg_name / 'SHA256SUMS'), sums(a.b / 'out' / a.pkg_name / 'SHA256SUMS')
    names = sorted(set(ma) | set(mb))
    result['package_files'] = {'total': len(names), 'identical': sum(ma.get(n) == mb.get(n) for n in names)}
    for n in names:
        if ma.get(n) != mb.get(n):
            result['differing_files'].append({'file': n, 'a': ma.get(n), 'b': mb.get(n), 'likely_cause': hint(n)})
    (a.out / 'repro-report.json').write_text(json.dumps(result, indent=1) + '\n')
    md = [f'# Reproducibility report v{a.version}', '',
          f"Two independent builds of the same commit, SOURCE_DATE_EPOCH={a.epoch}.", '']
    for k, v in result['artifacts'].items():
        md.append(f"- {k} tarball: {'IDENTICAL' if v['identical'] else 'DIFFERENT'} (A {v['build_a'][:16]}…, B {v['build_b'][:16]}…)")
    md.append(f"- package files: {result['package_files']['identical']}/{result['package_files']['total']} identical")
    if result['differing_files']:
        md += ['', '## Differing files', '']
        md += [f"- `{d['file']}`: {d['likely_cause']}" for d in result['differing_files']]
    (a.out / 'repro-report.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
