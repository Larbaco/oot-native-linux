"""nxs2_linux.py: the Linux target of nxs2.py: the game as a native program.

The same recompiled game as on Windows, built and run with Linux's own tools: nxlift's ELF objects linked into a shared object, the runtime (windows/src/nxrt, plat_linux.c)
as an ordinary executable over pthreads and mmap, SDL3 and Vulkan as in any Linux game. Nothing of Windows or macOS is emulated: no Wine, no DXVK, no translation layer.
The shader translator is a .NET assembly, which is portable code: .NET's own Linux runtime runs the same file the Windows build uses.
"""
import hashlib, os, pathlib, platform, re, shlex, shutil, subprocess, sys
from oot_native_i18n import DEFAULT_LANGUAGE, normalize_language, tr

HERE = pathlib.Path(__file__).resolve().parent
BUILD = HERE / 'windows' / 'build' / 'linux'   # build_dir(n) = linux-<TITLE> beside it (the runtime embeds the game profile)
TOOLCHAINS = pathlib.Path(os.environ.get('NXL_TOOLCHAINS') or HERE / 'toolchains')
LLVM = TOOLCHAINS / 'llvm-23.1.4'; SDL3 = TOOLCHAINS / 'SDL3-3.4.18'; VK = TOOLCHAINS / 'vulkan-headers'
URL_LLVM = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.4/LLVM-23.1.4-Linux-X64.tar.zst'
URL_SDL3 = 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz'
URL_VK = 'https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.367.tar.gz'
# Every toolchain download is verified against these SHA-256 values (release/pins.env carries the same pins; a unit test keeps them equal).
# NXS2_DOWNLOADS=<folder> supplies already-fetched copies by file name, so a release build needs no network for them.
PINNED_DOWNLOADS = {
    URL_LLVM: ('LLVM-23.1.4-Linux-X64.tar.zst', 'efdd1dbd089b99bfcfcde7b8eb1236163f2e88df5a981cca4f783ec9cb5e0378'),
    URL_SDL3: ('SDL3-3.4.18.tar.gz', '9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3'),
    URL_VK: ('Vulkan-Headers-v1.4.367.tar.gz', '0df7d58b5df617e0a6a17dec521af459206719f9c7cb12d05d09275597c16f70'),
}


def verify_sha256(path, expected):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    if h.hexdigest() != expected: raise RuntimeError(f'{path}: SHA-256 {h.hexdigest()} != pinned {expected}')


def build_dir(n):
    """The runtime embeds the game profile, so each game has its own build folder: windows/build/linux-<TITLE>."""
    return BUILD.with_name(f'linux-{n.TITLE}')


def build_jobs(default=None):
    try: requested = int(os.environ.get('NXS2_JOBS') or 0)
    except ValueError: requested = 0
    import oot_native_jobs as njobs   # the one 90%-of-the-threads rule shared with the launcher and nxs2.py
    return max(1, requested or default or njobs.default_jobs())


def tools(n):
    b = pathlib.Path(os.environ.get('NXS2_BIN') or HERE / 'bin'); dev = build_dir(n) / 'bin'; src = HERE / 'windows/src'
    def bundled(name): return b / name if (b / name).is_file() else dev / name
    sdl = pathlib.Path(os.environ.get('NXS2_SDL3') or b.parent / 'lib/libSDL3.so.0')
    if not sdl.is_file(): sdl = dev / 'libSDL3.so.0'
    t = {'nxanalyze': b / 'nxanalyze', 'nxlift': b / 'nxlift', 'nxrt': b / 'nxrt', 'libSDL3': sdl, 'ld.lld': LLVM / 'bin/ld.lld',
         'zbic-unpack': pathlib.Path(os.environ.get('NXS2_ZBIC_UNPACK') or bundled('zbic-unpack')),
         'hle_inventory.tsv': src / 'nxrt/gen/out/hle_inventory.tsv', 'nxshader': src / 'nxshader/nxshader.py', 'dmsh_abi.h': src / 'nxrt/gfx/dmsh_abi.h', 'dir': b}
    for name in ('nxanalyze', 'nxlift', 'nxrt'):
        t[name] = bundled(name)
    t['libSDL3.so.0'] = sdl
    t['ld.lld'] = pathlib.Path(os.environ.get('NXS2_LLD') or LLVM / 'bin/ld.lld')
    t['missing'] = [k for k, p in t.items() if k != 'dir' and not pathlib.Path(p).is_file()]
    return t


