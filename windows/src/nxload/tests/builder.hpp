// Synthetic inputs for the tests: a byte writer, an LZ4 block encoder, and an NSO builder with MOD0, a dynamic section,
// symbols with SysV and GNU hash tables, RELA/JMPREL/RELR relocations and a small .eh_frame.
#pragma once
#include "nxload/ehframe.hpp"
#include "nxload/module.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace build {

using namespace nxload;

struct Bytes {
    std::vector<uint8_t> b;
    size_t size() const { return b.size(); }
    void u8(uint64_t v) { b.push_back(uint8_t(v)); }
    void u16(uint64_t v) { put(v, 2); }
    void u32(uint64_t v) { put(v, 4); }
    void u64(uint64_t v) { put(v, 8); }
    void put(uint64_t v, int n) {
        for (int i = 0; i < n; i++) b.push_back(uint8_t(v >> (8 * i)));
    }
    void at32(size_t o, uint64_t v) {
        for (int i = 0; i < 4; i++) b[o + i] = uint8_t(v >> (8 * i));
    }
    void at64(size_t o, uint64_t v) {
        for (int i = 0; i < 8; i++) b[o + i] = uint8_t(v >> (8 * i));
    }
    void uleb(uint64_t v) {
        do {
            uint8_t c = v & 0x7f;
            v >>= 7;
            b.push_back(c | (v ? 0x80 : 0));
        } while (v);
    }
    void sleb(int64_t v) {
        for (bool more = true; more;) {
            uint8_t c = v & 0x7f;
            v >>= 7;
            more = !((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40)));
            b.push_back(c | (more ? 0x80 : 0));
        }
    }
    void str(std::string_view s) {
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
    }
    void bytes(std::span<const uint8_t> s) { b.insert(b.end(), s.begin(), s.end()); }
    void pad(size_t align, uint8_t fill = 0) {
        while (b.size() % align) b.push_back(fill);
    }
    void fill_to(size_t n) {
        if (b.size() < n) b.resize(n, 0);
    }
};

// Greedy LZ4 block encoder for zero runs (and plain literals otherwise); the block ends in literals as the format requires.
inline std::vector<uint8_t> lz4_encode(std::span<const uint8_t> in) {
    Bytes o;
    auto len = [&](size_t n) {
        for (n -= 15; n >= 255; n -= 255) o.u8(255);
        o.u8(n);
    };
    size_t lit = 0, i = 0;
    auto seq = [&](size_t lit_end, size_t match_len, size_t off) {
        const size_t nl = lit_end - lit;
        o.u8((std::min<size_t>(nl, 15) << 4) | (match_len ? std::min<size_t>(match_len - 4, 15) : 0));
        if (nl >= 15) len(nl);
        o.bytes(in.subspan(lit, nl));
        if (!match_len) return;
        o.u16(off);
        if (match_len - 4 >= 15) len(match_len - 4);
    };
    while (i + 12 < in.size()) {
        size_t run = 0;
        while (i + 1 + run + 12 < in.size() && in[i] == 0 && in[i + 1 + run] == 0) run++;
        if (in[i] == 0 && run >= 8) {
            seq(i + 1, run, 1);   // one literal zero, then a match repeating it
            i += 1 + run;
            lit = i;
        } else {
            i++;
        }
    }
    seq(in.size(), 0, 0);
    return o.b;
}

struct Sym {
    std::string name;
    uint64_t value = 0;   // text-relative when defined
    bool defined = true;
    uint8_t bind = STB_GLOBAL, type = STT_FUNC, vis = STV_DEFAULT;
};

struct Rel {
    uint64_t word;   // index into the data words
    uint32_t type, sym;
    int64_t addend = 0;
    bool plt = false;
};

struct Spec {
    std::vector<uint32_t> text = {0x14000002, 0, 0xd503201f, 0xd65f03c0};   // b +8; (MOD0 offset); nop; ret
    std::vector<Sym> syms;
    std::vector<Rel> rels;
    std::vector<uint64_t> words = std::vector<uint64_t>(64, 0);   // data words, after the dynamic section
    std::vector<size_t> relr;           // word indices relocated by DT_RELR
    size_t init_first = 0, init_count = 0;   // INIT_ARRAY over words
    std::vector<std::pair<uint64_t, uint64_t>> funcs = {{0, 16}};   // FDEs: text-relative start, size
    uint32_t bss = 0x1800;
    bool sysv = true, gnu = true, compress = true, hashed = true;
    std::string soname;
};

struct Built {
    std::vector<uint8_t> nso;
    uint64_t ro = 0, data = 0, words = 0;   // module offsets
};

