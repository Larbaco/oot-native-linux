import pathlib
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace

import nxs2_linux


class LinuxReleaseTests(unittest.TestCase):
    def test_shader_command_uses_bundled_translator_directory(self):
        package_bin = pathlib.Path('/package/builder/bin')
        n = SimpleNamespace(TITLE='0400486024DEC000', SHADER_VARIANTS=None)
        b = SimpleNamespace(lx=pathlib.Path('/scratch/build/linux'), out=pathlib.Path('/scratch/build'))
        t = {'nxshader': pathlib.Path('/package/builder/windows/src/nxshader/nxshader.py'), 'dir': package_bin}

        command = nxs2_linux.shader_command(n, b, t, 2, pathlib.Path('/scratch/build/logs/shaders.tsv'))

        self.assertEqual(package_bin, command[command.index('--xlate') + 1])
        self.assertEqual(nxs2_linux.HERE, command[command.index('--installer') + 1])

    def test_shader_failure_extracts_first_exception_headline(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = pathlib.Path(tmp) / 'lx_shaders.log'
            log.write_text('Traceback (most recent call last):\n  ...\nFileNotFoundError: missing translator\n', encoding='utf-8')
            self.assertEqual('FileNotFoundError: missing translator', nxs2_linux.first_exception_line(log))

    def test_build_environment_uses_invariant_dotnet_globalization(self):
        n = SimpleNamespace(AMPERE=pathlib.Path('/package/builder/ampere-lower/ampere-lower'), dotnet_root=lambda: '/package/dotnet')
        env = nxs2_linux.env(n)
        self.assertEqual('1', env['DOTNET_SYSTEM_GLOBALIZATION_INVARIANT'])
        self.assertEqual('/package/dotnet', env['DOTNET_ROOT'])

    def test_release_space_check_uses_decimal_45_gb_and_existing_parent(self):
        with tempfile.TemporaryDirectory() as tmp:
            destination = pathlib.Path(tmp) / 'not-created' / 'build'
            fake_usage = SimpleNamespace(free=44_999_999_999)
            with mock.patch.object(nxs2_linux.shutil, 'disk_usage', return_value=fake_usage) as disk_usage:
                check = nxs2_linux.release_space_check(destination)
            self.assertFalse(check['ok'])
            self.assertEqual('space_45gb', check['code'])
            self.assertIn('38 GB', check['name'])
            self.assertEqual(pathlib.Path(tmp), disk_usage.call_args.args[0])
            with mock.patch.object(nxs2_linux.shutil, 'disk_usage', return_value=SimpleNamespace(free=45_000_000_000)):
                self.assertTrue(nxs2_linux.release_space_check(destination)['ok'])


if __name__ == '__main__':
    unittest.main()