def doctor(n, quiet=False):
    """What this PC has of what setup and the build need; the problems as a list (empty = ready)."""
    bad = []
    def ok(what, good, fix=''):
        if not good: bad.append(what)
        if not quiet or not good: print(f'  {"ok" if good else "MISSING":8} {what}' + (f'   -> {fix}' if fix and not good else ''))
    ok(f'Python 3.12+ (running {platform.python_version()})', sys.version_info >= (3, 12))
    cpu = pathlib.Path('/proc/cpuinfo').read_text() if pathlib.Path('/proc/cpuinfo').is_file() else ''
    ok('an x86-64 CPU with AVX2 and FMA3', platform.machine() == 'x86_64' and ' avx2' in cpu and ' fma' in cpu)
    for mod, pip in (('lz4', 'lz4'), ('zstandard', 'zstandard'), ('fontTools', 'fonttools')):
        try: __import__(mod); have = True
        except ImportError: have = False
        ok(f'python module {mod}', have, f'{sys.executable} -m pip install "{pip}"')
    ok('.NET 10 runtime (the shader translator)', bool(n.dotnet_root()), 'https://dotnet.microsoft.com, or dotnet-install.sh --runtime dotnet --channel 10.0')
    for prog, fix in (('git', ''), ('cmake', ''), ('ninja', ''), ('curl', ''), ('zstd', ''), ('glslangValidator', 'package glslang-tools'), ('spirv-val', 'package spirv-tools'),
                      ('spirv-opt', 'package spirv-tools')):
        ok(prog, bool(shutil.which(prog)), fix or 'your package manager')
    ok('a Vulkan driver', any(pathlib.Path(d).is_dir() and any(pathlib.Path(d).glob('*.json')) for d in ('/usr/share/vulkan/icd.d', '/etc/vulkan/icd.d')), 'your graphics driver (Mesa or NVIDIA)')
    if not quiet:
        t = tools(n)
        print(f'  {"ok" if not t["missing"] else "later":8} the game\'s tools in {t["dir"]}' + (f' (nxs2.py setup builds them: {", ".join(t["missing"])})' if t['missing'] else ''))
    return bad


def release_space_check(build_dir=None):
    """Check the selected filesystem, allowing a not-yet-created build directory."""
    destination = pathlib.Path(build_dir).expanduser() if build_dir else pathlib.Path.home()
    probe = destination
    while not probe.exists() and probe.parent != probe:
        probe = probe.parent
    try:
        free_gb = shutil.disk_usage(probe).free / 1_000_000_000
        lang = normalize_language(os.environ.get('OOT_NATIVE_LANG')) or DEFAULT_LANGUAGE
        return {'code': 'space_45gb', 'name': tr(lang, 'doctor.space_45gb'), 'ok': free_gb >= 45,
                'detail': tr(lang, 'doctor.detail_space', available=f'{free_gb:.1f}'),
                'available_gb': f'{free_gb:.1f}', 'detail_key': 'doctor.detail_space'}
    except OSError as e:
        lang = normalize_language(os.environ.get('OOT_NATIVE_LANG')) or DEFAULT_LANGUAGE
        return {'code': 'space_45gb', 'name': tr(lang, 'doctor.space_45gb'), 'ok': False,
                'detail': tr(lang, 'doctor.detail_generic'), 'detail_key': 'doctor.detail_generic'}


