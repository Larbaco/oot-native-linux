// nxlift <exefs dir> --db <program.db> --out <dir> [options]: lifts program.db's functions to x86-64 COFF objects, one per
// partition of about 1,000 functions, and writes a report (report.md, partitions.tsv, unsupported.tsv). Everything it writes
// is derived from the game, so --out belongs under _local\.
//   --module NAME      only this module (repeatable; default all)
//   --fn ADDR          only this function, by absolute address (hex, repeatable); --sym NAME by symbol; --count N the first N
//   --closure          add every guest function the selected ones call directly, transitively
//   --O1 / --O2        optimisation level (default O2)
//   --ll               also write each partition's optimised IR (.ll); --ll-pre: the IR before optimisation (.pre.ll);
//                      --no-obj: no objects
//   --jobs N           worker threads, at most 4 (default 4); --partition N functions per partition (default 1000)
//   --priority normal  run at normal priority (default: idle, like build.cmd's start /low)
//   --dis ADDR         print a function's instructions and exit
//   --cache DIR        object cache keyed by each partition's IR before optimisation (default <out>\..\objcache); --no-cache
//   --no-trace-hooks   no function-entry trace hook (rt_trace_entry while the runtime's rt_trace_on is set)
//   --dll FILE         also write nxw_game.obj (the nxw_game_* exports) and link every object into FILE with lld-link
//   --nxrt-lib FILE    the runtime's import library for --dll (default: lib\nxrt.lib beside this tool's bin folder)
//   --lld FILE         lld-link (default: from PATH, else C:\Program Files\LLVM\bin\lld-link.exe)
// The Mac swap-in harness (docs/m4-swapin.md; nxlift built for arm64-apple-macos writes Mach-O .o files):
//   --swapin           direct calls and tail calls through the D4 lookup (Options::swapin), and nxw_swapin_table.o
//   --base-of M=ADDR   module M at ADDR (hex) instead of program.db's layout (repeatable)
//   --fn-list FILE     only these functions: lines "<module> <offset hex>" or "<absolute hex>" (# comments; repeatable)
#include "nxlift/lift.hpp"

#include "nxw/a64.hpp"

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Threading.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/thread.h>
#include <llvm/Support/xxhash.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>

namespace fs = std::filesystem;
using namespace nxlift;
using nxanalyze::none32;

