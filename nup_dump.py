"""nup_dump.py: a game dump read straight from one .nup file (https://github.com/a55uka/nup), with no folder extracted first.

The build needs three things from a dump: the ExeFS files, the RomFS image and, once, the few RomFS files that hold the shaders. Each comes out of the
container directly: the ExeFS into the build folder, the RomFS streamed into the image (the same bytes dm/tools/romfs_image.py writes for the same tree), and
the shader files copied out of that image for as long as the corpus stage reads them. Every file is checked against the container's SHA-256 as it is read.
"""
import contextlib, hashlib, json, os, pathlib, shutil, struct, sys, time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'third_party'))
from nup import format as F
from nup.read import NupReader

MAGIC = b'DMROMFS1'   # the image's layout: dm/tools/romfs_image.py in nxnative
HEAD = struct.Struct('<8sQQQ')
ENT = struct.Struct('<QQIHBB')
CHUNK = 8 << 20
STAMP_VERSION = 1   # raise it when the image this module writes changes


class Error(Exception): pass


@contextlib.contextmanager
def _reader(path):
    try:
        with NupReader(str(path)) as r: yield r
    except F.NupError as e: raise Error(f'{path}: {e}') from e


def _children(r, d):
    """A directory's children, sorted by name: (name, is dir, index)."""
    _, first, count = r.dirs[d]
    out = []
    for cid in r.children[first:first + count]:   # a child id below dir_count is a directory, the rest are files (SPEC section 4)
        isdir, i = cid < r.dir_count, cid if cid < r.dir_count else cid - r.dir_count
        out.append((r.dirs[i][0] if isdir else r.files[i][0], isdir, i))
    return sorted(out)


def _walk(r, d, prefix=''):
    for name, isdir, i in _children(r, d):
        p = prefix + name.decode('utf-8', 'surrogateescape')
        if isdir: yield from _walk(r, i, p + '/')
        else: yield p, i


def _stream(r, i, write):
    """One file's bytes, in chunks, to write(); raises Error unless they are the size and SHA-256 the container lists."""
    name, size, sha, off, stored, codec = r.files[i]
    h, n = hashlib.sha256(), 0
    if size:
        r._fh.seek(off)
        if codec == F.CODEC_ZSTD:
            try: import zstandard
            except ImportError: raise Error(f'reading a .nup needs zstandard: {sys.executable} -m pip install zstandard') from None
            try:
                with zstandard.ZstdDecompressor().stream_reader(r._fh, closefd=False) as z:
                    while n < size and (chunk := z.read(CHUNK)):   # n < size: past a file's one frame the reader would go on into the padding before the next
                        h.update(chunk); write(chunk); n += len(chunk)
            except zstandard.ZstdError as e: raise Error(f'{r.path}: {name.decode("utf-8", "replace")} is damaged: {e}') from e
        else:
            left = stored
            while left and (chunk := r._fh.read(min(left, CHUNK))):
                h.update(chunk); write(chunk); n += len(chunk); left -= len(chunk)
    if n != size or (size and h.digest() != sha): raise Error(f'{r.path}: {name.decode("utf-8", "replace")} does not match its checksum: the file is damaged')


def _sha_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        while chunk := f.read(CHUNK): h.update(chunk)
    return h.digest()


def _file_identity(p):
    s = os.stat(p)
    return [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]


