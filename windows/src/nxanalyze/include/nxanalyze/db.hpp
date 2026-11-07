// program.db: what nxanalyze found, for the lifter. A header, a section table, then arrays of fixed-size little-endian records.
// Addresses are module offsets (u32) plus a module index; module bases are in the module records. docs/m2-nxanalyze.md has the format.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nxanalyze {

inline constexpr char db_magic[8] = {'N', 'X', 'A', 'N', 'A', 'D', 'B', '\0'};
inline constexpr uint32_t db_version = 1;
inline constexpr uint32_t none32 = 0xffffffffu;

struct DbHeader {
    char magic[8];
    uint32_t version, header_size;
    uint32_t section_count, section_entry_size;
    uint64_t file_size;
    char producer[32];   // "nxanalyze <version>", NUL-padded
};
static_assert(sizeof(DbHeader) == 64);

struct DbSection {
    uint32_t id, record_size;
    uint64_t offset, count;
};
static_assert(sizeof(DbSection) == 24);

enum class Sec : uint32_t {
    strings = 1, modules, functions, blocks, edges, calls, indirect, jump_tables, jt_targets, call_sites, sites, spin_loops,
    symbols, plt_stubs,
};

// Function sources (bits): why the address is a function start
enum : uint16_t {
    src_fde = 1, src_bl = 2, src_tail = 4, src_reloc = 8, src_export = 16, src_init = 32, src_plt = 64, src_adr = 128,
    src_prologue = 256, src_entry = 512, src_symbol = 1024,
};
// Function flags
enum : uint32_t {
    fn_no_fde = 1, fn_plt = 2, fn_noreturn = 4, fn_lsda = 8, fn_jump_tables = 0x10, fn_indirect_tail = 0x20,
    fn_sp_switch = 0x40, fn_sp_dynamic = 0x80, fn_lr_write = 0x100, fn_context_branch = 0x200, fn_sysreg = 0x400, fn_svc = 0x800,
    fn_trap = 0x1000, fn_spin = 0x2000, fn_setjmp_caller = 0x4000, fn_unresolved = 0x10000, fn_exclusive = 0x20000, fn_leaf = 0x40000,
    fn_ret_other = 0x80000,
};
// Block terminators
enum : uint8_t {
    term_fall, term_branch, term_cond, term_jump_table, term_tail, term_indirect_tail, term_return, term_trap, term_noreturn_call,
    term_end, term_eret,
};
// Block flags
enum : uint8_t { blk_entry = 1, blk_landing_pad = 2, blk_jt_target = 4, blk_resume = 8, blk_spin_head = 0x10, blk_unreachable = 0x20 };
// Edge kinds
enum : uint8_t { edge_fall, edge_branch, edge_taken, edge_not_taken, edge_jump_table };
// Direct call kinds
enum : uint8_t { call_bl, call_tail, call_cond_tail };
// Bindings of symbols, PLT stubs and calls
enum : uint8_t { bind_guest, bind_forced, bind_swhle, bind_stub, bind_weak_zero, bind_none = 6 };
// Indirect-branch instruction
enum : uint8_t { ind_br, ind_blr, ind_ret };
// Indirect-branch classes
enum : uint8_t {
    ic_jump_table, ic_jump_table_unbounded, ic_table_local, ic_table_tail, ic_plt, ic_vtable, ic_vtable_stack, ic_got,
    ic_global_obj, ic_mem, ic_mem_indexed, ic_ldp, ic_stack, ic_arg, ic_call_result, ic_const, ic_acquire, ic_csel, ic_return_reg,
    ic_context, ic_other, ic_count
};
// Jump-table entry kinds and bound sources
enum : uint8_t { jt_rel_s8, jt_rel_u8, jt_rel_s16, jt_rel_u16, jt_rel_s32, jt_rel_u32, jt_abs64 };
enum : uint8_t { jb_cmp, jb_mask, jb_load_width, jb_heuristic };
// Special sites
enum : uint8_t {
    site_svc, site_mrs, site_msr, site_msr_imm, site_sys, site_udf, site_brk, site_hlt, site_wfe, site_wfi, site_yield, site_sev,
    site_sevl, site_excl_load, site_excl_store, site_clrex, site_sp_switch, site_sp_dynamic, site_lr_write, site_ret_reg,
    site_context_branch, site_setjmp_call, site_longjmp_call, site_pac_data, site_unresolved_branch, site_count
};
// Spin-loop kinds and flags
enum : uint8_t { spin_poll, spin_poll_store, spin_wfe, spin_yield, spin_register };
enum : uint32_t { spin_in_nxnative = 1, spin_acquire = 2 };   // found by nxnative's rewriter too; polls with ldar/ldapr

struct ModuleRec {
    uint32_t name;          // string offset
    uint32_t text_size;
    uint64_t base;          // as laid out by nxload (absolute = base + offset)
    uint32_t image_size;
    uint32_t first_function, function_count;
    uint32_t first_symbol, symbol_count;
    uint32_t flags;
    uint8_t build_id[20];
    uint32_t reserved;
};
static_assert(sizeof(ModuleRec) == 64);

