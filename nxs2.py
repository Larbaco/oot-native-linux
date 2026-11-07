#!/usr/bin/env python3
"""nxs2.py: build The Legend of Zelda: Ocarina of Time (Nintendo Switch 2 remake) as a native Linux program, from your own dump.

Everything derived from the game is made here, on your machine, from the dump you give; nothing of the
game is in this repository (see LEGAL.md). The code is recompiled (nxanalyze + nxlift) and runs on our own
runtime (nxrt, windows/src); the game's shaders go through ampere-lower (Eden-NS2's Switch 2 shader lowering)
and nxshader to Vulkan SPIR-V.

  nxs2.py setup                             fetch nxnative at the pinned commit, apply the Switch 2 patch, build ampere-lower, nxanalyze, nxlift and nxrt
  nxs2.py doctor                            check the tools this computer has (what setup and build need)
  nxs2.py build <dump> [options]    the whole build into <dump folder>/<title>_native (or --out); the dump: a folder, or one .nup file (read directly, nothing extracted)
       --analyze-lift-only  only the ExeFS, nxanalyze and nxlift (Linux)
       --dumps DIR...    shader dumps from an earlier run (DM_NVN_SHDUMP folders) to include from the start
       -j, --jobs N      parallel jobs (default: 90% of the available threads; or OOT_NATIVE_JOBS=N); --full-speed = every available thread
  nxs2.py run <build dir>                   play the built game
  nxs2.py status <build dir>                what the build folder holds and what it still lacks

Needs: Linux x86-64 with AVX2 and FMA3, Python 3.12 or newer with lz4, zstandard and fontTools, the .NET 10 runtime (the shader translator),
cmake, ninja, git, curl, zstd, glslang and spirv-tools, and a Vulkan 1.3 driver. `nxs2.py doctor` lists what is missing; docs/LINUX.md has the details.
"""
import argparse, concurrent.futures as cf, contextlib, fnmatch, hashlib, json, os, pathlib, platform, re, shutil, struct, subprocess, sys, time
from oot_native_i18n import (DEFAULT_LANGUAGE, dump_error_key, normalize_language,
                             translate_builder_error, translate_builder_message, tr)
import oot_native_jobs as njobs

TOOL_VERSION = '0.1.0'
DEFAULT_GAME = '0400486024DEC000'   # Ocarina of Time (Switch 2): what runs when no --game is given
TITLE = DEFAULT_GAME
NAME = 'The Legend of Zelda: Ocarina of Time'
SLUG = 'oot'; DEFAULT_VERSION = '2.0.0'; PKG_SUFFIX = 'oot'; SHADER_VARIANTS = ''   # select_game() reads the real values from games/<TITLE>/
UPSTREAM = 'https://github.com/seedds/nxnative'
UPSTREAM_COMMIT = 'c1df4dd'   # the nxnative commit host/nxnative-switch2.patch is made against
LINUX = sys.platform.startswith('linux')
HERE = pathlib.Path(__file__).resolve().parent
PROFILE_SRC = HERE / 'games' / TITLE   # games/<TITLE>/: game.tsv (nxnative's format) + profile.tsv (the port's keys) + the shader tables + the shader tables
TOOLS = HERE / 'tools'
AMPERE = HERE / 'ampere-lower' / 'ampere-lower'
NX = pathlib.Path(os.environ.get('NXNATIVE') or HERE / 'nxnative')
PY = sys.executable
T0 = time.time()

# the shader tables a profile has before the package is made (as nxnative's NSP script writes them for a provisional profile)
EMPTY_TABLES = {
    'shader_map.tsv': '# kind: evidence  filled by the shader package (nxs2.py package)\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n',
    'shader_facts.tsv': '# Answer table for translation queries: key <TAB> fact JSON. Filled by the shader package (nxs2.py package)\n',
    'shader_pairs.tsv': '# kind: evidence  vertex / pixel shader pairs within BNSH variations: vertex key <TAB> pixel key\n',
    'shader_map.tsv.conflicts.tsv': '# kind: evidence  control-section key conflict rulings: empty\ndm_ctl_sha16\tchosen_pc\tclass\tcandidates\trecords\n',
    'bf_want.tsv': '# generated\n',
    'pso_manifest.tsv': '# Prewarm seed list (shipped in the app, next to shader_map.tsv). Rows: S<TAB>shader key<TAB>stage / R<TAB>render pipeline key (pso_skey hex) / C<TAB>compute shader key.\n'
                        '# Empty until recorded (DM_PSO_MANIFEST=off DM_PSO_RECORD).\n'}


class Fail(Exception): pass


def hms(s):
    s = int(s); return f'{s // 60}:{s % 60:02d}' if s < 3600 else f'{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}'


def language(): return normalize_language(os.environ.get('OOT_NATIVE_LANG')) or DEFAULT_LANGUAGE


def say(msg): print(f'[{hms(time.time() - T0)}] {translate_builder_message(str(msg), language())}', flush=True)
def say_tr(text): print(f'[{hms(time.time() - T0)}] {text}', flush=True)   # already in the interface language


