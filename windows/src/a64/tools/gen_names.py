"""Regenerates src/a64/names.tbl: the operand name tables (system registers, SYS aliases, hints, barriers, PSTATE fields, prefetch
operations) as LLVM's disassembler spells them with the decoder's feature set. Each space is small, so it is enumerated in full through
llvm-objdump and only the named entries are kept; the printer builds the generic forms itself.
Usage: py -3 -I gen_names.py <scratch dir> <names.tbl>"""
import os, struct, subprocess, sys

OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'
MATTR = '+v8.5a,+crypto'


def elf_with_text(code):
    shstr = b'\0.text\0.shstrtab\0'
    text_off = 0x40
    shstr_off = text_off + len(code)
    sh_off = (shstr_off + len(shstr) + 7) & ~7
    eh = b'\x7fELF' + bytes([2, 1, 1, 0]) + bytes(8)
    eh += struct.pack('<HHIQQQIHHHHHH', 1, 183, 1, 0, 0, sh_off, 0, 64, 0, 0, 64, 3, 2)
    sh = bytes(64)
    sh += struct.pack('<IIQQQQIIQQ', 1, 1, 6, 0, text_off, len(code), 0, 0, 4, 0)
    sh += struct.pack('<IIQQQQIIQQ', 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    body = eh + code + shstr
    body += bytes(sh_off - len(body))
    return body + sh


def disasm(scratch, words):
    path = os.path.join(scratch, 'gen_names.o')
    with open(path, 'wb') as f:
        f.write(elf_with_text(b''.join(struct.pack('<I', w) for w in words)))
    out = subprocess.run([OBJDUMP, '-d', '-z', '--no-show-raw-insn', '--no-leading-addr', '--mattr=' + MATTR, path],
                         capture_output=True, text=True, check=True).stdout
    os.remove(path)
    lines = [l.strip().replace('\t', ' ') for l in out.splitlines() if l.startswith(' ') or l.startswith('\t')]
    if len(lines) != len(words):
        sys.exit(f'gen_names: {len(lines)} lines for {len(words)} words')
    return lines


def main():
    scratch, out = sys.argv[1:3]
    rows = []
    # system registers, op0 = 2 + o0: key o0:op1:CRn:CRm:op2
    keys = range(1 << 15)
    for kind, base, pick in (('sysreg_r', 0xd5300000, lambda t: t.split(', ', 1)[1]),
                             ('sysreg_w', 0xd5100000, lambda t: t.split(' ', 1)[1].split(',')[0])):
        for k, t in zip(keys, disasm(scratch, [base | k << 5 for k in keys])):
            name = pick(t)
            if not name.startswith(('S2_', 'S3_')):
                rows.append((kind, k, name))
    # 128-bit system register moves (MRRS/MSRR) use the same names: checked, not stored
    named = {(kind, k): name for kind, k, name in rows}
    keys = range(1 << 15)
    for kind, base, pick in (('sysreg_r', 0xd5700000, lambda t: t.split(', ')[2]),
                             ('sysreg_w', 0xd5500000, lambda t: t.split(' ', 1)[1].split(',')[0])):
        for k, t in zip(keys, disasm(scratch, [base | k << 5 for k in keys])):
            name = pick(t)
            if named.get((kind, k), f'S{2 + (k >> 14)}_') != name and not (name.startswith(('S2_', 'S3_')) and (kind, k) not in named):
                sys.exit(f'gen_names: 128-bit name {name!r} differs for {kind} {k:#x}')
    # PSTATE fields printed as "msr NAME, #imm" for every CRm: key op1:op2
    texts = disasm(scratch, [0xd500401f | op1 << 16 | crm << 8 | op2 << 5 for op1 in range(8) for op2 in range(8) for crm in range(16)])
    for i in range(64):
        group = texts[i * 16:i * 16 + 16]
        names = {t.split(' ')[1].rstrip(',') for t in group if t.startswith('msr ') and not t.startswith('msr S0_')}
        if len(names) == 1 and all(t == f'msr {next(iter(names))}, #{c:#x}' for c, t in enumerate(group)):
            rows.append(('pstate', i, next(iter(names))))
    # hints: full text unless "hint #0x.."
    for k, t in enumerate(disasm(scratch, [0xd503201f | k << 5 for k in range(128)])):
        if t != f'hint #{k:#x}':
            rows.append(('hint', k, t))
    # barriers: full text per CRm
    for kind, base in (('dsb', 0xd503309f), ('dmb', 0xd50330bf), ('isb', 0xd50330df)):
        for k, t in enumerate(disasm(scratch, [base | k << 8 for k in range(16)])):
            rows.append((kind, k, t))
    # prefetch operations: name for prfm's Rt
    for k, t in enumerate(disasm(scratch, [0xf9800000 | k for k in range(32)])):
        name = t.split(' ')[1].rstrip(',')
        if not name.startswith('#'):
            rows.append(('prfm', k, name))
    # range prefetch operations: rprfop = option<2>:option<0>:S:Rt<2:0>
    words = [0xf8a04818 | (k >> 5 & 1) << 15 | (k >> 4 & 1) << 13 | (k >> 3 & 1) << 12 | (k & 7) for k in range(64)]
    for k, t in enumerate(disasm(scratch, words)):
        name = t.split(' ')[1].rstrip(',')
        if not t.startswith('rprfm '):
            sys.exit(f'gen_names: unexpected RPRFM form {t!r}')
        if not name.startswith('#'):
            rows.append(('rprfm', k, name))
    # SYS aliases, key op1:CRn:CRm:op2: "sys" holds the text before the register (any Rt), "sysz" the full text for Rt = 31 only
    keys = range(1 << 14)
    r0 = disasm(scratch, [0xd5080000 | k << 5 for k in keys])
    r31 = disasm(scratch, [0xd5080000 | k << 5 | 31 for k in keys])
    for k, a, b in zip(keys, r0, r31):
        if not a.startswith('sys '):
            if not a.endswith('x0') or b != a[:-2] + 'xzr':
                sys.exit(f'gen_names: unexpected SYS alias form {a!r} / {b!r}')
            rows.append(('sys', k, a[:-2]))
        elif not b.startswith('sys '):
            rows.append(('sysz', k, b))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'# Generated by tools/gen_names.py from LLVM llvm-objdump --mattr={MATTR}. Do not edit.\n')
        f.write('# <kind> <hex key> <text>; see gen_names.py for the keys\n')
        for kind, k, t in rows:
            f.write(f'{kind} {k:x} {t}\n')
    print(f'gen_names: {len(rows)} names')


if __name__ == '__main__':
    main()