struct FunctionRec {
    uint32_t start, size;   // module offset, bytes
    uint32_t name;          // string offset (0 = unnamed)
    uint32_t flags;
    uint16_t sources;
    uint8_t module, reserved;
    uint32_t first_block, block_count;
    uint32_t lsda;          // module offset of the LSDA, 0 if none
    uint32_t first_call_site, call_site_count;   // LSDA call-site records
};
static_assert(sizeof(FunctionRec) == 40);

struct BlockRec {
    uint32_t start, size;
    uint32_t function;
    uint32_t first_edge;
    uint16_t edge_count;
    uint8_t term, flags;
};
static_assert(sizeof(BlockRec) == 20);

struct EdgeRec {
    uint32_t block;   // global block index
    uint8_t kind, reserved[3];
};
static_assert(sizeof(EdgeRec) == 8);

struct CallRec {          // every direct BL, and every B / B.cond that leaves its function
    uint32_t site;
    uint32_t function;    // the calling function
    uint32_t target;      // the target as branched to (module offset in the same module)
    uint32_t callee;      // function index after PLT binding, none32 for host or unbound
    uint32_t symbol;      // symbol record of a PLT-bound target, none32 otherwise
    uint8_t module, kind, binding, reserved;
};
static_assert(sizeof(CallRec) == 24);

struct IndirectRec {      // every BR, BLR and RET xN (N != 30), authenticated forms included
    uint32_t site;
    uint32_t function;
    uint32_t aux;         // jump table index; GOT/global slot (module offset); vtable slot offset
    uint32_t symbol;      // string: the symbol of a GOT slot, or 0
    uint8_t module, insn, cls, flags;   // flags: 1 several reaching definitions, 2 a GOT slot bound to host code (HLE),
                                        // 4 br with x30 not the caller's (a branch within the function, not a tail call)
};
static_assert(sizeof(IndirectRec) == 20);

struct JumpTableRec {
    uint32_t site;        // the BR
    uint32_t function;
    uint32_t table, anchor;   // module offsets (anchor 0 for absolute tables)
    uint32_t first_target, count;   // jt_targets[first_target + i] is index i's target: a module offset (a function index for a
                                    // table of function pointers), none32 for an entry never taken (null, LLVM's unreachable label)
    uint8_t module, kind, bound, shift;
};
static_assert(sizeof(JumpTableRec) == 28);

struct CallSiteRec {      // one LSDA call-site record
    uint32_t function;
    uint32_t start, size;     // module offsets of the covered range
    uint32_t landing_pad;     // module offset, 0 = none (unwinding continues)
    uint32_t action;          // 0 = cleanup only / none, else 1 + offset into the action table
    int32_t first_filter;     // type filter of the first action (0 cleanup, > 0 catch, < 0 exception specification)
    uint32_t type_name;       // string: type of the first catch clause ("" for catch (...))
    uint8_t module, reserved[3];
};
static_assert(sizeof(CallSiteRec) == 32);

struct SiteRec {
    uint32_t site;
    uint32_t function;
    uint32_t detail;      // SVC number, system register, UDF immediate, source class
    uint8_t module, kind;
    uint16_t reserved;
};
static_assert(sizeof(SiteRec) == 16);

struct SpinRec {
    uint32_t head, last;  // the loop head and its highest back-edge
    uint32_t function;
    uint32_t flags;
    uint16_t insns;
    uint8_t module, kind;
};
static_assert(sizeof(SpinRec) == 20);

struct SymbolRec {        // every import and export of every module, with its binding the way nxnative binds it
    uint32_t name;
    uint32_t index;       // dynsym index
    uint32_t address;     // provider module offset (guest) or 0
    uint32_t bl_sites, b_sites, blr_sites, data_refs;
    uint32_t forced_in;   // string: the nxnative file that takes it over, or 0
    uint8_t module, flags, binding, provider;   // flags: 1 import, 2 export, 4 function
};
static_assert(sizeof(SymbolRec) == 36);

struct PltStubRec {
    uint32_t stub, got;   // module offsets
    uint32_t symbol;      // symbol record index
    uint32_t callee;      // bound function index, none32 for host
    uint8_t module, binding, reserved[2];
};
static_assert(sizeof(PltStubRec) == 20);

struct Database {
    std::string strings = std::string(1, '\0');
    std::vector<ModuleRec> modules;
    std::vector<FunctionRec> functions;
    std::vector<BlockRec> blocks;
    std::vector<EdgeRec> edges;
    std::vector<CallRec> calls;
    std::vector<IndirectRec> indirect;
    std::vector<JumpTableRec> jump_tables;
    std::vector<uint32_t> jt_targets;
    std::vector<CallSiteRec> call_sites;
    std::vector<SiteRec> sites;
    std::vector<SpinRec> spin_loops;
    std::vector<SymbolRec> symbols;
    std::vector<PltStubRec> plt_stubs;

    uint32_t add_string(std::string_view s);
    const char* str(uint32_t off) const { return off < strings.size() ? strings.c_str() + off : ""; }
};

void write_db(const std::filesystem::path& p, const Database& db);
Database read_db(const std::filesystem::path& p);
std::vector<uint8_t> serialize(const Database& db);
Database deserialize(const std::vector<uint8_t>& bytes);

}  // namespace nxanalyze
