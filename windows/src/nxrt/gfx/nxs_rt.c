// The run-time side of nxshader (nxs_rt.h).
#include "nxs_rt.h"
#include "dm.h"
#include <stdatomic.h>
#include <wchar.h>

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_on;
static char g_cache[1024], g_dump[1024], g_tool[2048], g_build[1024];
static nxs_counts g_n;
static plat_rwlock_t g_lk = PLAT_RWLOCK_INIT;

#define ufopen(p, m) ((FILE *)plat_fopen((p), (m)))
#define isdir plat_is_dir
#define isfile plat_is_file
#define mkdirs plat_mkdirs
static char *slurp(const char *path, size_t *n, int nul) {
    FILE *f = ufopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    if (b) { if (nul) b[len] = 0; *n = (size_t)len; }
    return b;
}

static void init_once(void) {
    const char *e = getenv("NXRT_SHADER_CACHE");
    char repo[1024] = "";
    {   // the executable lives in <repo>/build/<tree>/bin
        if (!plat_exe_path(repo, sizeof repo))
            for (int up = 0; up < 4; up++) { char *s = strrchr(repo, PLAT_SEP[0]); if (s) *s = 0; }
    }
    if (e && *e) snprintf(g_cache, sizeof g_cache, "%s", e);
    else if (*repo) snprintf(g_cache, sizeof g_cache, "%s" PLAT_SEP "_local" PLAT_SEP "nxshader" PLAT_SEP "cache", repo);
    const char *d = getenv("NXRT_SHADER_DUMP"); snprintf(g_dump, sizeof g_dump, "%s", d && *d ? d : "");
    if (!*g_dump) snprintf(g_dump, sizeof g_dump, "%s" PLAT_SEP "dumps", g_cache);
    const char *b = getenv("NXRT_SHADER_BUILD"); snprintf(g_build, sizeof g_build, "%s", b ? b : "");
    const char *t = getenv("NXRT_SHADER_TOOL");
    char tool_py[1100]; snprintf(tool_py, sizeof tool_py, "%s" PLAT_SEP "src" PLAT_SEP "nxshader" PLAT_SEP "nxshader.py", repo);
    if (t && *t) snprintf(g_tool, sizeof g_tool, "%s", strcmp(t, "off") ? t : "");
    else if (*repo && isfile(tool_py)) snprintf(g_tool, sizeof g_tool,
#ifdef _WIN32
        "py -3 -I \"%s\" --cache \"%s\"",
#else
        "python3 -I \"%s\" --cache \"%s\"",
#endif
        tool_py, g_cache);
    g_on = *g_cache && isdir(g_cache);
    if (g_on) LOG("[nxs] shader cache %s; dumps to %s; on-demand translation %s", g_cache, g_dump, *g_tool ? g_tool : "off (NXRT_SHADER_TOOL)");
}
static void init(void) { pthread_once(&g_once, init_once); }
int nxs_rt_enabled(void) { init(); return g_on; }
const char *nxs_rt_cache_dir(void) { init(); return g_cache; }

// ---------- the key table ----------
enum { E_UNKNOWN, E_FOUND, E_MISSING };
typedef struct ent { char key[17]; int state, requested, dumped, failed; uint64_t next_look, miss_t0; nxs_shader sh; struct ent *next; } ent;
enum { NB = 4096 };
static ent *g_tab[NB];
static ent *find(const char *key, int make) {   // g_lk held exclusively when make
    uint64_t h = 1469598103934665603ull; for (const char *k = key; *k; k++) h = (h ^ (uint8_t)*k) * 1099511628211ull;
    ent **e = &g_tab[h % NB];
    while (*e && strcmp((*e)->key, key)) e = &(*e)->next;
    if (!*e && make) { ent *n = calloc(1, sizeof *n); if (!n) die("shader table allocation failed"); snprintf(n->key, sizeof n->key, "%s", key); *e = n; }
    return *e;
}
static int chain_of(char *out, size_t n) {
    char p[1100]; snprintf(p, sizeof p, "%s" PLAT_SEP "vk" PLAT_SEP "CHAIN", g_cache);
    size_t len; char *c = slurp(p, &len, 1); if (!c) return 0;
    size_t k = 0; while (k + 1 < n && c[k] && c[k] != '\n' && c[k] != '\r') { out[k] = c[k]; k++; } out[k] = 0;
    free(c); return k > 0;
}
static int load(ent *e) {   // g_lk held: the cache's entry, when it exists and its chain is the cache's
    char base[1100], p[1200], chain[64]; snprintf(base, sizeof base, "%s" PLAT_SEP "vk" PLAT_SEP "%s", g_cache, e->key);
    snprintf(p, sizeof p, "%s.json", base); size_t jn, sn; char *j = slurp(p, &jn, 1);
    if (!j) return 0;
    snprintf(p, sizeof p, "%s.spv", base); char *s = slurp(p, &sn, 0);
    const char *c = strstr(j, "\"chain\": \"");
    int ok = s && sn >= 20 && !(sn & 3) && chain_of(chain, sizeof chain) && c && !strncmp(c + 10, chain, strlen(chain)) && c[10 + strlen(chain)] == '"';
    if (!ok) { if (s && j && c) g_n.stale++; free(j); free(s); return 0; }
    e->sh = (nxs_shader){{0}, (const uint8_t *)s, sn, j, jn}; snprintf(e->sh.key, sizeof e->sh.key, "%s", e->key);
    e->state = E_FOUND; g_n.keys++;
    return 1;
}

