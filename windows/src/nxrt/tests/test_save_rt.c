/* The save round trip through nn::fs as the game drives it (hle/hle_fs.c on hle/save_txn.c, DESIGN D13), on a save in the game's own layout (B00 / G00 / S00):
   mount, change a file in place, create one, read back through the mount; before Commit the committed folder is untouched, after it the folder holds exactly the
   work directory's files (plain files with the names the game gave, nothing beside the save), the unchanged files byte for byte; a second transaction commits
   too. The folder is what another host (the Mac's dmhost) mounts as its save: no container, no transform. No game data. */
#include "guest_test.h"
#include "hle_adapters.h"

NXRT_EARLY_RESERVE()

static NxwGuestCpu g_a;
#define CALL(fn, ...) call_(adapter_##fn, (uint64_t[]){0, ##__VA_ARGS__} + 1, (int)(sizeof((uint64_t[]){0, ##__VA_ARGS__}) / 8) - 1)
static uint64_t call_(void (*fn)(NxwGuestCpu *), const uint64_t *a, int n) {
    g_a = *nxrt_cur_cpu();
    for (int i = 0; i < n; i++) g_a.x[i] = a[i];
    fn(&g_a);
    return g_a.x[0];
}
#define P(p) ((uint64_t)(uintptr_t)(p))
static char *gs(const char *s) { char *p = gt_alloc(strlen(s) + 1); strcpy(p, s); return p; }   /* a string in guest memory */

static void put(const char *root, const char *rel, char tag, size_t n) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", root, rel);
    char d[MAX_PATH]; strcpy(d, p); *strrchr(d, '\\') = 0; CreateDirectoryA(d, NULL);
    FILE *f = fopen(p, "wb"); for (size_t i = 0; i < n; i++) fputc((int)(unsigned char)(tag + i * 13), f); fclose(f);
}
static size_t slurp(const char *root, const char *rel, uint8_t *buf, size_t cap) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", root, rel);
    FILE *f = fopen(p, "rb"); if (!f) return (size_t)-1;
    size_t n = fread(buf, 1, cap, f); fclose(f); return n;
}
static int same_file(const char *a, const char *b, const char *rel) {
    static uint8_t x[1 << 20], y[1 << 20];
    size_t n = slurp(a, rel, x, sizeof x), m = slurp(b, rel, y, sizeof y);
    return n != (size_t)-1 && n == m && !memcmp(x, y, n);
}
static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void rm_tree(const char *p) {
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", p); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char c[MAX_PATH]; snprintf(c, sizeof c, "%s\\%s", p, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(c); else DeleteFileA(c); } while (FindNextFileA(h, &fd)); FindClose(h); }
    RemoveDirectoryA(p);
}
static int count_files(const char *p) {
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", p); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); int n = 0;
    if (h != INVALID_HANDLE_VALUE) { do { if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char c[MAX_PATH]; snprintf(c, sizeof c, "%s\\%s", p, fd.cFileName);
        n += fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? count_files(c) : 1; } while (FindNextFileA(h, &fd)); FindClose(h); }
    return n;
}

