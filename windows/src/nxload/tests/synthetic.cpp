// Format edge cases on synthetic data: LZ4, LEB128, RELR, symbol hashes, pointer encodings, .eh_frame, NSO/MOD0, linking, npdm.
#include "builder.hpp"
#include "check.hpp"
#include "nxload/npdm.hpp"
#include "nxload/program.hpp"

using namespace nxload;
using build::Bytes;

namespace {

std::vector<uint8_t> bytes(std::initializer_list<int> l) { return std::vector<uint8_t>(l.begin(), l.end()); }

std::vector<uint8_t> unlz4(const std::vector<uint8_t>& src, size_t n) {
    std::vector<uint8_t> out(n);
    lz4_decompress(src, out);
    return out;
}

uint64_t word(const Program& p, int m, uint64_t words, size_t i) { return p.read64(m, words + 8 * i); }

}  // namespace

TEST(lz4_literals) {
    CHECK(unlz4(bytes({0x30, 'a', 'b', 'c'}), 3) == bytes({'a', 'b', 'c'}));
    CHECK(unlz4(bytes({0x00}), 0).empty());
}

TEST(lz4_overlapping_matches) {
    std::vector<uint8_t> run(11, 'a');
    run.push_back('b');
    CHECK(unlz4(bytes({0x16, 'a', 1, 0, 0x10, 'b'}), 12) == run);   // offset 1: a run of the last byte
    CHECK(unlz4(bytes({0x22, 'a', 'b', 2, 0, 0x00}), 8) == bytes({'a', 'b', 'a', 'b', 'a', 'b', 'a', 'b'}));
    CHECK(unlz4(bytes({0x50, 'a', 'b', 'c', 'd', 'e', 4, 0, 0x00}), 9) == bytes({'a', 'b', 'c', 'd', 'e', 'b', 'c', 'd', 'e'}));   // offset >= length
}

TEST(lz4_long_lengths) {
    std::vector<uint8_t> src = {0xf0, 255, 30};   // 15 + 255 + 30 = 300 literals, the last sequence
    for (int i = 0; i < 300; i++) src.push_back(uint8_t(i));
    auto out = unlz4(src, 300);
    CHECK(out.size() == 300 && out[0] == 0 && out[299] == uint8_t(299));
    // 'x', then a match of 4 + 15 + 255 + 10 = 284 at offset 1, then 'y'
    auto m = unlz4(bytes({0x1f, 'x', 1, 0, 255, 10, 0x10, 'y'}), 286);
    CHECK(m.size() == 286 && m[0] == 'x' && m[284] == 'x' && m[285] == 'y');
}

TEST(lz4_malformed) {
    CHECK_THROWS(unlz4(bytes({0x10, 'a', 0, 0, 0x00}), 8));      // offset 0
    CHECK_THROWS(unlz4(bytes({0x10, 'a', 2, 0, 0x00}), 8));      // offset before the start of the output
    CHECK_THROWS(unlz4(bytes({0x40, 'a', 'b'}), 4));             // literals past the input
    CHECK_THROWS(unlz4(bytes({0x30, 'a', 'b', 'c'}), 2));        // literals past the output
    CHECK_THROWS(unlz4(bytes({0x30, 'a', 'b', 'c'}), 4));        // output not filled
    CHECK_THROWS(unlz4(bytes({0x16, 'a', 1, 0, 0x10, 'b'}), 8)); // match past the output
    CHECK_THROWS(unlz4(bytes({0xf0, 255}), 300));                // truncated length
    CHECK_THROWS(unlz4(bytes({0x10, 'a', 1}), 8));               // truncated offset
    CHECK_THROWS(unlz4({}, 0));                                  // no token at all
}

TEST(lz4_encoder_roundtrip) {
    std::vector<uint8_t> in;
    for (int i = 0; i < 5000; i++) in.push_back(i % 700 < 300 ? 0 : uint8_t(i * 7));
    const auto enc = build::lz4_encode(in);
    CHECK(enc.size() < in.size());
    CHECK(unlz4(enc, in.size()) == in);
}

TEST(leb128) {
    auto u = [](std::vector<uint8_t> b) { size_t p = 0; uint64_t v = read_uleb(b, p); return p == b.size() ? v : ~0ull; };
    auto s = [](std::vector<uint8_t> b) { size_t p = 0; int64_t v = read_sleb(b, p); return p == b.size() ? v : INT64_MIN; };
    CHECK_EQ(u(bytes({0xe5, 0x8e, 0x26})), 624485u);
    CHECK_EQ(u(bytes({0x80, 0x80, 0x00})), 0u);   // padded
    CHECK_EQ(u(bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01})), ~0ull);
    CHECK_EQ(s(bytes({0xc0, 0xbb, 0x78})), -123456);
    CHECK_EQ(s(bytes({0x3f})), 63);
    CHECK_EQ(s(bytes({0xc0, 0x00})), 64);
    CHECK_EQ(s(bytes({0x40})), -64);
    CHECK_EQ(s(bytes({0xbf, 0x7f})), -65);
    CHECK_THROWS(u(bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02})));   // 65 bits
    CHECK_THROWS(u(bytes({0x80})));
    CHECK_THROWS(s(bytes({0xff})));
}

