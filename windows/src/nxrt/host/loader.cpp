// from nxnative dm/host/loader.c (MIT, © seedds): the program's modules at their bases, symbol relocations bound the way Horizon's rtld binds them, host
// replacements by name (forced take-over first), segment permissions, module inits.
// Windows: nxload (src/nxload) reads, links and relocates the NSOs at fixed bases (main at 0x71_0000_0000); this file maps its relocated images into the
// modules' placeholder and rebinds the symbol relocations nxnative's bind_sym gives to the host: a forced name (overriding even the module's own
// definition), else an unresolved import the swhle table implements, else a named stub. Those slots receive host-call tokens (hle.c).
// Guest text is mapped read-only and not executable: the recompiled DLL runs instead, and the text is data for the interpreter and code readers.
// Gone with native execution: the rewriter's tables (fixups, gates, sites), the key page, TSD checks, patch areas, NRO pre-mapping (none for Pokopia).
#include "module.h"
#include "nxload/program.hpp"

#ifdef _WIN32
#include <windows.h>
#define strncasecmp _strnicmp
#else
#include <strings.h>
#include <errno.h>
#endif
#include "addrspace.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

extern "C" {
#include "hle_registry.h"
#include "nxrt/runtime.h"
void nxrt_logf(const char *fmt, ...);
uint64_t nxrt_guest_call(uint64_t guest_va, int n, const uint64_t *args);
module_t *g_mod;
int g_nmod;
}

using namespace nxload;

namespace {

std::unique_ptr<Program> g_prog;

// Each module's defined functions by address, built once after loading: nxrt_symbolize names guest addresses with them
struct SymAt { uint64_t addr, size; const char* name; };
std::vector<std::vector<SymAt>> g_syms;
void build_symbol_index() {
    g_syms.assign(g_prog->modules.size(), {});
    for (size_t i = 0; i < g_prog->modules.size(); i++) {
        for (const Symbol& s : g_prog->modules[i].symbols)
            if (s.defined() && !s.name.empty() && s.type == STT_FUNC) g_syms[i].push_back({g_prog->bases[i] + s.value, s.size, s.name.c_str()});
        std::sort(g_syms[i].begin(), g_syms[i].end(), [](const SymAt& a, const SymAt& b) { return a.addr < b.addr; });
    }
}

uint64_t& slot64(std::vector<uint8_t>& img, uint64_t off) { return *reinterpret_cast<uint64_t*>(img.data() + off); }

// nxnative's bind_sym, for the slots that leave the guest: forced take-over, then (for names no module defines) swhle, then a named stub
int bind_host(Program& p) {
    int bound = 0, forced = 0, swhle = 0, stubs = 0;
    for (size_t mi = 0; mi < p.modules.size(); mi++) {
        const Module& m = p.modules[mi];
        for (const auto* t : {&m.rela, &m.jmprel})
            for (const Reloc& r : *t) {
                if (!r.sym || (r.type != R_AARCH64_ABS64 && r.type != R_AARCH64_GLOB_DAT && r.type != R_AARCH64_JUMP_SLOT)) continue;
                const Symbol& s = m.symbols[r.sym];
                const Resolution& res = p.resolution[mi][r.sym];
                uint64_t tok = 0;
                if (nxrt_hle_find(s.name.c_str(), NXRT_HLE_FORCED)) tok = nxrt_token_of(s.name.c_str()), forced++;
                else if (res.kind == Bound::unresolved || (res.kind == Bound::weak_zero && nxrt_hle_find(s.name.c_str(), NXRT_HLE_SWHLE))) {
                    if (nxrt_hle_find(s.name.c_str(), NXRT_HLE_SWHLE)) tok = nxrt_token_of(s.name.c_str()), swhle++;
                    else tok = resolve_import(s.name.c_str()), stubs++;
                }
                if (!tok) continue;
                slot64(p.memory[mi], r.offset) = tok + (r.type == R_AARCH64_ABS64 ? uint64_t(r.addend) : 0);
                bound++;
            }
    }
    nxrt_logf("host replacements bound: %d relocation slots (%d forced, %d swhle, %d named stubs)", bound, forced, swhle, stubs);
    return bound;
}

}  // namespace