int main(void) {
    char root[MAX_PATH], save[MAX_PATH], ref[MAX_PATH], work[MAX_PATH];
    GetTempPathA(MAX_PATH, root); strcat(root, "nxrt-test-save-rt");
    snprintf(save, sizeof save, "%s\\save", root); snprintf(ref, sizeof ref, "%s\\ref", root); snprintf(work, sizeof work, "%s\\save.work", root);
    rm_tree(root); CreateDirectoryA(root, NULL); CreateDirectoryA(save, NULL); CreateDirectoryA(ref, NULL);
    const char *files[] = {"B00\\CMNDAT.BIN", "B00\\STGDAT01.BIN", "G00\\CMNDAT.BIN", "G00\\SCSHDAT.BIN", "G00\\STGDAT01.BIN", "S00\\SYDAT.BIN"};
    for (int i = 0; i < 6; i++) { put(save, files[i], (char)('a' + i), 4096u << i); put(ref, files[i], (char)('a' + i), 4096u << i); }
    _putenv_s("DM_SAVE", save);
    gt_init();

    T(CALL(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE, P(gs("save")), P(gt_alloc(16))) == 0);
    T(exists(work));
    /* the game's write: G00/CMNDAT.BIN changed in place (open for write, 1 KB at 512) */
    uint64_t *h = gt_alloc(8); uint8_t *buf = gt_alloc(65536); int64_t *wopt = gt_alloc(8); uint64_t *got = gt_alloc(8);
    T(CALL(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, P(h), P(gs("save:/G00/CMNDAT.BIN")), 3) == 0);
    for (int i = 0; i < 1024; i++) buf[i] = (uint8_t)(i * 7 + 1);
    *wopt = 1;   /* WriteOption: flush */
    T(CALL(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE, *h, 512, P(buf), 1024, P(wopt)) == 0);
    T(CALL(_ZN2nn2fs9FlushFileENS0_10FileHandleE, *h) == 0);
    CALL(_ZN2nn2fs9CloseFileENS0_10FileHandleE, *h);
    /* a new file */
    T(CALL(_ZN2nn2fs10CreateFileEPKcl, P(gs("save:/G00/NEWDAT.BIN")), 3000) == 0);
    T(CALL(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, P(h), P(gs("save:/G00/NEWDAT.BIN")), 2) == 0);
    for (int i = 0; i < 3000; i++) buf[i] = (uint8_t)(255 - i);
    T(CALL(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE, *h, 0, P(buf), 3000, P(wopt)) == 0);
    CALL(_ZN2nn2fs9CloseFileENS0_10FileHandleE, *h);
    /* before Commit: the committed folder is the old version */
    for (int i = 0; i < 6; i++) T(same_file(save, ref, files[i]));
    T(count_files(save) == 6);
    /* read back through the mount */
    T(CALL(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci, P(h), P(gs("save:/G00/CMNDAT.BIN")), 1) == 0);
    memset(buf, 0, 2048);
    T(CALL(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm, P(got), *h, 512, P(buf), 1024) == 0 && *got == 1024);
    int ok = 1; for (int i = 0; i < 1024; i++) ok &= buf[i] == (uint8_t)(i * 7 + 1);
    T(ok);
    CALL(_ZN2nn2fs9CloseFileENS0_10FileHandleE, *h);
    /* Commit */
    T(CALL(_ZN2nn2fs6CommitEPKc, P(gs("save"))) == 0);
    char side[MAX_PATH];
    snprintf(side, sizeof side, "%s.new", save); T(!exists(side)); snprintf(side, sizeof side, "%s.old", save); T(!exists(side)); snprintf(side, sizeof side, "%s.commit", save); T(!exists(side));
    T(count_files(save) == 7);
    for (int i = 0; i < 6; i++) if (i != 2) T(same_file(save, ref, files[i]));   /* untouched files byte for byte */
    T(same_file(save, work, "G00\\CMNDAT.BIN") && same_file(save, work, "G00\\NEWDAT.BIN"));
    static uint8_t on_disk[1 << 20]; size_t n = slurp(save, "G00\\CMNDAT.BIN", on_disk, sizeof on_disk);
    ok = n == 16384; for (int i = 0; ok && i < 1024; i++) ok &= on_disk[512 + i] == (uint8_t)(i * 7 + 1);
    T(ok && on_disk[0] == (uint8_t)'c' && on_disk[511] == (uint8_t)('c' + 511 * 13));   /* in place: the bytes around the write kept */
    n = slurp(save, "G00\\NEWDAT.BIN", on_disk, sizeof on_disk); T(n == 3000 && on_disk[0] == 255 && on_disk[2999] == (uint8_t)(255 - 2999));
    /* a second transaction: delete the new file, commit */
    T(CALL(_ZN2nn2fs10DeleteFileEPKc, P(gs("save:/G00/NEWDAT.BIN"))) == 0);
    T(CALL(_ZN2nn2fs6CommitEPKc, P(gs("save"))) == 0);
    T(count_files(save) == 6);
    snprintf(side, sizeof side, "%s\\G00\\NEWDAT.BIN", save); T(!exists(side));
    printf("save round trip: written through the mount, read back, the committed folder unchanged until Commit, then exactly the work directory (7 files, then 6)\n");
    rm_tree(root);
    printf("nxrt.save_rt: %s\n", gt_fail ? "FAIL" : "ok");
    return gt_fail;
}
