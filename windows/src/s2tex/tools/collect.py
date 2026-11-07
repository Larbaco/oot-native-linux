# collect.py: the "collect:<stage>[:<max>]" step of s2drive.py. Lists the registered 2D and 3D textures (DM_CTL texlist 1 / 2), saves each new
# sampled block-linear texture's guest storage (DM_CTL memdump), then the host's decoded level 0 of the same serials (DM_NVN_TEXRAW, set through
# DM_CTL env and written by the next texdump; it covers packaged textures, which DM_CTL texraw does not) into ~/s2tex/corpus, one manifest row each.
# Skipped before anything is saved: render targets (COMPRESSIBLE 0x8), linear textures (LINEAR 0x10, LINEAR_RENDER_TARGET 0x40), formats the host
# converts on upload (3-, 6- and 12-byte RGB, 24-bit depth), and textures larger than 16 MB.
import pathlib, re, shutil, time

HOME = pathlib.Path.home(); CORPUS = HOME / 's2tex/corpus'; SCRATCH = HOME / 's2tex/scratch'
LINE = re.compile(r'\[texlist\] tid=\d+ ser=(\d+) target (\d+) (\d+)x(\d+)x(\d+) format (0x[0-9a-f]+) levels (\d+) flags (0x[0-9a-f]+) '
                  r'storage (0x[0-9a-f]+) size (0x[0-9a-f]+) block height/depth log2 (\d+)/(\d+)')
RAW = re.compile(r'\[texraw\] ser=(\d+)')
COLS = 'id stage ser target width height depth format levels flags storage size bh bd bl_bytes raw_bytes cpu_written'.split()
MAX_BYTES = 16 << 20
CONVERTED = set(range(0x19, 0x25)) | set(range(0x5D, 0x63)) | {0x33, 0x35}


def text_since(log, start):
    with open(log, 'rb') as f:
        f.seek(start); return f.read().decode('utf-8', 'replace')


def wait_quiet(log, quiet=2.0, limit=30.0):
    t0 = time.time(); last = -1; seen = time.time()
    while time.time() - t0 < limit:
        size = log.stat().st_size
        if size != last: last = size; seen = time.time()
        elif time.time() - seen > quiet: return
        time.sleep(0.2)


def wait_file(p, limit=20.0):
    t0 = time.time(); last = -1
    while time.time() - t0 < limit:
        if p.exists():
            s = p.stat().st_size
            if s and s == last: return s
            last = s
        time.sleep(0.15)
    return p.stat().st_size if p.exists() else -1


def run(rd, log, say, arg):
    stage, _, mx = arg.partition(':'); mx = int(mx) if mx else 10**9
    CORPUS.mkdir(parents=True, exist_ok=True)
    seen_f = CORPUS / 'seen.txt'; seen = set(seen_f.read_text().split()) if seen_f.exists() else set()
    man = CORPUS / 'manifest.tsv'
    if not man.exists(): man.write_text('\t'.join(COLS) + '\n')
    start = log.stat().st_size
    say('texlist 1'); say('texlist 2'); time.sleep(1.0); wait_quiet(log)
    rows = {}
    for m in LINE.finditer(text_since(log, start)): rows[(m.group(1), m.group(9))] = m.groups()
    picked = []
    for g in rows.values():
        ser, target, w, h, d, fmt, levels, flags, storage, size, bh, bd = g
        key = f'{ser}:{storage}:{w}x{h}x{d}:{fmt}'
        if key in seen or int(flags, 16) & 0x58 or not 0 < int(size, 16) <= MAX_BYTES or int(fmt, 16) in CONVERTED: continue
        picked.append((key, g))
    picked = picked[:mx]
    print(f'[collect] {stage}: {len(rows)} textures listed, {len(picked)} new to save', flush=True)
    whole = text_since(log, 0)
    cpu = set(re.findall(r'CPU-written texture ser=(\d+) ', whole))
    out = []
    for b in range(0, len(picked), 100):
        batch = picked[b:b + 100]
        for key, g in batch:
            say(f'memdump {g[8]} {g[9]} {CORPUS / (stage + "_" + g[0] + ".bl")}')
        for key, g in batch: wait_file(CORPUS / f'{stage}_{g[0]}.bl')
        td = SCRATCH / f'td_{stage}_{b}'; shutil.rmtree(td, ignore_errors=True); td.mkdir(parents=True)
        s0 = log.stat().st_size
        say('env DM_NVN_TEXRAW=' + ','.join(g[0] for _, g in batch)); say(f'texdump {td}/x_%d.png')
        want = {g[0] for _, g in batch}; t0 = time.time()
        while time.time() - t0 < 60:
            if want <= set(RAW.findall(text_since(log, s0))): break
            time.sleep(0.3)
        for key, g in batch:
            tid = f'{stage}_{g[0]}'; src = td / f'x_{g[0]}.png.raw'
            nr = wait_file(src, 5.0)
            if nr > 0: shutil.move(str(src), str(CORPUS / f'{tid}.raw'))
            nb = (CORPUS / f'{tid}.bl').stat().st_size if (CORPUS / f'{tid}.bl').exists() else -1
            out.append([tid, stage] + list(g) + [str(nb), str(nr), '1' if g[0] in cpu else '0']); seen.add(key)
        wait_quiet(log, 1.5, 30)
        shutil.rmtree(td, ignore_errors=True)
    say('env DM_NVN_TEXRAW=')
    with open(man, 'a') as f:
        for r in out: f.write('\t'.join(r) + '\n')
    seen_f.write_text('\n'.join(sorted(seen)) + '\n')
    print(f'[collect] {stage}: saved {sum(1 for r in out if int(r[-2]) > 0 and int(r[-3]) > 0)} pairs', flush=True)