extern "C" int nxrt_load_modules(const char* exefs) {
    try {
        auto p = std::make_unique<Program>(load_program(exefs, false));
        for (const std::string& s : p->problems) nxrt_logf("[RED] nxload: %s", s.c_str());
        if (p->modules.empty()) { nxrt_logf("[RED] no modules in %s", exefs); return -1; }
        if (p->bases[0] != NXRT_MODULE_BASE || p->end() > NXRT_MODULE_BASE + NXRT_MODULE_REGION_SIZE) {
            nxrt_logf("[RED] modules at %#llx-%#llx do not fit the module region", (unsigned long long)p->bases[0], (unsigned long long)p->end());
            return -1;
        }
        if (!nxrt_as_range(NXRT_AS_MODULES)->reserved) { nxrt_logf("[RED] the module region is not reserved"); return -1; }
        nxrt_hle_note_bound(bind_host(*p));
        g_nmod = int(p->modules.size());
        g_mod = static_cast<module_t*>(calloc(size_t(g_nmod), sizeof(module_t)));
        for (int i = 0; i < g_nmod; i++) {
            const Module& m = p->modules[size_t(i)];
            module_t& o = g_mod[i];
            const uint64_t base = p->bases[size_t(i)], size = align_up(m.image_size(), page_size);
            snprintf(o.name, sizeof o.name, "%s", m.name.c_str());
            o.base = reinterpret_cast<uint8_t*>(base);
            o.size = size;
            o.text_end = m.seg_end(seg_text); o.ro_off = m.seg(seg_ro).mem_off; o.data_off = m.seg(seg_data).mem_off;
            std::string id;
            for (int k = 0; k < 20; k++) { char h[3]; snprintf(h, sizeof h, "%02x", m.nso.module_id[size_t(k)]); id += h; }
            snprintf(o.build_id, sizeof o.build_id, "%s", id.c_str());
            o.init = m.dyn.value(DT_INIT); o.fini = m.dyn.value(DT_FINI);
            o.init_array = m.dyn.value(DT_INIT_ARRAY); o.init_arraysz = m.dyn.value(DT_INIT_ARRAYSZ);
            if (as_commit_private(base, size, AS_RW)) { nxrt_logf("[RED] cannot map %s at %#llx (error %lu)", o.name, (unsigned long long)base, (unsigned long)errno); return -1; }
            memcpy(o.base, p->memory[size_t(i)].data(), m.image_size());
            // text and ro read-only (guest text is data here: never executable), data and bss read-write
            if (as_protect(base, align_up(o.data_off, page_size), AS_RO)) { nxrt_logf("[RED] cannot protect %s", o.name); return -1; }
            o.loaded = 1;
            nxrt_logf("%s: %#llx-%#llx text %#llx ro %#llx data %#llx, build %s", o.name, (unsigned long long)base, (unsigned long long)(base + size),
                      (unsigned long long)o.text_end, (unsigned long long)o.ro_off, (unsigned long long)o.data_off, o.build_id);
        }
        g_prog = std::move(p);
        for (auto& img : g_prog->memory) std::vector<uint8_t>().swap(img);   // the mapped copies are the images now
        build_symbol_index();
        return 0;
    } catch (const std::exception& e) {
        nxrt_logf("[RED] loading %s: %s", exefs, e.what());
        return -1;
    }
}