def run(cmd, log, env=None, cwd=None, ok=(0,), heartbeat=30, show=None):
    """Run cmd with its output in log (a file), a heartbeat line on the console while it runs (and the lines show matches, or all of them with
    -v). A non-zero exit not in ok stops the build with the log's last lines."""
    cmd = [str(c) for c in cmd]
    log = pathlib.Path(log); log.parent.mkdir(parents=True, exist_ok=True)
    e = dict(os.environ); e.update(env or {})
    last, t_beat, t_start = '', time.time(), time.time()
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        f.write('$ ' + ' '.join(cmd) + '\n'); f.flush()
        p = subprocess.Popen(cmd, env=e, cwd=str(cwd) if cwd else None, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        for line in p.stdout:
            f.write(line); last = line.rstrip()
            if VERBOSE or (show and re.search(show, line)):
                print('    ' + translate_builder_message(last[:160], language()), flush=True)
            if time.time() - t_beat > heartbeat:
                t_beat = time.time(); print(f'    ... {hms(time.time() - t_start)}  {translate_builder_message(last[:120], language())}', flush=True)
        rc = p.wait()
    if rc not in ok:
        tail = ''.join(log.read_text(encoding='utf-8', errors='replace').splitlines(keepends=True)[-25:])
        raise Fail(f'{pathlib.Path(cmd[0]).name} failed ({rc}); log: {log}\n{tail}')
    return rc


def which(name, *extra):
    p = shutil.which(name)
    if p: return p
    for x in extra:
        x = pathlib.Path(os.path.expanduser(x))
        if x.is_file() and os.access(x, os.X_OK): return str(x)
    return None


def dotnet_root():   # package runtime first, then a developer install
    d = which('dotnet', os.environ.get('NXS2_DOTNET_ROOT', '') + '/dotnet' if os.environ.get('NXS2_DOTNET_ROOT') else '', str(HERE.parent / 'dotnet' / 'dotnet'), '~/.dotnet/dotnet', '/usr/local/share/dotnet/dotnet', '/opt/homebrew/opt/dotnet/libexec/dotnet')
    return pathlib.Path(d).resolve().parent if d else None


def fingerprint(*parts):
    h = hashlib.sha256()
    for p in parts:
        if isinstance(p, pathlib.Path):
            st = p.stat() if p.exists() else None
            h.update(f'{p}:{st.st_size if st else -1}:{int(st.st_mtime) if st else 0}\n'.encode())
        else: h.update(repr(p).encode() + b'\n')
    return h.hexdigest()[:16]


def digest(p):
    """A file's content as a fingerprint part: for tools that a download or a rebuild gives new times without changing them"""
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    return f'{pathlib.Path(p).name}:{h.hexdigest()[:16]}'


class Build:
    def __init__(self, out):
        self.out = pathlib.Path(out).resolve()
        self.logs = self.out / 'logs'; self.stamps = self.out / 'stamps'
        self.extract = self.out / 'extract'; self.exefs = self.extract / 'ExeFS'; self.romfs_img = self.extract / 'RomFS.img'
        self.profile_root = self.out / 'profile'; self.profile = self.profile_root / TITLE
        self.shader = self.out / 'shader'; self.corpus = self.shader / 'corpus'; self.ctl = self.shader / 'corpus-ctl'
        self.maxwell = self.shader / 'maxwell'; self.pkg = self.shader / 'pkg'; self.tables = self.shader / 'tables'
        self.outputs = self.out / 'out'; self.rundir = self.out / 'run'
        self.lx = self.out / 'linux'      # the Linux game: program.db, game.so, the Vulkan shaders, the runtime (nxs2_linux.py)
        vf = self.out / 'version.txt'   # the game version the launchers carry, from the dump's build line (build writes it)
        self.version = vf.read_text().strip() if vf.is_file() else DEFAULT_VERSION
        self.game = read_game_tsv(PROFILE_SRC / 'game.tsv')
        self.modules = self.game['modules'][0].split()
        for d in (self.logs, self.stamps): d.mkdir(parents=True, exist_ok=True)

    def set_version(self, disp):
        self.version = disp if disp and disp != '?' else DEFAULT_VERSION   # a build line without a display version: the profile's default_version
        self.out.mkdir(parents=True, exist_ok=True); (self.out / 'version.txt').write_text(self.version + '\n')

    # ---- stamps: a stage is skipped when its inputs are as at the last run
    def fresh(self, name, key):
        f = self.stamps / f'{name}.json'
        return f.is_file() and json.loads(f.read_text()).get('key') == key

    def stamp(self, name, key): (self.stamps / f'{name}.json').write_text(json.dumps({'key': key, 'time': time.strftime('%Y-%m-%d %H:%M:%S')}) + '\n')

    def env(self, **more):
        e = {'DM_GAMES_DIR': str(self.profile_root), 'DM_GAMES_FIRST': '1', 'DM_GAME': TITLE, 'GAME': TITLE, 'DM_ALLOW_OTHER_METAL': '1', 'NXNATIVE': str(NX),
             'AMPERE_LOWER': str(AMPERE), 'PYTHONDONTWRITEBYTECODE': '1', 'PYTHONUTF8': '1',
             'S2_PKG_SUFFIX': PKG_SUFFIX, 'S2_GAME_NAME': NAME, 'J': str(JOBS)}   # every worker pool of the build: the lowering
        d = dotnet_root()
        if d: e['DOTNET_ROOT'] = str(d); e['PATH'] = f'{d}{os.pathsep}{os.environ.get("PATH", "")}'
        e.update(more); return e

    def corpus_dirs(self): return [d for d in (self.corpus / n for n in ('rdb', 'main-embedded', 'linkdata', 'epd', 'dumps')) if d.is_dir()]


def select_game(game):
    """Make games/<game>/ the profile of this run (--game, else $NXS2_GAME, else Ocarina of Time): TITLE, NAME and the port's keys come from it."""
    global TITLE, NAME, PROFILE_SRC, SLUG, DEFAULT_VERSION, PKG_SUFFIX, SHADER_VARIANTS, RUNTIME_INSTALLER
    game = game or os.environ.get('NXS2_GAME') or DEFAULT_GAME
    d = HERE / 'games' / game
    if not (d / 'game.tsv').is_file():
        known = sorted(p.name for p in (HERE / 'games').iterdir() if (p / 'game.tsv').is_file())
        raise Fail(f'no profile games/{game}/game.tsv' + (' (games/%s/README.md says what to fill in)' % game if d.is_dir() else '') + f'; profiles: {", ".join(known)}')
    g = read_game_tsv(d / 'game.tsv'); p = read_game_tsv(d / 'profile.tsv') if (d / 'profile.tsv').is_file() else {}
    TITLE, PROFILE_SRC, NAME = g['title_id'][0], d, g['name'][0]
    SLUG = (p.get('slug') or [TITLE.lower()])[0]; DEFAULT_VERSION = (p.get('default_version') or ['1.0.0'])[0]
    PKG_SUFFIX = (p.get('pkg_suffix') or [SLUG])[0]; SHADER_VARIANTS = (p.get('shader_variants') or [''])[0].strip()
    RUNTIME_INSTALLER = ('tools/s2_dumps.py', 'tools/lower_corpus.py', 'tools/s2_pkg.py', f'games/{TITLE}/runtime_keys.tsv')
    return d


def read_game_tsv(path):
    g = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.startswith('#'): continue
        k, _, v = line.partition('\t'); g.setdefault(k, []).append(v)
    return g


def build_id(nso):
    with open(nso, 'rb') as f:
        f.seek(0x40); return f.read(16).hex()


def lower_priority():
    """The whole build at low priority (its child processes inherit it), so the machine stays responsive and the build takes only the time other work leaves."""
    try: os.nice(10)
    except Exception: pass


# ---------------------------------------------------------------- environment
def doctor(quiet=False):
    """What this machine has of what the build needs. Returns the list of problems (empty = ready)."""
    if not LINUX: raise Fail('this builder runs on Linux only (nxs2.py build --linux)')
    return lx().doctor(sys.modules[__name__], quiet)


def patch_adds(patch):
    """The files a patch creates (its --- /dev/null entries)."""
    lines = patch.read_text(encoding='utf-8', errors='replace').splitlines()
    return [b[6:] for a, b in zip(lines, lines[1:]) if a == '--- /dev/null' and b.startswith('+++ b/')]


def build_translator():
    """Build the patched .NET translator from the pinned nxnative source tree."""
    project = NX / 'dm/gfx/tools/dmsh_xlate'
    csproj = project / 'dmsh_xlate.csproj'
    if not csproj.is_file(): raise Fail(f'the pinned nxnative checkout has no translator project: {csproj}')
    root = dotnet_root()
    dotnet = root / 'dotnet' if root else pathlib.Path(which('dotnet') or '')
    if not dotnet.is_file(): raise Fail('.NET 10 SDK is required to build dmsh_xlate from source')
    sdk = subprocess.run([dotnet, '--list-sdks'], capture_output=True, text=True)
    if sdk.returncode or not re.search(r'(^|\n)10\.', sdk.stdout):
        raise Fail('.NET 10 SDK is required to build dmsh_xlate from source')
    # Keep a generic upstream Windows profile example out of release source: the scanner
    # intentionally rejects user-profile path syntax. This changes a comment only.
    appdata_example = NX / 'dm/third_party/ryujinx/src/Ryujinx.Common/Configuration/AppDataManager.cs'
    if appdata_example.is_file():
        slash = chr(92)
        original = 'C:' + slash + 'Users' + slash + 'user' + slash + 'AppData' + slash + 'Roaming' + slash + 'Ryujinx' + slash + 'Logs'
        sanitized = '%APPDATA%' + slash + 'Ryujinx' + slash + 'Logs'
        text = appdata_example.read_text(encoding='utf-8')
        count = text.count(original)
        if count == 1:
            appdata_example.write_text(text.replace(original, sanitized), encoding='utf-8')
        elif count == 0 and sanitized not in text:
            raise Fail('the pinned nxnative user-profile example changed; inspect before release')
        elif count > 1:
            raise Fail('the pinned nxnative user-profile example occurs more than once')
    out = project.parent / 'dmsh_xlate_bin'
    say('building dmsh_xlate from the patched nxnative source (.NET 10)')
    run([dotnet, 'publish', '-c', 'Release', '-o', out, '-maxcpucount:4', '-p:UseAppHost=false'],
        HERE / 'setup-dmsh-xlate.log', cwd=project, heartbeat=30)
    lock = NX / 'dm/gfx/tools/tool_lock.py'
    # The upstream lock also fingerprints macOS/iOS Metal compilers through
    # xcrun. The Linux release builds this portable translator from source but
    # never uses nxnative's Metal shader chain, so do not rewrite that
    # platform-specific lock from the Ubuntu container.
    if lock.is_file() and os.environ.get('NXS2_RELEASE_PACKAGE') != '1':
        run([PY, lock, '--update-lock'], HERE / 'setup-dmsh-xlate-lock.log', cwd=lock.parent)
    dest = pathlib.Path(os.environ.get('NXS2_BIN') or HERE / 'bin')
    names = ('dmsh_xlate.dll', 'dmsh_xlate.deps.json', 'dmsh_xlate.runtimeconfig.json', 'Ryujinx.Common.dll', 'Ryujinx.Graphics.Shader.dll', 'Spv.Generator.dll',
             'Gommon.dll', 'Humanizer.dll', 'Microsoft.IO.RecyclableMemoryStream.dll', 'MsgPack.dll', 'System.CodeDom.dll')
    missing = [name for name in names if not (out / name).is_file()]
    if missing: raise Fail('dmsh_xlate publish omitted: ' + ', '.join(missing))
    for name in names: copy_if_changed(out / name, dest / name)


def setup(args):
    """nxnative at the pinned commit with the Switch 2 patch and the rebuilt translator, and ampere-lower built."""
    bad = doctor()
    if bad: raise Fail('install the missing tools above, then run setup again')
    if not (NX / '.git').is_dir():
        say(f'fetching nxnative ({UPSTREAM} at {UPSTREAM_COMMIT}) into {NX}')
        run(['git', 'clone', '--filter=blob:none', '--no-checkout', UPSTREAM, NX], HERE / 'setup-clone.log')
    run(['git', 'config', 'core.autocrlf', 'false'], HERE / 'setup-config.log', cwd=NX)   # the patches expect LF
    def head(): return subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True, cwd=NX).stdout.strip()
    marker = NX / 'dm/gfx/nvn/.switch2-patch'
    patch = HERE / 'host/nxnative-switch2.patch'
    want = fingerprint(patch)
    if not head().startswith(UPSTREAM_COMMIT[:7]):
        say(f'checking out nxnative {UPSTREAM_COMMIT} (was {head() or "nothing"})')
        run(['git', 'checkout', '-q', '--', '.'], HERE / 'setup-reset.log', cwd=NX, ok=(0, 1))   # drop a patch applied to another commit
        for m in (marker, NX / 'dm/gfx/nvn/.windows-extras'):
            if m.exists(): m.unlink()
        run(['git', 'fetch', 'origin', UPSTREAM_COMMIT], HERE / 'setup-fetch.log', cwd=NX, ok=(0, 128))
        run(['git', 'checkout', '-q', UPSTREAM_COMMIT], HERE / 'setup-checkout.log', cwd=NX)
    if not (marker.is_file() and marker.read_text().strip() == want):
        dirty = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], capture_output=True, text=True, cwd=NX).stdout.strip()
        if dirty:
            say('resetting nxnative to the pinned commit before applying the patch')
            run(['git', 'checkout', '-q', '--', '.'], HERE / 'setup-reset.log', cwd=NX)
            if (NX / 'dm/gfx/nvn/.windows-extras').exists(): (NX / 'dm/gfx/nvn/.windows-extras').unlink()
        for f in patch_adds(patch):   # the patch's own new files (s2tex) from an earlier apply: the reset leaves them, and git apply will not create them over themselves
            if (NX / f).is_file(): (NX / f).unlink()
        say('applying the Switch 2 host patch')
        run(['git', 'apply', '--whitespace=nowarn', patch], HERE / 'setup-patch.log', cwd=NX)
        marker.write_text(want + '\n')
    build_translator()
    if not AMPERE.is_file() or AMPERE.stat().st_mtime < (HERE / 'ampere-lower/main.cpp').stat().st_mtime:
        say('building ampere-lower (the Switch 2 shader lowering)')
        run(['sh', HERE / 'ampere-lower/build.sh'], HERE / 'setup-ampere.log')
    lx().build_tools(sys.modules[__name__])
    say(f'setup done: nxnative {head()} + Switch 2 patch at {NX}, ampere-lower at {AMPERE}')


