// report.json and report.md, both derived from program.db plus the findings kept beside it
#include "internal.hpp"
#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace nxanalyze {

namespace {

struct Summary {
    struct Mod {
        std::string name;
        uint32_t index = 0;
        std::map<std::string, int64_t> fn, src, src_nofde, calls, jt, lp, misc;
        std::map<std::string, std::map<std::string, int64_t>> ind;   // BR / BLR / RET -> class -> count
        std::map<std::string, int64_t> sites;
        std::vector<uint32_t> special;                               // function indices
        std::map<std::string, std::vector<uint32_t>> sysreg_fns;     // "mrs fpcr" -> functions
        std::map<std::string, int64_t> sysreg_count;
        std::map<uint32_t, std::vector<uint32_t>> svc;               // number -> sites
        std::map<std::string, int64_t> traps;
        std::map<std::string, std::vector<uint32_t>> examples;       // indirect class -> sites
        std::map<std::string, int64_t> hle;                          // "forced: imports" -> count
    };
    std::vector<Mod> mods;
};

std::string fname(const Database& db, uint32_t fi) {
    const FunctionRec& f = db.functions[fi];
    if (f.name) return db.str(f.name);
    // the nearest named function before it
    const ModuleRec& m = db.modules[f.module];
    for (uint32_t k = fi; k-- > m.first_function && fi - k < 64;)
        if (db.functions[k].name) return std::string(db.str(m.name)) + "+" + hex(f.start) + " (after " + db.str(db.functions[k].name) + ")";
    return std::string(db.str(m.name)) + "+" + hex(f.start);
}

std::string sysreg_label(uint8_t kind, uint32_t d) {
    if (kind == site_msr_imm) return "msr pstate " + hex(d);
    if (kind == site_sys) return "sys " + hex(d);
    const char* n = sysreg_name(d);
    return std::string(kind == site_mrs ? "mrs " : "msr ") + (n ? n : hex(d));
}

Summary summarize(const Analysis& a) {
    const Database& db = a.db;
    Summary s;
    for (uint32_t mi = 0; mi < db.modules.size(); mi++) {
        const ModuleRec& m = db.modules[mi];
        Summary::Mod sm;
        sm.name = db.str(m.name), sm.index = mi;
        static const std::pair<uint16_t, const char*> srcs[] = {{src_fde, "fde"}, {src_bl, "bl"}, {src_tail, "tail call"}, {src_reloc, "relocation"},
            {src_export, "export"}, {src_init, "init/fini"}, {src_plt, "plt"}, {src_adr, "adr/adrp+add"}, {src_prologue, "prologue scan"},
            {src_entry, "module entry"}, {src_symbol, "local symbol"}};
        uint64_t covered = 0;
        for (uint32_t fi = m.first_function; fi < m.first_function + m.function_count; fi++) {
            const FunctionRec& f = db.functions[fi];
            covered += f.size;
            sm.fn["total"]++;
            if (f.flags & fn_no_fde) sm.fn["without FDE"]++;
            if (f.flags & fn_plt) sm.fn["PLT stubs (and PLT header)"]++;
            if ((f.flags & fn_no_fde) && !(f.flags & fn_plt)) sm.fn["without FDE, not PLT"]++;
            if (f.flags & fn_noreturn) sm.fn["noreturn"]++;
            if (f.flags & fn_leaf) sm.fn["leaf (no call)"]++;
            if (f.flags & fn_lsda) sm.fn["with LSDA"]++;
            if (f.flags & fn_jump_tables) sm.fn["with jump tables"]++;
            if (f.flags & fn_spin) sm.fn["with spin loops"]++;
            for (auto [b, n] : srcs)
                if (f.sources & b) {
                    sm.src[n]++;
                    if ((f.flags & fn_no_fde) && !(f.flags & fn_plt)) sm.src_nofde[n]++;
                }
            if (f.flags & (fn_sp_switch | fn_lr_write | fn_context_branch | fn_ret_other)) sm.special.push_back(fi);
            sm.misc["blocks"] += f.block_count;
            sm.lp["call-site records"] += f.call_site_count;
        }
        sm.misc["bytes covered by functions"] = int64_t(covered);
        sm.misc["text bytes"] = m.text_size;
        s.mods.push_back(sm);
    }
    for (const BlockRec& b : db.blocks) {
        auto& sm = s.mods[db.functions[b.function].module];
        sm.misc["edges"] += b.edge_count;
        if (b.flags & blk_landing_pad) sm.lp["landing-pad blocks"]++;
        if (b.flags & blk_unreachable) sm.misc["blocks with no predecessor (not entry, pad or table target)"]++;
        if (b.flags & blk_resume) sm.misc["setjmp resume blocks"]++;
    }
    for (const CallRec& c : db.calls) {
        auto& sm = s.mods[c.module];
        static const char* kinds[] = {"bl", "b (tail call)", "b.cond/cbz/tbz (conditional tail call)"};
        sm.calls[kinds[c.kind]]++;
        if (c.binding == bind_none) sm.calls["unresolved"]++;
        else if (c.symbol != none32) sm.calls[std::string("through a PLT stub, bound ") + binding_name(c.binding)]++;
    }
    for (const IndirectRec& r : db.indirect) {
        auto& sm = s.mods[r.module];
        const char* k = r.insn == ind_br ? "BR" : r.insn == ind_blr ? "BLR" : "RET";
        sm.ind[k][indirect_class_name(r.cls)]++;
        if (r.insn == ind_br && (r.flags & 4)) sm.misc["BR with x30 not the caller's (a branch within the function, not a tail call)"]++;
        if (r.flags & 2) sm.misc[std::string(k) + " through a GOT slot bound to host code"]++;
        auto& ex = sm.examples[std::string(k) + "|" + indirect_class_name(r.cls)];
        if (ex.size() < 4) ex.push_back(uint32_t(&r - db.indirect.data()));
    }
    for (const JumpTableRec& j : db.jump_tables) {
        auto& sm = s.mods[j.module];
        static const char* kinds[] = {"s8", "u8", "s16", "u16", "s32", "u32", "abs64"};
        if (j.kind == jt_abs64) continue;
        sm.jt[std::string("entries ") + kinds[j.kind]]++;
        sm.jt["targets"] += j.count;
        if (j.bound == jb_cmp) sm.jt["bound from cmp + b.hi/b.hs"]++;
        if (j.bound == jb_mask) sm.jt["bound from and/ubfx"]++;
        if (j.bound == jb_load_width) sm.jt["bound from a byte/halfword index load"]++;
        if (j.bound == jb_heuristic) sm.jt["no bound (read while in function)"]++;
    }
    std::vector<std::set<uint32_t>> pads(db.modules.size());
    for (const CallSiteRec& c : db.call_sites) {
        auto& sm = s.mods[c.module];
        if (c.landing_pad) {
            sm.lp["call sites with a landing pad"]++;
            pads[c.module].insert(c.landing_pad);
            if (!c.action) sm.lp["  cleanup only"]++;
            else if (c.first_filter > 0) sm.lp[c.type_name ? "  first clause catches a type" : "  first clause is catch (...)"]++;
            else if (c.first_filter < 0) sm.lp["  first clause is an exception specification"]++;
            else sm.lp["  first clause is a cleanup"]++;
        } else {
            sm.lp["call sites without a landing pad"]++;
        }
    }
    for (size_t mi = 0; mi < s.mods.size(); mi++) s.mods[mi].lp["distinct landing pads"] = int64_t(pads[mi].size());
    for (const SymbolRec& y : db.symbols) {
        auto& h = s.mods[y.module].hle;
        const std::string b = binding_name(y.binding);
        if (y.flags & 1) h[b + ": imports"]++;
        if (y.flags & 2) h[b + ": exports"]++;
        if (y.bl_sites || y.b_sites || y.blr_sites || y.data_refs) h[b + ": referenced"]++;
        h[b + ": bl sites (through the PLT)"] += y.bl_sites;
        h[b + ": b/br sites (tail calls through the PLT or a GOT slot)"] += y.b_sites;
        h[b + ": blr sites (through a GOT slot)"] += y.blr_sites;
        h[b + ": data references (GLOB_DAT, ABS64)"] += y.data_refs;
    }
    for (const SiteRec& t : db.sites) {
        auto& sm = s.mods[t.module];
        sm.sites[site_kind_name(t.kind)]++;
        if (t.kind == site_mrs || t.kind == site_msr || t.kind == site_msr_imm || t.kind == site_sys) {
            const std::string l = sysreg_label(t.kind, t.detail);
            sm.sysreg_count[l]++;
            auto& v = sm.sysreg_fns[l];
            if (std::find(v.begin(), v.end(), t.function) == v.end()) v.push_back(t.function);
        }
        if (t.kind == site_svc) sm.svc[t.detail].push_back(t.function);
        if (t.kind == site_udf) sm.traps["udf #" + hex(t.detail)]++;
        if (t.kind == site_brk) sm.traps["brk #" + hex(t.detail)]++;
    }
    return s;
}

void kv_map(Json& j, std::string_view k, const std::map<std::string, int64_t>& m) {
    j.key(k).obj();
    for (auto& [a, b] : m) j.kv(a, b);
    j.end();
}

}  // namespace

