// Loading the dump and program.db, and the indexes the lifter needs.
#include "nxlift/lift.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace nxlift {

using namespace nxanalyze;

Inputs load_inputs(const std::filesystem::path& exefs, const std::filesystem::path& dbp, bool relocate,
                   const std::map<std::string, uint64_t, std::less<>>& bases) {
    Inputs in;
    nxload::LinkOptions lo;
    lo.relocate = relocate;
    in.db = read_db(dbp);
    if (!bases.empty()) {   // program.db must have been made with nxload's default layout, which is checked here first
        const nxload::Program dflt = nxload::load_program(exefs, false, {}, lo);
        for (const ModuleRec& m : in.db.modules) {
            const int i = dflt.index(in.db.str(m.name));
            if (i >= 0 && dflt.bases[size_t(i)] != m.base) nxw::fail(std::string("program.db was made with other module bases: ") + in.db.str(m.name));
        }
        lo.bases = bases;
    }
    in.prog = nxload::load_program(exefs, false, {}, lo);
    for (ModuleRec& m : in.db.modules) {
        const int i = in.prog.index(in.db.str(m.name));
        if (i < 0) nxw::fail(std::string("program.db names a module the dump lacks: ") + in.db.str(m.name));
        if (!bases.empty()) m.base = in.prog.bases[size_t(i)];
        if (in.prog.bases[size_t(i)] != m.base) nxw::fail(std::string("program.db was made with other module bases: ") + in.db.str(m.name));
        const auto& id = in.prog.modules[size_t(i)].nso.module_id;
        if (!std::equal(m.build_id, m.build_id + 20, id.begin())) nxw::fail(std::string("program.db is for another build of ") + in.db.str(m.name));
        in.prog_index.push_back(i);
    }
    in.call_at.reserve(in.db.calls.size());
    for (uint32_t i = 0; i < in.db.calls.size(); i++) in.call_at[uint64_t(in.db.calls[i].module) << 32 | in.db.calls[i].site] = i;
    in.fn_at.reserve(in.db.functions.size());
    for (uint32_t i = 0; i < in.db.functions.size(); i++) in.fn_at[in.fn_address(i)] = i;
    for (uint32_t i = 0; i < in.db.plt_stubs.size(); i++) in.plt_at[uint64_t(in.db.plt_stubs[i].module) << 32 | in.db.plt_stubs[i].stub] = i;
    return in;
}

const uint32_t* Inputs::words(uint32_t f) const {
    const FunctionRec& r = db.functions[f];
    const nxload::Module& m = prog.modules[size_t(prog_index[r.module])];
    return reinterpret_cast<const uint32_t*>(m.image.data() + r.start);
}

std::string Inputs::fn_name(uint32_t f) const {
    char b[32];
    std::snprintf(b, sizeof b, "fn_%llx", static_cast<unsigned long long>(fn_address(f)));
    return b;
}

uint32_t Inputs::find_symbol(std::string_view name) const {
    for (uint32_t i = 0; i < db.functions.size(); i++)
        if (db.str(db.functions[i].name) == name) return i;
    return none32;
}

std::vector<uint32_t> call_closure(const Inputs& in, const std::vector<uint32_t>& fns) {
    std::set<uint32_t> seen(fns.begin(), fns.end());
    std::vector<uint32_t> work(fns.begin(), fns.end()), out;
    // calls per function
    std::unordered_map<uint32_t, std::vector<uint32_t>> callees;
    for (const CallRec& c : in.db.calls)
        if (c.callee != none32) callees[c.function].push_back(c.callee);
    while (!work.empty()) {
        const uint32_t f = work.back();
        work.pop_back();
        out.push_back(f);
        for (uint32_t c : callees[f])
            if (seen.insert(c).second) work.push_back(c);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace nxlift
