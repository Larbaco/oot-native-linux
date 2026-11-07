/* The file API the ported nxnative files use, on Linux: the real POSIX calls, with Darwin's xattr and clone calls written over what Linux offers.
   Linux file systems are case-sensitive, so hle_fs.c's case-collision names (which a guest name in an extended attribute recovers) never arise on them; the
   xattr calls stay for a save folder on a case-insensitive mount, under the "user." namespace Linux requires. */
#ifndef NXRT_POSIX_FS_LINUX_H
#define NXRT_POSIX_FS_LINUX_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#include "save_txn.h"

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#define XATTR_NOFOLLOW 1
#define RENAME_SWAP 2
static inline void nx_xname_(char *out, size_t n, const char *name) { snprintf(out, n, "user.%s", name); }
static inline ssize_t nx_getxattr(const char *p, const char *name, void *buf, size_t n, uint32_t pos, int opt) {
    (void)pos; char x[256]; nx_xname_(x, sizeof x, name); return opt & XATTR_NOFOLLOW ? lgetxattr(p, x, buf, n) : getxattr(p, x, buf, n);
}
static inline int nx_setxattr(const char *p, const char *name, const void *v, size_t n, uint32_t pos, int opt) {
    (void)pos; char x[256]; nx_xname_(x, sizeof x, name); return opt & XATTR_NOFOLLOW ? lsetxattr(p, x, v, n, 0) : setxattr(p, x, v, n, 0);
}
static inline int nx_removexattr(const char *p, const char *name, int opt) {
    char x[256]; nx_xname_(x, sizeof x, name); return opt & XATTR_NOFOLLOW ? lremovexattr(p, x) : removexattr(p, x);
}
#define getxattr nx_getxattr
#define setxattr nx_setxattr
#define removexattr nx_removexattr
/* A copy of the tree (reflinked by the kernel where the file system can) */
static inline int clonefile(const char *from, const char *to, int flags) { (void)flags; return save_txn_copy_tree(from, to); }
/* A swap of two names: atomic where the file system can. hle_fs.c uses it only on its work directory, which a start rebuilds, so three renames do elsewhere */
static inline int renamex_np(const char *a, const char *b, int flags) {
    (void)flags;
    if (!syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, 2u /* RENAME_EXCHANGE */)) return 0;
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP && errno != EPERM) return -1;
    char t[4200]; snprintf(t, sizeof t, "%s.swap", a);
    if (rename(a, t)) return -1;
    if (rename(b, a)) { rename(t, a); return -1; }
    if (rename(t, b)) return -1;
    return 0;
}
#endif