def release_checks(n, build_dir=None):
    """End-user checks only; this mode never checks development toolchains."""
    checks = []
    lang = normalize_language(os.environ.get('OOT_NATIVE_LANG')) or DEFAULT_LANGUAGE
    def add(code, ok, detail='', detail_key='doctor.detail_generic'):
        checks.append({'code': code, 'name': tr(lang, 'doctor.' + code), 'ok': bool(ok),
                       'detail': detail, 'detail_key': detail_key})
    cpu = pathlib.Path('/proc/cpuinfo').read_text(errors='replace').lower() if pathlib.Path('/proc/cpuinfo').is_file() else ''
    add('cpu_avx2_fma3', platform.machine() == 'x86_64' and ' avx2' in cpu and ' fma' in cpu,
        detail_key='doctor.detail_cpu')
    vk_dirs = [pathlib.Path(x) for x in ('/usr/share/vulkan/icd.d', '/etc/vulkan/icd.d', pathlib.Path.home() / '.local/share/vulkan/icd.d')]
    icds = [str(p) for d in vk_dirs if d.is_dir() for p in d.glob('*.json')]
    details = []
    vulkan_ok = False
    try:
        import ctypes
        class AppInfo(ctypes.Structure):
            _fields_ = [('sType', ctypes.c_uint32), ('pNext', ctypes.c_void_p), ('pApplicationName', ctypes.c_char_p),
                        ('applicationVersion', ctypes.c_uint32), ('pEngineName', ctypes.c_char_p), ('engineVersion', ctypes.c_uint32), ('apiVersion', ctypes.c_uint32)]
        class InstanceInfo(ctypes.Structure):
            _fields_ = [('sType', ctypes.c_uint32), ('pNext', ctypes.c_void_p), ('flags', ctypes.c_uint32), ('pApplicationInfo', ctypes.POINTER(AppInfo)),
                        ('enabledLayerCount', ctypes.c_uint32), ('ppEnabledLayerNames', ctypes.POINTER(ctypes.c_char_p)),
                        ('enabledExtensionCount', ctypes.c_uint32), ('ppEnabledExtensionNames', ctypes.POINTER(ctypes.c_char_p))]
        loader = ctypes.CDLL('libvulkan.so.1')
        version = ctypes.c_uint32(0)
        loader.vkEnumerateInstanceVersion.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        loader.vkEnumerateInstanceVersion.restype = ctypes.c_int32
        result = loader.vkEnumerateInstanceVersion(ctypes.byref(version))
        def vkver(v): return (v >> 22, (v >> 12) & 0x3ff, v & 0xfff)
        if result != 0 or vkver(version.value) < (1, 3, 0):
            details.append(f'loader API {vkver(version.value)}')
        elif not icds:
            details.append('no Vulkan ICD manifest found')
        else:
            app = AppInfo(0, None, b'OoT Native doctor', 1, b'OoT Native', 1, (1 << 22) | (3 << 12))
            info = InstanceInfo(1, None, 0, ctypes.pointer(app), 0, None, 0, None)
            instance = ctypes.c_void_p()
            loader.vkCreateInstance.argtypes = [ctypes.POINTER(InstanceInfo), ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
            loader.vkCreateInstance.restype = ctypes.c_int32
            result = loader.vkCreateInstance(ctypes.byref(info), None, ctypes.byref(instance))
            if result != 0:
                details.append(f'vkCreateInstance returned {result}')
            else:
                count = ctypes.c_uint32(0)
                loader.vkEnumeratePhysicalDevices.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_void_p)]
                loader.vkEnumeratePhysicalDevices.restype = ctypes.c_int32
                result = loader.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), None)
                devices = (ctypes.c_void_p * count.value)()
                if result == 0 and count.value:
                    result = loader.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), devices)
                    loader.vkGetPhysicalDeviceProperties.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
                    supported = []
                    for device in devices:
                        props = (ctypes.c_uint32 * 256)()
                        loader.vkGetPhysicalDeviceProperties(device, ctypes.byref(props))
                        supported.append(vkver(props[0]))
                    vulkan_ok = result == 0 and any(v >= (1, 3, 0) for v in supported)
                    details.append('physical device API ' + ', '.join('.'.join(map(str, v)) for v in supported))
                else:
                    details.append('no Vulkan physical device enumerated')
                loader.vkDestroyInstance.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
                loader.vkDestroyInstance(instance, None)
    except (OSError, AttributeError, ValueError) as exc:
        details.append(str(exc))
    add('vulkan_1_3', vulkan_ok, '; '.join(details + icds) if details or icds else '',
        detail_key='doctor.detail_vulkan')
    font = os.environ.get('STD_FONT')
    font_ok = bool(font and pathlib.Path(font).is_file()) or any(pathlib.Path(d).is_dir() and any(pathlib.Path(d).glob('*.ttf')) for d in ('/usr/share/fonts', str(pathlib.Path.home() / '.local/share/fonts')))
    add('ttf_font', font_ok, detail_key='doctor.detail_font')
    checks.append(release_space_check(build_dir))
    return checks


