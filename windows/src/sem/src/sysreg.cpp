// Which system registers the semantics handle inline. Its own object file, so the lifter can link it without the rest.
#include "nxw/sem.hpp"

using namespace nxw::sem;

extern "C" int nxw_sem_sysreg_inline(uint32_t sr, int write) {
    if (write) return sr == sr_nzcv || sr == sr_fpsr || sr == sr_tpidr_el0;   // fpcr goes to the runtime (MXCSR)
    switch (sr) {
    case sr_nzcv:
    case sr_fpcr:
    case sr_fpsr:
    case sr_tpidr_el0:
    case sr_tpidrro_el0:
    case sr_ctr_el0:
    case sr_dczid_el0:
    case sr_cntfrq_el0: return 1;
    default: return 0;   // cntpct_el0, cntvct_el0 and anything rarer: rt_mrs
    }
}