namespace {

using Clock = std::chrono::steady_clock;
double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct Partition {
    uint32_t module = 0, index = 0;
    std::vector<uint32_t> fns;
    uint64_t instructions = 0;
    // results
    double lift_ms = 0, opt_ms = 0, codegen_ms = 0;
    uint64_t obj_bytes = 0;
    bool done = false, cached = false;
    std::string error;
};

uint64_t parse_hex(const std::string& s) { return std::stoull(s, nullptr, 16); }

// Bumped whenever optimisation or code generation changes in a way the IR does not show
constexpr const char* k_cache_version = "nxlift-objcache-1";

// The cache key: the partition's IR after finish() (semantics inlined, before optimisation), the level and the version
std::string cache_key(const llvm::Module& m, int level) {
    llvm::SmallVector<char, 0> bc;
    llvm::raw_svector_ostream os(bc);
    llvm::WriteBitcodeToFile(m, os);
    const std::string salt = std::string(k_cache_version) + "/O" + std::to_string(level);
    bc.append(salt.begin(), salt.end());
    const llvm::XXH128_hash_t h = llvm::xxh3_128bits(reinterpret_cast<const uint8_t*>(bc.data()), bc.size());
    char s[40];
    std::snprintf(s, sizeof s, "%016llx%016llx", static_cast<unsigned long long>(h.high64), static_cast<unsigned long long>(h.low64));
    return s;
}

// The output object as a hard link to the cached one (a copy where linking fails); never written through
void place_object(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::remove(to, ec);
    fs::create_hard_link(from, to, ec);
    if (ec) fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

#ifdef _WIN32
// Runs a command line at idle priority and waits; the exit code
int run_idle(const std::string& cmd) {
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::string c = cmd;
    if (!CreateProcessA(nullptr, c.data(), nullptr, nullptr, FALSE, IDLE_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(rc);
}

fs::path find_lld() {
    char p[MAX_PATH];
    if (SearchPathA(nullptr, "lld-link.exe", nullptr, MAX_PATH, p, nullptr)) return p;
    return "C:\\Program Files\\LLVM\\bin\\lld-link.exe";
}
#else
// Runs a program at the lowest priority and waits; the exit code
int run_idle(const std::vector<std::string>& argv) {
    std::vector<char*> a;
    for (const std::string& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, a[0], nullptr, &at, a.data(), environ);
    posix_spawnattr_destroy(&at);
    if (rc) return 127;
    setpriority(PRIO_PROCESS, id_t(pid), 19);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
#endif

// --fn-list: "<module> <offset>" or "<absolute address>" per line, hex; # starts a comment; other columns are ignored
void read_fn_list(const Inputs& in, const fs::path& p, std::vector<uint32_t>& sel) {
    std::ifstream f(p);
    if (!f) nxw::fail("cannot read " + p.string());
    std::string line;
    size_t missing = 0;
    while (std::getline(f, line)) {
        if (const size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        char a[64] = {}, b[64] = {};
        const int n = std::sscanf(line.c_str(), "%63s %63s", a, b);
        if (n < 1) continue;
        uint64_t addr = 0;
        bool module_named = false;
        for (uint32_t m = 0; m < in.db.modules.size() && n == 2; m++)
            if (std::string_view(in.module_name(m)) == a) addr = in.db.modules[m].base + parse_hex(b), module_named = true;
        if (!module_named) addr = parse_hex(a);
        auto it = in.fn_at.find(addr);
        if (it == in.fn_at.end()) missing++;
        else sel.push_back(it->second);
    }
    if (missing) std::printf("nxlift: %s: %zu lines name no function start\n", p.string().c_str(), missing);
}

void disassemble(const Inputs& in, uint32_t f) {
    const auto& r = in.db.functions[f];
    std::printf("%s %s (%s+0x%x, %u bytes, %u blocks)\n", in.fn_name(f).c_str(), in.db.str(r.name), in.module_name(r.module), r.start,
                r.size, r.block_count);
    const uint32_t* w = in.words(f);
    const uint64_t a = in.fn_address(f);
    for (uint32_t i = 0; i < r.size / 4; i++)
        std::printf("  %llx: %08x  %s\n", static_cast<unsigned long long>(a + 4 * i), w[i], nxw::a64::print(w[i], a + 4 * i).c_str());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        fs::path exefs, dbp, out, cache, dll, nxrt_lib, lld;
        Options opt;
        opt.entry_trace = true;
        std::set<std::string> modules;
        std::vector<uint64_t> fn_addrs;
        std::vector<std::string> syms;
        std::vector<fs::path> fn_lists;
        std::map<std::string, uint64_t, std::less<>> bases;
        uint64_t count = 0, dis = 0;
        bool closure = false, ll = false, ll_pre = false, obj = true, low = true, use_cache = true;
        unsigned jobs = 4, part_size = 1000;
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) nxw::fail(a + " needs a value");
                return argv[++i];
            };
            if (a == "--db") dbp = next();
            else if (a == "--out") out = next();
            else if (a == "--module") modules.insert(next());
            else if (a == "--fn") fn_addrs.push_back(parse_hex(next()));
            else if (a == "--sym") syms.push_back(next());
            else if (a == "--count") count = std::stoull(next());
            else if (a == "--closure") closure = true;
            else if (a == "--O1") opt.opt = 1;
            else if (a == "--O2") opt.opt = 2;
            else if (a == "--ll") ll = true;
            else if (a == "--ll-pre") ll_pre = true;
            else if (a == "--no-obj") obj = false;
            else if (a == "--jobs") jobs = unsigned(std::stoul(next()));
            else if (a == "--partition") part_size = unsigned(std::stoul(next()));
            else if (a == "--priority") low = next() != "normal";
            else if (a == "--dis") dis = parse_hex(next());
            else if (a == "--cache") cache = next();
            else if (a == "--no-cache") use_cache = false;
            else if (a == "--no-trace-hooks") opt.entry_trace = false;
            else if (a == "--dll") dll = next();
            else if (a == "--nxrt-lib") nxrt_lib = next();
            else if (a == "--lld") lld = next();
            else if (a == "--swapin") opt.swapin = true, opt.dllimport = false;
            else if (a == "--fn-list") fn_lists.push_back(next());
            else if (a == "--base-of") {
                const std::string v = next();
                const size_t eq = v.find('=');
                if (eq == std::string::npos) nxw::fail("--base-of needs MODULE=ADDR");
                bases[v.substr(0, eq)] = parse_hex(v.substr(eq + 1));
            }
            else if (exefs.empty()) exefs = a;
            else nxw::fail("unexpected argument " + a);
        }
        if (exefs.empty() || dbp.empty() || (out.empty() && !dis)) {
            std::fprintf(stderr, "usage: nxlift <exefs dir> --db <program.db> --out <dir> [options] (see the source header)\n");
            return 2;
        }
#ifdef _WIN32
        if (jobs > 4) std::printf("nxlift: at most 4 jobs on this PC; using 4\n"), jobs = 4;
        if (low) SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
#else
        if (low) setpriority(PRIO_PROCESS, 0, 19);
#endif
        if (jobs == 0) jobs = 1;

        const auto t0 = Clock::now();
        const Inputs in = load_inputs(exefs, dbp, false, bases);
        std::printf("nxlift: %zu functions in %zu modules, loaded in %.1f s\n", in.db.functions.size(), in.db.modules.size(),
                    ms(Clock::now() - t0) / 1000);
        if (dis) {
            auto it = in.fn_at.find(dis);
            if (it == in.fn_at.end()) nxw::fail("no function starts at that address");
            disassemble(in, it->second);
            return 0;
        }

        // selection
        std::vector<uint32_t> sel;
        for (uint64_t a : fn_addrs) {
            auto it = in.fn_at.find(a);
            if (it == in.fn_at.end()) nxw::fail("no function starts at " + std::to_string(a));
            sel.push_back(it->second);
        }
        for (const std::string& s : syms) {
            const uint32_t f = in.find_symbol(s);
            if (f == none32) nxw::fail("no function named " + s);
            sel.push_back(f);
        }
        for (const fs::path& p : fn_lists) read_fn_list(in, p, sel);
        if (sel.empty())
            for (uint32_t f = 0; f < in.db.functions.size(); f++)
                if (modules.empty() || modules.count(in.module_name(in.db.functions[f].module))) sel.push_back(f);
        if (closure) sel = call_closure(in, sel);
        std::sort(sel.begin(), sel.end());
        sel.erase(std::unique(sel.begin(), sel.end()), sel.end());
        if (count && sel.size() > count) sel.resize(count);

        // partitions: consecutive functions of one module; a function over 16 KB gets its own
        std::vector<Partition> parts;
        std::vector<uint32_t> per_module(in.db.modules.size(), 0);
        for (uint32_t f : sel) {
            const auto& r = in.db.functions[f];
            const bool big = r.size > 16384;
            if (parts.empty() || parts.back().module != r.module || parts.back().fns.size() >= part_size || big ||
                (in.db.functions[parts.back().fns.back()].size > 16384)) {
                parts.push_back({});
                parts.back().module = r.module;
                parts.back().index = per_module[r.module]++;
            }
            parts.back().fns.push_back(f);
            parts.back().instructions += r.size / 4;
        }
        std::printf("nxlift: %zu functions in %zu partitions, O%d, %u jobs%s\n", sel.size(), parts.size(), opt.opt, jobs,
                    opt.swapin ? ", swap-in" : "");
#ifdef _WIN32
        const std::string obj_ext = ".obj";
#else
        const std::string obj_ext = ".o";   // Mach-O (the swap-in harness)
#endif
        fs::create_directories(out);
        for (uint32_t m = 0; m < in.db.modules.size(); m++)
            if (per_module[m]) fs::create_directories(out / in.module_name(m));
        if (cache.empty()) cache = fs::absolute(out).parent_path() / "objcache";
        if (use_cache && obj) fs::create_directories(cache);
        use_cache = use_cache && obj;

        std::atomic<size_t> next{0};
        std::atomic<size_t> finished{0};
        std::mutex mu;
        LiftStats total;
        auto worker = [&] {
            llvm::LLVMContext ctx;
            Lifter L(in, ctx, opt);
            for (size_t i; (i = next++) < parts.size();) {
                Partition& p = parts[i];
                char name[64];
                std::snprintf(name, sizeof name, "%s/p%04u", in.module_name(p.module), p.index);
                try {
                    const auto a = Clock::now();
                    auto M = L.new_module(name);
                    for (uint32_t f : p.fns) L.lift(*M, f);
                    L.finish(*M);
                    if (ll_pre) {
                        std::error_code ec;
                        llvm::raw_fd_ostream os((out / (std::string(name) + ".pre.ll")).string(), ec);
                        M->print(os, nullptr);
                    }
                    const auto b = Clock::now();
                    p.lift_ms = ms(b - a);
                    const fs::path objp = out / (std::string(name) + obj_ext);
                    const fs::path cached = use_cache ? cache / (cache_key(*M, opt.opt) + obj_ext) : fs::path();
                    if (use_cache && !ll && fs::exists(cached)) {   // same IR, same object: no optimisation or code generation
                        place_object(cached, objp);
                        p.obj_bytes = fs::file_size(cached);
                        p.done = p.cached = true;
                    } else {
                        L.optimize(*M);
                        const auto c = Clock::now();
                        if (ll) {
                            std::error_code ec;
                            llvm::raw_fd_ostream os((out / (std::string(name) + ".ll")).string(), ec);
                            M->print(os, nullptr);
                        }
                        if (obj) {
                            const std::vector<char> o = L.emit_object(*M);
                            const fs::path w = use_cache ? fs::path(cached.string() + ".tmp") : objp;
                            {
                                std::ofstream f(w, std::ios::binary);
                                f.write(o.data(), std::streamsize(o.size()));
                                if (!f) nxw::fail("cannot write " + w.string());
                            }
                            if (use_cache) {
                                fs::rename(w, cached);
                                place_object(cached, objp);
                            }
                            p.obj_bytes = o.size();
                        }
                        p.opt_ms = ms(c - b), p.codegen_ms = ms(Clock::now() - c);
                        p.done = true;
                    }
                } catch (const std::exception& e) {
                    p.error = e.what();
                }
                const size_t n = ++finished;
                std::lock_guard<std::mutex> g(mu);
                std::printf("[%zu/%zu] %s: %zu functions, %llu insns, lift %.0f ms, opt %.0f ms, codegen %.0f ms, %llu KB%s%s%s\n", n,
                            parts.size(), name, p.fns.size(), static_cast<unsigned long long>(p.instructions), p.lift_ms, p.opt_ms,
                            p.codegen_ms, static_cast<unsigned long long>(p.obj_bytes / 1024), p.cached ? " (cached)" : "",
                            p.error.empty() ? "" : "  ERROR: ", p.error.c_str());
                std::fflush(stdout);
            }
            std::lock_guard<std::mutex> g(mu);
            total.add(L.stats);
        };
        std::vector<llvm::thread> threads;
        for (unsigned j = 0; j < jobs; j++) threads.emplace_back(std::optional<unsigned>(64u << 20), worker);
        for (auto& t : threads) t.join();
        const double wall = ms(Clock::now() - t0) / 1000;

        // report
        std::ofstream tsv(out / "partitions.tsv");
        tsv << "module\tpartition\tfunctions\tinstructions\tlift_ms\topt_ms\tcodegen_ms\tobj_bytes\tcached\terror\n";
        struct Sum {
            uint64_t parts = 0, fns = 0, insns = 0, bytes = 0, failed = 0, cached = 0;
            double lift = 0, opt = 0, cg = 0;
        };
        std::map<std::string, Sum> sums;
        for (const Partition& p : parts) {
            tsv << in.module_name(p.module) << '\t' << p.index << '\t' << p.fns.size() << '\t' << p.instructions << '\t' << p.lift_ms
                << '\t' << p.opt_ms << '\t' << p.codegen_ms << '\t' << p.obj_bytes << '\t' << p.cached << '\t' << p.error << '\n';
            Sum& s = sums[in.module_name(p.module)];
            s.parts++, s.fns += p.fns.size(), s.insns += p.instructions, s.bytes += p.obj_bytes, s.failed += !p.done;
            s.cached += p.cached, s.lift += p.lift_ms, s.opt += p.opt_ms, s.cg += p.codegen_ms;
        }
        std::ofstream un(out / "unsupported.tsv");
        un << "op\tinstructions\n";
        for (auto& [op, n] : total.unsupported) un << op << '\t' << n << '\n';
        std::ofstream md(out / "report.md");
        md << "# nxlift run\n\nO" << opt.opt << ", " << jobs << " jobs" << (low ? " at idle priority" : "") << ", wall " << wall
           << " s.\n\n| module | partitions | from the cache | functions | instructions | lift s | opt s | codegen s | objects MB | failed |\n"
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        for (auto& [m, s] : sums) {
            char line[256];
            std::snprintf(line, sizeof line, "| %s | %llu | %llu | %llu | %llu | %.1f | %.1f | %.1f | %.1f | %llu |\n", m.c_str(),
                          static_cast<unsigned long long>(s.parts), static_cast<unsigned long long>(s.cached),
                          static_cast<unsigned long long>(s.fns), static_cast<unsigned long long>(s.insns), s.lift / 1000, s.opt / 1000,
                          s.cg / 1000, s.bytes / 1048576.0, static_cast<unsigned long long>(s.failed));
            md << line;
        }
        md << "\nLifted: " << total.functions << " functions, " << total.instructions << " instructions, " << total.blocks
           << " blocks; direct calls " << total.calls_guest << " guest + " << total.calls_host << " host; indirect calls "
           << total.calls_indirect << "; tail calls " << total.tails << " direct + " << total.tails_indirect << " indirect; "
           << total.jump_tables << " jump tables; " << total.svcs << " SVCs; " << total.sysreg_calls << " runtime MRS/MSR; "
           << total.yields << " yields; " << total.contexts << " context functions (rt_nonlocal); " << total.traps
           << " trap sites.\n\n## Instructions without semantics\n\n";
        if (total.unsupported.empty()) md << "None.\n";
        for (auto& [op, n] : total.unsupported) md << "- " << op << ": " << n << "\n";
        std::printf("nxlift: %llu functions, %llu instructions, %zu ops without semantics, %llu yields, %llu context functions, %.1f s\n",
                    static_cast<unsigned long long>(total.functions), static_cast<unsigned long long>(total.instructions),
                    total.unsupported.size(), static_cast<unsigned long long>(total.yields),
                    static_cast<unsigned long long>(total.contexts), wall);
        size_t failed = 0;
        for (const Partition& p : parts) failed += !p.done;
        if (failed) return 1;

        // The swap-in dylib's table (src/swapin links it with the partitions' objects and the runtime)
        if (opt.swapin && obj) {
            std::vector<uint32_t> all;
            for (const Partition& p : parts) all.insert(all.end(), p.fns.begin(), p.fns.end());
            llvm::LLVMContext ctx;
            Lifter L(in, ctx, opt);
            const std::vector<char> o = swapin_table_object(L, in, all);
            std::ofstream f(out / ("nxw_swapin_table" + obj_ext), std::ios::binary);
            f.write(o.data(), std::streamsize(o.size()));
            std::printf("nxlift: nxw_swapin_table%s: %zu functions\n", obj_ext.c_str(), all.size());
        }

#ifdef _WIN32
        // The game DLL: the nxw_game_* object, then every partition's object and the runtime's import library
        if (!dll.empty()) {
            const auto l0 = Clock::now();
            std::vector<uint32_t> all;
            for (const Partition& p : parts) all.insert(all.end(), p.fns.begin(), p.fns.end());
            {
                llvm::LLVMContext ctx;
                Lifter L(in, ctx, opt);
                const std::vector<char> o = game_table_object(L, in, all);
                std::ofstream f(out / "nxw_game.obj", std::ios::binary);
                f.write(o.data(), std::streamsize(o.size()));
            }
            if (nxrt_lib.empty()) {
                char self[MAX_PATH];
                GetModuleFileNameA(nullptr, self, MAX_PATH);
                nxrt_lib = fs::path(self).parent_path().parent_path() / "lib" / "nxrt.lib";
            }
            if (!fs::exists(nxrt_lib)) nxw::fail("no runtime import library at " + nxrt_lib.string() + " (--nxrt-lib)");
            if (lld.empty()) lld = find_lld();
            const fs::path rsp = out / "link.rsp";
            {
                std::ofstream r(rsp);
                r << "/DLL\n/NOENTRY\n/NODEFAULTLIB\n/MACHINE:X64\n/OPT:REF\n/OPT:NOICF\n/INCREMENTAL:NO\n/HIGHENTROPYVA:NO\n";
                r << "\"/OUT:" << fs::absolute(dll).string() << "\"\n";
                for (const Partition& p : parts) {
                    char name[64];
                    std::snprintf(name, sizeof name, "%s/p%04u.obj", in.module_name(p.module), p.index);
                    r << '"' << fs::absolute(out / name).string() << "\"\n";
                }
                r << '"' << fs::absolute(out / "nxw_game.obj").string() << "\"\n";
                r << '"' << fs::absolute(nxrt_lib).string() << "\"\n";
            }
            std::printf("nxlift: linking %s with %s\n", dll.string().c_str(), lld.string().c_str());
            std::fflush(stdout);
            const int rc = run_idle("\"" + lld.string() + "\" @\"" + fs::absolute(rsp).string() + "\"");
            if (rc) nxw::fail("lld-link failed with exit code " + std::to_string(rc));
            std::printf("nxlift: %s: %zu functions, %.1f MB, linked in %.1f s\n", dll.string().c_str(), all.size(),
                        fs::file_size(dll) / 1048576.0, ms(Clock::now() - l0) / 1000);
        }
#else
        // The game as a shared object: the nxw_game_* object and every partition's. The rt_* symbols stay undefined here and bind to the runtime executable when it
        // loads the object; the C library's functions the code generator calls come from the process too. -z now: everything binds at load, nothing lazily in play
        if (!dll.empty()) {
            const auto l0 = Clock::now();
            std::vector<uint32_t> all;
            for (const Partition& p : parts) all.insert(all.end(), p.fns.begin(), p.fns.end());
            {
                llvm::LLVMContext ctx;
                Lifter L(in, ctx, opt);
                const std::vector<char> o = game_table_object(L, in, all);
                std::ofstream f(out / "nxw_game.o", std::ios::binary);
                f.write(o.data(), std::streamsize(o.size()));
            }
            if (lld.empty()) lld = "ld.lld";
            const fs::path rsp = out / "link.rsp";
            {
                std::ofstream r(rsp);
                r << "-shared\n-nostdlib\n-z\nnow\n-z\nrelro\n-z\nnoexecstack\n--hash-style=gnu\n--build-id=none\n--gc-sections\n--icf=none\n";
                r << "-o\n\"" << fs::absolute(dll).string() << "\"\n";
                for (const Partition& p : parts) {
                    char name[64];
                    std::snprintf(name, sizeof name, "%s/p%04u.o", in.module_name(p.module), p.index);
                    r << '"' << fs::absolute(out / name).string() << "\"\n";
                }
                r << '"' << fs::absolute(out / "nxw_game.o").string() << "\"\n";
            }
            std::printf("nxlift: linking %s with %s\n", dll.string().c_str(), lld.string().c_str());
            std::fflush(stdout);
            const int rc = run_idle({lld.string(), "@" + fs::absolute(rsp).string()});
            if (rc) nxw::fail("ld.lld failed with exit code " + std::to_string(rc));
            std::printf("nxlift: %s: %zu functions, %.1f MB, linked in %.1f s\n", dll.string().c_str(), all.size(),
                        fs::file_size(dll) / 1048576.0, ms(Clock::now() - l0) / 1000);
        }
        (void)nxrt_lib;
#endif
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nxlift: %s\n", e.what());
        return 1;
    }
}