def fetch_toolchain(n):
    """What the tools are built with, into toolchains/ (nothing system-wide): LLVM 23.1.4 (the recompiler is built on its libraries), SDL3 3.4.18, the Vulkan headers."""
    TOOLCHAINS.mkdir(parents=True, exist_ok=True)
    def get(url, out):
        name, expected = PINNED_DOWNLOADS[url]
        have = pathlib.Path(os.environ.get('NXS2_DOWNLOADS') or '') / name
        if os.environ.get('NXS2_DOWNLOADS') and have.is_file(): shutil.copyfile(have, out)
        else: n.run(['curl', '-fL', '--retry', '3', '-o', out, url], HERE / 'setup-fetch.log', heartbeat=60)
        try: verify_sha256(out, expected)
        except RuntimeError as e:
            pathlib.Path(out).unlink(missing_ok=True)
            raise n.Fail(f'refusing an unverified download: {e}')
    if not (LLVM / 'bin/clang').is_file():
        n.say('fetching LLVM 23.1.4 (1.2 GB, 12 GB unpacked)')
        tar = TOOLCHAINS / 'LLVM.tar.zst'; get(URL_LLVM, tar); LLVM.mkdir(exist_ok=True)
        n.run(['sh', '-c', f'zstd -d --long=30 -c "{tar}" | tar -xf - -C "{LLVM}" --strip-components=1'], HERE / 'setup-llvm.log', heartbeat=60)
        tar.unlink()
    if not (VK / 'include/vulkan/vulkan.h').is_file():
        n.say('fetching the Vulkan headers')
        tar = TOOLCHAINS / 'vk.tar.gz'; get(URL_VK, tar); VK.mkdir(exist_ok=True)
        n.run(['tar', 'xzf', tar, '-C', VK, '--strip-components=1'], HERE / 'setup-vk.log'); tar.unlink()
    if not (SDL3 / 'include/SDL3/SDL.h').is_file():
        n.say('building SDL3 3.4.18 (window, input, sound)')
        tar = TOOLCHAINS / 'sdl3.tar.gz'; get(URL_SDL3, tar); src = TOOLCHAINS / 'src'; src.mkdir(exist_ok=True)
        n.run(['tar', 'xzf', tar, '-C', src], HERE / 'setup-sdl.log'); tar.unlink()
        sdl = src / 'SDL3-3.4.18'; off = []
        for _ in range(16):   # an X11 extension whose development package this PC lacks is left out, not fatal
            shutil.rmtree(sdl / 'build', ignore_errors=True)
            r = subprocess.run(['cmake', '-S', sdl, '-B', sdl / 'build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_INSTALL_PREFIX={SDL3}', '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF',
                                '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF', *off], capture_output=True, text=True)
            m = re.search(r'configure with (-D[A-Z0-9_]+=OFF)', r.stdout + r.stderr)
            if not m: break
            off.append(m.group(1))
        if r.returncode: raise n.Fail('SDL3 does not configure here: install its build dependencies (libx11-dev, libwayland-dev, libxkbcommon-dev, libpulse-dev, libudev-dev)\n' + (r.stdout + r.stderr)[-800:])
        n.run(['cmake', '--build', sdl / 'build', '-j', build_jobs()], HERE / 'setup-sdl.log', heartbeat=60)
        n.run(['cmake', '--install', sdl / 'build'], HERE / 'setup-sdl.log')


