// Per function: basic blocks and edges, jump tables, indirect branches, calls, LSDA call sites and landing pads, special sites,
// spin loops. Data flow is local: reaching definitions found by walking the CFG backwards from a use.
#include "internal.hpp"
#include "lsda.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace nxanalyze {

using namespace nxload;

namespace {

constexpr uint32_t bit(uint32_t r) { return r >= 31 ? 0 : 1u << r; }   // a general register, never SP or zr

enum Origin : uint32_t { org_sp = 0, org_load = 1, org_arg = 2, org_call = 3, org_other = 4 };

struct Defs {
    uint32_t idx[16] = {};
    uint8_t n = 0;
    bool entry = false, limit = false, orphan = false, more = false;
    // one definition on every path from the function entry; paths from blocks nothing is known to reach (only an unresolved
    // indirect branch could) are ignored
    int unique() const { return n == 1 && !entry && !limit && !more ? int(idx[0]) : -1; }
    int first() const { return n ? int(idx[0]) : -1; }
};

struct Jt {
    uint32_t table = 0, anchor = 0;
    uint8_t kind = 0, bound = 0, shift = 0;
    bool local = true;                 // all targets inside this function
    std::vector<uint64_t> targets;     // module offsets when local, absolute otherwise
};

constexpr uint64_t never = ~0ull;   // a table entry the program never takes (null pointer, unreachable label)

bool is_setjmp(std::string_view n) { return n == "setjmp" || n == "_setjmp" || n == "__setjmp" || n == "sigsetjmp" || n == "__sigsetjmp"; }
bool is_longjmp(std::string_view n) { return n == "longjmp" || n == "_longjmp" || n == "siglongjmp" || n == "__longjmp_chk"; }

class FnAnalyzer {
public:
    FnAnalyzer(Ctx& x, ModCtx& c, uint8_t mod) : x(x), c(c), db(x.out->db), mod(mod) {}
    void run(uint32_t local);

private:
    struct B {
        uint32_t s = 0, e = 0;
        uint8_t term = term_fall, flags = 0;
        bool pad = false;   // only nops
        uint32_t sfirst = 0, scount = 0;
    };
    Ctx& x;
    ModCtx& c;
    Database& db;
    uint8_t mod;
    const Fn* fn = nullptr;
    uint32_t gfi = 0, n = 0;
    std::vector<Insn> ins;
    std::vector<Fx> fx;
    std::vector<uint8_t> leader, noret, resume;
    std::vector<uint32_t> block_of;
    std::vector<B> blocks;
    std::vector<std::pair<uint32_t, uint8_t>> succ;
    std::vector<uint32_t> pfirst, pcount, preds;
    std::vector<uint32_t> stamp, wstamp;   // visited marks: find_defs, and the bound search (which calls find_defs)
    uint32_t epoch = 0, wepoch = 0;
    int lr_def_any = -1;   // whether any instruction of the function writes x30 (or is a call): a function with none keeps the caller's x30 everywhere, however big it is
    std::map<uint32_t, Jt> jts;
    std::vector<uint32_t> lps;
    Lsda lsda;
    uint32_t flags = 0;
    std::vector<SiteRec> fsites;
public:
    std::vector<uint32_t>* bases_out = nullptr;      // first pass: collect table starts
    const std::vector<uint32_t>* bases = nullptr;    // second pass: every table start in the module, sorted
private:

    uint64_t pc(uint32_t i) const { return fn->start + 4ull * i; }
    bool intra(uint64_t t) const { return t >= fn->start && t < fn->end && !(t & 3); }
    uint32_t idx(uint64_t t) const { return uint32_t((t - fn->start) / 4); }
    auto& stat(const char* k) { return c.stat[k]; }
    void note(const std::string& s) {
        if (c.notes.size() < 400) c.notes.push_back(s);
    }
    std::string where(uint32_t i) const { return hex(pc(i)) + " in " + (fn->name.empty() ? hex(fn->start) : fn->name); }
    void site(uint32_t i, uint8_t kind, uint32_t detail) { fsites.push_back({uint32_t(pc(i)), gfi, detail, mod, kind, 0}); }

    const Fn* callee(uint64_t t) const {
        const int j = c.fn_starting(uint32_t(t));
        return j >= 0 ? &c.fns[size_t(j)] : nullptr;
    }
    std::string callee_name(uint64_t t) const {
        const Fn* f = callee(t);
        return f ? f->name : std::string();
    }

