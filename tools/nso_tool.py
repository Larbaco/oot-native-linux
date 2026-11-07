#!/usr/bin/env python3
"""Read-only NSO inspector for Phase A (Eden-NS2 feasibility study).

  nso_tool.py <nso> --out <dir>   writes <dir>/<name>.text/.rodata/.data (decompressed),
                                  <dir>/<name>.strings.txt (printable strings >= 6 chars),
                                  <dir>/<name>.svc.txt (SVC numbers used, with counts),
                                  <dir>/<name>.nvn.txt (distinct nvn* identifiers found),
                                  <dir>/<name>.info.txt (segment sizes, build id, module name)
Never writes to the input location.
"""
import argparse, os, re, shutil, struct, subprocess, sys
from collections import Counter
from pathlib import Path
import lz4.block

def zbic_decompress(raw: bytes, size: int) -> bytes:
    """NSO flags bit 7 (SDK >= 22): decompress in the isolated GPL helper process."""
    if raw[:4] != b"ZBIC":
        raise ValueError("ZBIC segment without the ZBIC magic")
    tool = os.environ.get("NXS2_ZBIC_UNPACK") or shutil.which("zbic-unpack")
    if not tool:
        raise RuntimeError("ZBIC data needs the separate zbic-unpack helper in PATH")
    result = subprocess.run([tool, "--stream"], input=raw, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace").strip() or "zbic-unpack failed")
    out = result.stdout
    if len(out) != size:
        raise ValueError(f"ZBIC size mismatch: {len(out)} != {size}")
    return out

def read_nso(path: Path):
    data = path.read_bytes()
    if data[:4] != b"NSO0":
        sys.exit(f"{path}: not an NSO0 file")
    flags = struct.unpack_from("<I", data, 0x0C)[0]
    segs = {}
    for i, name in enumerate(("text", "rodata", "data")):
        file_off, mem_off, size = struct.unpack_from("<III", data, 0x10 + i * 0x10)
        comp_size = struct.unpack_from("<I", data, 0x60 + i * 4)[0]
        raw = data[file_off:file_off + comp_size]
        if flags & (1 << i):
            raw = zbic_decompress(raw, size) if flags & 0x80 else lz4.block.decompress(raw, uncompressed_size=size)
        segs[name] = (mem_off, raw)
    build_id = data[0x40:0x60].hex()
    mod_name_off, mod_name_size = struct.unpack_from("<II", data, 0x1C), None
    bss = struct.unpack_from("<I", data, 0x3C)[0]
    return segs, build_id, bss

STR_RE = re.compile(rb"[\x20-\x7e]{6,}")
NVN_RE = re.compile(rb"\bnvn[A-Z][A-Za-z0-9_]+")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    name = a.nso.name
    segs, build_id, bss = read_nso(a.nso)
    info = [f"file {a.nso}", f"build_id {build_id}", f"bss {bss:#x}"]
    strings, nvn = [], set()
    for seg, (mem_off, raw) in segs.items():
        (a.out / f"{name}.{seg}").write_bytes(raw)
        info.append(f"{seg} mem_off={mem_off:#x} size={len(raw):#x}")
        for m in STR_RE.finditer(raw):
            s = m.group().decode("ascii")
            strings.append(f"{seg}+{m.start():#x}\t{s}")
        nvn.update(m.group().decode() for m in NVN_RE.finditer(raw))
    text = segs["text"][1]
    svcs = Counter()
    for off in range(0, len(text) - 3, 4):
        w = struct.unpack_from("<I", text, off)[0]
        if (w & 0xFFE0001F) == 0xD4000001:          # SVC #imm16
            svcs[(w >> 5) & 0xFFFF] += 1
    (a.out / f"{name}.strings.txt").write_text("\n".join(strings), encoding="utf-8")
    (a.out / f"{name}.nvn.txt").write_text("\n".join(sorted(nvn)), encoding="utf-8")
    (a.out / f"{name}.svc.txt").write_text(
        "\n".join(f"{n:#04x}\t{c}" for n, c in sorted(svcs.items())), encoding="utf-8")
    sdk = sorted({s.split("\t", 1)[1] for s in strings if "SDK" in s and ("nnSdk" in s or "Version" in s)})[:5]
    info.append(f"svc_distinct {len(svcs)} svc_sites {sum(svcs.values())}")
    info.append(f"nvn_identifiers {len(nvn)}")
    info.append("sdk_strings " + " | ".join(sdk))
    (a.out / f"{name}.info.txt").write_text("\n".join(info), encoding="utf-8")
    print("\n".join(info))

if __name__ == "__main__":
    main()
