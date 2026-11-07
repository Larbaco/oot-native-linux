#!/usr/bin/env python3
"""Shader programs of a Nintendo EPD engine game (Switch 2 NVN2): bfsha/BNSH, SARC, sharcb, NDSR, effect files.

  epd_shaders.py <shader dir> --romfs <RomFS.img or folder> --sources <shader_sources.tsv>

Reads the RomFS read-only and writes what the Koei path gets from glslc2_extract.py and s2_ctl_scan.py, so
lower_corpus.py, s2_pkg.py and nxshader.py take it unchanged:
  <shader dir>/corpus/epd/<corpus key>-stage<N>-<hdr>.program.bin + <corpus key>.txt
  <shader dir>/corpus-ctl/<host key>.ctl + hostkeys.tsv
Task and mesh programs have no path through the Maxwell translator, so they go to a folder of their own, with the
same names, for measuring the lowering alone:
  <shader dir>/corpus-mesh/<corpus key>-stage<N>-<hdr>.program.bin + <corpus key>.txt, hostkeys.tsv
  <shader dir>/epd_report.json: containers, programs per stage and source, everything skipped and why

The containers (documented in docs/SHADERS-EPD.md):
  .zs        a zstd frame, usually against a dictionary of Pack/ZsDic.pack.zs (chosen by the frame's dictionary ID)
  SARC       Nintendo's archive; members are walked. FSHA (bfsha), BYML (effect .vsetb) and FRES hold BNSH files
  BNSH       variations -> program -> stage records in slots (0 vertex, 1 tess control, 2 tess eval, 3 geometry,
             4 fragment, 5 compute, 7 task, 8 mesh), each pointing at one control block and one code area. Records share them
  BAHS       .sharcb: a control block is preceded by the u64 code offset (-0x68) and its u32 size (-0x60); the offset
             counts from the code section (header +0x1c)
  NDSR       .pck (DLSS): a table of u32 rows: control offset, size << 4 | tag, code offset, size << 4 | tag
Layout of a control block as read here: magic 0x98761234, +4 = 0xc9, +0x38 end of the instructions, +0x40..+0x44 the
c1 constant bank at the end of the code area, +0x44 the bytes uploaded to GPU memory, +0x54 >> 16 the NVN stage.
The BNSH variation layout follows KillzXGaming's ShaderLibrary (MIT), the records past compute were found here.
"""
import argparse, collections, fnmatch, hashlib, json, os, pathlib, re, shutil, struct, subprocess, sys

CTL_MAGIC = bytes.fromhex("34127698")
ZSTD_MAGIC = bytes.fromhex("28b52ffd")
ZBIC_MAGIC = b"ZBIC"
BNSH_MAGIC = b"BNSH\0\0\0\0"
DICT_PACK = "Pack/ZsDic.pack.zs"
CTL_MIN = 0xe0
NVN_TO_DUMP = {0: 0, 1: 4, 2: 3, 3: 1, 4: 2, 5: 5, 6: 6, 7: 7}   # NVN stage in the control block -> stage number of the corpus
SLOT_TO_DUMP = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 7: 6, 8: 7}   # slot of a BNSH program's record -> stage number of the corpus
STAGE_NAME = ("vertex", "tess-control", "tess-eval", "geometry", "fragment", "compute", "task", "mesh")
COMPUTE, TASK, MESH = 5, 6, 7
HEADER_BYTES = 0x80


class Bad(ValueError):
    """A container that does not parse as what its magic says"""


def _take(d, off, n):
    if off < 0 or n < 0 or off + n > len(d): raise Bad(f"range {off:#x}+{n:#x} outside {len(d):#x} bytes")
    return d[off:off + n]


def _u32(d, off): return struct.unpack("<I", _take(d, off, 4))[0]
def _u64(d, off): return struct.unpack("<Q", _take(d, off, 8))[0]


class Blob:
    """One program: the control block and the code area, and where they were found"""
    __slots__ = ("stage", "ctl", "code", "source")

    def __init__(self, stage, ctl, code, source):
        self.stage, self.ctl, self.code, self.source = stage, ctl, code, source


