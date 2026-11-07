// nxnative's by-name HLE (MIT, ours): the forced takeovers (HLE_FORCE, HLE_FORCE_BIG and the macros built on them), the swhle
// table (HLE and hle_table.c's libc passthroughs), and the NVN names behind nvnBootstrapLoader. Binding follows nxnative's loader:
// forced name, else the modules in load order, else an undefined weak symbol is 0, else swhle, else a named stub.
#include "internal.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>

namespace nxanalyze {

using namespace nxload;
namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// Comments out, string and character literals kept; then #if 0 blocks out
std::string strip(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "//") == 0) {
            while (i < s.size() && s[i] != '\n') i++;
        } else if (s.compare(i, 2, "/*") == 0) {
            const size_t e = s.find("*/", i + 2);
            for (size_t k = i; k < std::min(e == std::string::npos ? s.size() : e + 2, s.size()); k++)
                if (s[k] == '\n') o += '\n';
            i = e == std::string::npos ? s.size() : e + 2;
        } else if (s[i] == '"' || s[i] == '\'') {
            const char q = s[i];
            o += s[i++];
            while (i < s.size() && s[i] != q) {
                if (s[i] == '\\' && i + 1 < s.size()) o += s[i++];
                o += s[i++];
            }
            if (i < s.size()) o += s[i++];
        } else {
            o += s[i++];
        }
    }
    std::istringstream in(o);
    std::string line, out;
    int skip = 0;
    while (std::getline(in, line)) {
        std::string t = line;
        t.erase(0, t.find_first_not_of(" \t"));
        if (skip) {
            if (t.starts_with("#if")) skip++;
            else if (t.starts_with("#endif")) skip--;
            out += '\n';
            continue;
        }
        if (t.starts_with("#if 0")) { skip = 1; out += '\n'; continue; }
        out += line;
        out += '\n';
    }
    return out;
}

bool ident_char(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

// A #define continued over several lines is one line
std::string join_continuations(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\n') { o += "  "; i++; continue; }
        if (s[i] == '\\' && i + 2 < s.size() && s[i + 1] == '\r' && s[i + 2] == '\n') { o += "   "; i += 2; continue; }
        o += s[i];
    }
    return o;
}

// A file-local macro that builds its HLE_FORCE names with # (hle_hid.c's TOUCH(N)), expanded at each use: every name its expansion forces
void expand_name_macros(const std::string& s, const std::function<void(const std::string&)>& f) {
    for (size_t at = s.find("#define"); at != std::string::npos; at = s.find("#define", at + 1)) {
        const size_t eol = s.find('\n', at);
        const std::string line = s.substr(at, eol - at);
        size_t n0 = line.find_first_not_of(" \t", 7), n1 = n0;
        if (n0 == std::string::npos) continue;
        while (n1 < line.size() && ident_char(line[n1])) n1++;
        if (n1 >= line.size() || line[n1] != '(') continue;
        const size_t pe = line.find(')', n1);
        if (pe == std::string::npos) continue;
        const std::string name = line.substr(n0, n1 - n0), param = line.substr(n1 + 1, pe - n1 - 1), body = line.substr(pe + 1);
        if (param.empty() || !std::all_of(param.begin(), param.end(), ident_char) || body.find("HLE_FORCE") == std::string::npos ||
            body.find("#" + param) == std::string::npos)
            continue;
        for (size_t u = s.find(name + "("); u != std::string::npos; u = s.find(name + "(", u + 1)) {
            if ((u && ident_char(s[u - 1])) || (u >= at && u < eol)) continue;
            const size_t ae = s.find(')', u);
            std::string arg = s.substr(u + name.size() + 1, ae - u - name.size() - 1);
            arg.erase(0, arg.find_first_not_of(" \t"));
            arg.erase(arg.find_last_not_of(" \t") + 1);
            std::string e;   // the body with #param stringified and param replaced
            for (size_t k = 0; k < body.size();) {
                if (body.compare(k, param.size() + 1, "#" + param) == 0 && (k + param.size() + 1 >= body.size() || !ident_char(body[k + param.size() + 1]))) {
                    e += '"' + arg + '"', k += param.size() + 1;
                } else if (body.compare(k, param.size(), param) == 0 && (!k || !ident_char(body[k - 1])) &&
                           (k + param.size() >= body.size() || !ident_char(body[k + param.size()]))) {
                    e += arg, k += param.size();
                } else {
                    e += body[k++];
                }
            }
            // HLE_FORCE( followed by adjacent literals: their concatenation is the name
            for (size_t h = e.find("HLE_FORCE"); h != std::string::npos; h = e.find("HLE_FORCE", h + 1)) {
                size_t p = e.find('(', h);
                if (p == std::string::npos) break;
                std::string n;
                for (p++;;) {
                    while (p < e.size() && std::isspace(static_cast<unsigned char>(e[p]))) p++;
                    if (p >= e.size() || e[p] != '"') break;
                    const size_t q = e.find('"', p + 1);
                    n += e.substr(p + 1, q - p - 1);
                    p = q + 1;
                }
                if (!n.empty()) f(n);
            }
        }
    }
}

