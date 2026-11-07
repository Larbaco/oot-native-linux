#!/usr/bin/env python3
"""Record the game's shader containers per HOST key (SWITCH2 bring-up helper, read-only on the game).

  s2_ctl_scan.py <out dir> blob <file> [label]
  s2_ctl_scan.py <out dir> linkdata <LI.IDX> <LD.BIN>
  s2_ctl_scan.py <out dir> rdb <file.fdata|.file> ...

For every container (magic 0x19866891, version 0x00220015): the 0xe0 control block (magic
0x98761234 inside the descriptor region) and the code area. Writes <out>/<hostkey>.ctl and appends
to <out>/hostkeys.tsv: hostkey, corpuskey, stage (dump numbering), code_len, insn_bytes, group x y z,
shared, source. hostkey = sha256(control[:0xe0] + code area without its 0xe0 trailer, control +0x44 bytes)[:8], what the host's keys_hash
computes for a Switch 2 control block; corpuskey = blake2b(code after the SPH, trimmed to 16 bytes)
as glslc2_extract.py names the corpus (so the lowered code of each host key can be found).
"""
import hashlib, os, pathlib, re, struct, sys

_NX = pathlib.Path(os.environ.get("NXNATIVE") or pathlib.Path(__file__).resolve().parents[1] / "nxnative")   # nxnative checkout: its shader_split.py reads the archive formats
sys.path.insert(0, str(_NX / "dm" / "gfx" / "tools")); sys.path.insert(0, str(_NX / "dm" / "tools"))

MAGIC = b"\x91\x68\x86\x19"
CTL_MAGIC = b"\x34\x12\x76\x98"
VERSION = 0x00220015
NVN_TO_DUMP = {0: 0, 1: 4, 2: 3, 3: 1, 4: 2, 5: 5}
SEEN = set()
STATS = {"containers": 0, "written": 0, "dupes": 0, "bad": 0, "len_mismatch": 0}
OUT = None
TSV = None


def containers(d: bytes, label: str):
    for m in re.finditer(MAGIC, d):
        h = m.start()
        if h + 0xe0 > len(d) or struct.unpack_from("<I", d, h + 8)[0] != VERSION:
            continue
        total, reg_len, code_len, stage_nvn, hdr = (struct.unpack_from("<I", d, h + o)[0] for o in (0x5c, 0x60, 0xa8, 0xd4, 0xdc))
        if not (0xe0 <= hdr <= 0x400 and 0 < reg_len < 0x10000 and 0 < code_len < 0x400000 and hdr + reg_len + code_len <= total <= len(d) - h):
            STATS["bad"] += 1; continue
        stage = NVN_TO_DUMP.get(stage_nvn)
        region = d[h + hdr: h + hdr + reg_len]
        ci = region.find(CTL_MAGIC)
        if stage is None or ci < 0 or ci + 0xe0 > len(region):
            STATS["bad"] += 1; continue
        STATS["containers"] += 1
        ctl = region[ci: ci + 0xe0]
        area = d[h + hdr + reg_len: h + hdr + reg_len + code_len]
        host_len = struct.unpack_from("<I", ctl, 0x44)[0] + struct.unpack_from("<I", ctl, 0x30)[0]   # +0x44 (not +0x40): 24 of main's 160 differ by 256/512 bytes at +0x40
        if host_len != code_len:
            STATS["len_mismatch"] += 1
        gpu_len = struct.unpack_from("<I", ctl, 0x44)[0]   # what the game puts in GPU memory: instructions and padding, not the 0xe0 reflection trailer (the host hashes that length)
        keyctl = ctl[:0x20] + bytes(8) + ctl[0x28:]   # the driver rewrites control +0x20..+0x27 at run time (6 of 71 requests differed only there): hashed as zeros, as the profile key shader_key_skip makes the host do
        hostkey = hashlib.sha256(keyctl + area[:gpu_len]).hexdigest()[:16]
        if hostkey in SEEN:
            STATS["dupes"] += 1; continue
        SEEN.add(hostkey)
        header_bytes = 0 if stage == 5 else 0x80
        code = area[header_bytes:]
        code = code[: len(code) - len(code) % 16]
        corpuskey = hashlib.blake2b(code, digest_size=8).hexdigest()
        insn = struct.unpack_from("<I", ctl, 0x38)[0]
        gx, gy, gz = struct.unpack_from("<3I", ctl, 0x58)
        shared = struct.unpack_from("<I", ctl, 0xb8)[0]
        (OUT / f"{hostkey}.ctl").write_bytes(ctl)
        TSV.write(f"{hostkey}\t{corpuskey}\t{stage}\t{code_len}\t{insn}\t{gx} {gy} {gz}\t{shared}\t{label}@{h:#x}\n")
        STATS["written"] += 1


def do_blob(path, label=None):
    containers(pathlib.Path(path).read_bytes(), label or os.path.basename(path))


def do_linkdata(idx_path, bin_path):
    import shader_split as ss
    idx = pathlib.Path(idx_path).read_bytes()
    n = len(idx) // 32
    with open(bin_path, "rb") as f:
        for i in range(n):
            off, inflated, stored, comp = struct.unpack_from("<QQQQ", idx, i * 32)
            if not inflated:
                continue
            f.seek(off); raw = f.read(stored if comp else inflated)
            data = (ss.kt_gz(raw) if comp else None) or raw
            if MAGIC in data:
                containers(data, f"linkdata#{i}")


def do_rdb(paths):
    import mmap
    import shader_split as ss
    for p in paths:
        with open(p, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as d:
            name = os.path.basename(p); n = 0
            for a, b, flags in ss.idrk_entries(d):
                n += 1
                try:
                    data = ss.idrk_data(d, a, b, flags)
                except Exception:
                    continue
                if MAGIC in data:
                    containers(data, f"{name}#IDRK@{a:#x}")
            print(f"{name}: {n} entries scanned, containers so far {STATS['containers']}", flush=True)


def main():
    global OUT, TSV
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    OUT = pathlib.Path(sys.argv[1]); OUT.mkdir(parents=True, exist_ok=True)
    tsv_path = OUT / "hostkeys.tsv"
    if tsv_path.exists():
        for l in tsv_path.read_text().splitlines():
            if l and not l.startswith("#"):
                SEEN.add(l.split("\t")[0])
    new = not tsv_path.exists()
    TSV = open(tsv_path, "a", encoding="utf-8")
    if new:
        TSV.write("# hostkey\tcorpuskey\tstage\tcode_len\tinsn_bytes\tgroup\tshared\tsource\n")
    mode, args = sys.argv[2], sys.argv[3:]
    if mode == "blob":
        do_blob(*args)
    elif mode == "linkdata":
        do_linkdata(*args)
    elif mode == "rdb":
        do_rdb(args)
    else:
        sys.exit(__doc__)
    TSV.close()
    print(f"done: {STATS}")


if __name__ == "__main__":
    main()
