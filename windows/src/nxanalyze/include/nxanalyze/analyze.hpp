// nxanalyze: the program analysis the lifter consumes. Functions (FDEs plus BL targets, relocations, exports, init/fini, PLT stubs
// and a conservative prologue scan for code without FDEs), per-function CFGs, jump tables, every indirect branch classified,
// LSDA call sites and landing pads, special functions (SP/LR rewriters, context branches, system registers, SVCs, traps),
// spin loops, and nxnative's HLE binding of every import and export.
#pragma once
#include "nxanalyze/db.hpp"
#include "nxload/program.hpp"

#include <map>

namespace nxanalyze {

struct Options {
    std::filesystem::path nxnative;               // nxnative source tree (dm/host, dm/gfx/nvn); empty: no HLE binding
    std::filesystem::path hle_inventory;          // or the runtime's registry (nxrt's gen/out/hle_inventory.tsv), which wins over nxnative
    std::vector<std::filesystem::path> traces;    // Mac trace runs (executed_<module>.txt, trace/ind_log.bin, trace/modules.tsv)
    std::filesystem::path rewriter;               // nxnative rewriter tables (<module>.sites.tsv) for the spin-loop comparison
};

// Findings that are not part of program.db: counts and examples for the report
struct TraceCheck {
    std::string run, module;
    size_t executed = 0, missing = 0;             // executed function starts, and those not in the function set
    std::vector<uint32_t> missing_examples;
};
struct IndirectCheck {
    std::string run;
    size_t records = 0, pairs = 0, sites = 0;
    std::map<std::string, size_t> by_class;       // "BR jump table: target in table" ...
    std::vector<std::string> problems;
};
struct SpinCheck {
    std::string module;
    size_t ours = 0, theirs = 0, both = 0;
    std::vector<uint32_t> only_ours, only_theirs;
};

struct Analysis {
    Database db;
    std::vector<std::map<std::string, int64_t>> stats;   // per module record of db.modules
    std::vector<std::vector<std::string>> notes;
    std::vector<TraceCheck> trace_checks;
    std::vector<IndirectCheck> indirect_checks;
    std::vector<SpinCheck> spin_checks;
    std::map<std::string, int64_t> hle_stats;
    std::vector<std::string> problems;
};

Analysis analyze(const nxload::Program& p, const Options& opt = {});

void write_report_json(const std::filesystem::path& p, const Analysis& a);
void write_report_md(const std::filesystem::path& p, const Analysis& a, const std::string& title);

const char* indirect_class_name(uint8_t cls);
const char* site_kind_name(uint8_t kind);
const char* binding_name(uint8_t b);

// The takeover check (takeover.cpp): program.db's host-bound symbols against the runtime's registry inventory (nxrt gen/out/hle_inventory.tsv).
// differences is empty when every symbol the program imports or exports is forced on both sides or neither, and swhle the same way.
struct TakeoverCheck {
    size_t symbols = 0, db_forced = 0, forced_names = 0, db_swhle = 0, registry_forced = 0, registry_swhle = 0;
    std::vector<std::string> differences;
};
TakeoverCheck check_takeovers(const Database& db, const std::filesystem::path& inventory);

}  // namespace nxanalyze
