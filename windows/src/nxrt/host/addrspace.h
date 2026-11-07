// Guest memory primitives over the reserved ranges and the guest section (host/addrspace.c on Windows, host/addrspace_linux.c on Linux)
#pragma once
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
enum { AS_NONE = PAGE_NOACCESS, AS_RO = PAGE_READONLY, AS_RW = PAGE_READWRITE, AS_RX = PAGE_EXECUTE_READ };
#else
enum { AS_NONE, AS_RO, AS_RW, AS_RX };
#endif

#ifdef __cplusplus
extern "C" {
#endif
int as_have_placeholders(void);
int as_commit_private(uint64_t a, uint64_t n, int prot);     // a free reserved range → private committed memory
int as_release_private(uint64_t a, uint64_t n);              // private memory → reserved
int as_section_offset(uint64_t a, uint64_t n, uint64_t *off);   // where a range of the alias / stack / heap regions lies in the section
int as_map_view(uint64_t a, uint64_t n, uint64_t off);       // a view of the section at a (a reserved range)
int as_unmap_view(uint64_t a);                               // the view at a → reserved
void as_coalesce(uint64_t lo, uint64_t hi);
int as_guest_views(void);                                    // alias and heap views (once)
int as_commit(uint64_t a, uint64_t n);
int as_protect(uint64_t a, uint64_t n, int prot);
int as_discard(uint64_t a, uint64_t n);
#ifdef __cplusplus
}
#endif
