#include "nxload/module.hpp"

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

template <class T> T rd(std::span<const uint8_t> b, uint64_t off) {
    if (off > b.size()) fail("read past the end at " + hex(off));
    return load_le<T>(b, size_t(off));
}

void need(bool ok, const std::string& what) {
    if (!ok) fail(what);
}

}  // namespace

uint32_t elf_hash(std::string_view s) {
    uint32_t h = 0;
    for (unsigned char c : s) {
        h = (h << 4) + c;
        const uint32_t g = h & 0xf0000000;
        h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

uint32_t gnu_hash(std::string_view s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = h * 33 + c;
    return h;
}

uint64_t read_uleb(std::span<const uint8_t> b, size_t& p) {
    uint64_t r = 0;
    for (unsigned sh = 0;; sh += 7) {
        need(p < b.size(), "uleb128 past the end");
        const uint8_t c = b[p++];
        need(sh < 64 && (sh < 57 || (c & 0x7f) >> (64 - sh) == 0), "uleb128 overflows 64 bits");
        r |= uint64_t(c & 0x7f) << sh;
        if (!(c & 0x80)) return r;
    }
}

int64_t read_sleb(std::span<const uint8_t> b, size_t& p) {
    uint64_t r = 0;
    unsigned sh = 0;
    uint8_t c;
    do {
        need(p < b.size(), "sleb128 past the end");
        need(sh < 64, "sleb128 overflows 64 bits");
        c = b[p++];
        r |= uint64_t(c & 0x7f) << sh;
        sh += 7;
    } while (c & 0x80);
    if (sh < 64 && (c & 0x40)) r |= ~uint64_t(0) << sh;
    return int64_t(r);
}

std::vector<uint64_t> decode_relr(std::span<const uint8_t> t) {
    need(t.size() % 8 == 0, "DT_RELR: size is not a multiple of 8");
    std::vector<uint64_t> out;
    uint64_t where = 0;
    bool started = false;
    for (size_t i = 0; i < t.size(); i += 8) {
        const uint64_t e = load_le<uint64_t>(t, i);
        if (!(e & 1)) {
            out.push_back(e);
            where = e + 8;
            started = true;
            continue;
        }
        need(started, "DT_RELR: bitmap entry with no address before it");
        for (unsigned b = 1; b < 64; b++)
            if (e >> b & 1) out.push_back(where + (b - 1) * 8);
        where += 63 * 8;
    }
    return out;
}

std::string dt_name(int64_t tag) {
    static const std::map<int64_t, const char*> names = {
        {DT_NULL, "NULL"}, {DT_NEEDED, "NEEDED"}, {DT_PLTRELSZ, "PLTRELSZ"}, {DT_PLTGOT, "PLTGOT"}, {DT_HASH, "HASH"},
        {DT_STRTAB, "STRTAB"}, {DT_SYMTAB, "SYMTAB"}, {DT_RELA, "RELA"}, {DT_RELASZ, "RELASZ"}, {DT_RELAENT, "RELAENT"},
        {DT_STRSZ, "STRSZ"}, {DT_SYMENT, "SYMENT"}, {DT_INIT, "INIT"}, {DT_FINI, "FINI"}, {DT_SONAME, "SONAME"}, {15, "RPATH"},
        {DT_SYMBOLIC, "SYMBOLIC"}, {DT_REL, "REL"}, {18, "RELSZ"}, {19, "RELENT"}, {DT_PLTREL, "PLTREL"}, {DT_DEBUG, "DEBUG"},
        {DT_TEXTREL, "TEXTREL"}, {DT_JMPREL, "JMPREL"}, {DT_BIND_NOW, "BIND_NOW"}, {DT_INIT_ARRAY, "INIT_ARRAY"},
        {DT_FINI_ARRAY, "FINI_ARRAY"}, {DT_INIT_ARRAYSZ, "INIT_ARRAYSZ"}, {DT_FINI_ARRAYSZ, "FINI_ARRAYSZ"}, {29, "RUNPATH"},
        {DT_FLAGS, "FLAGS"}, {DT_PREINIT_ARRAY, "PREINIT_ARRAY"}, {DT_PREINIT_ARRAYSZ, "PREINIT_ARRAYSZ"}, {DT_RELRSZ, "RELRSZ"},
        {DT_RELR, "RELR"}, {DT_RELRENT, "RELRENT"}, {DT_GNU_HASH, "GNU_HASH"}, {DT_RELACOUNT, "RELACOUNT"}, {DT_RELCOUNT, "RELCOUNT"},
        {DT_FLAGS_1, "FLAGS_1"}, {0x6ffffff0, "VERSYM"}, {0x6ffffffe, "VERNEED"}, {0x6fffffff, "VERNEEDNUM"},
    };
    auto it = names.find(tag);
    return it != names.end() ? it->second : hex(uint64_t(tag));
}

std::string reloc_name(uint32_t type) {
    switch (type) {
    case R_AARCH64_NONE: return "NONE";
    case R_AARCH64_ABS64: return "ABS64";
    case R_AARCH64_ABS32: return "ABS32";
    case R_AARCH64_PREL64: return "PREL64";
    case R_AARCH64_COPY: return "COPY";
    case R_AARCH64_GLOB_DAT: return "GLOB_DAT";
    case R_AARCH64_JUMP_SLOT: return "JUMP_SLOT";
    case R_AARCH64_RELATIVE: return "RELATIVE";
    case R_AARCH64_TLS_DTPMOD64: return "TLS_DTPMOD64";
    case R_AARCH64_TLS_DTPREL64: return "TLS_DTPREL64";
    case R_AARCH64_TLS_TPREL64: return "TLS_TPREL64";
    case R_AARCH64_TLSDESC: return "TLSDESC";
    case R_AARCH64_IRELATIVE: return "IRELATIVE";
    }
    return "type" + std::to_string(type);
}

std::string bind_name(uint8_t b) {
    switch (b) {
    case STB_LOCAL: return "local";
    case STB_GLOBAL: return "global";
    case STB_WEAK: return "weak";
    }
    return "bind" + std::to_string(b);
}

std::string type_name(uint8_t t) {
    switch (t) {
    case STT_NOTYPE: return "notype";
    case STT_OBJECT: return "object";
    case STT_FUNC: return "func";
    case STT_SECTION: return "section";
    case STT_FILE: return "file";
    case STT_COMMON: return "common";
    case STT_TLS: return "tls";
    }
    return "type" + std::to_string(t);
}

std::optional<uint64_t> Dynamic::get(int64_t tag) const {
    for (auto& [t, v] : tags)
        if (t == tag) return v;
    return std::nullopt;
}

size_t Dynamic::count(int64_t tag) const {
    return size_t(std::count_if(tags.begin(), tags.end(), [&](auto& t) { return t.first == tag; }));
}

NsoHeader parse_nso_header(std::span<const uint8_t> f) {
    need(f.size() >= 0x100 && std::memcmp(f.data(), "NSO0", 4) == 0, "not an NSO0 file");
    NsoHeader h;
    h.version = rd<uint32_t>(f, 0x04);
    h.flags = rd<uint32_t>(f, 0x0c);
    for (int i = 0; i < 3; i++) {
        Segment& s = h.seg[i];
        s.file_off = rd<uint32_t>(f, 0x10 + 0x10 * i);
        s.mem_off = rd<uint32_t>(f, 0x14 + 0x10 * i);
        s.size = rd<uint32_t>(f, 0x18 + 0x10 * i);
        s.file_size = rd<uint32_t>(f, 0x60 + 4 * i);
        s.compressed = h.flags >> i & 1;
        s.hashed = h.flags >> (3 + i) & 1;
        std::memcpy(s.sha256.data(), f.data() + 0xa0 + 0x20 * i, 32);
    }
    h.module_name = {rd<uint32_t>(f, 0x1c), rd<uint32_t>(f, 0x2c)};
    h.bss_size = rd<uint32_t>(f, 0x3c);
    std::memcpy(h.module_id.data(), f.data() + 0x40, 32);
    h.api_info = {rd<uint32_t>(f, 0x88), rd<uint32_t>(f, 0x8c)};
    h.dynstr = {rd<uint32_t>(f, 0x90), rd<uint32_t>(f, 0x94)};
    h.dynsym = {rd<uint32_t>(f, 0x98), rd<uint32_t>(f, 0x9c)};
    return h;
}

std::string Module::build_id() const {
    size_t n = nso.module_id.size();
    while (n > 16 && nso.module_id[n - 1] == 0) n--;
    std::string s;
    for (size_t i = 0; i < n; i++) {
        char b[3];
        std::snprintf(b, sizeof b, "%02x", nso.module_id[i]);
        s += b;
    }
    return s;
}

std::string Module::where(uint64_t off) const {
    for (int s = 0; s < 3; s++)
        if (off >= seg(s).mem_off && off < seg_end(s)) return seg_names[s];
    if (off >= seg_end(seg_data) && off < image_size()) return "bss";
    return "outside";
}

std::string Module::string_at(uint64_t off) const {
    need(off < image.size(), name + ": string at " + hex(off) + " outside the image");
    auto e = std::find(image.begin() + ptrdiff_t(off), image.end(), uint8_t(0));
    need(e != image.end(), name + ": unterminated string at " + hex(off));
    return std::string(image.begin() + ptrdiff_t(off), e);
}

uint32_t Module::lookup_sysv(std::string_view nm) const {
    if (!hash || hash->bucket.empty()) return 0;
    uint32_t i = hash->bucket[elf_hash(nm) % hash->bucket.size()];
    for (size_t steps = 0; i && steps <= hash->chain.size(); steps++) {
        if (i >= symbols.size() || i >= hash->chain.size()) return 0;
        if (symbols[i].defined() && symbols[i].name == nm) return i;
        i = hash->chain[i];
    }
    return 0;
}

uint32_t Module::lookup_gnu(std::string_view nm) const {
    if (!gnu_hash || gnu_hash->bucket.empty() || gnu_hash->bloom.empty()) return 0;
    const GnuHash& g = *gnu_hash;
    const uint32_t h = nxload::gnu_hash(nm);
    const uint64_t word = g.bloom[(h / 64) % g.bloom.size()];
    const uint64_t mask = (uint64_t(1) << (h % 64)) | (uint64_t(1) << ((h >> g.bloom_shift) % 64));
    if ((word & mask) != mask) return 0;
    for (uint32_t i = g.bucket[h % g.bucket.size()]; i >= g.symoffset && i < symbols.size(); i++) {
        const size_t k = i - g.symoffset;
        if (k >= g.chain.size()) return 0;
        if ((g.chain[k] | 1) == (h | 1) && symbols[i].defined() && symbols[i].name == nm) return i;
        if (g.chain[k] & 1) return 0;
    }
    return 0;
}

uint32_t Module::lookup(std::string_view nm) const {
    if (gnu_hash) return lookup_gnu(nm);
    if (hash) return lookup_sysv(nm);
    auto it = by_name.find(nm);
    return it == by_name.end() ? 0 : it->second;
}

uint32_t Module::symbol_count_sysv() const { return hash ? uint32_t(hash->chain.size()) : 0; }
uint32_t Module::symbol_count_gnu() const { return gnu_hash ? gnu_hash->symoffset + uint32_t(gnu_hash->chain.size()) : 0; }

size_t Module::hash_mismatches() const {
    size_t bad = 0;
    for (auto& [nm, i] : by_name) {
        if (hash && lookup_sysv(nm) != i) bad++;
        if (gnu_hash && lookup_gnu(nm) != i) bad++;
    }
    return bad;
}

std::vector<uint64_t> Module::relative_targets() const {
    std::vector<uint64_t> out;
    out.reserve(relr.size());
    for (uint64_t a : relr) out.push_back(rd<uint64_t>(image, a));
    for (auto& r : rela)
        if (r.type == R_AARCH64_RELATIVE) out.push_back(uint64_t(r.addend));
    return out;
}

std::vector<SvcSite> Module::svc_sites() const {
    std::vector<SvcSite> out;
    for (uint64_t o = 0; o + 4 <= seg(seg_text).size; o += 4) {
        const uint32_t w = load_le<uint32_t>(image, size_t(o));
        if ((w & 0xffe0001f) == 0xd4000001) out.push_back({o, (w >> 5) & 0xffff});
    }
    return out;
}

namespace {

void read_hash_tables(Module& m) {
    std::span<const uint8_t> img = m.image;
    if (auto o = m.dyn.get(DT_HASH)) {
        const uint32_t nb = rd<uint32_t>(img, *o), nc = rd<uint32_t>(img, *o + 4);
        need(*o + 8 + 4 * (uint64_t(nb) + nc) <= img.size(), m.name + ": DT_HASH runs past the image");
        SysvHash t;
        t.bucket.resize(nb);
        t.chain.resize(nc);
        for (uint32_t i = 0; i < nb; i++) t.bucket[i] = rd<uint32_t>(img, *o + 8 + 4 * i);
        for (uint32_t i = 0; i < nc; i++) t.chain[i] = rd<uint32_t>(img, *o + 8 + 4 * (uint64_t(nb) + i));
        m.hash = std::move(t);
    }
    if (auto o = m.dyn.get(DT_GNU_HASH)) {
        const uint32_t nb = rd<uint32_t>(img, *o), nbloom = rd<uint32_t>(img, *o + 8);
        GnuHash t;
        t.symoffset = rd<uint32_t>(img, *o + 4);
        t.bloom_shift = rd<uint32_t>(img, *o + 12);
        uint64_t p = *o + 16;
        need(p + 8 * uint64_t(nbloom) + 4 * uint64_t(nb) <= img.size(), m.name + ": DT_GNU_HASH runs past the image");
        for (uint32_t i = 0; i < nbloom; i++, p += 8) t.bloom.push_back(rd<uint64_t>(img, p));
        for (uint32_t i = 0; i < nb; i++, p += 4) t.bucket.push_back(rd<uint32_t>(img, p));
        // The chain has no stored length: it runs to the end of the highest bucket's chain
        uint32_t last = 0;
        for (uint32_t b : t.bucket) last = std::max(last, b);
        if (last) {
            need(last >= t.symoffset, m.name + ": DT_GNU_HASH bucket below symoffset");
            for (uint32_t i = t.symoffset;; i++) {
                const uint32_t v = rd<uint32_t>(img, p + 4 * uint64_t(i - t.symoffset));
                t.chain.push_back(v);
                if (i >= last && (v & 1)) break;
            }
        }
        m.gnu_hash = std::move(t);
    }
}

std::vector<Reloc> read_rela(const Module& m, uint64_t off, uint64_t size, const char* what) {
    need(size % 24 == 0, m.name + ": " + what + " size is not a multiple of 24");
    need(off + size <= m.image.size(), m.name + ": " + what + " runs past the image");
    std::vector<Reloc> out(size / 24);
    for (size_t i = 0; i < out.size(); i++) {
        const uint64_t p = off + 24 * i, info = rd<uint64_t>(m.image, p + 8);
        out[i] = {rd<uint64_t>(m.image, p), uint32_t(info), uint32_t(info >> 32), rd<int64_t>(m.image, p + 16)};
        need(out[i].offset + 8 <= m.image.size(), m.name + ": " + what + " entry " + std::to_string(i) + " targets " + hex(out[i].offset) + ", outside the image");
        need(out[i].sym < m.symbols.size(), m.name + ": " + what + " entry " + std::to_string(i) + " names symbol " + std::to_string(out[i].sym) + ", past the symbol table");
    }
    return out;
}

}  // namespace

Module load_module(std::string name, std::span<const uint8_t> f, const LoadOptions& opt) {
    Module m;
    m.name = std::move(name);
    m.file_size = f.size();
    m.nso = parse_nso_header(f);
    const NsoHeader& h = m.nso;
    const std::string& n = m.name;

    need(h.seg[seg_text].mem_off == 0, n + ": text does not start at 0");
    for (int i = 0; i < 3; i++) {
        const Segment& s = h.seg[i];
        need(s.mem_off % page_size == 0, n + ": " + seg_names[i] + " is not page-aligned");
        need(i == 0 || s.mem_off >= m.seg_end(i - 1), n + ": " + seg_names[i] + " overlaps the segment before it");
        const uint64_t stored = s.compressed ? s.file_size : s.size;
        need(uint64_t(s.file_off) + stored <= f.size(), n + ": " + seg_names[i] + " runs past the end of the file");
    }
    m.image.assign(align_up(m.seg_end(seg_data) + h.bss_size, page_size), 0);
    bool any_hashed = false;
    for (int i = 0; i < 3; i++) {
        const Segment& s = h.seg[i];
        auto dst = std::span(m.image).subspan(s.mem_off, s.size);
        auto src = f.subspan(s.file_off, s.compressed ? s.file_size : s.size);
        if (s.compressed) {
            try {
                need(!(h.flags >> 7 & 1), n + ": ZBIC-compressed NSO must be normalized by the separate zbic-unpack tool first");
                lz4_decompress(src, dst);
            } catch (const nxw::Error& e) {
                fail(n + ": " + seg_names[i] + ": " + e.what());
            }
        } else {
            std::memcpy(dst.data(), src.data(), s.size);
        }
        if (s.hashed && opt.verify_hashes) {
            need(sha256(dst) == s.sha256, n + ": " + seg_names[i] + " SHA-256 does not match the header");
            any_hashed = true;
        }
    }
    m.hashes_checked = any_hashed;
    std::span<const uint8_t> img = m.image;

    // MOD0: its offset is the second word of text; its fields are relative to MOD0 itself
    const uint64_t m0 = rd<uint32_t>(img, 4);
    need(m0 + 0x1c <= img.size() && std::memcmp(img.data() + m0, "MOD0", 4) == 0, n + ": no MOD0 at " + hex(m0));
    auto rel = [&](int k) { return uint64_t(int64_t(m0) + rd<int32_t>(img, m0 + 4 + 4 * k)); };
    m.mod0 = {m0, rel(0), rel(1), rel(2), rel(3), rel(4), rel(5)};
    need(m.mod0.bss_start <= m.mod0.bss_end && m.mod0.eh_frame_hdr_start <= m.mod0.eh_frame_hdr_end && m.mod0.eh_frame_hdr_end <= img.size(),
         n + ": MOD0 ranges are inconsistent");
    if (m.mod0.bss_end > img.size()) {
        m.notes.push_back("MOD0 bss ends past the header's bss; image extended");
        m.image.resize(align_up(m.mod0.bss_end, page_size));
        img = m.image;
    }

    for (uint64_t p = m.mod0.dynamic;; p += 16) {
        need(p + 16 <= img.size(), n + ": the dynamic section runs past the image");
        const int64_t tag = rd<int64_t>(img, p);
        if (tag == DT_NULL) break;
        m.dyn.tags.push_back({tag, rd<uint64_t>(img, p + 8)});
    }
    const Dynamic& d = m.dyn;
    need(!d.get(DT_REL) && !d.get(DT_TEXTREL), n + ": DT_REL / DT_TEXTREL are not supported");
    need(d.value(DT_SYMENT) == 24 || !d.get(DT_SYMENT), n + ": DT_SYMENT is not 24");
    need(d.value(DT_RELAENT) == 24 || !d.get(DT_RELAENT), n + ": DT_RELAENT is not 24");
    need(d.value(DT_RELRENT) == 8 || !d.get(DT_RELRENT), n + ": DT_RELRENT is not 8");
    need(!d.get(DT_JMPREL) || d.value(DT_PLTREL) == uint64_t(DT_RELA), n + ": PLT relocations are not RELA");
    need(d.get(DT_STRTAB) && d.get(DT_SYMTAB), n + ": no DT_STRTAB / DT_SYMTAB");
    const uint64_t strtab = d.value(DT_STRTAB), strsz = d.value(DT_STRSZ), symtab = d.value(DT_SYMTAB);
    need(strtab + strsz <= img.size() && (strsz == 0 || img[strtab + strsz - 1] == 0), n + ": DT_STRTAB / DT_STRSZ out of range");
    auto str = [&](uint64_t o) {
        need(o < strsz, n + ": string offset " + hex(o) + " past DT_STRSZ");
        return m.string_at(strtab + o);
    };
    for (auto& [t, v] : d.tags) {
        if (t == DT_NEEDED) m.dyn.needed.push_back(str(v));
        if (t == DT_SONAME) m.dyn.soname = str(v);
    }
    const uint64_t ro = h.seg[seg_ro].mem_off;
    if (symtab != ro + h.dynsym.off) m.notes.push_back("DT_SYMTAB differs from the NSO header's dynsym");
    if (strtab != ro + h.dynstr.off || strsz != h.dynstr.size) m.notes.push_back("DT_STRTAB / DT_STRSZ differ from the NSO header's dynstr");

    read_hash_tables(m);
    const uint32_t n_hdr = h.dynsym.size / 24, n_sysv = m.symbol_count_sysv(), n_gnu = m.symbol_count_gnu();
    const uint32_t nsym = n_hdr ? n_hdr : n_sysv ? n_sysv : n_gnu;
    need(nsym > 0, n + ": no way to tell the symbol count");
    if (n_hdr && n_sysv && n_hdr != n_sysv)
        m.notes.push_back("symbol counts disagree: header " + std::to_string(n_hdr) + ", DT_HASH " + std::to_string(n_sysv));
    if (n_gnu > nsym) m.notes.push_back("DT_GNU_HASH chains reach past the symbol table");
    need(symtab + 24 * uint64_t(nsym) <= img.size(), n + ": the symbol table runs past the image");
    m.symbols.resize(nsym);
    for (uint32_t i = 0; i < nsym; i++) {
        const uint64_t o = symtab + 24 * uint64_t(i);
        Symbol& s = m.symbols[i];
        s.name = str(rd<uint32_t>(img, o));
        s.bind = img[o + 4] >> 4;
        s.type = img[o + 4] & 0xf;
        s.visibility = img[o + 5] & 3;
        s.shndx = rd<uint16_t>(img, o + 6);
        s.value = rd<uint64_t>(img, o + 8);
        s.size = rd<uint64_t>(img, o + 16);
        if (s.exported()) m.by_name.emplace(s.name, i);
    }

    if (auto o = d.get(DT_RELA)) m.rela = read_rela(m, *o, d.value(DT_RELASZ), "DT_RELA");
    if (auto o = d.get(DT_JMPREL)) m.jmprel = read_rela(m, *o, d.value(DT_PLTRELSZ), "DT_JMPREL");
    if (auto o = d.get(DT_RELR)) {
        need(*o + d.value(DT_RELRSZ) <= img.size(), n + ": DT_RELR runs past the image");
        m.relr = decode_relr(img.subspan(*o, d.value(DT_RELRSZ)));
        for (uint64_t a : m.relr) need(a + 8 <= img.size(), n + ": DT_RELR relocates " + hex(a) + ", outside the image");
    }
    if (auto c = d.get(DT_RELACOUNT)) {
        size_t lead = 0;
        while (lead < m.rela.size() && m.rela[lead].type == R_AARCH64_RELATIVE) lead++;
        if (lead != *c) m.notes.push_back("DT_RELACOUNT " + std::to_string(*c) + " but " + std::to_string(lead) + " leading RELATIVE relocations");
    }
    return m;
}

Module load_module_file(const std::filesystem::path& p, const LoadOptions& opt) {
    return load_module(p.filename().string(), nxw::read_file(p), opt);
}

}  // namespace nxload