# ---------------------------------------------------------------- the dump
def find_dump(args):
    """(exefs folder, romfs folder) of the update the recipe is for, under the folder given (the dump as a Switch 2 dumping tool writes it:
    <game>/updates/<version>/{exefs,romfs}, or a folder holding exefs/ and romfs/ directly), checked by the executable's build ID. A .nup file: see find_dump_nup."""
    if not (args.exefs and args.romfs) and args.dump and is_nup(args.dump): return find_dump_nup(pathlib.Path(args.dump).expanduser().resolve())
    if args.exefs and args.romfs: cands = [(pathlib.Path(args.exefs), pathlib.Path(args.romfs))]
    else:
        d = pathlib.Path(args.dump).expanduser().resolve()
        roots = [d]
        if (d / 'updates').is_dir(): roots += sorted((p for p in (d / 'updates').iterdir() if p.is_dir()), reverse=True)
        if (d / 'base').is_dir(): roots.append(d / 'base')
        cands = []
        for r in roots:
            for ex, ro in (('exefs', 'romfs'), ('ExeFS', 'RomFS'), ('exefs', 'RomFS'), ('ExeFS', 'romfs')):
                if (r / ex / 'main').is_file() and (r / ro).is_dir() and (r / ex, r / ro) not in cands: cands.append((r / ex, r / ro))
    if not cands: raise Fail(f'no dump under {args.dump}: expected <folder>/updates/<version>/exefs/main and .../romfs/, or exefs/ and romfs/ in the folder itself')
    seen = []
    for ex, ro in cands:
        bid = build_id(ex / 'main'); seen.append(f'{ex}: main build {bid}')
        for ver, disp, want in builds():
            if bid != want['main']: continue
            for m, b in want.items():
                if not (ex / m).is_file(): raise Fail(f'{ex / m} is missing from the dump')
                if build_id(ex / m) != b: raise Fail(f'{ex / m} has build {build_id(ex / m)}, the recipe for {ver} expects {b}')
                short = nso_short(ex / m)
                if short: raise Fail(f'{ex / m} is {short} bytes shorter than its own header says: it was copied in text mode, which drops a byte from every '
                                     'CR LF pair in files without an extension. Copy the exefs folder again in binary mode (FileZilla: Transfer > Transfer type > Binary)')
            if not any(ro.rglob('*')): raise Fail(f'{ro} is empty')
            return ex, ro, f'{ver} ({disp})' if disp != '?' else ver, disp
    raise Fail(f'this recipe is for {NAME} ' + ', '.join(f'{v} (main build {w["main"]})' for v, _, w in builds()) + '; the dump holds:\n  ' +
               '\n  '.join(seen) + '\n(a base-game folder is not enough: the game\'s update is needed)')


