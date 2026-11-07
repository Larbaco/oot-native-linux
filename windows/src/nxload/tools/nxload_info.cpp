// nxload-info <exefs dir> [--json <file>] [--base <addr>] [--base-of <module>=<addr>] [--no-rtld] [--no-hash-check]
// Prints what nxload makes of a game's ExeFS. --json writes the same plus the full symbol, binding, relocation and FDE lists, for nxanalyze.
#include "json.hpp"
#include "nxload/ehframe.hpp"
#include "nxload/npdm.hpp"
#include "nxload/program.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <tuple>

using namespace nxload;

namespace {

std::string num(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

std::string hx(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

std::string mib(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f MiB", double(v) / 1048576.0);
    return b;
}

template <class M> std::string counts(const M& m) {   // "a 3, b 1": by descending count
    std::vector<std::pair<std::string, size_t>> v(m.begin(), m.end());
    std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string s;
    for (auto& [k, n] : v) s += (s.empty() ? "" : ", ") + k + " " + num(n);
    return s.empty() ? "none" : s;
}

std::string ranges(const std::set<uint32_t>& s) {   // "0x01-0x29 0x2c"
    std::string out;
    for (auto it = s.begin(); it != s.end();) {
        uint32_t a = *it, b = a;
        while (++it != s.end() && *it == b + 1) b = *it;
        char buf[32];
        if (a == b) std::snprintf(buf, sizeof buf, "0x%02x", a);
        else std::snprintf(buf, sizeof buf, "0x%02x-0x%02x", a, b);
        out += (out.empty() ? "" : " ") + std::string(buf);
    }
    return out.empty() ? "none" : out;
}

std::set<uint32_t> svc_set(const std::bitset<256>& b) {
    std::set<uint32_t> s;
    for (uint32_t i = 0; i < b.size(); i++)
        if (b[i]) s.insert(i);
    return s;
}

struct Facts {
    EhFrame eh;
    std::vector<SvcSite> svcs;
    std::map<std::string, size_t> imports_by_type, exports_by_type, rela, jmprel, rel_targets, fdes_by_aug, imports_from;
    size_t imports = 0, imports_weak = 0, exports = 0, exports_weak = 0, locals = 0, nondefault_vis = 0, hash_bad = 0;
    size_t text_targets = 0, text_targets_at_fde = 0, interposed = 0, interposed_used = 0;
    size_t zero_size = 0, outside_text = 0, duplicates = 0, overlaps = 0;
    uint64_t covered = 0;
    std::vector<uint64_t> text_target_list;
    std::vector<std::pair<std::string, Bound>> unresolved;
};

Facts facts(const Program& p, size_t i) {
    const Module& m = p.modules[i];
    Facts f;
    f.eh = parse_eh_frame(m);
    f.svcs = m.svc_sites();
    for (uint32_t s = 1; s < m.symbols.size(); s++) {
        const Symbol& y = m.symbols[s];
        const Resolution& r = p.resolution[i][s];
        if (y.imported()) {
            f.imports++;
            f.imports_weak += y.bind == STB_WEAK;
            f.imports_by_type[type_name(y.type)]++;
            if (r.provider >= 0) f.imports_from[p.modules[size_t(r.provider)].name]++;
            else f.unresolved.push_back({y.name, r.kind});
        } else if (y.exported()) {
            f.exports++;
            f.exports_weak += y.bind == STB_WEAK;
            f.exports_by_type[type_name(y.type)]++;
            f.interposed += r.kind == Bound::other;
        } else if (y.defined()) {
            f.locals++;
        }
        f.nondefault_vis += y.defined() && y.visibility != STV_DEFAULT;
    }
    std::set<uint32_t> used;
    for (const auto* t : {&m.rela, &m.jmprel})
        for (const Reloc& r : *t) {
            if (r.sym && m.symbols[r.sym].defined() && p.resolution[i][r.sym].kind == Bound::other) used.insert(r.sym);
            (t == &m.rela ? f.rela : f.jmprel)[reloc_name(r.type)]++;
        }
    f.interposed_used = used.size();
    f.hash_bad = m.hash_mismatches();

    const uint64_t text_end = m.seg_end(seg_text);
    std::vector<std::pair<uint64_t, uint64_t>> rs;
    for (const Fde& d : f.eh.fdes) {
        f.fdes_by_aug[f.eh.cies[d.cie].augmentation]++;
        f.zero_size += d.size == 0;
        f.outside_text += d.start >= text_end;
        rs.push_back({d.start, d.start + d.size});
    }
    std::sort(rs.begin(), rs.end());
    uint64_t last = 0;
    for (auto [s, e] : rs) {
        if (s >= text_end) break;
        e = std::min(e, text_end);
        if (s < last) f.overlaps++;
        if (e > std::max(s, last)) f.covered += e - std::max(s, last);
        last = std::max(last, e);
    }
    std::set<uint64_t> starts;
    for (auto& r : rs) starts.insert(r.first);
    f.duplicates = rs.size() - starts.size();
    std::set<uint64_t> tt;
    for (uint64_t t : m.relative_targets()) {
        f.rel_targets[m.where(t)]++;
        if (t < text_end) tt.insert(t);
    }
    f.text_target_list.assign(tt.begin(), tt.end());
    f.text_targets = tt.size();
    for (uint64_t t : tt) f.text_targets_at_fde += starts.count(t);
    return f;
}

std::string personality(const Module& m, const Cie& c) {
    if (c.personality_enc == DW_EH_PE_omit) return "";
    if (c.personality_enc & DW_EH_PE_indirect) {
        for (const auto* t : {&m.rela, &m.jmprel})
            for (const Reloc& r : *t)
                if (r.offset == c.personality && r.sym) return "*" + hx(c.personality) + " = " + m.symbols[r.sym].name;
        return "*" + hx(c.personality);
    }
    for (const Symbol& s : m.symbols)
        if (s.defined() && s.value == c.personality && !s.name.empty()) return s.name;
    return hx(c.personality);
}

std::string enc(uint8_t e) {
    char b[8];
    std::snprintf(b, sizeof b, "%02x", e);
    return b;
}

std::vector<uint64_t> array_entries(const Program& p, size_t i, int64_t tag, int64_t size_tag) {
    const Module& m = p.modules[i];
    std::vector<uint64_t> out;
    if (auto a = m.dyn.get(tag); a && !p.memory.empty())
        for (uint64_t k = 0; k < m.dyn.value(size_tag) / 8; k++) out.push_back(p.read64(int(i), *a + 8 * k));
    return out;
}

void print_module(const Program& p, size_t i, const Facts& f) {
    const Module& m = p.modules[i];
    const Dynamic& d = m.dyn;
    std::printf("\n== %s  build %s  base %s  image %s%s\n", m.name.c_str(), m.build_id().c_str(), hx(p.bases[i]).c_str(), hx(m.image_size()).c_str(),
                p.in_scope[i] ? "" : "  (outside the lookup scope)");
    std::printf("   file %s B, NSO version %u, flags %#x, %s\n", num(m.file_size).c_str(), m.nso.version, m.nso.flags,
                m.hashes_checked ? "segment SHA-256 checked" : "segment hashes not checked");
    for (int s = 0; s < 3; s++) {
        const Segment& g = m.seg(s);
        std::printf("   %-5s %10s (%s) at %#9x, stored %s B%s\n", seg_names[s], hx(g.size).c_str(), mib(g.size).c_str(), g.mem_off,
                    num(g.compressed ? g.file_size : g.size).c_str(), g.compressed ? ((m.nso.flags >> 7 & 1) ? " ZBIC" : " LZ4") : "");
    }
    std::printf("   bss   %10s (%s)\n", hx(m.nso.bss_size).c_str(), mib(m.nso.bss_size).c_str());
    std::printf("   MOD0 at %s: .dynamic %s, bss %s..%s, .eh_frame_hdr %s..%s, module object %s\n", hx(m.mod0.offset).c_str(), hx(m.mod0.dynamic).c_str(),
                hx(m.mod0.bss_start).c_str(), hx(m.mod0.bss_end).c_str(), hx(m.mod0.eh_frame_hdr_start).c_str(), hx(m.mod0.eh_frame_hdr_end).c_str(),
                hx(m.mod0.module_object).c_str());
    std::string tags;
    std::map<int64_t, size_t> seen;
    for (auto& [t, v] : d.tags) seen[t]++;
    for (auto& [t, v] : d.tags)
        if (seen.count(t) && seen[t]) {
            tags += " " + dt_name(t) + (seen[t] > 1 ? " x" + std::to_string(seen[t]) : "");
            seen[t] = 0;
        }
    std::printf("   dynamic: %zu tags:%s\n", d.tags.size(), tags.c_str());
    std::string needed;
    for (auto& n : d.needed) needed += (needed.empty() ? "" : ", ") + n;
    if (!needed.empty()) std::printf("   NEEDED %s\n", needed.c_str());
    if (!d.soname.empty()) std::printf("   SONAME %s\n", d.soname.c_str());
    const auto init = array_entries(p, i, DT_INIT_ARRAY, DT_INIT_ARRAYSZ), fini = array_entries(p, i, DT_FINI_ARRAY, DT_FINI_ARRAYSZ);
    const size_t init_in_text = size_t(std::count_if(init.begin(), init.end(), [&](uint64_t a) { return a - p.bases[i] < m.seg_end(seg_text); }));
    std::printf("   INIT %s, FINI %s, INIT_ARRAY %s entries (%s in text), FINI_ARRAY %s, PREINIT_ARRAY %s; FLAGS %s, FLAGS_1 %s\n",
                d.get(DT_INIT) ? hx(*d.get(DT_INIT)).c_str() : "-", d.get(DT_FINI) ? hx(*d.get(DT_FINI)).c_str() : "-",
                num(d.value(DT_INIT_ARRAYSZ) / 8).c_str(), num(init_in_text).c_str(), num(d.value(DT_FINI_ARRAYSZ) / 8).c_str(),
                num(d.value(DT_PREINIT_ARRAYSZ) / 8).c_str(), hx(d.value(DT_FLAGS)).c_str(), hx(d.value(DT_FLAGS_1)).c_str());
    (void)fini;
    std::printf("   symbols %s (NSO header %s, DT_HASH %s, DT_GNU_HASH %s); hash lookups %s\n", num(m.symbols.size()).c_str(),
                num(m.nso.dynsym.size / 24).c_str(), m.hash ? num(m.symbol_count_sysv()).c_str() : "-", m.gnu_hash ? num(m.symbol_count_gnu()).c_str() : "-",
                f.hash_bad ? ("FAIL for " + num(f.hash_bad)).c_str() : "find every export");
    std::printf("   imports %s (weak %s; %s)\n", num(f.imports).c_str(), num(f.imports_weak).c_str(), counts(f.imports_by_type).c_str());
    std::printf("   exports %s (weak %s; %s); other defined %s; non-default visibility %s\n", num(f.exports).c_str(), num(f.exports_weak).c_str(),
                counts(f.exports_by_type).c_str(), num(f.locals).c_str(), num(f.nondefault_vis).c_str());
    std::printf("   RELA %s; JMPREL %s; RELR %s\n", counts(f.rela).c_str(), counts(f.jmprel).c_str(), num(m.relr.size()).c_str());
    std::printf("   relative targets: %s; distinct in text %s, at an FDE start %s\n", counts(f.rel_targets).c_str(), num(f.text_targets).c_str(),
                num(f.text_targets_at_fde).c_str());
    const RelocStats& st = p.stats[i];
    std::printf("   symbol relocations bound to: own module %s, another module %s, 0 (weak) %s, unresolved %s\n", num(st.sym_self).c_str(),
                num(st.sym_other).c_str(), num(st.sym_weak_zero).c_str(), num(st.sym_unresolved).c_str());
    const EhFrame& e = f.eh;
    std::printf("   .eh_frame_hdr v%u (enc %s/%s/%s): %s FDEs; .eh_frame %s..%s: %s FDEs, %s CIEs; table = walk: %s\n", e.hdr.version,
                enc(e.hdr.ptr_enc).c_str(), enc(e.hdr.count_enc).c_str(), enc(e.hdr.table_enc).c_str(), num(e.hdr.fde_count).c_str(), hx(e.begin).c_str(),
                hx(e.end).c_str(), num(e.fdes.size()).c_str(), num(e.cies.size()).c_str(), e.hdr_matches ? "yes" : "NO");
    std::map<uint32_t, size_t> per_cie;
    for (const Fde& d2 : e.fdes) per_cie[d2.cie]++;
    for (size_t c = 0; c < e.cies.size(); c++) {
        const Cie& k = e.cies[c];
        const std::string pers = personality(m, k);
        std::printf("     CIE %s v%u \"%s\" code %llu data %lld ra x%llu fde_enc %s%s%s: %s FDEs\n", hx(k.offset).c_str(), k.version, k.augmentation.c_str(),
                    static_cast<unsigned long long>(k.code_align), static_cast<long long>(k.data_align), static_cast<unsigned long long>(k.ra_register),
                    enc(k.fde_enc).c_str(), k.lsda_enc != DW_EH_PE_omit ? (" lsda_enc " + enc(k.lsda_enc)).c_str() : "",
                    pers.empty() ? "" : (" personality " + pers).c_str(), num(per_cie[uint32_t(c)]).c_str());
    }
    const uint64_t text = m.seg(seg_text).size;
    std::printf("   FDEs by augmentation: %s; with LSDA %s; zero-size %s, outside text %s, duplicate starts %s, overlaps %s; text covered %.2f%%\n",
                counts(f.fdes_by_aug).c_str(), num(e.lsda_count()).c_str(), num(f.zero_size).c_str(), num(f.outside_text).c_str(), num(f.duplicates).c_str(),
                num(f.overlaps).c_str(), text ? 100.0 * double(f.covered) / double(text) : 0.0);
    std::set<uint32_t> nums;
    for (auto& s : f.svcs) nums.insert(s.imm);
    std::printf("   SVC sites %s (%s distinct)%s%s\n", num(f.svcs.size()).c_str(), num(nums.size()).c_str(), nums.empty() ? "" : ": ", nums.empty() ? "" : ranges(nums).c_str());
    for (auto& n : m.notes) std::printf("   note: %s\n", n.c_str());
}

void print_link(const Program& p, const std::vector<Facts>& fs) {
    std::printf("\n== link (lookup order");
    for (size_t i = 0; i < p.modules.size(); i++)
        if (p.in_scope[i]) std::printf(" %s", p.modules[i].name.c_str());
    std::printf("), program %s..%s\n", hx(p.bases.empty() ? 0 : p.bases[0]).c_str(), hx(p.end()).c_str());
    std::printf("   %-9s %8s", "imports", "total");
    for (auto& m : p.modules)
        if (p.in_scope[size_t(&m - p.modules.data())]) std::printf(" %9s", m.name.c_str());
    std::printf("  unresolved\n");
    for (size_t i = 0; i < p.modules.size(); i++) {
        const Facts& f = fs[i];
        std::printf("   %-9s %8s", p.modules[i].name.c_str(), num(f.imports).c_str());
        for (size_t j = 0; j < p.modules.size(); j++) {
            if (!p.in_scope[j]) continue;
            auto it = f.imports_from.find(p.modules[j].name);
            std::printf(" %9s", j == i ? "-" : num(it == f.imports_from.end() ? 0 : it->second).c_str());
        }
        std::printf("  %s\n", num(f.unresolved.size()).c_str());
        for (auto& [n, k] : f.unresolved) std::printf("             %s %s\n", k == Bound::weak_zero ? "weak, bound to 0:" : "UNRESOLVED:", n.c_str());
    }
    std::string dups;
    for (size_t i = 0; i < p.modules.size(); i++)
        for (size_t j = i + 1; j < p.modules.size(); j++) {
            if (!p.in_scope[i] || !p.in_scope[j]) continue;
            size_t n = 0;
            for (auto& [name, idx] : p.modules[i].by_name) n += p.modules[j].by_name.count(name);
            dups += (dups.empty() ? "" : ", ") + p.modules[i].name + " & " + p.modules[j].name + " " + num(n);
        }
    std::printf("   exported by two modules: %s\n", dups.c_str());
    for (size_t i = 0; i < p.modules.size(); i++)
        if (fs[i].interposed)
            std::printf("   %s: %s of its exports bound to an earlier module's definition (%s of them used by its relocations)\n", p.modules[i].name.c_str(),
                        num(fs[i].interposed).c_str(), num(fs[i].interposed_used).c_str());
    std::printf("   problems: %s\n", p.problems.empty() ? "none" : "");
    for (auto& s : p.problems) std::printf("     %s\n", s.c_str());
}

void print_npdm(const Npdm& n, const Program& p, const std::vector<Facts>& fs) {
    std::printf("\n== main.npdm  \"%s\"%s%s\n", n.name.c_str(), n.product_code.empty() ? "" : " product ", n.product_code.c_str());
    std::printf("   flags %#x (%s-bit, address space type %d), main thread priority %u core %u stack %s, system resource %s, version %u, word at 0x10 %#x\n",
                n.flags, n.is_64bit() ? "64" : "32", n.address_space(), n.main_thread_priority, n.main_thread_core, hx(n.main_thread_stack_size).c_str(),
                hx(n.system_resource_size).c_str(), n.version, n.reserved_10);
    std::printf("   ACI0 program ID %016llx; ACID (magic at +%#x) program IDs %016llx..%016llx, flags %#x\n", static_cast<unsigned long long>(n.aci0.program_id),
                n.acid.magic_offset, static_cast<unsigned long long>(n.acid.program_id_min), static_cast<unsigned long long>(n.acid.program_id_max), n.acid.flags);
    const KernelCaps& k = n.aci0.kc;
    std::printf("   kernel capabilities: ACI0 %zu descriptors, ACID %zu, %s\n", k.raw.size(), n.acid.kc.raw.size(),
                k.raw == n.acid.kc.raw ? "identical" : (k.svcs == n.acid.kc.svcs ? "differ (same SVC mask)" : "DIFFER, SVC masks too"));
    if (k.has_thread_info)
        std::printf("     thread priority %u..%u, cores %u..%u\n", k.highest_priority, k.lowest_priority, k.min_core, k.max_core);
    std::printf("     program type %s, kernel version %s, handle table %s, misc flags %s, memory maps %d, I/O maps %d, interrupt pairs %d\n",
                k.program_type ? std::to_string(*k.program_type).c_str() : "-",
                k.kernel_major ? (std::to_string(*k.kernel_major) + "." + std::to_string(*k.kernel_minor)).c_str() : "-",
                k.handle_table_size ? std::to_string(*k.handle_table_size).c_str() : "-", k.misc_flags ? hx(*k.misc_flags).c_str() : "-", k.memory_maps,
                k.io_maps, k.interrupts);
    const std::set<uint32_t> allowed = svc_set(k.svcs);
    std::printf("     SVCs allowed (%zu): %s\n", allowed.size(), ranges(allowed).c_str());
    size_t servers = 0;
    for (auto& s : n.aci0.services) servers += s.server;
    std::printf("   services %zu (%zu as server); FS permissions ACI0 %#llx, ACID %#llx\n", n.aci0.services.size(), servers,
                static_cast<unsigned long long>(n.aci0.fs_permissions), static_cast<unsigned long long>(n.acid.fs_permissions));
    std::set<uint32_t> used_all;
    for (size_t i = 0; i < p.modules.size(); i++) {
        std::set<uint32_t> used, denied;
        for (auto& s : fs[i].svcs) used.insert(s.imm);
        if (used.empty()) continue;
        used_all.insert(used.begin(), used.end());
        for (uint32_t s : used)
            if (s >= k.svcs.size() || !k.svcs[s]) denied.insert(s);
        std::printf("   %s: %s SVC sites, %s distinct SVCs, %s not permitted by main.npdm%s%s\n", p.modules[i].name.c_str(), num(fs[i].svcs.size()).c_str(),
                    num(used.size()).c_str(), num(denied.size()).c_str(), denied.empty() ? "" : ": ", denied.empty() ? "" : ranges(denied).c_str());
    }
    std::set<uint32_t> unused;
    std::set_difference(allowed.begin(), allowed.end(), used_all.begin(), used_all.end(), std::inserter(unused, unused.end()));
    std::printf("   permitted but never issued: %s\n", ranges(unused).c_str());
}

void write_json(const std::filesystem::path& out, const std::filesystem::path& dir, const Program& p, const std::vector<Facts>& fs, const std::optional<Npdm>& npdm) {
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o) nxw::fail("cannot write " + out.string());
    Json j(o);
    const auto u8 = dir.u8string();
    j.obj().kv("format", "nxload-info").kv("version", 1).kv("exefs", std::string_view(reinterpret_cast<const char*>(u8.data()), u8.size()));
    j.key("modules").arr();
    for (size_t i = 0; i < p.modules.size(); i++) {
        const Module& m = p.modules[i];
        const Facts& f = fs[i];
        j.obj().kv("name", m.name).kv("build_id", m.build_id()).kv("base", p.bases[i]).kv("image_size", m.image_size()).kv("in_scope", bool(p.in_scope[i]));
        j.kv("file_size", m.file_size).kv("hashes_checked", m.hashes_checked).kv("bss_size", m.nso.bss_size);
        j.key("segments").obj();
        for (int s = 0; s < 3; s++) {
            const Segment& g = m.seg(s);
            j.key(seg_names[s]).obj().kv("mem_off", g.mem_off).kv("size", g.size).kv("stored", g.compressed ? g.file_size : g.size);
            j.kv("compressed", g.compressed).kv("hashed", g.hashed).end();
        }
        j.end();
        j.key("mod0").obj().kv("offset", m.mod0.offset).kv("dynamic", m.mod0.dynamic).kv("bss_start", m.mod0.bss_start).kv("bss_end", m.mod0.bss_end);
        j.kv("eh_frame_hdr_start", m.mod0.eh_frame_hdr_start).kv("eh_frame_hdr_end", m.mod0.eh_frame_hdr_end).kv("module_object", m.mod0.module_object).end();
        j.key("dynamic").arr();
        for (auto& [t, v] : m.dyn.tags) j.arr().val(dt_name(t)).val(v).end();
        j.end();
        j.key("needed").arr();
        for (auto& n : m.dyn.needed) j.val(n);
        j.end().kv("soname", m.dyn.soname);
        for (auto [tag, size_tag, name] : {std::tuple{DT_INIT_ARRAY, DT_INIT_ARRAYSZ, "init_array"}, std::tuple{DT_FINI_ARRAY, DT_FINI_ARRAYSZ, "fini_array"},
                                            std::tuple{DT_PREINIT_ARRAY, DT_PREINIT_ARRAYSZ, "preinit_array"}}) {
            j.key(name).arr();
            for (uint64_t a : array_entries(p, i, tag, size_tag)) j.val(a);
            j.end();
        }
        j.key("symbol_columns").arr().val("name").val("value").val("size").val("bind").val("type").val("visibility").val("shndx").end();
        j.key("symbols").arr();
        for (uint32_t s = 1; s < m.symbols.size(); s++) {
            const Symbol& y = m.symbols[s];
            j.arr().val(y.name).val(y.value).val(y.size).val(bind_name(y.bind)).val(type_name(y.type)).val(uint32_t(y.visibility)).val(uint32_t(y.shndx)).end();
        }
        j.end();
        j.key("binding_columns").arr().val("symbol").val("name").val("bound").val("provider").val("provider_symbol").val("address").end();
        j.key("bindings").arr();
        for (uint32_t s = 1; s < m.symbols.size(); s++) {
            const Resolution& r = p.resolution[i][s];
            if (r.kind == Bound::none || r.kind == Bound::self) continue;
            j.arr().val(s).val(m.symbols[s].name).val(bound_name(r.kind)).val(r.provider >= 0 ? p.modules[size_t(r.provider)].name : std::string());
            j.val(r.sym).val(r.address).end();
        }
        j.end();
        j.key("relocation_columns").arr().val("offset").val("type").val("symbol").val("addend").end();
        for (auto [name, table] : {std::pair{"rela", &m.rela}, std::pair{"jmprel", &m.jmprel}}) {
            j.key(name).arr();
            for (const Reloc& r : *table) j.arr().val(r.offset).val(reloc_name(r.type)).val(r.sym).val(r.addend).end();
            j.end();
        }
        j.kv("relr_count", uint64_t(m.relr.size()));
        j.key("relative_targets").obj();
        for (auto& [w, n] : f.rel_targets) j.kv(w, uint64_t(n));
        j.end();
        j.key("relative_text_targets").arr();
        for (uint64_t t : f.text_target_list) j.val(t);
        j.end();
        const EhFrame& e = f.eh;
        j.key("eh_frame").obj();
        j.key("hdr").obj().kv("version", uint32_t(e.hdr.version)).kv("ptr_enc", uint32_t(e.hdr.ptr_enc)).kv("count_enc", uint32_t(e.hdr.count_enc));
        j.kv("table_enc", uint32_t(e.hdr.table_enc)).kv("eh_frame", e.hdr.eh_frame).kv("fde_count", e.hdr.fde_count).end();
        j.kv("begin", e.begin).kv("end", e.end).kv("hdr_matches", e.hdr_matches);
        j.key("cies").arr();
        for (const Cie& c : e.cies) {
            j.obj().kv("offset", c.offset).kv("version", uint32_t(c.version)).kv("augmentation", c.augmentation).kv("code_align", c.code_align);
            j.kv("data_align", c.data_align).kv("ra_register", c.ra_register).kv("fde_enc", uint32_t(c.fde_enc)).kv("lsda_enc", uint32_t(c.lsda_enc));
            j.kv("personality_enc", uint32_t(c.personality_enc)).kv("personality", c.personality).kv("personality_name", personality(m, c));
            j.kv("insns", c.insns).kv("insns_size", c.insns_size).end();
        }
        j.end();
        j.key("fde_columns").arr().val("start").val("size").val("lsda").val("cie").val("offset").val("insns").val("insns_size").end();
        j.key("fdes").arr();
        for (const Fde& d : e.fdes) j.arr().val(d.start).val(d.size).val(d.lsda).val(d.cie).val(d.offset).val(d.insns).val(d.insns_size).end();
        j.end().end();
        j.key("svcs").arr();
        for (auto& s : f.svcs) j.arr().val(s.offset).val(s.imm).end();
        j.end();
        j.key("notes").arr();
        for (auto& n : m.notes) j.val(n);
        j.end().end();
    }
    j.end();
    j.key("problems").arr();
    for (auto& s : p.problems) j.val(s);
    j.end();
    if (npdm) {
        const Npdm& n = *npdm;
        j.key("npdm").obj().kv("name", n.name).kv("product_code", n.product_code).kv("flags", uint32_t(n.flags));
        j.kv("main_thread_priority", uint32_t(n.main_thread_priority)).kv("main_thread_core", uint32_t(n.main_thread_core));
        j.kv("main_thread_stack_size", n.main_thread_stack_size).kv("system_resource_size", n.system_resource_size).kv("version", n.version);
        for (auto [name, ac] : {std::pair{"aci0", &n.aci0}, std::pair{"acid", &n.acid}}) {
            j.key(name).obj().kv("program_id", ac->program_id).kv("program_id_min", ac->program_id_min).kv("program_id_max", ac->program_id_max);
            j.kv("flags", ac->flags).kv("fs_permissions", ac->fs_permissions);
            j.key("services").arr();
            for (auto& s : ac->services) j.arr().val(s.name).val(s.server).end();
            j.end().key("kernel_caps").arr();
            for (uint32_t w : ac->kc.raw) j.val(w);
            j.end().key("svcs").arr();
            for (uint32_t s : svc_set(ac->kc.svcs)) j.val(s);
            j.end().end();
        }
        j.end();
    }
    j.end();
}

[[noreturn]] void usage() {
    std::fprintf(stderr, "usage: nxload-info <exefs dir> [--json <file>] [--base <addr>] [--base-of <module>=<addr>] [--no-rtld] [--no-hash-check]\n");
    std::exit(2);
}

uint64_t parse_u64(const std::string& s) {
    size_t used = 0;
    const uint64_t v = std::stoull(s, &used, 0);
    if (used != s.size()) nxw::fail("not a number: " + s);
    return v;
}

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::filesystem::path dir, json;
        LoadOptions lopt;
        LinkOptions opt;
        bool rtld = true;
        for (int a = 1; a < argc; a++) {
            const std::wstring s = argv[a];
            auto next = [&] {
                if (a + 1 >= argc) usage();
                return argv[++a];
            };
            if (s == L"--json") json = next();
            else if (s == L"--base") opt.base = parse_u64(narrow(next()));
            else if (s == L"--base-of") {
                const std::string v = narrow(next());
                const size_t eq = v.find('=');
                if (eq == std::string::npos) usage();
                opt.bases[v.substr(0, eq)] = parse_u64(v.substr(eq + 1));
            } else if (s == L"--no-rtld") rtld = false;
            else if (s == L"--no-hash-check") lopt.verify_hashes = false;
            else if (s.starts_with(L"-") || !dir.empty()) usage();
            else dir = s;
        }
        if (dir.empty()) usage();
        const auto t0 = std::chrono::steady_clock::now();
        const Program p = load_program(dir, rtld, lopt, opt);
        std::vector<Facts> fs;
        for (size_t i = 0; i < p.modules.size(); i++) fs.push_back(facts(p, i));
        std::optional<Npdm> npdm;
        if (std::filesystem::is_regular_file(dir / "main.npdm")) npdm = parse_npdm(nxw::read_file(dir / "main.npdm"));
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        std::printf("nxload-info: %zu modules loaded, linked and relocated in %.2f s\n", p.modules.size(), secs);
        for (size_t i = 0; i < p.modules.size(); i++) print_module(p, i, fs[i]);
        print_link(p, fs);
        if (npdm) print_npdm(*npdm, p, fs);
        if (!json.empty()) {
            write_json(json, dir, p, fs, npdm);
            std::printf("\nJSON written to %s\n", reinterpret_cast<const char*>(json.u8string().c_str()));
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nxload-info: %s\n", e.what());
        return 1;
    }
}

#ifndef _WIN32
int main(int argc, char** argv) {   // the Mac build (docs/m4-swapin.md): the same with wide arguments
    std::vector<std::wstring> w;
    std::vector<wchar_t*> p;
    for (int i = 0; i < argc; i++) w.emplace_back(argv[i], argv[i] + std::strlen(argv[i]));
    for (auto& s : w) p.push_back(s.data());
    return wmain(argc, p.data());
}
#endif
