// The save commit on Linux (DESIGN D13, the same contract as save_txn.c on NTFS). Where the file system can swap two directories atomically, the commit is one operation:
//   1. copy the work directory tree to <save>.new, flushing every file and directory;
//   2. swap <save> and <save>.new with renameat2(RENAME_EXCHANGE) and flush the parent;
//   3. delete <save>.new, which is the old version now.
// Other file systems refuse the swap (eCryptfs, NFS, some FUSE ones). There, as on NTFS, the commit is a protocol a later start can finish or undo: after step 1, write the
// marker <save>.commit, rename <save> to <save>.old and <save>.new to <save>, delete the marker, delete <save>.old.
// save_txn_recover, before a save is mounted: with the marker, .new is complete, so the commit is finished; without it, a .new is an unfinished copy or the swapped-out old
// version and goes, and a missing <save> with an .old beside it is put back. Killed at any moment, the save is the old version or the new one, never a mix.
// Copies use copy_file_range, which reflinks on btrfs and XFS and is an in-kernel copy elsewhere.
#include "save_txn.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

static void kill_point(const char *name) {
    static int armed = -1, left; static char at[32];
    if (armed < 0) {
        const char *e = getenv("NXRT_SAVE_KILL_AT"); armed = e && *e;
        if (armed) { snprintf(at, sizeof at, "%s", e); char *c = strchr(at, ':'); left = 1; if (c) { *c = 0; left = atoi(c + 1); } }
    }
    if (armed && !strcmp(at, name) && !--left) _exit(77);
}
static int flush_dir(const char *p) { int fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (fd < 0) return -1; int r = fsync(fd); close(fd); return r; }
static int flush_parent(const char *p) {
    char b[PATH_MAX]; snprintf(b, sizeof b, "%s", p);
    char *s = strrchr(b, '/'); if (s == b) s[1] = 0; else if (s) *s = 0; else snprintf(b, sizeof b, ".");
    return flush_dir(b);
}
static int copy_xattrs(const char *from, const char *to) {
    char names[1024]; ssize_t n = llistxattr(from, names, sizeof names);
    for (ssize_t k = 0; k < n;) {
        const char *x = names + k; char v[1024]; ssize_t l = lgetxattr(from, x, v, sizeof v);
        if (l >= 0) lsetxattr(to, x, v, (size_t)l, 0);
        k += (ssize_t)strlen(x) + 1;
    }
    return 0;
}
static int copy_file(const char *from, const char *to) {
    int a = open(from, O_RDONLY | O_CLOEXEC); if (a < 0) return -1;
    struct stat st; if (fstat(a, &st)) { int e = errno; close(a); errno = e; return -1; }
    int b = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777); if (b < 0) { int e = errno; close(a); errno = e; return -1; }
    off_t left = st.st_size; int rc = 0;
    while (left > 0) {
        ssize_t k = copy_file_range(a, NULL, b, NULL, (size_t)left, 0);
        if (k < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {   // across file systems on an older kernel
            static _Thread_local char *tb;
            if (!tb) tb = malloc(1 << 20);
            k = read(a, tb, 1 << 20); if (k > 0) { char *q = tb; ssize_t m = k; while (m > 0) { ssize_t w = write(b, q, (size_t)m); if (w < 0) { if (errno == EINTR) continue; k = -1; break; } q += w; m -= w; } }
        }
        if (k < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (!k) break;
        left -= k;
    }
    if (!rc && fsync(b)) rc = -1;
    int e = errno; close(a); close(b); errno = e;
    if (!rc) copy_xattrs(from, to);
    return rc;
}
int save_txn_copy_tree(const char *from, const char *to) {
    struct stat st; if (lstat(from, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { int r = copy_file(from, to); kill_point("copied"); return r; }
    if (mkdir(to, 0777) && errno != EEXIST) return -1;
    DIR *d = opendir(from); if (!d) return -1;
    struct dirent *e; int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX];
        if (snprintf(a, sizeof a, "%s/%s", from, e->d_name) >= (int)sizeof a || snprintf(b, sizeof b, "%s/%s", to, e->d_name) >= (int)sizeof b) { errno = ENAMETOOLONG; rc = -1; break; }
        rc = save_txn_copy_tree(a, b);
    }
    int er = errno; closedir(d); errno = er;
    if (!rc) { copy_xattrs(from, to); rc = flush_dir(to); }
    return rc;
}
int save_txn_rm_tree(const char *p) {
    struct stat st; if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX]; snprintf(a, sizeof a, "%s/%s", p, e->d_name);
        rc |= save_txn_rm_tree(a);
    }
    closedir(d);
    return rc | rmdir(p);
}
static void names(const char *save, char *neu, char *old, char *mark) {
    snprintf(neu, PATH_MAX, "%s.new", save); snprintf(old, PATH_MAX, "%s.old", save); snprintf(mark, PATH_MAX, "%s.commit", save);
}
static int exists(const char *p) { struct stat st; return !lstat(p, &st); }
static int write_marker(const char *mark, const char *save) {
    int fd = open(mark, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); if (fd < 0) return -1;
    int r = fsync(fd); close(fd);
    return r ? r : flush_parent(save);
}
int save_txn_recover(const char *save) {
    char neu[PATH_MAX], old[PATH_MAX], mark[PATH_MAX]; names(save, neu, old, mark);
    if (exists(mark)) {   // .new was complete when the marker was written: roll forward, from whichever step the process died in
        if (exists(neu)) {
            if (exists(save)) { if (save_txn_rm_tree(old) || rename(save, old)) return -1; }
            if (rename(neu, save)) return -1;
            flush_parent(save);
        }
        if (unlink(mark)) return -1;
        flush_parent(save);
        return save_txn_rm_tree(old) ? -1 : 1;
    }
    int did = 0;
    if (exists(neu)) { if (save_txn_rm_tree(neu)) return -1; did = 2; }
    if (exists(old)) {
        if (!exists(save)) { if (rename(old, save)) return -1; flush_parent(save); }
        else if (save_txn_rm_tree(old)) return -1;
        did = 2;
    }
    return did;
}
int save_txn_commit(const char *work, const char *save) {
    char neu[PATH_MAX], old[PATH_MAX], mark[PATH_MAX]; names(save, neu, old, mark);
    if (save_txn_rm_tree(neu)) return -1;
    if (save_txn_copy_tree(work, neu)) { int e = errno; save_txn_rm_tree(neu); errno = e; return -1; }
    if (flush_parent(neu)) return -1;
    kill_point("flushed");
    if (exists(save) && !syscall(SYS_renameat2, AT_FDCWD, neu, AT_FDCWD, save, 2u /* RENAME_EXCHANGE */)) {
        kill_point("renamed_new");
        if (flush_parent(save)) return -1;
        kill_point("unmarked");
        if (save_txn_rm_tree(neu)) return -1;
        kill_point("done");
        return 0;
    }
    if (exists(save) && errno != EINVAL && errno != ENOSYS && errno != ENOTSUP && errno != EPERM) return -1;
    if (write_marker(mark, save)) return -1;
    kill_point("marker");
    if (exists(save) && rename(save, old)) return -1;
    kill_point("renamed_old");
    if (rename(neu, save)) return -1;
    if (flush_parent(save)) return -1;
    kill_point("renamed_new");
    if (unlink(mark) || flush_parent(save)) return -1;
    kill_point("unmarked");
    if (save_txn_rm_tree(old)) return -1;
    kill_point("done");
    return 0;
}
