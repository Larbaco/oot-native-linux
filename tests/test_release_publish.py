import copy
import hashlib
import importlib.util
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
REL = ROOT / 'release'


def load(name):
    spec = importlib.util.spec_from_file_location(name, REL / f'{name}.py')
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


pc = load('publish_check')
rd = load('render_docs')

POKO = 'poko' + 'pia'
AT = '@'
PKG = 'oot-native-linux-x86_64-v1.2.3-2026-01-02'


def good_manifest():
    return {
        'version': '1.2.3', 'commit': 'a' * 40, 'tree_clean': True, 'requires_rebuild': False,
        'gates': [{'name': 'g1', 'status': 'PASS'}, {'name': 'g2', 'status': 'PASS'}],
        'smoke_gates': [{'id': 's1', 'status': 'PASS'}],
        'ctest': {'total': 28, 'failed': 0},
        'abi': {'abi_break_allowed': False},
        'artifacts': {k: {'file': f, 'sha256': '0' * 64, 'bytes': 1} for k, f in
                      (('package', PKG + '.tar.xz'), ('source', PKG + '-source.tar.xz'), ('thirdparty_source', PKG + '-thirdparty-source.tar.xz'))},
    }


class ManifestTests(unittest.TestCase):
    def test_good_manifest_passes(self):
        self.assertEqual([], pc.check_manifest(good_manifest(), '1.2.3'))

    def test_each_failure_is_reported(self):
        cases = {
            'version': lambda m: m.update(version='9.9.9'),
            'failed gate': lambda m: m['gates'].append({'name': 'bad', 'status': 'FAIL'}),
            'skipped gate': lambda m: m['gates'].append({'name': 'sk', 'status': 'SKIPPED'}),
            'failed smoke gate': lambda m: m['smoke_gates'].append({'id': 'x', 'status': 'FAIL'}),
            'no smoke gates': lambda m: m.update(smoke_gates=[]),
            'no gates': lambda m: m.update(gates=[]),
            'ctest failures': lambda m: m.update(ctest={'total': 28, 'failed': 1}),
            'ctest missing': lambda m: m.pop('ctest'),
            'dirty tree': lambda m: m.update(tree_clean=False),
            'abi break not allowed': lambda m: m.update(requires_rebuild=True),
            'requires_rebuild missing': lambda m: m.pop('requires_rebuild'),
            'artifact missing': lambda m: m['artifacts'].pop('thirdparty_source'),
        }
        for label, mutate in cases.items():
            m = copy.deepcopy(good_manifest())
            mutate(m)
            self.assertTrue(pc.check_manifest(m, '1.2.3'), label)

    def test_an_allowed_abi_break_is_accepted(self):
        m = good_manifest()
        m.update(requires_rebuild=True)
        m['abi']['abi_break_allowed'] = True
        self.assertEqual([], pc.check_manifest(m, '1.2.3'))


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.m = good_manifest()
        for key, art in self.m['artifacts'].items():
            data = key.encode() * 10
            (self.tmp / art['file']).write_bytes(data)
            art['sha256'] = hashlib.sha256(data).hexdigest()
            art['bytes'] = len(data)
            (self.tmp / (art['file'] + '.sha256')).write_text(f"{art['sha256']}  {art['file']}\n")

    def test_matching_files_pass(self):
        self.assertEqual([], pc.check_artifacts(self.tmp, self.m))

    def test_corrupted_file_fails(self):
        (self.tmp / (PKG + '.tar.xz')).write_bytes(b'tampered')
        self.assertTrue(pc.check_artifacts(self.tmp, self.m))

    def test_sidecar_mismatch_and_missing_sidecar_fail(self):
        (self.tmp / (PKG + '-source.tar.xz.sha256')).write_text('f' * 64 + f'  {PKG}-source.tar.xz\n')
        self.assertTrue(any('sidecar' in p for p in pc.check_artifacts(self.tmp, self.m)))
        (self.tmp / (PKG + '-thirdparty-source.tar.xz.sha256')).unlink()
        self.assertTrue(any('missing' in p for p in pc.check_artifacts(self.tmp, self.m)))

    def test_manifest_hash_mismatch_fails(self):
        self.m['artifacts']['package']['sha256'] = '1' * 64
        self.assertTrue(any('manifest' in p for p in pc.check_artifacts(self.tmp, self.m)))


class GitStateTests(unittest.TestCase):
    def test_commit_must_be_head_and_tree_clean(self):
        self.assertEqual([], pc.check_commit('a' * 40, 'a' * 40, ''))
        self.assertTrue(pc.check_commit('a' * 40, 'b' * 40, ''))
        self.assertTrue(pc.check_commit('a' * 40, 'a' * 40, ' M release/pipeline.sh\n'))
        self.assertTrue(pc.check_commit('a' * 40, 'a' * 40, '?? stray\n'))


