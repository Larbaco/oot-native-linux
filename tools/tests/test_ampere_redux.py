#!/usr/bin/env python3
"""Synthetic REDUX (0x3c4) lowering tests. The signed MIN/MAX forms are the unsigned
ones with lane values biased by 0x80000000 and the result un-biased at the end.

  python3 -I tools/tests/test_ampere_redux.py        [env AMPERE_LOWER=path of a built ampere-lower, else it is built in a temp folder]"""
import os, pathlib, shutil, struct, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXIT = (0x794d, 0x000fea0003800000)   # generic EXIT, always taken
SIGNED = 0x200   # bit 73: signed MIN / MAX


def redux(word2):
    """One synthetic REDUX instruction; word2 supplies only the tested reduction mode."""
    return ((0x0b << 24) | (6 << 16) | 0x73c4, word2)


class ReduxLowering(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = pathlib.Path(tempfile.mkdtemp(prefix="redux_test_"))
        exe = os.environ.get("AMPERE_LOWER")
        if not exe:
            exe = str(cls.tmp / "ampere-lower")
            src = ROOT / "ampere-lower"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-w", f"-I{src}", str(src / "main.cpp"), "-o", exe], check=True)
        cls.exe = exe

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def lower(self, word2):
        prog = self.tmp / "p.bin"
        prog.write_bytes(b"".join(struct.pack("<QQ", lo, hi) for lo, hi in (redux(word2), EXIT)))
        r = subprocess.run([self.exe, str(prog), "--stage", "5", "--header-bytes", "0", "--maxwell", str(self.tmp / "p.mx"), "-o", os.devnull],
                           capture_output=True, text=True)
        out = (r.stdout + r.stderr).strip()
        if r.returncode:
            return None, out
        return int(out.split("maxwell=")[1].split()[0]), out

    def test_unsigned_forms_lower(self):
        for name, w2 in (("SUM", 0xc000), ("OR", 0x4000), ("MIN.U32", 0x10000), ("MAX.U32", 0x14000)):
            n, out = self.lower(w2)
            self.assertIsNotNone(n, f"{name}: {out}")

    def test_signed_min_max_lower(self):
        for name, w2 in (("MIN.S32", 0x10000 | SIGNED), ("MAX.S32", 0x14000 | SIGNED)):
            n, out = self.lower(w2)
            self.assertIsNotNone(n, f"{name}: {out}")

    def test_signed_adds_the_bias_xors(self):
        # one LOP32I.XOR per lane (32) plus one on the result: 33 more Maxwell instructions
        for unsigned in (0x10000, 0x14000):
            nu, _ = self.lower(unsigned)
            ns, _ = self.lower(unsigned | SIGNED)
            self.assertGreater(ns, nu)
            self.assertEqual((ns - nu) % 33, 0, f"{ns} - {nu}")

    def test_signed_sum_or_and_unknown_modes_are_refused(self):
        for w2 in (0xc000 | SIGNED, 0x4000 | SIGNED, 0x10300, 0x18000):
            n, out = self.lower(w2)
            self.assertIsNone(n, hex(w2))
            self.assertIn("native warp sum mode", out)


if __name__ == "__main__":
    unittest.main()