TEST(relr_decoding) {
    Bytes t;
    t.u64(0x1000);
    t.u64(0b1011 | 1ull << 63);   // bits 1, 3, 63 → 0x1008, 0x1018, 0x11f8
    t.u64(0b11);                  // the next 63 words: bit 1 → 0x1200
    t.u64(0x3000);
    CHECK(decode_relr(t.b) == (std::vector<uint64_t>{0x1000, 0x1008, 0x1018, 0x11f8, 0x1200, 0x3000}));
    CHECK(decode_relr({}).empty());
    Bytes bad;
    bad.u64(0b101);
    CHECK_THROWS(decode_relr(bad.b));   // a bitmap first
    CHECK_THROWS(decode_relr(bytes({0, 0, 0, 0})));
}

TEST(symbol_hashes) {
    CHECK_EQ(elf_hash(""), 0u);
    CHECK_EQ(gnu_hash(""), 5381u);
    CHECK_EQ(elf_hash("printf"), 0x077905a6u);
    CHECK_EQ(gnu_hash("printf"), 0x156b2bb8u);
    CHECK_EQ(elf_hash("flapenguin.me"), 0x03987915u);   // exercises the high-nibble fold
    CHECK_EQ(gnu_hash("flapenguin.me"), 0x8ae9f18eu);
}

TEST(pointer_encodings) {
    Bytes b;
    b.fill_to(0x10);
    b.u32(uint32_t(-8));   // 0x10: pcrel sdata4 → 8
    b.u16(0x1234);         // 0x14
    b.u16(uint16_t(-2));   // 0x16
    b.uleb(300);           // 0x18
    b.sleb(-300);          // 0x1a
    b.u64(0x7100000000);   // 0x1c
    b.u32(0x20);           // 0x24: datarel
    size_t p = 0x10;
    CHECK_EQ(read_encoded(b.b, p, DW_EH_PE_pcrel | DW_EH_PE_sdata4), 8u);
    CHECK_EQ(read_encoded(b.b, p, DW_EH_PE_udata2), 0x1234u);
    CHECK_EQ(read_encoded(b.b, p, DW_EH_PE_sdata2), uint64_t(-2));
    CHECK_EQ(read_encoded(b.b, p, DW_EH_PE_uleb128), 300u);
    CHECK_EQ(read_encoded(b.b, p, DW_EH_PE_sleb128), uint64_t(-300));
    CHECK_EQ(read_encoded(b.b, p, DW_EH_PE_absptr), 0x7100000000u);
    CHECK_EQ(read_encoded(b.b, p, DW_EH_PE_datarel | DW_EH_PE_udata4, 0x1000), 0x1020u);
    CHECK_EQ(p, 0x28u);
    p = 0x10;
    CHECK_EQ(read_encoded(b.b, p, DW_EH_PE_udata4), 0xfffffff8u);
    for (uint8_t e : {uint8_t(DW_EH_PE_omit), uint8_t(DW_EH_PE_textrel | 3), uint8_t(DW_EH_PE_funcrel | 3), uint8_t(DW_EH_PE_aligned), uint8_t(0x05)}) {
        p = 0x10;
        CHECK_THROWS(read_encoded(b.b, p, e));
    }
    p = 0x26;
    CHECK_THROWS(read_encoded(b.b, p, DW_EH_PE_udata4));   // past the end
}

