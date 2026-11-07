/* The POSIX file API the ported nxnative files use (hle_fs.c, romfs_image.h, hle_sdk16.c), over the UCRT and Win32, so they stay close to the originals.
   - Descriptors are UCRT low-level descriptors opened in binary mode; pread/pwrite use ReadFile/WriteFile at an explicit offset (64-bit).
   - stat / fstat / lstat are the 64-bit forms (struct stat is _stat64: RomFS images are larger than 2 GB). Include this after any system header.
   - Directories: opendir/readdir over FindFirstFile, with d_type (DT_DIR / DT_REG).
   - Extended attributes are NTFS alternate data streams ("path:name"); like APFS, NTFS is case-insensitive, so hle_fs.c's case-collision names work unchanged.
   - clonefile copies a tree (CopyFile keeps the streams); renamex_np(RENAME_SWAP) swaps two directories with three renames, which is not atomic: hle_fs.c uses it
     only on its work directory, which a start rebuilds; the save's commit is hle/save_txn.c (DESIGN D13). */
#ifndef NXRT_POSIX_FS_WIN_H
#define NXRT_POSIX_FS_WIN_H
#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _SSIZE_T_DEFINED
typedef int64_t ssize_t;
#define _SSIZE_T_DEFINED
#endif
#ifndef F_OK
#define F_OK 0
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#ifndef ENOTSUP
#define ENOTSUP 129
#endif
#define XATTR_NOFOLLOW 1
#define RENAME_SWAP 2

static inline int posix_errno_from_win(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return EACCES;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_NOACCESS: return EFAULT;
    default: return EIO;
    }
}

/* ---- descriptors ---- */
static inline int nx_open(const char *p, int flags, ...) { return _open(p, flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE); }
#define open nx_open
#define close _close
#define unlink _unlink
#define rmdir _rmdir
#define access _access
static inline int nx_mkdir(const char *p, int mode) { (void)mode; return _mkdir(p); }
#define mkdir nx_mkdir
static inline int ftruncate(int fd, int64_t n) { errno_t e = _chsize_s(fd, n); if (e) { errno = e; return -1; } return 0; }
static inline int fsync(int fd) { return _commit(fd); }
static inline ssize_t nx_rw(int fd, void *buf, size_t n, int64_t off, int wr) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED o; memset(&o, 0, sizeof o);
    o.Offset = (DWORD)off; o.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD k = n > 0x40000000u ? 0x40000000u : (DWORD)n, got = 0;
    BOOL ok = wr ? WriteFile(h, buf, k, &got, &o) : ReadFile(h, buf, k, &got, &o);
    if (!ok) { DWORD e = GetLastError(); if (e == ERROR_HANDLE_EOF) return 0; errno = posix_errno_from_win(e); return -1; }
    return (ssize_t)got;
}
static inline ssize_t pread(int fd, void *buf, size_t n, int64_t off) { return nx_rw(fd, buf, n, off, 0); }
static inline ssize_t pwrite(int fd, const void *buf, size_t n, int64_t off) { return nx_rw(fd, (void *)buf, n, off, 1); }

/* ---- stat: the 64-bit forms ---- */
#define stat _stat64
#define fstat _fstat64
#define lstat _stat64

