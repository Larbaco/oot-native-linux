#!/usr/bin/env python3
"""Extract Switch 2 NVN shader containers (GLSLC output version 0x00220015) into the program-dump
format that shader_pipeline.py / ampere-spirv read: <key>-stage<N>-<hdr>.program.bin + <key>.txt.

  glslc2_extract.py <out dir> blob <file> [label]          a plain blob (e.g. main.rodata)
  glslc2_extract.py <out dir> linkdata <LI.IDX> <LD.BIN>  Koei Tecmo LINKDATA (32-byte index entries)
  glslc2_extract.py <out dir> rdb <file.fdata|.file> ...  Koei Tecmo RDB data files (IDRK entries)

Container layout (validated on the Pokopia corpus of the upstream project, 28/28 run-time programs): magic 0x19866891 at +0,
version 0x00220015 at +8, total length +0x5c, header length +0xdc, control length +0x60,
code length +0xa8, stage +0xd4 (0 vertex, 1 fragment, 5 compute; NVN order), code area =
header + control; graphics code areas start with a 0x80 shader program header, compute with none.
Reads the inputs read-only; writes only into <out dir>. Only shader programs are written.
"""
import hashlib, os, pathlib, re, struct, sys, zlib

_NX = pathlib.Path(os.environ.get("NXNATIVE") or pathlib.Path(__file__).resolve().parents[1] / "nxnative")   # nxnative checkout: its shader_split.py reads the archive formats
sys.path.insert(0, str(_NX / "dm" / "gfx" / "tools")); sys.path.insert(0, str(_NX / "dm" / "tools"))

MAGIC = b"\x91\x68\x86\x19"
VERSION = 0x00220015
# NVN shader stage order -> Eden dump stage numbers (0 VertexB, 1 TessControl, 2 TessEval, 3 Geometry, 4 Fragment, 5 Compute)
NVN_TO_DUMP = {0: 0, 1: 4, 2: 3, 3: 1, 4: 2, 5: 5}
SEEN = set()
STATS = {"containers": 0, "written": 0, "dupes": 0, "bad": 0}

def containers(d: bytes, label: str, out: pathlib.Path):
    for m in re.finditer(MAGIC, d):
        h = m.start()
        if h + 0xe0 > len(d) or struct.unpack_from("<I", d, h + 8)[0] != VERSION:
            continue
        total, ctl, code_len, stage_nvn, hdr = (struct.unpack_from("<I", d, h + o)[0] for o in (0x5c, 0x60, 0xa8, 0xd4, 0xdc))
        if not (0xe0 <= hdr <= 0x400 and 0 < ctl < 0x10000 and 0 < code_len < 0x400000 and hdr + ctl + code_len <= total <= len(d) - h):
            STATS["bad"] += 1; continue
        STATS["containers"] += 1
        stage = NVN_TO_DUMP.get(stage_nvn)
        if stage is None:
            STATS["bad"] += 1; continue
        area = d[h + hdr + ctl: h + hdr + ctl + code_len]
        header_bytes = 0 if stage == 5 else 0x80
        if len(area) <= header_bytes:
            STATS["bad"] += 1; continue
        code = area[header_bytes:]
        code = code[: len(code) - len(code) % 16]
        key = hashlib.blake2b(code, digest_size=8).hexdigest()
        if key in SEEN:
            STATS["dupes"] += 1; continue
        SEEN.add(key)
        hdr_key = hashlib.blake2b(area[:header_bytes], digest_size=8).hexdigest() if header_bytes else "0" * 16
        (out / f"{key}-stage{stage}-{hdr_key}.program.bin").write_bytes(area[:header_bytes] + code)
        (out / f"{key}.txt").write_text(f"stage {stage}\nprogram 0\nheader_bytes {header_bytes}\nsource_bytes {len(code)}\nsource {label}@{h:#x}\n")
        STATS["written"] += 1

def do_blob(out, path, label=None):
    containers(pathlib.Path(path).read_bytes(), label or os.path.basename(path), out)

def do_linkdata(out, idx_path, bin_path):
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
                containers(data, f"linkdata#{i}", out)

def do_rdb(out, paths):
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
                    containers(data, f"{name}#IDRK@{a:#x}", out)
            print(f"{name}: {n} entries scanned, containers so far {STATS['containers']}", flush=True)

def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    out = pathlib.Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    mode, args = sys.argv[2], sys.argv[3:]
    if mode == "blob":
        do_blob(out, *args)
    elif mode == "linkdata":
        do_linkdata(out, *args)
    elif mode == "rdb":
        do_rdb(out, args)
    else:
        sys.exit(__doc__)
    print(f"done: {STATS}")

if __name__ == "__main__":
    main()
