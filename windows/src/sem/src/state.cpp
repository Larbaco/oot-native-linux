// Moving the register state between NxwGuestCpu (the ABI, nxnative's frame) and the local State, and the D4 lookup.
#include "common.hpp"

using namespace nxw::sem;

typedef u64 __attribute__((may_alias)) U64a;

// Straight-line code, and the vector registers as whole 16-byte values (not memcpy), so SROA turns each field into SSA values
extern "C" __attribute__((no_builtin("memcpy"))) void nxw_sem_enter(State* s, const NxwGuestCpu* cpu) {
#pragma clang loop unroll(full) vectorize(disable)
    for (unsigned i = 0; i < 31; i++) s->x[i] = cpu->x[i];
    s->x[31] = 0;
#pragma clang loop unroll(full) vectorize(disable)
    for (unsigned i = 0; i < 32; i++) s->q[i] = *reinterpret_cast<const V128*>(cpu->q[i]);
    s->sp = cpu->sp;
    const u64 f = cpu->nzcv;
    s->n = f >> 31 & 1, s->z = f >> 30 & 1, s->c = f >> 29 & 1, s->v = f >> 28 & 1;
    s->fpcr = cpu->fpcr, s->fpsr = cpu->fpsr;
    s->tpidr_el0 = cpu->tpidr_el0, s->tpidrro_el0 = cpu->tpidrro_el0;
    s->excl_addr = cpu->excl_addr;
    s->excl_val[0] = reinterpret_cast<const U64a*>(cpu->excl_val)[0], s->excl_val[1] = reinterpret_cast<const U64a*>(cpu->excl_val)[1];
}

// Stores the fields the masks select (bits in nxw/sem.hpp). The lifter passes the fields its function's instructions write:
// any other field still holds what the last nxw_sem_enter read, which is what cpu holds.
extern "C" __attribute__((no_builtin("memcpy"))) void nxw_sem_leave_mask(const State* s, NxwGuestCpu* cpu, u64 m0, u64 m1) {
#pragma clang loop unroll(full) vectorize(disable)
    for (unsigned i = 0; i < 31; i++)
        if (m0 >> i & 1) cpu->x[i] = s->x[i];
    if (m0 >> 31 & 1) cpu->sp = s->sp;
#pragma clang loop unroll(full) vectorize(disable)
    for (unsigned i = 0; i < 32; i++)
        if (m0 >> (32 + i) & 1) *reinterpret_cast<V128*>(cpu->q[i]) = s->q[i];
    if (m1 & lm_nzcv) cpu->nzcv = u64(s->n) << 31 | u64(s->z) << 30 | u64(s->c) << 29 | u64(s->v) << 28;
    if (m1 & lm_fpcr) cpu->fpcr = s->fpcr;
    if (m1 & lm_fpsr) cpu->fpsr = s->fpsr;
    if (m1 & lm_tpidr) cpu->tpidr_el0 = s->tpidr_el0;
    if (m1 & lm_tpidrro) cpu->tpidrro_el0 = s->tpidrro_el0;
    if (m1 & lm_excl) {
        cpu->excl_addr = s->excl_addr;
        reinterpret_cast<U64a*>(cpu->excl_val)[0] = s->excl_val[0], reinterpret_cast<U64a*>(cpu->excl_val)[1] = s->excl_val[1];
    }
}

extern "C" void nxw_sem_leave(const State* s, NxwGuestCpu* cpu) { nxw_sem_leave_mask(s, cpu, ~0ull, ~0ull); }

extern "C" NxwGuestFn nxw_sem_lookup(NxwGuestCpu* cpu, uint64_t target) {
    const u64 i = (target - rt_code_base) >> 2;
    if (i < rt_code_words) {
        const u32 r = rt_code_table[i];
        if (r) return reinterpret_cast<NxwGuestFn>(rt_host_base + r);
    }
    return rt_miss(cpu, target);
}
