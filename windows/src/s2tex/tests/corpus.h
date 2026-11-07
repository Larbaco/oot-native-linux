/* Reading the real-data corpus (tools/collect.py writes it): manifest.tsv, and per texture <id>.bl (guest storage, all levels) and
 * <id>.raw (the host's decoded level 0 from DM_NVN_TEXRAW: a text line "width height depth metal-format bytes-per-row", then each depth
 * slice of height rows at that pitch; a block-compressed level fills the first height / block-height rows). Shared by the C test and the
 * Metal harness. MIT licence, part of pokopia-native. */
#ifndef S2TEX_CORPUS_H
#define S2TEX_CORPUS_H

#include "s2tex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char id[96], stage[32];
    unsigned ser, target, width, height, depth, format, levels, flags, bh, bd, cpu_written;
    unsigned long long size;   /* the storage size the host lists (all levels) */
} corpus_entry;

typedef struct {
    s2tex_surface surf;            /* level 0, Switch 2 order */
    unsigned char *bl, *raw_file;  /* storage; the whole .raw file */
    const unsigned char *raw;      /* its data, after the header line */
    size_t bl_size, raw_size, row_pitch, slice_pitch;
    unsigned long long data_bytes; /* texel bytes compared: width x bytes per block x rows x slices */
} corpus_pair;

/* NVN format -> bytes per block and block footprint (NVNformat enumeration, nvn.h) */
static int corpus_format(unsigned f, unsigned *bpb, unsigned *bw, unsigned *bh) {
    static const struct { unsigned char lo, hi, bpb; } r[] = {
        {0x01, 0x04, 1}, {0x05, 0x09, 2}, {0x0A, 0x0C, 4}, {0x0D, 0x10, 2}, {0x11, 0x15, 4}, {0x16, 0x18, 8}, {0x19, 0x1C, 3},
        {0x1D, 0x21, 6}, {0x22, 0x24, 12}, {0x25, 0x28, 4}, {0x29, 0x2D, 8}, {0x2E, 0x30, 16}, {0x31, 0x31, 1}, {0x32, 0x32, 2},
        {0x33, 0x35, 4}, {0x36, 0x36, 8}, {0x37, 0x38, 4}, {0x39, 0x3C, 2}, {0x3D, 0x40, 4}, {0x51, 0x52, 1}, {0x53, 0x54, 2},
        {0x55, 0x56, 4}, {0x57, 0x58, 2}, {0x59, 0x5A, 4}, {0x5B, 0x5C, 8}, {0x5D, 0x5E, 3}, {0x5F, 0x60, 6}, {0x61, 0x62, 12},
        {0x63, 0x64, 4}, {0x65, 0x66, 8}, {0x67, 0x68, 16}, {0x69, 0x6C, 4}, {0x6D, 0x70, 4}, {0x71, 0x75, 8}, {0x76, 0x78, 16},
        {0x95, 0x98, 2}, {0x99, 0x9C, 4},
    };
    static const unsigned char astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    *bw = *bh = 1;
    if (f >= 0x41 && f <= 0x50) {   /* DXT1-5, RGTC1/2, BPTC: 4x4 blocks; DXT1 and RGTC1 are 8 bytes */
        *bw = *bh = 4;
        *bpb = (f <= 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4A) ? 8 : 16;
        return 1;
    }
    if (f >= 0x79 && f <= 0x94) { unsigned k = (f - 0x79) % 14; *bw = astc[k][0]; *bh = astc[k][1]; *bpb = 16; return 1; }
    for (size_t i = 0; i < sizeof r / sizeof *r; i++)
        if (f >= r[i].lo && f <= r[i].hi) { *bpb = r[i].bpb; return 1; }
    return 0;
}