namespace {

struct EhImage {
    Bytes b;
    size_t hdr = 0x200, hdr_end = 0, table = 0, eh = 0, terminator = 0;
    std::vector<size_t> fdes;
};

// .eh_frame_hdr at 0x200 and an .eh_frame with three CIEs ("zR" v1; "zPLRB" v3 with an indirect personality; "zRSQ" with absolute
// pointers and an unknown augmentation skipped by length) and four FDEs, one of them in the 64-bit length format
EhImage eh_image() {
    EhImage e;
    Bytes& b = e.b;
    b.fill_to(e.hdr);
    b.u8(1), b.u8(0x1b), b.u8(0x03), b.u8(0x3b);
    const size_t ptr = b.size();
    b.u32(0);
    b.u32(4);
    e.table = b.size();
    for (int i = 0; i < 4; i++) b.u64(0);
    e.hdr_end = b.size();
    b.pad(16);
    e.eh = b.size();
    b.at32(ptr, e.eh - ptr);
    auto close = [&](size_t at) {
        b.pad(4);
        b.at32(at, b.size() - at - 4);
    };
    const size_t a = b.size();
    b.u32(0), b.u32(0), b.u8(1), b.str("zR"), b.uleb(4), b.sleb(-8), b.u8(30), b.uleb(1), b.u8(0x1b), b.u8(0x0c), b.u8(31), b.u8(0);
    close(a);
    const size_t bb = b.size();
    b.u32(0), b.u32(0), b.u8(3), b.str("zPLRB"), b.uleb(4), b.sleb(-8), b.uleb(30), b.uleb(7), b.u8(0x9b);
    b.u32(uint32_t(0x380 - b.size()));
    b.u8(0x1b), b.u8(0x1b), b.u8(0x0c), b.u8(31), b.u8(0);
    close(bb);
    const size_t c = b.size();
    b.u32(0), b.u32(0), b.u8(1), b.str("zRSQ"), b.uleb(4), b.sleb(-8), b.u8(30), b.uleb(2), b.u8(0x03), b.u8(0xee);
    close(c);
    auto fde = [&](size_t cie, uint64_t start, uint32_t size, bool absolute, int lsda, bool wide) {   // lsda: -1 none, 0 raw zero, else target
        const size_t at = b.size();
        e.fdes.push_back(at);
        if (wide) b.u32(0xffffffff), b.u64(0);
        else b.u32(0);
        b.u32(b.size() - cie);
        b.u32(absolute ? uint32_t(start) : uint32_t(start - b.size()));
        b.u32(size);
        if (lsda < 0) b.uleb(0);
        else {
            b.uleb(4);
            b.u32(lsda ? uint32_t(uint64_t(lsda) - b.size()) : 0);
        }
        b.u8(0x0e), b.u8(16);   // def_cfa_offset 16
        b.pad(4);
        if (wide) b.at64(at + 4, b.size() - at - 12);
        else b.at32(at, b.size() - at - 4);
    };
    fde(a, 0x10, 0x20, false, -1, false);
    fde(bb, 0x40, 0x10, false, 0x300, false);
    fde(bb, 0x60, 0x08, false, 0, true);
    fde(c, 0x80, 0x04, true, -1, false);
    e.terminator = b.size();
    b.u32(0);
    const uint64_t starts[4] = {0x10, 0x40, 0x60, 0x80};
    for (int i = 0; i < 4; i++) {
        b.at32(e.table + 8 * i, starts[i] - e.hdr);
        b.at32(e.table + 8 * i + 4, e.fdes[i] - e.hdr);
    }
    b.fill_to(0x400);
    return e;
}

}  // namespace

TEST(eh_frame_cies_and_fdes) {
    EhImage e = eh_image();
    const EhFrame f = parse_eh_frame(e.b.b, e.hdr, e.hdr_end, e.b.size());
    CHECK_EQ(f.hdr.version, 1);
    CHECK_EQ(f.hdr.eh_frame, e.eh);
    CHECK_EQ(f.hdr.fde_count, 4u);
    CHECK_EQ(f.end, e.terminator + 4);
    CHECK(f.hdr_matches);
    CHECK_EQ(f.cies.size(), 3u);
    CHECK_EQ(f.fdes.size(), 4u);
    if (f.cies.size() != 3 || f.fdes.size() != 4) return;
    CHECK_EQ(f.cies[0].augmentation, "zR");
    CHECK_EQ(f.cies[0].fde_enc, 0x1b);
    CHECK_EQ(f.cies[0].data_align, -8);
    CHECK_EQ(f.cies[0].ra_register, 30u);
    CHECK_EQ(f.cies[1].version, 3);
    CHECK_EQ(f.cies[1].augmentation, "zPLRB");
    CHECK_EQ(f.cies[1].personality_enc, 0x9b);
    CHECK_EQ(f.cies[1].personality, 0x380u);   // the slot, not followed
    CHECK_EQ(f.cies[1].lsda_enc, 0x1b);
    CHECK(f.cies[1].b_key && !f.cies[0].b_key);
    CHECK(f.cies[2].signal_frame);
    CHECK_EQ(f.cies[2].fde_enc, 0x03);
    CHECK_EQ(f.cies[0].insns_size, 3u);   // def_cfa sp, 0
    const uint64_t start[4] = {0x10, 0x40, 0x60, 0x80}, size[4] = {0x20, 0x10, 0x08, 0x04}, lsda[4] = {0, 0x300, 0, 0};
    const uint32_t cie[4] = {0, 1, 1, 2};
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(f.fdes[i].offset, e.fdes[i]);
        CHECK_EQ(f.fdes[i].start, start[i]);
        CHECK_EQ(f.fdes[i].size, size[i]);
        CHECK_EQ(f.fdes[i].lsda, lsda[i]);
        CHECK_EQ(f.fdes[i].cie, cie[i]);
        CHECK(f.fdes[i].insns_size >= 2);
    }
    CHECK_EQ(f.lsda_count(), 1u);
}

