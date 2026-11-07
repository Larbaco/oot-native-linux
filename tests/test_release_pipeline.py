import importlib.util
import json
import pathlib
import re
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
REL = ROOT / 'release'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


abi = load('release_abi', REL / 'release_abi.py')
pins_mod = load('release_pins', REL / 'release_pins.py')
manifest = load('release_manifest', REL / 'release_manifest.py')


def descriptor(exports=('rt_svc', 'adapter_foo', 'helper'), entries=(('foo', 1, 1), ('bar', 3, 0)), **const):
    consts = {'abi_version': 1, 'guest_cpu_size': 896, 'token_base': 1, 'token_size': 0x100000, 'token_stride': 16,
              'module_base': 2, 'module_region_size': 3, 'guest_as_base': 4, 'token_capacity': 65536, 'stub_capacity': 4096}
    consts.update(const)
    return abi.build_descriptor({'constants': consts, 'entries': [list(e) for e in entries]}, list(exports), 'test')


class AbiCompareTests(unittest.TestCase):
    def test_identical_is_compatible(self):
        self.assertFalse(abi.compare(descriptor(), descriptor())['requires_rebuild'])

    def test_non_surface_export_changes_do_not_matter(self):
        r = abi.compare(descriptor(), descriptor(exports=('rt_svc', 'adapter_foo', 'renamed_helper')))
        self.assertFalse(r['requires_rebuild'])

    def test_added_surface_export_is_compatible_but_reported(self):
        r = abi.compare(descriptor(), descriptor(exports=('rt_svc', 'adapter_foo', 'rt_new')))
        self.assertFalse(r['requires_rebuild'])
        self.assertEqual(['rt_new'], r['exports_added'])

    def test_removed_surface_export_requires_rebuild(self):
        r = abi.compare(descriptor(), descriptor(exports=('rt_svc',)))
        self.assertTrue(r['requires_rebuild'])
        self.assertEqual(['adapter_foo'], r['exports_removed'])

    def test_registry_change_requires_rebuild(self):
        for entries in ((('foo', 1, 1),), (('foo', 1, 2), ('bar', 3, 0)), (('foo', 1, 1), ('bar', 3, 0), ('baz', 2, 1))):
            self.assertTrue(abi.compare(descriptor(), descriptor(entries=entries))['requires_rebuild'], entries)

    def test_registry_order_is_irrelevant(self):
        self.assertFalse(abi.compare(descriptor(), descriptor(entries=(('bar', 3, 0), ('foo', 1, 1))))['requires_rebuild'])

    def test_hard_constants_require_rebuild(self):
        for change in ({'abi_version': 2}, {'guest_cpu_size': 960}, {'token_base': 9}, {'stub_capacity': 8192}):
            self.assertTrue(abi.compare(descriptor(), descriptor(**change))['requires_rebuild'], change)


class AbiParseTests(unittest.TestCase):
    INFO = ('nxrt-abi-info 1\nabi_version 1\nguest_cpu_size 896\ntoken_base 0x70fff00000\ntoken_size 0x100000\ntoken_stride 16\n'
            'module_base 0x7100000000\nmodule_region_size 0x10000000\nguest_as_base 0x7200000000\ntoken_capacity 65536\n'
            'stub_capacity 4096\nregistry_count 2\nentry\t0\tfoo\t1\t1\nentry\t1\tbar\t3\t0\n')

    def test_parse(self):
        p = abi.parse_abi_info(self.INFO)
        self.assertEqual(0x70FFF00000, p['constants']['token_base'])
        self.assertEqual([['foo', 1, 1], ['bar', 3, 0]], p['entries'])

    def test_count_mismatch_and_bad_header_are_rejected(self):
        with self.assertRaises(ValueError): abi.parse_abi_info(self.INFO.replace('registry_count 2', 'registry_count 3'))
        with self.assertRaises(ValueError): abi.parse_abi_info('hello\n')

    def test_source_parser_matches_generated_registry_format(self):
        files = {
            'windows/src/abi/include/nxw/guest_cpu.h': '#define NXW_ABI_VERSION 1\nNXW_STATIC_ASSERT(sizeof(NxwGuestCpu) == 896, "ABI 1");\n',
            'windows/src/nxrt/include/nxrt/rt_abi.h': ('#define NXRT_TOKEN_BASE         0x70FFF00000ull\n#define NXRT_TOKEN_SIZE         0x100000ull\n'
                '#define NXRT_TOKEN_STRIDE       16u\n#define NXRT_MODULE_BASE        0x7100000000ull\n'
                '#define NXRT_MODULE_REGION_SIZE 0x10000000ull\n#define NXRT_GUEST_AS_BASE      0x7200000000ull\n'),
            'windows/src/nxrt/host/hle.c': '#define MAX_STUB 4096   // x\n',
            'windows/src/nxrt/gen/out/hle_registry.gen.c': '    {"foo", NXRT_HLE_FORCED, 1, "f.c", "why", adapter_foo},\n    {"bar", NXRT_HLE_NVN, 0, "g.c", "why", adapter_bar},\n',
        }
        p = abi.parse_source(files)
        self.assertEqual(65536, p['constants']['token_capacity'])
        self.assertEqual([['foo', 1, 1], ['bar', 3, 0]], p['entries'])

    def test_repository_sources_parse(self):
        files = abi.read_source_files(ROOT)
        p = abi.parse_source(files)
        self.assertGreater(len(p['entries']), 1000)
        self.assertEqual(1, p['constants']['abi_version'])


