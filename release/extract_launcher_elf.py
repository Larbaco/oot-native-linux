#!/usr/bin/env python3
"""Extract the shared libraries embedded in the PyInstaller launcher (for ldd checks) and verify its license map.
usage: extract_launcher_elf.py <out dir> <launcher> <package dir>"""
import pathlib, sys
from PyInstaller.archive.readers import CArchiveReader
out, exe, package = map(pathlib.Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
archive = CArchiveReader(str(exe))
external_libraries = set()
for name in archive.toc:
    if '.so' not in name.lower():
        continue
    data = archive.extract(name)
    if not isinstance(data, bytes) or data[:4] != bytes((127, 69, 76, 70)):
        continue
    library = pathlib.PurePosixPath(name).name
    if library.startswith('lib') and library not in {'libpython3.12.so.1.0', 'libtcl8.6.so', 'libtk8.6.so'}:
        external_libraries.add(library)
    relative = pathlib.PurePosixPath(name)
    if relative.is_absolute() or '..' in relative.parts:
        raise SystemExit(f'unsafe PyInstaller archive member {name!r}')
    target = out / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    target.chmod(0o644)
count = sum(1 for p in out.rglob('*') if p.is_file())
if not count:
    raise SystemExit('no embedded ELF libraries extracted from the PyInstaller launcher')
manifest = package/'LICENSES/PyInstaller-embedded-libraries.txt'
licensed = {line.split(' -> ',1)[0] for line in manifest.read_text().splitlines() if ' -> ' in line}
if external_libraries != licensed:
    raise SystemExit('PyInstaller shared-library license map mismatch: '
                     f'unlicensed={sorted(external_libraries-licensed)} '
                     f'not-embedded={sorted(licensed-external_libraries)}')
print(f'extracted {count} PyInstaller ELF files for ldd validation')