TEST(eh_frame_inconsistencies) {
    {   // the search table out of order
        EhImage e = eh_image();
        e.b.at32(e.table, 0x40 - e.hdr);
        e.b.at32(e.table + 8, 0x10 - e.hdr);
        CHECK(!parse_eh_frame(e.b.b, e.hdr, e.hdr_end, e.b.size()).hdr_matches);
    }
    {   // a table entry pointing at the wrong FDE
        EhImage e = eh_image();
        e.b.at32(e.table + 4, e.fdes[1] - e.hdr);
        CHECK(!parse_eh_frame(e.b.b, e.hdr, e.hdr_end, e.b.size()).hdr_matches);
    }
    {   // no terminator: the walk stops at the limit
        EhImage e = eh_image();
        const EhFrame f = parse_eh_frame(e.b.b, e.hdr, e.hdr_end, e.terminator);
        CHECK_EQ(f.fdes.size(), 4u);
        CHECK_EQ(f.end, e.terminator);
    }
    {   // a record running past the limit
        EhImage e = eh_image();
        CHECK_THROWS(parse_eh_frame(e.b.b, e.hdr, e.hdr_end, e.fdes[3] + 8));
    }
    {   // an FDE whose CIE pointer lands on another FDE
        EhImage e = eh_image();
        e.b.at32(e.fdes[1] + 4, e.fdes[1] + 4 - e.fdes[0]);
        CHECK_THROWS(parse_eh_frame(e.b.b, e.hdr, e.hdr_end, e.b.size()));
    }
    {   // an unsupported CIE version
        EhImage e = eh_image();
        e.b.b[e.eh + 8] = 2;
        CHECK_THROWS(parse_eh_frame(e.b.b, e.hdr, e.hdr_end, e.b.size()));
    }
    {   // a wrong .eh_frame_hdr version
        EhImage e = eh_image();
        e.b.b[e.hdr] = 2;
        CHECK_THROWS(parse_eh_frame(e.b.b, e.hdr, e.hdr_end, e.b.size()));
    }
}

namespace {

build::Spec spec_with(std::vector<build::Sym> syms, std::vector<build::Rel> rels = {}) {
    build::Spec s;
    s.syms = std::move(syms);
    s.rels = std::move(rels);
    return s;
}

}  // namespace