void write_report_json(const std::filesystem::path& p, const Analysis& a) {
    const Database& db = a.db;
    const Summary s = summarize(a);
    std::ofstream o(p, std::ios::binary);
    Json j(o);
    j.obj().kv("tool", "nxanalyze").kv("db_version", db_version);
    j.key("modules").arr();
    for (size_t mi = 0; mi < s.mods.size(); mi++) {
        const auto& sm = s.mods[mi];
        const ModuleRec& m = db.modules[mi];
        std::string id;
        for (uint8_t b : m.build_id) id += hex(0x100 | b).substr(3);
        j.obj().kv("name", sm.name).kv("build_id", id).kv("base", hex(m.base)).kv("text_size", m.text_size);
        kv_map(j, "functions", sm.fn);
        kv_map(j, "sources", sm.src);
        kv_map(j, "sources_without_fde", sm.src_nofde);
        kv_map(j, "cfg", sm.misc);
        kv_map(j, "direct_branches", sm.calls);
        j.key("indirect").obj();
        for (auto& [k, m2] : sm.ind) kv_map(j, k, m2);
        j.end();
        kv_map(j, "jump_tables", sm.jt);
        kv_map(j, "landing_pads", sm.lp);
        kv_map(j, "hle", sm.hle);
        kv_map(j, "sites", sm.sites);
        kv_map(j, "traps", sm.traps);
        j.key("system_registers").arr();
        for (auto& [r, fns] : sm.sysreg_fns) {
            j.obj().kv("access", r).kv("sites", sm.sysreg_count.at(r)).key("functions").arr();
            for (uint32_t f : fns) j.val(fname(db, f));
            j.end().end();
        }
        j.end();
        j.key("svc").arr();
        for (auto& [no, fns] : sm.svc) {
            j.obj().kv("svc", hex(no)).key("functions").arr();
            for (uint32_t f : fns) j.val(fname(db, f));
            j.end().end();
        }
        j.end();
        j.key("special_functions").arr();
        for (uint32_t fi : sm.special) {
            const FunctionRec& f = db.functions[fi];
            j.obj().kv("function", fname(db, fi)).kv("offset", hex(f.start)).kv("flags", hex(f.flags)).key("sites").arr();
            for (const SiteRec& t : db.sites)
                if (t.function == fi && t.kind >= site_sp_switch && t.kind <= site_longjmp_call)
                    j.obj().kv("at", hex(t.site)).kv("kind", site_kind_name(t.kind)).kv("detail", t.detail).end();
            j.end().end();
        }
        j.end();
        j.key("indirect_examples").obj();
        for (auto& [k, v] : sm.examples) {
            j.key(k).arr();
            for (uint32_t r : v) j.val(hex(db.indirect[r].site) + " in " + fname(db, db.indirect[r].function));
            j.end();
        }
        j.end();
        j.key("spin_loops").arr();
        for (const SpinRec& r : db.spin_loops)
            if (r.module == mi)
                j.obj().kv("head", hex(r.head)).kv("last", hex(r.last)).kv("kind", r.kind).kv("insns", uint32_t(r.insns))
                    .kv("in_nxnative", bool(r.flags & spin_in_nxnative)).kv("function", fname(db, r.function)).end();
        j.end();
        kv_map(j, "stats", a.stats[mi]);
        j.key("notes").arr();
        for (const auto& n : a.notes[mi]) j.val(n);
        j.end();
        j.end();
    }
    j.end();
    kv_map(j, "hle", a.hle_stats);
    j.key("hle_symbols").arr();
    for (const SymbolRec& y : db.symbols)
        if (y.binding != bind_guest)
            j.obj().kv("module", db.str(db.modules[y.module].name)).kv("name", db.str(y.name)).kv("binding", binding_name(y.binding))
                .kv("forced_in", db.str(y.forced_in)).kv("import", bool(y.flags & 1)).kv("bl", y.bl_sites).kv("b", y.b_sites).kv("blr", y.blr_sites)
                .kv("data", y.data_refs).end();
    j.end();
    j.key("trace_coverage").arr();
    for (const auto& t : a.trace_checks) {
        j.obj().kv("run", t.run).kv("module", t.module).kv("executed", uint64_t(t.executed)).kv("missing", uint64_t(t.missing)).key("missing_examples").arr();
        for (uint32_t o : t.missing_examples) j.val(hex(o));
        j.end().end();
    }
    j.end();
    j.key("trace_indirect").arr();
    for (const auto& t : a.indirect_checks) {
        j.obj().kv("run", t.run).kv("records", uint64_t(t.records)).kv("pairs", uint64_t(t.pairs)).kv("sites", uint64_t(t.sites));
        std::map<std::string, int64_t> m(t.by_class.begin(), t.by_class.end());
        kv_map(j, "by_class", m);
        j.key("problems").arr();
        for (const auto& pr : t.problems) j.val(pr);
        j.end().end();
    }
    j.end();
    j.key("spin_vs_nxnative").arr();
    for (const auto& t : a.spin_checks) {
        j.obj().kv("module", t.module).kv("ours", uint64_t(t.ours)).kv("theirs", uint64_t(t.theirs)).kv("both", uint64_t(t.both));
        j.key("only_ours").arr();
        for (uint32_t h : t.only_ours) j.val(hex(h));
        j.end().key("only_theirs").arr();
        for (uint32_t h : t.only_theirs) j.val(hex(h));
        j.end().end();
    }
    j.end();
    j.key("problems").arr();
    for (const auto& pr : a.problems) j.val(pr);
    j.end();
    j.end();
}

