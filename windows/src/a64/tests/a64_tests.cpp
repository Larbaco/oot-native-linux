// Hand-made encodings for the tricky classes: expected op, fields and LLVM 23's text (llvm-objdump --mattr=+v8.5a,+crypto).
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include "nxw/a64.hpp"

using namespace nxw::a64;

namespace {

struct F {
    const char* name;
    uint32_t value;
};
struct Case {
    uint32_t word;
    uint64_t pc;
    Op op;
    const char* text;
    F fields[7];
};

const Case kCases[] = {
    // bitmask immediates, including reserved encodings and LLVM's MOVZ > MOVN > ORR preference for "mov"
    {0x92407c20, 0, Op::AND_imm, "and x0, x1, #0xffffffff", {{"sf", 1}, {"N", 1}, {"immr", 0}, {"imms", 31}, {"Rn", 1}, {"Rd", 0}}},
    {0x927fffff, 0, Op::Unallocated, "<unknown>", {}},   // N = 1, imms = 111111
    {0x12400000, 0, Op::Unallocated, "<unknown>", {}},   // 32-bit with N = 1
    {0xb201f3ea, 0, Op::ORR_imm, "mov x10, #-0x5555555555555556", {{"N", 0}, {"immr", 1}, {"imms", 60}, {"Rn", 31}, {"Rd", 10}}},
    {0x321043f7, 0, Op::ORR_imm, "orr w23, wzr, #0xffff0001", {{"sf", 0}, {"immr", 16}, {"imms", 16}, {"Rn", 31}}},
    {0xf26f011f, 0, Op::ANDS_imm, "tst x8, #0x20000", {{"N", 1}, {"immr", 47}, {"imms", 0}, {"Rn", 8}, {"Rd", 31}}},
    {0x12003108, 0, Op::AND_imm, "and w8, w8, #0x1fff", {{"imms", 12}}},
    {0x927ae53f, 0, Op::AND_imm, "and sp, x9, #0xffffffffffffffc0", {{"Rd", 31}}},
    // shifted and extended registers
    {0x8b224c20, 0, Op::ADD_ext, "add x0, x1, w2, uxtw #3", {{"sf", 1}, {"Rm", 2}, {"option", 2}, {"imm3", 3}, {"Rn", 1}, {"Rd", 0}}},
    {0xcb2873f9, 0, Op::SUB_ext, "sub x25, sp, x8, lsl #4", {{"Rm", 8}, {"option", 3}, {"imm3", 4}, {"Rn", 31}, {"Rd", 25}}},
    {0x8b887d29, 0, Op::ADD_reg, "add x9, x9, x8, asr #31", {{"shift", 2}, {"Rm", 8}, {"imm6", 31}, {"Rn", 9}, {"Rd", 9}}},
    {0x2a0203e8, 0, Op::ORR_reg, "mov w8, w2", {{"shift", 0}, {"imm6", 0}, {"Rn", 31}, {"Rm", 2}, {"Rd", 8}}},
    {0x2a0817e0, 0, Op::ORR_reg, "orr w0, wzr, w8, lsl #5", {{"imm6", 5}}},
    {0xf868d928, 0, Op::LDR_reg, "ldr x8, [x9, w8, sxtw #3]", {{"sf", 1}, {"Rm", 8}, {"option", 6}, {"S", 1}, {"Rn", 9}, {"Rt", 8}}},
    {0x38796908, 0, Op::LDRB_reg, "ldrb w8, [x8, x25]", {{"option", 3}, {"S", 0}, {"Rm", 25}}},
    // bitfield and extract aliases
    {0x531f7b4a, 0, Op::UBFM, "lsl w10, w26, #1", {{"sf", 0}, {"immr", 31}, {"imms", 30}, {"Rn", 26}, {"Rd", 10}}},
    {0x93407ee8, 0, Op::SBFM, "sxtw x8, w23", {{"sf", 1}, {"N", 1}, {"immr", 0}, {"imms", 31}}},
    {0xd3767d2b, 0, Op::UBFM, "ubfiz x11, x9, #10, #32", {{"immr", 54}, {"imms", 31}}},
    {0x33140fe9, 0, Op::BFM, "bfc w9, #12, #4", {{"immr", 20}, {"imms", 3}, {"Rn", 31}, {"Rd", 9}}},
    {0x13884108, 0, Op::EXTR, "ror w8, w8, #0x10", {{"Rm", 8}, {"imms", 16}, {"Rn", 8}}},
    // SIMD element and indexed forms
    {0x4f801022, 0, Op::FMLA_ve, "fmla v2.4s, v1.4s, v0.s[0]", {{"Q", 1}, {"sz", 0}, {"L", 0}, {"M", 0}, {"Rm", 0}, {"H", 0}, {"Rd", 2}}},
    {0x5fa01023, 0, Op::FMLA_si, "fmla s3, s1, v0.s[1]", {{"sz", 0}, {"L", 1}, {"H", 0}, {"Rn", 1}, {"Rd", 3}}},
    {0x0f40a042, 0, Op::SMULL_ve, "smull v2.4s, v2.4h, v0.h[0]", {{"Q", 0}, {"size", 1}, {"L", 0}, {"M", 0}, {"H", 0}}},
    {0x5e0c0401, 0, Op::DUP_elem_s, "mov s1, v0.s[1]", {{"imm5", 12}, {"Rn", 0}, {"Rd", 1}}},
    {0x4e181c20, 0, Op::INS_gen, "mov v0.d[1], x1", {{"imm5", 24}, {"Rn", 1}}},
    {0x6e180462, 0, Op::INS_elem, "mov v2.d[1], v3.d[0]", {{"imm5", 24}, {"imm4", 0}, {"Rn", 3}, {"Rd", 2}}},
    {0x0e1c3cb7, 0, Op::UMOV, "mov w23, v5.s[3]", {{"Q", 0}, {"imm5", 28}}},
    {0x0e1f3c0c, 0, Op::UMOV, "umov w12, v0.b[15]", {{"imm5", 31}}},
    {0x0dff61c4, 0, Op::LD4_lane_post, "ld4 { v4.h, v5.h, v6.h, v7.h }[0], [x14], #8", {{"Q", 0}, {"Rm", 31}, {"kind", 1}, {"S", 0}, {"size", 0}, {"Rn", 14}, {"Rt", 4}}},
    {0x4ddfc921, 0, Op::LD1R_post, "ld1r { v1.4s }, [x9], #4", {{"Q", 1}, {"Rm", 31}, {"size", 2}}},
    {0x6f00e401, 0, Op::MOVI_64, "movi v1.2d, #0000000000000000", {{"Q", 1}, {"abc", 0}, {"defgh", 0}}},
    {0x2f07e600, 0, Op::MOVI_64, "movi d0, #0xffffffff00000000", {{"Q", 0}, {"abc", 7}, {"defgh", 16}}},
    {0x0f046402, 0, Op::MOVI_32, "movi v2.2s, #0x80, lsl #24", {{"abc", 4}, {"sh", 3}, {"defgh", 0}}},
    {0x4f03f603, 0, Op::FMOV_v_s, "fmov v3.4s, #1.00000000", {{"abc", 3}, {"defgh", 16}}},
    {0x4f168ca4, 0, Op::RSHRN_v, "rshrn2 v4.8h, v5.4s, #0xa", {{"Q", 1}, {"immh", 2}, {"immb", 6}, {"Rn", 5}, {"Rd", 4}}},
    {0x0ee1e004, 0, Op::PMULL, "pmull v4.1q, v0.1d, v1.1d", {{"Q", 0}, {"size", 3}, {"Rm", 1}}},
    {0x1e7e1000, 0, Op::FMOV_imm, "fmov d0, #-1.00000000", {{"ft", 1}, {"imm8", 0xf0}}},
    // load/store pair addressing modes
    {0xa9bf7bfd, 0, Op::STP_pre, "stp x29, x30, [sp, #-0x10]!", {{"sf", 1}, {"imm7", 0x7e}, {"Rt2", 30}, {"Rn", 31}, {"Rt", 29}}},
    {0xa8c27bfd, 0, Op::LDP_post, "ldp x29, x30, [sp], #0x20", {{"imm7", 4}}},
    {0xad4113e1, 0, Op::LDP_fp_off, "ldp q1, q4, [sp, #0x20]", {{"opc", 2}, {"imm7", 2}, {"Rt2", 4}, {"Rt", 1}}},
    {0x694927e8, 0, Op::LDPSW_off, "ldpsw x8, x9, [sp, #0x48]", {{"imm7", 18}}},
    {0xfd4063e8, 0, Op::LDR_fp_imm, "ldr d8, [sp, #0xc0]", {{"size", 3}, {"opc1", 0}, {"imm12", 24}}},
    // system registers and system instructions
    {0xd53bd068, 0, Op::MRS, "mrs x8, TPIDRRO_EL0", {{"o0", 1}, {"op1", 3}, {"CRn", 13}, {"CRm", 0}, {"op2", 3}, {"Rt", 8}}},
    {0xd51b4400, 0, Op::MSR_reg, "msr FPCR, x0", {{"CRn", 4}, {"CRm", 4}, {"op2", 0}}},
    {0xd5300000, 0, Op::MRS, "mrs x0, S2_0_C0_C0_0", {{"o0", 0}}},
    {0xd5034fdf, 0, Op::MSR_imm, "msr DAIFSet, #0xf", {{"op1", 3}, {"CRm", 15}, {"op2", 6}}},
    {0xd50b7e2a, 0, Op::SYS, "dc civac, x10", {{"op1", 3}, {"CRn", 7}, {"CRm", 14}, {"op2", 1}, {"Rt", 10}}},
    {0xd508751f, 0, Op::SYS, "ic iallu", {{"Rt", 31}}},
    {0xd5087520, 0, Op::SYS, "sys #0x0, c7, c5, #0x1, x0", {{"op2", 1}, {"Rt", 0}}},
    {0xd503249f, 0, Op::HINT, "bti j", {{"CRm", 4}, {"op2", 4}}},
    // pointer authentication
    {0xd503237f, 0, Op::HINT, "pacibsp", {{"CRm", 3}, {"op2", 3}}},
    {0xd65f0fff, 0, Op::RETAA, "retab", {{"M", 1}}},
    {0xdac10121, 0, Op::PAC, "pacia x1, x9", {{"Z", 0}, {"D", 0}, {"key", 0}, {"Rn", 9}, {"Rd", 1}}},
    {0xf8600c20, 0, Op::LDRAA, "ldraa x0, [x1, #-0x1000]!", {{"M", 0}, {"S", 1}, {"imm9", 0}, {"W", 1}}},
    // LSE: size and ordering variants
    {0xc8edfd4e, 0, Op::CAS, "casal x13, x14, [x10]", {{"size", 3}, {"L", 1}, {"Rs", 13}, {"o0", 1}, {"Rn", 10}, {"Rt", 14}}},
    {0x78ec0129, 0, Op::LDADD, "ldaddalh w12, w9, [x9]", {{"size", 1}, {"A", 1}, {"R", 1}, {"Rs", 12}}},
    {0xb821001f, 0, Op::LDADD, "stadd w1, [x0]", {{"size", 2}, {"A", 0}, {"R", 0}, {"Rt", 31}}},
    {0x38e18100, 0, Op::SWP, "swpalb w1, w0, [x8]", {{"size", 0}, {"A", 1}, {"R", 1}}},
    {0x38bfc108, 0, Op::LDAPR, "ldaprb w8, [x8]", {{"size", 0}, {"Rn", 8}, {"Rt", 8}}},
    {0x48207c82, 0, Op::CASP, "casp x0, x1, x2, x3, [x4]", {{"sz", 1}, {"L", 0}, {"Rs", 0}, {"o0", 0}, {"Rt", 2}}},
    // branches (targets are absolute) and unallocated words
    {0x54fffea1, 0xe8, Op::B_cond, "b.ne 0xbc", {{"imm19", 0x7fff5}, {"cond", 1}}},
    {0xb707fd08, 0xec, Op::TBNZ, "tbnz x8, #0x20, 0x8c", {{"b5", 1}, {"b40", 0}, {"imm14", 0x3fe8}, {"Rt", 8}}},
    {0x04000000, 0, Op::Unallocated, "<unknown>", {}},   // SVE space
    {0xd9200800, 0, Op::Unallocated, "<unknown>", {}},   // MTE STG (not in the feature set)
    {0x00010000, 0, Op::Unallocated, "<unknown>", {}},
};

int failures = 0;

void check(bool ok, const Case& c, const std::string& what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL %08x: %s\n", c.word, what.c_str());
}

}  // namespace

