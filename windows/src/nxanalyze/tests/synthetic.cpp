// Tests without game data: instruction effects, bitmask immediates, the program.db round trip, and the whole analysis on a small
// synthetic module (a bounded jump table, a spin loop, a tail call, code without an FDE).
#include "builder.hpp"
#include "check.hpp"
#include "insn.hpp"
#include "nxanalyze/analyze.hpp"

using namespace nxanalyze;

namespace {

Insn I(uint32_t w) { return {w, nxw::a64::decode(w)}; }

uint32_t adrp(uint64_t pc, uint64_t target, uint32_t rd) {
    const int64_t imm = int64_t(target >> 12) - int64_t(pc >> 12);
    return 0x90000000u | uint32_t(imm & 3) << 29 | uint32_t((imm >> 2) & 0x7ffff) << 5 | rd;
}
uint32_t adr(uint64_t pc, uint64_t target, uint32_t rd) {
    const int64_t imm = int64_t(target) - int64_t(pc);
    return 0x10000000u | uint32_t(imm & 3) << 29 | uint32_t((imm >> 2) & 0x7ffff) << 5 | rd;
}
uint32_t add_imm(uint32_t rd, uint32_t rn, uint32_t imm) { return 0x91000000u | imm << 10 | rn << 5 | rd; }
uint32_t bcond(uint64_t pc, uint64_t target, uint32_t cond) { return 0x54000000u | uint32_t(((int64_t(target) - int64_t(pc)) >> 2) & 0x7ffff) << 5 | cond; }
uint32_t b(uint64_t pc, uint64_t target) { return 0x14000000u | uint32_t(((int64_t(target) - int64_t(pc)) >> 2) & 0x3ffffff); }

// 0x10 f: a switch over w0 (cmp #3, b.hi, byte table in data, adr anchor), cases at 0x30, 0x38, 0x40
// 0x48 g: ldar/cbz spin, then a tail call to h
// 0x54 h: code without an FDE, reached only by g's tail call
Analysis synthetic(uint64_t& table) {
    build::Spec s;
    s.funcs = {{0, 4}, {0x10, 0x38}, {0x48, 0xc}};
    s.words[0] = 0x00040200;   // entries 0, 2, 4, 0
    auto text_for = [&](uint64_t tab) {
        return std::vector<uint32_t>{
            0x14000004, 0, 0xd503201f, 0xd503201f,   // b 0x10; MOD0 offset; padding
            0x71000c1f, bcond(0x14, 0x30, 8), adrp(0x18, tab, 9), add_imm(9, 9, uint32_t(tab & 0xfff)), adr(0x20, 0x30, 10),
            0x3860692b, 0x8b0b094a, 0xd61f0140,                 // ldrb w11, [x9, x0]; add x10, x10, x11, lsl #2; br x10
            0x52800000, 0xd65f03c0, 0x52800020, 0xd65f03c0, 0x52800040, 0xd65f03c0,
            0x88dffc28, 0x34ffffe8, b(0x50, 0x54),            // g: ldar w8, [x1]; cbz w8, -4; b h
            0x52800060, 0xd65f03c0,                             // h
        };
    };
    s.text = text_for(0);
    table = build::build_nso(s).words;   // where the data words land does not depend on the text's contents
    s.text = text_for(table);
    static nxload::Program p;
    p = nxload::link({nxload::load_module("main", build::build_nso(s).nso)});
    return analyze(p);
}

}  // namespace

TEST(whole_analysis) {
    uint64_t table = 0;
    const Analysis a = synthetic(table);
    const Database& db = a.db;
    CHECK_EQ(db.modules.size(), 1u);
    size_t unresolved = 0;
    for (const SiteRec& s : db.sites) unresolved += s.kind == site_unresolved_branch;
    CHECK_EQ(unresolved, 0u);
    // the entry word, f, g, and h found through g's tail call
    std::vector<uint32_t> starts;
    for (const FunctionRec& f : db.functions) starts.push_back(f.start);
    CHECK(starts == std::vector<uint32_t>({0, 0x10, 0x48, 0x54}));
    if (!CHECK_EQ(db.jump_tables.size(), 1u)) return;
    const JumpTableRec& j = db.jump_tables[0];
    CHECK_EQ(j.site, 0x2cu);
    CHECK_EQ(j.table, table);
    CHECK_EQ(j.count, 4u);
    CHECK_EQ(j.bound, jb_cmp);
    CHECK_EQ(j.kind, jt_rel_u8);
    CHECK(std::vector<uint32_t>(db.jt_targets.begin(), db.jt_targets.end()) == std::vector<uint32_t>({0x30, 0x38, 0x40, 0x30}));
    // f's blocks: 0x10 (cmp, b.hi), 0x18 (the dispatch), and one per case
    const FunctionRec& f = db.functions[1];
    CHECK_EQ(f.block_count, 5u);
    CHECK_EQ(db.blocks[f.first_block + 1].term, term_jump_table);
    CHECK_EQ(db.blocks[f.first_block + 1].edge_count, 3u);
    if (CHECK_EQ(db.spin_loops.size(), 1u)) {
        CHECK_EQ(db.spin_loops[0].head, 0x48u);
        CHECK(db.spin_loops[0].flags & spin_acquire);
    }
    bool tail = false;
    for (const CallRec& c : db.calls) tail |= c.kind == call_tail && c.site == 0x50 && c.callee == 3;
    CHECK(tail);
    CHECK(db.functions[3].flags & fn_no_fde);
}

