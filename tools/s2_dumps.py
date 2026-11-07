#!/usr/bin/env python3
"""Turn the host's DM_NVN_SHDUMP dumps into shader sources (SWITCH2 bring-up helper).

  s2_dumps.py <shdump dir>... --corpus <corpus out dir> --ctl <corpus-ctl dir>

For every <key>.ctl + <key>.code pair: the host key recomputed with the current rule
(sha256(control[:0xe0] with +0x20..+0x27 zeroed + code)[:8]), the corpus key (blake2b of the code
after the SPH, trimmed to 16 bytes), the stage (SPH word 0, or compute when the control block says
so), the thread group and shared memory fields. Writes a corpus entry (<corpuskey>-stage<N>-<hdr>
.program.bin + <corpuskey>.txt) when the corpus has none, the 0xe0 control block as
<ctl dir>/<hostkey>.ctl, and appends to <ctl dir>/hostkeys.tsv (same columns as s2_ctl_scan.py,
source "dump:<file>"). Dumps whose host key is already listed are skipped.
"""
import hashlib, pathlib, struct, sys


def stage_of(ctl: bytes, code: bytes):
    """Dump stage numbering: 0 vertex, 1 tess control, 2 tess eval, 3 geometry, 4 fragment, 5 compute."""
    if struct.unpack_from("<I", ctl, 0x54)[0] == 0x50000 or len(code) < 0x80:
        return 5, 0
    w0 = struct.unpack_from("<I", code, 0)[0]
    sph_type, shader_type = w0 & 0x1f, (w0 >> 10) & 0xf
    if sph_type == 2 or shader_type == 5:
        return 4, 0x80
    return {1: 0, 2: 1, 3: 2, 4: 3}.get(shader_type, 0), 0x80


def main():
    args = sys.argv[1:]
    corpus = pathlib.Path(args[args.index("--corpus") + 1]); ctl_dir = pathlib.Path(args[args.index("--ctl") + 1])
    dirs = [pathlib.Path(a) for a in args if not a.startswith("--") and a not in (str(corpus), str(ctl_dir))]
    corpus.mkdir(parents=True, exist_ok=True); ctl_dir.mkdir(parents=True, exist_ok=True)
    tsv = ctl_dir / "hostkeys.tsv"
    seen = {l.split("\t")[0] for l in tsv.read_text(encoding="utf-8").splitlines() if l and l[0] != "#"} if tsv.exists() else set()
    have_corpus = {p.name.split("-")[0] for d in [corpus] for p in d.glob("*.program.bin")}
    for d in ["corpus/main-embedded", "corpus/linkdata", "corpus/rdb", "corpus/phaseA1"]:
        have_corpus |= {p.name.split("-")[0] for p in pathlib.Path(d).glob("*.program.bin")}
    new_keys = new_corpus = skipped = nocode = 0
    with open(tsv, "a", encoding="utf-8") as out:
        if not seen:
            out.write("# hostkey\tcorpuskey\tstage\tcode_len\tinsn_bytes\tgroup\tshared\tsource\n")
        for d in dirs:
            for c in sorted(d.glob("*.ctl")):
                code_p = c.with_suffix(".code")
                if not code_p.exists() or code_p.stat().st_size < 16:
                    nocode += 1; continue
                ctl = c.read_bytes()[:0xe0]; code = code_p.read_bytes()
                if struct.unpack_from("<I", ctl, 8)[0] not in (0x14, 0x11, 0xb):   # control block versions seen: 0x14 (Ocarina of Time: 38150 of 38215 blocks in corpus-ctl), 0x11 (most of Pokopia), 0xb (6 of 71 requests)
                    nocode += 1; continue
                gpu_len = struct.unpack_from("<I", ctl, 0x44)[0]
                code = code[:gpu_len]
                keyctl = ctl[:0x20] + bytes(8) + ctl[0x28:]
                hostkey = hashlib.sha256(keyctl + code).hexdigest()[:16]
                if hostkey in seen:
                    skipped += 1; continue
                seen.add(hostkey)
                stage, hdr = stage_of(ctl, code)
                insn_end = struct.unpack_from("<I", ctl, 0x38)[0]   # control +0x38: end of the instructions; the c1 constant bank follows at +0x40..+0x44 (s2_pkg.py copies it from the dump)
                body = code[hdr:insn_end] if hdr < insn_end <= len(code) else code[hdr:]
                body = body[: len(body) - len(body) % 16]
                corpuskey = hashlib.blake2b(body, digest_size=8).hexdigest()
                if corpuskey not in have_corpus:
                    hdr_key = hashlib.blake2b(code[:hdr], digest_size=8).hexdigest() if hdr else "0" * 16
                    (corpus / f"{corpuskey}-stage{stage}-{hdr_key}.program.bin").write_bytes(code[:hdr] + body)
                    (corpus / f"{corpuskey}.txt").write_text(f"stage {stage}\nprogram 0\nheader_bytes {hdr}\nsource_bytes {len(body)}\nsource dump:{c.name}\n")
                    have_corpus.add(corpuskey); new_corpus += 1
                (ctl_dir / f"{hostkey}.ctl").write_bytes(ctl)
                c40, c44 = struct.unpack_from("<2I", ctl, 0x40)   # the c1 constant bank at the end of the uploaded code (s2_pkg.py reads <hostkey>.c1 first)
                if c44 > c40 and len(code) >= c44: (ctl_dir / f"{hostkey}.c1").write_bytes(code[c40:c44])
                insn = struct.unpack_from("<I", ctl, 0x38)[0]; gx, gy, gz = struct.unpack_from("<3I", ctl, 0x58); shared = struct.unpack_from("<I", ctl, 0xb8)[0]
                out.write(f"{hostkey}\t{corpuskey}\t{stage}\t{len(code)}\t{insn}\t{gx} {gy} {gz}\t{shared}\tdump:{c.name}\n")
                new_keys += 1
    print(f"dumps: new host keys {new_keys}, new corpus programs {new_corpus}, already known {skipped}, without usable code {nocode}")


if __name__ == "__main__":
    main()
