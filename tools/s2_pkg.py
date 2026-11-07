#!/usr/bin/env python3
"""Package the game's lowered shaders for nxnative's chain (SWITCH2 bring-up helper).

  s2_pkg.py <corpus-ctl dir> <lowered dir> <pkg out dir> <tables out dir> [<corpus dir>...]

Inputs: <corpus-ctl>/hostkeys.tsv and <hostkey>.ctl from s2_ctl_scan.py; <lowered>/<corpuskey>.mx.bin
from lower_corpus.py (0x50 SPH + Maxwell instructions with scheduling words + Eden's address map).
Outputs, in the shape dm/gfx/tools/shader_split.py writes: <pkg>/<stage>_<suffix>.bin record files
('BTCDBTCD' + 0x30 header with +0x28 control length and +0x2c code length, then code area, then a
0x880-byte control section), <tables>/shader_map.tsv, shader_facts.tsv, shader_pairs.tsv.
The code area is the Switch 1 form the chain strips: graphics = 0x30 header (magic 0x12345678) +
the lowered 0x50 SPH + instructions; compute = 0x30 header (magic 0x12345679) + zero padding to
0x100 + instructions. The synthesized control section carries only what the chain reads: its own
length at +0x20, c1 start and code length at +0x700/+0x704, and for compute the thread group at
+0x748 and shared memory at +0x754 from the Switch 2 control block.
c1 (the shader's immediate constant bank, e.g. a full-screen vertex shader's three positions) lies in
the Switch 2 code area between control +0x40 and +0x44 (the end of the uploaded code); it is copied
from the original program (<corpus dir>/<corpuskey>-*.program.bin or a dump) behind the lowered
instructions, where the chain's +0x700/+0x704 convention expects it.
"""
import collections, json, os, pathlib, struct, sys

CTL_MAGIC = bytes.fromhex("34127698")
GFX_MAGIC = bytes.fromhex("78563412")
CS_MAGIC = bytes.fromhex("79563412")
STAGE = {0: "vs", 1: "hs", 2: "ds", 3: "gs", 4: "ps", 5: "cs"}
# Eden lays lowered code out with scheduling words at absolute 32-byte boundaries after a 0x50 SPH (first one at 0x60);
# Ryujinx expects them at 32-byte boundaries from the code start (0x50 + 32k). A 16-byte prefix (one scheduling word, one NOP)
# shifts the whole body by 0x10 so both agree; relative branches are unchanged by a uniform shift.
SHIFT = bytes(8) + (0x50B0000000070F00).to_bytes(8, "little")   # sched 0, NOP
STAGE_NAME = {"vs": "vertex", "hs": "hull", "ds": "domain", "gs": "geometry", "ps": "pixel", "cs": "compute"}


MAP_MAGIC = 0x50414d4e4544454e   # Eden's native address map, appended to the lowered code (program_address_map.h)


