#!/usr/bin/env python3
"""Runtime-ABI descriptor and release-to-release compatibility gate.

The recompiled game (game.so) is built against one nxrt: it imports the runtime's rt_*/adapter_*
symbols by name, expects NXW_ABI_VERSION and the NxwGuestCpu layout, the guest address plan, and the
build-time takeover check (nxanalyze) reads the host-call registry. A new release whose runtime keeps
all of that compatible lets a tester reuse an existing build folder; anything else needs a rebuild.

Two independent views are extracted for every release and must agree for the new one:
  * binary  - `nxrt --abi-info` (compile-time constants + the compiled registry) and `nm -D` (exports);
  * source  - the generated registry (hle_registry.gen.c) and the headers in the corresponding-source
              archive. Releases that predate --abi-info are described from this view alone.

Rules (requires_rebuild = true when any applies):
  abi_version, sizeof(NxwGuestCpu), the guest address plan or the host-call slot capacities changed;
  a game-facing export (rt_*, adapter_*, nxw_*) was removed;
  the host-call registry changed in any way (name, kind or port state: the build-time takeover set).
Added exports and added registry entries are reported, and they still require a rebuild whenever they
change the registry hash (the build embeds the registry's host-bound set).
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile

SCHEMA = 1
SURFACE = re.compile(r'^(rt_|adapter_|nxw_)')
KINDS = {1: 'forced', 2: 'swhle', 3: 'nvn'}
PORTED = {0: 'stub', 1: 'ported', 2: 'trivial'}
KIND_BY_NAME = {'FORCED': 1, 'SWHLE': 2, 'NVN': 3}
PLAN_KEYS = ('token_base', 'token_size', 'token_stride', 'module_base', 'module_region_size', 'guest_as_base')
HARD_KEYS = ('abi_version', 'guest_cpu_size', 'token_capacity', 'stub_capacity') + PLAN_KEYS


def sha256_text(lines) -> str:
    return hashlib.sha256(('\n'.join(lines) + '\n').encode()).hexdigest()


def registry_summary(entries: list[list]) -> dict:
    """entries: [name, kind(1-3), ported(0-2)] in any order."""
    canon = sorted((n, int(k), int(p)) for n, k, p in entries)
    if len({n for n, _, _ in canon}) != len(canon):
        raise ValueError('duplicate host-call registry names')
    by_kind: dict[str, int] = {}
    by_state: dict[str, int] = {}
    for _, k, p in canon:
        by_kind[KINDS[k]] = by_kind.get(KINDS[k], 0) + 1
        by_state[PORTED[p]] = by_state.get(PORTED[p], 0) + 1
    return {
        'count': len(canon), 'by_kind': dict(sorted(by_kind.items())), 'by_state': dict(sorted(by_state.items())),
        'sha256': sha256_text(f'{n}\t{k}\t{p}' for n, k, p in canon),
        'entries': [[n, k, p] for n, k, p in canon],
    }


def exports_summary(names) -> dict:
    surface = sorted({n for n in names if SURFACE.match(n)})
    return {'prefixes': ['rt_', 'adapter_', 'nxw_'], 'total_dynamic_symbols': len(set(names)),
            'count': len(surface), 'sha256': sha256_text(surface), 'names': surface}


def parse_abi_info(text: str) -> dict:
    """Parse `nxrt --abi-info` output into constants + registry entries."""
    lines = text.splitlines()
    if not lines or lines[0].strip() != 'nxrt-abi-info 1':
        raise ValueError('not an nxrt --abi-info report (missing "nxrt-abi-info 1" header)')
    consts: dict[str, int] = {}
    entries: list[list] = []
    for line in lines[1:]:
        if not line.strip():
            continue
        if line.startswith('entry\t'):
            _, idx, name, kind, ported = line.split('\t')
            if int(idx) != len(entries):
                raise ValueError(f'registry index out of order at {idx}')
            entries.append([name, int(kind), int(ported)])
            continue
        key, value = line.split()
        consts[key] = int(value, 0)
    declared = consts.pop('registry_count', None)
    if declared != len(entries):
        raise ValueError(f'registry_count {declared} != {len(entries)} entry lines')
    missing = [k for k in HARD_KEYS if k not in consts]
    if missing:
        raise ValueError(f'--abi-info lacks {missing}')
    return {'constants': consts, 'entries': entries}


def _define(text: str, name: str) -> int:
    m = re.search(rf'#define\s+{name}\s+(0x[0-9a-fA-F]+|\d+)', text)
    if not m:
        raise ValueError(f'header lacks #define {name}')
    return int(m.group(1), 0)


def parse_source(files: dict[str, str]) -> dict:
    """The same facts from the corresponding-source files (a dict of repo-relative path -> text)."""
    def need(path):
        if path not in files:
            raise ValueError(f'source archive lacks {path}')
        return files[path]
    cpu = need('windows/src/abi/include/nxw/guest_cpu.h')
    rt = need('windows/src/nxrt/include/nxrt/rt_abi.h')
    hle = need('windows/src/nxrt/host/hle.c')
    reg = need('windows/src/nxrt/gen/out/hle_registry.gen.c')
    size = re.search(r'NXW_STATIC_ASSERT\(sizeof\(NxwGuestCpu\)\s*==\s*(\d+)', cpu)
    if not size:
        raise ValueError('guest_cpu.h lacks the NxwGuestCpu size assert')
    token_size = _define(rt, 'NXRT_TOKEN_SIZE')
    stride = _define(rt, 'NXRT_TOKEN_STRIDE')
    consts = {
        'abi_version': _define(cpu, 'NXW_ABI_VERSION'), 'guest_cpu_size': int(size.group(1)),
        'token_base': _define(rt, 'NXRT_TOKEN_BASE'), 'token_size': token_size, 'token_stride': stride,
        'module_base': _define(rt, 'NXRT_MODULE_BASE'), 'module_region_size': _define(rt, 'NXRT_MODULE_REGION_SIZE'),
        'guest_as_base': _define(rt, 'NXRT_GUEST_AS_BASE'),
        'token_capacity': token_size // stride, 'stub_capacity': _define(hle, 'MAX_STUB'),
    }
    pat = re.compile(r'^\s*\{"([^"]+)",\s*NXRT_HLE_(FORCED|SWHLE|NVN),\s*(\d+),', re.M)
    entries = [[m.group(1), KIND_BY_NAME[m.group(2)], int(m.group(3))] for m in pat.finditer(reg)]
    if not entries:
        raise ValueError('hle_registry.gen.c has no registry entries')
    return {'constants': consts, 'entries': entries}


SOURCE_FILES = ('windows/src/abi/include/nxw/guest_cpu.h', 'windows/src/nxrt/include/nxrt/rt_abi.h',
                'windows/src/nxrt/host/hle.c', 'windows/src/nxrt/gen/out/hle_registry.gen.c')


def read_source_files(source: pathlib.Path) -> dict[str, str]:
    """From a corresponding-source tarball (any top-level folder), a directory, or a package tarball's nested source archive."""
    if source.is_dir():
        return {f: (source / f).read_text(encoding='utf-8') for f in SOURCE_FILES if (source / f).is_file()}
    out: dict[str, str] = {}
    with tarfile.open(source, 'r:*') as tf:
        for member in tf:
            if not member.isfile():
                continue
            parts = pathlib.PurePosixPath(member.name).parts
            rel = pathlib.PurePosixPath(*parts[1:]).as_posix() if len(parts) > 1 else member.name
            if rel in SOURCE_FILES:
                out[rel] = tf.extractfile(member).read().decode('utf-8')
    return out