static unsigned char *corpus_read(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(n > 0 ? (size_t)n : 1);
    if (p && n > 0 && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    if (p) *size = (size_t)(n > 0 ? n : 0);
    return p;
}

/* The manifest's rows; returns their count, or -1 without a manifest. Columns are found by name. */
static int corpus_load(const char *dir, corpus_entry **out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/manifest.tsv", dir);
    size_t size;
    unsigned char *txt = corpus_read(path, &size);
    if (!txt) return -1;
    char *s = realloc(txt, size + 1);
    if (!s) { free(txt); return -1; }
    s[size] = 0;
    enum { C_ID, C_STAGE, C_SER, C_TARGET, C_W, C_H, C_D, C_FMT, C_LEVELS, C_FLAGS, C_BH, C_BD, C_CPU, C_SIZE, NC };
    static const char *names[NC] = {"id", "stage", "ser", "target", "width", "height", "depth", "format", "levels", "flags", "bh", "bd", "cpu_written", "size"};
    int col[NC];
    for (int i = 0; i < NC; i++) col[i] = -1;
    int cap = 64, n = 0, line = 0;
    corpus_entry *e = malloc(cap * sizeof *e);
    for (char *ln = s, *next; ln && *ln; ln = next, line++) {
        next = strchr(ln, '\n');
        if (next) *next++ = 0;
        size_t L = strlen(ln);
        if (L && ln[L - 1] == '\r') ln[--L] = 0;
        if (!L) continue;
        char *f[32]; int nf = 0;
        for (char *p = ln; nf < 32; ) { f[nf++] = p; p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        if (!line) { for (int c = 0; c < nf; c++) for (int k = 0; k < NC; k++) if (!strcmp(f[c], names[k])) col[k] = c; continue; }
        if (n == cap) { cap *= 2; e = realloc(e, cap * sizeof *e); }
        corpus_entry *x = &e[n];
        memset(x, 0, sizeof *x);
        #define FIELD(k) (col[k] >= 0 && col[k] < nf ? f[col[k]] : "0")
        snprintf(x->id, sizeof x->id, "%s", FIELD(C_ID));
        snprintf(x->stage, sizeof x->stage, "%s", FIELD(C_STAGE));
        x->ser = (unsigned)strtoul(FIELD(C_SER), NULL, 0);
        x->target = (unsigned)strtoul(FIELD(C_TARGET), NULL, 0);
        x->width = (unsigned)strtoul(FIELD(C_W), NULL, 0);
        x->height = (unsigned)strtoul(FIELD(C_H), NULL, 0);
        x->depth = (unsigned)strtoul(FIELD(C_D), NULL, 0);
        x->format = (unsigned)strtoul(FIELD(C_FMT), NULL, 0);
        x->levels = (unsigned)strtoul(FIELD(C_LEVELS), NULL, 0);
        x->flags = (unsigned)strtoul(FIELD(C_FLAGS), NULL, 0);
        x->bh = (unsigned)strtoul(FIELD(C_BH), NULL, 0);
        x->bd = (unsigned)strtoul(FIELD(C_BD), NULL, 0);
        x->cpu_written = (unsigned)strtoul(FIELD(C_CPU), NULL, 0);
        x->size = strtoull(FIELD(C_SIZE), NULL, 0);
        #undef FIELD
        n++;
    }
    free(s);
    *out = e;
    return n;
}

/* Loads one pair and its level 0 surface; returns NULL, or why the pair cannot be used */
static const char *corpus_open(const char *dir, const corpus_entry *e, corpus_pair *p) {
    memset(p, 0, sizeof *p);
    unsigned bpb, bw, bh;
    if (e->target != 1 && e->target != 2) return "not a 2D or 3D texture";
    if (!corpus_format(e->format, &bpb, &bw, &bh)) return "unknown format";
    s2tex_texture t = {e->width, e->height, e->target == 2 ? e->depth : 1, bw, bh, bpb, e->bh, e->bd, e->levels, S2TEX_SWITCH2};
    if (s2tex_level(&t, 0, &p->surf) == UINT64_MAX) return "invalid layout";
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.bl", dir, e->id);
    if (!(p->bl = corpus_read(path, &p->bl_size))) return "no storage file";
    snprintf(path, sizeof path, "%s/%s.raw", dir, e->id);
    if (!(p->raw_file = corpus_read(path, &p->raw_size))) return "no decoded file";
    if (p->bl_size < s2tex_size(&p->surf)) return "storage shorter than level 0";
    unsigned char *nl = memchr(p->raw_file, '\n', p->raw_size < 128 ? p->raw_size : 128);
    if (!nl) return "decoded file without a header";
    unsigned long w, h, d, fmt, row;
    char hdr[129];
    memcpy(hdr, p->raw_file, (size_t)(nl - p->raw_file)); hdr[nl - p->raw_file] = 0;
    if (sscanf(hdr, "%lu %lu %lu %lu %lu", &w, &h, &d, &fmt, &row) != 5) return "decoded header not understood";
    if (w != e->width || h != e->height || d != (e->target == 2 ? e->depth : 1)) return "decoded size differs from the texture";
    p->raw = nl + 1;
    p->row_pitch = row;
    p->slice_pitch = (size_t)row * h;
    if (row < (size_t)p->surf.width * bpb) return "decoded rows shorter than a block row";
    if ((size_t)(p->raw_file + p->raw_size - p->raw) < p->slice_pitch * p->surf.depth) return "decoded file truncated";
    p->data_bytes = (unsigned long long)p->surf.width * bpb * p->surf.height * p->surf.depth;
    return NULL;
}

/* Bytes of the decoded level that differ from lin (laid out at the same pitches) */
static unsigned long long corpus_diff(const corpus_pair *p, const unsigned char *lin) {
    unsigned long long bad = 0;
    size_t rb = (size_t)p->surf.width * p->surf.bytes_per_block;
    for (uint32_t z = 0; z < p->surf.depth; z++)
        for (uint32_t y = 0; y < p->surf.height; y++) {
            size_t o = z * p->slice_pitch + y * p->row_pitch;
            if (memcmp(p->raw + o, lin + o, rb))
                for (size_t x = 0; x < rb; x++) bad += p->raw[o + x] != lin[o + x];
        }
    return bad;
}

static void corpus_close(corpus_pair *p) { free(p->bl); free(p->raw_file); memset(p, 0, sizeof *p); }

typedef struct {
    unsigned n, by_bpb[17], by_bh[6], by_bd[6], by_fmt[256], three_d, mips, cpu, pkg, ragged_w, ragged_h, partial_gob, by_stage_n, size_same;
    char stages[16][32]; unsigned by_stage[16];
    unsigned long long bytes;
    unsigned min_w, max_w, min_h, max_h;
    unsigned distinct; unsigned long long hash[4096];   /* level 0 storage contents seen (the same texture can be listed in several stages and runs) */
} corpus_stats;

static void corpus_count(corpus_stats *s, const corpus_entry *e, const corpus_pair *p) {
    const s2tex_surface *f = &p->surf;
    if (!s->n) { s->min_w = s->min_h = ~0u; }
    s->n++;
    unsigned long long h = 0xcbf29ce484222325ull;   /* FNV-1a of the level 0 storage and the layout */
    size_t len = (size_t)s2tex_size(f);
    for (size_t i = 0; i < len; i++) { h ^= p->bl[i]; h *= 0x100000001b3ull; }
    h ^= (unsigned long long)e->width << 40 ^ (unsigned long long)e->height << 20 ^ e->format;
    unsigned j = 0;
    while (j < s->distinct && s->hash[j] != h) j++;
    if (j == s->distinct && j < 4096) s->hash[s->distinct++] = h;
    if (f->bytes_per_block <= 16) s->by_bpb[f->bytes_per_block]++;
    s->by_bh[f->block_height]++;
    s->by_bd[f->block_depth]++;
    s->by_fmt[e->format & 255]++;
    s->three_d += e->target == 2;
    s->mips += e->levels > 1;
    if (e->cpu_written) s->cpu++; else s->pkg++;
    unsigned bpb, bw, bh;
    if (corpus_format(e->format, &bpb, &bw, &bh)) {
        s2tex_texture t = {e->width, e->height, e->target == 2 ? e->depth : 1, bw, bh, bpb, e->bh, e->bd, e->levels, S2TEX_SWITCH2};
        s->size_same += s2tex_layer_size(&t) == e->size;
    }
    uint32_t rb = f->width * f->bytes_per_block;
    s->partial_gob += (rb % 64) != 0;
    s->ragged_h += (f->height % (8u << f->block_height)) != 0;
    s->ragged_w += (e->width & (e->width - 1)) != 0 || (e->height & (e->height - 1)) != 0;
    s->bytes += p->data_bytes;
    if (e->width < s->min_w) s->min_w = e->width;
    if (e->width > s->max_w) s->max_w = e->width;
    if (e->height < s->min_h) s->min_h = e->height;
    if (e->height > s->max_h) s->max_h = e->height;
    unsigned k = 0;
    while (k < s->by_stage_n && strcmp(s->stages[k], e->stage)) k++;
    if (k == s->by_stage_n && k < 16) { snprintf(s->stages[k], sizeof s->stages[k], "%s", e->stage); s->by_stage_n++; }
    if (k < 16) s->by_stage[k]++;
}

static void corpus_print_stats(const corpus_stats *s) {
    printf("corpus: %u pairs reproduced (%u distinct textures), %.1f MB of texels; sizes %ux%u to %ux%u\n", s->n, s->distinct, s->bytes / 1e6,
           s->min_w, s->min_h, s->max_w, s->max_h);
    printf("  bytes per block:");
    for (int i = 1; i <= 16; i++) if (s->by_bpb[i]) printf(" %d: %u", i, s->by_bpb[i]);
    printf("\n  level 0 block height (log2 GOBs):");
    for (int i = 0; i < 6; i++) if (s->by_bh[i]) printf(" %d: %u", i, s->by_bh[i]);
    printf("\n  block depth (log2):");
    for (int i = 0; i < 6; i++) if (s->by_bd[i]) printf(" %d: %u", i, s->by_bd[i]);
    printf("\n  formats:");
    for (int i = 0; i < 256; i++) if (s->by_fmt[i]) printf(" %#x: %u", i, s->by_fmt[i]);
    printf("\n  3D: %u; with mip levels: %u; packaged or GPU-uploaded: %u; CPU-written: %u\n", s->three_d, s->mips, s->pkg, s->cpu);
    printf("  s2tex's storage size (all levels) equals the host's listed size: %u of %u\n", s->size_same, s->n);
    printf("  non-power-of-two sizes: %u; rows not a whole GOB (64 B): %u; height not a whole block: %u\n", s->ragged_w, s->partial_gob, s->ragged_h);
    printf("  by stage:");
    for (unsigned k = 0; k < s->by_stage_n; k++) printf(" %s: %u", s->stages[k], s->by_stage[k]);
    printf("\n");
}

#endif
