"""Isolated ZBIC helper support with synthetic NSOs (no game files needed)."""
import os, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import nso_tool, lz4.block, zstandard

ZBIC_HELPER = os.environ.get("NXS2_ZBIC_UNPACK") or shutil.which("zbic-unpack")

def zbic_compress(raw):
    frame = zstandard.ZstdCompressor(write_content_size=True).compress(raw)
    return b"ZBIC" + frame[4:]


def make_nso(codec):
    segs = [bytes(range(256)) * 16 + b"text" * 500, b"rodata " * 700, bytes(64) + b"data" * 300]
    f = bytearray(0x100)
    f[0:4] = b"NSO0"
    flags = 0x7 | (0x80 if codec == "zbic" else 0)
    struct.pack_into("<I", f, 0x0C, flags)
    for i, raw in enumerate(segs):
        stored = zbic_compress(raw) if codec == "zbic" else lz4.block.compress(raw, store_size=False)
        struct.pack_into("<III", f, 0x10 + 0x10 * i, len(f), 0x1000 * i, len(raw))
        struct.pack_into("<I", f, 0x60 + 4 * i, len(stored))
        f += stored
    return bytes(f), segs


class ZbicNso(unittest.TestCase):
    def check(self, codec):
        data, segs = make_nso(codec)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m"
            p.write_bytes(data)
            got, _, _ = nso_tool.read_nso(p)
            for name, raw in zip(("text", "rodata", "data"), segs):
                self.assertEqual(got[name][1], raw)

    @unittest.skipUnless(ZBIC_HELPER, "the separate zbic-unpack helper is not built")
    def test_zbic(self): self.check("zbic")

    @unittest.skipUnless(ZBIC_HELPER, "the separate zbic-unpack helper is not built")
    def test_zbic_helper_normalizes_nso(self):
        data, segs = make_nso("zbic")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "main"
            p.write_bytes(data)
            result = subprocess.run([ZBIC_HELPER, p], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            normalized = p.read_bytes()
            flags = struct.unpack_from("<I", normalized, 0x0C)[0]
            self.assertEqual(flags & 0x87, 0)
            got, _, _ = nso_tool.read_nso(p)
            for name, raw in zip(("text", "rodata", "data"), segs):
                self.assertEqual(got[name][1], raw)
    def test_lz4_unchanged(self): self.check("lz4")

    def test_bad_magic(self):
        with self.assertRaises(ValueError):
            nso_tool.zbic_decompress(b"NOPE" + bytes(20), 4)


if __name__ == "__main__":
    unittest.main()
