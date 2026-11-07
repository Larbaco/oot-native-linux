// The DM_CTL "prof" / NXRT_PROF entry points on Linux. The Windows sampler (prof.c) suspends threads itself; here perf does this better from outside the process
// (perf record -g -p <pid>), and the JIT-less shared object has real symbols for it.
#include "dm.h"
void nxrt_prof_start(double secs) { (void)secs; LOG("[prof] use perf on Linux: perf record -F 999 -g -p %d", (int)getpid()); }
void nxrt_prof_env(void) { if (getenv("NXRT_PROF")) nxrt_prof_start(0); }