/* ---- directories ---- */
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
struct dirent { char d_name[MAX_PATH]; unsigned char d_type; };
typedef struct { HANDLE h; WIN32_FIND_DATAA fd; int first; struct dirent e; char path[MAX_PATH]; } DIR;
static inline DIR *opendir(const char *p) {
    DIR *d = (DIR *)calloc(1, sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    char pat[MAX_PATH + 4]; snprintf(pat, sizeof pat, "%s/*", p);
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { errno = posix_errno_from_win(GetLastError()); free(d); return NULL; }
    d->first = 1; strlcpy(d->path, p, sizeof d->path);
    return d;
}
static inline struct dirent *readdir(DIR *d) {
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = 0;
    strlcpy(d->e.d_name, d->fd.cFileName, sizeof d->e.d_name);
    d->e.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->e;
}
static inline int closedir(DIR *d) { if (d) { FindClose(d->h); free(d); } return 0; }

/* ---- extended attributes: NTFS alternate data streams ---- */
static inline ssize_t getxattr(const char *p, const char *name, void *buf, size_t n, uint32_t pos, int opt) {
    (void)pos; (void)opt;
    char s[MAX_PATH * 2]; snprintf(s, sizeof s, "%s:%s", p, name);
    HANDLE h = CreateFileA(s, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOATTR : posix_errno_from_win(e); return -1; }
    DWORD got = 0; BOOL ok = ReadFile(h, buf, (DWORD)n, &got, NULL); CloseHandle(h);
    if (!ok) { errno = EIO; return -1; }
    return (ssize_t)got;
}
static inline int setxattr(const char *p, const char *name, const void *v, size_t n, uint32_t pos, int opt) {
    (void)pos; (void)opt;
    char s[MAX_PATH * 2]; snprintf(s, sizeof s, "%s:%s", p, name);
    HANDLE h = CreateFileA(s, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = posix_errno_from_win(GetLastError()); return -1; }
    DWORD put = 0; BOOL ok = WriteFile(h, v, (DWORD)n, &put, NULL); CloseHandle(h);
    if (!ok || put != n) { errno = EIO; return -1; }
    return 0;
}
static inline int removexattr(const char *p, const char *name, int opt) {
    (void)opt;
    char s[MAX_PATH * 2]; snprintf(s, sizeof s, "%s:%s", p, name);
    if (DeleteFileA(s)) return 0;
    DWORD e = GetLastError(); errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOATTR : posix_errno_from_win(e);
    return -1;
}

/* ---- clonefile and the directory swap ---- */
static inline int clonefile(const char *from, const char *to, int flags) {
    (void)flags;
    DWORD a = GetFileAttributesA(from);
    if (a == INVALID_FILE_ATTRIBUTES) { errno = posix_errno_from_win(GetLastError()); return -1; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        if (CopyFileA(from, to, TRUE)) return 0;
        errno = posix_errno_from_win(GetLastError()); return -1;
    }
    if (!CreateDirectoryA(to, NULL)) { errno = posix_errno_from_win(GetLastError()); return -1; }
    DIR *d = opendir(from); if (!d) return -1;
    struct dirent *e; int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char f[MAX_PATH * 2], t[MAX_PATH * 2];
        snprintf(f, sizeof f, "%s/%s", from, e->d_name); snprintf(t, sizeof t, "%s/%s", to, e->d_name);
        rc = clonefile(f, t, 0);
    }
    closedir(d);
    return rc;
}
static inline int renamex_np(const char *a, const char *b, int flags) {
    if (flags != RENAME_SWAP) { errno = EINVAL; return -1; }
    char tmp[MAX_PATH * 2]; snprintf(tmp, sizeof tmp, "%s.swap", b);
    if (!MoveFileExA(b, tmp, 0)) { errno = posix_errno_from_win(GetLastError()); return -1; }
    if (!MoveFileExA(a, b, 0)) { int e = posix_errno_from_win(GetLastError()); MoveFileExA(tmp, b, 0); errno = e; return -1; }
    if (!MoveFileExA(tmp, a, 0)) { errno = posix_errno_from_win(GetLastError()); return -1; }
    return 0;
}

/* ---- strings ---- */
#define strtok_r strtok_s
static inline size_t strlcat(char *d, const char *s, size_t n) {
    size_t l = strnlen(d, n);
    if (l == n) return n + strlen(s);
    return l + strlcpy(d + l, s, n - l);
}

/* ---- rwlocks: SRWLOCK, which must know who releases an exclusive hold ---- */
typedef struct { SRWLOCK l; volatile DWORD writer; } pthread_rwlock_t;
static inline int pthread_rwlock_init(pthread_rwlock_t *r, const void *a) { (void)a; InitializeSRWLock(&r->l); r->writer = 0; return 0; }
static inline int pthread_rwlock_rdlock(pthread_rwlock_t *r) { AcquireSRWLockShared(&r->l); return 0; }
static inline int pthread_rwlock_wrlock(pthread_rwlock_t *r) { AcquireSRWLockExclusive(&r->l); r->writer = GetCurrentThreadId(); return 0; }
static inline int pthread_rwlock_unlock(pthread_rwlock_t *r) {
    if (r->writer == GetCurrentThreadId()) { r->writer = 0; ReleaseSRWLockExclusive(&r->l); } else ReleaseSRWLockShared(&r->l);
    return 0;
}

#endif