// Every NAME("literal" invocation of one of the macros, outside #define lines
void scan_calls(const std::string& s, const std::unordered_set<std::string>& macros, const std::function<void(const std::string&, const std::string&)>& f) {
    for (size_t i = 0; i < s.size(); i++) {
        if (!ident_char(s[i]) || (i && ident_char(s[i - 1]))) continue;
        size_t e = i;
        while (e < s.size() && ident_char(s[e])) e++;
        const std::string id = s.substr(i, e - i);
        i = e - 1;
        if (!macros.count(id)) continue;
        size_t p = e;
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
        if (p >= s.size() || s[p] != '(') continue;
        p++;
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) p++;
        if (p >= s.size() || s[p] != '"') continue;
        const size_t q = s.find('"', p + 1);
        if (q == std::string::npos) continue;
        const size_t ls = s.rfind('\n', i);
        std::string head = s.substr(ls == std::string::npos ? 0 : ls + 1, i - (ls == std::string::npos ? 0 : ls + 1));
        head.erase(0, head.find_first_not_of(" \t"));
        if (head.starts_with("#define")) continue;
        f(id, s.substr(p + 1, q - p - 1));
    }
}

}  // namespace

Hle load_hle(const fs::path& root) {
    Hle h;
    std::vector<fs::path> files;
    for (const char* sub : {"dm/host", "dm/gfx", "dm/gfx/nvn"}) {   // dm/gfx: hle_nv.c and hle_vi.c (nv:: and vi:: take-overs)
        const fs::path d = root / sub;
        if (!fs::is_directory(d)) nxw::fail("nxnative: no folder " + d.string());
        for (const auto& e : fs::directory_iterator(d))
            if (e.is_regular_file() && e.path().extension() == ".c") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
        const std::string s = join_continuations(strip(read_text(p)));
        std::unordered_set<std::string> macros{"HLE_FORCE", "HLE_FORCE_BIG"};
        for (size_t at = s.find("#define"); at != std::string::npos; at = s.find("#define", at + 1)) {
            const size_t eol = s.find('\n', at);
            const std::string line = s.substr(at, eol - at);
            const size_t n0 = line.find_first_not_of(" \t", 7);
            if (n0 == std::string::npos) continue;
            size_t n1 = n0;
            while (n1 < line.size() && ident_char(line[n1])) n1++;
            if (n1 >= line.size() || line[n1] != '(') continue;
            const size_t body = line.find(')', n1);
            if (body == std::string::npos) continue;
            const size_t b0 = line.find_first_not_of(" \t", body + 1);
            if (b0 != std::string::npos && (line.compare(b0, 10, "HLE_FORCE(") == 0 || line.compare(b0, 14, "HLE_FORCE_BIG(") == 0))
                macros.insert(line.substr(n0, n1 - n0));
        }
        const std::string file = p.filename().string();
        size_t forced = 0;
        scan_calls(s, macros, [&](const std::string&, const std::string& name) {
            h.forced_entries++, forced++;
            h.forced.emplace(name, file);
        });
        expand_name_macros(s, [&](const std::string& name) {
            h.forced_entries++, forced++;
            h.forced.emplace(name, file);
        });
        scan_calls(s, {"HLE"}, [&](const std::string&, const std::string& name) { h.swhle_entries++, h.swhle.insert(name); });
        if (file == "hle_table.c")
            for (size_t at = s.find("L("); at != std::string::npos; at = s.find("L(", at + 2)) {
                if (at && ident_char(s[at - 1])) continue;
                const size_t e = s.find(')', at);
                const std::string name = s.substr(at + 2, e - at - 2);
                if (!name.empty() && std::all_of(name.begin(), name.end(), ident_char)) h.swhle_entries++, h.swhle.insert(name);
            }
        if (forced) h.files++;
    }
    std::ifstream names(root / "dm/gfx/nvn/names.txt");
    for (std::string line; std::getline(names, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) h.nvn.push_back(line);
    }
    h.loaded = true;
    return h;
}