def lx():
    import nxs2_linux
    return nxs2_linux


def is_nup(dump):
    p = pathlib.Path(dump).expanduser()
    return p.is_file() and p.suffix.lower() == '.nup'


@contextlib.contextmanager
def nup_errors():
    import nup_dump
    try: yield
    except nup_dump.Error as e: raise Fail(str(e)) from e


def romfs_requirements(files, marker, source_table):
    """Return missing required files and unmatched optional source globs.

    The shader table's `glob` rows describe a set of inputs, not a literal
    filename. Some game updates may omit an entire glob family, so a glob with
    no matches is informational; the marker, build IDs and explicit bfsha/sarc
    paths remain required.
    """
    missing = [] if marker in files else [marker]
    unmatched_globs = []
    for line in source_table.splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        fields = line.split('\t')
        kind, paths = fields[0], fields[1:]
        if kind == 'blob':
            continue
        if kind == 'glob':
            for pattern in paths:
                if not any(fnmatch.fnmatchcase(path, pattern) for path in files):
                    unmatched_globs.append(pattern)
        else:
            missing.extend(p for p in paths if p not in files)
    return missing, unmatched_globs


def find_dump_nup(path, warnings=None):
    """(package, package, version, display version) of the game in a .nup, by the same checks as a folder: the package stands in for both folders until the
    stages take what they need from it (st_extract, and st_corpus through `package / path`)."""
    import nup_dump
    with nup_errors():
        cands = nup_dump.candidates(path)
        if not cands: raise Fail(f'no package with both an ExeFS and a RomFS in {path}:\n  ' + '\n  '.join(nup_dump.describe(path)))
        seen = []
        for c in cands:
            bid = c.build_id('main'); seen.append(f'{c.parent}: main build {bid}')
            for ver, disp, want in builds():
                if bid != want['main']: continue
                for m, b in want.items():
                    have = c.build_id(m)
                    if have is None: raise Fail(f'{m} is missing from {c.parent}')
                    if have != b: raise Fail(f'{m} in {c.parent} has build {have}, the recipe for {ver} expects {b}')
                files = c.romfs_paths()   # before the long image: an update that holds only the files it changes cannot be used as it is
                marker = read_game_tsv(PROFILE_SRC / 'game.tsv')['romfs_marker'][0]
                missing, unmatched_globs = romfs_requirements(files, marker, (PROFILE_SRC / 'shader_sources.tsv').read_text())
                if missing: raise Fail(f'the RomFS of {c.parent} lacks ' + ', '.join(missing) + ': it is not the whole game\'s files')
                messages = [f'optional RomFS glob matched no files: {pattern}' for pattern in unmatched_globs]
                if warnings is not None:
                    warnings.extend(messages)
                else:
                    for message in messages:
                        print(translate_builder_message(f'warning: {message}', language()), file=sys.stderr)
                return c, c, f'{ver} ({disp})' if disp != '?' else ver, disp
    raise Fail(f'this recipe is for {NAME} ' + ', '.join(f'{v} (main build {w["main"]})' for v, _, w in builds()) + '; the .nup holds:\n  ' +
               '\n  '.join(seen) + '\n(a base-game package is not enough: the game\'s update is needed)')


def nso_short(p):
    """How many bytes an NSO lacks against the segments its header lists (0 = complete)."""
    h = p.read_bytes()[:0x70]; flags = struct.unpack_from('<I', h, 0xC)[0]
    end = max(struct.unpack_from('<I', h, 0x10 + 0x10 * i)[0] + struct.unpack_from('<I', h, 0x60 + 4 * i if flags & (1 << i) else 0x18 + 0x10 * i)[0] for i in range(3))
    return max(0, end - p.stat().st_size)


