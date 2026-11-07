// nxanalyze <exefs dir> --out <dir> [--nxnative <dir> | --hle <hle_inventory.tsv>] [--traces <run dir>]... [--rewriter <dir>] [--no-defaults]
// Writes program.db, report.json and report.md. Without --no-defaults, run from the repository root it also uses
// ..\installer\nxnative for HLE binding, and _local\mac-traces (runs and rewriter tables) when the trace build IDs match the dump.
// --hle binds from the runtime's registry (nxrt's gen/out/hle_inventory.tsv) instead of nxnative's sources: the installer's Windows build.
// nxanalyze --check-takeovers <program.db> <hle_inventory.tsv>: the takeover check (takeover.cpp); exit 1 with the differences listed
#include "nxanalyze/analyze.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace nxanalyze;

namespace {

std::string id_hex(const ModuleRec& m) {
    std::string s;
    char b[3];
    for (int i = 0; i < 16; i++) std::snprintf(b, sizeof b, "%02x", m.build_id[i]), s += b;
    return s;
}

// The run's modules.tsv names each module's build ID (32 hex digits)
bool trace_matches(const fs::path& run, const nxload::Program& p) {
    std::ifstream f(run / "trace" / "modules.tsv");
    std::string line;
    size_t matched = 0;
    while (std::getline(f, line)) {
        std::vector<std::string> c;
        std::stringstream s(line);
        for (std::string x; std::getline(s, x, '\t');) c.push_back(x);
        if (c.size() < 6 || c[0] == "name") continue;
        const int i = p.index(c[0]);
        if (i < 0 || c[5].size() < 32 || p.modules[size_t(i)].build_id().compare(0, 32, c[5], 0, 32) != 0) return false;
        matched++;
    }
    return matched > 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--check-takeovers") {
            const TakeoverCheck t = check_takeovers(read_db(argv[2]), argv[3]);
            std::printf("takeover check: %zu symbols; program.db forces %zu (%zu names), the registry has %zu forced and %zu swhle entries; "
                        "%zu bound swhle; %zu differences\n", t.symbols, t.db_forced, t.forced_names, t.registry_forced, t.registry_swhle, t.db_swhle,
                        t.differences.size());
            for (const std::string& d : t.differences) std::printf("  %s\n", d.c_str());
            return t.differences.empty() ? 0 : 1;
        }
        fs::path exefs, out;
        Options opt;
        bool defaults = true, have_nxnative = false, have_traces = false, have_rewriter = false;
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) nxw::fail(a + " needs a value");
                return argv[++i];
            };
            if (a == "--out") out = next();
            else if (a == "--nxnative") opt.nxnative = next(), have_nxnative = true;
            else if (a == "--hle") opt.hle_inventory = next(), have_nxnative = true;
            else if (a == "--traces") opt.traces.push_back(next()), have_traces = true;
            else if (a == "--rewriter") opt.rewriter = next(), have_rewriter = true;
            else if (a == "--no-defaults") defaults = false;
            else if (exefs.empty()) exefs = a;
            else nxw::fail("unexpected argument " + a);
        }
        if (exefs.empty() || out.empty()) {
            std::fprintf(stderr, "usage: nxanalyze <exefs dir> --out <dir> [--nxnative <dir> | --hle <hle_inventory.tsv>] [--traces <run dir>]... [--rewriter <dir>] [--no-defaults]\n");
            return 2;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const nxload::Program p = nxload::load_program(exefs);
        if (defaults) {
            if (!have_nxnative && fs::is_directory("../installer/nxnative/dm/host")) opt.nxnative = "../installer/nxnative";
            if (!have_traces && fs::is_directory("_local/mac-traces"))
                for (const auto& e : fs::directory_iterator("_local/mac-traces"))
                    if (e.is_directory() && fs::exists(e.path() / "executed_main.txt") && fs::file_size(e.path() / "executed_main.txt") > 0 &&
                        trace_matches(e.path(), p))
                        opt.traces.push_back(e.path());
            if (!have_rewriter && !opt.traces.empty() && fs::is_directory("_local/mac-traces/rewriter")) opt.rewriter = "_local/mac-traces/rewriter";
        }
        Analysis a = analyze(p, opt);
        fs::create_directories(out);
        write_db(out / "program.db", a.db);
        std::string title = "nxanalyze report: main " + id_hex(a.db.modules[0]);
        write_report_json(out / "report.json", a);
        write_report_md(out / "report.md", a, title);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const Database& db = a.db;
        std::printf("%s\n", title.c_str());
        for (size_t mi = 0; mi < db.modules.size(); mi++) {
            const ModuleRec& m = db.modules[mi];
            size_t unresolved = 0;
            for (const SiteRec& s : db.sites) unresolved += s.module == mi && s.kind == site_unresolved_branch;
            std::printf("  %-8s functions %7u  unresolved direct branches %zu\n", db.str(m.name), m.function_count, unresolved);
        }
        std::printf("  blocks %zu, edges %zu, calls %zu, indirect %zu, jump tables %zu, call sites %zu, sites %zu, spin loops %zu, symbols %zu\n",
                    db.blocks.size(), db.edges.size(), db.calls.size(), db.indirect.size(), db.jump_tables.size(), db.call_sites.size(), db.sites.size(),
                    db.spin_loops.size(), db.symbols.size());
        for (const auto& t : a.trace_checks)
            std::printf("  trace %s %s: %zu executed starts, %zu missing\n", t.run.c_str(), t.module.c_str(), t.executed, t.missing);
        const std::string hle = !opt.hle_inventory.empty() ? "the registry " + opt.hle_inventory.string()
                                : opt.nxnative.empty() ? std::string("not used") : "nxnative " + opt.nxnative.string();
        std::printf("  HLE binding: %s; %.2f s\n", hle.c_str(), secs);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nxanalyze: %s\n", e.what());
        return 1;
    }
}