void write_report_md(const std::filesystem::path& p, const Analysis& a, const std::string& title) {
    const Database& db = a.db;
    const Summary s = summarize(a);
    std::ostringstream o;
    auto table = [&](const std::string& head, const std::vector<std::string>& keys, auto&& get) {
        o << "| " << head;
        for (const auto& m : s.mods) o << " | " << m.name;
        o << " |\n|---";
        for (size_t i = 0; i < s.mods.size(); i++) o << "|---:";
        o << "|\n";
        for (const auto& k : keys) {
            o << "| " << k;
            for (const auto& m : s.mods) o << " | " << get(m, k);
            o << " |\n";
        }
        o << "\n";
    };
    auto keys_of = [&](auto pick) {
        std::set<std::string> k;
        for (const auto& m : s.mods)
            for (auto& [a2, b] : pick(m)) k.insert(a2);
        return std::vector<std::string>(k.begin(), k.end());
    };
    auto val = [](const std::map<std::string, int64_t>& m, const std::string& k) {
        auto it = m.find(k);
        return it == m.end() ? std::string("0") : std::to_string(it->second);
    };
    o << "# " << title << "\n\nGenerated by nxanalyze (program.db version " << db_version << "). Module offsets are relative to each module's base.\n\n";
    o << "## Functions\n\n";
    table("functions", keys_of([](auto& m) { return m.fn; }), [&](auto& m, auto& k) { return val(m.fn, k); });
    o << "Sources (a start can have several):\n\n";
    table("source", keys_of([](auto& m) { return m.src; }), [&](auto& m, auto& k) { return val(m.src, k); });
    o << "Sources of functions without an FDE (PLT excluded):\n\n";
    table("source", keys_of([](auto& m) { return m.src_nofde; }), [&](auto& m, auto& k) { return val(m.src_nofde, k); });
    o << "## CFG and direct branches\n\n";
    table("quantity", keys_of([](auto& m) { return m.misc; }), [&](auto& m, auto& k) { return val(m.misc, k); });
    table("direct calls and tail calls", keys_of([](auto& m) { return m.calls; }), [&](auto& m, auto& k) { return val(m.calls, k); });
    o << "## Indirect branches\n\n";
    for (const char* k : {"BR", "BLR", "RET"}) {
        std::set<std::string> cls;
        for (const auto& m : s.mods)
            if (auto it = m.ind.find(k); it != m.ind.end())
                for (auto& [c, n] : it->second) cls.insert(c);
        table(std::string(k) + " class", std::vector<std::string>(cls.begin(), cls.end()), [&](auto& m, auto& c) {
            auto it = m.ind.find(k);
            return it == m.ind.end() ? std::string("0") : val(it->second, c);
        });
    }
    o << "## Jump tables\n\n";
    table("relative jump tables", keys_of([](auto& m) { return m.jt; }), [&](auto& m, auto& k) { return val(m.jt, k); });
    o << "## Landing pads\n\n";
    table("LSDA", keys_of([](auto& m) { return m.lp; }), [&](auto& m, auto& k) { return val(m.lp, k); });
    o << "## Special sites\n\n";
    table("site", keys_of([](auto& m) { return m.sites; }), [&](auto& m, auto& k) { return val(m.sites, k); });
    for (const auto& m : s.mods) {
        if (m.special.empty() && m.sysreg_fns.empty() && m.svc.empty()) continue;
        o << "### " << m.name << "\n\n";
        if (!m.special.empty()) {
            o << "| function | offset | sites |\n|---|---|---|\n";
            for (uint32_t fi : m.special) {
                o << "| `" << fname(db, fi) << "` | " << hex(db.functions[fi].start) << " | ";
                bool first = true;
                for (const SiteRec& t : db.sites)
                    if (t.function == fi && t.kind >= site_sp_switch && t.kind <= site_context_branch) {
                        o << (first ? "" : "; ") << site_kind_name(t.kind) << " at " << hex(t.site);
                        first = false;
                    }
                o << " |\n";
            }
            o << "\n";
        }
        if (!m.sysreg_fns.empty()) {
            o << "| system register access | sites | functions |\n|---|---:|---|\n";
            for (auto& [r, fns] : m.sysreg_fns) {
                o << "| " << r << " | " << m.sysreg_count.at(r) << " | ";
                for (size_t i = 0; i < fns.size() && i < 8; i++) o << (i ? ", " : "") << "`" << fname(db, fns[i]) << "`";
                if (fns.size() > 8) o << ", … (" << fns.size() << ")";
                o << " |\n";
            }
            o << "\n";
        }
        if (!m.svc.empty()) {
            o << "SVC numbers and the functions that issue them: ";
            bool first = true;
            for (auto& [no, fns] : m.svc) o << (first ? "" : "; ") << hex(no) << " (" << fns.size() << ")", first = false;
            o << "\n\n";
        }
        if (!m.traps.empty()) {
            o << "Traps inside functions: ";
            bool first = true;
            for (auto& [k, n] : m.traps) o << (first ? "" : ", ") << k << " ×" << n, first = false;
            o << "\n\n";
        }
    }
    o << "## Spin loops\n\n| module | head | last back-edge | kind | instructions | in nxnative | function |\n|---|---|---|---|---:|---|---|\n";
    static const char* sk[] = {"poll", "poll with invariant stores", "wfe", "yield", "registers only"};
    for (const SpinRec& r : db.spin_loops)
        o << "| " << db.str(db.modules[r.module].name) << " | " << hex(r.head) << " | " << hex(r.last) << " | " << sk[r.kind] << " | " << r.insns << " | "
          << ((r.flags & spin_in_nxnative) ? "yes" : "") << " | `" << fname(db, r.function) << "` |\n";
    o << "\n";
    for (const auto& t : a.spin_checks) {
        o << "- " << t.module << ": ours " << t.ours << ", nxnative's rewriter " << t.theirs << ", both " << t.both;
        if (!t.only_ours.empty()) {
            o << "; only ours:";
            for (uint32_t h : t.only_ours) o << " " << hex(h);
        }
        if (!t.only_theirs.empty()) {
            o << "; only nxnative's:";
            for (uint32_t h : t.only_theirs) o << " " << hex(h);
        }
        o << "\n";
    }
    o << "\n## HLE binding (nxnative)\n\nnxnative's sources: ";
    for (auto& [k, v] : a.hle_stats) o << k << " " << v << "; ";
    o << "\n\n";
    table("binding", keys_of([](auto& m) { return m.hle; }), [&](auto& m, auto& k) { return val(m.hle, k); });
    o << "\n## Mac traces\n\n";
    for (const auto& t : a.trace_checks)
        o << "- " << t.run << " " << t.module << ": " << t.executed << " executed function starts, " << t.missing << " not in our set\n";
    for (const auto& t : a.indirect_checks) {
        o << "- " << t.run << " indirect targets: " << t.records << " records, " << t.pairs << " distinct site-target pairs, " << t.sites << " sites\n";
        for (auto& [k, n] : t.by_class) o << "  - " << k << ": " << n << "\n";
        for (const auto& pr : t.problems) o << "  - problem: " << pr << "\n";
    }
    o << "\n## Indirect-branch examples\n\n";
    for (const auto& m : s.mods)
        for (auto& [k, v] : m.examples) {
            o << "- " << m.name << " " << k.substr(0, k.find('|')) << ", " << k.substr(k.find('|') + 1) << ":";
            for (uint32_t r : v) o << " `" << hex(db.indirect[r].site) << "` in `" << fname(db, db.indirect[r].function) << "`;";
            o << "\n";
        }
    o << "\n## Notes\n\n";
    for (size_t mi = 0; mi < a.notes.size(); mi++) {
        o << "### " << s.mods[mi].name << "\n\n";
        for (auto& [k, v] : a.stats[mi]) o << "- " << k << ": " << v << "\n";
        for (size_t i = 0; i < a.notes[mi].size() && i < 120; i++) o << "- " << a.notes[mi][i] << "\n";
        o << "\n";
    }
    for (const auto& pr : a.problems) o << "- problem: " << pr << "\n";
    std::ofstream f(p, std::ios::binary);
    f << o.str();
}

}  // namespace nxanalyze
