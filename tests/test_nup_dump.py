#!/usr/bin/env python3
"""The .nup path of nxs2.py against containers made up here (no game data). Needs zstandard and the packer of https://github.com/a55uka/nup
(NUP_TOOLS=<its tools/nup folder>, or `nup` installed); skipped without it. Also needs nxnative's romfs_image.py to compare the image (NXNATIVE=<checkout>); that
one test is skipped without it. Run: python3 tests/test_nup_dump.py"""
import importlib.util, os, pathlib, shutil, sys, tempfile, types, unittest

HERE = pathlib.Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import nxs2, nup_dump   # nup_dump puts third_party first on sys.path, so `nup` is the vendored reader

PACK = None
tools = os.environ.get('NUP_TOOLS')
if tools and (pathlib.Path(tools) / 'nup/pack.py').is_file():
    sys.path.insert(0, tools)
    for m in [m for m in sys.modules if m == 'nup' or m.startswith('nup.')]: del sys.modules[m]   # the whole package from there, for its packer
    from nup.pack import pack as PACK
    from nup import format as _F, read as _R
    nup_dump.F, nup_dump.NupReader = _F, _R.NupReader

ROMFS_IMAGE = pathlib.Path(os.environ.get('NXNATIVE', HERE / 'nxnative')) / 'dm/tools/romfs_image.py'


def nso(build):   # enough of an NSO for the build ID check: the ModuleId at 0x40
    return b'NSO0' + b'\0' * 0x3c + bytes.fromhex(build) + b'\0' * 0x100


def tree(root, files):
    for rel, data in files.items():
        p = pathlib.Path(root) / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(data)


def game_files(build):
    g = nxs2.read_game_tsv(nxs2.PROFILE_SRC / 'game.tsv')
    out = {f'exefs/{m}': nso(b) for m, b in build.items()}
    out['exefs/main.npdm'] = b'npdm'
    out['romfs/' + g['romfs_marker'][0]] = b'marker' * 1000
    for line in (nxs2.PROFILE_SRC / 'shader_sources.tsv').read_text().splitlines():
        if line.strip() and not line.startswith(('#', 'blob\t')):
            for p in line.split('\t')[1:]: out['romfs/' + p] = p.encode() * 500
    out['romfs/a/empty.bin'] = b''
    out['romfs/B/z.bin'] = os.urandom(1 << 12)   # incompressible: stored raw in the container
    return out


@unittest.skipUnless(PACK, 'needs NUP_TOOLS=<nup/tools/nup> for the packer')
class NupDump(unittest.TestCase):
    def setUp(self):
        self.t = pathlib.Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.t, True)
        self.builds = nxs2.builds()

    def pack(self, layout):
        for rel, files in layout.items(): tree(self.t / 'd' / rel, files)
        out = self.t / 'g.nup'; PACK(str(out), str(self.t / 'd'), quiet=True) if 'quiet' in PACK.__code__.co_varnames else PACK(str(out), str(self.t / 'd'))
        return out

    def two_updates(self):
        (old, _, a), (new, _, b) = self.builds[0], self.builds[1]
        return self.pack({'base': {'exefs/main': nso('00' * 16)}, f'updates/{old[1:]}': game_files(a), f'updates/{new[1:]}': game_files(b)})

    def args(self, f): return types.SimpleNamespace(exefs=None, romfs=None, dump=str(f))

    def test_newest_update_wins_and_outputs_match_the_files(self):
        f = self.two_updates()
        ex, ro, ver, _ = nxs2.find_dump(self.args(f))
        self.assertEqual(ver.split()[0], self.builds[1][0])
        ex.copy_exefs(self.t / 'exefs')
        want = game_files(self.builds[1][2])
        self.assertEqual((self.t / 'exefs/main.npdm').read_bytes(), want['exefs/main.npdm'])
        self.assertTrue(ro.write_image(self.t / 'r.img'))
        self.assertFalse(ro.write_image(self.t / 'r.img'), 'the second time the image is kept')
        ro.use_image(self.t / 'r.img', self.t / 'files')
        marker = nxs2.read_game_tsv(nxs2.PROFILE_SRC / 'game.tsv')['romfs_marker'][0]
        self.assertEqual((ro / marker).read_bytes(), want['romfs/' + marker])
        self.assertEqual((ro / 'a/empty.bin').read_bytes(), b'')
        self.assertFalse((ro / 'no/such.bin').exists())
        ro.discard_files(); self.assertFalse((self.t / 'files').exists())

    @unittest.skipUnless(ROMFS_IMAGE.is_file(), 'needs nxnative (NXNATIVE=...) for romfs_image.py')
    def test_image_is_what_romfs_image_writes(self):
        spec = importlib.util.spec_from_file_location('romfs_image', ROMFS_IMAGE); ri = importlib.util.module_from_spec(spec); spec.loader.exec_module(ri)
        f = self.two_updates()
        _, ro, *_ = nxs2.find_dump(self.args(f))
        ro.write_image(self.t / 'a.img')
        ri.pack(self.t / 'd' / 'updates' / self.builds[1][0][1:] / 'romfs', self.t / 'b.img')
        self.assertEqual((self.t / 'a.img').read_bytes(), (self.t / 'b.img').read_bytes())

    def test_base_only_is_refused(self):
        _, _, a = self.builds[0]
        f = self.pack({'base': {'exefs/main': nso('00' * 16), 'romfs/x': b'x'}})
        with self.assertRaisesRegex(nxs2.Fail, 'update is needed'): nxs2.find_dump(self.args(f))

    def test_update_without_all_the_files_is_refused(self):
        v, _, a = self.builds[0]
        files = {k: d for k, d in game_files(a).items() if 'LD_C0_R' not in k}
        f = self.pack({'base': {'exefs/main': nso('00' * 16)}, f'updates/{v[1:]}': files})
        with self.assertRaisesRegex(nxs2.Fail, 'LD_C0_R.BIN'): nxs2.find_dump(self.args(f))

    def test_damage_is_found_and_no_image_appears(self):
        v, _, a = self.builds[0]
        f = self.pack({'base': {'exefs/main': nso('00' * 16)}, f'updates/{v[1:]}': game_files(a)})
        _, ro, *_ = nxs2.find_dump(self.args(f))
        with nup_dump.NupReader(str(f)) as r:
            i = next(i for i, e in enumerate(r.files) if e[0] == b'z.bin'); off = r.files[i][3]
        with open(f, 'r+b') as h: h.seek(off); b = h.read(1); h.seek(off); h.write(bytes([b[0] ^ 0xff]))
        with self.assertRaises(nup_dump.Error): ro.write_image(self.t / 'r.img')
        self.assertFalse((self.t / 'r.img').exists())


class Paths(unittest.TestCase):
    def test_build_folder_beside_the_file_without_the_extension(self):
        t = pathlib.Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, t, True)
        (t / 'x.NUP').write_bytes(b''); (t / 'folder.nup').mkdir()
        self.assertTrue(nxs2.is_nup(t / 'x.NUP'))
        self.assertFalse(nxs2.is_nup(t / 'folder.nup'), 'a folder is a folder, whatever its name')


if __name__ == '__main__': unittest.main()