// ---------- on-demand translation: NXRT_SHADER_JOBS at a time (default 2, at most 4), at below-normal priority ----------
// M7: two at once halve a fresh build's first start, when the title screen and the first area ask for about a hundred run-time shaders at about 0.7 s each
typedef struct req { char key[17]; struct req *next; } req;
static req *g_q, **g_qt = &g_q; static pthread_mutex_t g_qlk = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_qcv = PTHREAD_COND_INITIALIZER;
static int g_qbusy, g_qthread;   // translations running, worker threads started
static unsigned qthread(void *a) {
    (void)a; plat_thread_nice(PLAT_PRIO_BELOW_NORMAL);
    for (;;) {
        pthread_mutex_lock(&g_qlk);
        while (!g_q) { pthread_cond_broadcast(&g_qcv); pthread_cond_wait(&g_qcv, &g_qlk); }
        req *r = g_q; g_q = r->next; if (!g_q) g_qt = &g_q; g_qbusy++;
        pthread_mutex_unlock(&g_qlk);
        char cmd[4096], logp[1200]; uint64_t t0 = now_ns();
        snprintf(cmd, sizeof cmd, "%s ondemand --dump \"%s\" --key %s%s%s%s", g_tool, g_dump, r->key, *g_build ? " --build \"" : "", g_build, *g_build ? "\"" : "");
        snprintf(logp, sizeof logp, "%s" PLAT_SEP "%s.log", g_dump, r->key);
        int code = plat_run_logged(cmd, logp);
        plat_rw_wrlock(&g_lk);
        ent *e = find(r->key, 1);
        int got = code == 0 && load(e);
        if (got) { g_n.arrived++; LOG("[nxs] shader %s translated on demand in %.1f s", r->key, (now_ns() - t0) / 1e9); }
        else { e->failed = 1; g_n.failed++; LOG("[RED] shader %s: on-demand translation failed (exit %d; %s)", r->key, code, logp); }
        plat_rw_wrunlock(&g_lk);
        free(r);
        pthread_mutex_lock(&g_qlk); g_qbusy--; pthread_cond_broadcast(&g_qcv); pthread_mutex_unlock(&g_qlk);
    }
    return 0;
}
static void request(const char *key) {
    req *r = calloc(1, sizeof *r); if (!r) return;
    snprintf(r->key, sizeof r->key, "%s", key);
    pthread_mutex_lock(&g_qlk);
    *g_qt = r; g_qt = &r->next;
    const char *j = getenv("NXRT_SHADER_JOBS"); int jobs = j && *j ? atoi(j) : 2; jobs = jobs < 1 ? 1 : jobs > 4 ? 4 : jobs;
    while (g_qthread < jobs) {
        g_qthread++; plat_thread_start(qthread, NULL, 0);
    }
    pthread_cond_broadcast(&g_qcv);
    pthread_mutex_unlock(&g_qlk);
}
void nxs_rt_wait_idle(uint64_t timeout_ns) {
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_qlk);
    while ((g_q || g_qbusy) && now_ns() - t0 < timeout_ns) plat_cond_wait_ms(&g_qcv, &g_qlk, (unsigned)(100));
    pthread_mutex_unlock(&g_qlk);
}