def builds():
    """The game versions the profile knows: (version, display version, {module: build ID}) per build line."""
    out = []
    for line in read_game_tsv(PROFILE_SRC / 'game.tsv')['build']:
        ver, disp, *mods = line.split('\t')
        out.append((ver, disp, dict(m.split('=', 1) for m in mods if '=' in m)))
    return out


# ---------------------------------------------------------------- stages
def st_profile(b):
    """The game profile the tools read: game.tsv, the entitlements and the empty shader tables (the package fills them later)."""
    b.profile.mkdir(parents=True, exist_ok=True)
    for name in ('game.tsv', 'profile.tsv', *sorted(p.name for p in PROFILE_SRC.glob('*.entitlements')), 'runtime_keys.tsv', 'shader_sources.tsv', 'shader_facts.seed.tsv'):
        src = PROFILE_SRC / name
        if src.is_file(): shutil.copyfile(src, b.profile / name)
    for name, text in EMPTY_TABLES.items():
        if not (b.profile / name).is_file(): (b.profile / name).write_text(text)


def st_extract(b, exefs, romfs):
    """The game as nxnative's tools read it: the ExeFS copied, a control record with the game's name, the RomFS packed into one image."""
    nup = not isinstance(exefs, pathlib.Path)
    b.exefs.mkdir(parents=True, exist_ok=True)
    if nup:
        with nup_errors(): exefs.copy_exefs(b.exefs)
    else:
        for f in sorted(exefs.iterdir()):
            if f.is_file() and (not (b.exefs / f.name).is_file() or (b.exefs / f.name).stat().st_size != f.stat().st_size): shutil.copy2(f, b.exefs / f.name)
    lx().normalize_zbic(sys.modules[__name__], b)
    nacp = b.extract / 'control.nacp'
    data = make_nacp(NAME, int(TITLE, 16), b.version)
    if not nacp.is_file() or nacp.read_bytes() != data: nacp.write_bytes(data)
    if nup:
        say(f'RomFS image from {romfs.parent}')
        def progress(done, total): say(f'  RomFS image: {done * 100 // total} % of {total / (1 << 30):.1f} GB')
        with nup_errors():
            if not romfs.write_image(b.romfs_img, progress): say('RomFS image: up to date')
        romfs.use_image(b.romfs_img, b.extract / 'nup-files')   # the shader sources are read from here, only if the corpus stage has to run
    else:
        say(f'RomFS image from {romfs} ({sum(1 for _ in romfs.rglob("*") if _.is_file())} files)')
        run([PY, NX / 'dm/tools/romfs_image.py', '--reuse', romfs, b.romfs_img], b.logs / 'romfs_image.log', env=b.env())
    if not b.romfs_img.is_file(): raise Fail('no RomFS image was written')


def make_nacp(name, title, version):   # a control record (NACP) with the game's name and version: Switch 2 dumps carry none, and the app build reads one
    import struct
    d = bytearray(0x4000)
    for lang in range(16):
        d[lang * 0x300:lang * 0x300 + len(name)] = name.encode(); d[lang * 0x300 + 0x200:lang * 0x300 + 0x207] = b'unknown'
    struct.pack_into('<Q', d, 0x3038, title); d[0x3060:0x3060 + len(version)] = version.encode()
    struct.pack_into('<Q', d, 0x3078, title); struct.pack_into('<Q', d, 0x30B0, title)
    return bytes(d)


def st_corpus(b, romfs, dumps):
    """The game's shaders as the lowering reads them: every shader container in the game's files (shader_sources.tsv says where, read straight from
    the dump's RomFS folder, read-only), and any shader dumps of earlier runs (--dumps). Writes <shader>/corpus/<set>/ and <shader>/corpus-ctl/."""
    sources = [l.split('\t') for l in (PROFILE_SRC / 'shader_sources.tsv').read_text().splitlines() if l.strip() and not l.startswith('#')]
    if st_corpus_epd(b, sources): return add_dumps(b, dumps)   # Nintendo EPD engine games: bfsha/sarc/glob sources, no RDB or LINKDATA
    key = fingerprint(b.exefs / 'main', b.romfs_img, *sorted(TOOLS.glob('*.py')), sources)
    if not (b.fresh('corpus', key) and (b.ctl / 'hostkeys.tsv').is_file()):
        for d in (b.corpus, b.ctl):
            if d.is_dir(): shutil.rmtree(d)
        nso = b.shader / 'nso'; nso.mkdir(parents=True, exist_ok=True)
        say('reading the executable\'s data (nso_tool.py)')
        run([PY, TOOLS / 'nso_tool.py', b.exefs / 'main', '--out', nso], b.logs / 'nso_tool.log', env=b.env())
        jobs = []   # (label, corpus set, mode, args)
        for kind, *paths in sources:
            if kind == 'blob': jobs.append(('main', 'main-embedded', 'blob', [nso / 'main.rodata', 'main.rodata']))
            elif kind == 'linkdata': jobs.append(('linkdata', 'linkdata', 'linkdata', [romfs / p for p in paths]))
            elif kind == 'rdb': jobs.append((pathlib.Path(paths[0]).name, 'rdb', 'rdb', [romfs / paths[0]]))
        for label, cset, mode, args in jobs:
            for p in args:
                if isinstance(p, pathlib.Path) and not p.is_file(): raise Fail(f'shader source {p} is missing from the dump')
        say(f'extracting the shaders of {len(jobs)} sources')
        def extract(j):
            run([PY, TOOLS / 'glslc2_extract.py', b.corpus / j[1], j[2], *j[3]], b.logs / f'corpus_{j[0]}.log', env=b.env(), heartbeat=120); return j[0]
        with cf.ThreadPoolExecutor(max(1, min(4, os.cpu_count() or 2))) as ex:   # the extractions in parallel; the control scans one at a time (they append to one hostkeys.tsv)
            for label in ex.map(extract, jobs): say(f'  extracted {label}')
        for label, cset, mode, args in jobs:
            run([PY, TOOLS / 's2_ctl_scan.py', b.ctl, mode, *args], b.logs / f'ctl_{label}.log', env=b.env(), heartbeat=120)
        b.stamp('corpus', key)
    else: say('corpus: up to date')
    add_dumps(b, dumps)


