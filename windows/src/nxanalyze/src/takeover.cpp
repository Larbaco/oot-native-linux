// The takeover check: program.db's host-bound symbols against the runtime's registry (nxrt's gen/out/hle_inventory.tsv, written by gen_hle.py).
// Both read nxnative's sources by name, in two parsers (this program's hle.cpp and the Python generator); M3 found them disagreeing (dm/gfx was read by
// neither, then by one), which bound main's nv:: calls to the SDK's own graphics boot. The lifter turns a forced binding into a call of the adapter and the
// runtime fills data slots from its registry, so the two sets must be equal for every symbol the program imports or exports.
#include "nxanalyze/analyze.hpp"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace nxanalyze {

TakeoverCheck check_takeovers(const Database& db, const std::filesystem::path& inventory) {
    TakeoverCheck t;
    std::ifstream f(inventory);
    if (!f) throw std::runtime_error("cannot read the runtime's registry inventory " + inventory.string());
    std::map<std::string, std::pair<std::string, std::string>> reg;   // name -> kind, file
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> c;
        std::stringstream s(line);
        for (std::string x; std::getline(s, x, '\t');) c.push_back(x);
        if (c.size() < 3) continue;
        reg.emplace(c[0], std::make_pair(c[1], c[2]));
        t.registry_forced += c[1] == "forced";
        t.registry_swhle += c[1] == "swhle";
    }
    auto kind = [&](const std::string& n) { auto it = reg.find(n); return it == reg.end() ? std::string() : it->second.first; };
    std::set<std::string> seen;
    for (const SymbolRec& y : db.symbols) {
        const std::string name = db.str(y.name), mod = y.module < db.modules.size() ? db.str(db.modules[y.module].name) : "?";
        t.symbols++;
        const std::string k = kind(name);
        const bool db_forced = y.binding == bind_forced, reg_forced = k == "forced";
        if (db_forced) t.db_forced++;
        if (db_forced && reg_forced) { if (seen.insert(name).second) t.forced_names++; continue; }
        if (db_forced != reg_forced) {
            std::string d = mod + " " + name + ": ";
            if (db_forced) d += std::string("program.db binds it to the host (forced in ") + db.str(y.forced_in) + "), the runtime's registry " +
                                (k.empty() ? "has no such entry" : "has it as " + k);
            else d += std::string("program.db binds it ") + binding_name(y.binding) + ", the runtime's registry takes it over (forced in " + reg.at(name).second + ")";
            t.differences.push_back(d);
            continue;
        }
        // Not forced on either side: an import nothing defines goes to the C-library passthroughs (swhle) when the registry has it, else to a named stub
        if (y.binding == bind_swhle || y.binding == bind_stub || y.binding == bind_weak_zero) {
            const bool db_swhle = y.binding == bind_swhle, reg_swhle = k == "swhle";
            t.db_swhle += db_swhle;
            if (db_swhle != reg_swhle)
                t.differences.push_back(mod + " " + name + ": program.db binds it " + binding_name(y.binding) + ", the runtime's registry " +
                                        (reg_swhle ? "implements it (swhle)" : k.empty() ? "has no such entry" : "has it as " + k));
        }
    }
    return t;
}

}  // namespace nxanalyze
