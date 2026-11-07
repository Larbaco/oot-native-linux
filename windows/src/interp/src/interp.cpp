// The interpreter loop: decode, dispatch to nxw_sem_<Op>, and do the control transfers the lifter does in IR.
#include "nxw/interp.hpp"

#include "nxw/a64.hpp"
#include "nxw/rt.h"
#include "nxw/sem.hpp"

using nxw::a64::Op;
using nxw::sem::State;

extern "C" {
#define NXW_SEM_OP(NAME) uint64_t nxw_sem_##NAME(State* s, uint32_t w, uint64_t pc);
#include "sem_ops.inc"
#undef NXW_SEM_OP
}

namespace {

using SemFn = uint64_t (*)(State*, uint32_t, uint64_t);

struct Dispatch {
    SemFn fn[nxw::a64::op_count] = {};
    constexpr Dispatch() {
#define NXW_SEM_OP(NAME) fn[unsigned(Op::NAME)] = &nxw_sem_##NAME;
#include "sem_ops.inc"
#undef NXW_SEM_OP
    }
};
constinit const Dispatch dispatch;

thread_local nxw::interp::Stats g_stats;

int64_t sext(uint64_t v, unsigned bits) { return int64_t(v << (64 - bits)) >> (64 - bits); }

uint64_t cond_target(Op op, uint32_t w, uint64_t pc) {
    if (op == Op::TBZ || op == Op::TBNZ) return pc + uint64_t(sext(w >> 5 & 0x3fff, 14) * 4);
    return pc + uint64_t(sext(w >> 5 & 0x7ffff, 19) * 4);
}

// The translated function for a target, or null when rt_code_table has none
NxwGuestFn table_hit(uint64_t target) {
    const uint64_t i = (target - rt_code_base) >> 2;
    if (i >= rt_code_words || !rt_code_table[i]) return nullptr;
    return reinterpret_cast<NxwGuestFn>(rt_host_base + rt_code_table[i]);
}

[[noreturn]] void trap(State& s, NxwGuestCpu* cpu, uint64_t pc, uint32_t why) {
    nxw_sem_leave(&s, cpu);
    cpu->pc = pc;
    rt_trap(cpu, pc, why);
}

void call(State& s, NxwGuestCpu* cpu, uint64_t target) {
    g_stats.calls++;
    nxw_sem_leave(&s, cpu);
    nxw_sem_lookup(cpu, target)(cpu);
    nxw_sem_enter(&s, cpu);
}

}  // namespace

nxw::interp::Stats& nxw::interp::stats() { return g_stats; }

extern "C" void nxw_interp_run(NxwGuestCpu* cpu, uint64_t pc) {
    g_stats.entries++;
    State s;
    nxw_sem_enter(&s, cpu);
    const uint64_t caller_lr = s.x[30];   // where a RET to the host caller goes
    for (;;) {
        const uint32_t w = *reinterpret_cast<const volatile uint32_t*>(uintptr_t(pc));
        const Op op = nxw::a64::decode(w);
        const SemFn fn = dispatch.fn[unsigned(op)];
        g_stats.instructions++;
        switch (op) {
        case Op::B: pc = fn(&s, w, pc); continue;
        case Op::BL:
        case Op::BLR:
        case Op::BLRAA:
        case Op::BLRAAZ: {
            const uint64_t t = fn(&s, w, pc);
            call(s, cpu, t);
            pc += 4;
            continue;
        }
        case Op::B_cond:
        case Op::CBZ:
        case Op::CBNZ:
        case Op::TBZ:
        case Op::TBNZ: pc = fn(&s, w, pc) ? cond_target(op, w, pc) : pc + 4; continue;
        case Op::RET:
        case Op::RETAA:
        case Op::BR:
        case Op::BRAA:
        case Op::BRAAZ: {
            const uint64_t t = fn(&s, w, pc);
            const bool ret = op == Op::RETAA || (op == Op::RET && ((w >> 5 & 31) == 30 || t == caller_lr));
            if (ret) {
                nxw_sem_leave(&s, cpu);
                return;
            }
            // translated code: a tail call; otherwise keep interpreting there (a jump-table label, or the start of the
            // tail-called function, whose RET then returns to our caller: both are right)
            if (const NxwGuestFn f = table_hit(t)) {
                nxw_sem_leave(&s, cpu);
                f(cpu);
                return;
            }
            pc = t;
            continue;
        }
        case Op::SVC:
            nxw_sem_leave(&s, cpu);
            cpu->pc = pc;
            rt_svc(cpu, w >> 5 & 0xffff);
            nxw_sem_enter(&s, cpu);
            pc += 4;
            continue;
        case Op::HINT:   // YIELD, WFE and WFI yield to the host, as in lifted code
            if ((w >> 8 & 15) == 0 && (w >> 5 & 7) >= 1 && (w >> 5 & 7) <= 3) {
                nxw_sem_leave(&s, cpu);
                cpu->pc = pc;
                rt_yield(cpu, (w >> 5 & 7) == 1 ? NXW_YIELD_HINT : NXW_YIELD_WFE, pc);
                nxw_sem_enter(&s, cpu);
                pc += 4;
                continue;
            }
            break;
        case Op::MRS:
        case Op::MSR_reg: {
            const uint32_t sr = w >> 5 & 0x7fff;
            const bool write = op == Op::MSR_reg;
            if (!nxw_sem_sysreg_inline(sr, write)) {
                nxw_sem_leave(&s, cpu);
                cpu->pc = pc;
                const unsigned rt = w & 31;
                if (write) {
                    rt_msr(cpu, sr, rt == 31 ? 0 : cpu->x[rt]);
                } else {
                    const uint64_t v = rt_mrs(cpu, sr);
                    if (rt != 31) cpu->x[rt] = v;
                }
                nxw_sem_enter(&s, cpu);
                pc += 4;
                continue;
            }
            break;
        }
        case Op::UDF: trap(s, cpu, pc, NXW_TRAP_UDF | (w & 0xffff) << 8);
        case Op::BRK: trap(s, cpu, pc, NXW_TRAP_BRK | (w >> 5 & 0xffff) << 8);
        case Op::HLT: trap(s, cpu, pc, NXW_TRAP_HLT | (w >> 5 & 0xffff) << 8);
        case Op::Unallocated: trap(s, cpu, pc, NXW_TRAP_UNDECODABLE);
        case Op::ERET:
        case Op::ERETAA:
        case Op::DRPS: trap(s, cpu, pc, NXW_TRAP_ERET);
        case Op::HVC:
        case Op::SMC:
        case Op::DCPS1:
        case Op::DCPS2:
        case Op::DCPS3:
        case Op::MSR_imm: trap(s, cpu, pc, NXW_TRAP_SYSTEM | uint32_t(op) << 8);
        default: break;
        }
        if (!fn) trap(s, cpu, pc, NXW_TRAP_UNSUPPORTED | uint32_t(op) << 8);
        fn(&s, w, pc);
        pc += 4;
    }
}

extern "C" void nxw_interp_trampoline(NxwGuestCpu* cpu) { nxw_interp_run(cpu, cpu->pc); }
