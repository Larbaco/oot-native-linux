// A64 instruction decoder, generated from src/a64/a64.tbl. Covers ARMv8.0-8.5 base, FP, AdvSIMD, crypto (AES, SHA1/SHA256),
// CRC32, LSE, RCpc, PAC and BTI, with the decode boundaries of LLVM 23 (+v8.5a,+crypto). SVE, SME, MTE and FP16 arithmetic are
// unallocated. decode() gives the op; Fields<Op> (generated) or op_fields() give its fields.
#pragma once
#include <cstdint>
#include <span>
#include <string>

#include "nxw/a64_gen.hpp"   // enum class Op, Fields<Op>

namespace nxw::a64 {

struct FieldInfo {
    const char* name;
    uint8_t lo, width;
};
struct Encoding {
    uint32_t mask, value;   // a word encodes the op when (w & mask) == value, outside the table's exclusions
    const char* ext;        // architecture extension tag from the table ("" for the base set)
};

Op decode(uint32_t w);
const char* op_name(Op op);
std::span<const FieldInfo> op_fields(Op op);
const Encoding& op_encoding(Op op);
inline uint32_t field(uint32_t w, const FieldInfo& f) { return w >> f.lo & ((1u << f.width) - 1); }

// LLVM-style assembly ("add x0, x1, #0x10"); branch targets are absolute, computed from pc. "<unknown>" when unallocated.
std::string print(uint32_t w, uint64_t pc);
void print(std::string& out, uint32_t w, uint64_t pc);

}  // namespace nxw::a64