def relocate_brx_tables(low, c1, stat):
    """Eden lowers an Ampere BRX to `BRX Ra, -(address)-8`, so the Maxwell target is the absolute value the shader read from its
    c1 jump table, and that value is an Ampere program address. Rewrite every table entry to the lowered address through Eden's
    map, so the translator's targets and the shader's run-time comparison agree. A table is found from each BRX's LDC c[1][R+base]."""
    n = len(low) // 8; words = struct.unpack(f"<{n}Q", low[: n * 8])
    if n < 3 or words[-1] != MAP_MAGIC:
        return c1
    count = words[-2]; amap = words[n - 2 - count: n - 2]; ncode = n - 2 - count
    c1 = bytearray(c1); done = set(); tables = 0
    for i in range(10, ncode):
        if (i * 8) % 32 == 0 or words[i] >> 52 != 0xE25:
            continue
        ra = (words[i] >> 8) & 0xFF; base = None
        for j in range(i - 1, max(9, i - 96), -1):
            w = words[j]
            if (j * 8) % 32 == 0: continue
            if w >> 52 == 0xEF9 and (w & 0xFF) == ra:
                if (w >> 36) & 0xF == 1: base = (w >> 20) & 0xFFFF
                break
        if base is None or base in done:
            if base is None: stat["brx without c1 table"] += 1
            continue
        done.add(base); k = 0
        while base + 4 * k + 4 <= len(c1) and k < 1024:
            e = struct.unpack_from("<I", c1, base + 4 * k)[0]
            if e == 0 or e % 16 or e // 16 >= count or amap[e // 16] < 0x50:
                break
            struct.pack_into("<I", c1, base + 4 * k, amap[e // 16]); k += 1
        if k: tables += 1; stat["brx table entries relocated"] += k
        else: stat["brx table empty"] += 1
    if tables: stat["brx tables"] += tables
    return bytes(c1)


PKG_SUFFIX = os.environ.get("S2_PKG_SUFFIX", "oot")   # profile.tsv pkg_suffix (nxs2.py / the launcher pass it); the package files are <stage>_<suffix>.bin
GAME_NAME = os.environ.get("S2_GAME_NAME", "The Legend of Zelda: Ocarina of Time")


def main():
    ctl_dir, low_dir, pkg, tables = (pathlib.Path(a) for a in sys.argv[1:5])
    pkg.mkdir(parents=True, exist_ok=True); tables.mkdir(parents=True, exist_ok=True)
    progs = {}
    for d in sys.argv[5:]:
        for f in pathlib.Path(d).glob("*.program.bin"): progs.setdefault(f.name.split("-")[0], f)
    rows_in = [l.split("\t") for l in (ctl_dir / "hostkeys.tsv").read_text(encoding="utf-8").splitlines() if l and not l.startswith("#")]
    recs = collections.defaultdict(list); rows = []; facts = {}; stat = collections.Counter(); seen = set()
    for hostkey, corpuskey, stage, code_len, insn, group, shared, source in rows_in:
        if hostkey in seen:
            continue
        seen.add(hostkey)
        st = STAGE[int(stage)]
        mx = low_dir / f"{corpuskey}.mx.bin"
        if not mx.exists():
            stat["no lowered code"] += 1; continue
        low = mx.read_bytes()
        if st == "cs":
            code = bytearray(CS_MAGIC + bytes(0x100 - 4)) + SHIFT + low[0x50:]
        else:
            code = bytearray(GFX_MAGIC + bytes(0x30 - 4)) + low[:0x50] + SHIFT + low[0x50:]
        if CTL_MAGIC in code:
            stat["control magic inside code"] += 1; continue
        # c1: the constant bank between Switch 2 control +0x40 and +0x44 of the original code area, appended behind the lowered instructions
        c1 = b""
        s2ctl = (ctl_dir / f"{hostkey}.ctl").read_bytes()
        c40, c44 = struct.unpack_from("<2I", s2ctl, 0x40)
        if c44 > c40 and (ctl_dir / f"{hostkey}.c1").exists():   # a run-time dump's constants (s2_dumps.py)
            c1 = (ctl_dir / f"{hostkey}.c1").read_bytes(); stat["c1 from dump"] += 1
        elif c44 > c40:
            orig = progs.get(corpuskey)
            if orig is None:
                stat["c1 without original program"] += 1
            else:
                ob = orig.read_bytes()
                if len(ob) >= c44:
                    c1 = ob[c40:c44]; stat["c1 copied"] += 1
                else:
                    stat["c1 beyond the program"] += 1
        if c1: c1 = relocate_brx_tables(low, c1, stat)
        c1_at = len(code); code += c1
        ctl = bytearray(0x880)
        ctl[0:4] = CTL_MAGIC
        struct.pack_into("<I", ctl, 0x20, 0x880)
        struct.pack_into("<2I", ctl, 0x700, c1_at, len(code))
        if st == "cs":
            x, y, z = (int(v) for v in group.split())
            sh = int(shared)
            if not (1 <= x <= 1024 and 1 <= y <= 1024 and 1 <= z <= 64):
                x, y, z = 64, 1, 1; stat["compute group defaulted"] += 1
            if not 0 < sh <= 0x8000:   # the Switch 2 block's shared-memory field is not identified yet; Apple GPUs allow 32 KB of threadgroup memory
                sh = 0x8000; stat["shared defaulted"] += 1
            struct.pack_into("<3I", ctl, 0x748, x, y, z)
            struct.pack_into("<I", ctl, 0x754, sh)
            facts[hostkey] = {"local": [x, y, z], "shared": (sh + 15) // 16 * 16}
        rec = bytearray(b"BTCDBTCD" + bytes(0x30)); struct.pack_into("<2I", rec, 0x28, len(ctl), len(code)); rec += code + ctl
        assert rec.find(CTL_MAGIC) == 0x38 + len(code)
        name = f"{st}_{PKG_SUFFIX}.bin"
        rows.append((name, len(recs[name]), STAGE_NAME[st], hostkey, source)); recs[name].append(bytes(rec)); stat[st] += 1
    for f in pkg.glob("*.bin"):
        f.unlink()
    for name, rr in recs.items():
        (pkg / name).write_bytes(b"".join(rr))
    with open(tables / "shader_map.tsv", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# kind: evidence  SWITCH2: {GAME_NAME} GLSLC 0x00220015 containers (main + LINKDATA + RDB) keyed as the host keys Switch 2 control blocks (s2_ctl_scan.py), code lowered to Maxwell by Eden-NS2 (lower_corpus.py), packaged by s2_pkg.py\n")
        f.write("# column 5 records where each key first appears; no PC shader pairing supplied\n")
        f.write("package\tindex\tstage\tdm_ctl_sha16\tsource\n")
        for r in rows:
            f.write("\t".join(map(str, r)) + "\n")
    with open(tables / "shader_facts.tsv", "w", encoding="utf-8", newline="\n") as f:
        f.write("# Answer table for translation queries (SWITCH2 s2_pkg.py): key <TAB> fact JSON. Only the thread group / shared memory of compute shaders, from the Switch 2 control block (+0x58, +0xb8).\n")
        for k in sorted(facts):
            f.write(k + "\t" + json.dumps(facts[k], sort_keys=True, separators=(",", ":")) + "\n")
    with open(tables / "shader_pairs.tsv", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# (vertex key, pixel key) pairs: none known for {GAME_NAME} yet (SWITCH2 s2_pkg.py)\n")
    print("host keys", len(seen), "records", len(rows), dict(stat))


if __name__ == "__main__":
    main()
