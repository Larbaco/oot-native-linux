/* The layout asserts live in the header; compiling this as C checks them for the runtime's C code too. */
#include "nxw/guest_cpu.h"

int main(void) { return sizeof(NxwGuestCpu) == 896 ? 0 : 1; }