def nm_exports(nxrt: pathlib.Path) -> list[str]:
    res = subprocess.run(['nm', '-D', '--defined-only', str(nxrt)], capture_output=True, text=True, check=True)
    return [line.split()[-1] for line in res.stdout.splitlines() if line.strip()]


def build_descriptor(parsed: dict, exports: list[str], origin: str) -> dict:
    c = parsed['constants']
    return {
        'schema': SCHEMA, 'origin': origin,
        'abi_version': c['abi_version'], 'guest_cpu_size': c['guest_cpu_size'],
        'address_plan': {k: c[k] for k in PLAN_KEYS},
        'host_call_slots': {'registry_entries': len(parsed['entries']), 'token_capacity': c['token_capacity'],
                            'stub_capacity': c['stub_capacity']},
        'registry': registry_summary(parsed['entries']),
        'exports': exports_summary(exports),
    }


def _comparable(d: dict) -> dict:
    plan = d['address_plan']
    return {'abi_version': d['abi_version'], 'guest_cpu_size': d['guest_cpu_size'],
            **{k: plan[k] for k in PLAN_KEYS}, 'token_capacity': d['host_call_slots']['token_capacity'],
            'stub_capacity': d['host_call_slots']['stub_capacity']}


def describe(nxrt: pathlib.Path, abi_info: pathlib.Path, source: pathlib.Path) -> dict:
    """The descriptor of a new build; the binary's own report must agree with the source archive."""
    exports = nm_exports(nxrt)
    binary = build_descriptor(parse_abi_info(abi_info.read_text(encoding='utf-8')), exports, 'binary')
    src = build_descriptor(parse_source(read_source_files(source)), exports, 'source')
    problems = []
    if _comparable(binary) != _comparable(src):
        diff = {k: (v, _comparable(src)[k]) for k, v in _comparable(binary).items() if v != _comparable(src)[k]}
        problems.append(f'constants differ between nxrt --abi-info and the source archive (binary, source): {diff}')
    if binary['registry']['sha256'] != src['registry']['sha256']:
        a = {tuple(e) for e in binary['registry']['entries']}
        b = {tuple(e) for e in src['registry']['entries']}
        problems.append('host-call registry differs between nxrt --abi-info and hle_registry.gen.c: '
                        f'{len(a - b)} only in binary, {len(b - a)} only in source (e.g. {sorted(a ^ b)[:3]})')
    if problems:
        raise SystemExit('FAIL: runtime ABI self-check:\n  ' + '\n  '.join(problems))
    binary['self_check'] = 'binary and source views agree'
    return binary