def sarc_members(d):
    """(name, bytes) of a little-endian SARC"""
    if _take(d, 0, 4) != b"SARC" or d[6:8] != b"\xff\xfe": raise Bad("not a little-endian SARC")
    head, _, base = struct.unpack("<H", d[4:6])[0], _u32(d, 8), _u32(d, 12)
    if _take(d, head, 4) != b"SFAT": raise Bad("no SFAT")
    sfat_len, count = struct.unpack("<HH", _take(d, head + 4, 4))
    nodes = head + sfat_len
    sfnt = nodes + count * 0x10
    if _take(d, sfnt, 4) != b"SFNT": raise Bad("no SFNT")
    names = sfnt + struct.unpack("<H", _take(d, sfnt + 4, 2))[0]
    out = []
    for i in range(count):
        flags, a, b = struct.unpack("<3I", _take(d, nodes + i * 0x10 + 4, 12))
        end = d.find(b"\0", names + (flags & 0xffffff) * 4)
        if flags >> 24 != 1 or end < 0: raise Bad("SARC member without a name")
        out.append((d[names + (flags & 0xffffff) * 4:end].decode("utf-8"), _take(d, base + a, b - a)))
    return out


def bnsh_size(d, off):
    return _u32(d, off + 0x1c)


def bnsh_programs(b, source):
    """Every distinct stage record of a BNSH. Variations point at shared records, so the pointer pair identifies a program."""
    if b[:8] != BNSH_MAGIC or _take(b, 0x60, 4) != b"grsc": raise Bad("bad BNSH header")
    count, variations = _u32(b, 0x7c), _u64(b, 0x80)
    seen, out = set(), []
    for v in range(count):
        program = _u64(b, variations + v * 0x40 + 0x10)
        if not program: raise Bad(f"variation {v} without a program")
        for slot, stage in SLOT_TO_DUMP.items():
            rec = _u64(b, program + 8 + slot * 8)
            if not rec: continue
            ctl_off, code_off = _u64(b, rec + 8), _u64(b, rec + 16)
            code_len, ctl_len = struct.unpack("<2I", _take(b, rec + 24, 8))
            if (ctl_off, code_off) in seen: continue
            seen.add((ctl_off, code_off))
            out.append((stage, ctl_off, ctl_len, code_off, code_len))
    return [Blob(stage, _take(b, co, cl), _take(b, do, dl), f"{source}@{do:#x}") for stage, co, cl, do, dl in out]


def ctl_offsets(d):
    i = d.find(CTL_MAGIC)
    while i >= 0:
        if i + CTL_MIN <= len(d) and d[i + 4] == 0xc9 and d[i + 5:i + 8] == b"\0\0\0": yield i
        i = d.find(CTL_MAGIC, i + 4)


def _nvn_slot(ctl):
    nvn = _u32(ctl, 0x54) >> 16
    if nvn not in NVN_TO_DUMP: raise Bad(f"control block with NVN stage {nvn}")
    return NVN_TO_DUMP[nvn]


def sharcb_programs(d, source):
    base = _u32(d, 0x1c)
    out = []
    # BAHS records may refer back to a shared control block. The signed
    # displacement at +0xc is relative to the record, not the pointer field.
    # Inline blocks are simply the special case displacement == 0x68.
    seen = set()
    for rec in range(0, min(base, len(d)) - 15, 8):
        ctl_len = _u32(d, rec + 8)
        if not CTL_MIN <= ctl_len <= 0x1000: continue
        c = rec + struct.unpack_from("<i", d, rec + 12)[0]
        if c < 0 or c + ctl_len > len(d) or d[c:c + 8] != CTL_MAGIC + b"\xc9\0\0\0": continue
        code_off = base + _u64(d, rec)
        if (c, code_off) in seen: continue
        seen.add((c, code_off))
        ctl = _take(d, c, ctl_len); n = _u32(ctl, 0x44)
        # Reject accidental pointer-shaped words in reflection metadata.
        if not n or n % 16 or code_off < base or code_off + n > len(d): continue
        if not header_ok(_nvn_slot(ctl), d[code_off:code_off + n]): continue
        out.append(Blob(_nvn_slot(ctl), ctl, _take(d, code_off, n), f"{source}@{code_off:#x}"))
    return out


