#!/usr/bin/env python3
"""A sample of the real corpus through the whole chain into a scratch cache, every module checked by spirv-val (ctest nxshader.spirv_val_sample). The game's
shaders never enter the repository: the sample comes from an installer build folder (NXSHADER_BUILD, default <work>/installer-test-win/build201); exit 77
without one. Takes the first three shaders of each stage and two of the run-time keys."""
import os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import nxs_front as F, nxshader as N


def main():
    build = os.environ.get('NXSHADER_BUILD') or os.path.join(N.WORK, 'installer-test-win', 'build201')
    xl = os.path.join(N.DEFAULT_INSTALLER, 'host', 'windows', 'dmsh_xlate_bin')
    if not os.path.isdir(os.path.join(build, 'shader', 'pkg')) or not os.path.exists(os.path.join(xl, 'dmsh_xlate.dll')) or not N.vk_tool('spirv-val'):
        print(f'no build folder with a shader package at {build} (or no translator / Vulkan SDK): skipped'); return 77
    c = F.Corpus(build)
    by = {}
    for k in c.keys(): by.setdefault(c.loc[k][0][:2], []).append(k)
    keys = [k for s in sorted(by) for k in by[s][:3]] + [k for k in N.runtime_keys(N.DEFAULT_INSTALLER) if k in c.loc][:2]
    cache = tempfile.mkdtemp(prefix='nxshader_sample_')
    try:
        r = subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(HERE), 'nxshader.py'), '--cache', cache, 'build', build, '--keys', *keys], capture_output=True, text=True)
        print(r.stdout.strip()); print(r.stderr.strip()[-2000:])
        v = subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(HERE), 'nxshader.py'), '--cache', cache, 'validate'], capture_output=True, text=True)
        print(v.stdout.strip())
        ok = r.returncode == 0 and v.returncode == 0 and f'on {len(keys)} modules' in v.stdout
        print('nxshader.spirv_val_sample: ' + ('ok' if ok else 'FAILED'))
        return 0 if ok else 1
    finally: shutil.rmtree(cache, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