inline Built build_nso(const Spec& s) {
    Built r;
    Bytes text;
    for (uint32_t w : s.text) text.u32(w);
    r.ro = align_up(text.size(), page_size);

    // ro: MOD0, dynsym, dynstr, hashes, relocations, .eh_frame_hdr, .eh_frame
    Bytes ro;
    ro.fill_to(0x20);
    const size_t symtab = ro.size();
    Bytes strtab;
    strtab.u8(0);
    std::vector<uint32_t> name_off;
    for (auto& y : s.syms) {
        name_off.push_back(uint32_t(strtab.size()));
        strtab.str(y.name);
    }
    const uint32_t soname_off = uint32_t(strtab.size());
    strtab.str(s.soname);
    ro.fill_to(symtab + 24);   // the null symbol
    for (size_t i = 0; i < s.syms.size(); i++) {
        const Sym& y = s.syms[i];
        ro.u32(name_off[i]);
        ro.u8(uint8_t(y.bind << 4 | y.type));
        ro.u8(y.vis);
        ro.u16(y.defined ? 1 : 0);
        ro.u64(y.defined ? y.value : 0);
        ro.u64(y.defined ? 4 : 0);
    }
    const size_t nsym = s.syms.size() + 1;
    const size_t strpos = ro.size();
    ro.bytes(strtab.b);
    ro.pad(8);
    size_t hash = 0, gnuh = 0;
    if (s.sysv) {
        hash = ro.size();
        const uint32_t nb = 3;
        std::vector<uint32_t> bucket(nb, 0), chain(nsym, 0);
        for (size_t i = nsym - 1; i >= 1; i--) {
            const uint32_t b = elf_hash(s.syms[i - 1].name) % nb;
            chain[i] = bucket[b];
            bucket[b] = uint32_t(i);
        }
        ro.u32(nb);
        ro.u32(nsym);
        for (uint32_t v : bucket) ro.u32(v);
        for (uint32_t v : chain) ro.u32(v);
        ro.pad(8);
    }
    if (s.gnu) {   // one bucket holding every symbol from index 1
        gnuh = ro.size();
        uint64_t bloom = 0;
        for (auto& y : s.syms) {
            const uint32_t h = gnu_hash(y.name);
            bloom |= uint64_t(1) << (h % 64) | uint64_t(1) << ((h >> 6) % 64);
        }
        ro.u32(1);
        ro.u32(1);
        ro.u32(1);
        ro.u32(6);
        ro.u64(bloom);
        ro.u32(nsym > 1 ? 1 : 0);
        for (size_t i = 0; i < s.syms.size(); i++) ro.u32((gnu_hash(s.syms[i].name) & ~1u) | (i + 1 == s.syms.size() ? 1 : 0));
        ro.pad(8);
    }
    r.data = align_up(r.ro + align_up(ro.size() + 0x400, 8), page_size);   // leaves room for relocations and .eh_frame below
    const uint64_t dyn_size = 0x200;
    r.words = r.data + dyn_size;
    auto rela_table = [&](bool plt) {
        const size_t at = ro.size();
        for (auto& x : s.rels)
            if (x.plt == plt) {
                ro.u64(r.words + 8 * x.word);
                ro.u64(uint64_t(x.sym) << 32 | x.type);
                ro.u64(uint64_t(x.addend));
            }
        return std::pair{at, ro.size() - at};
    };
    const auto [rela, relasz] = rela_table(false);
    const auto [jmprel, pltsz] = rela_table(true);
    const size_t relr = ro.size();
    {   // addresses, packing following runs into bitmaps
        std::vector<uint64_t> a;
        for (size_t w : s.relr) a.push_back(r.words + 8 * w);
        std::sort(a.begin(), a.end());
        for (size_t i = 0; i < a.size();) {
            ro.u64(a[i]);
            uint64_t base = a[i] + 8;
            i++;
            while (i < a.size() && a[i] >= base && a[i] < base + 63 * 8) {
                uint64_t bits = 1;
                while (i < a.size() && a[i] < base + 63 * 8) bits |= uint64_t(1) << ((a[i] - base) / 8 + 1), i++;
                ro.u64(bits);
                base += 63 * 8;
            }
        }
    }
    const size_t relrsz = ro.size() - relr;
    // .eh_frame_hdr then .eh_frame: one "zR" CIE, an FDE per function
    ro.pad(4);
    const size_t ehh = ro.size();
    ro.u8(1), ro.u8(0x1b), ro.u8(0x03), ro.u8(0x3b);
    const size_t ehh_ptr = ro.size();
    ro.u32(0);
    ro.u32(s.funcs.size());
    const size_t table = ro.size();
    for (size_t i = 0; i < s.funcs.size(); i++) ro.u64(0);
    const size_t ehh_end = ro.size();
    ro.pad(8);
    const size_t eh = ro.size();
    ro.at32(ehh_ptr, uint32_t(int64_t(r.ro + eh) - int64_t(r.ro + ehh_ptr)));
    const size_t cie = ro.size();
    ro.u32(0), ro.u32(0), ro.u8(1), ro.str("zR"), ro.uleb(4), ro.sleb(-8), ro.u8(30), ro.uleb(1), ro.u8(0x1b);
    ro.u8(0x0c), ro.u8(31), ro.u8(0);   // def_cfa sp, 0
    ro.pad(4);
    ro.at32(cie, ro.size() - cie - 4);
    std::vector<std::pair<uint64_t, size_t>> rows;
    for (auto [start, size] : s.funcs) {
        const size_t f = ro.size();
        ro.u32(0);
        ro.u32(ro.size() - cie);
        ro.u32(uint32_t(int64_t(start) - int64_t(r.ro + ro.size())));
        ro.u32(size);
        ro.uleb(0);
        ro.pad(4);
        ro.at32(f, ro.size() - f - 4);
        rows.push_back({start, f});
    }
    ro.u32(0);
    std::sort(rows.begin(), rows.end());
    for (size_t i = 0; i < rows.size(); i++) {
        ro.at32(table + 8 * i, uint32_t(int64_t(rows[i].first) - int64_t(r.ro + ehh)));
        ro.at32(table + 8 * i + 4, uint32_t(int64_t(r.ro + rows[i].second) - int64_t(r.ro + ehh)));
    }
    if (r.ro + ro.size() > r.data) throw std::runtime_error("builder: ro overflows into data");

    // data: the dynamic section, then the words; bss follows
    Bytes data;
    auto tag = [&](int64_t t, uint64_t v) { data.u64(uint64_t(t)), data.u64(v); };
    if (!s.soname.empty()) tag(DT_SONAME, soname_off);
    tag(DT_SYMTAB, r.ro + symtab);
    tag(DT_SYMENT, 24);
    tag(DT_STRTAB, r.ro + strpos);
    tag(DT_STRSZ, strtab.size());
    if (s.sysv) tag(DT_HASH, r.ro + hash);
    if (s.gnu) tag(DT_GNU_HASH, r.ro + gnuh);
    if (relasz) tag(DT_RELA, r.ro + rela), tag(DT_RELASZ, relasz), tag(DT_RELAENT, 24);
    if (pltsz) tag(DT_JMPREL, r.ro + jmprel), tag(DT_PLTRELSZ, pltsz), tag(DT_PLTREL, DT_RELA);
    if (relrsz) tag(DT_RELR, r.ro + relr), tag(DT_RELRSZ, relrsz), tag(DT_RELRENT, 8);
    if (s.init_count) tag(DT_INIT_ARRAY, r.words + 8 * s.init_first), tag(DT_INIT_ARRAYSZ, 8 * s.init_count);
    tag(DT_NULL, 0);
    data.fill_to(dyn_size);
    for (uint64_t w : s.words) data.u64(w);
    const uint64_t data_end = r.data + data.size(), bss_start = align_up(data_end, 8);

    // MOD0 at the start of ro; the second text word points at it
    auto rel32 = [&](uint64_t to) { return uint32_t(int64_t(to) - int64_t(r.ro)); };
    ro.b[0] = 'M', ro.b[1] = 'O', ro.b[2] = 'D', ro.b[3] = '0';
    ro.at32(4, rel32(r.data));
    ro.at32(8, rel32(bss_start));
    ro.at32(12, rel32(data_end + s.bss));
    ro.at32(16, rel32(r.ro + ehh));
    ro.at32(20, rel32(r.ro + ehh_end));
    ro.at32(24, rel32(bss_start));
    text.at32(4, r.ro);

    // NSO0
    Bytes f;
    f.fill_to(0x100);
    const Bytes* segs[3] = {&text, &ro, &data};
    const uint64_t mem[3] = {0, r.ro, r.data};
    uint32_t flags = 0;
    for (int i = 0; i < 3; i++) {
        const auto& raw = segs[i]->b;
        const auto stored = s.compress ? lz4_encode(raw) : raw;
        const size_t off = f.size();
        f.bytes(stored);
        f.at32(0x10 + 0x10 * i, off);
        f.at32(0x14 + 0x10 * i, mem[i]);
        f.at32(0x18 + 0x10 * i, raw.size());
        f.at32(0x60 + 4 * i, stored.size());
        if (s.compress) flags |= 1u << i;
        if (s.hashed) {
            flags |= 8u << i;
            const auto h = sha256(raw);
            std::copy(h.begin(), h.end(), f.b.begin() + 0xa0 + 0x20 * i);
        }
    }
    std::memcpy(f.b.data(), "NSO0", 4);
    f.at32(0x0c, flags);
    f.at32(0x3c, uint32_t(align_up(data_end + s.bss, page_size) - data_end));
    for (int i = 0; i < 16; i++) f.b[0x40 + i] = uint8_t(0xa0 + i);
    f.at32(0x90, strpos), f.at32(0x94, strtab.size());
    f.at32(0x98, symtab), f.at32(0x9c, 24 * nsym);
    r.nso = std::move(f.b);
    return r;
}

}  // namespace build