// The dump in DM_NVN_SHDUMP's format (nvn_mtl.m): <key>.ctl, the 0x880-byte control section (a Switch 2 block is its 0xe0 bytes, the rest zeros), <key>.code, the code area
static int dump(const char *key, const NVNshaderData *sd) {
    if (!sd->control) return 0;
    const uint8_t *c = sd->control; uint32_t ncode; memcpy(&ncode, c + 0x704, 4);
    int s2 = nvn_ctl_s2(c);
    if (s2) memcpy(&ncode, c + 0x44, 4);
    if (!sd->data || !ncode || ncode >= (1u << 24)) return 0;
    uint8_t ctl[0x880] = {0}; memcpy(ctl, c, s2 ? 0xe0 : 0x880);
    mkdirs(g_dump);
    char p[1200]; FILE *f;
    snprintf(p, sizeof p, "%s" PLAT_SEP "%s.ctl", g_dump, key); if (!(f = ufopen(p, "wb"))) return 0; fwrite(ctl, 1, sizeof ctl, f); fclose(f);
    snprintf(p, sizeof p, "%s" PLAT_SEP "%s.code", g_dump, key); if (!(f = ufopen(p, "wb"))) return 0; fwrite((const void *)(uintptr_t)sd->data, 1, ncode, f); fclose(f);
    return 1;
}
static const nxs_shader *get(const char *key, const NVNshaderData *sd) {
    init();
    if (!g_on) return NULL;
    plat_rw_wrlock(&g_lk);
    g_n.lookups++;
    ent *e = find(key, 1);
    if (e->state == E_FOUND) { g_n.found++; plat_rw_wrunlock(&g_lk); return &e->sh; }
    uint64_t now = now_ns();
    if (e->state == E_UNKNOWN || now >= e->next_look) {   // a missing entry is looked for again twice a second (on-demand results, a build run meanwhile)
        if (load(e)) { if (e->state == E_FOUND && e->miss_t0) LOG("[nxs] shader %s is in the cache now (%.1f s after it was missed)", key, (now - e->miss_t0) / 1e9); g_n.found++; plat_rw_wrunlock(&g_lk); return &e->sh; }
        e->next_look = now + 500000000ull;
    }
    int first = e->state == E_UNKNOWN;
    if (first) { e->state = E_MISSING; e->miss_t0 = now; g_n.missing_keys++; }
    int dumped = 0;
    if (!e->dumped && sd && dump(key, sd)) { dumped = e->dumped = 1; g_n.dumped++; if (*g_tool) { e->requested = 1; g_n.requested++; } }
    plat_rw_wrunlock(&g_lk);
    if (first || dumped) LOG("[RED] shader %s is not in the shader cache (%s)%s", key, g_cache,
                             dumped ? (*g_tool ? ": dumped, on-demand translation requested" : ": dumped (no on-demand translation: NXRT_SHADER_TOOL)") : sd ? ": could not be dumped" : "");
    if (dumped && *g_tool) request(key);
    return NULL;
}
const nxs_shader *nxs_get(const char *key) { return get(key, NULL); }
// M6 phase B: a quiet lookup for the pipeline prewarm (vk_pipe.c), whose lists may name another game version's shaders: the entry when the cache has it, else NULL, nothing
// logged and nothing marked missing (a real miss later is still logged, dumped and requested)
const nxs_shader *nxs_peek(const char *key) {
    init();
    if (!g_on || !key || strlen(key) != 16) return NULL;
    plat_rw_wrlock(&g_lk);
    ent *e = find(key, 1); const nxs_shader *r = NULL;
    if (e->state == E_FOUND || (e->state == E_UNKNOWN && load(e))) r = &e->sh;
    plat_rw_wrunlock(&g_lk);
    return r;
}
int nxs_resolve(const prog_t *p, unsigned i, const nxs_shader **out) {
    uint8_t k[8]; char key[17];
    if (!prog_key(p, i, k)) return -1;
    for (int j = 0; j < 8; j++) snprintf(key + 2 * j, 3, "%02x", k[j]);
    *out = get(key, &p->sd[i]);
    if (!*out && prog_rekey(p, i, k)) {   // the code reached the pool after the first hash (GPU copies written back by the replay)
        char old[17]; memcpy(old, key, 17);
        for (int j = 0; j < 8; j++) snprintf(key + 2 * j, 3, "%02x", k[j]);
        *out = get(key, &p->sd[i]);
        static int nlog; if (nlog++ < 64) LOG("[nxs] program %p stage %u keyed again: %s -> %s (%s)", (const void *)p, i, old, key, *out ? "in the cache" : "still missing");
    }
    return *out != NULL;
}
void nxs_counts_get(nxs_counts *c) { plat_rw_rdlock(&g_lk); *c = g_n; plat_rw_rdunlock(&g_lk); }