def baseline_from_package(package: pathlib.Path) -> dict:
    """Describe an existing release (no --abi-info needed): its nxrt exports plus the nested/accompanying source archive."""
    with tempfile.TemporaryDirectory(prefix='abi-baseline-') as tmp:
        tmp_path = pathlib.Path(tmp)
        if package.is_dir():
            nxrt = package / 'builder/bin/nxrt'
            nested = next((p for p in sorted(package.glob('*-source.tar.xz')) if 'thirdparty' not in p.name), None)
            files = read_source_files(nested)
        else:
            nxrt = tmp_path / 'nxrt'
            files = {}
            with tarfile.open(package, 'r:*') as tf:
                for member in tf:
                    if not member.isfile():
                        continue
                    if member.name.endswith('/builder/bin/nxrt'):
                        nxrt.write_bytes(tf.extractfile(member).read())
                    elif member.name.endswith('-source.tar.xz') and 'thirdparty' not in member.name and member.name.count('/') == 1:
                        nested_bytes = tf.extractfile(member).read()
                        nested_path = tmp_path / 'nested-source.tar.xz'
                        nested_path.write_bytes(nested_bytes)
                        files = read_source_files(nested_path)
        if not nxrt.is_file() or not files:
            raise SystemExit(f'FAIL: cannot read nxrt and source archive from {package}')
        return build_descriptor(parse_source(files), nm_exports(nxrt), 'source+exports (release predates --abi-info)')


def load_descriptor(path: pathlib.Path) -> dict:
    """A descriptor JSON, a release manifest (its .abi.json sidecar is used), or a package tarball/directory."""
    if path.is_dir() or path.name.endswith(('.tar.xz', '.tar.gz', '.tar')):
        return baseline_from_package(path)
    data = json.loads(path.read_text(encoding='utf-8'))
    if 'abi' in data and 'registry' not in data:   # a release-manifest.json
        sidecar = path.parent / data['abi'].get('file', '')
        if data['abi'].get('file') and sidecar.is_file():
            digest = hashlib.sha256(sidecar.read_bytes()).hexdigest()
            if digest != data['abi'].get('file_sha256'):
                raise SystemExit(f'FAIL: ABI sidecar {sidecar} does not match its manifest hash')
            return json.loads(sidecar.read_text(encoding='utf-8'))
        raise SystemExit(f'FAIL: manifest {path} has no readable ABI sidecar; pass the package tarball or the .abi.json')
    return data


