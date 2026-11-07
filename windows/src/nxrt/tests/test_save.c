/* The save commit on NTFS (hle/save_txn.c, DESIGN D13), killed at every step. A save in the game's own layout (B00, G00, S00) holds version A; the work
   directory holds version B (a file changed in each folder, one deleted, one added, a guest name in an alternate data stream). For every kill point of
   save_txn_commit, and after every file of its copy, a child process commits B over A and terminates itself there; then the start-up recovery runs, and the
   save must be exactly A or exactly B, never a mix, with nothing left beside it. Before the commit marker is written the save stays A; from the marker on the
   commit is finished and it is B. Then a recovered save takes the next commit, and recovery is idempotent. No game data. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nxrt/runtime.h"
#include "save_txn.h"

NXRT_EARLY_RESERVE()

static int g_fail;
#define T(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static void put(const char *root, const char *rel, char tag, size_t n) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", root, rel);
    char d[MAX_PATH]; strcpy(d, p); *strrchr(d, '\\') = 0; CreateDirectoryA(d, NULL);
    FILE *f = fopen(p, "wb"); if (!f) { printf("cannot write %s\n", p); exit(2); }
    for (size_t i = 0; i < n; i++) fputc((int)(unsigned char)(tag + i * 7), f);
    fclose(f);
}
static void stream(const char *root, const char *rel, const char *text) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s:org.d2rmobile.guest-name", root, rel);
    FILE *f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); }
}
static void version_a(const char *root) {
    CreateDirectoryA(root, NULL);
    put(root, "B00\\BCKDAT.BIN", 'a', 1024); put(root, "G00\\CMNDAT.BIN", 'b', 65536); put(root, "G00\\STGDAT01.BIN", 'c', 131072); put(root, "S00\\SYDAT.BIN", 'd', 4096);
}
static void version_b(const char *root) {
    CreateDirectoryA(root, NULL);
    put(root, "B00\\BCKDAT.BIN", 'A', 1024); put(root, "G00\\CMNDAT.BIN", 'B', 65536); put(root, "G00\\SCSHDAT.BIN", 'E', 32768); put(root, "S00\\SYDAT.BIN", 'D', 4096);
    stream(root, "G00\\CMNDAT.BIN", "CmnDat.bin");
}
/* A tree as text: "relative path:size:checksum" per file, sorted by the directory walk (NTFS lists names in order) */
static void digest(const char *root, const char *rel, char *out, size_t n) {
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s%s%s\\*", root, *rel ? "\\" : "", rel);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char r[MAX_PATH]; snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "\\" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { digest(root, r, out, n); continue; }
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", root, r);
        FILE *f = fopen(p, "rb"); unsigned long long sum = 1469598103934665603ull, size = 0; int c;
        while (f && (c = fgetc(f)) != EOF) { sum = (sum ^ (unsigned)c) * 1099511628211ull; size++; }
        if (f) fclose(f);
        size_t l = strlen(out); snprintf(out + l, n - l, "%s:%llu:%llx;", r, size, sum);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static int run_child(const char *self, const char *work, const char *save, const char *kill) {
    char cmd[MAX_PATH * 4]; snprintf(cmd, sizeof cmd, "\"%s\" child \"%s\" \"%s\"", self, work, save);
    SetEnvironmentVariableA("NXRT_SAVE_KILL_AT", kill);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si; PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("cannot start the child (%lu)\n", GetLastError()); exit(2); }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    SetEnvironmentVariableA("NXRT_SAVE_KILL_AT", NULL);
    return (int)code;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "child")) return save_txn_commit(argv[2], argv[3]) ? 1 : 0;
    char tmp[MAX_PATH], root[MAX_PATH], save[MAX_PATH], work[MAX_PATH], self[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp); snprintf(root, sizeof root, "%snxrt-save-test-%lu", tmp, GetCurrentProcessId());
    GetModuleFileNameA(NULL, self, sizeof self);
    CreateDirectoryA(root, NULL);
    snprintf(save, sizeof save, "%s\\save", root); snprintf(work, sizeof work, "%s\\save.work", root);
    char ref_a[8192] = "", ref_b[8192] = "", got[8192];
    { char a[MAX_PATH], b[MAX_PATH]; snprintf(a, sizeof a, "%s\\ref_a", root); snprintf(b, sizeof b, "%s\\ref_b", root);
      version_a(a); version_b(b); digest(a, "", ref_a, sizeof ref_a); digest(b, "", ref_b, sizeof ref_b); save_txn_rm_tree(a); save_txn_rm_tree(b); }
    T(strcmp(ref_a, ref_b) != 0);
    version_b(work);
    char points[16][24]; int np = 0;
    for (int k = 1; k <= 4; k++) snprintf(points[np++], sizeof points[0], "copied:%d", k);   /* version B has four files */
    const char *rest[] = {"flushed", "marker", "renamed_old", "renamed_new", "unmarked", "done", ""};
    for (int k = 0; k < 7; k++) snprintf(points[np++], sizeof points[0], "%s", rest[k]);
    int after_marker = 0, as_a = 0, as_b = 0;
    for (int k = 0; k < np; k++) {
        char neu[MAX_PATH], old[MAX_PATH], mark[MAX_PATH];
        snprintf(neu, sizeof neu, "%s.new", save); snprintf(old, sizeof old, "%s.old", save); snprintf(mark, sizeof mark, "%s.commit", save);
        save_txn_rm_tree(save); save_txn_rm_tree(neu); save_txn_rm_tree(old); DeleteFileA(mark);
        version_a(save);
        if (!strcmp(points[k], "marker")) after_marker = 1;
        int code = run_child(self, work, save, points[k]);
        int killed = code == 9;
        T(killed == (points[k][0] != 0));   /* every point is reached; the last run has no kill */
        int r = save_txn_recover(save);
        got[0] = 0; digest(save, "", got, sizeof got);
        int is_a = !strcmp(got, ref_a), is_b = !strcmp(got, ref_b);
        printf("%-12s child exit %d, recovery %d: the save is %s\n", points[k][0] ? points[k] : "(no kill)", code, r, is_a ? "version A (old)" : is_b ? "version B (new)" : "A MIX");
        T(is_a || is_b);
        T(is_b == (after_marker || !points[k][0]));   /* before the marker the old version, from the marker on the new one */
        T(!exists(neu) && !exists(old) && !exists(mark));
        T(save_txn_recover(save) == 0);               /* idempotent */
        as_a += is_a; as_b += is_b;
        /* the recovered save takes the next commit */
        if (is_a) { T(save_txn_commit(work, save) == 0); got[0] = 0; digest(save, "", got, sizeof got); T(!strcmp(got, ref_b)); }
    }
    { char p[MAX_PATH], s[64] = ""; snprintf(p, sizeof p, "%s\\G00\\CMNDAT.BIN:org.d2rmobile.guest-name", save);
      FILE *f = fopen(p, "rb"); if (f) { fgets(s, sizeof s, f); fclose(f); }
      T(!strcmp(s, "CmnDat.bin")); }   /* the guest name stream came along */
    save_txn_rm_tree(root);
    printf("%d kill points: %d left the old version, %d the new one, none a mix\n", np, as_a, as_b);
    printf("nxrt.save: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail;
}