def st_corpus_epd(b, sources):
    """Shader sources of a Nintendo EPD engine game (bfsha, sarc, glob lines of shader_sources.tsv): tools/epd_shaders.py reads the RomFS image (read-only) and
    writes <shader>/corpus/epd/ and <shader>/corpus-ctl/ (and corpus-mesh/ for the task/mesh programs, which only the measurement reads). False if the profile has no such source."""
    if not any(s[0] in ('bfsha', 'sarc', 'glob') for s in sources): return False
    key = fingerprint(b.romfs_img, TOOLS / 'epd_shaders.py', sources)
    if b.fresh('corpus', key) and (b.ctl / 'hostkeys.tsv').is_file(): say('corpus: up to date'); return True
    for d in (b.corpus, b.ctl, b.shader / 'corpus-mesh'):
        if d.is_dir(): shutil.rmtree(d)
    say('extracting the shaders of the EPD containers (epd_shaders.py)')
    run([PY, TOOLS / 'epd_shaders.py', b.shader, '--romfs', b.romfs_img, '--sources', PROFILE_SRC / 'shader_sources.tsv'], b.logs / 'epd_shaders.log', env=b.env(), heartbeat=120, show=r'^done|files')
    b.stamp('corpus', key); return True


def add_dumps(b, dumps):
    """Shader dumps recorded while the game ran (DM_NVN_SHDUMP folders: <key>.ctl + <key>.code) become corpus entries and host keys."""
    dirs = [pathlib.Path(d).expanduser() for d in dumps if pathlib.Path(d).expanduser().is_dir() and any(pathlib.Path(d).expanduser().glob('*.ctl'))]
    if not dirs: return
    say(f'adding shader dumps from {len(dirs)} folder(s)')
    run([PY, TOOLS / 's2_dumps.py', *dirs, '--corpus', b.corpus / 'dumps', '--ctl', b.ctl], b.logs / 's2_dumps.log', env=b.env(), show=r'new|added|programs')


def st_lower(b, jobs):
    """Every corpus program lowered to Maxwell with ampere-lower (incremental: programs lowered before are kept)."""
    say('lowering the shaders (ampere-lower)')
    run([PY, TOOLS / 'lower_corpus.py', b.maxwell, *b.corpus_dirs()], b.logs / 'lower.log', env=b.env(J=str(jobs)), show=r'^programs|already done|^done')


def st_package(b):
    """The shader package nxnative's chain reads, and the profile's shader tables (shader_map.tsv, shader_facts.tsv with the hand-made answers on top)."""
    say('packaging the shaders (s2_pkg.py)')
    for d in (b.pkg, b.tables):
        if d.is_dir(): shutil.rmtree(d)
    run([PY, TOOLS / 's2_pkg.py', b.ctl, b.maxwell, b.pkg, b.tables, *b.corpus_dirs()], b.logs / 's2_pkg.log', env=b.env(), show=r'records|keys|written|pkg')
    shutil.copyfile(b.tables / 'shader_map.tsv', b.profile / 'shader_map.tsv')
    if (b.tables / 'shader_pairs.tsv').is_file(): shutil.copyfile(b.tables / 'shader_pairs.tsv', b.profile / 'shader_pairs.tsv')
    facts = {}
    order = []
    header = []
    for line in (b.tables / 'shader_facts.tsv').read_text(encoding='utf-8').splitlines():
        if line.startswith('#'): header.append(line); continue
        if not line.strip(): continue
        k = line.split('\t')[0]
        if k not in facts: order.append(k)
        facts[k] = line
    seed = PROFILE_SRC / 'shader_facts.seed.tsv'
    n = 0
    if seed.is_file():
        for line in seed.read_text(encoding='utf-8').splitlines():
            if line.startswith('#') or not line.strip(): continue
            k = line.split('\t')[0]
            if k not in facts: order.append(k)
            facts[k] = line; n += 1
    (b.profile / 'shader_facts.tsv').write_text('\n'.join(header + [facts[k] for k in order]) + '\n', encoding='utf-8')
    say(f'tables: {len(order)} facts rows ({n} hand-made), shader map {sum(1 for l in (b.profile / "shader_map.tsv").read_text().splitlines() if l and not l.startswith("#")) - 1} entries')


# ---------------------------------------------------------------- the Linux game (nxs2_linux.py)
def copy_if_changed(src, dst):
    dst = pathlib.Path(dst); dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_file() and dst.stat().st_size == pathlib.Path(src).stat().st_size and dst.read_bytes() == pathlib.Path(src).read_bytes(): return
    try: shutil.copy2(src, dst)
    except PermissionError: raise Fail(f'{dst} is in use: close the game, then build again')


RUNTIME_INSTALLER = ('tools/s2_dumps.py', 'tools/lower_corpus.py', 'tools/s2_pkg.py', f'games/{TITLE}/runtime_keys.tsv')   # the on-demand path's steps


def run_linux(args):
    b = Build(args.build); sh = lx().launcher_path(sys.modules[__name__], b)
    if not sh.is_file(): raise Fail(f'{b.out} has no Linux game yet: nxs2.py build <dump> --linux')
    say(f'starting {sh}')
    sys.exit(subprocess.call([str(sh), *args.env]))


# ---------------------------------------------------------------- report
def status(args):
    b = Build(args.build)
    for name, p in (('ExeFS', b.exefs / 'main'), ('RomFS image', b.romfs_img), ('shader corpus', b.ctl / 'hostkeys.tsv'), ('lowered shaders', b.maxwell / 'results.tsv'),
                    ('shader package', b.pkg), ('shader map', b.profile / 'shader_map.tsv'), ('Linux: program.db', b.lx / 'analyze/program.db'), ('Linux: game.so', b.lx / 'game.so'),
                    ('Linux: runtime', b.lx / 'runtime/nxrt')):
        print(f'  {"ok     " if p.exists() else "missing"}  {name}')
    vk = b.lx / 'shaders'
    if vk.is_dir(): print(f'  Linux: {sum(1 for _ in vk.rglob("*.spv"))} Vulkan shader modules, {sum(1 for _ in (vk / "dumps").glob("*.ctl")) if (vk / "dumps").is_dir() else 0} run-time shaders recorded')
    for p in sorted(b.outputs.glob('*')) if b.outputs.is_dir() else []: print(f'  output   {p}')


# ---------------------------------------------------------------- commands
def need_setup():
    """Setup's tools live in this copy of the repository: a fresh download or a copy in another folder has none until setup runs there."""
    if lx().tools(sys.modules[__name__])['missing']: raise Fail('this copy is not set up yet: run nxs2.py setup here once (it builds the recompiler and the runtime for this PC)')
    if not AMPERE.is_file():
        raise Fail(f'this copy of {HERE.name} is not set up yet (ampere-lower at {AMPERE}); run nxs2.py setup here once.')


