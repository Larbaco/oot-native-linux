"""Writes the decompressed text segment of each NSO module to <out>/<module>.text (raw little-endian words, length rounded down to 4).
Game code: <out> must be under _local/. Usage: py -3 -I nso_text.py <ExeFS dir> <out dir> [module ...]"""
import hashlib, os, struct, sys

import lz4.block


def text_of(path):
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0':
        raise SystemExit(f'{path}: not an NSO')
    flags = struct.unpack_from('<I', d, 0xC)[0]
    fo, mo, sz = struct.unpack_from('<III', d, 0x10)
    csz = struct.unpack_from('<I', d, 0x60)[0]
    raw = d[fo:fo + (csz if flags & 1 else sz)]
    if flags & 1:
        raw = lz4.block.decompress(raw, uncompressed_size=sz)
    if len(raw) != sz or mo != 0:
        raise SystemExit(f'{path}: unexpected text segment')
    if flags & 8 and hashlib.sha256(raw).digest() != d[0xA0:0xC0]:
        raise SystemExit(f'{path}: text SHA-256 mismatch')
    return raw[:sz & ~3]


def main():
    exefs, out = sys.argv[1:3]
    os.makedirs(out, exist_ok=True)
    for name in sys.argv[3:] or ['main', 'sdk', 'subsdk0', 'rtld']:
        t = text_of(os.path.join(exefs, name))
        with open(os.path.join(out, f'{name}.text'), 'wb') as f:
            f.write(t)
        print(f'{name}: {len(t) // 4} words')


if __name__ == '__main__':
    main()
