#!/usr/bin/env python3
"""Lower every program of the offline corpus to Maxwell with ampere-lower (the same output as
Eden-NS2's ampere-spirv --maxwell). Output: <out>/<corpus key>.mx.bin (0x50 SPH + Maxwell instructions
with scheduling words + Eden's address map) and <out>/results.tsv (key, stage, status, ampere, maxwell).

  lower_corpus.py <out dir> <corpus dir>...   [env J=workers, AMPERE_LOWER=path of the tool]
"""
import concurrent.futures as cf, os, pathlib, re, subprocess, sys, tempfile

EXE = pathlib.Path(os.environ.get("AMPERE_LOWER") or pathlib.Path(__file__).resolve().parents[1] / "ampere-lower" / "ampere-lower")   # the lowering tool (ampere-lower/build.sh)


def one(item):
    key, stage, hdr, prog, out = item
    mx = out / f"{key}.mx.bin"
    with tempfile.TemporaryDirectory() as t:
        spv = pathlib.Path(t) / "o.spv"
        r = subprocess.run([str(EXE), str(prog), "--stage", str(stage), "--header-bytes", str(hdr), "-o", str(spv), "--maxwell", str(mx)],
                           capture_output=True, text=True, timeout=600)
    m = re.search(r"OK stage=(\d+) ampere=(\d+)/?(\d*) maxwell=(\d+)", r.stdout)
    why = (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or [f"exit {r.returncode}"])[-1][:120]   # CAPTURE_FAIL and LOWER_FAIL are printed on stdout
    if r.returncode == 0 and m:
        status = "OK"
    elif mx.exists() and mx.stat().st_size > 0x50:
        status = "LOWERED_ONLY:" + why
    else:
        status = "FAIL:" + why
    return key, stage, status, (m.group(2) if m else ""), (m.group(4) if m else "")


def main():
    out = pathlib.Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    items = {}
    for d in sys.argv[2:]:
        progs = {}   # one listing of the folder: a glob per key is quadratic, and takes hours with tens of thousands of programs
        for f in pathlib.Path(d).glob("*.program.bin"):
            progs.setdefault(f.name.split("-")[0], f)
        for txt in pathlib.Path(d).glob("*.txt"):
            key = txt.stem
            if key in items or key not in progs:
                continue
            meta = dict(l.split(None, 1) for l in txt.read_text().splitlines() if l.strip())
            items[key] = (key, int(meta["stage"]), int(meta["header_bytes"]), progs[key], out)
    print(f"programs {len(items)}", flush=True)
    done = {}
    res = out / "results.tsv"
    if res.exists():
        for l in res.read_text().splitlines()[1:]:
            f = l.split("\t")
            if len(f) >= 3 and f[2] == "OK" and (out / f"{f[0]}.mx.bin").exists(): done[f[0]] = l
    todo = [v for k, v in items.items() if k not in done]
    print(f"already done {len(done)}, to do {len(todo)}", flush=True)
    j = max(1, min(3, int(os.environ.get("J", "3"))))
    rows = list(done.values())
    with cf.ThreadPoolExecutor(j) as ex:
        for i, (key, stage, status, a, m) in enumerate(ex.map(one, todo)):
            rows.append("\t".join([key, str(stage), status, a, m]))
            if i % 200 == 0:
                print(f"{i}/{len(todo)}", flush=True)
                res.write_text("key\tstage\tstatus\tampere\tmaxwell\n" + "\n".join(rows) + "\n")
    res.write_text("key\tstage\tstatus\tampere\tmaxwell\n" + "\n".join(rows) + "\n")
    import collections
    c = collections.Counter(r.split("\t")[2].split(":")[0] for r in rows)
    print("done:", dict(c), flush=True)


if __name__ == "__main__":
    main()
