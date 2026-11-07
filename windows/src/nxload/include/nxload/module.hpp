// One NSO module: header, segments (LZ4, SHA-256), MOD0, dynamic section, dynamic symbols and their hash tables, relocations.
// Offsets are module-relative (the module's image starts at 0); the program linker (program.hpp) adds bases.
#pragma once
#include "nxw/common.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nxload {

using nxw::fail;

inline constexpr uint64_t page_size = 0x1000;
constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// LZ4 block format: src must decompress to exactly dst.size() bytes
void lz4_decompress(std::span<const uint8_t> src, std::span<uint8_t> dst);
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);   // Windows BCrypt, CommonCrypto on the Mac

uint32_t elf_hash(std::string_view name);   // DT_HASH
uint32_t gnu_hash(std::string_view name);   // DT_GNU_HASH

// LEB128, bounds-checked; p advances past the value
uint64_t read_uleb(std::span<const uint8_t> b, size_t& p);
int64_t read_sleb(std::span<const uint8_t> b, size_t& p);

// DT_RELR: the addresses an encoded table relocates
std::vector<uint64_t> decode_relr(std::span<const uint8_t> table);

enum Seg : int { seg_text, seg_ro, seg_data };
inline constexpr const char* seg_names[3] = {"text", "ro", "data"};

struct Segment {
    uint32_t file_off = 0, mem_off = 0, size = 0, file_size = 0;
    bool compressed = false, hashed = false;
    std::array<uint8_t, 32> sha256{};
};

struct Extent { uint32_t off = 0, size = 0; };

struct NsoHeader {
    uint32_t version = 0, flags = 0, bss_size = 0;
    std::array<Segment, 3> seg;
    std::array<uint8_t, 32> module_id{};   // the build ID
    Extent module_name;                    // in the file
    Extent api_info, dynstr, dynsym;       // relative to ro
};
NsoHeader parse_nso_header(std::span<const uint8_t> file);

struct Mod0 {
    uint64_t offset = 0, dynamic = 0, bss_start = 0, bss_end = 0, eh_frame_hdr_start = 0, eh_frame_hdr_end = 0, module_object = 0;
};

enum : int64_t {
    DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_PLTGOT = 3, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
    DT_RELAENT = 9, DT_STRSZ = 10, DT_SYMENT = 11, DT_INIT = 12, DT_FINI = 13, DT_SONAME = 14, DT_SYMBOLIC = 16, DT_REL = 17,
    DT_PLTREL = 20, DT_DEBUG = 21, DT_TEXTREL = 22, DT_JMPREL = 23, DT_BIND_NOW = 24, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26,
    DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28, DT_FLAGS = 30, DT_PREINIT_ARRAY = 32, DT_PREINIT_ARRAYSZ = 33, DT_RELRSZ = 35,
    DT_RELR = 36, DT_RELRENT = 37, DT_GNU_HASH = 0x6ffffef5, DT_RELACOUNT = 0x6ffffff9, DT_RELCOUNT = 0x6ffffffa, DT_FLAGS_1 = 0x6ffffffb,
};
std::string dt_name(int64_t tag);

enum : uint32_t {
    R_AARCH64_NONE = 0, R_AARCH64_ABS64 = 257, R_AARCH64_ABS32 = 258, R_AARCH64_PREL64 = 260, R_AARCH64_COPY = 1024,
    R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027, R_AARCH64_TLS_DTPMOD64 = 1028,
    R_AARCH64_TLS_DTPREL64 = 1029, R_AARCH64_TLS_TPREL64 = 1030, R_AARCH64_TLSDESC = 1031, R_AARCH64_IRELATIVE = 1032,
};
std::string reloc_name(uint32_t type);

