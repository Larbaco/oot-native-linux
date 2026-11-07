// Checks against what ran on real hardware (research/05): executed function starts, and the targets the Mac recorded for every
// executed br/blr (the first eight per site); and the spin loops against nxnative's rewriter tables.
#include "internal.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace nxanalyze {

namespace fs = std::filesystem;

namespace {

std::vector<std::vector<std::string>> read_tsv(const fs::path& p) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream f(p);
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> r;
        std::stringstream s(line);
        for (std::string cell; std::getline(s, cell, '\t');) r.push_back(cell);
        if (!r.empty()) rows.push_back(r);
    }
    return rows;
}

uint64_t num(const std::string& s) { return std::stoull(s, nullptr, 0); }

struct MacModule {
    std::string name;
    uint64_t base = 0, size = 0;
};

}  // namespace

void check_traces(Ctx& x) {
    const Database& db = x.out->db;
    std::map<std::pair<size_t, uint32_t>, size_t> ind_at;   // (module, site) -> indirect record
    for (size_t i = 0; i < db.indirect.size(); i++) ind_at[{db.indirect[i].module, db.indirect[i].site}] = i;
    for (const fs::path& run : x.opt->traces) {
        const std::string rn = run.filename().string();
        for (size_t mi = 0; mi < x.mods.size(); mi++) {
            ModCtx& c = x.mods[mi];
            const fs::path f = run / ("executed_" + c.m->name + ".txt");
            if (!fs::exists(f)) continue;
            TraceCheck t;
            t.run = rn, t.module = c.m->name;
            for (const auto& r : read_tsv(f)) {
                const uint64_t off = std::stoull(r[0], nullptr, 16);
                t.executed++;
                if (off >= c.text || c.fn_starting(uint32_t(off)) < 0) {
                    t.missing++;
                    if (t.missing_examples.size() < 20) t.missing_examples.push_back(uint32_t(off));
                }
            }
            x.out->trace_checks.push_back(t);
        }
        // indirect branch targets
        const fs::path log = run / "trace" / "ind_log.bin", mods = run / "trace" / "modules.tsv";
        if (!fs::exists(log) || !fs::exists(mods)) continue;
        std::vector<MacModule> mm;
        for (const auto& r : read_tsv(mods))
            if (r.size() >= 3 && r[0] != "name") mm.push_back({r[0], num(r[1]), num(r[2])});
        auto locate = [&](uint64_t a) -> std::pair<int, uint64_t> {
            for (const auto& m : mm)
                if (a >= m.base && a - m.base < m.size)
                    for (size_t mi = 0; mi < x.mods.size(); mi++)
                        if (x.mods[mi].m->name == m.name) return {int(mi), a - m.base};
            return {-1, 0};
        };
        IndirectCheck ic;
        ic.run = rn;
        const std::vector<uint8_t> bytes = nxw::read_file(log);
        std::set<std::pair<uint64_t, uint64_t>> pairs;
        std::set<uint64_t> sites;
        for (size_t o = 0; o + 24 <= bytes.size(); o += 24) {
            const uint64_t at = nxw::load_le<uint64_t>(bytes, o + 8), t = nxw::load_le<uint64_t>(bytes, o + 16);
            ic.records++;
            if (!pairs.insert({at, t}).second) continue;
            sites.insert(at);
            auto [sm, so] = locate(at);
            if (sm < 0) continue;
            ModCtx& c = x.mods[size_t(sm)];
            const bool isbr = (c.word(uint32_t(so)) & 0xfffffc1f) == 0xd61f0000;
            const std::string k = isbr ? "BR" : "BLR";
            if (so >= c.text) { ic.by_class[k + ": site in nxnative's patch area"]++; continue; }
            auto rec = ind_at.find({size_t(sm), uint32_t(so)});
            if (rec == ind_at.end()) {
                ic.by_class[k + ": site not in program.db"]++;
                if (ic.problems.size() < 30) ic.problems.push_back(k + " site " + c.m->name + "+" + hex(so) + " is not an indirect site in program.db");
                continue;
            }
            const IndirectRec& r = db.indirect[rec->second];
            auto [tm, to] = locate(t);
            std::string cls;
            const bool table = r.cls == ic_jump_table || r.cls == ic_jump_table_unbounded || r.cls == ic_table_local;
            if (tm < 0) cls = std::string("host code; static class: ") + indirect_class_name(r.cls);
            else if (to >= x.mods[size_t(tm)].text) cls = "nxnative patch area";
            else if (table) {
                const JumpTableRec& j = db.jump_tables[r.aux];
                const bool in = tm == sm && std::find(db.jt_targets.begin() + j.first_target, db.jt_targets.begin() + j.first_target + j.count, uint32_t(to)) !=
                                                db.jt_targets.begin() + j.first_target + j.count;
                cls = in ? "jump table: target in the decoded table" : "jump table: target NOT in the decoded table";
                if (!in && ic.problems.size() < 30) ic.problems.push_back("jump table at " + c.m->name + "+" + hex(so) + " went to " + hex(to));
            } else if (x.mods[size_t(tm)].fn_starting(uint32_t(to)) >= 0) {
                cls = r.cls == ic_table_tail ? "function table: function start" : "function start";
            } else {
                cls = "NOT a function start";
                if (ic.problems.size() < 30) ic.problems.push_back(k + " at " + c.m->name + "+" + hex(so) + " (" + indirect_class_name(r.cls) + ") went to " + x.mods[size_t(tm)].m->name + "+" + hex(to));
            }
            ic.by_class[k + ": " + cls]++;
        }
        ic.pairs = pairs.size(), ic.sites = sites.size();
        x.out->indirect_checks.push_back(ic);
    }
}

void check_rewriter(Ctx& x) {
    Database& db = x.out->db;
    for (size_t mi = 0; mi < x.mods.size(); mi++) {
        const fs::path f = x.opt->rewriter / (x.mods[mi].m->name + ".sites.tsv");
        if (!fs::exists(f)) continue;
        std::set<uint32_t> theirs;
        for (const auto& r : read_tsv(f))
            if (r.size() >= 2 && r[1] == "S") theirs.insert(uint32_t(num(r[0])));
        SpinCheck s;
        s.module = x.mods[mi].m->name;
        s.theirs = theirs.size();
        std::set<uint32_t> ours;
        for (SpinRec& r : db.spin_loops) {
            if (r.module != mi || (r.kind != spin_poll && r.kind != spin_poll_store && r.kind != spin_register)) continue;
            ours.insert(r.head);
            if (theirs.count(r.head)) r.flags |= spin_in_nxnative;
        }
        s.ours = ours.size();
        for (uint32_t h : ours)
            if (theirs.count(h)) s.both++;
            else s.only_ours.push_back(h);
        for (uint32_t h : theirs)
            if (!ours.count(h)) s.only_theirs.push_back(h);
        x.out->spin_checks.push_back(s);
    }
}

}  // namespace nxanalyze
