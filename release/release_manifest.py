#!/usr/bin/env python3
"""Gate evidence and release-manifest.json for release/pipeline.sh.

check-smoke   every required PASS line must be present in the smoke log (a missing line is a FAIL, not a skip)
check-ctest   the CTest summary must show zero failures and the Vulkan loader test passed
manifest      assemble release-manifest.json from the run's facts and gate results
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

# (id, regex of the PASS line the packaged smoke must print)
SMOKE_REQUIRED = [
    ('package-sha256sums', r'^PASS: package internal SHA256SUMS'),
    ('vulkan-2b-no-vk-exports', r'^PASS: nxrt exports no vk\[A-Z\]\* dynamic symbols'),
    ('vulkan-2b-strings', r'^PASS: nxrt contains libvulkan\.so\.1 and internal \[vk\] backend strings'),
    ('vulkan-2a-backend', r'^PASS: candidate nxrt has Vulkan backend; no ExeFS/game was loaded'),
    ('vulkan-2e-reference', r'^PASS: previous-release-reference nxrt has Vulkan backend; no ExeFS/game was loaded'),
    ('movie-ldd-from-package', r'^PASS: nxrt resolves libavcodec and libdav1d from the package'),
    ('movie-capability', r'^PASS: nxrt reports built-in AV1 decoder capability without loading ExeFS'),
    ('movie-synthetic-av1', r'^PASS: packaged nxrt-movie-decode decoded synthetic AV1 and observed EOS'),
    ('vulkan-2d-ldd-nxrt', r'^PASS: ldd resolved all nxrt dependencies'),
    ('vulkan-2c-loader-probe', r'^PASS: Vulkan LD_PRELOAD constructor probe completed'),
    ('abi-info', r'^PASS: nxrt --abi-info reports the runtime ABI'),
    ('python-licenses', r'^PASS: CPython embedded and dynamically linked library license notices are present'),
    ('launcher-help', r'^PASS: packaged launcher --help'),
    ('release-doctor', r'^PASS: packaged release doctor'),
    ('builder-imports', r'^PASS: imported packaged builder modules'),
    ('builder-preflight', r'^PASS: package build preflight'),
    ('invalid-dump-lang-en', r"^PASS: --lang en rejects a synthetic invalid \.nup"),
    ('invalid-dump-lang-ptbr', r"^PASS: --lang pt-BR rejects a synthetic invalid \.nup"),
    ('invalid-dump-env-en', r"^PASS: OOT_NATIVE_LANG=en rejects a synthetic invalid \.nup"),
    ('invalid-dump-env-ptbr', r"^PASS: OOT_NATIVE_LANG=pt-BR rejects a synthetic invalid \.nup"),
    ('check-dump-json-code', r'^PASS: check-dump --json returns the same stable code'),
    ('gui-english-default', r'^PASS: launcher GUI defaults to English'),
    ('zbic-file-mode', r'^PASS: zbic-unpack normalizes a synthetic NSO'),
    ('ldd-all-elf', r'^PASS: ldd resolved all \d+ package and PyInstaller-embedded ELF files'),
    ('translator-no-icu', r'^PASS: packaged dmsh_xlate starts without system ICU'),
    ('smoke-complete', r'^PASS: complete packaged smoke finished$'),
]
REFERENCE_SKIP = 'SKIP: previous-release-reference'


def check_smoke(log: pathlib.Path, allow_no_reference: bool) -> list[dict]:
    lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
    found, problems = [], []
    for gate_id, pattern in SMOKE_REQUIRED:
        rx = re.compile(pattern)
        hit = next((l for l in lines if rx.search(l)), None)
        if hit is None and gate_id == 'vulkan-2e-reference' and allow_no_reference and any(l.startswith(REFERENCE_SKIP) for l in lines):
            found.append({'id': gate_id, 'status': 'SKIPPED', 'line': REFERENCE_SKIP + ' (no previous release)'})
        elif hit is None:
            problems.append(gate_id)
            found.append({'id': gate_id, 'status': 'FAIL', 'line': ''})
        else:
            found.append({'id': gate_id, 'status': 'PASS', 'line': hit})
    if any(l.startswith('FAIL') for l in lines):
        problems.append('a FAIL line is present in the smoke log')
    if problems:
        raise SystemExit('FAIL: required smoke gates missing: ' + ', '.join(problems))
    return found


def check_ctest(log: pathlib.Path) -> dict:
    text = log.read_text(encoding='utf-8', errors='replace')
    m = re.search(r'(\d+)% tests passed, (\d+) tests failed out of (\d+)', text)
    if not m or int(m.group(2)) != 0:
        raise SystemExit(f'FAIL: CTest did not finish cleanly ({m.group(0) if m else "no summary line"})')
    vk = re.search(r'Test\s+#\d+:\s+nxrt\.vulkan_loader_interposition\s+\.+\s*Passed', text)
    if not vk:
        raise SystemExit('FAIL: CTest nxrt.vulkan_loader_interposition did not pass (or did not run)')
    skipped = len(re.findall(r'\*\*\*Skipped|Not Run', text))
    return {'total': int(m.group(3)), 'failed': 0, 'skipped': skipped, 'vulkan_loader_interposition': 'Passed'}


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def artifact(path: pathlib.Path) -> dict:
    return {'file': path.name, 'bytes': path.stat().st_size, 'sha256': sha256_file(path)}


def sums_index(staging: pathlib.Path) -> dict[str, str]:
    out = {}
    for line in (staging / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split(None, 1)
        out[name.lstrip('*')] = digest
    return out


def build_manifest(a) -> dict:
    ctx = json.loads(pathlib.Path(a.context).read_text())
    gates = [json.loads(l) for l in pathlib.Path(a.gates).read_text().splitlines() if l.strip()]
    sums = sums_index(a.staging)
    key_files = ['./builder/bin/nxrt', './builder/bin/nxrt-movie-decode', './builder/lib/libavcodec.so.61', './builder/lib/libdav1d.so.7', './builder/bin/nxlift', './builder/bin/nxanalyze', './builder/bin/zbic-unpack',
                 './builder/bin/ld.lld', './builder/bin/dmsh_xlate.dll', './builder/lib/libSDL3.so.0',
                 './builder/ampere-lower/ampere-lower', './oot-native-launcher']
    abi_result = json.loads(pathlib.Path(a.abi_result).read_text())
    abi_desc = json.loads(pathlib.Path(a.abi_desc).read_text())
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import release_abi
    manifest = {
        'schema': 1, 'name': 'oot-native', 'version': ctx['version'], 'tag': 'v' + ctx['version'],
        'commit': ctx['commit'], 'branch': ctx['branch'], 'tree_clean': True,
        'source_date_epoch': ctx['source_date_epoch'], 'source_date_utc': ctx['source_date_utc'],
        'release_date': ctx['release_date'],
        'builder_image': ctx['builder_image'], 'smoke_image': ctx['smoke_image'],
        'pinned_inputs': ctx['pinned_inputs'],
        'artifacts': {
            'package': artifact(pathlib.Path(a.package)), 'source': artifact(pathlib.Path(a.source)), 'thirdparty_source': artifact(pathlib.Path(a.thirdparty)),
            'package_sha256sums_sha256': sha256_file(a.staging / 'SHA256SUMS'),
            'package_files': len(sums),
            'key_files': {k[2:]: sums[k] for k in key_files if k in sums},
        },
        'gates': gates,
        'smoke_gates': json.loads(pathlib.Path(a.smoke_gates).read_text()),
        'ctest': json.loads(pathlib.Path(a.ctest).read_text()),
        'abi': {
            **release_abi.summary(abi_desc), 'file': pathlib.Path(a.abi_file).name, 'file_sha256': sha256_file(pathlib.Path(a.abi_file)),
            'baseline': ctx['abi_baseline'], 'comparison': {k: abi_result[k] for k in (
                'previous', 'new', 'reasons', 'exports_added', 'exports_removed', 'registry_added', 'registry_removed', 'registry_changed')},
            'abi_break_allowed': abi_result['abi_break_allowed'],
        },
        'requires_rebuild': abi_result['requires_rebuild'],
        'reproducibility': ctx.get('reproducibility', {'verified': False, 'note': 'run `make verify-repro` to compare two builds'}),
    }
    return manifest


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('check-smoke'); s.add_argument('log', type=pathlib.Path); s.add_argument('--out', type=pathlib.Path, required=True)
    s.add_argument('--allow-no-reference', action='store_true')
    c = sub.add_parser('check-ctest'); c.add_argument('log', type=pathlib.Path); c.add_argument('--out', type=pathlib.Path, required=True)
    m = sub.add_parser('manifest')
    for opt in ('context', 'gates', 'smoke-gates', 'ctest', 'abi-result', 'abi-desc', 'abi-file', 'package', 'source', 'thirdparty', 'staging', 'out'):
        m.add_argument('--' + opt, type=pathlib.Path, required=True)
    a = ap.parse_args(argv)
    if a.cmd == 'check-smoke':
        a.out.write_text(json.dumps(check_smoke(a.log, a.allow_no_reference), indent=1) + '\n')
    elif a.cmd == 'check-ctest':
        a.out.write_text(json.dumps(check_ctest(a.log), indent=1) + '\n')
    else:
        a.out.write_text(json.dumps(build_manifest(a), indent=1, sort_keys=True) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