// The same sets from the runtime's registry (gen_hle.py's hle_inventory.tsv: name, kind, file, ...): program.db then forces exactly what the nxrt.exe
// built from that registry takes over, and the player's PC needs no nxnative tree for the analysis (the installer's Windows build)
Hle load_hle_inventory(const fs::path& tsv) {
    std::ifstream f(tsv);
    if (!f) nxw::fail("cannot read the runtime's registry inventory " + tsv.string());
    Hle h;
    std::unordered_set<std::string> files;
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> c;
        std::stringstream s(line);
        for (std::string x; std::getline(s, x, '\t');) c.push_back(x);
        if (c.size() < 3) continue;
        if (c[1] == "forced") h.forced_entries++, h.forced.emplace(c[0], c[2]), files.insert(c[2]);
        else if (c[1] == "swhle") h.swhle_entries++, h.swhle.insert(c[0]);
        else if (c[1] == "nvn") h.nvn.push_back(c[0]);
    }
    if (h.forced.empty()) nxw::fail("the registry inventory " + tsv.string() + " names no forced take-over");
    h.files = files.size();
    h.loaded = true;
    return h;
}

void mark_noreturn(Ctx& x) {
    static const std::unordered_set<std::string> known = {
        "longjmp", "_longjmp", "siglongjmp", "abort", "exit", "_exit", "_Exit", "quick_exit", "__cxa_throw", "__cxa_rethrow",
        "_Unwind_Resume", "__cxa_bad_cast", "__cxa_bad_typeid", "__cxa_throw_bad_array_new_length", "_ZSt9terminatev",
        "__stack_chk_fail", "pthread_exit",
    };
    for (auto& c : x.mods)
        for (Fn& f : c.fns) {
            if (f.flags & fn_plt) continue;
            bool may_return = false;
            for (uint32_t o = f.start; o < f.end && !may_return; o += 4) {
                const FlowInfo fl = flow(c.insn(o), o);
                if (fl.kind == Flow::ret || fl.kind == Flow::br || fl.kind == Flow::eret) may_return = true;
                else if ((fl.kind == Flow::b || fl.kind == Flow::bcond) && (fl.target < f.start || fl.target >= f.end)) may_return = true;
            }
            if (!may_return || known.count(f.name)) f.flags |= fn_noreturn;
        }
    for (auto& c : x.mods)
        for (Fn& f : c.fns) {
            auto p = c.plt_sym.find(f.start);
            if (p == c.plt_sym.end()) continue;
            const Resolution& r = x.p->resolution[size_t(c.index)][p->second];
            if (r.kind != Bound::self && r.kind != Bound::other) continue;
            auto [m2, f2] = x.fn_at_abs(r.address);
            if (m2 && (m2->fns[size_t(f2)].flags & fn_noreturn)) f.flags |= fn_noreturn;
            if (known.count(f.name)) f.flags |= fn_noreturn;
        }
}