def compare(prev: dict, new: dict) -> dict:
    reasons: list[str] = []
    old_c, new_c = _comparable(prev), _comparable(new)
    for key in HARD_KEYS:
        if old_c[key] != new_c[key]:
            reasons.append(f'{key}: {old_c[key]} -> {new_c[key]}')
    old_exp, new_exp = set(prev['exports']['names']), set(new['exports']['names'])
    removed_exports = sorted(old_exp - new_exp)
    added_exports = sorted(new_exp - old_exp)
    if removed_exports:
        reasons.append(f'{len(removed_exports)} game-facing exports removed (e.g. {removed_exports[:3]})')
    old_reg = {e[0]: tuple(e[1:]) for e in prev['registry']['entries']}
    new_reg = {e[0]: tuple(e[1:]) for e in new['registry']['entries']}
    reg_added = sorted(set(new_reg) - set(old_reg))
    reg_removed = sorted(set(old_reg) - set(new_reg))
    reg_changed = sorted(n for n in set(old_reg) & set(new_reg) if old_reg[n] != new_reg[n])
    if prev['registry']['sha256'] != new['registry']['sha256']:
        reasons.append(f'host-call registry changed: +{len(reg_added)} -{len(reg_removed)} ~{len(reg_changed)} entries')
    return {
        'requires_rebuild': bool(reasons), 'reasons': reasons,
        'previous': {'origin': prev.get('origin'), 'abi_version': prev['abi_version'],
                     'registry_sha256': prev['registry']['sha256'], 'exports_sha256': prev['exports']['sha256'],
                     'registry_count': prev['registry']['count'], 'exports_count': prev['exports']['count']},
        'new': {'origin': new.get('origin'), 'abi_version': new['abi_version'],
                'registry_sha256': new['registry']['sha256'], 'exports_sha256': new['exports']['sha256'],
                'registry_count': new['registry']['count'], 'exports_count': new['exports']['count']},
        'exports_removed': removed_exports, 'exports_added': added_exports,
        'registry_added': reg_added, 'registry_removed': reg_removed, 'registry_changed': reg_changed,
    }


def summary(d: dict) -> dict:
    """The part of a descriptor kept inside release-manifest.json (the lists live in the .abi.json sidecar)."""
    return {
        'schema': d['schema'], 'origin': d['origin'], 'abi_version': d['abi_version'], 'guest_cpu_size': d['guest_cpu_size'],
        'address_plan': d['address_plan'], 'host_call_slots': d['host_call_slots'],
        'registry': {k: v for k, v in d['registry'].items() if k != 'entries'},
        'exports': {k: v for k, v in d['exports'].items() if k != 'names'},
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('describe', help='descriptor of a new build (self-checked binary vs source)')
    d.add_argument('--nxrt', type=pathlib.Path, required=True)
    d.add_argument('--abi-info', type=pathlib.Path, required=True)
    d.add_argument('--source', type=pathlib.Path, required=True)
    d.add_argument('--out', type=pathlib.Path, required=True)
    b = sub.add_parser('baseline', help='descriptor of an existing release package (tarball or directory)')
    b.add_argument('--package', type=pathlib.Path, required=True)
    b.add_argument('--out', type=pathlib.Path, required=True)
    c = sub.add_parser('compare', help='previous vs new; exit 0 compatible, 3 rebuild required and not allowed')
    c.add_argument('--prev', type=pathlib.Path, required=True, help='descriptor/.abi.json, manifest, or package tarball/dir')
    c.add_argument('--new', type=pathlib.Path, required=True)
    c.add_argument('--allow-break', action='store_true')
    c.add_argument('--out', type=pathlib.Path, required=True)
    args = ap.parse_args(argv)
    if args.cmd == 'describe':
        args.out.write_text(json.dumps(describe(args.nxrt, args.abi_info, args.source), indent=1, sort_keys=True) + '\n')
        return 0
    if args.cmd == 'baseline':
        args.out.write_text(json.dumps(baseline_from_package(args.package), indent=1, sort_keys=True) + '\n')
        return 0
    result = compare(load_descriptor(args.prev), json.loads(args.new.read_text(encoding='utf-8')))
    result['abi_break_allowed'] = bool(args.allow_break)
    args.out.write_text(json.dumps(result, indent=1, sort_keys=True) + '\n')
    print(f"requires_rebuild={str(result['requires_rebuild']).lower()}")
    for reason in result['reasons']:
        print(f'  - {reason}')
    if result['requires_rebuild'] and not args.allow_break:
        print('FAIL: runtime ABI break; rerun with ALLOW_ABI_BREAK=1 to promote deliberately', file=sys.stderr)
        return 3
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