int main() {
    for (const Case& c : kCases) {
        Op op = decode(c.word);
        check(op == c.op, c, std::string("op ") + op_name(op) + ", expected " + op_name(c.op));
        std::string text = print(c.word, c.pc);
        check(text == c.text, c, "text \"" + text + "\", expected \"" + c.text + "\"");
        for (const F& f : c.fields) {
            if (!f.name) break;
            bool found = false;
            for (const FieldInfo& fi : op_fields(op))
                if (!std::strcmp(fi.name, f.name)) {
                    found = true;
                    check(field(c.word, fi) == f.value, c,
                          std::string("field ") + f.name + " = " + std::to_string(field(c.word, fi)) + ", expected " + std::to_string(f.value));
                }
            check(found, c, std::string("no field ") + f.name);
        }
    }
    // the typed accessors agree with the generic field table
    Fields<Op::ADD_ext> add(0x8b224c20);
    if (add.sf != 1 || add.Rm != 2 || add.option != 2 || add.imm3 != 3 || add.Rn != 1 || add.Rd != 0) {
        ++failures;
        std::puts("FAIL Fields<Op::ADD_ext>");
    }
    Fields<Op::CAS> cas(0xc8edfd4e);
    if (cas.size != 3 || cas.L != 1 || cas.o0 != 1 || cas.Rs != 13) {
        ++failures;
        std::puts("FAIL Fields<Op::CAS>");
    }
    std::printf("a64-tests: %zu cases, %d failures\n", std::size(kCases), failures);
    return failures ? 1 : 0;
}
