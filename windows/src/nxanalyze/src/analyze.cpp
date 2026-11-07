#include "internal.hpp"

#include <cstring>

namespace nxanalyze {

using namespace nxload;

const char* indirect_class_name(uint8_t c) {
    static const char* const n[ic_count] = {
        "jump table", "jump table (bound not found, entries read until one leaves the function)", "absolute table, labels",
        "table of function pointers (indexed tail call)", "PLT stub", "vtable-style ([[obj+o]+slot])", "vtable-style, object from the stack",
        "GOT / global function pointer (adrp; ldr)", "pointer from a global object (adrp; ldr; ldr)", "pointer from memory, base from an argument or computed",
        "register-indexed pointer table", "pointer from ldp", "reloaded from the stack", "incoming argument register", "call result",
        "constant (adr / adrp+add)", "acquire load", "csel", "return through another register", "context restore (loaded with SP or x30)", "other",
    };
    return c < ic_count ? n[c] : "?";
}

const char* site_kind_name(uint8_t k) {
    static const char* const n[site_count] = {
        "svc", "mrs", "msr", "msr (immediate)", "sys", "udf", "brk", "hlt", "wfe", "wfi", "yield", "sev", "sevl", "load exclusive",
        "store exclusive", "clrex", "SP from a non-stack value", "dynamic stack allocation", "return address (x30) not the caller's",
        "ret through another register", "branch through a loaded context", "call to setjmp", "call to longjmp", "PAC data / strip",
        "unresolved direct branch",
    };
    return k < site_count ? n[k] : "?";
}

const char* binding_name(uint8_t b) {
    switch (b) {
    case bind_guest: return "guest";
    case bind_forced: return "forced";
    case bind_swhle: return "swhle";
    case bind_stub: return "stub";
    case bind_weak_zero: return "weak_zero";
    default: return "none";
    }
}

Analysis analyze(const Program& p, const Options& opt) {
    Analysis a;
    Database& db = a.db;
    Ctx x;
    x.p = &p, x.opt = &opt, x.out = &a;
    if (p.memory.size() != p.modules.size()) nxw::fail("nxanalyze needs the relocated images (LinkOptions::relocate)");
    for (size_t i = 0; i < p.modules.size(); i++) {
        if (p.modules[i].name == "rtld") continue;   // nxload and the runtime do rtld's work; it is not recompiled
        ModCtx c;
        c.index = int(i), c.m = &p.modules[i], c.base = p.bases[i], c.img = p.modules[i].image, c.rel = p.memory[i];
        x.mods.push_back(std::move(c));
    }
    if (!opt.hle_inventory.empty()) x.hle = load_hle_inventory(opt.hle_inventory);
    else if (!opt.nxnative.empty()) x.hle = load_hle(opt.nxnative);
    for (auto& c : x.mods) decode_module(c), discover(x, c);
    add_cross_module_seeds(x);
    uint32_t next = 0;
    for (auto& c : x.mods) {
        finish_discovery(x, c);
        c.first_fn = next;
        next += uint32_t(c.fns.size());
        ModuleRec r{};
        r.name = db.add_string(c.m->name), r.text_size = c.text, r.base = c.base, r.image_size = uint32_t(c.m->image_size());
        std::memcpy(r.build_id, c.m->nso.module_id.data(), sizeof r.build_id);
        db.modules.push_back(r);
    }
    mark_noreturn(x);
    bind_symbols(x);
    for (size_t i = 0; i < x.mods.size(); i++) {
        db.modules[i].first_function = uint32_t(db.functions.size());
        analyze_functions(x, x.mods[i]);
        db.modules[i].function_count = uint32_t(db.functions.size()) - db.modules[i].first_function;
    }
    if (!opt.traces.empty()) check_traces(x);
    if (!opt.rewriter.empty()) check_rewriter(x);
    for (auto& c : x.mods) {
        a.stats.push_back(c.stat);
        a.notes.push_back(c.notes);
    }
    for (const auto& s : p.problems) a.problems.push_back(s);
    return a;
}

}  // namespace nxanalyze
