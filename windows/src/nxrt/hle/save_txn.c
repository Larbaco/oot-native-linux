// The save commit on NTFS (DESIGN D13); the protocol is in save_txn.h. Written for nxrt (no nxnative counterpart: nxnative swaps directories with APFS's RENAME_SWAP).
#include "save_txn.h"
#include "dm.h"
#ifdef _WIN32
#include <windows.h>
#endif

// ---------- Kill points (tests only) ----------
static void kill_point(const char *name) {
    static char want[32]; static long nth = -1, seen;
    if (nth < 0) {
        const char *e = getenv("NXRT_SAVE_KILL_AT"); nth = 0;
        if (e && *e) { const char *c = strchr(e, ':'); size_t n = c ? (size_t)(c - e) : strlen(e); if (n >= sizeof want) n = sizeof want - 1;
                       memcpy(want, e, n); want[n] = 0; nth = c ? strtol(c + 1, NULL, 10) : 1; if (nth < 1) nth = 1; }
    }
    if (nth > 0 && !strcmp(want, name) && ++seen == nth) TerminateProcess(GetCurrentProcess(), 9);
}

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static int fail(void) { errno = EIO; DWORD e = GetLastError(); if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) errno = ENOSPC; else if (e == ERROR_ACCESS_DENIED) errno = EACCES; return -1; }
static int flush_file(const char *p) {
    HANDLE h = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail();
    BOOL ok = FlushFileBuffers(h); CloseHandle(h);
    return ok ? 0 : fail();
}
int save_txn_copy_tree(const char *from, const char *to) {
    if (!CreateDirectoryA(to, NULL)) return fail();
    char pat[MAX_PATH * 2]; snprintf(pat, sizeof pat, "%s\\*", from);
    WIN32_FIND_DATAA fd; HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : fail();
    int rc = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char a[MAX_PATH * 2], b[MAX_PATH * 2];
        snprintf(a, sizeof a, "%s\\%s", from, fd.cFileName); snprintf(b, sizeof b, "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rc = save_txn_copy_tree(a, b);
        else if (!CopyFileA(a, b, TRUE)) rc = fail();   // CopyFile keeps the alternate data streams: the guest names (hle_fs.c)
        else rc = flush_file(b);
        if (!rc && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) kill_point("copied");
    } while (!rc && FindNextFileA(f, &fd));
    FindClose(f);
    return rc;
}
int save_txn_rm_tree(const char *p) {
    DWORD at = GetFileAttributesA(p);
    if (at == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(at & FILE_ATTRIBUTE_DIRECTORY)) { SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL); return DeleteFileA(p) ? 0 : fail(); }
    char pat[MAX_PATH * 2]; snprintf(pat, sizeof pat, "%s\\*", p);
    WIN32_FIND_DATAA fd; HANDLE f = FindFirstFileA(pat, &fd); int rc = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char c[MAX_PATH * 2]; snprintf(c, sizeof c, "%s\\%s", p, fd.cFileName);
            if (save_txn_rm_tree(c)) rc = -1;
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    if (!RemoveDirectoryA(p)) rc = fail();
    return rc;
}
static int write_marker(const char *marker, const char *save) {
    HANDLE h = CreateFileA(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail();
    char text[MAX_PATH * 2 + 64]; int n = snprintf(text, sizeof text, "nxrt save commit 1\r\n%s.new is complete: it replaces %s\r\n", save, save);
    DWORD w = 0; BOOL ok = WriteFile(h, text, (DWORD)n, &w, NULL) && w == (DWORD)n && FlushFileBuffers(h);
    CloseHandle(h);
    return ok ? 0 : fail();
}
static void names(const char *save, char *neu, char *old, char *marker, size_t n) {
    snprintf(neu, n, "%s.new", save); snprintf(old, n, "%s.old", save); snprintf(marker, n, "%s.commit", save);
}

int save_txn_recover(const char *save) {
    char neu[MAX_PATH * 2], old[MAX_PATH * 2], marker[MAX_PATH * 2]; names(save, neu, old, marker, sizeof neu);
    if (exists(marker)) {   // .new was complete: finish the commit from whichever step it stopped at
        if (exists(neu)) {
            if (exists(save)) {
                if (exists(old) && save_txn_rm_tree(old)) return -1;
                if (!MoveFileExA(save, old, MOVEFILE_WRITE_THROUGH)) return fail();
            }
            if (!MoveFileExA(neu, save, MOVEFILE_WRITE_THROUGH)) return fail();
        } else if (!exists(save)) { errno = ENOENT; LOG("[RED] save transaction: the commit marker %s names a version that is gone (no %s, no %s)", marker, neu, save); return -1; }
        if (!DeleteFileA(marker)) return fail();
        save_txn_rm_tree(old);
        LOG("save transaction: an interrupted commit of %s was finished at start (the new version is the save)", save);
        return 1;
    }
    int r = 0;
    if (exists(neu)) { if (save_txn_rm_tree(neu)) return -1; r = 2; LOG("save transaction: %s, a copy for a commit that never completed, was deleted (the save is the version before it)", neu); }
    if (!exists(save) && exists(old)) { if (!MoveFileExA(old, save, MOVEFILE_WRITE_THROUGH)) return fail(); r = 2; LOG("save transaction: %s was put back as the save", old); }
    else if (exists(old)) save_txn_rm_tree(old);   // the replaced version of a finished commit, whose deletion was cut short
    return r;
}

int save_txn_commit(const char *work, const char *save) {
    char neu[MAX_PATH * 2], old[MAX_PATH * 2], marker[MAX_PATH * 2]; names(save, neu, old, marker, sizeof neu);
    if (save_txn_recover(save) < 0) return -1;   // an earlier commit cut short is finished (or undone) first
    if (save_txn_copy_tree(work, neu)) { int e = errno; save_txn_rm_tree(neu); errno = e; return -1; }
    kill_point("flushed");
    if (write_marker(marker, save)) { int e = errno; DeleteFileA(marker); save_txn_rm_tree(neu); errno = e; return -1; }
    kill_point("marker");
    // From here the commit goes forward: a failure leaves the marker, and the next commit or start finishes it
    if (exists(save) && !MoveFileExA(save, old, MOVEFILE_WRITE_THROUGH)) return fail();
    kill_point("renamed_old");
    if (!MoveFileExA(neu, save, MOVEFILE_WRITE_THROUGH)) return fail();
    kill_point("renamed_new");
    if (!DeleteFileA(marker)) return fail();
    kill_point("unmarked");
    save_txn_rm_tree(old);
    kill_point("done");
    return 0;
}
