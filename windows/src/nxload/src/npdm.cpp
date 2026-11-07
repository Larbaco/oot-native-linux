#include "nxload/npdm.hpp"

#include <bit>

namespace nxload {

using nxw::load_le;

namespace {

void need(bool ok, const std::string& what) {
    if (!ok) fail(what);
}

std::span<const uint8_t> sub(std::span<const uint8_t> b, uint64_t off, uint64_t size, const char* what) {
    need(off <= b.size() && size <= b.size() - off, std::string("npdm: ") + what + " out of range");
    return b.subspan(size_t(off), size_t(size));
}

std::string cstr(std::span<const uint8_t> b) {
    std::string s(b.begin(), b.end());
    return s.substr(0, s.find('\0'));
}

std::vector<Service> parse_services(std::span<const uint8_t> b) {
    std::vector<Service> out;
    for (size_t p = 0; p < b.size();) {
        const uint8_t ctrl = b[p++];
        const size_t len = (ctrl & 7) + 1u;
        need(len <= b.size() - p, "npdm: service access control entry runs past its table");
        out.push_back({std::string(b.begin() + ptrdiff_t(p), b.begin() + ptrdiff_t(p + len)), (ctrl & 0x80) != 0});
        p += len;
    }
    return out;
}

// FS access control (ACI0 and ACID alike): u8 version, 3 bytes, u64 permission bits; service and kernel tables follow, by offset
void parse_tables(AccessControl& ac, std::span<const uint8_t> block, uint64_t fields) {
    const uint32_t fs_off = load_le<uint32_t>(block, size_t(fields + 0x20)), fs_size = load_le<uint32_t>(block, size_t(fields + 0x24));
    const uint32_t sac_off = load_le<uint32_t>(block, size_t(fields + 0x28)), sac_size = load_le<uint32_t>(block, size_t(fields + 0x2c));
    const uint32_t kc_off = load_le<uint32_t>(block, size_t(fields + 0x30)), kc_size = load_le<uint32_t>(block, size_t(fields + 0x34));
    auto fs = sub(block, fs_off, fs_size, "FS access control");
    if (fs.size() >= 12) ac.fs_permissions = load_le<uint64_t>(fs, 4);
    ac.services = parse_services(sub(block, sac_off, sac_size, "service access control"));
    ac.kc = parse_kernel_caps(sub(block, kc_off, kc_size, "kernel capabilities"));
}

}  // namespace

KernelCaps parse_kernel_caps(std::span<const uint8_t> b) {
    need(b.size() % 4 == 0, "npdm: kernel capabilities are not whole words");
    KernelCaps k;
    int map_words = 0;
    for (size_t i = 0; i < b.size(); i += 4) {
        const uint32_t w = load_le<uint32_t>(b, i);
        const int kind = std::countr_one(w);   // the descriptor type is the number of low one bits
        k.raw.push_back(w);
        k.kinds[kind]++;
        switch (kind) {
        case 3:
            k.has_thread_info = true;
            k.lowest_priority = (w >> 4) & 0x3f;
            k.highest_priority = (w >> 10) & 0x3f;
            k.min_core = (w >> 16) & 0xff;
            k.max_core = uint8_t(w >> 24);
            break;
        case 4:
            for (unsigned s = 0; s < 24; s++)
                if (w >> (5 + s) & 1) k.svcs.set((w >> 29) * 24 + s);
            break;
        case 6: map_words++; break;
        case 7: k.io_maps++; break;
        case 11: k.interrupts++; break;
        case 13: k.program_type = (w >> 14) & 7; break;
        case 14:
            k.kernel_minor = (w >> 15) & 0xf;
            k.kernel_major = w >> 19;
            break;
        case 15: k.handle_table_size = (w >> 16) & 0x3ff; break;
        case 16: k.misc_flags = w >> 17; break;
        }
    }
    k.memory_maps = map_words / 2;   // a memory map takes two descriptors
    return k;
}

Npdm parse_npdm(std::span<const uint8_t> f) {
    need(f.size() >= 0x80 && std::memcmp(f.data(), "META", 4) == 0, "npdm: no META header");
    Npdm n;
    n.flags = f[0x0c];
    n.main_thread_priority = f[0x0e];
    n.main_thread_core = f[0x0f];
    n.reserved_10 = load_le<uint32_t>(f, 0x10);
    n.system_resource_size = load_le<uint32_t>(f, 0x14);
    n.version = load_le<uint32_t>(f, 0x18);
    n.main_thread_stack_size = load_le<uint32_t>(f, 0x1c);
    n.name = cstr(f.subspan(0x20, 0x10));
    n.product_code = cstr(f.subspan(0x30, 0x10));

    AccessControl& a = n.aci0;
    a.offset = load_le<uint32_t>(f, 0x70);
    a.size = load_le<uint32_t>(f, 0x74);
    auto aci = sub(f, a.offset, a.size, "ACI0");
    need(aci.size() >= 0x40 && std::memcmp(aci.data(), "ACI0", 4) == 0, "npdm: no ACI0");
    a.program_id = load_le<uint64_t>(aci, 0x10);
    parse_tables(a, aci, 0);

    // ACID: signature and key come first: 0x200 bytes in the classic layout, 0x300 in Switch 2 npdms. Table offsets count from the block start
    AccessControl& c = n.acid;
    c.offset = load_le<uint32_t>(f, 0x78);
    c.size = load_le<uint32_t>(f, 0x7c);
    auto acid = sub(f, c.offset, c.size, "ACID");
    for (uint32_t mo : {0x200u, 0x300u})
        if (acid.size() >= mo + 0x40 && std::memcmp(acid.data() + mo, "ACID", 4) == 0) c.magic_offset = mo;
    need(c.magic_offset != 0, "npdm: no ACID magic at +0x200 or +0x300");
    c.flags = load_le<uint32_t>(acid, c.magic_offset + 0x0c);
    c.program_id_min = load_le<uint64_t>(acid, c.magic_offset + 0x10);
    c.program_id_max = load_le<uint64_t>(acid, c.magic_offset + 0x18);
    parse_tables(c, acid, c.magic_offset);
    return n;
}

}  // namespace nxload