class RemoteStateTests(unittest.TestCase):
    def test_builder_must_be_private(self):
        self.assertEqual([], pc.check_builder_private({'private': True}))
        self.assertTrue(pc.check_builder_private({'private': False}))
        self.assertTrue(pc.check_builder_private({}))

    def test_tag_and_release_must_not_exist(self):
        self.assertEqual([], pc.check_public_absent(404, 404))
        self.assertTrue(pc.check_public_absent(200, 404))
        self.assertTrue(pc.check_public_absent(404, 200))
        self.assertTrue(pc.check_public_absent(0, 404))     # could not be queried: fail closed
        self.assertTrue(pc.check_public_absent(404, 401))

    def test_identity(self):
        self.assertEqual([], pc.check_identity('A', ('a' + AT + 'x.test'), 'A', ('a' + AT + 'x.test')))
        self.assertTrue(pc.check_identity('', '', 'A', ('a' + AT + 'x.test')))
        self.assertTrue(pc.check_identity('B', ('a' + AT + 'x.test'), 'A', ('a' + AT + 'x.test')))
        self.assertTrue(pc.check_identity('A', ('b' + AT + 'x.test'), 'A', ('a' + AT + 'x.test')))
        self.assertEqual([], pc.check_identity('B', ('b' + AT + 'x.test'), 'A', ('a' + AT + 'x.test'), allow_new=True))
        self.assertTrue(pc.check_identity('', '', 'A', ('a' + AT + 'x.test'), allow_new=True))


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def make_tar(self, files, extra=None):
        path = self.tmp / 'src.tar.xz'
        with tarfile.open(path, 'w:xz') as tf:
            for name, data in files.items():
                info = tarfile.TarInfo('root/' + name)
                info.size = len(data)
                info.mode = 0o755 if name.endswith('.sh') else 0o644
                tf.addfile(info, io.BytesIO(data))
            if extra:
                tf.addfile(extra)
        return path

    def test_export_drops_upstream_and_matches_allowlist(self):
        tar = self.make_tar({'a.txt': b'a', 'run.sh': b'#!/bin/sh\n', 'upstream/x/y.cs': b'x'})
        names = set(pc.extract_export(tar, self.tmp / 'out'))
        self.assertEqual({'a.txt', 'run.sh'}, names)
        self.assertFalse((self.tmp / 'out' / 'upstream').exists())
        self.assertTrue((self.tmp / 'out' / 'run.sh').stat().st_mode & 0o100)
        self.assertEqual([], pc.check_export(names, {'a.txt', 'run.sh', 'upstream/x/y.cs'}))

    def test_missing_and_extra_files_fail(self):
        self.assertTrue(pc.check_export({'a'}, {'a', 'b'}))
        self.assertTrue(pc.check_export({'a', 'extra'}, {'a'}))

    def test_symlink_members_are_refused(self):
        link = tarfile.TarInfo('root/link')
        link.type = tarfile.SYMTYPE
        link.linkname = '/etc/passwd'
        tar = self.make_tar({'a.txt': b'a'}, extra=link)
        with self.assertRaises(SystemExit):
            pc.extract_export(tar, self.tmp / 'out')

    def test_path_traversal_members_are_skipped(self):
        path = self.tmp / 'evil.tar.xz'
        with tarfile.open(path, 'w:xz') as tf:
            info = tarfile.TarInfo('root/../escape.txt')
            info.size = 1
            tf.addfile(info, io.BytesIO(b'x'))
        pc.extract_export(path, self.tmp / 'out')
        self.assertFalse((self.tmp / 'escape.txt').exists())


class ScanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)

    def scan(self, files, previous=None):
        root = self.tmp / 't'
        shutil.rmtree(root, ignore_errors=True)
        for name, data in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes(data)
        return pc.scan_export(root, previous)[0]

    def test_clean_tree_passes(self):
        self.assertEqual([], self.scan({'README.md': b'hello\n', 'git.txt': b'git@github.com\n'}))

    def test_personal_markers_fail(self):
        samples = [b'192.' + b'168.1.2', b'/ho' + b'me/someone', b'/mn' + b't/cabal/x', b'lar' + b'baco', b'/Us' + b'ers/x',
                   b'-----BEGIN RSA ' + b'PRIVATE KEY-----', b'ghp_' + b'a' * 20, b'AKIA' + b'A' * 16, b'x' + b'oxb-123', b'sk-' + b'a' * 24]
        for s in samples:
            self.assertTrue(self.scan({'doc.md': b'text ' + s + b'\n'}), s)

    def test_email_but_not_the_git_remote_form(self):
        self.assertTrue(self.scan({'doc.md': ('write to someone' + AT + 'example.org\n').encode()}))

    def test_forbidden_types_and_binaries_fail(self):
        self.assertTrue(self.scan({'a.nup': b'x'}))
        self.assertTrue(self.scan({'a.png': b'x'}))
        self.assertTrue(self.scan({'blob.bin': b'\0\1\2'}))
        self.assertTrue(self.scan({'big.txt': b'a' * (5 * 1024 * 1024 + 1)}))
        self.assertTrue(self.scan({POKO + '-notes.md': b'x'}))

    def test_new_title_text_needs_an_owner_decision(self):
        self.assertEqual([], self.scan({'a.md': ('credit ' + POKO.capitalize()).encode(), 'b.md': b'x'}, previous={'a.md'}))
        self.assertTrue(self.scan({'a.md': ('credit ' + POKO.capitalize()).encode(), 'b.md': (POKO.capitalize() + ' too').encode()}, previous={'a.md'}))

    def test_tree_diff(self):
        a, b = self.tmp / 'a', self.tmp / 'b'
        for d in (a, b):
            d.mkdir()
            (d / 'f').write_text('x')
        self.assertEqual([], pc.tree_diff(a, b))
        (b / 'f').write_text('y')
        (b / 'extra').write_text('z')
        self.assertEqual(2, len(pc.tree_diff(a, b)))


class NotesAndAssetsTests(unittest.TestCase):
    def test_notes_fill_the_placeholders_and_fail_on_unknown_ones(self):
        out = pc.render_notes('@TAG@ @PKG_NAME@\n@SHA256_BLOCK@\n', '1.2.3', PKG, {'b': 'bb', 'a': 'aa'})
        self.assertIn('v1.2.3 ' + PKG, out)
        self.assertIn('aa  a\nbb  b', out)
        with self.assertRaises(SystemExit):
            pc.render_notes('@NOPE@', '1.2.3', PKG, {})

    def test_notes_with_personal_data_are_refused(self):
        self.assertTrue(pc.check_notes('contact me: someone' + AT + 'example.org'))
        self.assertTrue(pc.check_notes('see /ho' + 'me/user/file'))
        self.assertEqual([], pc.check_notes('all good'))

    def test_the_versioned_notes_template_renders_clean(self):
        template = (REL / 'notes' / 'v0.1.4.md').read_text()
        out = pc.render_notes(template, '0.1.4', 'oot-native-linux-x86_64-v0.1.4-2026-11-07', {'a': 'x' * 64})
        self.assertEqual([], pc.check_notes(out))
        self.assertNotIn('v0.1.3-2026', out)

    def test_release_asset_checks(self):
        sums = {n: ('%064x' % i) for i, n in enumerate(pc.asset_names(PKG)[:3])}
        names = pc.asset_names(PKG)
        good = {'prerelease': True, 'draft': False, 'assets': [
            {'name': n, 'digest': 'sha256:' + sums[n] if n in sums else None} for n in names]}
        self.assertEqual([], pc.check_release_assets(good, sums, PKG, True))
        bad = copy.deepcopy(good)
        bad['assets'][0]['digest'] = 'sha256:' + '9' * 64
        self.assertTrue(pc.check_release_assets(bad, sums, PKG, True))
        short = copy.deepcopy(good)
        short['assets'].pop()
        self.assertTrue(pc.check_release_assets(short, sums, PKG, True))
        self.assertTrue(pc.check_release_assets(good, sums, PKG, False))     # prerelease flag mismatch
        draft = copy.deepcopy(good)
        draft['draft'] = True
        self.assertTrue(pc.check_release_assets(draft, sums, PKG, True))


