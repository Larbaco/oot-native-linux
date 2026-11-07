// LSDA (.gcc_except_table) parsing, per the Itanium C++ ABI's exception-handling tables as LLVM emits them: a header (LPStart,
// TType and call-site encodings), the call-site table, the action table, and the type table that ends at the TType base.
#include "lsda.hpp"

#include <cstdio>

namespace nxanalyze {

using namespace nxload;

namespace {

uint8_t byte(std::span<const uint8_t> b, size_t& p) {
    if (p >= b.size()) nxw::fail("LSDA runs past the image");
    return b[p++];
}

size_t encoded_size(uint8_t enc) {
    switch (enc & 0x0f) {
    case DW_EH_PE_absptr: case DW_EH_PE_udata8: case DW_EH_PE_sdata8: return 8;
    case DW_EH_PE_udata4: case DW_EH_PE_sdata4: return 4;
    case DW_EH_PE_udata2: case DW_EH_PE_sdata2: return 2;
    default: nxw::fail("LSDA: type table encoding " + hex(enc) + " has no fixed size");
    }
}

}  // namespace

std::string type_name_at(const ModCtx& c, uint64_t slot) {
    auto r = c.rel_at.find(uint32_t(slot));
    if (r == c.rel_at.end()) return c.read64_rel(uint32_t(slot)) ? "?" : "";
    if (r->second.sym) return c.symbol_name(r->second.sym);
    const uint64_t a = nxw::load_le<uint64_t>(c.img, size_t(slot));   // RELR: the implicit addend is the target's offset
    if (auto s = c.sym_at.find(uint32_t(a)); s != c.sym_at.end()) return c.symbol_name(s->second);
    return "typeinfo@" + hex(a);
}

Lsda parse_lsda(const ModCtx& c, uint32_t lsda, uint32_t fn_start) {
    Lsda out;
    const auto img = c.img;
    size_t p = lsda;
    const uint8_t lp_enc = byte(img, p);
    out.lp_start = fn_start;
    if (lp_enc != DW_EH_PE_omit) out.lp_start = read_encoded(img, p, lp_enc & 0x7f);
    out.ttype_enc = byte(img, p);
    if (out.ttype_enc != DW_EH_PE_omit) {
        const uint64_t off = read_uleb(img, p);
        out.ttype_base = p + off;
    }
    const uint8_t cs_enc = byte(img, p);
    const uint64_t cs_len = read_uleb(img, p);
    const size_t cs_end = p + size_t(cs_len);
    out.actions = cs_end;
    while (p < cs_end) {
        LsdaCallSite s;
        s.start = fn_start + read_encoded(img, p, cs_enc & 0x0f);
        s.size = read_encoded(img, p, cs_enc & 0x0f);
        const uint64_t lp = read_encoded(img, p, cs_enc & 0x0f);
        s.landing_pad = lp ? out.lp_start + lp : 0;
        s.action = read_uleb(img, p);
        if (s.action) {
            size_t q = out.actions + size_t(s.action - 1);
            s.first_filter = read_sleb(img, q);
            if (s.first_filter > 0 && out.ttype_enc != DW_EH_PE_omit) {
                const size_t at = size_t(out.ttype_base - uint64_t(s.first_filter) * encoded_size(out.ttype_enc));
                size_t t = at;
                const uint64_t raw = read_encoded(img, t, out.ttype_enc & 0x0f);   // 0 is catch (...), before any application
                t = at;
                const uint64_t v = raw ? read_encoded(img, t, out.ttype_enc & 0x7f) : 0;
                s.catch_all = raw == 0;
                if (v) s.first_type = (out.ttype_enc & DW_EH_PE_indirect) ? type_name_at(c, v) : "typeinfo@" + hex(v);
            }
            // the whole action chain: cleanup, catch or filter clauses
            size_t q2 = out.actions + size_t(s.action - 1);
            for (int guard = 0; guard < 64; guard++) {
                const int64_t filter = read_sleb(img, q2);
                const size_t here = q2;
                const int64_t next = read_sleb(img, q2);
                if (filter > 0) s.catches++;
                else if (filter < 0) s.filters++;
                else s.cleanup = true;
                if (!next) break;
                q2 = size_t(int64_t(here) + next);
            }
        } else {
            s.cleanup = s.landing_pad != 0;
        }
        out.sites.push_back(std::move(s));
    }
    return out;
}

}  // namespace nxanalyze