def build_tools(n):
    """nxanalyze, nxlift and nxrt from windows/src, with the toolchain above. Incremental: only what changed compiles again."""
    fetch_toolchain(n)
    env = {'PATH': f'{LLVM / "bin"}:{os.environ.get("PATH", "")}'}
    n.say('building the recompiler and the runtime (windows/src)')
    n.run(['cmake', '-S', HERE / 'windows', '-B', build_dir(n), '-G', 'Ninja', f'-DNXRT_GAME={n.TITLE}', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
           f'-DNXW_LLVM_DIR={LLVM}', f'-DNXRT_SDL3_DIR={SDL3}', f'-DNXRT_VK_INCLUDE={VK / "include"}', *shlex.split(os.environ.get('NXS2_CMAKE_ARGS', ''))], HERE / 'setup-cmake.log', env=env)
    n.run(['ninja', '-C', build_dir(n), '-j', build_jobs(), 'nxanalyze-cli', 'nxlift-cli', 'nxrt', 'zbic-unpack'], HERE / 'setup-ninja.log', env=env, heartbeat=60)
    n.copy_if_changed(next(p for p in (SDL3 / 'lib/libSDL3.so.0', SDL3 / 'lib64/libSDL3.so.0') if p.exists()), build_dir(n) / 'bin/libSDL3.so.0')


def env(n):
    e = {'AMPERE_LOWER': str(n.AMPERE), 'PYTHONDONTWRITEBYTECODE': '1', 'PYTHONUTF8': '1',
         'DOTNET_SYSTEM_GLOBALIZATION_INVARIANT': '1'}
    d = n.dotnet_root()
    if d: e['DOTNET_ROOT'] = str(d); e['PATH'] = f'{d}:{os.environ.get("PATH", "")}'
    return e


def shader_command(n, b, t, jobs, report):
    """Build nxshader with the packaged translator directory selected explicitly."""
    return [sys.executable, t['nxshader'], '--cache', b.lx / 'shaders',
            '--installer', HERE, '--xlate', t['dir'], '--game', n.TITLE,
            'build', b.out, '--jobs', jobs, '--report', report,
            *(['--variants', n.SHADER_VARIANTS] if n.SHADER_VARIANTS else [])]


def first_exception_line(path):
    """Return the first exception headline from a subprocess traceback, if present."""
    try:
        lines = pathlib.Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return ''
    pattern = re.compile(r'^\s*[A-Za-z_][\w.]*(?:Error|Exception):')
    return next((line.strip() for line in lines if pattern.match(line)), '')