extern "C" {

module_t* module_of(const void* p) {
    const uint8_t* a = static_cast<const uint8_t*>(p);
    for (int i = 0; i < g_nmod; i++) if (a >= g_mod[i].base && a < g_mod[i].base + g_mod[i].size) return &g_mod[i];
    return nullptr;
}
void* module_sym(module_t* m, const char* name) {
    if (!g_prog || !m) return nullptr;
    const int i = g_prog->index(m->name);
    if (i < 0) return nullptr;
    const uint32_t s = g_prog->modules[size_t(i)].lookup(name);
    return s ? reinterpret_cast<void*>(g_prog->bases[size_t(i)] + g_prog->modules[size_t(i)].symbols[s].value) : nullptr;
}
// The first module in load order that exports the name (nxnative's loaded_sym)
void* guest_sym(const char* name) {
    if (!g_prog) return nullptr;
    auto f = g_prog->find(name);
    return f ? reinterpret_cast<void*>(g_prog->bases[size_t(f->first)] + g_prog->modules[size_t(f->first)].symbols[f->second].value) : nullptr;
}
uint64_t nxrt_guest_sym(const char* name) { return reinterpret_cast<uint64_t>(guest_sym(name)); }
void nxrt_symbolize(uint64_t a, char* out, size_t n) {
    module_t* m = module_of(reinterpret_cast<void*>(a));
    if (!m) {
        const char* tok = nxrt_token_name(a);
        if (a == NXRT_RETURN_TO_HOST) snprintf(out, n, "host:return");
        else if (tok) snprintf(out, n, "host:%s", tok);
        else snprintf(out, n, "%#llx", static_cast<unsigned long long>(a));
        return;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(m->base);
    const size_t k = size_t(m - g_mod);
    if (k < g_syms.size()) {
        const auto& v = g_syms[k];
        auto it = std::upper_bound(v.begin(), v.end(), a, [](uint64_t x, const SymAt& s) { return x < s.addr; });
        if (it != v.begin() && a - (it - 1)->addr < ((it - 1)->size ? (it - 1)->size : 0x1000)) {
            const SymAt& s = *(it - 1);
            if (a == s.addr) snprintf(out, n, "%s+%#llx %s", m->name, static_cast<unsigned long long>(a - base), s.name);
            else snprintf(out, n, "%s+%#llx %s+%#llx", m->name, static_cast<unsigned long long>(a - base), s.name, static_cast<unsigned long long>(a - s.addr));
            return;
        }
    }
    snprintf(out, n, "%s+%#llx", m->name, static_cast<unsigned long long>(a - base));
}
int nxrt_module_count(void) { return g_nmod; }
int nxrt_module_get(int i, nxrt_module_info* o) {
    if (i < 0 || i >= g_nmod) return -1;
    const module_t& m = g_mod[i];
    *o = {m.name, reinterpret_cast<uint64_t>(m.base), m.size, m.text_end, m.ro_off, m.data_off, m.build_id, m.init};
    return 0;
}
int dm_main_imports(const char* prefix) {
    if (!g_prog) return 0;
    const int i = g_prog->index("main");
    if (i < 0) return 0;
    for (const Symbol& s : g_prog->modules[size_t(i)].symbols) if (s.imported() && s.name.rfind(prefix, 0) == 0) return 1;
    return 0;
}
// SDK major version, from the version string the SDK's libraries keep in the sdk module's read-only data ("NintendoSDK_nnSdk-10_4_0"; SDK 12 spells it NintendoSdk_;
// the Switch 2's adds "Generic_": "NintendoSdk_nnSdk-Generic_20_5_25", which nxnative's parser does not read)
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = reinterpret_cast<const char*>(g_mod[i].base + g_mod[i].ro_off), *e = reinterpret_cast<const char*>(g_mod[i].base + g_mod[i].size);
        for (const char* p = b; p + 7 < e; p++)
            if (!memcmp(p, "_nnSdk-", 7) && p - b >= 11 && !strncasecmp(p - 11, "NintendoSDK", 11)) {
                const char* v = p + 7;
                if (!strncmp(v, "Generic_", 8)) v += 8;
                if (*v < '0' || *v > '9') continue;
                major = atoi(v); nxrt_logf("sdk: NintendoSDK nnSdk-%.20s (major %d)", p + 7, major); break; }
    }
    if (!major) nxrt_logf("[RED] sdk: no SDK version string in the sdk module; SDK-dependent behaviour takes the newer SDKs' side");
    return major;
}
// rtld's callback B: each module's DT_INIT (global constructors), as guest calls
void run_module_inits(void) {
    for (int i = 0; i < g_nmod; i++) {
        module_t* m = &g_mod[i];
        if (!m->init || m->nro) continue;
        nxrt_logf("%s: DT_INIT +%#llx (init_array %llu entries)", m->name, (unsigned long long)m->init, (unsigned long long)(m->init_arraysz / 8));
        nxrt_guest_call(reinterpret_cast<uint64_t>(m->base) + m->init, 0, nullptr);
    }
}

}  // extern "C"
