#!/usr/bin/env python3
# s2drive.py <build folder> <tag> [--host <dmhost>] [--env K=V ...] step ...
# A copy of ~/setup/menudrive.py for the s2tex corpus: the run folder is ~/s2tex/runs/<tag> (a fresh copy of the run-4 save), and the boot
# sequence is a step of its own ("boot": 40 s, then A eight times) so the title screen can be sampled first.
# Steps: a key ("X", "A+B", "Down~0.4"), "wait:<s>", "shot:<name>", "ctl:<raw line>", "boot", "collect:<name>" (see collect.py).
# The corpus of docs/s2tex.md came from two runs (with this file and collect.py copied to ~/s2tex on the Mac):
#   s2drive.py ~/installer-test/build201-clean c1 wait:45 collect:title shot:title boot shot:game collect:game X wait:3 shot:bag collect:bag B wait:2
#              Plus wait:3 shot:menu collect:menu B wait:2 LUp~4 wait:3 collect:walk1 LLeft~3 wait:3 collect:walk2 shot:end
#   s2drive.py ~/installer-test/build201-clean c2 wait:45 boot Plus wait:2 A wait:5 shot:dex collect:dex R wait:3 shot:dex2 collect:dex2 B wait:2 B wait:2
#              Plus wait:2 Right wait:1 A wait:5 shot:hab collect:hab B wait:2 B wait:2 RRight~2 wait:1 LUp~5 wait:3 shot:walk3 collect:walk3
import os, pathlib, shutil, subprocess, sys, time

H = pathlib.Path.home(); B = pathlib.Path(sys.argv[1]).expanduser(); TAG = sys.argv[2]; rest = sys.argv[3:]
host = B / 'host/dmhost'; extra = {}
while rest and rest[0].startswith('--'):
    if rest[0] == '--host': host = pathlib.Path(rest[1]).expanduser(); rest = rest[2:]
    elif rest[0] == '--env': k, v = rest[1].split('=', 1); extra[k] = v; rest = rest[2:]
    else: sys.exit(f'unknown option {rest[0]}')
rd = H / f's2tex/runs/{TAG}'; shutil.rmtree(rd, ignore_errors=True); rd.mkdir(parents=True)
shutil.copytree(H / 'setup/save_run4/save', rd / 'save'); (rd / 'temp').mkdir()
ctl = rd / 'ctl.txt'; ctl.write_text(''); log = rd / 'host.log'
env = dict(os.environ, DM_ROOT=str(B), DM_ROMFS=str(B / 'extract/RomFS.img'), DM_SAVE=str(rd / 'save'), DM_TEMP=str(rd / 'temp'), DM_MSL_DIR=str(B / 'shader/cache'),
           DM_FONT_DIR=str(B / 'font'), DM_NVN_SHMAP=str(B / 'profile/0400486024DEC000/shader_map.tsv'), DM_LANG='en-US', DM_AUDIO='0', DM_CTL=str(ctl), **extra)


def say(line):
    with open(ctl, 'a') as f: f.write(line + '\n')


sys.path.insert(0, str(pathlib.Path(__file__).parent))
p = subprocess.Popen([str(host), str(B / 'patched')], env=env, cwd=rd, stdout=open(log, 'w'), stderr=subprocess.STDOUT)
(rd / 'pid').write_text(str(p.pid))
try:
    for s in rest:
        if p.poll() is not None: sys.exit(f'the host exited ({p.returncode}) before step {s}')
        if s == 'boot':
            time.sleep(40)
            for _ in range(8): say('A'); time.sleep(4)
            time.sleep(8)
        elif s.startswith('wait:'): time.sleep(float(s[5:]))
        elif s.startswith('shot:'):
            f = rd / f'{s[5:]}.png'; say(f'shot {f}')
            for _ in range(40):
                if f.exists() and f.stat().st_size: break
                time.sleep(0.25)
            print(f'{f.name}: {"ok" if f.exists() else "none"}', flush=True)
        elif s.startswith('ctl:'): say(s[4:])
        elif s.startswith('collect:'):
            import collect
            collect.run(rd, log, say, s[8:])
        else: say(s); time.sleep(1.0)
finally:
    p.terminate()
    try: p.wait(10)
    except subprocess.TimeoutExpired: p.kill()