def preflight(n):
    """Start every packaged tool needed by a full build before extracting game data."""
    t = tools(n)
    if t['missing']:
        raise n.Fail('release build tools missing: ' + ', '.join(t['missing']))
    packaged = pathlib.Path(t['dir'])
    def find_tool(name):
        candidate = packaged / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
        found = shutil.which(name)
        return pathlib.Path(found) if found else candidate
    required = {
        'spirv-opt': find_tool('spirv-opt'),
        'spirv-val': find_tool('spirv-val'),
        'ld.lld': pathlib.Path(t['ld.lld']),
        'ampere-lower': pathlib.Path(n.AMPERE),
        'zbic-unpack': pathlib.Path(t['zbic-unpack']),
    }
    for name, path in required.items():
        if not path.is_file() or not os.access(path, os.X_OK):
            raise n.Fail(f'release preflight: {name} is missing or not executable: {path}')

    dotnet_root = n.dotnet_root()
    dotnet = pathlib.Path(dotnet_root) / 'dotnet' if dotnet_root else pathlib.Path(shutil.which('dotnet') or '')
    if not dotnet.is_file() or not os.access(dotnet, os.X_OK):
        raise n.Fail('release preflight: packaged .NET runtime is missing or not executable')
    nxshader_dir = pathlib.Path(t['nxshader']).parent
    if str(nxshader_dir) not in sys.path:
        sys.path.insert(0, str(nxshader_dir))
    try:
        import nxs_front
        translator = nxs_front.Translator(str(packaged), dotnet=str(dotnet))
    except Exception as exc:
        raise n.Fail(f'release preflight: dmsh_xlate initialization failed: {exc}') from exc
    n.say(f'preflight: dmsh_xlate source SHA {translator.source_sha}')

    probes = (
        ('spirv-opt', required['spirv-opt'], ('--version',), (0,)),
        ('spirv-val', required['spirv-val'], ('--version',), (0,)),
        ('ld.lld', required['ld.lld'], ('--version',), (0,)),
        ('ampere-lower', required['ampere-lower'], (), (1,)),
        ('zbic-unpack', required['zbic-unpack'], (), (2,)),
    )
    for name, path, args, accepted in probes:
        try:
            result = subprocess.run([str(path), *args], capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise n.Fail(f'release preflight: {name} failed to start: {exc}') from exc
        if result.returncode not in accepted:
            detail = (result.stdout + result.stderr).strip().splitlines()
            raise n.Fail(f'release preflight: {name} returned {result.returncode}' + (f': {detail[0]}' if detail else ''))
    n.say('preflight: spirv-opt, spirv-val, ld.lld, ampere-lower and zbic-unpack ready')


def analyze(n, b, t):
    db = b.lx / 'analyze' / 'program.db'
    key = n.fingerprint(*(b.exefs / m for m in b.modules), n.digest(t['nxanalyze']), n.digest(t['hle_inventory.tsv']))
    if b.fresh('lx_analyze', key) and db.is_file(): return n.say('analysis: up to date')
    n.say('analysing the game\'s code (nxanalyze)')
    n.run([t['nxanalyze'], b.exefs, '--out', db.parent, '--hle', t['hle_inventory.tsv'], '--no-defaults'], b.logs / 'lx_analyze.log', show=r'functions|HLE binding')
    n.run([t['nxanalyze'], '--check-takeovers', db, t['hle_inventory.tsv']], b.logs / 'lx_takeovers.log', show=r'^takeover check')
    b.stamp('lx_analyze', key)


def shaders(n, b, t, jobs):
    """The game's shaders as Vulkan SPIR-V in linux/shaders, the cache the runtime reads. Shaders already there are kept."""
    w = max(1, min(8, jobs)); rep = b.logs / 'lx_shaders.txt'
    n.say(f'translating the shaders for Vulkan (nxshader, {w} worker{"s" if w > 1 else ""})')
    if rep.is_file(): rep.unlink()
    cmd = shader_command(n, b, t, w, rep)
    n.run(cmd,
          b.logs / 'lx_shaders.log', env=env(n), ok=(0, 1), heartbeat=60, show=r'^shaders |^spirv-val')
    m = re.search(r'spirv-val \(vulkan1\.3\): (\d+) pass, (\d+) fail', rep.read_text(encoding='utf-8') if rep.is_file() else '')
    if not m or not int(m.group(1)):
        log = b.logs / 'lx_shaders.log'
        detail = first_exception_line(log)
        message = 'no shader was translated'
        if detail: message += f'; first exception: {detail}'
        raise n.Fail(f'{message}; see {log}')
    if int(m.group(2)): n.say(f'  {m.group(2)} shaders failed validation: their draws are skipped and logged ({rep})')


def fonts(n, b):
    """Stand-ins for the console's shared fonts from this PC's own (nothing from the console): STD_FONT and LETTER_FONT, else what fontconfig finds."""
    d = b.lx / 'fonts'; d.mkdir(parents=True, exist_ok=True)
    def find(*names):
        for x in names:
            r = subprocess.run(['fc-match', '-f', '%{file}', x], capture_output=True, text=True) if shutil.which('fc-match') else None
            if r and not r.returncode and r.stdout.lower().endswith('.ttf') and pathlib.Path(r.stdout).is_file(): return r.stdout
        return None
    std = os.environ.get('STD_FONT') or find('Arial Unicode MS', 'Liberation Sans', 'DejaVu Sans', 'Noto Sans')
    if not std or not pathlib.Path(std).is_file(): raise n.Fail('no standard font found: install fonts-liberation, or set STD_FONT=<a Unicode TTF>')
    if not (d / 'standard.ttf').is_file(): shutil.copyfile(std, d / 'standard.ttf')
    letter = os.environ.get('LETTER_FONT') or find('Arial Rounded MT Bold', 'Liberation Sans:bold', 'DejaVu Sans:bold', 'Noto Sans:bold')
    if not (d / 'nintendo_ext.ttf').is_file() and letter:
        n.run([sys.executable, n.HERE / 'tools/make_ext_font.py', d / 'nintendo_ext.ttf', letter], b.logs / 'make_ext_font.log', env=b.env())


def normalize_zbic(n, b):
    """Normalize ZBIC modules in the private build copy before nxanalyze sees them."""
    tool = tools(n)['zbic-unpack']
    for p in sorted(b.exefs.iterdir()):
        if not p.is_file(): continue
        try: head = p.read_bytes()[:0x10]
        except OSError: continue
        if len(head) >= 0x10 and head[:4] == b'NSO0' and int.from_bytes(head[0x0c:0x10], 'little') & 0x80:
            n.run([tool, p], b.logs / f'zbic-unpack-{p.name}.log')


def lift(n, b, t, jobs):
    so = b.lx / 'game.so'; db = b.lx / 'analyze' / 'program.db'
    key = n.fingerprint(db, n.digest(t['nxlift']), n.digest(t['ld.lld']))
    if b.fresh('lx_lift', key) and so.is_file(): return n.say('game.so: up to date')
    w = max(1, jobs)
    n.say(f'recompiling the game\'s code for this PC (nxlift, {w} worker{"s" if w > 1 else ""} at the lowest priority)')
    n.run([t['nxlift'], b.exefs, '--db', db, '--out', b.lx / 'lift', '--cache', b.lx / 'objcache', '--jobs', w, '--dll', so, '--lld', t['ld.lld']], b.logs / 'lx_lift.log', heartbeat=60, show=r'^nxlift:')
    if not so.is_file(): raise n.Fail(f'nxlift wrote no {so}; see {b.logs / "lx_lift.log"}')
    b.stamp('lx_lift', key)


def data_dir(n, version):
    root = pathlib.Path(os.environ.get('XDG_DATA_HOME') or pathlib.Path.home() / '.local/share') / f'{n.SLUG}-native'
    return root / version


def launcher_path(n, b): return b.outputs / f'play-{n.SLUG}-{b.version}'


def stage_lift(n, b, jobs, detach=False):
    """Only analyze and lift (no RomFS, shaders, fonts or launcher). detach: nxlift runs in its own session at idle priority, past this process."""
    t = tools(n)
    miss = [k for k in ('nxanalyze', 'nxlift', 'ld.lld', 'hle_inventory.tsv') if k in t['missing']]
    if miss: raise n.Fail(f'missing {", ".join(miss)}: build the tools (nxs2.py setup, or NXL_TOOLCHAINS=<toolchains> with cmake as in build_tools)')
    analyze(n, b, t)
    if not detach: return lift(n, b, t, jobs)
    db = b.lx / 'analyze' / 'program.db'; log = b.logs / 'lx_lift.log'
    cmd = [t['nxlift'], b.exefs, '--db', db, '--out', b.lx / 'lift', '--cache', b.lx / 'objcache', '--jobs', max(1, jobs), '--dll', b.lx / 'game.so', '--lld', t['ld.lld']]
    line = 'exec nice -n 19 ionice -c3 ' + ' '.join(shlex.quote(str(c)) for c in cmd)
    with open(log, 'wb') as f:
        p = subprocess.Popen(['setsid', 'nohup', 'sh', '-c', line], stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    (b.logs / 'lx_lift.pid').write_text(str(p.pid) + '\n')
    n.say(f'nxlift detached: pid {p.pid}, log {log}')


def play(n, b, t):
    """The runtime shader support beside the game, and its relocatable launcher out/play-<slug>-<version>."""
    rt = b.lx / 'runtime'; cp = n.copy_if_changed
    nxs = pathlib.Path(t['nxshader']).parent
    for f in sorted(nxs.glob('*.py')): cp(f, rt / 'nxshader' / f.name)
    cp(t['dmsh_abi.h'], rt / 'nxshader' / 'dmsh_abi.h')
    for rel in n.RUNTIME_INSTALLER:
        if (HERE / rel).is_file(): cp(HERE / rel, rt / 'installer' / rel)   # a new game's runtime_keys.tsv may not exist yet
    cp(n.AMPERE, rt / 'installer/ampere-lower/ampere-lower'); (rt / 'installer/ampere-lower/ampere-lower').chmod(0o755)
    translator_files = ('dmsh_xlate.dll', 'dmsh_xlate.deps.json', 'dmsh_xlate.runtimeconfig.json', 'Ryujinx.Common.dll', 'Ryujinx.Graphics.Shader.dll', 'Spv.Generator.dll',
                        'Gommon.dll', 'Humanizer.dll', 'Microsoft.IO.RecyclableMemoryStream.dll', 'MsgPack.dll', 'System.CodeDom.dll')
    for name in translator_files:
        f = pathlib.Path(t['dir']) / name
        if not f.is_file(): raise n.Fail(f'missing packaged shader translator file: {f}')
        cp(f, rt / 'installer/host/windows/dmsh_xlate_bin' / name)
    seed = pathlib.Path(os.environ['NXW_PIPELINE_SEED']) if os.environ.get('NXW_PIPELINE_SEED') else n.PROFILE_SRC / f'pipelines.{b.version}.tsv'
    if seed.is_file(): cp(seed, b.lx / 'pipelines-seed.tsv')
    q = shlex.quote; data = data_dir(n, b.version); dn = n.dotnet_root()
    b.outputs.mkdir(parents=True, exist_ok=True)
    sh = launcher_path(n, b)
    sh.write_text(f'''#!/bin/sh
# {n.NAME} {b.version} on Linux, built from your own dump by the OoT Native builder. A native program: no Wine, no translation layer.
# Switches: --fullscreen, --window 1600x900, --silent, --gamemode.
 B=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
L="$B/linux"; D="${{XDG_DATA_HOME:-$HOME/.local/share}}/{n.SLUG}-native/{b.version}"
NATIVE_HOME=${{OOT_NATIVE_HOME:?start this build through the OoT Native launcher}}
NXRT="$NATIVE_HOME/builder/bin/nxrt"
mkdir -p "$D/save" "$D/temp" "$D/logs" "$D/vulkan" "$L/shaders/dumps"
export S2_PKG_SUFFIX={q(n.PKG_SUFFIX)} S2_GAME_NAME={q(n.NAME)}
export NXRT_VK_CACHE_DIR="$D/vulkan" DM_AUDIO=1 DM_FONT_DIR="$L/fonts" NXRT_SHADER_CACHE="$L/shaders" NXRT_SHADER_BUILD="$B"
export NXRT_SHADER_TOOL="\\\"$NATIVE_HOME/python/bin/python3\\\" -I \\\"$L/runtime/nxshader/nxshader.py\\\" --game {n.TITLE} --cache \\\"$L/shaders\\\" --installer \\\"$L/runtime/installer\\\" --xlate \\\"$NATIVE_HOME/builder/bin\\\"" AMPERE_LOWER="$L/runtime/installer/ampere-lower/ampere-lower"
export DOTNET_ROOT="$NATIVE_HOME/dotnet" DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 PATH="$NATIVE_HOME/dotnet:$NATIVE_HOME/builder/bin:$PATH"
[ -f "$L/pipelines-seed.tsv" ] && export NXRT_VK_PIPELINE_SEED="$L/pipelines-seed.tsv"
PRE=""
while [ $# -gt 0 ]; do case "$1" in
  --fullscreen) export NXRT_VK_FULLSCREEN=1;;
  --window) shift; export NXRT_VK_WINDOW="$1";;
  --silent) export DM_AUDIO=0;;
  --gamemode) PRE="gamemoderun";;
  *) echo "unknown switch: $1 (--fullscreen, --window WxH, --silent, --gamemode)" >&2; exit 2;;
esac; shift; done
cd "$D" && exec $PRE "$NXRT" "$B/extract/ExeFS" --dll "$L/game.so" --romfs "$B/extract/RomFS.img" --save "$D/save" --temp "$D/temp" --log "$D/logs" --gfx vulkan
''', encoding='utf-8')
    sh.chmod(0o755)
    n.say(f'Linux game: {sh}  (save: {data / "save"})')
    return sh


def stage(n, b, jobs):
    t = tools(n)
    if t['missing']: raise n.Fail(f'the game\'s tools are not built yet (missing {", ".join(t["missing"])}): run nxs2.py setup')
    analyze(n, b, t); shaders(n, b, t, jobs); fonts(n, b); lift(n, b, t, jobs)
    return play(n, b, t)
