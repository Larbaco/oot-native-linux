#!/usr/bin/env python3
"""Synthetic Ampere lowering cases for instruction forms used by the OoT shader path.
The operand fields below are intentionally chosen test values, not captured program words.
Each synthetic instruction is lowered alone, followed by a generic EXIT; the check is
"no LOWER_FAIL", not the particular Maxwell encoding.

  python3 -I tools/tests/test_ampere_ocarina_compute.py   [env AMPERE_LOWER=path of a built ampere-lower, else it is built in a temp folder]"""
import os, pathlib, shutil, struct, subprocess, tempfile, unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXIT = (0x000000000000794d, 0x000fea0003800000)

FORMS = {
    "VOTEU synthetic destination":    (0x00177886, 0x00000000, 0x030a0100, 0x000fca00),
    "UISETP guarded by UP2":          (0x1208728c, 0x0000003f, 0x0bfa5070, 0x000fe200),
    "UPRMT sign-extend of a halfword": (0x090d7896, 0x00009910, 0x0800003f, 0x000fcc00),
    "F2FP.U8 pack of two floats":     (0x110c7243, 0x0000000b, 0x000004ff, 0x000fc400),
    "F2FP.U8 of one float (Ra = RZ)":  (0xff0e7243, 0x00000006, 0x000004ff, 0x000fe400),
    "ULDC.32 indexed, odd immediate":  (0x090b7abb, 0x01000a40, 0x08000800, 0x000fc600),
    "UPLOP3 with a synthetic table":  (0x0000789c, 0x00000000, 0x00721000, 0x000fcc00),
    "IMAD.HI.U32 synthetic operands":   (0x11127827, 0x13579bdf, 0x078e00ff, 0x000fe200),
    "IABS synthetic operand":          (0x000c7213, 0x00000053, 0x00000000, 0x000fe400),
    "S2R synthetic register":         (0x000f7919, 0x00000000, 0x00000000, 0x000e2600),
}


def pack(words):
    return struct.pack("<QQ", words[0] | (words[1] << 32), words[2] | (words[3] << 32))


class OcarinaCompute(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = pathlib.Path(tempfile.mkdtemp(prefix="ocarina_test_"))
        exe = os.environ.get("AMPERE_LOWER")
        if not exe:
            exe = str(cls.tmp / "ampere-lower")
            src = ROOT / "ampere-lower"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O1", "-w", f"-I{src}", str(src / "main.cpp"), "-o", exe], check=True)
        cls.exe = exe

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def lower(self, words):
        prog = self.tmp / "p.bin"
        prog.write_bytes(pack(words) + struct.pack("<QQ", *EXIT))
        r = subprocess.run([self.exe, str(prog), "--stage", "5", "--header-bytes", "0", "--maxwell", str(self.tmp / "p.mx")], capture_output=True, text=True)
        return r.returncode, (r.stdout + r.stderr).strip()

    def test_each_form_lowers(self):
        for name, words in FORMS.items():
            with self.subTest(name):
                code, out = self.lower(words)
                self.assertEqual(code, 0, out)
                self.assertTrue(out.startswith("OK"), out)

    def test_pack_of_two_floats_is_longer_than_one(self):
        two = int(self.lower(FORMS["F2FP.U8 pack of two floats"])[1].split("maxwell=")[1])
        one = int(self.lower(FORMS["F2FP.U8 of one float (Ra = RZ)"])[1].split("maxwell=")[1])
        self.assertGreater(two, one)

    def test_unguarded_vs_guarded_compare(self):
        guarded = FORMS["UISETP guarded by UP2"]
        unguarded = (guarded[0] | 0x7000, *guarded[1:])
        g = int(self.lower(guarded)[1].split("maxwell=")[1])
        u = int(self.lower(unguarded)[1].split("maxwell=")[1])
        self.assertEqual(g, u)   # the guard only changes the predicate field of the compare


if __name__ == "__main__":
    unittest.main()
