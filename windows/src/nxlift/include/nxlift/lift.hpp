// nxlift: program.db functions to LLVM IR through the semantics bitcode, then optimised x86-64 code (COFF objects, or a JIT).
// One guest function becomes `void fn_<guest address hex>(NxwGuestCpu* noalias)` (DESIGN D2); docs/m3-lifter.md has the design.
#pragma once
#include "nxanalyze/db.hpp"
#include "nxload/program.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
class Function;
class TargetMachine;
}  // namespace llvm

namespace nxlift {

// The program as the lifter reads it: nxload's modules (text words) and program.db, cross-indexed
struct Inputs {
    nxload::Program prog;
    nxanalyze::Database db;
    std::vector<int> prog_index;   // db module record -> prog.modules index
    std::unordered_map<uint64_t, uint32_t> call_at;   // (db module << 32 | site) -> db.calls index
    std::unordered_map<uint64_t, uint32_t> fn_at;     // absolute start -> db.functions index
    std::unordered_map<uint64_t, uint32_t> plt_at;    // (db module << 32 | stub) -> db.plt_stubs index

    uint64_t fn_address(uint32_t f) const { return db.modules[db.functions[f].module].base + db.functions[f].start; }
    const uint32_t* words(uint32_t f) const;   // the function's instruction words
    std::string fn_name(uint32_t f) const;     // "fn_7100001234"
    const char* module_name(uint32_t m) const { return db.str(db.modules[m].name); }
    // A function by symbol name (exact match), or none32
    uint32_t find_symbol(std::string_view name) const;
};
// relocate: keep the relocated images (the JIT harness maps them); the lifter itself only needs text.
// bases: modules placed at other addresses than program.db's (by name); program.db's own records are module-relative, so its
// module bases are moved to match (the Mac swap-in harness maps nxnative's rewritten images there, docs/m4-swapin.md)
Inputs load_inputs(const std::filesystem::path& exefs, const std::filesystem::path& db, bool relocate,
                   const std::map<std::string, uint64_t, std::less<>>& bases = {});

struct Options {
    int opt = 2;                   // 1 or 2 (O1 / O2)
    bool jit = false;              // ORC JIT: no unwind tables, no dllimport, large code model
    bool dllimport = true;         // objects: the rt_* data symbols come from nxrt.exe
    bool spin_yield = true;        // rt_yield at each spin-loop head (program.db spin_loops), YIELD, WFE and WFI
    bool entry_trace = false;      // rt_trace_entry at function entry while rt_trace_on is set (the runtime's NXRT_TRACE)
    // The Mac swap-in harness (docs/m4-swapin.md): every direct call and tail call goes to its target as the guest branched to it
    // (a PLT stub stays a stub) through nxw_sem_lookup, so the runtime decides at run time whether a callee runs lifted or native
    bool swapin = false;
};

// Per-op counts of instructions lifted without semantics (they trap at run time), and other per-run facts
struct LiftStats {
    std::map<std::string, uint64_t> unsupported;   // op name -> instructions
    uint64_t functions = 0, instructions = 0, blocks = 0;
    uint64_t calls_guest = 0, calls_host = 0, calls_indirect = 0, tails = 0, tails_indirect = 0, jump_tables = 0, traps = 0,
             svcs = 0, sysreg_calls = 0, yields = 0, contexts = 0;
    void add(const LiftStats& o);
};

// Lifts functions into modules of one LLVMContext. The semantics bitcode is parsed once per Lifter; each module gets the
// semantics it uses linked in when it is finished.
class Lifter {
public:
    Lifter(const Inputs& in, llvm::LLVMContext& ctx, const Options& opt);
    ~Lifter();

    std::unique_ptr<llvm::Module> new_module(const std::string& name);
    llvm::Function* lift(llvm::Module& m, uint32_t fn);   // defines fn_<addr>
    // A straight-line sequence of instruction words at pc, then a return (no branches): for instruction-level tests
    llvm::Function* lift_words(llvm::Module& m, const std::string& name, const std::vector<uint32_t>& words, uint64_t pc);
    // Links and inlines the semantics, tags guest memory and cpu accesses for alias analysis, verifies
    void finish(llvm::Module& m);
    // The default O1/O2 pipeline (cross-function inlining off: every lifted function is noinline)
    void optimize(llvm::Module& m);
    // COFF x86-64 object of the module
    std::vector<char> emit_object(llvm::Module& m);
    llvm::TargetMachine& target();
    bool has_semantics(const std::string& op) const;

    LiftStats stats;
    struct Impl;

private:
    std::unique_ptr<Impl> p;
};

// The semantics bitcode embedded in the lifter
std::string_view semantics_bitcode();

// Every guest function directly called (BL and tail calls) from fns, transitively; host-bound callees are not included
std::vector<uint32_t> call_closure(const Inputs& in, const std::vector<uint32_t>& fns);

// The game DLL's own object (docs/m3-boot.md): the nxw_game_* exports nxrt checks and loads (nxrt/rt_abi.h), with one
// {guest address, RVA} entry per lifted function (image-relative relocations against fn_<addr>), and what LLVM's code may call
// in a freestanding DLL (__chkstk, memcpy, memmove, memset, _fltused)
std::vector<char> game_table_object(Lifter& L, const Inputs& in, const std::vector<uint32_t>& fns);

// The swap-in dylib's table (src/swapin/swapin.h): {guest address, fn_<addr>, program.db flags, size} per lifted function, the
// module bases the code was lifted for, and main's build ID
std::vector<char> swapin_table_object(Lifter& L, const Inputs& in, const std::vector<uint32_t>& fns);

}  // namespace nxlift
