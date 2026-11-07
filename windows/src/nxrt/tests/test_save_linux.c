// The Linux save commit killed at every step (DESIGN D13): after a commit dies anywhere and a start recovers, the save is entirely the old version or entirely the new one.
// test_save_linux <scratch folder>: run it on each kind of file system that matters (one that can swap directories, one that cannot).
#include "save_txn.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void put(const char *root, const char *rel, const char *text) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", root, rel);
    mkdir(root, 0777);
    for (char *s = strchr(p + strlen(root) + 1, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(p, 0777); *s = '/'; }
    FILE *f = fopen(p, "w"); if (!f) { perror(p); exit(2); } fputs(text, f); fclose(f);
}
static int same(const char *root, const char *rel, const char *text) {
    char p[PATH_MAX], b[64] = ""; snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return !strcmp(b, text);
}
static const char *version(const char *save) {   // both files agree, or it is a mix
    if (same(save, "S00/a.bin", "old a") && same(save, "G00/b.bin", "old b")) return "old";
    if (same(save, "S00/a.bin", "new a") && same(save, "G00/b.bin", "new b")) return "new";
    return "MIX";
}
int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "save-test";
    static const char *points[] = {"", "copied:1", "copied:2", "flushed", "marker", "renamed_old", "renamed_new", "unmarked", "done"};
    char save[PATH_MAX], work[PATH_MAX]; snprintf(save, sizeof save, "%s/save", base); snprintf(work, sizeof work, "%s/work", base);
    int bad = 0;
    for (int first = 0; first < 2; first++) for (size_t i = 0; i < sizeof points / sizeof *points; i++) {
        save_txn_rm_tree(base); mkdir(base, 0777);
        if (!first) { put(save, "S00/a.bin", "old a"); put(save, "G00/b.bin", "old b"); }
        put(work, "S00/a.bin", "new a"); put(work, "G00/b.bin", "new b");
        pid_t c = fork();
        if (!c) { if (*points[i]) setenv("NXRT_SAVE_KILL_AT", points[i], 1); _exit(save_txn_commit(work, save) ? 3 : 0); }
        int st = 0; waitpid(c, &st, 0);
        int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (code != 0 && code != 77) { printf("FAIL commit at %s exit %d (%s)\n", points[i], code, strerror(errno)); bad++; continue; }
        int rc = save_txn_recover(save);
        struct stat s; const char *v = !stat(save, &s) ? version(save) : "absent";
        int ok = rc >= 0 && strcmp(v, "MIX") && (first ? (!strcmp(v, "new") || !strcmp(v, "absent")) : (!strcmp(v, "new") || !strcmp(v, "old")));
        char left[PATH_MAX]; const char *sfx[] = {".new", ".old", ".commit"};
        for (int k = 0; k < 3; k++) { snprintf(left, sizeof left, "%s%s", save, sfx[k]); if (!lstat(left, &s)) { ok = 0; printf("  left behind: %s\n", left); } }
        if (code == 0 && !*points[i] && strcmp(v, "new")) ok = 0;   // a commit that returned is the new version
        printf("%s %s kill at %-12s exit %2d recover %d -> %s\n", ok ? "ok  " : "FAIL", first ? "first save" : "over a save", *points[i] ? points[i] : "(none)", code, rc, v);
        bad += !ok;
    }
    save_txn_rm_tree(base);
    printf(bad ? "%d failures\n" : "all good\n", bad);
    return bad ? 1 : 0;
}