enum : uint8_t { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum : uint8_t { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4, STT_COMMON = 5, STT_TLS = 6 };
enum : uint8_t { STV_DEFAULT = 0, STV_INTERNAL = 1, STV_HIDDEN = 2, STV_PROTECTED = 3 };
inline constexpr uint16_t SHN_UNDEF = 0, SHN_ABS = 0xfff1;
std::string bind_name(uint8_t bind);
std::string type_name(uint8_t type);

struct Symbol {
    std::string name;
    uint64_t value = 0, size = 0;
    uint8_t bind = 0, type = 0, visibility = 0;
    uint16_t shndx = 0;
    bool defined() const { return shndx != SHN_UNDEF; }
    bool imported() const { return !defined() && !name.empty(); }
    bool exported() const { return defined() && (bind == STB_GLOBAL || bind == STB_WEAK) && !name.empty(); }   // the census's definition
    bool visible() const { return exported() && (visibility == STV_DEFAULT || visibility == STV_PROTECTED); }   // other modules may bind to it
    bool preemptible() const { return exported() && visibility == STV_DEFAULT; }   // an earlier module's definition overrides it
};

struct Reloc {
    uint64_t offset = 0;
    uint32_t type = 0, sym = 0;
    int64_t addend = 0;
};

struct Dynamic {
    std::vector<std::pair<int64_t, uint64_t>> tags;   // in file order, up to DT_NULL
    std::vector<std::string> needed;
    std::string soname;
    std::optional<uint64_t> get(int64_t tag) const;   // the first such tag
    uint64_t value(int64_t tag) const { return get(tag).value_or(0); }
    size_t count(int64_t tag) const;
};

struct SysvHash {
    std::vector<uint32_t> bucket, chain;
};
struct GnuHash {
    uint32_t symoffset = 0, bloom_shift = 0;
    std::vector<uint64_t> bloom;
    std::vector<uint32_t> bucket, chain;   // chain[i] belongs to symbol symoffset + i
};

struct SvcSite { uint64_t offset; uint32_t imm; };

struct LoadOptions {
    bool verify_hashes = true;   // segments the header marks as hashed
};

struct Module {
    std::string name;
    uint64_t file_size = 0;
    NsoHeader nso;
    bool hashes_checked = false;   // every hashed segment was checked and matched
    std::vector<uint8_t> image;    // text, ro and data at their memory offsets; bss zero; image.size() == image_size()
    Mod0 mod0;
    Dynamic dyn;
    std::vector<Symbol> symbols;   // index 0 is the null symbol
    std::optional<SysvHash> hash;
    std::optional<GnuHash> gnu_hash;
    std::vector<Reloc> rela, jmprel;
    std::vector<uint64_t> relr;           // offsets DT_RELR relocates
    std::vector<std::string> notes;       // inconsistencies found while loading (not fatal)

    std::span<const uint8_t> bytes() const { return image; }
    const Segment& seg(int s) const { return nso.seg[s]; }
    uint64_t seg_end(int s) const { return uint64_t(nso.seg[s].mem_off) + nso.seg[s].size; }
    uint64_t image_size() const { return image.size(); }
    std::string build_id() const;   // the first 16 bytes of the module ID, hex
    std::string where(uint64_t off) const;   // "text", "ro", "data", "bss" or "outside"
    std::string string_at(uint64_t off) const;   // NUL-terminated, bounds-checked

    // Symbol lookup the way rtld does it, through the module's own GNU or SysV hash table (a map when it has neither):
    // the index of the defined, exported symbol with that name, or 0
    uint32_t lookup(std::string_view name) const;
    uint32_t lookup_sysv(std::string_view name) const;
    uint32_t lookup_gnu(std::string_view name) const;
    uint32_t symbol_count_sysv() const;   // nchain, 0 without DT_HASH
    uint32_t symbol_count_gnu() const;    // highest hashed index + 1, 0 without DT_GNU_HASH
    size_t hash_mismatches() const;       // exported symbols some present hash table cannot find

    // Every relative relocation's target (RELR implicit addends, then RELATIVE addends), module-relative
    std::vector<uint64_t> relative_targets() const;
    std::vector<SvcSite> svc_sites() const;   // SVC #imm words in text

    struct NameHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, uint32_t, NameHash, std::equal_to<>> by_name;   // exported symbols, first index wins
};

Module load_module(std::string name, std::span<const uint8_t> file, const LoadOptions& opt = {});
Module load_module_file(const std::filesystem::path& p, const LoadOptions& opt = {});

}  // namespace nxload
