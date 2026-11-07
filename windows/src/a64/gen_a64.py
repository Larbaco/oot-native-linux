"""Generates the A64 decoder and printer from the pattern table.
Usage: py -3 -I gen_a64.py <a64.tbl> <names.tbl> <out dir>   (writes <out>/nxw/a64_gen.hpp and <out>/a64_gen.cpp)

a64.tbl, one encoding per line (first match in table order wins; the generator keeps that order inside the decision tree):
    NAME  <pattern tokens, MSB first, 32 bits>  [!field=bits[,field=bits]...]  [@ext]  [? <C++ condition>]  | <format>
    = <C++ condition> | <format>          alias for the line above, tried in order; the first true one is printed
Pattern tokens: 0/1 runs (fixed), x runs (ignored), `name` (1-bit field) or `name:N` (N-bit field).
`!` excludes words whose fields match (x = any bit); `?` adds a decode condition over the fields. ' | ' (with spaces) separates
the format, so bitwise or is written without spaces. Formats are LLVM assembly with {helper arg, ...} calls into the Printer
(print_impl.hpp); arguments and conditions are C++ expressions over the fields, `w` (the word) and `pc`.
names.tbl: `<kind> <hex key> <text>` lines, emitted as sorted detail::names_<kind> tables for the printer."""
import os, re, sys
from dataclasses import dataclass, field


@dataclass
class Enc:
    name: str
    line: int
    fields: list            # (name, lo, width)
    mask: int
    value: int
    excl: list              # (mask, value)
    ext: str
    fmt: str
    pred: str               # C++ decode condition over the fields ('' = none)
    aliases: list = field(default_factory=list)   # (cond, fmt)


def die(msg):
    sys.exit(f'gen_a64: {msg}')


