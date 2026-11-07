#!/usr/bin/env python3
"""Offline dispatch test of cached compute shaders on lavapipe with dummy resources: does the shader end?
  probe_cache.py <cache dir> <key> [key ...] [--groups X,Y,Z] [--fill zero|ones|rand|ramp] [--timeout S] [--probe PATH] [--guarded]
Builds the bindings from the cache's reflection (descriptor kinds) and the module (image dimensions), runs vk_dispatch_probe (tests/vk_dispatch_probe.c) with the lavapipe
ICD only (the probe refuses a hardware device), prints one line per shader. --guarded runs the loop_guard / cas_atomic_load passes on the module first (what a v5 cache
publishes), to see the guard end a shader that otherwise does not.  Never run this against a hardware driver."""
import argparse, os, subprocess, sys, tempfile, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import json, nxs_spv as S

LVP = '/usr/share/vulkan/icd.d/lvp_icd.json'


def binding_args(m, meta):
    """the probe's binding arguments for a module and its reflection"""
    args = []
    for b in meta['bindings']:
        n, d = b['binding'], b['descriptor']
        vid = next((v for v, sc, p in m.global_vars() if m.deco(v, S.DecBinding) == [n]), None)
        dim, arr = 1, False
        if vid is not None:
            t = m.type_desc(m.inst(vid)[1]); t = t.get('of', t)
            while t.get('t') == 'array': t = t['of']
            im = t.get('image') if t.get('t') == 'sampled_image' else t
            if im and im.get('t') == 'image': dim, arr = im['dim'], bool(im['arrayed'])
        suffix = {0: '1d', 1: '2d', 2: '3d', 3: 's'}.get(dim, '2d')
        if d == 'uniform_buffer': args.append(f'{n}:ubo')
        elif d == 'storage_buffer': args.append(f'{n}:ssbo')
        elif d == 'combined_image_sampler': args.append(f'{n}:' + ('cubes' if dim == 3 else 'cis' + suffix + ('a' if arr else '')))
        elif d == 'sampled_image': args.append(f'{n}:tex' + (suffix if dim != 3 else '2d') + ('a' if arr else ''))
        elif d == 'storage_image': args.append(f'{n}:img' + (suffix if dim != 3 else '2d') + ('a' if arr else ''))
        elif d == 'sampler': args.append(f'{n}:smp')
        elif d == 'uniform_texel_buffer': args.append(f'{n}:utb')
        elif d == 'storage_texel_buffer': args.append(f'{n}:stb')
        else: raise SystemExit(f'binding {n}: descriptor {d} not supported by the probe')
    return args


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cache'); ap.add_argument('keys', nargs='+'); ap.add_argument('--groups', default='40,45,1'); ap.add_argument('--fill', default='rand')
    ap.add_argument('--timeout', type=float, default=60); ap.add_argument('--probe'); ap.add_argument('--guarded', action='store_true'); ap.add_argument('--threads', default='3')
    a = ap.parse_args()
    w = tempfile.mkdtemp(prefix='probe_cache_')
    probe = a.probe or os.path.join(w, 'vk_dispatch_probe')
    if not a.probe:
        r = subprocess.run([shutil.which('clang') or 'cc', '-O1', '-o', probe, os.path.join(HERE, 'vk_dispatch_probe.c'), '-lvulkan'], capture_output=True, text=True)
        if r.returncode: sys.exit('cannot build the probe: ' + r.stderr[:300])
    env = dict(os.environ, VK_DRIVER_FILES=LVP, VK_ICD_FILENAMES=LVP, LP_NUM_THREADS=a.threads)
    rc = 0
    try:
        for k in a.keys:
            spv_p = os.path.join(a.cache, 'vk', k + '.spv'); meta = json.load(open(os.path.join(a.cache, 'vk', k + '.json')))
            if meta['stage'] != 'cs': print(f'{k}: not a compute shader'); continue
            m = S.Module(open(spv_p, 'rb').read())
            if a.guarded:
                for p in S.DEFAULT_PASSES[-2:]:
                    if p.applies(m, 'cs'): p.run(m, 'cs')
            run = os.path.join(w, k + '.spv'); open(run, 'wb').write(m.bytes())
            args = [probe, run, *a.groups.split(','), str(a.timeout), a.fill, *binding_args(m, meta)]
            try: r = subprocess.run(args, capture_output=True, text=True, env=env, timeout=a.timeout + 120)
            except subprocess.TimeoutExpired: print(f'{k}: HUNG (probe did not return)'); rc = 1; continue
            print(f'{k}: {(r.stdout.strip().splitlines() or [r.stderr.strip()[:200]])[0]} (exit {r.returncode}) bindings {" ".join(args[7:])}')
            rc |= r.returncode == 3
    finally: shutil.rmtree(w, ignore_errors=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
