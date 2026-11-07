// .eh_frame layout per the LSB "Exception Frames" chapter and the DWARF CIE/FDE format; the LSDA rule follows LLVM libunwind.
#include "nxload/ehframe.hpp"

#include <algorithm>
#include <cstdio>

namespace nxload {

using nxw::load_le;

namespace {

std::string hex(uint64_t v) {
    char s[24];
    std::snprintf(s, sizeof s, "%#llx", static_cast<unsigned long long>(v));
    return s;
}

void need(bool ok, const std::string& what) {
    if (!ok) fail(what);
}

uint8_t byte(std::span<const uint8_t> b, size_t& p) {
    need(p < b.size(), "read past the end at " + hex(p));
    return b[p++];
}

bool is_z(const std::string& aug) { return !aug.empty() && aug[0] == 'z'; }

Cie parse_cie(std::span<const uint8_t> rec, uint64_t off, uint64_t body) {
    Cie c;
    c.offset = off;
    size_t p = size_t(body) + 4;
    c.version = byte(rec, p);
    need(c.version == 1 || c.version == 3 || c.version == 4, "CIE at " + hex(off) + ": version " + std::to_string(c.version));
    const auto nul = std::find(rec.begin() + ptrdiff_t(p), rec.end(), uint8_t(0));
    need(nul != rec.end(), "CIE at " + hex(off) + ": unterminated augmentation");
    c.augmentation.assign(rec.begin() + ptrdiff_t(p), nul);
    p = size_t(nul - rec.begin()) + 1;
    need(c.augmentation.empty() || is_z(c.augmentation), "CIE at " + hex(off) + ": augmentation \"" + c.augmentation + "\" without 'z'");
    if (c.version == 4) {
        const uint8_t addr_size = byte(rec, p), seg_size = byte(rec, p);
        need(addr_size == 8 && seg_size == 0, "CIE at " + hex(off) + ": address size " + std::to_string(addr_size));
    }
    c.code_align = read_uleb(rec, p);
    c.data_align = read_sleb(rec, p);
    c.ra_register = c.version == 1 ? byte(rec, p) : read_uleb(rec, p);
    if (is_z(c.augmentation)) {
        const uint64_t len = read_uleb(rec, p);
        need(len <= rec.size() - p, "CIE at " + hex(off) + ": augmentation data runs past the record");
        const size_t aug_end = p + size_t(len);
        auto data = rec.first(aug_end);
        for (char ch : std::string_view(c.augmentation).substr(1)) {
            if (ch == 'R') c.fde_enc = byte(data, p);
            else if (ch == 'L') c.lsda_enc = byte(data, p);
            else if (ch == 'P') {
                c.personality_enc = byte(data, p);
                c.personality = read_encoded(data, p, c.personality_enc & 0x7f);
            } else if (ch == 'S') c.signal_frame = true;
            else if (ch == 'B') c.b_key = true;
            else if (ch == 'G') c.mte_tagged = true;
            else break;   // unknown: the rest is skipped by length
        }
        p = aug_end;
    }
    c.insns = p;
    c.insns_size = rec.size() - p;
    return c;
}

Fde parse_fde(std::span<const uint8_t> rec, uint64_t off, uint64_t body, const Cie& c, uint32_t cie_index) {
    Fde f;
    f.offset = off;
    f.cie = cie_index;
    size_t p = size_t(body) + 4;
    f.start = read_encoded(rec, p, c.fde_enc & 0x7f);
    f.size = read_encoded(rec, p, c.fde_enc & 0x0f);
    if (is_z(c.augmentation)) {
        const uint64_t len = read_uleb(rec, p);
        need(len <= rec.size() - p, "FDE at " + hex(off) + ": augmentation data runs past the record");
        const size_t aug_end = p + size_t(len);
        if (c.lsda_enc != DW_EH_PE_omit) {
            auto data = rec.first(aug_end);
            size_t q = p;
            if (read_encoded(data, q, c.lsda_enc & 0x0f) != 0) {   // a zero raw value means no LSDA, whatever the application
                q = p;
                f.lsda = read_encoded(data, q, c.lsda_enc & 0x7f);
            }
        }
        p = aug_end;
    }
    f.insns = p;
    f.insns_size = rec.size() - p;
    return f;
}

}  // namespace

uint64_t read_encoded(std::span<const uint8_t> b, size_t& p, uint8_t enc, uint64_t datarel_base) {
    need(enc != DW_EH_PE_omit, "read_encoded: the pointer is omitted");
    const size_t at = p;
    uint64_t v = 0;
    switch (enc & 0x0f) {
    case DW_EH_PE_absptr:
    case DW_EH_PE_udata8:
    case DW_EH_PE_sdata8: v = load_le<uint64_t>(b, p), p += 8; break;
    case DW_EH_PE_udata4: v = load_le<uint32_t>(b, p), p += 4; break;
    case DW_EH_PE_sdata4: v = uint64_t(int64_t(load_le<int32_t>(b, p))), p += 4; break;
    case DW_EH_PE_udata2: v = load_le<uint16_t>(b, p), p += 2; break;
    case DW_EH_PE_sdata2: v = uint64_t(int64_t(load_le<int16_t>(b, p))), p += 2; break;
    case DW_EH_PE_uleb128: v = read_uleb(b, p); break;
    case DW_EH_PE_sleb128: v = uint64_t(read_sleb(b, p)); break;
    default: fail("pointer encoding " + hex(enc) + ": unknown format");
    }
    switch (enc & 0x70) {
    case 0: break;
    case DW_EH_PE_pcrel: v += at; break;
    case DW_EH_PE_datarel: v += datarel_base; break;
    default: fail("pointer encoding " + hex(enc) + ": unsupported application");
    }
    return v;
}

size_t EhFrame::lsda_count() const {
    return size_t(std::count_if(fdes.begin(), fdes.end(), [](const Fde& f) { return f.lsda != 0; }));
}

EhFrame parse_eh_frame(std::span<const uint8_t> img, uint64_t hdr_start, uint64_t hdr_end, uint64_t limit) {
    EhFrame e;
    need(hdr_start + 4 <= hdr_end && hdr_end <= img.size(), ".eh_frame_hdr out of range");
    auto hb = img.first(size_t(hdr_end));
    size_t p = size_t(hdr_start);
    EhFrameHdr& h = e.hdr;
    h.version = byte(hb, p);
    h.ptr_enc = byte(hb, p);
    h.count_enc = byte(hb, p);
    h.table_enc = byte(hb, p);
    need(h.version == 1, ".eh_frame_hdr version " + std::to_string(h.version));
    h.eh_frame = read_encoded(hb, p, h.ptr_enc & 0x7f, hdr_start);
    if (h.count_enc != DW_EH_PE_omit && h.table_enc != DW_EH_PE_omit) {
        h.fde_count = read_encoded(hb, p, h.count_enc & 0x7f, hdr_start);
        need(h.fde_count <= hb.size() - p, ".eh_frame_hdr: FDE count " + std::to_string(h.fde_count) + " cannot fit");
        h.table.resize(size_t(h.fde_count));
        for (auto& [loc, fde] : h.table) {
            loc = read_encoded(hb, p, h.table_enc & 0x7f, hdr_start);
            fde = read_encoded(hb, p, h.table_enc & 0x7f, hdr_start);
        }
    }

    limit = std::min<uint64_t>(limit, img.size());
    e.begin = h.eh_frame;
    need(e.begin < limit, ".eh_frame at " + hex(e.begin) + " is past " + hex(limit));
    auto lb = img.first(size_t(limit));
    std::map<uint64_t, uint32_t> cie_at;
    uint64_t o = e.begin;
    while (o + 4 <= limit) {
        uint64_t len = load_le<uint32_t>(lb, size_t(o)), hdr = 4;
        if (len == 0) {
            o += 4;
            break;
        }
        if (len == 0xffffffff) len = load_le<uint64_t>(lb, size_t(o + 4)), hdr = 12;
        const uint64_t body = o + hdr;
        need(len >= 4 && len <= limit - body, ".eh_frame record at " + hex(o) + " runs past " + hex(limit));
        auto rec = lb.first(size_t(body + len));
        const uint32_t id = load_le<uint32_t>(rec, size_t(body));
        if (id == 0) {
            cie_at[o] = uint32_t(e.cies.size());
            e.cies.push_back(parse_cie(rec, o, body));
        } else {
            auto it = id <= body ? cie_at.find(body - id) : cie_at.end();
            need(it != cie_at.end(), "FDE at " + hex(o) + " names no CIE");
            e.fdes.push_back(parse_fde(rec, o, body, e.cies[it->second], it->second));
        }
        o = body + len;
    }
    e.end = o;

    // The search table must name every walked FDE once, sorted by location, each entry pointing at the FDE for that location
    std::map<uint64_t, uint64_t> start_of;
    for (const Fde& f : e.fdes) start_of[f.offset] = f.start;
    std::vector<uint64_t> walked, table;
    for (const Fde& f : e.fdes) walked.push_back(f.start);
    for (auto& [loc, fde] : h.table) table.push_back(loc);
    std::sort(walked.begin(), walked.end());
    e.hdr_matches = std::is_sorted(table.begin(), table.end()) && table == walked &&
                    std::all_of(h.table.begin(), h.table.end(), [&](auto& t) {
                        auto it = start_of.find(t.second);
                        return it != start_of.end() && it->second == t.first;
                    });
    return e;
}

EhFrame parse_eh_frame(const Module& m) {
    const uint64_t hs = m.mod0.eh_frame_hdr_start;
    if (hs == m.mod0.eh_frame_hdr_end) return {};
    uint64_t limit = m.image_size();
    for (int s = 0; s < 3; s++)
        if (hs >= m.seg(s).mem_off && hs < m.seg_end(s)) limit = m.seg_end(s);
    return parse_eh_frame(m.image, hs, m.mod0.eh_frame_hdr_end, limit);
}

}  // namespace nxload
