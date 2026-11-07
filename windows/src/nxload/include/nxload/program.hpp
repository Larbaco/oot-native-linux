// The program: modules placed at their bases, symbols bound the way Horizon's rtld binds them, relocated images.
//
// Load order is main, subsdk0..subsdk9, sdk (rtld, when loaded, goes last and takes no part in lookups). Lookup scope is that order:
// an undefined symbol, or a global/weak definition with default visibility, binds to the first module in scope that exports the name
// (with default or protected visibility), so an earlier module's definition overrides a later module's own. Local, hidden and
// protected definitions bind to their own module; an undefined weak symbol nothing exports binds to 0.
#pragma once
#include "nxload/module.hpp"

namespace nxload {

inline constexpr uint64_t default_base = 0x71'0000'0000;

struct LinkOptions {
    uint64_t base = default_base;   // of the first module
    std::map<std::string, uint64_t, std::less<>> bases;   // fixed bases by module name; the others follow the previous module, page-aligned
    bool relocate = true;           // build the relocated images
};

enum class Bound : uint8_t { none, self, other, weak_zero, unresolved };
std::string bound_name(Bound b);

struct Resolution {
    Bound kind = Bound::none;
    int provider = -1;     // module index
    uint32_t sym = 0;      // symbol index in the provider
    uint64_t address = 0;  // absolute
};

struct RelocStats {
    std::map<uint32_t, size_t> applied;   // RELA and JMPREL by type
    size_t relr = 0;
    size_t sym_self = 0, sym_other = 0, sym_weak_zero = 0, sym_unresolved = 0;   // symbol relocations by binding
};

struct Program {
    std::vector<Module> modules;
    std::vector<uint64_t> bases;
    std::vector<bool> in_scope;
    std::vector<std::vector<Resolution>> resolution;   // [module][symbol index]
    std::vector<std::vector<uint8_t>> memory;          // relocated images (empty unless LinkOptions::relocate)
    std::vector<RelocStats> stats;
    std::vector<std::string> problems;                 // unresolved non-weak symbols, unsupported relocations

    int index(std::string_view name) const;            // -1 when absent
    int module_at(uint64_t addr) const;                // -1 outside every module
    uint64_t end() const;
    // The first module in scope that exports the name, and the symbol's index there
    std::optional<std::pair<int, uint32_t>> find(std::string_view name) const;
    // A relocated word of a module's image (requires the relocated images)
    uint64_t read64(int module, uint64_t offset) const;
};

// The standard load order of the modules present in an ExeFS folder: main, subsdk0..9, sdk, then rtld when with_rtld
std::vector<std::string> exefs_modules(const std::filesystem::path& dir, bool with_rtld);

Program link(std::vector<Module> modules, const LinkOptions& opt = {});
Program load_program(const std::filesystem::path& exefs, bool with_rtld = false, const LoadOptions& lo = {}, const LinkOptions& opt = {});

}  // namespace nxload