void bind_symbols(Ctx& x) {
    Database& db = x.out->db;
    const Program& p = *x.p;
    std::unordered_set<std::string> nvn(x.hle.nvn.begin(), x.hle.nvn.end());
    std::vector<int> db_index(p.modules.size(), -1);
    for (size_t i = 0; i < x.mods.size(); i++) db_index[size_t(x.mods[i].index)] = int(i);
    x.symrec.assign(x.mods.size(), {});
    std::unordered_set<std::string> forced_hit;
    for (size_t mi = 0; mi < x.mods.size(); mi++) {
        ModCtx& c = x.mods[mi];
        const Module& m = *c.m;
        db.modules[mi].first_symbol = uint32_t(db.symbols.size());
        for (uint32_t s = 1; s < m.symbols.size(); s++) {
            const Symbol& y = m.symbols[s];
            if (y.name.empty() || (!y.imported() && !y.exported())) continue;
            SymbolRec r{};
            r.name = db.add_string(y.name), r.index = s, r.module = uint8_t(mi), r.provider = uint8_t(mi);
            r.flags = uint8_t((y.imported() ? 1 : 0) | (y.exported() ? 2 : 0) | (y.type == STT_FUNC ? 4 : 0) | (nvn.count(y.name) ? 8 : 0));
            const Resolution& b = p.resolution[size_t(c.index)][s];
            auto f = x.hle.forced.find(y.name);
            if (f != x.hle.forced.end()) r.binding = bind_forced, r.forced_in = db.add_string(f->second);
            else if (b.kind == Bound::self || b.kind == Bound::other) {
                r.binding = bind_guest;
                const int pi = db_index[size_t(b.provider)];
                if (pi >= 0) r.provider = uint8_t(pi), r.address = uint32_t(b.address - x.mods[size_t(pi)].base);
            } else if (b.kind == Bound::weak_zero) r.binding = x.hle.swhle.count(y.name) ? bind_swhle : bind_weak_zero;
            else r.binding = x.hle.swhle.count(y.name) ? bind_swhle : bind_stub;
            x.symrec[mi][s] = uint32_t(db.symbols.size());
            db.symbols.push_back(r);
        }
        db.modules[mi].symbol_count = uint32_t(db.symbols.size()) - db.modules[mi].first_symbol;
        for (const auto* t : {&m.rela, &m.jmprel})
            for (const Reloc& r : *t) {
                if (!r.sym) continue;
                if (x.hle.forced.count(m.symbols[r.sym].name)) forced_hit.insert(m.symbols[r.sym].name);
                auto it = x.symrec[mi].find(r.sym);
                if (it != x.symrec[mi].end() && r.type != R_AARCH64_JUMP_SLOT) db.symbols[it->second].data_refs++;
            }
        std::vector<std::pair<uint32_t, uint32_t>> stubs(c.plt_sym.begin(), c.plt_sym.end());
        std::sort(stubs.begin(), stubs.end());
        for (auto [off, sym] : stubs) {
            PltStubRec s{};
            s.stub = off, s.got = c.plt_got[off], s.module = uint8_t(mi), s.symbol = none32, s.callee = none32, s.binding = bind_none;
            auto it = x.symrec[mi].find(sym);
            if (it != x.symrec[mi].end()) {
                const SymbolRec& r = db.symbols[it->second];
                s.symbol = it->second, s.binding = r.binding;
                if (r.binding == bind_guest) {
                    auto [m2, f2] = x.fn_at_abs(x.mods[r.provider].base + r.address);
                    if (m2) s.callee = m2->first_fn + uint32_t(f2);
                }
            }
            db.plt_stubs.push_back(s);
        }
    }
    auto& hs = x.out->hle_stats;
    hs["forced_entries"] = int64_t(x.hle.forced_entries);
    hs["forced_names"] = int64_t(x.hle.forced.size());
    hs["forced_files"] = int64_t(x.hle.files);
    hs["forced_names_referenced"] = int64_t(forced_hit.size());
    hs["swhle_entries"] = int64_t(x.hle.swhle_entries);
    hs["swhle_names"] = int64_t(x.hle.swhle.size());
    hs["nvn_names"] = int64_t(x.hle.nvn.size());
}

}  // namespace nxanalyze