class PinsTests(unittest.TestCase):
    def test_pins_file_is_well_formed(self):
        pins = pins_mod.load()
        self.assertTrue(pins_mod.downloads(pins))
        self.assertRegex(pins['UBUNTU_BASE_DIGEST'], r'^sha256:[0-9a-f]{64}$')
        self.assertRegex(pins['NXNATIVE_COMMIT'], r'^[0-9a-f]{40}$')

    def test_toolchain_pins_match_nxs2_linux(self):
        pins = pins_mod.load()
        spec = importlib.util.spec_from_file_location('nxs2_linux_pins', ROOT / 'nxs2_linux.py')
        text = (ROOT / 'nxs2_linux.py').read_text()
        for key in ('LLVM', 'SDL3', 'VULKAN'):
            self.assertIn(pins[f'DL_{key}_SHA256'], text, key)
            self.assertIn(pins[f'DL_{key}_FILE'], text, key)
            self.assertIn(pins[f'DL_{key}_URL'], text, key)

    def test_nxnative_pin_matches_the_setup_script(self):
        pins = pins_mod.load()
        m = re.search(r"UPSTREAM_COMMIT = '([0-9a-f]+)'", (ROOT / 'nxs2.py').read_text())
        self.assertTrue(pins['NXNATIVE_COMMIT'].startswith(m.group(1)))

    def test_malformed_pins_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = pathlib.Path(tmp) / 'pins.env'
            bad.write_text('KEY = value\n')
            with self.assertRaises(ValueError): pins_mod.load(bad)
            bad.write_text('A=1\nA=2\n')
            with self.assertRaises(ValueError): pins_mod.load(bad)

    def test_dockerfiles_have_no_floating_inputs(self):
        for name in ('Dockerfile.ubuntu22.04', 'Dockerfile.smoke.ubuntu22.04'):
            text = (REL / name).read_text()
            self.assertIn('FROM --platform=linux/amd64 ubuntu@${UBUNTU_BASE_DIGEST}', text)
            self.assertNotRegex(text, r'FROM[^\n]*:22\.04')
            self.assertNotIn('dotnet-install', text)
        self.assertIn('--require-hashes', (REL / 'Dockerfile.ubuntu22.04').read_text())

    def test_every_pip_requirement_is_hashed(self):
        for lock in REL.glob('requirements-*.lock'):
            body = lock.read_text().replace('\\\n', ' ')
            reqs = [l for l in body.splitlines() if l and not l.startswith('#')]
            self.assertTrue(reqs)
            for line in (re.sub(r'\s+', ' ', r).strip() for r in reqs):
                self.assertRegex(line, r'^[A-Za-z0-9_.-]+==[0-9][^ ]* (--hash=sha256:[0-9a-f]{64} ?)+$', f'{lock.name}: {line[:60]}')


class BaselineTests(unittest.TestCase):
    def test_committed_v013_baseline_is_well_formed_and_matches_its_source(self):
        d = json.loads((REL / 'abi-baseline-v0.1.3.json').read_text())
        self.assertEqual(1, d['abi_version'])
        self.assertEqual(d['registry']['count'], len(d['registry']['entries']))
        self.assertEqual(abi.registry_summary(d['registry']['entries'])['sha256'], d['registry']['sha256'])
        self.assertEqual(abi.exports_summary(d['exports']['names'])['sha256'], d['exports']['sha256'])
        self.assertIn('provenance', d)

    def test_current_source_registry_is_compatible_with_the_baseline(self):
        d = json.loads((REL / 'abi-baseline-v0.1.3.json').read_text())
        now = abi.build_descriptor(abi.parse_source(abi.read_source_files(ROOT)), d['exports']['names'], 'test')
        r = abi.compare(d, now)
        self.assertFalse(r['requires_rebuild'], r['reasons'])


class SmokeScriptTests(unittest.TestCase):
    def test_every_required_smoke_gate_has_its_message_in_package_smoke_sh(self):
        script = (REL / 'package-smoke.sh').read_text()
        for gate_id, pattern in manifest.SMOKE_REQUIRED:
            literal = re.split(r'\\|[\[\](){}*+?$|]', pattern.lstrip('^'))[0]
            if gate_id.startswith(('vulkan-2a', 'vulkan-2e')):
                literal = 'nxrt has Vulkan backend'
            elif gate_id.startswith('invalid-dump'):
                literal = 'rejects a synthetic invalid'
            self.assertIn(literal.strip(), script, gate_id)


class SmokeEvidenceTests(unittest.TestCase):
    def good_log(self):
        lines = []
        for gate_id, pattern in manifest.SMOKE_REQUIRED:
            line = pattern.replace('\\', '').replace('^', '').replace('$', '')
            line = re.sub(r'\[A-Z\]\*', 'vkX*', line).replace('\\d+', '7')
            lines.append(line)
        return '\n'.join(lines) + '\n'

    def test_required_patterns_are_self_consistent(self):
        for gate_id, pattern in manifest.SMOKE_REQUIRED:
            re.compile(pattern)

    def test_missing_vulkan_gate_fails_closed(self):
        full = '\n'.join(f'PASS: {i}' for i, _ in manifest.SMOKE_REQUIRED)
        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp) / 'smoke.log'
            log.write_text(full)
            with self.assertRaises(SystemExit) as ctx: manifest.check_smoke(log, False)
            self.assertIn('vulkan-2b', str(ctx.exception))

    def test_fail_line_blocks_even_when_all_markers_exist(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp) / 'smoke.log'
            log.write_text('FAIL: nxrt exports a Vulkan API symbol\n')
            with self.assertRaises(SystemExit): manifest.check_smoke(log, False)


if __name__ == '__main__':
    unittest.main()
