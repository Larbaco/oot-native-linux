// The takeover check (src/takeover.cpp): synthetic cases without game data, then the real program.db files under _local\analyze (2.0.1 and 1.0.5)
// against nxrt's generated registry, when they exist. Usage: nxanalyze-takeover-tests <repository root>
#include "check.hpp"
#include "nxanalyze/analyze.hpp"

#include <fstream>

using namespace nxanalyze;
namespace fs = std::filesystem;

namespace {

fs::path g_root;

Database two_modules() {
    Database db;
    for (const char* n : {"main", "sdk"}) {
        ModuleRec m{};
        m.name = db.add_string(n);
        db.modules.push_back(m);
    }
    auto sym = [&](uint8_t mod, const char* name, uint8_t binding, const char* forced_in = nullptr) {
        SymbolRec y{};
        y.name = db.add_string(name), y.module = mod, y.flags = 1, y.binding = binding;
        if (forced_in) y.forced_in = db.add_string(forced_in);
        db.symbols.push_back(y);
    };
    sym(0, "_ZN2nv18InitializeGraphicsEPvm", bind_forced, "hle_nv.c");
    sym(1, "_ZN2nv18InitializeGraphicsEPvm", bind_forced, "hle_nv.c");   // the export in sdk is forced too
    sym(0, "_ZN2nn2os10SleepThreadENS_8TimeSpanE", bind_guest);
    sym(0, "strtol", bind_swhle);
    sym(0, "_ZN2nn5audio12OpenAudioOutEv", bind_guest);                  // the registry forces it: the M3 nv:: case
    sym(0, "nosuchname", bind_stub);
    return db;
}
fs::path inventory(const char* name, const char* rows) {
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f << "# name\tkind\tfile\tline\tstatus\tsignature\n" << rows;
    return p;
}

TEST(takeover_equal_sets) {
    Database db = two_modules();
    db.symbols[4].binding = bind_forced, db.symbols[4].forced_in = db.add_string("hle_audio.c");
    const fs::path inv = inventory("nxw_takeover_eq.tsv",
                                   "_ZN2nv18InitializeGraphicsEPvm\tforced\thle_nv.c\t1\tported\tvoid ()\n"
                                   "_ZN2nn5audio12OpenAudioOutEv\tforced\thle_audio.c\t1\tported\tvoid ()\n"
                                   "strtol\tswhle\thle_table.c\t1\tported\tint64_t ()\n");
    const TakeoverCheck t = check_takeovers(db, inv);
    CHECK_EQ(t.differences.size(), 0u);
    CHECK_EQ(t.db_forced, 3u);
    CHECK_EQ(t.forced_names, 2u);
    CHECK_EQ(t.registry_forced, 2u);
    CHECK_EQ(t.db_swhle, 1u);
    fs::remove(inv);
}

TEST(takeover_differences_listed) {
    const Database db = two_modules();
    // the registry forces OpenAudioOut (program.db: guest), lacks nv:: (program.db: forced), and implements nosuchname (program.db: named stub) but not strtol
    const fs::path inv = inventory("nxw_takeover_diff.tsv",
                                   "_ZN2nn5audio12OpenAudioOutEv\tforced\thle_audio.c\t1\tported\tvoid ()\n"
                                   "nosuchname\tswhle\thle_table.c\t1\tported\tint ()\n");
    const TakeoverCheck t = check_takeovers(db, inv);
    for (const std::string& d : t.differences) std::printf("    %s\n", d.c_str());
    CHECK_EQ(t.differences.size(), 5u);   // nv:: in main and in sdk, OpenAudioOut, strtol, nosuchname
    auto has = [&](const char* s) { for (const std::string& d : t.differences) if (d.find(s) != std::string::npos) return true; return false; };
    CHECK(has("main _ZN2nv18InitializeGraphicsEPvm: program.db binds it to the host (forced in hle_nv.c), the runtime's registry has no such entry"));
    CHECK(has("main _ZN2nn5audio12OpenAudioOutEv: program.db binds it guest, the runtime's registry takes it over (forced in hle_audio.c)"));
    CHECK(has("main strtol: program.db binds it swhle, the runtime's registry has no such entry"));
    CHECK(has("main nosuchname: program.db binds it stub, the runtime's registry implements it (swhle)"));
    fs::remove(inv);
}

TEST(takeover_unreadable_inventory) { CHECK_THROWS(check_takeovers(two_modules(), "no/such/inventory.tsv")); }

// The real thing: every program.db nxanalyze wrote for the game against the registry nxrt is built from
TEST(takeover_game_program_db) {
    const fs::path inv = g_root / "src/nxrt/gen/out/hle_inventory.tsv";
    int checked = 0;
    for (const char* v : {"2.0.1", "1.0.5"}) {
        const fs::path db = g_root / "_local/analyze" / v / "program.db";
        if (!fs::exists(db)) { std::printf("    %s: no program.db at %s (skipped)\n", v, db.string().c_str()); continue; }
        const TakeoverCheck t = check_takeovers(read_db(db), inv);
        std::printf("    %s: %zu symbols, %zu forced bindings (%zu names), %zu swhle, %zu differences\n", v, t.symbols, t.db_forced, t.forced_names, t.db_swhle,
                    t.differences.size());
        for (const std::string& d : t.differences) std::printf("      %s\n", d.c_str());
        CHECK_EQ(t.differences.size(), 0u);
        checked++;
    }
    if (!checked) std::printf("    no game program.db: only the synthetic cases ran\n");
}

}  // namespace

int main(int argc, char** argv) {
    g_root = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    return check::run();
}