def parse_table(path):
    encs, names = [], set()
    for no, raw in enumerate(open(path, encoding='utf-8'), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('='):
            if not encs:
                die(f'{path}:{no}: alias before any encoding')
            cond, _, fmt = line[1:].partition(' | ')
            if not fmt.strip():
                die(f'{path}:{no}: alias without format')
            encs[-1].aliases.append((cond.strip(), fmt.strip()))
            continue
        left, bar, fmt = line.partition(' | ')
        if not bar:
            die(f'{path}:{no}: missing | format')
        left, _, pred = left.partition('?')
        toks = left.split()
        name, toks = toks[0], toks[1:]
        if name in names:
            die(f'{path}:{no}: duplicate name {name}')
        names.add(name)
        pos, mask, value, fields, excl_toks, ext = 32, 0, 0, [], [], ''
        for t in toks:
            if t.startswith('!'):
                excl_toks.append(t[1:]); continue
            if t.startswith('@'):
                ext = t[1:]; continue
            if re.fullmatch(r'[01]+', t):
                for ch in t:
                    pos -= 1
                    mask |= 1 << pos
                    value |= int(ch) << pos
            elif re.fullmatch(r'x+', t):
                pos -= len(t)
            else:
                m = re.fullmatch(r'([A-Za-z_]\w*)(?::(\d+))?', t)
                if not m:
                    die(f'{path}:{no}: bad token {t}')
                width = int(m.group(2) or 1)
                pos -= width
                fields.append((m.group(1), pos, width))
        if pos != 0:
            die(f'{path}:{no}: pattern has {32 - pos} bits')
        fmap = {f[0]: f for f in fields}
        if len(fmap) != len(fields):
            die(f'{path}:{no}: repeated field name')
        excl = []
        for et in excl_toks:
            em = ev = 0
            for part in et.split(','):
                fname, _, bits = part.partition('=')
                if fname not in fmap or len(bits) != fmap[fname][2]:
                    die(f'{path}:{no}: bad exclusion {part}')
                lo, width = fmap[fname][1], fmap[fname][2]
                for i, ch in enumerate(bits):
                    b = lo + width - 1 - i
                    if ch in '01':
                        em |= 1 << b
                        ev |= int(ch) << b
            excl.append((em, ev))
        encs.append(Enc(name, no, fields, mask, value, excl, ext, fmt.strip(), pred.strip()))
    return encs


def parse_names(path):
    """names.tbl: `<kind> <hex key> <text...>` lines -> {kind: [(key, text)]}"""
    out = {}
    for raw in open(path, encoding='utf-8'):
        line = raw.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        kind, key, text = line.split(' ', 2)
        out.setdefault(kind, []).append((int(key, 16), text))
    return out


# ---- decision tree ----

def matches_any(e, w_mask, w_value):
    """can encoding e match some word with the bits in w_mask fixed to w_value"""
    common = e.mask & w_mask
    return (e.value & common) == (w_value & common)


class Gen:
    def __init__(self):
        self.out = []
        self.nodes = 0

    def emit(self, depth, s):
        self.out.append('    ' * depth + s)

    def leaf(self, depth, cand, known):
        for e in cand:
            need = e.mask & ~known
            conds = []
            if need:
                conds.append(f'(w & {need:#010x}u) == {e.value & need:#010x}u')
            for em, ev in e.excl:
                conds.append(f'(w & {em:#010x}u) != {ev:#010x}u')
            if e.pred:
                conds.append(f'pred_{e.name}(w)')
            if conds:
                self.emit(depth, f'if ({" && ".join(conds)}) return Op::{e.name};')
            else:
                self.emit(depth, f'return Op::{e.name};')
                return
        self.emit(depth, 'return Op::Unallocated;')

    def node(self, depth, cand, known):
        self.nodes += 1
        if len(cand) <= 2:
            return self.leaf(depth, cand, known)
        common = ~known & 0xFFFFFFFF
        for e in cand:
            common &= e.mask
        if common:
            # longest run of commonly fixed bits, at most 7
            best = (0, 0)
            b = 0
            while b < 32:
                if common >> b & 1:
                    s = b
                    while b < 32 and common >> b & 1:
                        b += 1
                    if b - s > best[1]:
                        best = (s, b - s)
                else:
                    b += 1
            lo, n = best
            if n > 7:
                lo, n = lo + n - 7, 7
            fm = ((1 << n) - 1) << lo
            groups = {}
            for e in cand:
                groups.setdefault((e.value & fm) >> lo, []).append(e)
            self.emit(depth, f'switch ((w >> {lo}) & {(1 << n) - 1:#x}) {{')
            for v in sorted(groups):
                self.emit(depth, f'case {v:#x}: {{')
                self.node(depth + 1, groups[v], known | fm)
                self.emit(depth, '}')
            self.emit(depth, 'default: return Op::Unallocated;')
            self.emit(depth, '}')
            return
        # no bit fixed in all candidates: split on the bit fixed in the most, duplicating the rest (order kept)
        best_bit, best_n = -1, 0
        for b in range(32):
            if known >> b & 1:
                continue
            n = sum(1 for e in cand if e.mask >> b & 1)
            if n > best_n:
                best_bit, best_n = b, n
        if best_bit < 0 or best_n < 2:
            return self.leaf(depth, cand, known)
        bm = 1 << best_bit
        zero = [e for e in cand if matches_any(e, bm, 0)]
        one = [e for e in cand if matches_any(e, bm, bm)]
        self.emit(depth, f'if (w & {bm:#010x}u) {{')
        self.node(depth + 1, one, known | bm)
        self.emit(depth, '} else {')
        self.node(depth + 1, zero, known | bm)
        self.emit(depth, '}')


# ---- formats ----

def split_args(s):
    args, depth, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return args


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def compile_fmt(fmt, where):
    """-> list of C++ statements"""
    stmts, i = [], 0
    while i < len(fmt):
        j = fmt.find('{', i)
        if j < 0:
            stmts.append(f'P.lit({cstr(fmt[i:])});'); break
        if j > i:
            stmts.append(f'P.lit({cstr(fmt[i:j])});')
        k = fmt.find('}', j)
        if k < 0:
            die(f'{where}: unclosed {{ in format')
        body = fmt[j + 1:k].strip()
        m = re.match(r'(\w+)\s*(.*)', body)
        if not m:
            die(f'{where}: bad call {{{body}}}')
        stmts.append(f'P.{m.group(1)}({", ".join(split_args(m.group(2)))});')
        i = k + 1
    return stmts


def main():
    tbl, names_path, out = sys.argv[1:4]
    encs = parse_table(tbl)
    names = parse_names(names_path)
    os.makedirs(os.path.join(out, 'nxw'), exist_ok=True)

    h = ['// Generated by src/a64/gen_a64.py from a64.tbl. Do not edit.', '#pragma once', '#include <cstdint>', '',
         'namespace nxw::a64 {', '', 'enum class Op : uint16_t {', '    Unallocated,']
    h += [f'    {e.name},' for e in encs]
    h += ['};', f'inline constexpr unsigned op_count = {len(encs) + 1};', '',
          'template <Op> struct Fields;']
    for e in encs:
        h.append(f'template <> struct Fields<Op::{e.name}> {{')
        if e.fields:
            h.append('    uint32_t ' + ', '.join(f[0] for f in e.fields) + ';')
            init = ', '.join(f'{f[0]}(w >> {f[1]} & {(1 << f[2]) - 1:#x})' for f in e.fields)
            h.append(f'    constexpr explicit Fields(uint32_t w) : {init} {{}}')
        else:
            h.append('    constexpr explicit Fields(uint32_t) {}')
        h.append('};')
    h += ['', '}  // namespace nxw::a64', '']

    c = ['// Generated by src/a64/gen_a64.py from a64.tbl. Do not edit.', '#include "nxw/a64.hpp"', '#include "print_impl.hpp"', '',
         'namespace nxw::a64 {', '',
         'namespace {', 'constexpr const char* kNames[] = {', '    "unallocated",']
    c += [f'    "{e.name}",' for e in encs]
    c += ['};']
    c.append('constexpr FieldInfo kFields[] = {')
    starts, n = [], 0
    for e in encs:
        starts.append(n)
        for f in e.fields:
            c.append(f'    {{"{f[0]}", {f[1]}, {f[2]}}},')
            n += 1
    c.append('    {"", 0, 0},')
    c.append('};')
    c.append('constexpr uint16_t kFieldStart[] = {0, ' + ', '.join(str(s) for s in starts) + f', {n}}};')
    c.append('constexpr Encoding kEncodings[] = {')
    c.append('    {0, 0, ""},')
    for e in encs:
        c.append(f'    {{{e.mask:#010x}u, {e.value:#010x}u, "{e.ext}"}},')
    c.append('};')
    c += ['}  // namespace', '',
          'const char* op_name(Op op) { return kNames[static_cast<unsigned>(op)]; }',
          'std::span<const FieldInfo> op_fields(Op op) {',
          '    auto i = static_cast<unsigned>(op);',
          '    if (i == 0) return {};',
          '    return {kFields + kFieldStart[i], kFields + kFieldStart[i + 1]};',
          '}',
          'const Encoding& op_encoding(Op op) { return kEncodings[static_cast<unsigned>(op)]; }', '']

    c.append('namespace {')
    c.append('using namespace detail;')
    for e in encs:
        if e.pred:
            c.append(f'inline bool pred_{e.name}(uint32_t w) {{')
            for f in e.fields:
                c.append(f'    [[maybe_unused]] const uint32_t {f[0]} = w >> {f[1]} & {(1 << f[2]) - 1:#x};')
            c.append(f'    return {e.pred};')
            c.append('}')
    c.append('}  // namespace')
    g = Gen()
    g.node(1, encs, 0)
    c += ['Op decode(uint32_t w) {'] + g.out + ['}', '']

    # names
    c.append('namespace detail {')
    for kind in sorted(names):
        items = sorted(names[kind])
        c.append(f'const NameEntry k_{kind}[] = {{')
        for key, text in items:
            c.append(f'    {{{key:#x}, {cstr(text)}}},')
        c.append('};')
        c.append(f'const std::span<const NameEntry> names_{kind}{{k_{kind}}};')
    c.append('}  // namespace detail')
    c.append('')

    c += ['void print_body(Printer& P, Op op, uint32_t w) {', '    [[maybe_unused]] const uint64_t pc = P.pc;',
          '    using namespace detail;', '    switch (op) {', '    case Op::Unallocated: P.lit("<unknown>"); return;']
    for e in encs:
        c.append(f'    case Op::{e.name}: {{')
        for f in e.fields:
            c.append(f'        [[maybe_unused]] const uint32_t {f[0]} = w >> {f[1]} & {(1 << f[2]) - 1:#x};')
        where = f'{tbl}:{e.line}'
        for cond, fmt in e.aliases:
            c.append(f'        if ({cond}) {{')
            c += ['            ' + s for s in compile_fmt(fmt, where)]
            c.append('            return;')
            c.append('        }')
        c += ['        ' + s for s in compile_fmt(e.fmt, where)]
        c.append('        return;')
        c.append('    }')
    c += ['    }', '}', '', '}  // namespace nxw::a64', '']

    def write(path, lines):
        text = '\n'.join(lines)
        if os.path.exists(path) and open(path, encoding='utf-8').read() == text:
            return
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    write(os.path.join(out, 'nxw', 'a64_gen.hpp'), h)
    write(os.path.join(out, 'a64_gen.cpp'), c)
    print(f'gen_a64: {len(encs)} encodings, {sum(len(e.aliases) for e in encs)} aliases, {g.nodes} tree nodes')


if __name__ == '__main__':
    main()