    void build();
    Defs find_defs(uint32_t at, uint32_t reg);
    std::optional<uint64_t> const_value(uint32_t at, uint32_t r, int depth = 0);
    std::optional<uint64_t> const_fallback(uint32_t at, uint32_t r);
    std::optional<uint64_t> def_value(uint32_t k, int depth);
    bool resolve_jt(uint32_t k, Jt& jt, std::string& why);
    struct BoundState {
        uint32_t r = 31;                                   // the register holding the index (31: known only as an expression)
        uint32_t expr_bound = 0, expr_src = 31;            // after a mask: its bound and source register
        uint64_t expr_key = 0;
        std::pair<int, uint32_t> expr_root{-2, 31};       // the copy root of the mask's source
        uint32_t pending_reg[4] = {}, pending_bound[4] = {};
        int npending = 0;
    };
    struct BoundOut {
        uint32_t best = 0;
        uint8_t src = jb_heuristic;
        bool fail = false;
        int steps = 0;
    };
    uint32_t find_bound(uint32_t at, uint32_t r, uint8_t& src);
    int flag_setter(uint32_t b, uint32_t last, uint32_t& between);
    std::pair<int, uint32_t> copy_root(uint32_t at, uint32_t r, int depth = 0);
    void bound_walk(uint32_t b, uint32_t pos, BoundState s, int depth, BoundOut& o);
    Origin origin(uint32_t at, uint32_t reg, int depth = 0);
    Origin return_address(uint32_t at, uint32_t& writer, int depth = 0);
    bool saved_sp_slot(uint32_t k, int depth);
    void special();
    void indirect();
    void calls();
    void spins();
    bool spin_region(uint32_t T, const std::vector<uint32_t>& backs, SpinRec& s);
    void emit();
};

void FnAnalyzer::build() {
    blocks.clear();
    block_of.assign(n, 0);
    for (uint32_t i = 0; i < n; i++) {
        if (i == 0 || leader[i]) {
            if (!blocks.empty()) blocks.back().e = i;
            blocks.push_back({i, n});
        }
        block_of[i] = uint32_t(blocks.size() - 1);
    }
    succ.clear();
    for (B& b : blocks) {
        const uint32_t k = b.e - 1;
        const FlowInfo fl = flow(ins[k], pc(k));
        b.sfirst = uint32_t(succ.size());
        b.pad = std::all_of(ins.begin() + b.s, ins.begin() + b.e, [](Insn i) { return i.w == 0xd503201f; });
        auto add = [&](uint32_t i, uint8_t kind) { succ.push_back({block_of[i], kind}); };
        switch (fl.kind) {
        case Flow::b:
            if (intra(fl.target)) b.term = term_branch, add(idx(fl.target), edge_branch);
            else b.term = term_tail;
            break;
        case Flow::bcond:
            b.term = term_cond;
            if (intra(fl.target)) add(idx(fl.target), edge_taken);
            if (k + 1 < n) add(k + 1, edge_not_taken);
            break;
        case Flow::br: {
            auto j = jts.find(k);
            if (j != jts.end() && j->second.local) {
                b.term = term_jump_table;
                std::vector<uint64_t> t = j->second.targets;
                std::erase(t, never);
                std::sort(t.begin(), t.end());
                t.erase(std::unique(t.begin(), t.end()), t.end());
                for (uint64_t a : t) add(idx(a), edge_jump_table);
            } else {
                b.term = term_indirect_tail;
            }
            break;
        }
        case Flow::ret: b.term = term_return; break;
        case Flow::trap: b.term = term_trap; break;
        case Flow::eret: b.term = term_eret; break;
        default:
            if (noret[k]) b.term = term_noreturn_call;
            else if (k + 1 < n) b.term = term_fall, add(k + 1, edge_fall);
            else b.term = term_end;
            break;
        }
        b.scount = uint32_t(succ.size()) - b.sfirst;
    }
    const size_t nb = blocks.size();
    pcount.assign(nb, 0);
    pfirst.assign(nb + 1, 0);
    for (auto& s : succ) pcount[s.first]++;
    for (size_t b = 0; b < nb; b++) pfirst[b + 1] = pfirst[b] + pcount[b];
    preds.assign(succ.size(), 0);
    std::vector<uint32_t> fill(pfirst.begin(), pfirst.end() - 1);
    for (size_t b = 0; b < nb; b++)
        for (uint32_t s = blocks[b].sfirst; s < blocks[b].sfirst + blocks[b].scount; s++) preds[fill[succ[s].first]++] = uint32_t(b);
    stamp.assign(nb, 0), wstamp.assign(nb, 0);
    epoch = wepoch = 0;
    lr_def_any = -1;
}

Defs FnAnalyzer::find_defs(uint32_t at, uint32_t reg) {
    Defs d;
    if (!reg) return d;
    if (reg == LR) {   // a huge function (a 27 000-instruction hash lookup in OoT's main) outruns the search budget below; with no x30 write at all the answer is the entry
        if (lr_def_any < 0) {
            lr_def_any = 0;
            for (uint32_t k = 0; k < n && !lr_def_any; k++) lr_def_any = ((fx[k].def | (fx[k].cls == Cls::call ? call_clobbered : 0)) & LR) != 0;
        }
        if (!lr_def_any) { d.entry = true; return d; }
    }
    ++epoch;
    std::vector<std::pair<uint32_t, uint32_t>> work{{block_of[at], at}};
    int budget = 20000;
    while (!work.empty()) {
        auto [b, pos] = work.back();
        work.pop_back();
        bool found = false;
        for (uint32_t k = pos; k-- > blocks[b].s;) {
            if (--budget < 0) {
                d.limit = true;
                return d;
            }
            const uint32_t def = fx[k].def | (fx[k].cls == Cls::call ? call_clobbered : 0);
            if (def & reg) {
                if (std::find(d.idx, d.idx + d.n, k) == d.idx + d.n) {
                    if (d.n < 16) d.idx[d.n++] = k;
                    else d.more = true;
                }
                found = true;
                break;
            }
        }
        if (found) continue;
        if (b == 0) d.entry = true;
        else if (!pcount[b] && !blocks[b].pad) d.orphan = true;   // alignment nops after a branch are no orphan
        for (uint32_t q = pfirst[b]; q < pfirst[b] + pcount[b]; q++) {
            const uint32_t p = preds[q];
            if (stamp[p] == epoch) continue;
            stamp[p] = epoch;
            work.push_back({p, blocks[p].e});
        }
    }
    return d;
}

// The constant an instruction writes: adr, adrp, adrp+add, register copies, and a pointer loaded from a relocated slot
std::optional<uint64_t> FnAnalyzer::def_value(uint32_t k, int depth) {
    const Insn I = ins[k];
    switch (I.op) {
    case Op::ADR: {
        const F<Op::ADR> f(I.w);
        return pc(k) + uint64_t(sext(f.immhi << 2 | f.immlo, 21));
    }
    case Op::ADRP: {
        const F<Op::ADRP> f(I.w);
        return (pc(k) & ~0xfffull) + uint64_t(sext(f.immhi << 2 | f.immlo, 21) * 4096);
    }
    case Op::ADD_imm: case Op::SUB_imm: {
        const F<Op::ADD_imm> f(I.w);
        if (!f.sf || f.Rn == 31) return std::nullopt;
        auto v = const_value(k, f.Rn, depth + 1);
        const uint64_t imm = uint64_t(f.imm12) << (f.sh ? 12 : 0);
        if (v) v = I.op == Op::ADD_imm ? *v + imm : *v - imm;
        return v;
    }
    case Op::ORR_reg: {
        const F<Op::ORR_reg> f(I.w);
        if (f.sf && f.Rn == 31 && !f.imm6) return const_value(k, f.Rm, depth + 1);
        return std::nullopt;
    }
    case Op::LDR_imm: {   // a pointer in a relocated slot of this module (adrp; ldr xN, [xN, #slot])
        const F<Op::LDR_imm> f(I.w);
        if (!f.sf || f.Rn == 31) return std::nullopt;
        const auto slot = const_value(k, f.Rn, depth + 1);
        if (!slot) return std::nullopt;
        const uint64_t at = *slot + f.imm12 * 8ull;
        auto r = c.rel_at.find(uint32_t(at));
        if (r == c.rel_at.end() || (r->second.type != 0 && r->second.type != R_AARCH64_RELATIVE)) return std::nullopt;
        const uint64_t a = c.read64_rel(uint32_t(at));
        if (a >= c.base && a - c.base < c.m->image_size()) return a - c.base;
        return std::nullopt;
    }
    default: return std::nullopt;
    }
}

// A constant address in r before `at`; several definitions are fine when they agree
std::optional<uint64_t> FnAnalyzer::const_value(uint32_t at, uint32_t r, int depth) {
    if (depth > 8 || r >= 31) return std::nullopt;
    const Defs d = find_defs(at, bit(r));
    if (!d.n || d.entry || d.limit || d.more) return std::nullopt;
    std::optional<uint64_t> out;
    for (uint8_t q = 0; q < d.n; q++) {
        const auto v = def_value(d.idx[q], depth);
        if (!v || (out && *out != *v)) return std::nullopt;
        out = v;
    }
    return out;
}

// When the reaching definitions disagree or cannot be followed (code reached only through other unresolved branches): the one
// constant among the definitions that have one, else the last write in address order. Only used where the result is checked
// (jump-table entries must land in the function).
std::optional<uint64_t> FnAnalyzer::const_fallback(uint32_t at, uint32_t r) {
    if (r >= 31) return std::nullopt;
    const Defs d = find_defs(at, bit(r));
    std::optional<uint64_t> one;
    for (uint8_t q = 0; q < d.n; q++) {
        const auto v = def_value(d.idx[q], 0);
        if (!v) continue;
        if (one && *one != *v) return std::nullopt;
        one = v;
    }
    if (one) return one;
    for (uint32_t k = at; k-- > 0 && at - k < 256;) {
        if (!(fx[k].def & bit(r))) continue;
        const Insn I = ins[k];
        if (I.op == Op::ADR || I.op == Op::ADRP) return def_value(k, 0);
        if (I.op == Op::ADD_imm && F<Op::ADD_imm>(I.w).sf) {
            const F<Op::ADD_imm> f(I.w);
            for (uint32_t j = k; j-- > 0 && k - j < 16;)
                if (fx[j].def & bit(f.Rn)) {
                    if (ins[j].op != Op::ADRP) return std::nullopt;
                    const F<Op::ADRP> p(ins[j].w);
                    return (pc(j) & ~0xfffull) + uint64_t(sext(p.immhi << 2 | p.immlo, 21) * 4096) + (uint64_t(f.imm12) << (f.sh ? 12 : 0));
                }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// The compare that sets the flags a b.cond at `last` tests, looking back through single-predecessor blocks; `between` collects
// the registers written after it
int FnAnalyzer::flag_setter(uint32_t b, uint32_t last, uint32_t& between) {
    between = 0;
    for (int hops = 0; hops < 4; hops++) {
        for (uint32_t k = last; k-- > blocks[b].s;) {
            if (fx[k].cls == Cls::call) return -1;
            if (fx[k].fdef) return int(k);
            between |= fx[k].def;
        }
        if (pcount[b] != 1) return -1;
        b = preds[pfirst[b]];
        last = blocks[b].e;
    }
    return -1;
}

// The value a register holds before `at`, through register copies: (the instruction that made it, its register), with -1 for a
// value that comes in from the function's caller; -2 when it is not one definition
std::pair<int, uint32_t> FnAnalyzer::copy_root(uint32_t at, uint32_t r, int depth) {
    if (r >= 31 || depth > 8) return {-2, r};
    const Defs d = find_defs(at, bit(r));
    if (!d.n && d.entry && !d.limit) return {-1, r};
    const int k = d.unique();
    if (k < 0) return {-2, r};
    const Insn I = ins[size_t(k)];
    if (I.op == Op::ORR_reg) {
        const F<Op::ORR_reg> f(I.w);
        if (f.Rn == 31 && !f.imm6 && f.Rm != 31) return copy_root(uint32_t(k), f.Rm, depth + 1);
    }
    return {k, r};
}

// The bound of a jump-table index: a dominating cmp #N with b.hi/b.hs (or b.ls/b.lo to the table), a mask or bit-field
// extract, or a byte/halfword load. A compare on another register counts once that register turns out to hold the index
// (a copy made before the compare), the index plus a constant (sub w8, w9, #1; cmp w8, #30), or, after a mask, the same
// masked value (cmp w8, #0x5c after an earlier and w8, w20, #0xff); a mask alone is the fallback.
void FnAnalyzer::bound_walk(uint32_t b, uint32_t pos, BoundState s, int depth, BoundOut& o) {
    auto done = [&](uint32_t v, uint8_t src) {
        if (v > o.best) o.best = v, o.src = src;
    };
    auto expr_of = [&](uint32_t k, uint32_t& srcreg, uint64_t& key) -> uint32_t {   // a mask-like definition: its bound, source and shape
        const Insn I = ins[k];
        if (I.op == Op::AND_imm) {
            const F<Op::AND_imm> f(I.w);
            const uint64_t m = bitmask_imm(f.N, f.immr, f.imms, f.sf ? 64 : 32);
            if (m && m <= 0xffff && !((m + 1) & m)) return srcreg = f.Rn, key = m, uint32_t(m + 1);
        } else if (I.op == Op::UBFM) {
            const F<Op::UBFM> f(I.w);
            if (f.imms >= f.immr && f.imms - f.immr + 1 <= 16) return srcreg = f.Rn, key = (1ull << 32) | f.immr << 8 | f.imms, 1u << (f.imms - f.immr + 1);
        }
        return 0;
    };
    for (uint32_t k = pos; k-- > blocks[b].s;) {
        if (++o.steps > 1200) return s.expr_bound ? done(s.expr_bound, jb_mask) : void(o.fail = true);
        const uint32_t def = fx[k].def | (fx[k].cls == Cls::call ? call_clobbered : 0);
        if (s.expr_bound) {
            if (def & bit(s.expr_src)) {
                const Insn M = ins[k];
                if (M.op != Op::ORR_reg || F<Op::ORR_reg>(M.w).Rn != 31 || F<Op::ORR_reg>(M.w).imm6 || F<Op::ORR_reg>(M.w).Rm == 31)
                    return done(s.expr_bound, jb_mask);
                s.expr_src = F<Op::ORR_reg>(M.w).Rm;   // the mask's source is a copy: follow it
                continue;
            }
            for (int q = 0; q < s.npending; q++)
                if (def & bit(s.pending_reg[q])) {
                    uint32_t sr;
                    uint64_t key;
                    if (expr_of(k, sr, key) && key == s.expr_key && (sr == s.expr_src || (s.expr_root.first != -2 && copy_root(k, sr) == s.expr_root)))
                        return done(s.pending_bound[q], jb_cmp);
                    s.pending_reg[q] = 31;
                }
            continue;
        }
        for (int q = 0; q < s.npending; q++)   // a pending compare on another register: is that register the index plus k?
            if (s.pending_reg[q] != 31 && (def & bit(s.pending_reg[q]))) {
                const Insn P = ins[k];
                if ((P.op == Op::SUB_imm || P.op == Op::ADD_imm) && !(def & bit(s.r))) {
                    const F<Op::SUB_imm> f(P.w);
                    const int64_t imm = int64_t(f.imm12) << (f.sh ? 12 : 0);
                    const int64_t u = int64_t(s.pending_bound[q]) + (P.op == Op::SUB_imm ? imm : -imm);
                    if (f.Rn == s.r && u > 0 && u <= 0x100000) return done(uint32_t(u), jb_cmp);
                }
                s.pending_reg[q] = 31;
            }
        if (!(def & bit(s.r))) continue;
        const Insn I = ins[k];
        if (I.op == Op::ORR_reg) {
            const F<Op::ORR_reg> f(I.w);
            if (f.Rn == 31 && f.imm6 == 0 && f.Rm != 31) {
                s.r = f.Rm;
                for (int q = 0; q < s.npending; q++)
                    if (s.pending_reg[q] == s.r) return done(s.pending_bound[q], jb_cmp);
                continue;
            }
        }
        uint32_t sr;
        uint64_t key;
        if (const uint32_t mb = expr_of(k, sr, key)) {
            s.expr_bound = mb, s.expr_src = sr, s.expr_key = key, s.r = 31, s.expr_root = copy_root(k, sr);
            continue;
        }
        switch (I.op) {
        case Op::LDRB_imm: case Op::LDRB_reg: case Op::LDURB: case Op::LDRB_post: case Op::LDRB_pre: case Op::LDARB: return done(256, jb_load_width);
        case Op::LDRH_imm: case Op::LDRH_reg: case Op::LDURH: case Op::LDRH_post: case Op::LDRH_pre: case Op::LDARH: return done(65536, jb_load_width);
        default: return o.fail = true, void();
        }
    }
    if (depth > 24 || !pcount[b]) {
        if (s.expr_bound) return done(s.expr_bound, jb_mask);
        return o.fail = true, void();
    }
    for (uint32_t q = pfirst[b]; q < pfirst[b] + pcount[b]; q++) {
        const uint32_t p = preds[q];
        if (wstamp[p] == wepoch) continue;
        wstamp[p] = wepoch;
        const B& P = blocks[p];
        const uint32_t last = P.e - 1;
        BoundState t = s;
        bool guarded = false;
        if (ins[last].op == Op::B_cond) {
            const F<Op::B_cond> bc(ins[last].w);
            const bool taken = flow(ins[last], pc(last)).target == pc(blocks[b].s), fall = P.e == blocks[b].s;
            uint32_t between = 0;
            const int k = taken != fall ? flag_setter(p, last, between) : -1;
            if (k >= 0 && ins[size_t(k)].op == Op::SUBS_imm) {
                const F<Op::SUBS_imm> f(ins[size_t(k)].w);
                const uint64_t N = uint64_t(f.imm12) << (f.sh ? 12 : 0);
                uint64_t u = 0;
                if (!taken && bc.cond == 8) u = N + 1;        // b.hi default
                else if (!taken && bc.cond == 2) u = N;       // b.hs default
                else if (taken && bc.cond == 9) u = N + 1;    // b.ls table
                else if (taken && bc.cond == 3) u = N;        // b.lo table
                if (u && u <= 0x100000 && !(between & bit(f.Rn))) {
                    if (f.Rn == t.r) done(uint32_t(u), jb_cmp), guarded = true;
                    else if (t.npending < 4) t.pending_reg[t.npending] = f.Rn, t.pending_bound[t.npending++] = uint32_t(u);
                }
            }
        }
        if (!guarded) bound_walk(p, P.e, t, depth + 1, o);
    }
}

uint32_t FnAnalyzer::find_bound(uint32_t at, uint32_t r, uint8_t& src) {
    BoundOut o;
    BoundState s;
    s.r = r;
    ++wepoch;
    bound_walk(block_of[at], at, s, 0, o);
    if (o.best && o.fail && !bases_out) stat("jt_bound_partial")++;
    src = o.src;
    return o.fail && !o.best ? 0 : o.best;
}

bool FnAnalyzer::resolve_jt(uint32_t k, Jt& jt, std::string& why) {
    const uint32_t reg = flow(ins[k], pc(k)).reg;
    const int d1 = find_defs(k, bit(reg)).unique();
    if (d1 < 0) return why = "target: no single definition", false;
    const Insn A = ins[size_t(d1)];
    uint32_t anchor_r = 0, ent_r = 0, ext = 3, sh = 0;
    if (A.op == Op::ADD_ext) {
        const F<Op::ADD_ext> f(A.w);
        if (!f.sf) return why = "32-bit add", false;
        anchor_r = f.Rn, ent_r = f.Rm, ext = f.option, sh = f.imm3;
    } else if (A.op == Op::ADD_reg) {
        const F<Op::ADD_reg> f(A.w);
        if (!f.sf || f.shift != 0) return why = "add with a shift other than lsl", false;
        anchor_r = f.Rn, ent_r = f.Rm, sh = f.imm6;
        if (!const_value(uint32_t(d1), anchor_r) && f.imm6 == 0) std::swap(anchor_r, ent_r);
    } else if (A.op == Op::LDR_reg) {   // absolute: ldr xT, [xTab, xI, lsl #3]; br xT
        const F<Op::LDR_reg> f(A.w);
        if (!f.sf || !f.S || (f.option != 3 && f.option != 2 && f.option != 6)) return why = "unusual absolute table load", false;
        auto tab = const_value(uint32_t(d1), f.Rn);
        if (!tab) return why = "absolute table: base not constant", false;
        uint8_t src = jb_cmp;
        uint32_t count = find_bound(uint32_t(d1), f.Rm, src);
        const bool bounded = count && count <= 4096;
        if (!bounded) count = 1024, src = jb_heuristic;   // read while every entry is a relocated function start or null
        jt.table = uint32_t(*tab), jt.anchor = 0, jt.kind = jt_abs64, jt.bound = src, jt.shift = 3;
        for (uint32_t i = 0; i < count; i++) {
            const uint64_t slot = *tab + 8ull * i;
            if (slot + 8 > c.rel.size()) break;
            const uint64_t a = c.read64_rel(uint32_t(slot));
            if (!c.rel_at.count(uint32_t(slot))) {
                if (a && bounded) return why = "absolute table: unrelocated entry", false;
                if (a || !bounded) break;
                jt.targets.push_back(never);   // a null entry (the code tests for it)
                continue;
            }
            if (!bounded && !x.fn_at_abs(a).first && !(a >= c.base && intra(a - c.base))) break;
            jt.targets.push_back(a);
        }
        while (!jt.targets.empty() && jt.targets.back() == never) jt.targets.pop_back();
        if (jt.targets.empty()) return why = "absolute table: no entry", false;
        jt.local = std::all_of(jt.targets.begin(), jt.targets.end(), [&](uint64_t a) { return a == never || (a >= c.base && intra(a - c.base)); });
        if (jt.local)
            for (uint64_t& a : jt.targets)
                if (a != never) a -= c.base;
        return true;
    } else {
        return why = std::string("target defined by ") + nxw::a64::op_name(A.op), false;
    }
    bool inferred = false;
    auto anchor = const_value(uint32_t(d1), anchor_r);
    if (!anchor && (anchor = const_fallback(uint32_t(d1), anchor_r))) inferred = true;
    if (!anchor) return why = "anchor not constant", false;
    const int ld = find_defs(uint32_t(d1), bit(ent_r)).unique();
    if (ld < 0) return why = "entry: no single definition", false;
    const Insn L = ins[size_t(ld)];
    uint32_t base = 0, index = 0, size = 0, scaled = 0;
    bool sign = false, sign64 = false;
    switch (L.op) {
    case Op::LDRB_reg: { const F<Op::LDRB_reg> f(L.w); base = f.Rn, index = f.Rm, size = 1, scaled = f.S; break; }
    case Op::LDRSB_reg: { const F<Op::LDRSB_reg> f(L.w); base = f.Rn, index = f.Rm, size = 1, scaled = f.S, sign = true, sign64 = !f.opc0; break; }
    case Op::LDRH_reg: { const F<Op::LDRH_reg> f(L.w); base = f.Rn, index = f.Rm, size = 2, scaled = f.S; break; }
    case Op::LDRSH_reg: { const F<Op::LDRSH_reg> f(L.w); base = f.Rn, index = f.Rm, size = 2, scaled = f.S, sign = true, sign64 = !f.opc0; break; }
    case Op::LDR_reg: { const F<Op::LDR_reg> f(L.w); if (f.sf) return why = "64-bit entries", false; base = f.Rn, index = f.Rm, size = 4, scaled = f.S; break; }
    case Op::LDRSW_reg: { const F<Op::LDRSW_reg> f(L.w); base = f.Rn, index = f.Rm, size = 4, scaled = f.S, sign = sign64 = true; break; }
    default: return why = std::string("entry loaded by ") + nxw::a64::op_name(L.op), false;
    }
    if (size > 1 && !scaled) return why = "unscaled index", false;
    auto table = const_value(uint32_t(ld), base);
    if (!table && (table = const_fallback(uint32_t(ld), base))) inferred = true;
    if (!table) return why = "table base not constant", false;
    uint8_t src = jb_heuristic;
    uint32_t count = find_bound(uint32_t(ld), index, src);
    // A bound that only comes from the index's width (a byte or halfword load, a mask) can exceed the table: such a table, like one
    // without a bound, is read up to the next table and while its entries land in the function
    const bool bounded = count != 0 && src == jb_cmp;
    if (!bounded) {
        count = count ? count : 4096;
        if (bases) {
            auto nb = std::upper_bound(bases->begin(), bases->end(), uint32_t(*table));
            if (nb != bases->end()) count = std::min<uint32_t>(count, (*nb - uint32_t(*table)) / size);
        }
    }
    jt.table = uint32_t(*table), jt.anchor = uint32_t(*anchor), jt.bound = src, jt.shift = uint8_t(sh);
    const bool ext_signed = ext >= 4 && ext != 7;
    const unsigned ext_bits = (ext & 3) == 0 ? 8 : (ext & 3) == 1 ? 16 : (ext & 3) == 2 ? 32 : 64;
    const bool s = sign || (ext_signed && ext_bits <= size * 8);
    jt.kind = uint8_t(size == 1 ? (s ? jt_rel_s8 : jt_rel_u8) : size == 2 ? (s ? jt_rel_s16 : jt_rel_u16) : (s ? jt_rel_s32 : jt_rel_u32));
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t at = *table + uint64_t(i) * size;
        if (at + size > c.img.size()) break;
        uint64_t v = 0;
        std::memcpy(&v, c.img.data() + at, size);
        if (sign) {
            v = uint64_t(sext(v, size * 8));
            if (!sign64) v &= 0xffffffffull;
        }
        if (A.op == Op::ADD_ext) {
            if (ext_bits < 64) v &= (1ull << ext_bits) - 1;
            if (ext_signed && ext_bits < 64) v = uint64_t(sext(v, ext_bits));
        }
        const uint64_t t = *anchor + (v << sh);
        if (src != jb_heuristic && t == fn->end) {   // LLVM's unreachable default: an empty label just past the function's end
            jt.targets.push_back(never);
            continue;
        }
        if (!intra(t)) {
            if (bounded) return why = "entry " + std::to_string(i) + " lands outside the function (" + hex(t) + ")", false;
            if (src != jb_heuristic && i > 0) src = jb_heuristic;   // the width's bound was not the table's
            break;
        }
        jt.targets.push_back(t);
    }
    while (!jt.targets.empty() && jt.targets.back() == never) jt.targets.pop_back();
    if (jt.targets.empty()) return why = "no entry inside the function", false;
    jt.bound = src;
    if (inferred && !bases_out) stat("jt_base_inferred")++;
    return true;
}

// Where a register's value comes from: the stack (SP or the frame pointer, through any arithmetic, or a frame slot that a saved
// SP was stored to), a load, an argument, a call. A value computed from SP and anything else (sp - size) counts as the stack, and
// so does a register any of whose reaching definitions is the stack (the save before a variable-length array, restored after it).
Origin FnAnalyzer::origin(uint32_t at, uint32_t reg, int depth) {
    if (reg == 31 || reg == 29) return org_sp;
    const Defs d = find_defs(at, bit(reg));
    if (d.entry && !d.n) return org_arg;
    if (!d.n) return org_other;
    Origin first = org_other;
    for (uint8_t q = 0; q < d.n; q++) {
        const uint32_t j = d.idx[q];
        const Fx& f = fx[j];
        Origin o = org_other;
        if (f.cls == Cls::call) o = org_call;
        else if (f.cls == Cls::load || f.cls == Cls::load_acq || f.cls == Cls::load_excl) o = depth < 6 && saved_sp_slot(j, depth) ? org_sp : org_load;
        else if (f.use & (SP | FP)) o = org_sp;
        else if (depth < 6)
            for (uint32_t r = 0; r < 31; r++)
                if (f.use & (1u << r)) {
                    const Origin u = origin(j, r, depth + 1);
                    if (u == org_sp) { o = org_sp; break; }
                    if (o == org_other) o = u;
                }
        if (o == org_sp) return org_sp;
        if (q == 0) first = o;
    }
    return d.entry ? org_arg : first;
}

// A 64-bit load from [sp or x29, #off] that some store in the function fills with a stack-derived value
bool FnAnalyzer::saved_sp_slot(uint32_t k, int depth) {
    auto slot = [&](uint32_t j, bool& store, uint32_t& base, int64_t& off, uint32_t& reg) {
        const Insn I = ins[j];
        switch (I.op) {
        case Op::LDR_imm: case Op::STR_imm: {
            const F<Op::LDR_imm> f(I.w);
            if (!f.sf) return false;
            store = I.op == Op::STR_imm, base = f.Rn, off = int64_t(f.imm12) * 8, reg = f.Rt;
            return true;
        }
        case Op::LDUR: case Op::STUR: {
            const F<Op::LDUR> f(I.w);
            if (!f.sf) return false;
            store = I.op == Op::STUR, base = f.Rn, off = sext(f.imm9, 9), reg = f.Rt;
            return true;
        }
        default: return false;
        }
    };
    bool st;
    uint32_t base, reg;
    int64_t off;
    if (!slot(k, st, base, off, reg) || st || (base != 31 && base != 29)) return false;
    for (uint32_t j = 0; j < n; j++) {
        bool s2;
        uint32_t b2, r2;
        int64_t o2;
        if (j != k && slot(j, s2, b2, o2, r2) && s2 && b2 == base && o2 == off && r2 != 31 && origin(j, r2, depth + 1) == org_sp) return true;
    }
    return false;
}

// The x30 a return or tail branch hands on: org_sp when it is the caller's (never written, or restored from the frame),
// otherwise where it came from, and the instruction that wrote it
Origin FnAnalyzer::return_address(uint32_t at, uint32_t& writer, int depth) {
    const Defs d = find_defs(at, LR);
    for (uint8_t q = 0; q < d.n; q++) {
        const uint32_t j = d.idx[q];
        const Fx& f = fx[j];
        const Insn I = ins[j];
        if (I.op == Op::HINT) {   // pacibsp, autibsp, xpaclri keep the value
            if (depth < 8) {
                const Origin o = return_address(j, writer, depth + 1);
                if (o != org_sp) return o;
            }
            continue;
        }
        writer = j;
        if (f.cls == Cls::call) return org_call;
        if (f.cls == Cls::load || f.cls == Cls::load_acq) {
            if (f.use & (SP | FP)) continue;
            return org_load;
        }
        return org_other;
    }
    return d.limit ? org_other : org_sp;
}

// SP and x30 rewriters, context branches, system registers, SVCs, traps, hints, exclusives
void FnAnalyzer::special() {
    for (uint32_t k = 0; k < n; k++) {
        const Insn I = ins[k];
        const Fx& f = fx[k];
        switch (I.op) {
        case Op::SVC: site(k, site_svc, F<Op::SVC>(I.w).imm16), flags |= fn_svc; break;
        case Op::MRS: site(k, site_mrs, sysreg_no(I.w)), flags |= fn_sysreg; break;
        case Op::MSR_reg: site(k, site_msr, sysreg_no(I.w)), flags |= fn_sysreg; break;
        case Op::MSR_imm: site(k, site_msr_imm, (I.w >> 5) & 0x7fff), flags |= fn_sysreg; break;
        case Op::SYS: case Op::SYSL: site(k, site_sys, (I.w >> 5) & 0x3fff), flags |= fn_sysreg; break;
        case Op::UDF: site(k, site_udf, F<Op::UDF>(I.w).imm16), flags |= fn_trap; break;
        case Op::BRK: site(k, site_brk, F<Op::BRK>(I.w).imm16), flags |= fn_trap; break;
        case Op::HLT: site(k, site_hlt, F<Op::HLT>(I.w).imm16), flags |= fn_trap; break;
        case Op::Unallocated: site(k, site_udf, I.w), flags |= fn_trap; break;
        case Op::CLREX: site(k, site_clrex, 0), flags |= fn_exclusive; break;
        case Op::PAC: case Op::AUT: case Op::XPACI: site(k, site_pac_data, 0); break;
        case Op::HINT:
            switch (hint_no(I.w)) {
            case hint_wfe: site(k, site_wfe, 0); break;
            case hint_wfi: site(k, site_wfi, 0); break;
            case hint_yield: site(k, site_yield, 0); break;
            case hint_sev: site(k, site_sev, 0); break;
            case hint_sevl: site(k, site_sevl, 0); break;
            case hint_xpaclri: case hint_pacia1716: case hint_pacib1716: case hint_autia1716: case hint_autib1716: site(k, site_pac_data, hint_no(I.w)); break;
            default: break;
            }
            break;
        default: break;
        }
        if (f.cls == Cls::load_excl) site(k, site_excl_load, 0), flags |= fn_exclusive;
        if (f.cls == Cls::store_excl) site(k, site_excl_store, 0), flags |= fn_exclusive;

        if (f.def & SP) {
            bool normal = false, dynamic = false;
            switch (I.op) {
            case Op::ADD_imm: case Op::SUB_imm: { const uint32_t rn = F<Op::ADD_imm>(I.w).Rn; normal = rn == 31 || rn == 29; break; }
            case Op::ADD_ext: case Op::SUB_ext: dynamic = F<Op::ADD_ext>(I.w).Rn == 31; break;
            default:   // pre/post-index writeback on SP
                normal = (f.cls == Cls::load || f.cls == Cls::store || f.cls == Cls::fp_load || f.cls == Cls::fp_store) && (f.use & SP);
                break;
            }
            if (dynamic) site(k, site_sp_dynamic, 0), flags |= fn_sp_dynamic;
            else if (!normal) {
                Origin o = org_other;
                for (uint32_t r = 0; r < 31; r++)
                    if (f.use & (1u << r)) {
                        const Origin u = origin(k, r);
                        if (u == org_sp) { o = org_sp; break; }
                        if (o == org_other) o = u;
                    }
                if (o == org_sp) site(k, site_sp_dynamic, 0), flags |= fn_sp_dynamic;
                else site(k, site_sp_switch, o), flags |= fn_sp_switch;
            }
        }
        // x30 as a return address: at every return and tail branch it must be the caller's (x30 as a scratch register between a
        // prologue that saved it and an epilogue that restores it is ordinary register allocation)
        const FlowInfo fl = flow(I, pc(k));
        const bool tail = fl.kind == Flow::b && !intra(fl.target);   // indirect branches are judged in indirect()
        if ((fl.kind == Flow::ret && fl.reg == 30) || tail || (fl.kind == Flow::br && fl.reg == 30 && !jts.count(k))) {
            uint32_t w = k;
            const Origin o = return_address(k, w);
            if (o != org_sp && std::none_of(fsites.begin(), fsites.end(), [&](const SiteRec& s) { return s.kind == site_lr_write && s.site == pc(w); }))
                site(w, site_lr_write, o), flags |= fn_lr_write;
        }
    }
}

void FnAnalyzer::indirect() {
    const bool plt = fn->flags & fn_plt;
    for (uint32_t k = 0; k < n; k++) {
        const FlowInfo fl = flow(ins[k], pc(k));
        if (fl.kind != Flow::br && fl.kind != Flow::blr && fl.kind != Flow::ret) continue;
        IndirectRec r{};
        r.site = uint32_t(pc(k)), r.function = gfi, r.module = mod, r.aux = none32;
        r.insn = fl.kind == Flow::br ? ind_br : fl.kind == Flow::blr ? ind_blr : ind_ret;
        if (fl.kind == Flow::ret) {
            if (fl.reg == 30) {   // a normal return, unless x30 came from a context in memory
                const Defs d = find_defs(k, LR);
                const int j = d.first();
                if (j >= 0 && (fx[size_t(j)].cls == Cls::load) && !(fx[size_t(j)].use & (SP | FP))) {
                    site(k, site_context_branch, ind_ret), flags |= fn_context_branch;
                    r.cls = ic_context;
                    db.indirect.push_back(r);
                }
                continue;
            }
            r.cls = ic_return_reg;
            site(k, site_ret_reg, fl.reg), flags |= fn_ret_other;
            db.indirect.push_back(r);
            continue;
        }
        if (plt && fl.kind == Flow::br) {
            r.cls = ic_plt;
            db.indirect.push_back(r);
            continue;
        }
        if (auto j = jts.find(k); j != jts.end()) {
            const Jt& t = j->second;
            r.aux = uint32_t(db.jump_tables.size());
            r.cls = t.kind == jt_abs64 ? (t.local ? ic_table_local : ic_table_tail) : t.bound == jb_heuristic ? ic_jump_table_unbounded : ic_jump_table;
            JumpTableRec jr{};
            jr.site = r.site, jr.function = gfi, jr.table = t.table, jr.anchor = t.anchor, jr.module = mod, jr.kind = t.kind,
            jr.bound = t.bound, jr.shift = t.shift;
            jr.first_target = uint32_t(db.jt_targets.size()), jr.count = uint32_t(t.targets.size());
            for (uint64_t a : t.targets) {
                if (a == never) db.jt_targets.push_back(none32);
                else if (t.local) db.jt_targets.push_back(uint32_t(a));
                else {
                    auto [m2, f2] = x.fn_at_abs(a);
                    db.jt_targets.push_back(m2 ? m2->first_fn + uint32_t(f2) : none32);   // function indices for tables of entries
                }
            }
            db.jump_tables.push_back(jr);
            if (t.local) flags |= fn_jump_tables;
            else flags |= fn_indirect_tail;
            db.indirect.push_back(r);
            continue;
        }
        uint32_t lr_writer = k;
        if (fl.kind == Flow::br && fl.reg == 30 && return_address(k, lr_writer) == org_sp) {   // br x30 with the caller's x30: a return
            r.cls = ic_return_reg;
            db.indirect.push_back(r);
            continue;
        }
        // Follow register copies to the instruction that produced the target
        uint32_t at = k, reg = fl.reg;
        int j = -1;
        Defs d;
        for (int hop = 0; hop < 4; hop++) {
            d = find_defs(at, bit(reg));
            j = d.first();
            if (d.n > 1 || d.more) r.flags |= 1;
            if (j < 0) break;
            const Insn I = ins[size_t(j)];
            if (I.op == Op::HINT && reg == 30) { at = uint32_t(j); continue; }   // autibsp and friends keep x30's value
            if (I.op == Op::ORR_reg) {
                const F<Op::ORR_reg> f(I.w);
                if (f.Rn == 31 && f.imm6 == 0 && f.Rm != 31) { at = uint32_t(j), reg = f.Rm; continue; }
            }
            break;
        }
        if (j < 0) r.cls = d.entry ? ic_arg : ic_other;
        else {
            const Insn I = ins[size_t(j)];
            const Fx& f = fx[size_t(j)];
            r.cls = ic_other;
            if (f.cls == Cls::call) r.cls = ic_call_result;
            else if (I.op == Op::ADR || I.op == Op::ADRP || I.op == Op::ADD_imm) {
                if (auto v = const_value(at, reg)) r.cls = ic_const, r.aux = uint32_t(*v);
            } else if (I.op == Op::CSEL) r.cls = ic_csel;
            else if (f.cls == Cls::load_acq) r.cls = ic_acquire;
            else if (I.op == Op::LDP_off || I.op == Op::LDP_post || I.op == Op::LDP_pre || I.op == Op::LDNP)
                r.cls = (f.use & (SP | FP)) ? ic_stack : (fl.kind == Flow::br && (flags & fn_sp_switch)) ? ic_context : ic_ldp;
            else if (I.op == Op::LDR_reg) r.cls = ic_mem_indexed;
            else if (f.cls == Cls::load) {
                const uint32_t base = (I.w >> 5) & 31;
                uint32_t slot_off = 0;
                if (I.op == Op::LDR_imm) slot_off = F<Op::LDR_imm>(I.w).imm12 * 8;
                else if (I.op == Op::LDUR) slot_off = uint32_t(sext(F<Op::LDUR>(I.w).imm9, 9));
                if (base == 31 || base == 29) r.cls = ic_stack;
                else if (fl.kind == Flow::br && (flags & fn_sp_switch)) r.cls = ic_context;
                else if (auto slot = const_value(uint32_t(j), base); slot && I.op == Op::LDR_imm) {
                    r.cls = ic_got;
                    r.aux = uint32_t(*slot + slot_off);
                    auto rel = c.rel_at.find(r.aux);
                    if (rel != c.rel_at.end() && rel->second.sym) {
                        r.symbol = db.add_string(c.symbol_name(rel->second.sym));
                        auto s = x.symrec[size_t(mod)].find(rel->second.sym);
                        if (s != x.symrec[size_t(mod)].end()) {
                            SymbolRec& sr = db.symbols[s->second];
                            (fl.kind == Flow::blr ? sr.blr_sites : sr.b_sites)++;
                            if (sr.binding != bind_guest) r.flags |= 2;
                        }
                    } else if (rel != c.rel_at.end()) {
                        const uint64_t t = nxw::load_le<uint64_t>(c.img, r.aux);
                        auto nm = c.names.find(uint32_t(t));
                        r.symbol = db.add_string(nm != c.names.end() ? nm->second : hex(t));
                    }
                } else {
                    const Defs bd = find_defs(uint32_t(j), bit(base));
                    const int b = bd.first();
                    if (b < 0) r.cls = bd.entry ? ic_mem : ic_other;
                    else {
                        const Insn BI = ins[size_t(b)];
                        const Fx& bf = fx[size_t(b)];
                        if (bf.cls == Cls::load) {
                            const uint32_t base2 = (BI.w >> 5) & 31;
                            if (base2 == 31 || base2 == 29) r.cls = ic_vtable_stack;
                            else if (const_value(uint32_t(b), base2)) r.cls = ic_global_obj;
                            else r.cls = ic_vtable;
                            r.aux = slot_off;
                        } else if (BI.op == Op::LDR_reg) r.cls = ic_mem_indexed;
                        else r.cls = ic_mem;
                    }
                }
            }
        }
        if (fl.kind == Flow::br && r.cls == ic_context) site(k, site_context_branch, ind_br), flags |= fn_context_branch;
        if (fl.kind == Flow::br) {
            uint32_t w = k;
            if (return_address(k, w) != org_sp) r.flags |= 4;   // x30 is not the caller's here: a branch within the function, not a tail call
            else flags |= fn_indirect_tail;
        }
        db.indirect.push_back(r);
    }
}

void FnAnalyzer::calls() {
    for (uint32_t k = 0; k < n; k++) {
        const FlowInfo fl = flow(ins[k], pc(k));
        uint8_t kind;
        if (fl.kind == Flow::bl) kind = call_bl;
        else if (fl.kind == Flow::b && !intra(fl.target)) kind = call_tail;
        else if (fl.kind == Flow::bcond && !intra(fl.target)) kind = call_cond_tail;
        else continue;
        CallRec r{};
        r.site = uint32_t(pc(k)), r.function = gfi, r.target = uint32_t(fl.target), r.module = mod, r.kind = kind;
        r.callee = none32, r.symbol = none32, r.binding = bind_guest;
        const int j = fl.target < c.text ? c.fn_starting(uint32_t(fl.target)) : -1;
        if (j < 0) {
            site(k, site_unresolved_branch, uint32_t(fl.kind)), flags |= fn_unresolved;
            r.binding = bind_none;
            stat("unresolved_direct_branches")++;
            note("unresolved " + std::string(fl.kind == Flow::bl ? "bl" : fl.kind == Flow::b ? "b" : "b.cond") + " at " + where(k) + " to " + hex(fl.target));
        } else {
            const Fn& g = c.fns[size_t(j)];
            r.callee = c.first_fn + uint32_t(j);
            if (auto p = c.plt_sym.find(g.start); p != c.plt_sym.end()) {
                auto s = x.symrec[size_t(mod)].find(p->second);
                if (s != x.symrec[size_t(mod)].end()) {
                    SymbolRec& sr = db.symbols[s->second];
                    r.symbol = s->second, r.binding = sr.binding;
                    (kind == call_bl ? sr.bl_sites : sr.b_sites)++;
                    r.callee = none32;
                    if (sr.binding == bind_guest) {
                        auto [m2, f2] = x.fn_at_abs(x.mods[sr.provider].base + sr.address);
                        if (m2) r.callee = m2->first_fn + uint32_t(f2);
                    }
                }
            }
            if (kind == call_cond_tail) stat("conditional_tail_calls")++;
            const std::string& nm = g.name;
            if (is_setjmp(nm)) site(k, site_setjmp_call, 0), flags |= fn_setjmp_caller;
            if (is_longjmp(nm)) site(k, site_longjmp_call, 0);
        }
        db.calls.push_back(r);
    }
}

// Busy-wait loops: a backward branch whose loop, from its head, runs only loads, register and flag arithmetic, direct branches,
// barriers and hints, and writes nothing that is live at the head, so only another thread can end it (nxnative's criterion).
void FnAnalyzer::spins() {
    std::map<uint32_t, std::vector<uint32_t>> heads;
    for (uint32_t k = 0; k < n; k++) {
        const FlowInfo fl = flow(ins[k], pc(k));
        if ((fl.kind == Flow::b || fl.kind == Flow::bcond) && intra(fl.target) && fl.target <= pc(k) && pc(k) - fl.target <= 512)
            heads[idx(fl.target)].push_back(k);
    }
    for (auto& [T, all_backs] : heads) {
        SpinRec s{};
        if (spin_region(T, all_backs, s)) { db.spin_loops.push_back(s); continue; }
        // a back-edge whose own paths poll, beside others that count or store a cursor
        std::vector<uint32_t> valid;
        for (uint32_t b : all_backs)
            if (spin_region(T, {b}, s)) valid.push_back(b);
        if (valid.empty()) continue;
        if (valid.size() == 1 || !spin_region(T, valid, s)) spin_region(T, {valid.front()}, s);
        db.spin_loops.push_back(s);
    }
}

bool FnAnalyzer::spin_region(uint32_t T, const std::vector<uint32_t>& backs, SpinRec& s) {
    const uint32_t hi = *std::max_element(backs.begin(), backs.end());
    const uint32_t len = hi - T + 1;
    std::vector<uint8_t> ban(len, 0);
    for (int round = 0; round < 8; round++) {
        auto allowed = [&](uint32_t k) {
            if (ban[k - T]) return false;
            switch (fx[k].cls) {
            case Cls::alu: case Cls::load: case Cls::load_acq: case Cls::load_excl: case Cls::fp: case Cls::fp_load: case Cls::prefetch:
            case Cls::branch: case Cls::barrier: case Cls::store: case Cls::fp_store: return true;
            case Cls::hint: { const uint32_t h = hint_no(ins[k].w); return h != hint_wfi && h != hint_sev && h != hint_sevl; }
            default: return false;
            }
        };
        auto next = [&](uint32_t k, uint32_t out[2]) {
            const FlowInfo fl = flow(ins[k], pc(k));
            uint32_t m = 0;
            auto in = [&](uint64_t t) { return intra(t) && idx(t) >= T && idx(t) <= hi; };
            if (fl.kind == Flow::b) { if (in(fl.target)) out[m++] = idx(fl.target); }
            else if (fl.kind == Flow::bcond) {
                if (in(fl.target)) out[m++] = idx(fl.target);
                if (k + 1 <= hi) out[m++] = k + 1;
            } else if (k + 1 <= hi) out[m++] = k + 1;
            return m;
        };
        std::vector<uint8_t> reach(len, 0), keep(len, 0);
        std::vector<uint32_t> todo{T};
        while (!todo.empty()) {
            const uint32_t k = todo.back();
            todo.pop_back();
            if (reach[k - T] || !allowed(k)) continue;
            reach[k - T] = 1;
            uint32_t s[2];
            for (uint32_t q = 0, m = next(k, s); q < m; q++) todo.push_back(s[q]);
        }
        bool any = false;
        for (uint32_t b : backs)
            if (reach[b - T]) keep[b - T] = 1, any = true;
        if (!any) return false;
        for (bool more = true; more;) {
            more = false;
            for (uint32_t k = hi + 1; k-- > T;) {
                if (!reach[k - T] || keep[k - T]) continue;
                uint32_t s[2];
                for (uint32_t q = 0, m = next(k, s); q < m; q++)
                    if (s[q] != T && keep[s[q] - T]) { keep[k - T] = 1, more = true; break; }
            }
        }
        // liveness over the kept region
        struct L { uint32_t g = 0, v = 0; bool f = false; };
        std::vector<L> live(len);
        uint32_t dg = 0, dv = 0, stores = 0, loads = 0, size = 0;
        bool df = false, wfe = false, yield = false, cond = false, acquire = false;
        for (uint32_t k = T; k <= hi; k++) {
            if (!keep[k - T]) continue;
            size++;
            dg |= fx[k].def, dv |= fx[k].vdef, df |= fx[k].fdef;
            const Cls cl = fx[k].cls;
            if (cl == Cls::store || cl == Cls::fp_store) stores++;
            if (cl == Cls::load || cl == Cls::load_acq || cl == Cls::load_excl || cl == Cls::fp_load) loads++;
            if (cl == Cls::load_acq) acquire = true;
            if (cl == Cls::hint && hint_no(ins[k].w) == hint_wfe) wfe = true;
            if (cl == Cls::hint && hint_no(ins[k].w) == hint_yield) yield = true;
            if (flow(ins[k], pc(k)).kind == Flow::bcond) cond = true;
        }
        for (bool more = true; more;) {
            more = false;
            for (uint32_t k = hi + 1; k-- > T;) {
                if (!keep[k - T]) continue;
                L out;
                uint32_t s[2];
                for (uint32_t q = 0, m = next(k, s); q < m; q++)
                    if (keep[s[q] - T]) out.g |= live[s[q] - T].g, out.v |= live[s[q] - T].v, out.f |= live[s[q] - T].f;
                const Fx& f = fx[k];
                const L in{f.use | (out.g & ~f.def), f.vuse | (out.v & ~f.vdef), f.fuse || (out.f && !f.fdef)};
                L& cur = live[k - T];
                if (in.g != cur.g || in.v != cur.v || in.f != cur.f) cur = in, more = true;
            }
        }
        // a store whose value or address a pass changes is no part of a spin: drop the paths through it and look again
        bool varying = false;
        for (uint32_t k = T; k <= hi; k++)
            if (keep[k - T] && (fx[k].cls == Cls::store || fx[k].cls == Cls::fp_store) && ((fx[k].use & dg) || (fx[k].vuse & dv) || fx[k].def))
                ban[k - T] = 1, varying = true;
        if (varying) continue;
        const L& lt = live[0];
        if ((lt.g & dg) || (lt.v & dv) || (lt.f && df)) return false;   // a counter or cursor: the loop ends by itself
        if ((!loads && !wfe && !yield && !cond) || stores > 4) return false;   // b . alone is a deliberate hang, not a wait
        s = {};
        s.head = uint32_t(pc(T)), s.last = uint32_t(pc(hi)), s.function = gfi, s.insns = uint16_t(std::min<uint32_t>(size, 0xffff)), s.module = mod;
        s.kind = wfe ? spin_wfe : yield ? spin_yield : !loads ? spin_register : stores ? spin_poll_store : spin_poll;
        if (acquire) s.flags |= spin_acquire;
        flags |= fn_spin;
        if (!blocks.empty() && leader[T]) blocks[block_of[T]].flags |= blk_spin_head;
        return true;
    }
    return false;
}

void FnAnalyzer::run(uint32_t local) {
    fn = &c.fns[local];
    gfi = c.first_fn + local;
    n = (fn->end - fn->start) / 4;
    flags = fn->flags;
    ins.resize(n);
    fx.resize(n);
    for (uint32_t i = 0; i < n; i++) ins[i] = c.insn(fn->start + 4 * i), fx[i] = effects(ins[i]);
    leader.assign(n + 1, 0);
    noret.assign(n, 0);
    resume.assign(n + 1, 0);
    jts.clear();
    lps.clear();
    fsites.clear();
    lsda = {};
    leader[0] = 1;
    bool any_call = false;
    std::vector<uint32_t> brs;
    for (uint32_t i = 0; i < n; i++) {
        const FlowInfo fl = flow(ins[i], pc(i));
        switch (fl.kind) {
        case Flow::b: case Flow::bcond:
            if (intra(fl.target)) leader[idx(fl.target)] = 1;
            leader[i + 1] = 1;
            break;
        case Flow::br:
            leader[i + 1] = 1;
            if (!(fn->flags & fn_plt)) brs.push_back(i);
            break;
        case Flow::ret: case Flow::trap: case Flow::eret: leader[i + 1] = 1; break;
        case Flow::bl: {
            any_call = true;
            const Fn* g = fl.target < c.text ? callee(fl.target) : nullptr;
            if (i + 1 == n || (g && (g->flags & fn_noreturn))) noret[i] = 1, leader[i + 1] = 1;
            else if (g && is_setjmp(g->name)) leader[i + 1] = 1, resume[i + 1] = 1;
            break;
        }
        case Flow::blr:   // the last instruction of a function: the callee does not return
            any_call = true;
            if (i + 1 == n) noret[i] = 1, leader[i + 1] = 1;
            break;
        default: break;
        }
    }
    if (!any_call) flags |= fn_leaf;
    if (fn->lsda) {
        flags |= fn_lsda;
        try {
            lsda = parse_lsda(c, fn->lsda, fn->start);
        } catch (const std::exception& e) {
            stat("lsda_errors")++;
            note("LSDA of " + where(0) + ": " + e.what());
        }
        for (const auto& s : lsda.sites)
            if (s.landing_pad) {
                if (intra(s.landing_pad)) leader[idx(s.landing_pad)] = 1, lps.push_back(idx(s.landing_pad));
                else stat("landing_pads_outside_function")++;
            }
    }
    std::map<uint32_t, std::string> failed;
    for (int round = 0; round < 5; round++) {
        build();
        bool added = false;
        for (uint32_t k : brs) {
            if (jts.count(k)) continue;
            Jt jt;
            std::string why;
            if (!resolve_jt(k, jt, why)) { failed[k] = why; continue; }
            failed.erase(k);
            if (jt.local)
                for (uint64_t t : jt.targets)
                    if (t != never && !leader[idx(t)]) leader[idx(t)] = 1, added = true;
            jts[k] = std::move(jt);
        }
        if (!added) break;
    }
    if (bases_out) {   // first pass: only where the tables start
        for (auto& [k, jt] : jts) bases_out->push_back(jt.table);
        return;
    }
    build();
    for (auto& [k, why] : failed) {
        const Defs d = find_defs(k, bit(flow(ins[k], pc(k)).reg));
        const int j = d.unique();
        // only branches that look like a table dispatch count as failed jump tables
        if (j < 0) continue;
        const Insn J = ins[size_t(j)];
        if (J.op == Op::ADD_ext || (J.op == Op::ADD_reg && F<Op::ADD_reg>(J.w).imm6 == 2)) {
            stat("jt_failed")++;
            note("jump table not resolved at " + where(k) + ": " + why);
        } else if (J.op == Op::LDR_reg && F<Op::LDR_reg>(J.w).sf && F<Op::LDR_reg>(J.w).S) {
            stat("jt_abs_failed")++;
            note("absolute table not resolved at " + where(k) + ": " + why);
        }
    }
    for (auto& [k, jt] : jts) {
        if (jt.kind == jt_abs64) { stat(jt.local ? "jt_abs_local" : "jt_abs_function_table")++; continue; }
        stat(jt.bound == jb_heuristic ? "jt_unbounded" : "jt_resolved")++;
        stat("jt_entries") += int64_t(jt.targets.size());
    }
    special();
    indirect();
    calls();
    spins();
    emit();
}

void FnAnalyzer::emit() {
    FunctionRec f{};
    f.start = fn->start, f.size = fn->end - fn->start, f.name = db.add_string(fn->name), f.flags = flags, f.sources = fn->sources, f.module = mod;
    f.first_block = uint32_t(db.blocks.size()), f.block_count = uint32_t(blocks.size()), f.lsda = fn->lsda;
    f.first_call_site = uint32_t(db.call_sites.size()), f.call_site_count = uint32_t(lsda.sites.size());
    for (const auto& s : lsda.sites) {
        CallSiteRec r{};
        r.function = gfi, r.start = uint32_t(s.start), r.size = uint32_t(s.size), r.landing_pad = uint32_t(s.landing_pad);
        r.action = uint32_t(s.action), r.first_filter = int32_t(s.first_filter), r.type_name = db.add_string(s.first_type), r.module = mod;
        db.call_sites.push_back(r);
    }
    blocks[0].flags |= blk_entry;
    for (uint32_t l : lps) blocks[block_of[l]].flags |= blk_landing_pad;
    for (auto& [k, jt] : jts)
        if (jt.local)
            for (uint64_t t : jt.targets)
                if (t != never) blocks[block_of[idx(t)]].flags |= blk_jt_target;
    for (uint32_t i = 0; i < n; i++)
        if (resume[i] && i < n) blocks[block_of[i]].flags |= blk_resume;
    const uint32_t first = f.first_block;
    for (size_t b = 0; b < blocks.size(); b++) {
        B& k = blocks[b];
        if (!pcount[b] && !k.pad && !(k.flags & (blk_entry | blk_landing_pad | blk_jt_target))) k.flags |= blk_unreachable;
        if (k.term == term_end) stat("blocks_falling_off_the_end")++;
        BlockRec r{};
        r.start = uint32_t(pc(k.s)), r.size = 4 * (k.e - k.s), r.function = gfi, r.first_edge = uint32_t(db.edges.size());
        r.edge_count = uint16_t(k.scount), r.term = k.term, r.flags = k.flags;
        for (uint32_t s = k.sfirst; s < k.sfirst + k.scount; s++) db.edges.push_back({first + succ[s].first, succ[s].second, {}});
        db.blocks.push_back(r);
    }
    f.flags = flags;
    db.functions.push_back(f);
    db.sites.insert(db.sites.end(), fsites.begin(), fsites.end());
}

}  // namespace

void analyze_functions(Ctx& x, ModCtx& c) {
    uint8_t mod = 0;
    for (size_t i = 0; i < x.mods.size(); i++)
        if (&x.mods[i] == &c) mod = uint8_t(i);
    // Two passes: the first finds where every jump table starts, so that a table whose bound is not found is read at most up to
    // the next one
    std::vector<uint32_t> bases;
    {
        FnAnalyzer a(x, c, mod);
        a.bases_out = &bases;
        for (uint32_t i = 0; i < c.fns.size(); i++) a.run(i);
    }
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    FnAnalyzer a(x, c, mod);
    a.bases = &bases;
    for (uint32_t i = 0; i < c.fns.size(); i++) a.run(i);
}

}  // namespace nxanalyze