class Package:
    """One package of a .nup that has both an ExeFS and a RomFS. `package / 'a/b'` is a RomFS file as a real path, for the tools that read files
    (after use_image: copied out of the image on first use, until discard_files)."""

    def __init__(self, path, index, kind, version):
        self.path, self.index, self.kind, self.version = pathlib.Path(path), index, kind, version
        self.parent = f'{self.path} (package {index}: {kind} v{version})'
        self.image = self.files_dir = None

    def _pkg(self, r): return r.packages[self.index]

    def build_id(self, module):
        """The NSO's ModuleId, as nxs2.py's build_id reads it from a file; None when the module is not in the ExeFS."""
        with _reader(self.path) as r:
            for n, isdir, i in _children(r, self._pkg(r).exefs_dir):
                if n.decode('utf-8', 'surrogateescape') == module and not isdir:
                    head = bytearray()
                    class Enough(Exception): pass
                    def take(chunk):
                        head.extend(chunk[:0x50])
                        raise Enough
                    try: _stream(r, i, take)
                    except Enough: pass
                    return bytes(head[0x40:0x50]).hex()
        return None

    def romfs_paths(self):
        with _reader(self.path) as r: return {p for p, _ in _walk(r, self._pkg(r).romfs_dir)}

    def copy_exefs(self, dest):
        """The ExeFS's files into dest; a file already there with the same content is left alone."""
        dest = pathlib.Path(dest); dest.mkdir(parents=True, exist_ok=True)
        with _reader(self.path) as r:
            for n, isdir, i in _children(r, self._pkg(r).exefs_dir):
                if isdir: continue
                p = dest / n.decode('utf-8', 'surrogateescape')
                if p.is_file() and p.stat().st_size == r.files[i][1] and _sha_file(p) == r.files[i][2]: continue
                part = p.with_name(p.name + '.part')
                with open(part, 'wb') as out: _stream(r, i, out.write)
                os.replace(part, p)

    def write_image(self, dst, progress=None):
        """The RomFS image at dst, unless dst is already this package's. Returns True when it wrote one."""
        dst = pathlib.Path(dst); stamp = pathlib.Path(f'{dst}.inputs.json'); part = pathlib.Path(f'{dst}.part')
        with _reader(self.path) as r:
            pkg = self._pkg(r)
            source = hashlib.sha256(json.dumps([STAMP_VERSION, r.super['toc_sha256'].hex(), self.index, pkg.romfs_dir]).encode()).hexdigest()
            with contextlib.suppress(OSError, ValueError):
                if json.loads(stamp.read_text()) == {'source': source, 'image': _file_identity(dst)}: return False
            ents = [[b'', True, pkg.romfs_dir, 0, 0]]   # name, is dir, index, a, b: breadth-first, each directory's children one consecutive run, as romfs_image.py lays them out
            k = 0
            while k < len(ents):
                if ents[k][1]:
                    kids = _children(r, ents[k][2])
                    ents[k][3], ents[k][4] = len(ents), len(kids)
                    ents.extend([n, d, i, 0, 0] for n, d, i in kids)
                k += 1
            total = sum(r.files[e[2]][1] for e in ents if not e[1])
            dst.parent.mkdir(parents=True, exist_ok=True)
            need = total + sum(ENT.size + len(e[0]) for e in ents) + HEAD.size
            free = shutil.disk_usage(dst.parent).free + (part.stat().st_size if part.exists() else 0)
            if free < need: raise Error(f'the RomFS image needs {need / (1 << 30):.1f} GB and {dst.parent} has {free / (1 << 30):.1f} GB free')
            done, t_beat = 0, time.time()
            with open(part, 'wb') as out:
                out.write(b'\0' * HEAD.size)
                off = HEAD.size
                def write(chunk):
                    nonlocal done, t_beat
                    out.write(chunk); done += len(chunk)
                    if progress and time.time() - t_beat >= 30:
                        t_beat = time.time(); progress(done, total)
                for e in ents:
                    if e[1]: continue
                    start = done
                    _stream(r, e[2], write)
                    e[3], e[4] = off, done - start; off += done - start
                pool, idx = bytearray(), bytearray()
                for name, isdir, _, a, b in ents:
                    idx += ENT.pack(a, b, len(pool), len(name), 0 if isdir else 1, 0); pool += name
                out.write(idx); out.write(pool)
                out.seek(0); out.write(HEAD.pack(MAGIC, off, len(idx) + len(pool), len(ents)))
        os.replace(part, dst)   # the index comes last and the name only now, so a cut-off build leaves no image that looks whole
        tmp = pathlib.Path(f'{stamp}.part')
        tmp.write_text(json.dumps({'source': source, 'image': _file_identity(dst)}))
        os.replace(tmp, stamp)
        return True

    def use_image(self, image, files_dir):
        self.image, self.files_dir = pathlib.Path(image), pathlib.Path(files_dir)

    def _image_entry(self, rel):
        """(offset, size) of a file in the image."""
        with open(self.image, 'rb') as f:
            magic, ioff, isz, n = HEAD.unpack(f.read(HEAD.size))
            if magic != MAGIC: raise Error(f'{self.image} is not a RomFS image')
            f.seek(ioff); raw = f.read(isz)
        pool = raw[n * ENT.size:]
        k = 0
        for part in rel.encode('utf-8', 'surrogateescape').split(b'/'):
            first, count, *_ = ENT.unpack_from(raw, k * ENT.size)
            for c in range(first, first + count):
                _, _, no, nl, _, _ = ENT.unpack_from(raw, c * ENT.size)
                if pool[no:no + nl] == part: k = c; break
            else: return None
        a, b, _, _, t, _ = ENT.unpack_from(raw, k * ENT.size)
        return (a, b) if t == 1 else None

    def __truediv__(self, rel):
        if not self.image: raise Error('use_image first: the RomFS files come out of the image')
        rel = str(rel); p = self.files_dir / rel
        if p.is_file(): return p
        where = self._image_entry(rel)
        if not where: return p   # the caller reports it as missing, as for a folder
        p.parent.mkdir(parents=True, exist_ok=True)
        h, left = hashlib.sha256(), where[1]
        with open(self.image, 'rb') as src, open(p, 'wb') as out:
            src.seek(where[0])
            while left:
                chunk = src.read(min(left, CHUNK))
                if not chunk: raise Error(f'{self.image} is cut off')
                h.update(chunk); out.write(chunk); left -= len(chunk)
        with _reader(self.path) as r:
            want = next((r.files[i][2] for q, i in _walk(r, self._pkg(r).romfs_dir) if q == rel), None)
        if h.digest() != want:
            p.unlink(); raise Error(f'{self.image} does not hold the {rel} of {self.path}: delete the image and build again')
        return p

    def discard_files(self):
        if self.files_dir and self.files_dir.is_dir(): shutil.rmtree(self.files_dir)


def candidates(path):
    """The container's packages that could be the game, as the folder search orders them: the newest update first, the base last. An update's RomFS is used as
    it is, never laid over the base's: a dump's update holds the whole game's files."""
    with _reader(path) as r:
        names = {F.KIND_BASE: 'base', F.KIND_UPDATE: 'update'}
        have = [p for p in r.packages if p.kind in names and p.exefs_dir != F.NO_DIR and p.romfs_dir != F.NO_DIR]
        have.sort(key=lambda p: (p.kind == F.KIND_UPDATE, p.version), reverse=True)
        return [Package(path, p.index, names[p.kind], p.version) for p in have]


def describe(path):
    """Every package, for the message when none is the game."""
    with _reader(path) as r:
        return [f'package {p.index}: {F.KIND_NAMES.get(p.kind, p.kind)} v{p.version}' + ('' if p.exefs_dir != F.NO_DIR else ', no ExeFS') + ('' if p.romfs_dir != F.NO_DIR else ', no RomFS')
                for p in r.packages]