def ndsr_programs(d, source):
    """A control block's table row is found by value and checked by sizes (the low 4 bits of a size word are a tag that varies: 6 and 7, or 0 and 1)"""
    out = []
    for c in ctl_offsets(d):
        key = struct.pack("<I", c); row = -1; i = d.find(key, 0, c); want = _u32(d, c + 0x44)
        while i >= 0:
            if i % 4 == 0 and i + 16 <= len(d):
                ctl_tag, code_off, code_tag = struct.unpack_from("<3I", d, i + 4)
                if ctl_tag >> 4 >= CTL_MIN and code_tag >> 4 == want: row = i; break
            i = d.find(key, i + 1, c)
        if row < 0: raise Bad(f"control block at {c:#x}: no table row")
        ctl_len, code_len = ctl_tag >> 4, code_tag >> 4
        out.append(Blob(_nvn_slot(_take(d, c, ctl_len)), _take(d, c, ctl_len), _take(d, code_off, code_len), f"{source}@{code_off:#x}"))
    return out


class Zstd:
    """.zs frames: the dictionary comes from the frame's ID; ZBIC is delegated to its isolated GPL helper process."""
    def __init__(self, dict_pack):
        import zstandard
        self.z = zstandard
        raw = self.z.ZstdDecompressor().decompress(dict_pack, max_output_size=1 << 28) if dict_pack else None
        self.dicts = {}
        for _, b in (sarc_members(raw) if raw else ()):
            if b[:4] == bytes.fromhex("37a430ec"): self.dicts[_u32(b, 4)] = self.z.ZstdCompressionDict(b)

    def decompress(self, d):
        if d[:4] == ZBIC_MAGIC:
            tool = os.environ.get("NXS2_ZBIC_UNPACK") or shutil.which("zbic-unpack")
            if not tool: raise Bad("ZBIC data needs the separate zbic-unpack helper in PATH")
            try: p = subprocess.run([tool, "--stream"], input=d, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            except OSError as e: raise Bad(f"could not start zbic-unpack: {e}") from e
            if p.returncode: raise Bad(p.stderr.decode("utf-8", "replace").strip() or "zbic-unpack failed")
            return p.stdout
        dic = self.dicts.get(self.z.get_frame_parameters(d).dict_id)
        return (self.z.ZstdDecompressor(dict_data=dic) if dic else self.z.ZstdDecompressor()).decompress(d, max_output_size=1 << 31)


def walk(d, label, zs, report, depth=0):
    """Every program under a file: containers are told by magic, not by file name"""
    if depth > 8: raise Bad("containers nested too deep")
    head = d[:4]
    if head in (ZSTD_MAGIC, ZBIC_MAGIC):
        yield from walk(zs.decompress(d), label, zs, report, depth + 1); return
    if head == b"SARC":
        for name, member in sarc_members(d):
            try: yield from walk(member, f"{label}!{name}", zs, report, depth + 1)
            except (Bad, struct.error, UnicodeDecodeError) as e: report["errors"][f"{label}!{name}"] = str(e)
        return
    if head == b"BAHS":
        report["containers"]["sharcb"] += 1; yield from sharcb_programs(d, label); return
    if head == b"NDSR":
        report["containers"]["ndsr"] += 1; yield from ndsr_programs(d, label); return
    spans, off = [], d.find(BNSH_MAGIC)   # FSHA, BYML, FRES, a bare BNSH: BNSH files are aligned blocks inside, each knowing its size
    while off >= 0:
        size = bnsh_size(d, off)
        report["containers"]["bnsh"] += 1
        yield from bnsh_programs(_take(d, off, size), f"{label}!BNSH@{off:#x}")
        spans.append((off, off + size)); off = d.find(BNSH_MAGIC, off + size)
    orphans = sum(1 for c in ctl_offsets(d) if not any(a <= c < b for a, b in spans))
    if orphans: report["control_blocks_outside_a_container"][label] = orphans


class RomFS:
    """A RomFS folder, or the DMROMFS1 image the builder writes (romfs_image.py)"""
    HEAD, ENT = struct.Struct("<8sQQQ"), struct.Struct("<QQIHBB")

    def __init__(self, path):
        self.path = pathlib.Path(path); self.files = {}
        if self.path.is_dir():
            self.files = None; return
        with open(self.path, "rb") as f:
            magic, ioff, isz, n = self.HEAD.unpack(f.read(self.HEAD.size))
            if magic != b"DMROMFS1": raise SystemExit(f"{path} is not a RomFS image or folder")
            f.seek(ioff); raw = f.read(isz)
        pool = raw[n * self.ENT.size:]
        stack = [(0, "")]
        while stack:
            k, pre = stack.pop()
            a, b, no, nl, typ, _ = self.ENT.unpack_from(raw, k * self.ENT.size)
            name = pool[no:no + nl].decode("utf-8", "surrogateescape")
            p = f"{pre}/{name}" if pre else name
            if typ == 1: self.files[p] = (a, b)
            else: stack.extend((c, p) for c in range(a, a + b))
        self.f = open(self.path, "rb")

    def names(self):
        if self.files is not None: return sorted(self.files)
        return sorted(p.relative_to(self.path).as_posix() for p in self.path.rglob("*") if p.is_file())

    def read(self, rel):
        if self.files is None: return (self.path / rel).read_bytes()
        a, b = self.files[rel]
        self.f.seek(a); return self.f.read(b)


def read_sources(path):
    """The kinds this tool reads from shader_sources.tsv: bfsha, sarc (paths) and glob (patterns; effect files number in the thousands)"""
    out = []
    for l in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        if l.strip() and not l.startswith("#"):
            kind, *rest = l.split("\t")
            if kind in ("bfsha", "sarc", "glob"): out += [(kind, p) for p in rest]
    return out


def header_ok(stage, code):
    """A graphics code area starts with its shader program header, whose type agrees with the stage. Task and mesh headers carry the types 1 and 3 of the vertex-processing stages"""
    if stage == COMPUTE: return True
    if len(code) <= HEADER_BYTES: return False
    w0 = struct.unpack_from("<I", code)[0]
    return (w0 & 0x1f) in (1, 2) and ((w0 >> 10) & 0xf in (1, 3) if stage in (TASK, MESH) else (w0 >> 10) & 0xf == stage + 1)


def host_key(ctl, code):
    """What the runtime's keys_hash computes for a Switch 2 control block (hle_nvn.c, s2_ctl_scan.py): the 0xe0 block with +0x20..+0x27 as zeros, and the uploaded code"""
    return hashlib.sha256(ctl[:0x20] + bytes(8) + ctl[0x28:CTL_MIN] + code[:_u32(ctl, 0x44)]).hexdigest()[:16]


def corpus_key(header, body):
    """Unlike glslc2_extract.py the header is part of the key: one code body runs under different program headers here (and the header is what lowering copies)"""
    return hashlib.blake2b(header + body, digest_size=8).hexdigest()


class Writer:
    def __init__(self, shader):
        self.corpus, self.ctl, self.mesh = shader / "corpus" / "epd", shader / "corpus-ctl", shader / "corpus-mesh"
        for d in (self.corpus, self.ctl, self.mesh): d.mkdir(parents=True, exist_ok=True)
        self.tsv = open(self.ctl / "hostkeys.tsv", "w", encoding="utf-8", newline="\n")
        self.mesh_tsv = open(self.mesh / "hostkeys.tsv", "w", encoding="utf-8", newline="\n")
        for f in (self.tsv, self.mesh_tsv): f.write("# hostkey\tcorpuskey\tstage\tcode_len\tinsn_bytes\tgroup\tshared\tsource\n")
        self.keys, self.programs = set(), set()

    def add(self, p, stat):
        stat["records"] += 1
        ctl, code = p.ctl, p.code
        n = _u32(ctl, 0x44)
        if _nvn_slot(ctl) != p.stage: raise Bad(f"{p.source}: record of stage {p.stage}, control block says {_nvn_slot(ctl)}")
        if n != len(code) or n % 16 or not n: raise Bad(f"{p.source}: code area {len(code):#x}, control block says {n:#x}")
        if not header_ok(p.stage, code): raise Bad(f"{p.source}: code area does not start with a {STAGE_NAME[p.stage]} program header")
        key = host_key(ctl, code)
        if key in self.keys: stat["duplicates"] += 1; return
        self.keys.add(key)
        hdr = 0 if p.stage == COMPUTE else HEADER_BYTES
        header, body = code[:hdr], code[hdr:]
        ck = corpus_key(header, body)
        out = self.mesh if p.stage in (TASK, MESH) else self.corpus
        if ck not in self.programs:
            self.programs.add(ck)
            hk = hashlib.blake2b(header, digest_size=8).hexdigest() if hdr else "0" * 16
            (out / f"{ck}-stage{p.stage}-{hk}.program.bin").write_bytes(header + body)
            (out / f"{ck}.txt").write_text(f"stage {p.stage}\nprogram 0\nheader_bytes {hdr}\nsource_bytes {len(body)}\nsource {p.source}\n")
        gx, gy, gz = struct.unpack_from("<3I", ctl, 0x58)
        row = f"{key}\t{ck}\t{p.stage}\t{n}\t{_u32(ctl, 0x38)}\t{gx} {gy} {gz}\t0\t{p.source}\n"   # shared 0: +0xb8 is not the shared memory size in these blocks (values over 20 MB), so s2_pkg.py uses its default
        if p.stage in (TASK, MESH): self.mesh_tsv.write(row)
        else:
            self.tsv.write(row); (self.ctl / f"{key}.ctl").write_bytes(ctl[:CTL_MIN])
        stat[STAGE_NAME[p.stage]] += 1

    def close(self):
        self.tsv.close(); self.mesh_tsv.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("shader", type=pathlib.Path); ap.add_argument("--romfs", required=True); ap.add_argument("--sources", required=True)
    a = ap.parse_args()
    rom = RomFS(a.romfs); names = rom.names()
    sources = read_sources(a.sources)
    if not sources: raise SystemExit(f"{a.sources} names no bfsha, sarc or glob sources")
    files = []
    for kind, pat in sources:
        hit = fnmatch.filter(names, pat) if kind == "glob" else [pat] if pat in set(names) else []
        if not hit: raise SystemExit(f"shader source {pat} is missing from the RomFS")
        files += [(kind, p) for p in hit if (kind, p) not in files]
    zs = Zstd(rom.read(DICT_PACK) if DICT_PACK in names else None)
    report = {"files": len(files), "containers": collections.Counter(), "errors": {}, "control_blocks_outside_a_container": {}, "skipped": collections.Counter(), "per_source": {}}
    w = Writer(a.shader); total = collections.Counter()
    for i, (kind, p) in enumerate(files):
        stat = collections.Counter()
        try:
            for prog in walk(rom.read(p), p, zs, report):
                try: w.add(prog, stat)
                except (Bad, struct.error) as e:
                    report["errors"][prog.source] = str(e); report["skipped"][re.sub(r"0x[0-9a-f]+|^\S+: ", "", str(e))] += 1
        except (Bad, struct.error, UnicodeDecodeError, OSError) as e:
            report["errors"][p] = str(e)
        total.update(stat)
        report["per_source"][p] = dict(stat)
        if i % 200 == 0 or i == len(files) - 1: print(f"{i + 1}/{len(files)} files, {sum(total[s] for s in STAGE_NAME)} programs", flush=True)
    w.close()
    report["totals"] = dict(total); report["containers"] = dict(report["containers"]); report["skipped"] = dict(report["skipped"])
    report["per_source"] = {k: v for k, v in sorted(report["per_source"].items())}
    (a.shader / "epd_report.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print("done:", json.dumps({"totals": report["totals"], "containers": report["containers"], "errors": len(report["errors"]), "skipped": report["skipped"], "control blocks outside a container": sum(report["control_blocks_outside_a_container"].values())}))


if __name__ == "__main__":
    sys.exit(main())
