// Checks against a real dump: NXW_DUMP names an ExeFS folder; exit 77 (skipped) without it. The acceptance criteria hold for any
// build: no unresolved direct branch, every FDE is a function, functions are disjoint and inside text, every block ends where the
// next begins, every edge stays in its function, and program.db survives a round trip.
#include "check.hpp"
#include "nxanalyze/analyze.hpp"

#include <cstdlib>

using namespace nxanalyze;

namespace {

const Analysis* g = nullptr;

}  // namespace

TEST(no_unresolved_direct_branches) {
    size_t n = 0;
    for (const SiteRec& s : g->db.sites) n += s.kind == site_unresolved_branch;
    CHECK_EQ(n, 0u);
    for (const CallRec& c : g->db.calls) CHECK(c.binding != bind_none);
}

TEST(functions_disjoint_and_complete) {
    const Database& db = g->db;
    for (const ModuleRec& m : db.modules) {
        uint64_t end = 0;
        size_t fde = 0;
        for (uint32_t i = m.first_function; i < m.first_function + m.function_count; i++) {
            const FunctionRec& f = db.functions[i];
            CHECK(f.start >= end);
            CHECK(f.start + f.size <= m.text_size);
            CHECK(f.size > 0);
            end = f.start + f.size;
            fde += (f.sources & src_fde) != 0;
            CHECK(f.block_count > 0);
        }
        CHECK(fde > 0);
    }
}

TEST(blocks_and_edges) {
    const Database& db = g->db;
    for (uint32_t fi = 0; fi < db.functions.size(); fi++) {
        const FunctionRec& f = db.functions[fi];
        uint32_t at = f.start;
        for (uint32_t b = f.first_block; b < f.first_block + f.block_count; b++) {
            const BlockRec& k = db.blocks[b];
            if (!CHECK_EQ(k.start, at)) break;
            at += k.size;
            for (uint32_t e = k.first_edge; e < k.first_edge + k.edge_count; e++) {
                const uint32_t t = db.edges[e].block;
                CHECK(t >= f.first_block && t < f.first_block + f.block_count);
            }
        }
        CHECK_EQ(at, f.start + f.size);
    }
}

TEST(jump_table_targets_are_blocks) {
    const Database& db = g->db;
    for (const JumpTableRec& j : db.jump_tables) {
        if (j.kind == jt_abs64) continue;
        const FunctionRec& f = db.functions[j.function];
        for (uint32_t t = j.first_target; t < j.first_target + j.count; t++) {
            const uint32_t a = db.jt_targets[t];
            if (a == none32) continue;   // an entry the program never takes
            CHECK(a >= f.start && a < f.start + f.size);
            bool block = false;
            for (uint32_t b = f.first_block; b < f.first_block + f.block_count && !block; b++) block = db.blocks[b].start == a;
            CHECK(block);
        }
    }
}

// NXW_TRACES (optional) names a Mac trace run of the same build (research/05): every executed function start is a function, and
// every recorded br/blr target is a function start, a label in the decoded jump table, or host code
TEST(trace_coverage) {
    for (const TraceCheck& t : g->trace_checks) CHECK_EQ(t.missing, 0u);
    for (const IndirectCheck& t : g->indirect_checks) CHECK(t.problems.empty());
}

TEST(round_trip) {
    const Database r = deserialize(serialize(g->db));
    CHECK_EQ(r.functions.size(), g->db.functions.size());
    CHECK_EQ(r.blocks.size(), g->db.blocks.size());
    CHECK_EQ(r.strings, g->db.strings);
}

int main() {
    const char* dump = std::getenv("NXW_DUMP");
    if (!dump || !*dump) {
        std::printf("NXW_DUMP not set: skipped\n");
        return 77;
    }
    const nxload::Program p = nxload::load_program(dump);
    Options opt;
    if (const char* t = std::getenv("NXW_TRACES"); t && *t) opt.traces.push_back(t);
    const Analysis a = analyze(p, opt);
    g = &a;
    return check::run();
}
