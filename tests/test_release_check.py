import importlib.util
import pathlib
import tarfile
import tempfile
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[1] / 'release' / 'release_check.py'
spec = importlib.util.spec_from_file_location('release_check', MODULE)
release_check = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(release_check)


class ReleaseCheckTests(unittest.TestCase):
    def test_synthetic_safe_file_passes(self):
        self.assertEqual([], release_check.scan_items({'README.txt': b'local build from user-owned dump\n'}, {'README.txt'}))

    def test_rejects_unlisted_game_file(self):
        errors = release_check.scan_items({'RomFS.img': b'\0synthetic'}, {'README.txt'})
        self.assertTrue(any('not in allowlist' in e for e in errors))
        self.assertTrue(any('forbidden filename/type' in e for e in errors))

    def test_rejects_signatures_and_personal_strings(self):
        errors = release_check.scan_items({'bin/tool': b'NSO0\0 ' + b'/' + b'home/' + b'example'}, {'bin/tool'})
        self.assertTrue(any('signature' in e for e in errors))
        self.assertTrue(any('personal' in e for e in errors))

    def test_rejects_external_hex_dump(self):
        data = b'bytes:\n' + (b'0x12345678, ' * 30)
        self.assertTrue(any('byte dump' in e for e in release_check.scan_items({'test.py': data}, {'test.py'})))

    def test_package_mode_allows_manifested_generated_managed_binary(self):
        items = {'builder/bin/dmsh_xlate.dll': b'MZ\0managed translator'}
        self.assertEqual([], release_check.scan_items(items, set(items), 'package'))

    def test_package_mode_still_rejects_game_assets_and_signatures(self):
        items = {'game.nso': b'NSO0' + b'\0' * 32, 'builder/cache.bin': b'SARC\0'}
        errors = release_check.scan_items(items, set(items), 'package')
        self.assertTrue(any('forbidden filename/type' in e for e in errors))
        self.assertTrue(any('content signature' in e for e in errors))

    def test_rejects_python_bytecode_in_licenses(self):
        item = {'LICENSES/notices.pyc': b'compiled synthetic fixture'}
        errors = release_check.scan_items(item, set(item), 'package')
        self.assertTrue(any('compiled Python cache' in e for e in errors))

    def test_rejects_other_title_profile_identifier(self):
        item = {'games/0400A3A0' '1EE4C000/profile.tsv': b'title\t0400A3A0' b'1EE4C000\n'}
        errors = release_check.scan_items(item, set(item))
        self.assertTrue(any('other-game title/profile' in e for e in errors))

    def test_rejects_dump_and_log_path_segments(self):
        errors = release_check.scan_items({'build/extract/RomFS/file.txt': b'synthetic'}, {'build/extract/RomFS/file.txt'})
        self.assertTrue(any('forbidden dump/build-data path' in e for e in errors))

    def test_preserves_public_author_emails_only_in_approved_paths(self):
        allowed = {'LICENSES/upstream.txt', 'README.md'}
        public_email = b'author' + b'@' + b'gmail.com'
        items = {
            'LICENSES/upstream.txt': b'Copyright ' + public_email + b'\n',
            'README.md': b'contact ' + public_email + b'\n',
        }
        errors = release_check.scan_items(items, allowed)
        self.assertTrue(any('README.md: personal' in e for e in errors))
        self.assertFalse(any('LICENSES/upstream.txt: personal' in e for e in errors))

    def test_embedded_python_author_email_does_not_hide_owner_markers(self):
        public_email = b'author' + b'@' + b'gmail.com'
        items = {
            'oot-native-launcher': b'\x7fELF' + public_email,
            'notes.txt': b'thia' + b'goo',
        }
        errors = release_check.scan_items(items, set(items), 'package')
        self.assertTrue(any('notes.txt: personal' in e for e in errors))
        self.assertFalse(any('oot-native-launcher: personal' in e for e in errors))

    def test_pinned_runtime_author_emails_are_exempt_only_at_upstream_paths(self):
        public_email = b'author' + b'@' + b'gmail.com'
        allowed_paths = {
            'python/lib/libpython3.12.so.1.0',
            'python/lib/python3.12/site-packages/zstandard-0.23.0.dist-info/METADATA',
            'python/lib/python3.12/site-packages/lz4-4.3.3.dist-info/METADATA',
            'python/lib/python3.12/site-packages/PIL/IcoImagePlugin.py',
            'python/lib/python3.12/site-packages/PIL/ImageMorph.py',
        }
        items = {path: b'upstream author ' + public_email for path in allowed_paths}
        items['python/lib/python3.12/site-packages/other.py'] = b'contact ' + public_email
        items['python/lib/python3.12/site-packages/PIL/ImageMorph.py'] += b' lar' + b'baco'

        errors = release_check.scan_items(items, set(items), 'package')
        self.assertTrue(any('other.py: personal' in e for e in errors))
        self.assertTrue(any('ImageMorph.py: personal' in e for e in errors))
        self.assertFalse(any(path in e for path in allowed_paths - {
            'python/lib/python3.12/site-packages/PIL/ImageMorph.py'
        } for e in errors))

    def test_standard_library_examples_are_preserved_but_private_ip_elsewhere_fails(self):
        allowed = {'python/lib/python3.12/ipaddress.py', 'src/example.py'}
        private_range = b'192.' + b'168.'
        items = {
            'python/lib/python3.12/ipaddress.py': b"IPv4Network('" + private_range + b"0.0/16')",
            'src/example.py': private_range + b'31.235',
        }
        errors = release_check.scan_items(items, allowed)
        self.assertTrue(any('src/example.py: personal' in e for e in errors))
        self.assertFalse(any('ipaddress.py: personal' in e for e in errors))

    def test_tar_rejects_non_root_names_and_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = pathlib.Path(tmp) / 'bad.tar'
            with tarfile.open(archive, 'w') as tf:
                info = tarfile.TarInfo('safe/README.txt')
                info.size = 1
                info.uid = 1000
                info.gid = 1000
                info.uname = 'lar' + 'baco'
                info.gname = 'lar' + 'baco'
                import io
                tf.addfile(info, io.BytesIO(b'x'))
            with self.assertRaisesRegex(ValueError, 'non-root tar metadata'):
                list(release_check.iter_tar(archive))

    def test_tar_accepts_root_normalized_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = pathlib.Path(tmp) / 'good.tar'
            with tarfile.open(archive, 'w') as tf:
                info = tarfile.TarInfo('safe/README.txt')
                info.size = 1
                info.uid = info.gid = 0
                info.uname = info.gname = 'root'
                import io
                tf.addfile(info, io.BytesIO(b'x'))
            self.assertEqual([('README.txt', b'x')], list(release_check.iter_tar(archive)))

    def test_tar_accepts_numeric_owner_with_empty_name_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = pathlib.Path(tmp) / 'numeric.tar'
            with tarfile.open(archive, 'w') as tf:
                info = tarfile.TarInfo('safe/README.txt')
                info.size = 1
                info.uid = info.gid = 0
                info.uname = info.gname = ''
                import io
                tf.addfile(info, io.BytesIO(b'x'))
            self.assertEqual([('README.txt', b'x')], list(release_check.iter_tar(archive)))


if __name__ == '__main__': unittest.main()
