#include "nxanalyze/db.hpp"

#include "nxw/common.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nxanalyze {

using nxw::fail;

uint32_t Database::add_string(std::string_view s) {
    if (s.empty()) return 0;
    const auto off = uint32_t(strings.size());
    strings.append(s);
    strings.push_back('\0');
    return off;
}

namespace {

template <class F> void for_sections(const Database& db, F&& f) {
    f(Sec::strings, 1, db.strings.data(), db.strings.size());
    f(Sec::modules, sizeof(ModuleRec), db.modules.data(), db.modules.size());
    f(Sec::functions, sizeof(FunctionRec), db.functions.data(), db.functions.size());
    f(Sec::blocks, sizeof(BlockRec), db.blocks.data(), db.blocks.size());
    f(Sec::edges, sizeof(EdgeRec), db.edges.data(), db.edges.size());
    f(Sec::calls, sizeof(CallRec), db.calls.data(), db.calls.size());
    f(Sec::indirect, sizeof(IndirectRec), db.indirect.data(), db.indirect.size());
    f(Sec::jump_tables, sizeof(JumpTableRec), db.jump_tables.data(), db.jump_tables.size());
    f(Sec::jt_targets, sizeof(uint32_t), db.jt_targets.data(), db.jt_targets.size());
    f(Sec::call_sites, sizeof(CallSiteRec), db.call_sites.data(), db.call_sites.size());
    f(Sec::sites, sizeof(SiteRec), db.sites.data(), db.sites.size());
    f(Sec::spin_loops, sizeof(SpinRec), db.spin_loops.data(), db.spin_loops.size());
    f(Sec::symbols, sizeof(SymbolRec), db.symbols.data(), db.symbols.size());
    f(Sec::plt_stubs, sizeof(PltStubRec), db.plt_stubs.data(), db.plt_stubs.size());
}

template <class T> void take(std::vector<T>& v, const uint8_t* p, uint64_t n) {
    v.resize(size_t(n));
    if (n) std::memcpy(v.data(), p, size_t(n) * sizeof(T));
}

}  // namespace

std::vector<uint8_t> serialize(const Database& db) {
    std::vector<DbSection> secs;
    uint64_t off = sizeof(DbHeader);
    for_sections(db, [&](Sec, uint32_t, const void*, size_t) { off += sizeof(DbSection); });
    for_sections(db, [&](Sec id, uint32_t rs, const void*, size_t n) {
        off = (off + 7) & ~uint64_t(7);
        secs.push_back({uint32_t(id), rs, off, n});
        off += uint64_t(rs) * n;
    });
    std::vector<uint8_t> out(size_t(off), 0);
    DbHeader h{};
    std::memcpy(h.magic, db_magic, sizeof h.magic);
    h.version = db_version;
    h.header_size = sizeof(DbHeader);
    h.section_count = uint32_t(secs.size());
    h.section_entry_size = sizeof(DbSection);
    h.file_size = off;
    std::snprintf(h.producer, sizeof h.producer, "nxanalyze %u", db_version);
    std::memcpy(out.data(), &h, sizeof h);
    std::memcpy(out.data() + sizeof h, secs.data(), secs.size() * sizeof(DbSection));
    size_t k = 0;
    for_sections(db, [&](Sec, uint32_t rs, const void* data, size_t n) {
        if (n) std::memcpy(out.data() + secs[k].offset, data, size_t(rs) * n);
        k++;
    });
    return out;
}

Database deserialize(const std::vector<uint8_t>& b) {
    if (b.size() < sizeof(DbHeader)) fail("program.db: too short");
    DbHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    if (std::memcmp(h.magic, db_magic, sizeof h.magic)) fail("program.db: bad magic");
    if (h.version != db_version) fail("program.db: version " + std::to_string(h.version) + ", expected " + std::to_string(db_version));
    if (h.file_size != b.size() || h.section_entry_size < sizeof(DbSection) ||
        h.header_size + uint64_t(h.section_count) * h.section_entry_size > b.size())
        fail("program.db: inconsistent header");
    Database db;
    db.strings.clear();
    for (uint32_t i = 0; i < h.section_count; i++) {
        DbSection s;
        std::memcpy(&s, b.data() + h.header_size + size_t(i) * h.section_entry_size, sizeof s);
        if (s.offset > b.size() || s.count > (b.size() - s.offset) / std::max<uint32_t>(s.record_size, 1)) fail("program.db: section out of range");
        const uint8_t* p = b.data() + s.offset;
        auto want = [&](size_t rs) {
            if (s.record_size != rs) fail("program.db: section " + std::to_string(s.id) + " has records of " + std::to_string(s.record_size) + " bytes");
        };
        switch (Sec(s.id)) {
        case Sec::strings: want(1), db.strings.assign(reinterpret_cast<const char*>(p), size_t(s.count)); break;
        case Sec::modules: want(sizeof(ModuleRec)), take(db.modules, p, s.count); break;
        case Sec::functions: want(sizeof(FunctionRec)), take(db.functions, p, s.count); break;
        case Sec::blocks: want(sizeof(BlockRec)), take(db.blocks, p, s.count); break;
        case Sec::edges: want(sizeof(EdgeRec)), take(db.edges, p, s.count); break;
        case Sec::calls: want(sizeof(CallRec)), take(db.calls, p, s.count); break;
        case Sec::indirect: want(sizeof(IndirectRec)), take(db.indirect, p, s.count); break;
        case Sec::jump_tables: want(sizeof(JumpTableRec)), take(db.jump_tables, p, s.count); break;
        case Sec::jt_targets: want(sizeof(uint32_t)), take(db.jt_targets, p, s.count); break;
        case Sec::call_sites: want(sizeof(CallSiteRec)), take(db.call_sites, p, s.count); break;
        case Sec::sites: want(sizeof(SiteRec)), take(db.sites, p, s.count); break;
        case Sec::spin_loops: want(sizeof(SpinRec)), take(db.spin_loops, p, s.count); break;
        case Sec::symbols: want(sizeof(SymbolRec)), take(db.symbols, p, s.count); break;
        case Sec::plt_stubs: want(sizeof(PltStubRec)), take(db.plt_stubs, p, s.count); break;
        default: break;   // sections a newer writer added
        }
    }
    if (db.strings.empty() || db.strings.back() != '\0') fail("program.db: string table not terminated");
    return db;
}

void write_db(const std::filesystem::path& p, const Database& db) { nxw::write_file(p, serialize(db)); }
Database read_db(const std::filesystem::path& p) { return deserialize(nxw::read_file(p)); }

}  // namespace nxanalyze