class RenderDocsTests(unittest.TestCase):
    def test_placeholders_follow_version(self):
        text = rd.render_text('v@VERSION@ @PACKAGE_ARCHIVE@ @TESTER_GUIDE@ @THIRDPARTY_SOURCE_ARCHIVE@ GLIBC_@GLIBC_BASELINE@', '1.2.3-rc1', PKG)
        self.assertEqual(f'v1.2.3-rc1 {PKG}.tar.xz TESTER-v1.2.3.md {PKG}-thirdparty-source.tar.xz GLIBC_@GLIBC_BASELINE@', text)
        with self.assertRaises(SystemExit):
            rd.render_text('@UNKNOWN@', '1.2.3', PKG)

    def check(self, files, version='1.2.3', pkg=PKG):
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        for name, text in files.items():
            (tmp / name).parent.mkdir(parents=True, exist_ok=True)
            (tmp / name).write_text(text)
        return rd.check_tree(tmp, version, pkg)

    def test_other_release_file_names_fail(self):
        self.assertEqual([], self.check({'README.md': f'see {PKG}.tar.xz and {PKG}-source.tar.xz.'}))
        self.assertTrue(self.check({'README.md': 'see oot-native-linux-x86_64-v1.2.2-2026-01-01.tar.xz'}))
        self.assertTrue(self.check({'sub/doc.md': 'see oot-native-linux-x86_64-v0.1.3-2026-11-07-source.tar.xz'}))
        self.assertTrue(self.check({'README.md': 'read TESTER-v1.2.2.md'}))
        self.assertEqual([], self.check({'README.md': 'read TESTER-v1.2.3.md'}))

    def test_any_unfilled_placeholder_fails_unless_the_template_is_explicitly_allowed(self):
        self.assertTrue(self.check({'README.md': 'v@VERSION@'}))
        self.assertTrue(self.check({'README.md': 'GLIBC_@GLIBC_BASELINE@'}))
        self.assertTrue(self.check({'builder/release/README-RELEASE.md': 'GLIBCXX_@GLIBCXX_BASELINE@'}))
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        (tmp / 'release').mkdir()
        (tmp / 'release' / 'README-RELEASE.md').write_text('GLIBC_@GLIBC_BASELINE@')
        (tmp / 'other.md').write_text('GLIBC_@GLIBC_BASELINE@')
        self.assertEqual(1, len(rd.check_tree(tmp, '1.2.3', PKG, ('release/README-RELEASE.md',))))   # only the template is exempt
        self.assertEqual(2, len(rd.check_tree(tmp, '1.2.3', PKG)))

    def test_unconfirmed_platform_claims_fail(self):
        self.assertTrue(self.check({'TESTER-v1.2.3.md': 'send your ' + 'Steam' + 'OS version'}))
        self.assertEqual([], self.check({'TESTER-v1.2.3.md': 'send your Linux distribution and version'}))

    def test_package_files_do_not_carry_the_release_readme_template(self):
        self.assertNotIn('release/README-RELEASE.md', (REL / 'package-files.txt').read_text().split())

    def test_third_party_trees_are_not_scanned(self):
        self.assertEqual([], self.check({'LICENSES/x.txt': 'oot-native-linux-x86_64-v0.0.1-x', 'python/doc.md': 'oot-native-linux-x86_64-v0.0.1-x'}))

    def test_the_repository_documentation_renders_and_checks_clean(self):
        tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, tmp)
        for name in ('README.md', 'release/README-RELEASE.md', 'release/THIRD-PARTY-SOURCE.md', 'TESTER-v0.1.4.md', 'release/allowlist.txt'):
            (tmp / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / name, tmp / name)
        pkg = 'oot-native-linux-x86_64-v0.1.4-2026-11-07'
        rd.render_tree(tmp, '0.1.4', pkg)
        self.assertEqual([], rd.check_tree(tmp, '0.1.4', pkg, ('release/README-RELEASE.md',)))
        self.assertTrue(rd.check_tree(tmp, '0.1.4', pkg))      # the template alone keeps its baseline placeholders, nowhere else
        self.assertEqual([], rd.check_tester_guide(tmp, '0.1.4'))
        self.assertTrue(rd.check_tester_guide(tmp, '0.1.5'))      # no guide for another version
        self.assertTrue(rd.check_tree(tmp, '0.1.5', 'oot-native-linux-x86_64-v0.1.5-2026-11-08'))


@unittest.skipUnless(shutil.which('gh') and shutil.which('git'), 'needs gh and git')
class PublishScriptGuardTests(unittest.TestCase):
    def run_script(self, env_extra):
        env = {'PATH': '/usr/bin:/bin:/usr/local/bin', 'HOME': tempfile.gettempdir()}
        env.update(env_extra)
        return subprocess.run(['bash', str(REL / 'publish.sh')], capture_output=True, text=True, env=env, cwd=ROOT)

    def test_version_and_out_are_required(self):
        self.assertEqual(2, self.run_script({}).returncode)
        self.assertEqual(2, self.run_script({'VERSION': 'banana', 'OUT': '/'}).returncode)

    def test_a_missing_manifest_stops_the_dry_run_before_any_remote_call(self):
        with tempfile.TemporaryDirectory() as out:
            r = self.run_script({'VERSION': '9.9.9', 'OUT': out})
            self.assertEqual(1, r.returncode)
            self.assertIn('[FAIL] local-preconditions', r.stderr)
            self.assertNotIn('remote-preconditions', r.stdout)
            self.assertIn('nothing was published', r.stderr)

    def test_publish_flag_is_validated(self):
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(2, self.run_script({'VERSION': '9.9.9', 'OUT': out, 'PUBLISH': 'yes'}).returncode)


if __name__ == '__main__':
    unittest.main()