TEST(nso_module_layout) {
    build::Spec s = spec_with({{"foo", 0x8}, {"bar", 0, false}, {"baz", 0xc, true, STB_WEAK, STT_OBJECT}});
    s.text = {0x14000002, 0, 0xd4000801 /* svc #0x40 */, 0xd65f03c0, 0xd4000521 /* svc #0x29 */, 0xd503201f};
    s.soname = "test.nss";
    s.funcs = {{0x8, 8}, {0x0, 8}};
    for (bool compress : {true, false}) {
        s.compress = compress;
        const build::Built r = build::build_nso(s);
        const Module m = load_module("t", r.nso);
        CHECK(m.hashes_checked);
        CHECK_EQ(m.seg(seg_text).size, 24u);
        CHECK_EQ(m.seg(seg_ro).mem_off, r.ro);
        CHECK_EQ(m.seg(seg_data).mem_off, r.data);
        CHECK_EQ(m.seg(seg_text).compressed, compress);
        CHECK_EQ(m.image_size() % page_size, 0u);
        CHECK_EQ(m.mod0.offset, r.ro);
        CHECK_EQ(m.mod0.dynamic, r.data);
        CHECK(m.mod0.bss_start >= m.seg_end(seg_data) && m.mod0.bss_end <= m.image_size());
        CHECK_EQ(m.dyn.soname, "test.nss");
        CHECK_EQ(m.symbols.size(), 4u);
        CHECK_EQ(m.symbols[1].name, "foo");
        CHECK(m.symbols[1].exported() && m.symbols[2].imported() && m.symbols[3].bind == STB_WEAK);
        CHECK_EQ(m.lookup_sysv("foo"), 1u);
        CHECK_EQ(m.lookup_gnu("baz"), 3u);
        CHECK_EQ(m.lookup("bar"), 0u);   // undefined
        CHECK_EQ(m.lookup("nope"), 0u);
        CHECK_EQ(m.symbol_count_sysv(), 4u);
        CHECK_EQ(m.symbol_count_gnu(), 4u);
        CHECK_EQ(m.hash_mismatches(), 0u);
        CHECK(m.notes.empty());
        CHECK_EQ(m.build_id(), "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
        CHECK_EQ(m.where(0x4), "text");
        CHECK_EQ(m.where(r.ro), "ro");
        CHECK_EQ(m.where(r.words), "data");
        CHECK_EQ(m.where(m.image_size() - 1), "bss");
        CHECK_EQ(m.where(m.image_size()), "outside");
        const auto svcs = m.svc_sites();
        CHECK(svcs.size() == 2 && svcs[0].offset == 8 && svcs[0].imm == 0x40 && svcs[1].imm == 0x29);
        const EhFrame e = parse_eh_frame(m);
        CHECK(e.hdr_matches && e.fdes.size() == 2);
    }
}

TEST(nso_module_hash_tables) {
    std::vector<build::Sym> syms;
    for (int i = 0; i < 40; i++) syms.push_back({"sym" + std::to_string(i), uint64_t(4 * i), i % 5 != 0});
    for (auto [sysv, gnu] : {std::pair{true, false}, std::pair{false, true}, std::pair{false, false}}) {
        build::Spec s = spec_with(syms);
        s.sysv = sysv, s.gnu = gnu;
        const Module m = load_module("t", build::build_nso(s).nso);
        CHECK_EQ(m.symbols.size(), 41u);
        int found = 0;
        for (int i = 0; i < 40; i++) found += m.lookup("sym" + std::to_string(i)) == uint32_t(i + 1);
        CHECK_EQ(found, 32);   // the 8 undefined ones are not found
        CHECK_EQ(m.hash_mismatches(), 0u);
    }
    Module m = load_module("t", build::build_nso(spec_with(syms)).nso);
    m.gnu_hash->bloom[0] = 0;   // a broken bloom filter hides every symbol from the GNU table
    CHECK_EQ(m.hash_mismatches(), 32u);
    CHECK_EQ(m.lookup_sysv("sym1"), 2u);
}

TEST(nso_malformed) {
    const build::Spec s = spec_with({{"foo", 0x8}});
    const auto good = build::build_nso(s).nso;
    CHECK_THROWS(load_module("t", std::vector<uint8_t>(good.begin(), good.begin() + 0x80)));
    auto bad = good;
    bad[0] = 'X';
    CHECK_THROWS(load_module("t", bad));
    bad = good;
    bad.resize(bad.size() - 8);   // data runs past the end of the file
    CHECK_THROWS(load_module("t", bad));
    bad = good;
    bad[0x14] = 0x10;   // text not at 0
    CHECK_THROWS(load_module("t", bad));

    build::Spec plain = s;
    plain.compress = false;
    const auto raw = build::build_nso(plain).nso;
    bad = raw;
    bad[0x100 + 8] ^= 1;   // text content no longer matches its SHA-256
    CHECK_THROWS(load_module("t", bad));
    LoadOptions no_check;
    no_check.verify_hashes = false;
    CHECK(!load_module("t", bad, no_check).hashes_checked);
    bad = raw;
    bad[0x100 + 4] += 4;   // the MOD0 offset no longer points at MOD0
    CHECK_THROWS(load_module("t", bad, no_check));

    bad = good;
    const uint32_t text_off = nxw::load_le<uint32_t>(good, 0x10);
    bad[text_off] = 0x00;   // corrupt LZ4: no literals, then a match before the start of the output
    CHECK_THROWS(load_module("t", bad));
}

namespace {

struct Linked {
    std::vector<build::Built> built;
    Program p;
};

// main imports f (exported by subsdk0 and sdk), g (sdk), w (weak, nobody), u (nobody), x (rtld only); sdk imports main's malloc
// and defines op_new, which main also exports; main and sdk both define a protected _init; subsdk0 imports sdk's protected pv
Linked link_four(const LinkOptions& opt = {}, bool sysv = true, bool gnu = true) {
    using build::Rel;
    build::Spec main = spec_with({{"f", 0, false}, {"g", 0, false}, {"w", 0, false, STB_WEAK}, {"malloc", 0x8}, {"op_new", 0xc}, {"u", 0, false},
                                  {"hid", 0x4, true, STB_GLOBAL, STT_FUNC, STV_HIDDEN}, {"x", 0, false},
                                  {"_init", 0x10, true, STB_GLOBAL, STT_FUNC, STV_PROTECTED}},
                                 {{0, R_AARCH64_JUMP_SLOT, 1, 0, true}, {1, R_AARCH64_JUMP_SLOT, 2, 0, true}, {2, R_AARCH64_GLOB_DAT, 3},
                                  {3, R_AARCH64_ABS64, 2, 0x10}, {4, R_AARCH64_RELATIVE, 0, 0x20}, {5, R_AARCH64_JUMP_SLOT, 6, 0, true},
                                  {6, R_AARCH64_ABS64, 7, 4}, {12, R_AARCH64_ABS64, 8}});
    main.relr = {7, 8, 9, 10, 40};
    main.words[7] = 0x4, main.words[8] = 0x8, main.words[9] = 0xc, main.words[10] = 0, main.words[40] = 0x10;
    main.init_first = 7, main.init_count = 2;
    main.sysv = sysv, main.gnu = gnu;
    build::Spec sub = spec_with({{"f", 0x4}, {"g", 0, false}, {"pv", 0, false}}, {{0, R_AARCH64_GLOB_DAT, 2}, {1, R_AARCH64_GLOB_DAT, 3}});
    build::Spec sdk = spec_with({{"f", 0x8}, {"g", 0xc}, {"op_new", 0x4}, {"malloc", 0, false}, {"loc", 0x0, true, STB_LOCAL},
                                 {"_init", 0x10, true, STB_GLOBAL, STT_FUNC, STV_PROTECTED}, {"pv", 0x14, true, STB_GLOBAL, STT_OBJECT, STV_PROTECTED}},
                                {{0, R_AARCH64_JUMP_SLOT, 3, 0, true}, {1, R_AARCH64_JUMP_SLOT, 4, 0, true}, {2, R_AARCH64_GLOB_DAT, 2},
                                 {3, R_AARCH64_ABS64, 5, 1}, {4, R_AARCH64_ABS64, 6}});
    sdk.sysv = sysv, sdk.gnu = gnu;
    build::Spec rtld = spec_with({{"x", 0x4}, {"g", 0, false}}, {{0, R_AARCH64_GLOB_DAT, 2}});
    Linked l;
    std::vector<Module> mods;
    const char* names[4] = {"main", "subsdk0", "sdk", "rtld"};
    int k = 0;
    for (const build::Spec* s : {&main, &sub, &sdk, &rtld}) {
        l.built.push_back(build::build_nso(*s));
        mods.push_back(load_module(names[k++], l.built.back().nso));
    }
    l.p = link(std::move(mods), opt);
    return l;
}

}  // namespace

TEST(link_bases_and_scope) {
    const Linked l = link_four();
    const Program& p = l.p;
    CHECK_EQ(p.bases[0], 0x7100000000u);
    for (int i = 1; i < 4; i++) CHECK_EQ(p.bases[i], p.bases[i - 1] + p.modules[i - 1].image_size());
    CHECK(p.in_scope[0] && p.in_scope[1] && p.in_scope[2] && !p.in_scope[3]);
    CHECK_EQ(p.index("sdk"), 2);
    CHECK_EQ(p.module_at(p.bases[1] + 1), 1);
    CHECK_EQ(p.module_at(p.end()), -1);
    CHECK_EQ(p.module_at(0x7100000000 - 1), -1);
    auto f = p.find("f");
    CHECK(f && f->first == 1);
    CHECK(!p.find("x"));   // only rtld exports it
    CHECK(!p.find("hid"));
}

TEST(link_binding_rules) {
    for (auto [sysv, gnu] : {std::pair{true, true}, std::pair{true, false}, std::pair{false, true}}) {
        const Linked l = link_four({}, sysv, gnu);
        const Program& p = l.p;
        auto r = [&](int m, uint32_t s) { return p.resolution[size_t(m)][s]; };
        CHECK(r(0, 1).kind == Bound::other && r(0, 1).provider == 1 && r(0, 1).address == p.bases[1] + 4);   // f: subsdk0 before sdk
        CHECK(r(0, 2).kind == Bound::other && r(0, 2).provider == 2 && r(0, 2).address == p.bases[2] + 0xc);
        CHECK(r(0, 3).kind == Bound::weak_zero && r(0, 3).address == 0);
        CHECK(r(0, 6).kind == Bound::unresolved);
        CHECK(r(0, 7).kind == Bound::self && r(0, 7).address == p.bases[0] + 4);   // hidden
        CHECK(r(0, 8).kind == Bound::unresolved);   // rtld is outside the scope
        CHECK(r(0, 4).kind == Bound::self);
        CHECK(r(2, 3).kind == Bound::other && r(2, 3).provider == 0 && r(2, 3).address == p.bases[0] + 0xc);   // op_new: main's wins
        CHECK(r(2, 4).kind == Bound::other && r(2, 4).provider == 0);   // malloc from main
        CHECK(r(2, 2).kind == Bound::self);
        CHECK(r(2, 5).kind == Bound::self && r(2, 5).address == p.bases[2]);   // local
        CHECK(r(3, 2).kind == Bound::other && r(3, 2).provider == 2);
        CHECK(r(3, 1).kind == Bound::self);
        CHECK(r(0, 9).kind == Bound::self);
        CHECK(r(2, 6).kind == Bound::self && r(2, 6).address == p.bases[2] + 0x10);   // protected: never preempted by main's
        CHECK(r(1, 3).kind == Bound::other && r(1, 3).provider == 2 && r(1, 3).address == p.bases[2] + 0x14);   // but importable
        CHECK_EQ(p.problems.size(), 2u);   // u and x
    }
}

TEST(link_relocated_images) {
    const Linked l = link_four();
    const Program& p = l.p;
    const uint64_t b0 = p.bases[0], b1 = p.bases[1], b2 = p.bases[2], w0 = l.built[0].words, w2 = l.built[2].words, w3 = l.built[3].words;
    CHECK_EQ(word(p, 0, w0, 0), b1 + 4);
    CHECK_EQ(word(p, 0, w0, 1), b2 + 0xc);
    CHECK_EQ(word(p, 0, w0, 2), 0u);
    CHECK_EQ(word(p, 0, w0, 3), b2 + 0xc + 0x10);
    CHECK_EQ(word(p, 0, w0, 4), b0 + 0x20);
    CHECK_EQ(word(p, 0, w0, 5), 0u);
    CHECK_EQ(word(p, 0, w0, 6), b0 + 8);
    CHECK_EQ(word(p, 0, w0, 7), b0 + 4);
    CHECK_EQ(word(p, 0, w0, 10), b0);
    CHECK_EQ(word(p, 0, w0, 40), b0 + 0x10);
    CHECK_EQ(word(p, 0, w0, 11), 0u);   // untouched
    CHECK_EQ(word(p, 2, w2, 0), b0 + 0xc);
    CHECK_EQ(word(p, 2, w2, 1), b0 + 8);
    CHECK_EQ(word(p, 2, w2, 2), b2 + 0xc);
    CHECK_EQ(word(p, 2, w2, 3), b2 + 1);
    CHECK_EQ(word(p, 2, w2, 4), b2 + 0x10);
    CHECK_EQ(word(p, 1, l.built[1].words, 1), b2 + 0x14);
    CHECK_EQ(word(p, 3, w3, 0), b2 + 0xc);
    const RelocStats& s = p.stats[0];
    CHECK_EQ(s.relr, 5u);
    CHECK_EQ(s.sym_other, 3u);
    CHECK_EQ(s.sym_self, 1u);
    CHECK_EQ(s.sym_weak_zero, 1u);
    CHECK_EQ(s.sym_unresolved, 2u);
    CHECK_EQ(s.applied.at(R_AARCH64_JUMP_SLOT), 3u);
    CHECK_EQ(s.applied.at(R_AARCH64_RELATIVE), 1u);
    CHECK_EQ(p.modules[0].relr.size(), 5u);
    CHECK_EQ(p.read64(0, *p.modules[0].dyn.get(DT_INIT_ARRAY) + 8), b0 + 8);
    CHECK(p.modules[0].relative_targets() == (std::vector<uint64_t>{4, 8, 0xc, 0, 0x10, 0x20}));
    CHECK(p.memory[0].size() == p.modules[0].image_size());
    CHECK(p.modules[0].image != p.memory[0] && word(p, 1, l.built[1].words, 0) == b2 + 0xc);
}

TEST(link_options) {
    LinkOptions o;
    o.base = 0x80000000;
    o.bases["sdk"] = 0x7200000000;
    const Linked l = link_four(o);
    CHECK_EQ(l.p.bases[0], 0x80000000u);
    CHECK_EQ(l.p.bases[2], 0x7200000000u);
    CHECK_EQ(l.p.bases[3], 0x7200000000u + l.p.modules[2].image_size());
    o.relocate = false;
    CHECK(link_four(o).p.memory.empty());
    LinkOptions overlap;
    overlap.bases["subsdk0"] = default_base;
    CHECK_THROWS(link_four(overlap));
    LinkOptions unaligned;
    unaligned.base = 0x7100000010;
    CHECK_THROWS(link_four(unaligned));
}

namespace {

std::vector<uint8_t> npdm_image(uint32_t acid_magic_at) {
    Bytes f;
    f.fill_to(0x80);
    std::memcpy(f.b.data(), "META", 4);
    f.b[0x0c] = 0x27, f.b[0x0e] = 44, f.b[0x0f] = 0;
    f.at32(0x1c, 0x100000);
    std::memcpy(f.b.data() + 0x20, "Application", 11);
    auto caps = [](Bytes& b) {
        b.u32(0x7 | 59u << 4 | 28u << 10 | 0u << 16 | 3u << 24);        // thread info
        b.u32(0xf | 0xfffffeu << 5 | 0u << 29);                         // SVCs 0x01-0x17
        b.u32(0xf | 0x000003u << 5 | 1u << 29);                         // SVCs 0x18-0x19
        b.u32(0xf | 0x000100u << 5 | 5u << 29);                         // SVC 0x80
        b.u32(0x1fff | 1u << 14);                                       // program type 1
        b.u32(0x3fff | 1u << 15 | 9u << 19);                            // kernel 9.1
        b.u32(0x7fff | 512u << 16);                                     // handle table
        b.u32(0xffff | 1u << 17);                                       // misc flags
        b.u32(0xffffffff);                                              // padding
    };
    auto sac = [](Bytes& b) {
        b.u8(6), b.bytes(bytes({'f', 's', 'p', '-', 's', 'r', 'v'}));
        b.u8(0x80 | 2), b.bytes(bytes({'s', 'r', 'v'}));
    };
    const size_t aci = f.size();
    f.bytes(bytes({'A', 'C', 'I', '0'}));
    f.fill_to(aci + 0x10);
    f.u64(0x0100000000000000);
    f.fill_to(aci + 0x40);
    const size_t fs = f.size() - aci;
    f.u32(1), f.u64(0x8000000000000000);
    const size_t s0 = f.size() - aci;
    sac(f);
    const size_t s1 = f.size() - aci;
    f.pad(4);
    const size_t k0 = f.size() - aci;
    caps(f);
    const size_t k1 = f.size() - aci;
    f.at32(aci + 0x20, fs), f.at32(aci + 0x24, s0 - fs);
    f.at32(aci + 0x28, s0), f.at32(aci + 0x2c, s1 - s0);
    f.at32(aci + 0x30, k0), f.at32(aci + 0x34, k1 - k0);
    f.at32(0x70, aci), f.at32(0x74, k1);
    f.pad(16);
    const size_t acid = f.size();
    f.fill_to(acid + acid_magic_at);
    f.bytes(bytes({'A', 'C', 'I', 'D'}));
    f.fill_to(acid + acid_magic_at + 0x0c);
    f.u32(3), f.u64(0x0400000000010000), f.u64(0x04ffffffffffffff);
    f.fill_to(acid + acid_magic_at + 0x40);
    const size_t afs = f.size() - acid;
    f.u32(1), f.u64(0x8000000000000001);
    const size_t as0 = f.size() - acid;
    sac(f);
    const size_t as1 = f.size() - acid;
    f.pad(4);
    const size_t ak0 = f.size() - acid;
    caps(f);
    const size_t ak1 = f.size() - acid;
    const size_t m = acid + acid_magic_at;
    f.at32(m + 0x20, afs), f.at32(m + 0x24, as0 - afs);
    f.at32(m + 0x28, as0), f.at32(m + 0x2c, as1 - as0);
    f.at32(m + 0x30, ak0), f.at32(m + 0x34, ak1 - ak0);
    f.at32(0x78, acid), f.at32(0x7c, ak1);
    return f.b;
}

}  // namespace

TEST(npdm_capabilities) {
    for (uint32_t at : {0x200u, 0x300u}) {
        const Npdm n = parse_npdm(npdm_image(at));
        CHECK_EQ(n.name, "Application");
        CHECK(n.is_64bit() && n.address_space() == 3);
        CHECK_EQ(n.main_thread_priority, 44);
        CHECK_EQ(n.main_thread_stack_size, 0x100000u);
        CHECK_EQ(n.aci0.program_id, 0x0100000000000000u);
        CHECK_EQ(n.acid.magic_offset, at);
        CHECK_EQ(n.acid.program_id_min, 0x0400000000010000u);
        CHECK_EQ(n.acid.program_id_max, 0x04ffffffffffffffu);
        CHECK_EQ(n.acid.flags, 3u);
        CHECK_EQ(n.aci0.fs_permissions, 0x8000000000000000u);
        CHECK_EQ(n.acid.fs_permissions, 0x8000000000000001u);
        for (const AccessControl* a : {&n.aci0, &n.acid}) {
            const KernelCaps& k = a->kc;
            CHECK_EQ(k.raw.size(), 9u);
            CHECK_EQ(k.svcs.count(), 23u + 2u + 1u);
            CHECK(!k.svcs[0] && k.svcs[1] && k.svcs[0x17] && k.svcs[0x18] && k.svcs[0x19] && !k.svcs[0x1a] && k.svcs[0x80]);
            CHECK(k.has_thread_info && k.highest_priority == 28 && k.lowest_priority == 59 && k.max_core == 3);
            CHECK(k.program_type == 1u && k.kernel_major == 9u && k.kernel_minor == 1u && k.handle_table_size == 512u && k.misc_flags == 1u);
            CHECK_EQ(k.kinds.at(4), 3);
            CHECK_EQ(k.kinds.at(32), 1);
            CHECK_EQ(a->services.size(), 2u);
            if (a->services.size() == 2) {
                CHECK(a->services[0].name == "fsp-srv" && !a->services[0].server);
                CHECK(a->services[1].name == "srv" && a->services[1].server);
            }
        }
    }
    auto bad = npdm_image(0x300);
    const uint32_t acid = nxw::load_le<uint32_t>(bad, 0x78);
    bad[acid + 0x300] = 'X';
    CHECK_THROWS(parse_npdm(bad));
    bad = npdm_image(0x300);
    bad[0] = 'X';
    CHECK_THROWS(parse_npdm(bad));
    bad = npdm_image(0x300);
    build::Bytes w{bad};
    w.at32(0x74, 0x10000);   // ACI0 size past the file
    CHECK_THROWS(parse_npdm(w.b));
}

int main() { return check::run(); }