TEST(flow_kinds) {
    CHECK(flow(I(0x94000010), 0x1000).kind == Flow::bl);
    CHECK_EQ(flow(I(0x94000010), 0x1000).target, 0x1040u);
    CHECK_EQ(flow(I(0x17ffffff), 0x1000).target, 0xffcu);                 // b .-4
    CHECK(flow(I(0x54000088), 0x1000).kind == Flow::bcond);             // b.hi
    CHECK_EQ(flow(I(0x54000088), 0x1000).target, 0x1010u);
    CHECK(flow(I(0xd61f0100), 0).kind == Flow::br);                     // br x8
    CHECK_EQ(flow(I(0xd61f0100), 0).reg, 8u);
    CHECK(flow(I(0xd65f0fff), 0).kind == Flow::ret);                    // retab
    CHECK(flow(I(0x00000000), 0).kind == Flow::trap);                   // udf #0
}

TEST(effects_basic) {
    Fx f = effects(I(0x910003fd));   // mov x29, sp
    CHECK_EQ(f.def, FP);
    CHECK_EQ(f.use, SP);
    f = effects(I(0x9100005f));      // mov sp, x2
    CHECK_EQ(f.def, SP);
    CHECK_EQ(f.use, 1u << 2);
    f = effects(I(0xa9bf7bfd));      // stp x29, x30, [sp, #-16]!
    CHECK(f.cls == Cls::store);
    CHECK_EQ(f.def, SP);
    CHECK_EQ(f.use, SP | FP | LR);
    f = effects(I(0xa8c17bfd));      // ldp x29, x30, [sp], #16
    CHECK_EQ(f.def, SP | FP | LR);
    f = effects(I(0x7100151f));      // cmp w8, #5
    CHECK(f.fdef);
    CHECK_EQ(f.def, 0u);
    CHECK_EQ(f.use, 1u << 8);
    f = effects(I(0x38686949));      // ldrb w9, [x10, x8]
    CHECK(f.cls == Cls::load);
    CHECK_EQ(f.def, 1u << 9);
    CHECK_EQ(f.use, (1u << 10) | (1u << 8));
    f = effects(I(0x8b29096a));      // add x10, x11, w9, uxtb #2
    CHECK_EQ(f.def, 1u << 10);
    CHECK_EQ(f.use, (1u << 11) | (1u << 9));
    f = effects(I(0xd503237f));      // pacibsp
    CHECK(f.cls == Cls::hint);
    CHECK_EQ(f.def, LR);
    f = effects(I(0x88dffc08));      // ldar w8, [x0]
    CHECK(f.cls == Cls::load_acq);
    f = effects(I(0x1e602000));      // fcmp d0, d0
    CHECK(f.fdef);
    CHECK_EQ(f.vuse, 1u);
    f = effects(I(0x9e660008));      // fmov x8, d0
    CHECK_EQ(f.def, 1u << 8);
}

TEST(bitmask) {
    CHECK_EQ(bitmask_imm(0, 0, 2, 32), 7u);           // and w8, w8, #0x7
    CHECK_EQ(bitmask_imm(1, 0, 7, 64), 0xffu);
    CHECK_EQ(bitmask_imm(0, 0, 0x3c, 64), 0x5555555555555555ull);
}

TEST(db_round_trip) {
    Database db;
    ModuleRec m{};
    m.name = db.add_string("main"), m.text_size = 0x1000, m.base = 0x7100000000;
    db.modules.push_back(m);
    FunctionRec f{};
    f.start = 0x10, f.size = 0x20, f.name = db.add_string("f"), f.flags = fn_leaf, f.sources = src_fde;
    db.functions.push_back(f);
    db.blocks.push_back({0x10, 0x20, 0, 0, 0, term_return, blk_entry});
    db.jt_targets = {0x14, 0x18};
    const auto bytes = serialize(db);
    const Database r = deserialize(bytes);
    CHECK_EQ(r.modules.size(), 1u);
    CHECK_EQ(std::string(r.str(r.modules[0].name)), std::string("main"));
    CHECK_EQ(r.modules[0].base, 0x7100000000ull);
    CHECK_EQ(r.functions.size(), 1u);
    CHECK_EQ(std::string(r.str(r.functions[0].name)), std::string("f"));
    CHECK_EQ(r.blocks[0].term, term_return);
    CHECK_EQ(r.jt_targets.size(), 2u);
    auto bad = bytes;
    bad[8] = 99;   // version
    CHECK_THROWS(deserialize(bad));
    bad = bytes;
    bad.pop_back();
    CHECK_THROWS(deserialize(bad));
}

int main() { return check::run(); }