def build(args):
    if not LINUX: raise Fail('this builder runs on Linux only')
    if not args.analyze_lift_only:
        # A packaged release carries its build tools. End-user builds need only
        # the runtime prerequisites checked by the launcher's release doctor.
        if os.environ.get('NXS2_RELEASE_PACKAGE') != '1':
            bad = doctor(quiet=True)
            if bad: doctor(); raise Fail('install the missing tools above (nxs2.py doctor), then build again')
        need_setup()
        lx().preflight(sys.modules[__name__])
    exefs, romfs, ver, disp = find_dump(args)
    if args.out: out = pathlib.Path(args.out).expanduser()
    elif args.dump: d = pathlib.Path(args.dump).expanduser().resolve(); out = d.parent / f'{d.stem if is_nup(d) else d.name}_native'   # beside the dump, never inside it
    else: raise Fail('--exefs/--romfs need --out <build folder>')
    b = Build(out); jobs = JOBS
    b.set_version(disp)
    say(f'OoT Native builder {TOOL_VERSION}: {NAME} {ver}: dump {exefs.parent}, build folder {b.out}')
    say(f'load: {jobs} worker(s) of {JOBS_CHOICE.plan.threads} CPU threads, low priority' + {'cli': ' (--jobs)', 'env': f' ({njobs.ENV_VAR})', 'full': ' (--full-speed)', 'default': ' (default: 90% of the threads)'}.get(JOBS_CHOICE.source, ''))
    for w in njobs.render_warnings(language(), JOBS_CHOICE.warnings):
        if os.environ.get('OOT_NATIVE_LAUNCHER') != '1': say_tr(w)   # the launcher has already shown them
    free = shutil.disk_usage(b.out).free / 1_000_000_000
    if free < 45 and not b.romfs_img.is_file(): say(f'WARNING: {free:.0f} GB free where the build folder is; reserve at least 45 GB (the measured OoT build used 38G)')
    st_profile(b)
    if args.analyze_lift_only:   # ExeFS only: no RomFS image, no shader corpus (TODO: a bfsha/BNSH extractor for games without RDB/LINKDATA, see docs/FASE1-INSPECAO.md)
        b.exefs.mkdir(parents=True, exist_ok=True)
        if isinstance(exefs, pathlib.Path):
            for f in sorted(exefs.iterdir()):
                if f.is_file(): shutil.copy2(f, b.exefs / f.name)
        else:
            with nup_errors(): exefs.copy_exefs(b.exefs)
        lx().normalize_zbic(sys.modules[__name__], b)
        lx().stage_lift(sys.modules[__name__], b, jobs, detach=args.detach_lift)
        return say('done (analyze + lift only)')
    st_extract(b, exefs, romfs)
    try:
        with nup_errors(): st_corpus(b, romfs, args.dumps or default_dump_dirs(b))   # run-time shader dumps of earlier runs: a rebuilt corpus keeps them
    finally:
        if not isinstance(romfs, pathlib.Path): romfs.discard_files()
    st_lower(b, jobs)
    st_package(b)
    lx().stage(sys.modules[__name__], b, jobs)
    say(f'the Linux game: run "{lx().launcher_path(sys.modules[__name__], b)}" (or nxs2.py run "{b.out}")')
    say('done')


def default_dump_dirs(b):
    dirs = [b.rundir / 'shdump', b.lx / 'shaders' / 'dumps']   # the Linux game records the run-time shaders in the same form (and translates them for itself on the spot)
    return [d for d in dirs if d.is_dir()]


def main():
    global VERBOSE
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument('-v', '--verbose', action='store_true', help='show every line the tools print (default: a heartbeat and the important lines; everything goes to <build>/logs)')
    ap.add_argument('--game', help=f'the game profile, a folder of games/ (default: $NXS2_GAME, else {DEFAULT_GAME} = {NAME})')
    sub = ap.add_subparsers(dest='cmd', required=True)
    ap.add_argument('--version', action='version', version=f'OoT Native builder {TOOL_VERSION}')
    s = sub.add_parser('setup'); s.set_defaults(f=setup)
    s = sub.add_parser('doctor'); s.add_argument('--release', action='store_true', help='check only end-user runtime requirements'); s.add_argument('--json', action='store_true'); s.add_argument('--out', help='check disk space on this build destination'); s.add_argument('-j', '--jobs', type=jobs_arg, help='build jobs to check the RAM against (default: as for build)'); s.set_defaults(f=run_doctor)
    s = sub.add_parser('check-dump', help='quickly validate a user-owned .nup without extracting or modifying it')
    s.add_argument('dump'); s.add_argument('--json', action='store_true'); s.set_defaults(f=check_dump)
    s = sub.add_parser('build')
    s.add_argument('dump', nargs='?', help='the dump folder (<game>/updates/<version>/{exefs,romfs} inside, or exefs/ and romfs/ directly), or one .nup file')
    s.add_argument('--exefs'); s.add_argument('--romfs'); s.add_argument('--out', help='the build folder (default: <dump>/../<title>_native)')
    s.add_argument('--linux', action='store_true', help='the Linux game (the only target; accepted so older command lines keep working)')
    s.add_argument('--analyze-lift-only', action='store_true', help='only the ExeFS, nxanalyze and nxlift (no RomFS image, no shader corpus, shaders, fonts or launcher)')
    s.add_argument('--detach-lift', action='store_true', help='with --analyze-lift-only: start nxlift detached (setsid, nohup) and return')
    s.add_argument('--dumps', nargs='*', default=[], help='shader dumps from an earlier run (DM_NVN_SHDUMP folders)')
    s.add_argument('-j', '--jobs', type=jobs_arg, help=f'worker processes at once, a whole number of 1 or more (default: 90%% of the available threads; or set {njobs.ENV_VAR})')
    s.add_argument('--full-speed', action='store_true', help='every available thread (100%%) instead of the 90%% default; -j N wins over it')
    s.set_defaults(f=build)
    s = sub.add_parser('run'); s.add_argument('build'); s.add_argument('env', nargs='*', help='switches for the launcher (--fullscreen, --window WxH, --silent, --gamemode)'); s.set_defaults(f=run_linux)
    s = sub.add_parser('status'); s.add_argument('build'); s.set_defaults(f=status)
    for p in sub.choices.values(): p.add_argument('--game', default=argparse.SUPPRESS, help=argparse.SUPPRESS)
    a = ap.parse_args(); VERBOSE = a.verbose
    try: select_game(a.game)
    except Fail as e: print(f'\n[FAIL] {translate_builder_error(str(e), language())}', file=sys.stderr); sys.exit(1)
    if a.cmd == 'build' and not (a.dump or (a.exefs and a.romfs)): ap.error('build needs the dump folder or .nup file, or --exefs and --romfs')
    global JOBS, JOBS_CHOICE
    if a.cmd in ('build', 'doctor'):
        try: JOBS_CHOICE = pick_jobs(a)
        except njobs.JobsError as e: ap.error(tr(language(), 'jobs.invalid', source=e.source, value=e.value))
        JOBS = JOBS_CHOICE.jobs
    if a.cmd in ('setup', 'build'): lower_priority()
    try:
        rc = a.f(a)
        if a.cmd in ('doctor', 'check-dump') and isinstance(rc, int): sys.exit(rc)
    except Fail as e: print(f'\n[FAIL] {translate_builder_error(str(e), language())}', file=sys.stderr); sys.exit(1)
    except KeyboardInterrupt: print('\n' + tr(language(), 'builder_interrupted'), file=sys.stderr); sys.exit(130)


