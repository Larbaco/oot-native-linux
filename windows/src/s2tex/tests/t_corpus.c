/* The real-data corpus: pairs of a texture's guest storage (memdump) and the level 0 the current Mac host decoded from it (DM_NVN_TEXRAW),
 * collected by tools/collect.py into a folder outside the repository (_local/s2tex/corpus). Each pair must be reproduced bit for bit by
 * s2tex in Switch 2 order; the Tegra X1 order is run beside it for contrast. Exit 77 (skipped) when the corpus is absent.
 * Usage: s2tex_corpus <corpus folder> [-v]. MIT licence, part of pokopia-native. */
#include "s2tex.h"
#include "corpus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Whether the two orders give different bytes for this pair: only then can the pair tell them apart */
static int orders_differ(const corpus_pair *p, const unsigned char *a, const unsigned char *b) {
    size_t rb = (size_t)p->surf.width * p->surf.bytes_per_block;
    for (uint32_t z = 0; z < p->surf.depth; z++)
        for (uint32_t y = 0; y < p->surf.height; y++) {
            size_t o = z * p->slice_pitch + y * p->row_pitch;
            if (memcmp(a + o, b + o, rb)) return 1;
        }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: s2tex_corpus <corpus folder> [-v]\n"); return 2; }
    int verbose = argc > 2 && !strcmp(argv[2], "-v");
    corpus_entry *e = NULL;
    int n = corpus_load(argv[1], &e);
    if (n < 0) { printf("s2tex corpus: no manifest in %s, skipped\n", argv[1]); return 77; }
    corpus_stats st;
    memset(&st, 0, sizeof st);
    int pass = 0, fail = 0, skip = 0, tx1_same = 0, telling = 0, telling_s2 = 0, telling_tx1 = 0;
    for (int i = 0; i < n; i++) {
        corpus_pair p;
        const char *why = corpus_open(argv[1], &e[i], &p);
        if (why) { skip++; printf("SKIP %s: %s\n", e[i].id, why); corpus_close(&p); continue; }
        size_t lin_size = (size_t)p.slice_pitch * p.surf.depth;
        unsigned char *s2 = calloc(lin_size, 1), *tx = calloc(lin_size, 1);
        int r1 = s2tex_deswizzle(&p.surf, p.bl, p.bl_size, s2, p.row_pitch, p.slice_pitch);
        s2tex_surface t = p.surf; t.order = S2TEX_TX1;
        int r2 = s2tex_deswizzle(&t, p.bl, p.bl_size, tx, p.row_pitch, p.slice_pitch);
        uint64_t bad = r1 ? UINT64_MAX : corpus_diff(&p, s2), bad_tx1 = r2 ? UINT64_MAX : corpus_diff(&p, tx);
        int tells = !r1 && !r2 && orders_differ(&p, s2, tx);
        if (!bad) { pass++; corpus_count(&st, &e[i], &p); } else fail++;
        if (!bad_tx1) tx1_same++;
        if (tells) { telling++; telling_s2 += !bad; telling_tx1 += !bad_tx1; }
        if (bad || verbose)
            printf("%s %s: %ux%ux%u format %#x, %u B/block, block height/depth 2^%u/2^%u: Switch 2 order %llu bytes differ, Tegra X1 order %llu of %llu%s\n",
                   bad ? "FAIL" : "ok  ", e[i].id, e[i].width, e[i].height, e[i].depth, e[i].format, p.surf.bytes_per_block, p.surf.block_height,
                   p.surf.block_depth, (unsigned long long)bad, (unsigned long long)bad_tx1, (unsigned long long)p.data_bytes,
                   tells ? "" : " (both orders give the same bytes)");
        free(s2); free(tx);
        corpus_close(&p);
    }
    corpus_print_stats(&st);
    printf("s2tex corpus: %d pairs reproduced bit for bit in Switch 2 order, %d differ, %d skipped; the Tegra X1 order reproduces %d\n", pass, fail, skip, tx1_same);
    printf("  %d pairs tell the two orders apart: Switch 2 reproduces %d of them, Tegra X1 %d\n", telling, telling_s2, telling_tx1);
    free(e);
    return fail || !pass ? 1 : 0;
}
