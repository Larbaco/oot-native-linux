// State shared by the analysis passes (discover, cfg, lsda, special, hle, traces, report).
#pragma once
#include "insn.hpp"
#include "nxanalyze/analyze.hpp"
#include "nxload/ehframe.hpp"

#include <map>
#include <unordered_map>
#include <unordered_set>

namespace nxanalyze {

std::string hex(uint64_t v);

struct Fn {
    uint32_t start = 0, end = 0;   // module offsets, end exclusive
    uint16_t sources = 0;
    uint32_t flags = 0;
    int fde = -1;
    uint32_t lsda = 0;
    std::string name;
};

struct RelAt {
    uint32_t type = 0;   // R_AARCH64_*, or 0 for RELR
    uint32_t sym = 0;
};

struct ModCtx {
    int index = 0;   // in the Program
    const nxload::Module* m = nullptr;
    uint64_t base = 0;
    std::span<const uint8_t> img, rel;   // pristine and relocated images
    uint32_t text = 0;
    std::vector<Op> ops;                 // one per text word
    nxload::EhFrame eh;
    std::unordered_map<uint32_t, RelAt> rel_at;           // relocated slots by offset
    std::unordered_map<uint32_t, std::string> names;      // defined symbols in text by offset (exports first)
    std::unordered_map<uint32_t, uint32_t> sym_at;        // any defined symbol by offset -> dynsym index (the first)
    std::vector<Fn> fns;                                  // sorted by start, disjoint
    uint32_t first_fn = 0;                                // global index of fns[0]
    std::unordered_map<uint32_t, uint32_t> plt_sym;       // PLT stub offset -> dynsym index
    std::unordered_map<uint32_t, uint32_t> plt_got;       // PLT stub offset -> its GOT slot
    std::map<uint32_t, uint32_t> plt0;                    // PLT header offset -> size
    std::vector<std::pair<uint32_t, uint32_t>> fde_ranges;   // sorted (start, end)
    std::map<uint32_t, uint16_t> seeds;                   // function starts found so far, with their sources
    std::vector<std::pair<uint32_t, uint32_t>> adr_refs;  // (site, target) of adr / adrp+add into text
    int fde_at(uint32_t off) const;                       // index into fde_ranges, -1
    // Discovery notes for the report
    std::map<std::string, int64_t> stat;
    std::vector<std::string> notes;

    uint32_t word(uint32_t off) const { return nxw::load_le<uint32_t>(img, off); }
    Insn insn(uint32_t off) const { return {word(off), ops[off / 4]}; }
    bool in_text(uint64_t off) const { return off < text; }
    int fn_at(uint32_t off) const;          // the function containing off, -1
    int fn_starting(uint32_t off) const;    // the function starting at off, -1
    uint64_t read64_rel(uint32_t off) const { return nxw::load_le<uint64_t>(rel, off); }
    const std::string& symbol_name(uint32_t sym) const { return m->symbols[sym].name; }
};

struct Hle {
    bool loaded = false;
    std::unordered_map<std::string, std::string> forced;   // name -> file
    std::unordered_set<std::string> swhle;
    std::vector<std::string> nvn;
    size_t forced_entries = 0, swhle_entries = 0, files = 0;
};
Hle load_hle(const std::filesystem::path& nxnative);
Hle load_hle_inventory(const std::filesystem::path& tsv);

struct Ctx {
    const nxload::Program* p = nullptr;
    const Options* opt = nullptr;
    std::vector<ModCtx> mods;
    Hle hle;
    Analysis* out = nullptr;
    // symbol record index by (module, dynsym index)
    std::vector<std::unordered_map<uint32_t, uint32_t>> symrec;

    // The function an absolute address starts, as (module ctx, local index), or nullptr
    std::pair<ModCtx*, int> fn_at_abs(uint64_t a);
};

void decode_module(ModCtx& c);
void discover(Ctx& x, ModCtx& c);
void add_cross_module_seeds(Ctx& x);
void finish_discovery(Ctx& x, ModCtx& c);
void mark_noreturn(Ctx& x);
void bind_symbols(Ctx& x);
void analyze_functions(Ctx& x, ModCtx& c);
void check_traces(Ctx& x);
void check_rewriter(Ctx& x);

}  // namespace nxanalyze
