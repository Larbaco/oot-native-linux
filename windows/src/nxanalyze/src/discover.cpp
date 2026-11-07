// Function discovery: FDEs first, then every other source of function starts; code outside the FDEs is partitioned by those
// starts plus a conservative prologue scan.
#include "internal.hpp"

#include <algorithm>
#include <cstdio>

namespace nxanalyze {

using namespace nxload;

std::string hex(uint64_t v) {
    char s[24];
    std::snprintf(s, sizeof s, "%#llx", static_cast<unsigned long long>(v));
    return s;
}

int ModCtx::fde_at(uint32_t off) const {
    auto it = std::upper_bound(fde_ranges.begin(), fde_ranges.end(), off, [](uint32_t o, const auto& r) { return o < r.first; });
    if (it == fde_ranges.begin()) return -1;
    --it;
    return off < it->second ? int(it - fde_ranges.begin()) : -1;
}

int ModCtx::fn_at(uint32_t off) const {
    auto it = std::upper_bound(fns.begin(), fns.end(), off, [](uint32_t o, const Fn& f) { return o < f.start; });
    if (it == fns.begin()) return -1;
    --it;
    return off < it->end ? int(it - fns.begin()) : -1;
}

int ModCtx::fn_starting(uint32_t off) const {
    auto it = std::lower_bound(fns.begin(), fns.end(), off, [](const Fn& f, uint32_t o) { return f.start < o; });
    return it != fns.end() && it->start == off ? int(it - fns.begin()) : -1;
}

std::pair<ModCtx*, int> Ctx::fn_at_abs(uint64_t a) {
    for (auto& c : mods)
        if (a >= c.base && a - c.base < c.text) {
            const int f = c.fn_starting(uint32_t(a - c.base));
            return {f >= 0 ? &c : nullptr, f};
        }
    return {nullptr, -1};
}

void decode_module(ModCtx& c) {
    const Module& m = *c.m;
    c.text = m.seg(seg_text).size & ~3u;
    c.ops.resize(c.text / 4);
    for (uint32_t k = 0; k < c.text / 4; k++) c.ops[k] = nxw::a64::decode(c.word(k * 4));
    for (uint64_t a : m.relr) c.rel_at[uint32_t(a)] = {0, 0};
    for (const auto* t : {&m.rela, &m.jmprel})
        for (const Reloc& r : *t) c.rel_at[uint32_t(r.offset)] = {r.type, r.sym};
    for (uint32_t s = 1; s < m.symbols.size(); s++) {
        const Symbol& y = m.symbols[s];
        if (y.defined() && y.shndx != SHN_ABS && !y.name.empty()) c.sym_at.emplace(uint32_t(y.value), s);
        if (!y.defined() || y.shndx == SHN_ABS || y.name.empty() || y.value >= c.text) continue;
        if (y.type != STT_FUNC && y.type != STT_NOTYPE) continue;
        auto [it, fresh] = c.names.emplace(uint32_t(y.value), y.name);
        if (!fresh && !m.symbols[m.lookup(it->second)].exported() && y.exported()) it->second = y.name;
    }
    c.eh = parse_eh_frame(m);
    for (const Fde& f : c.eh.fdes) c.fde_ranges.push_back({uint32_t(f.start), uint32_t(f.start + f.size)});
    std::sort(c.fde_ranges.begin(), c.fde_ranges.end());
}

namespace {

bool is_header_word(uint32_t off) { return off == 4 || off == 8; }   // MOD0 offset and the word after it

void seed(ModCtx& c, uint64_t off, uint16_t src) {
    if (off < c.text && off % 4 == 0 && !is_header_word(uint32_t(off))) c.seeds[uint32_t(off)] |= src;
}

// adrp x16, page; ldr x17, [x16, #lo]; add x16, x16, #lo; br x17
bool plt_stub_at(const ModCtx& c, uint32_t off, uint32_t& got) {
    if (off + 16 > c.text) return false;
    const Insn a = c.insn(off), l = c.insn(off + 4), d = c.insn(off + 8), b = c.insn(off + 12);
    if (a.op != Op::ADRP || (a.w & 31) != 16 || l.op != Op::LDR_imm || d.op != Op::ADD_imm || b.op != Op::BR) return false;
    const F<Op::LDR_imm> lf(l.w);
    const F<Op::ADD_imm> df(d.w);
    if (!lf.sf || lf.Rn != 16 || lf.Rt != 17 || df.Rd != 16 || df.Rn != 16 || df.sh || F<Op::BR>(b.w).Rn != 17) return false;
    const F<Op::ADRP> af(a.w);
    const uint64_t page = (uint64_t(off) & ~0xfffull) + uint64_t(sext(af.immhi << 2 | af.immlo, 21) * 4096);
    got = uint32_t(page + lf.imm12 * 8);
    return uint32_t(page + df.imm12) == got;
}

bool is_terminator(Insn i) {
    switch (i.op) {
    case Op::RET: case Op::RETAA: case Op::B: case Op::BR: case Op::BRAA: case Op::BRAAZ: case Op::UDF: case Op::BRK: return true;
    default: return false;
    }
}

bool looks_like_prologue(Insn i) {
    switch (i.op) {
    case Op::HINT: {
        const uint32_t h = hint_no(i.w);
        return h == hint_pacibsp || h == hint_paciasp;
    }
    case Op::STP_pre: {
        const F<Op::STP_pre> f(i.w);
        return f.sf && f.Rn == 31 && sext(f.imm7, 7) < 0 && f.Rt >= 19 && f.Rt2 >= 19;
    }
    case Op::SUB_imm: {
        const F<Op::SUB_imm> f(i.w);
        return f.sf && f.Rd == 31 && f.Rn == 31;
    }
    default: return false;
    }
}

}  // namespace

void discover(Ctx& x, ModCtx& c) {
    const Module& m = *c.m;
    auto& st = c.stat;
    for (const Fde& f : c.eh.fdes) seed(c, f.start, src_fde);
    seed(c, 0, src_entry);

    for (uint32_t s = 1; s < m.symbols.size(); s++) {
        const Symbol& y = m.symbols[s];
        if (!y.defined() || y.shndx == SHN_ABS || y.name.empty() || y.value >= c.text) continue;
        if (y.type != STT_FUNC && y.type != STT_NOTYPE) continue;
        const int f = c.fde_at(uint32_t(y.value));
        if (f >= 0 && c.fde_ranges[size_t(f)].first != y.value) {
            st["symbols_mid_function"]++;
            continue;
        }
        seed(c, y.value, y.exported() ? src_export : src_symbol);
    }

    for (int64_t tag : {DT_INIT, DT_FINI})
        if (auto v = m.dyn.get(tag)) seed(c, *v, src_init);
    for (auto [arr, sz] : {std::pair{DT_INIT_ARRAY, DT_INIT_ARRAYSZ}, {DT_FINI_ARRAY, DT_FINI_ARRAYSZ}, {DT_PREINIT_ARRAY, DT_PREINIT_ARRAYSZ}}) {
        const uint64_t a = m.dyn.value(arr), n = m.dyn.value(sz) / 8;
        for (uint64_t k = 0; k < n; k++) {
            const uint64_t v = c.read64_rel(uint32_t(a + 8 * k));
            if (v >= c.base) seed(c, v - c.base, src_init);
        }
    }

    for (uint64_t t : m.relative_targets()) {
        if (t >= c.text) continue;
        const int f = c.fde_at(uint32_t(t));
        if (f >= 0 && c.fde_ranges[size_t(f)].first != t) {
            st["reloc_targets_mid_function"]++;
            continue;
        }
        seed(c, t, src_reloc);
    }

    // PLT: the stubs and the header that precedes them, all outside the FDEs
    for (uint32_t off = 0; off + 16 <= c.text; off += 4) {
        uint32_t got;
        if (c.fde_at(off) >= 0 || !plt_stub_at(c, off, got)) continue;
        auto r = c.rel_at.find(got);
        if (r != c.rel_at.end() && r->second.type == R_AARCH64_JUMP_SLOT) {
            c.plt_sym[off] = r->second.sym;
            c.plt_got[off] = got;
            seed(c, off, src_plt);
        } else if (off >= 4 && c.insn(off - 4).op == Op::STP_pre) {
            c.plt0[off - 4] = 32;
            seed(c, off - 4, src_plt);
        }
        off += 12;
    }

    // Direct branches and address computations, over every word of text
    for (uint32_t off = 0; off < c.text; off += 4) {
        if (is_header_word(off)) continue;
        const Insn i = c.insn(off);
        const FlowInfo fl = flow(i, off);
        if (fl.kind == Flow::bl) {
            if (fl.target < c.text) seed(c, fl.target, src_bl);
            else st["bl_outside_text"]++;
        } else if (fl.kind == Flow::b || fl.kind == Flow::bcond) {
            const int f = c.fde_at(off);
            if (f < 0) continue;   // code without FDEs: decided once it is partitioned
            const auto [s, e] = c.fde_ranges[size_t(f)];
            if (fl.target >= s && fl.target < e) continue;
            seed(c, fl.target, src_tail);
            if (fl.kind == Flow::bcond) st["conditional_branches_leaving_fde"]++;
        } else if (i.op == Op::ADR) {
            const F<Op::ADR> a(i.w);
            c.adr_refs.push_back({off, uint32_t(off + uint64_t(sext(a.immhi << 2 | a.immlo, 21)))});
        } else if (i.op == Op::ADD_imm) {
            const F<Op::ADD_imm> a(i.w);
            if (!a.sf || a.sh) continue;
            for (uint32_t back = 4; back <= 24 && back <= off; back += 4) {   // the adrp within six words, its register untouched since
                const Insn p = c.insn(off - back);
                if (p.op == Op::ADRP && (p.w & 31) == a.Rn) {
                    const F<Op::ADRP> pf(p.w);
                    const uint64_t t = ((uint64_t(off - back) & ~0xfffull) + uint64_t(sext(pf.immhi << 2 | pf.immlo, 21) * 4096)) + a.imm12;
                    if (t < c.text) c.adr_refs.push_back({off, uint32_t(t)});
                    break;
                }
                const Fx fx = effects(p);
                if ((fx.def & (1u << a.Rn)) || fx.cls == Cls::branch || fx.cls == Cls::ret || fx.cls == Cls::indirect) break;
            }
        }
    }
    for (auto [site, t] : c.adr_refs) {
        if (t >= c.text || t % 4) continue;
        const int fs = c.fde_at(site), ft = c.fde_at(t);
        if (fs >= 0 && fs == ft) continue;                  // a label in the same function (jump-table anchors)
        if (ft >= 0 && c.fde_ranges[size_t(ft)].first != t) { st["adr_into_other_function"]++; continue; }
        if (ft < 0 && fs < 0) continue;                     // both outside the FDEs: decided once that code is partitioned
        seed(c, t, src_adr);
    }
    (void)x;
}

void add_cross_module_seeds(Ctx& x) {
    const Program& p = *x.p;
    for (auto& c : x.mods) {
        const Module& m = *c.m;
        for (const auto* t : {&m.rela, &m.jmprel})
            for (const Reloc& r : *t) {
                if (!r.sym) continue;
                const Resolution& b = p.resolution[size_t(c.index)][r.sym];
                if (b.kind != Bound::self && b.kind != Bound::other) continue;
                const uint64_t a = b.address + uint64_t(r.addend);
                for (auto& d : x.mods)
                    if (a >= d.base && a - d.base < d.text) seed(d, a - d.base, src_reloc);
            }
    }
}

void finish_discovery(Ctx& x, ModCtx& c) {
    auto& st = c.stat;
    // FDE functions
    for (size_t k = 0; k < c.eh.fdes.size(); k++) {
        const Fde& f = c.eh.fdes[k];
        Fn fn;
        fn.start = uint32_t(f.start), fn.end = uint32_t(f.start + f.size), fn.fde = int(k), fn.lsda = uint32_t(f.lsda);
        fn.sources = c.seeds[fn.start];
        c.fns.push_back(fn);
    }
    // Code outside the FDEs, gap by gap
    std::vector<std::pair<uint32_t, uint32_t>> gaps;
    uint32_t at = 0;
    for (auto [s, e] : c.fde_ranges) {
        if (s > at) gaps.push_back({at, s});
        at = std::max(at, e);
    }
    if (at < c.text) gaps.push_back({at, c.text});
    size_t orphan_words = 0, gap_code_words = 0;
    for (auto [a, b] : gaps) {
        std::vector<std::pair<uint32_t, bool>> starts;   // (offset, strong)
        bool after_zero = true;
        for (uint32_t o = a; o < b; o += 4) {
            if (is_header_word(o)) { after_zero = true; continue; }
            const Insn i = c.insn(o);
            if (i.w == 0) { after_zero = true; continue; }
            const bool prev_term = o == a || (o >= 4 && (is_terminator(c.insn(o - 4)) || c.insn(o - 4).w == 0));
            auto s = c.seeds.find(o);
            if (s != c.seeds.end()) starts.push_back({o, true});
            else if (after_zero || (prev_term && looks_like_prologue(i))) starts.push_back({o, false});
            after_zero = false;
            if (!is_padding(i)) gap_code_words++;
        }
        // A weak start that the code before it branches to is a label of that code (shrink-wrapped prologues), not a function
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t k = 1; k < starts.size(); k++) {
                if (starts[k].second) continue;
                const uint32_t lo = starts[k - 1].first, t = starts[k].first;
                for (uint32_t o = lo; o < t; o += 4) {
                    const FlowInfo fl = flow(c.insn(o), o);
                    if ((fl.kind == Flow::b || fl.kind == Flow::bcond) && fl.target >= t &&
                        fl.target < (k + 1 < starts.size() ? starts[k + 1].first : b)) {
                        starts.erase(starts.begin() + ptrdiff_t(k));
                        changed = true;
                        break;
                    }
                }
                if (changed) break;
            }
        }
        // A b from one function of this code into the middle of another, right after a terminator there, is a tail call to a
        // function that nothing else references
        std::vector<uint32_t> tail_starts;
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t k = 0; k < starts.size() && !changed; k++) {
                const uint32_t lo = starts[k].first, hi = k + 1 < starts.size() ? starts[k + 1].first : b;
                for (uint32_t o = lo; o < hi; o += 4) {
                    const FlowInfo fl = flow(c.insn(o), o);
                    if (fl.kind != Flow::b || (fl.target >= lo && fl.target < hi) || fl.target < a || fl.target >= b || fl.target % 4) continue;
                    const uint32_t t = uint32_t(fl.target);
                    auto it = std::lower_bound(starts.begin(), starts.end(), std::pair{t, false});
                    if (it != starts.end() && it->first == t) continue;
                    if (t < 4 || !(is_terminator(c.insn(t - 4)) || c.insn(t - 4).w == 0)) continue;
                    starts.insert(it, {t, true});
                    tail_starts.push_back(t);
                    changed = true;
                    break;
                }
            }
        }
        for (uint32_t t : tail_starts) c.seeds[t] |= src_tail;
        for (size_t k = 0; k < starts.size(); k++) {
            const uint32_t s = starts[k].first;
            uint32_t e = k + 1 < starts.size() ? starts[k + 1].first : b;
            if (s < 4 && e > 4) e = 4;   // the entry word, then the header
            while (e > s && is_padding(c.insn(e - 4))) e -= 4;
            if (e == s) { st["padding_seeds"]++; continue; }
            Fn fn;
            fn.start = s, fn.end = e, fn.flags = fn_no_fde;
            fn.sources = starts[k].second ? c.seeds[s] : uint16_t(src_prologue);
            if (c.plt_sym.count(s) || c.plt0.count(s)) fn.flags |= fn_plt;
            c.fns.push_back(fn);
        }
        // words of code that no function covers
        for (uint32_t o = a; o < b; o += 4)
            if (!is_header_word(o) && !is_padding(c.insn(o)) && (starts.empty() || starts[0].first > o)) orphan_words++;
    }
    std::sort(c.fns.begin(), c.fns.end(), [](const Fn& p, const Fn& q) { return p.start < q.start; });
    st["gap_code_words"] = int64_t(gap_code_words);
    st["orphan_code_words"] = int64_t(orphan_words);

    // Names, and the sources of seeds that turned out to be inside a function
    for (Fn& f : c.fns) {
        auto n = c.names.find(f.start);
        if (n != c.names.end()) f.name = n->second;
        else if (auto p = c.plt_sym.find(f.start); p != c.plt_sym.end()) f.name = c.symbol_name(p->second);
        else if (c.plt0.count(f.start)) f.name = "<plt0>";
    }
    for (auto [o, src] : c.seeds)
        if (c.fn_starting(o) < 0) {
            st["seeds_not_function_start"]++;
            if (c.notes.size() < 40) c.notes.push_back("seed " + hex(o) + " (sources " + hex(src) + ") is not a function start");
        }
    (void)x;
}

}  // namespace nxanalyze