def jobs_arg(text):
    try: return njobs.parse_jobs(text)
    except njobs.JobsError: raise argparse.ArgumentTypeError(tr(language(), 'jobs.invalid', source='--jobs', value=text))


def pick_jobs(a):
    """--jobs > OOT_NATIVE_JOBS > --full-speed > the 90 % default (oot_native_jobs.default_jobs, the one rule the launcher uses too). Warns (never refuses) when the choice passes
    the threads or the memory ceiling. The shader compiles keep every worker at 100 % for a long time, a long all-core load that some machines do not take (13th and 14th
    generation Intel Core CPUs without Intel's 2024 microcode fix crash under it): -j N or OOT_NATIVE_JOBS=N uses fewer."""
    plan = njobs.default_plan()
    if getattr(a, 'full_speed', False) and getattr(a, 'jobs', None) is None and not (os.environ.get(njobs.ENV_VAR) or '').strip():
        c = njobs.JobsChoice(plan.threads, 'full', plan, njobs.explicit_warnings(plan.threads, plan))
    else: c = njobs.resolve_jobs(getattr(a, 'jobs', None), plan=plan)
    return c


def release_resources(jobs_choice):
    """Threads, default jobs and available RAM for the release doctor, and the RAM warning when a per-job estimate says the jobs will not fit."""
    p = jobs_choice.plan
    ram = p.mem_available
    return {'threads': p.threads, 'default_jobs': p.jobs, 'jobs': jobs_choice.jobs, 'mem_available_gb': None if ram is None else round(ram / 1e9, 1),
            'mem_per_job_gb': None if p.mem_per_job is None else round(p.mem_per_job / 1e9, 1), 'low_memory': njobs.low_memory(jobs_choice.jobs, p)}


def run_doctor(args):
    if args.release:
        checks = lx().release_checks(sys.modules[__name__], args.out)
        res = release_resources(JOBS_CHOICE)
        if args.json: print(json.dumps({'ok': all(c['ok'] for c in checks), 'checks': checks, 'resources': res}, ensure_ascii=False))
        else:
            ram = tr(language(), 'doctor.ram_unknown') if res['mem_available_gb'] is None else f"{res['mem_available_gb']:.1f} GB"
            print(f"  {'info':8} " + tr(language(), 'doctor.resources', threads=res['threads'], default=res['default_jobs'], jobs=res['jobs'], ram=ram))
            if res['low_memory']: print(f"  {'warning':8} " + tr(language(), 'doctor.warn_ram', jobs=res['jobs'], ram=ram))
            for c in checks:
                label = tr(language(), 'doctor.' + c.get('code', ''))
                values = {'available': c.get('available_gb', '?')} if c.get('code') == 'space_45gb' else {}
                detail = '' if c['ok'] else tr(language(), c.get('detail_key', 'doctor.detail_generic'), **values)
                print(f"  {tr(language(), 'doctor.ok') if c['ok'] else tr(language(), 'doctor.missing'):8} {label}" + (f" — {detail}" if detail else ''))
        return 0 if all(c['ok'] for c in checks) else 1
    bad = doctor()
    if args.json: print(json.dumps({'ok': not bad, 'missing': bad}, ensure_ascii=False))
    return 1 if bad else 0


def check_dump(args):
    result = {'ok': False, 'code': 'invalid', 'version': None, 'title_id': TITLE, 'error': None, 'warnings': []}
    try:
        dump = pathlib.Path(args.dump).expanduser().resolve()
        with dump.open('rb') as f:
            magic = f.read(4)
        if magic[:3] != b'NUP':
            result.update(code='invalid_format', error='input does not have NUP magic')
        else:
            _, _, version, display = find_dump_nup(dump, result['warnings'])
            result.update(ok=True, code='accepted', version=display or version)
    except Exception as e:
        result['error'] = str(e) or type(e).__name__
        result['code'] = classify_dump_error(result['error'], e)
    if args.json: print(json.dumps(result, ensure_ascii=False))
    elif result['ok']: print(tr(language(), 'dump.accepted', version=result['version']))
    else: print(tr(language(), 'dump.rejected', reason=tr(language(), dump_error_key(result['code']))), file=sys.stderr)
    return 0 if result['ok'] else 1


def classify_dump_error(message, exc=None):
    text = str(message).lower()
    if isinstance(exc, (PermissionError, FileNotFoundError)):
        return 'unreadable'
    if any(word in text for word in ('encrypt', 'decryption', 'criptograf', 'descriptograf')):
        return 'encrypted'
    if any(word in text for word in ('this recipe is for', 'the .nup holds', 'dump holds', 'main build')):
        return 'wrong_game_version'
    if any(word in text for word in ('is missing', 'lacks ', 'no package with both', 'update is needed', 'update is required')):
        return 'incomplete'
    if isinstance(exc, OSError):
        return 'unreadable'
    if any(word in text for word in ('truncated', 'unexpected eof', 'corrupt', 'invalid header', 'bad magic')):
        return 'corrupt'
    return 'invalid'


VERBOSE = False
JOBS = 4   # set in main() from -j / OOT_NATIVE_JOBS / --full-speed / default_jobs()
JOBS_CHOICE = None   # the njobs.JobsChoice behind JOBS
if __name__ == '__main__': main()
