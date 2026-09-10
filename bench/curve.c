/* curve.c -- emit the speed/ratio curve as machine readable TSV.
 *
 * The stated design goal of this library is to "move the line as far as
 * possible toward the top left of the graph": more ratio at more speed than
 * 7-Zip's LZMA2. That is a claim about a curve, so it needs an instrument that
 * draws one reproducibly, and that a later change can be diffed against.
 *
 * For every compression level, and optionally the high-compression table too,
 * the whole corpus is compressed in one pass and then decompressed in one
 * pass. Each pass runs -n times and the fastest is kept, which is the usual
 * convention for compression benchmarks: the fastest run is the one least
 * disturbed by everything else on the machine.
 *
 * Output is one TSV row per (mode, level) on stdout, plus '#' comment lines
 * carrying the run's parameters, so two runs are directly comparable with
 * diff and the file can be fed to a plotter unchanged.
 *
 * -s measures the corpus as one solid stream instead of file by file, which is
 * what an archiver actually does and what the published graph this library is
 * measured against was drawn from. The difference is not cosmetic: file by
 * file, the useful dictionary is capped by the largest single file, so above
 * that size the dictionary stops mattering and levels that differ only in
 * dictionary size produce byte identical output. Any question about dictionary
 * size, and therefore the known 2x dictionary caveat of this match finder,
 * can only be asked in solid mode.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../uf-lzma2.h"
#include "../mem.h"
#include "../util.h"

#define MAX_FILES 4096

typedef struct {
    char *name;
    unsigned char *src;
    size_t srcSize;
    unsigned char *cmp;
    size_t cmpCap;
    size_t cmpSize;
} bfile;

static bfile files[MAX_FILES];
static unsigned nbFiles = 0;
static int solid = 0;
static unsigned char *decBuf = NULL;
static size_t decCap = 0;

static int loadFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "curve: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }   /* skip empty files */
    if (nbFiles == MAX_FILES) { fclose(f); fprintf(stderr, "curve: too many files\n"); return 1; }

    bfile *b = &files[nbFiles];
    b->srcSize = (size_t)n;
    b->src = malloc(b->srcSize);
    b->cmpCap = UF2_compressBound(b->srcSize);
    b->cmp = malloc(b->cmpCap);
    if (!b->src || !b->cmp) { fclose(f); fprintf(stderr, "curve: out of memory\n"); return 1; }
    if (fread(b->src, 1, b->srcSize, f) != b->srcSize) {
        fclose(f); fprintf(stderr, "curve: short read on %s\n", path); return 1;
    }
    fclose(f);
    const char *base = strrchr(path, '/');
    b->name = strdup(base ? base + 1 : path);
    if (b->srcSize > decCap) decCap = b->srcSize;
    nbFiles++;
    return 0;
}

/* one compression pass over the whole corpus; returns total compressed bytes */
static size_t compressPass(UF2_CCtx *cctx, int *err)
{
    size_t total = 0;
    for (unsigned i = 0; i < nbFiles; i++) {
        size_t cs = UF2_compressCCtx(cctx, files[i].cmp, files[i].cmpCap,
                                     files[i].src, files[i].srcSize, 0);
        if (UF2_isError(cs)) { *err = 1; return 0; }
        files[i].cmpSize = cs;
        total += cs;
    }
    return total;
}

/* one decompression pass; verifies every file against its source */
static int decompressPass(UF2_DCtx *dctx)
{
    for (unsigned i = 0; i < nbFiles; i++) {
        size_t ds = UF2_decompressDCtx(dctx, decBuf, files[i].srcSize,
                                       files[i].cmp, files[i].cmpSize);
        if (UF2_isError(ds) || ds != files[i].srcSize
            || memcmp(decBuf, files[i].src, files[i].srcSize) != 0)
            return 1;
    }
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s [options] <file> ...\n", prog);
    printf("  -T#   compression threads (default 1)\n");
    printf("  -D#   decompression threads (default 1)\n");
    printf("  -n#   iterations per pass, fastest is kept (default 3)\n");
    printf("  -b#   first level (default 1)\n");
    printf("  -e#   last level (default max)\n");
    printf("  -H    also measure the high-compression table\n");
    printf("  -s    measure the corpus as one solid stream, as an archiver would\n");
    printf("  -h    this help\n");
    printf("\nSingle threaded by default so that runs are comparable.\n");
}

int main(int argc, char **argv)
{
    unsigned cThreads = 1, dThreads = 1, iters = 3;
    int firstLevel = 1, lastLevel = 0, doHigh = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        switch (argv[i][1]) {
            case 'T': cThreads = (unsigned)atoi(argv[i] + 2); break;
            case 'D': dThreads = (unsigned)atoi(argv[i] + 2); break;
            case 'n': iters = (unsigned)atoi(argv[i] + 2); break;
            case 'b': firstLevel = atoi(argv[i] + 2); break;
            case 'e': lastLevel = atoi(argv[i] + 2); break;
            case 'H': doHigh = 1; break;
            case 's': solid = 1; break;
            case 'h': usage(argv[0]); return 0;
            default: fprintf(stderr, "curve: unknown option %s\n", argv[i]); return 1;
        }
    }
    if (iters < 1) iters = 1;

    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-' && loadFile(argv[i])) return 2;

    if (nbFiles == 0) { usage(argv[0]); return 1; }

    if (solid) {
        /* fold the corpus into a single input, in the order given */
        size_t total = 0;
        for (unsigned i = 0; i < nbFiles; i++) total += files[i].srcSize;
        unsigned char *all = malloc(total);
        if (!all) { fprintf(stderr, "curve: out of memory\n"); return 2; }
        size_t at = 0;
        for (unsigned i = 0; i < nbFiles; i++) {
            memcpy(all + at, files[i].src, files[i].srcSize);
            at += files[i].srcSize;
            free(files[i].src); free(files[i].cmp); free(files[i].name);
        }
        files[0].name = strdup("<solid>");
        files[0].src = all;
        files[0].srcSize = total;
        files[0].cmpCap = UF2_compressBound(total);
        files[0].cmp = malloc(files[0].cmpCap);
        if (!files[0].cmp) { fprintf(stderr, "curve: out of memory\n"); return 2; }
        nbFiles = 1;
        decCap = total;
    }

    decBuf = malloc(decCap);
    if (!decBuf) { fprintf(stderr, "curve: out of memory\n"); return 2; }

    size_t corpusBytes = 0;
    for (unsigned i = 0; i < nbFiles; i++) corpusBytes += files[i].srcSize;

    printf("# uf-lzma2 %s\n", UF2_versionString());
    printf("# corpus_files=%u corpus_bytes=%zu mode=%s\n",
           nbFiles, corpusBytes, solid ? "solid" : "per-file");
    printf("# cthreads=%u dthreads=%u iterations=%u\n", cThreads, dThreads, iters);
    printf("mode\tlevel\tdict\tin_bytes\tout_bytes\tratio\tc_MBps\td_MBps\n");
    fflush(stdout);

    for (int high = 0; high <= doHigh; high++) {
        int maxLevel = high ? UF2_maxHighCLevel() : UF2_maxCLevel();
        int last = lastLevel ? lastLevel : maxLevel;
        if (last > maxLevel) last = maxLevel;

        for (int level = firstLevel; level <= last; level++) {
            UF2_CCtx *cctx = UF2_createCCtxMt(cThreads);
            UF2_DCtx *dctx = UF2_createDCtxMt(dThreads);
            if (!cctx || !dctx) { fprintf(stderr, "curve: context allocation failed\n"); return 2; }
            UF2_CCtx_setParameter(cctx, UF2_p_highCompression, (size_t)high);
            UF2_CCtx_setParameter(cctx, UF2_p_compressionLevel, (size_t)level);

            UF2_compressionParameters params;
            UF2_getLevelParameters(level, high, &params);

            int err = 0;
            size_t outBytes = 0;
            U64 bestC = (U64)-1, bestD = (U64)-1;

            for (unsigned it = 0; it < iters; it++) {
                UTIL_waitForNextTick();
                UTIL_time_t t = UTIL_getTime();
                outBytes = compressPass(cctx, &err);
                U64 span = UTIL_clockSpanMicro(t);
                if (err) { fprintf(stderr, "curve: compression failed at level %d\n", level); return 3; }
                if (span < bestC) bestC = span;
            }
            for (unsigned it = 0; it < iters; it++) {
                UTIL_waitForNextTick();
                UTIL_time_t t = UTIL_getTime();
                if (decompressPass(dctx)) {
                    fprintf(stderr, "curve: decompression mismatch at level %d\n", level);
                    return 3;
                }
                U64 span = UTIL_clockSpanMicro(t);
                if (span < bestD) bestD = span;
            }

            double mb = (double)corpusBytes / (1024.0 * 1024.0);
            printf("%s\t%d\t%u\t%zu\t%zu\t%.6f\t%.2f\t%.2f\n",
                   high ? "high" : "normal", level, (unsigned)params.dictionarySize,
                   corpusBytes, outBytes, (double)outBytes / (double)corpusBytes,
                   bestC ? mb / ((double)bestC / 1000000.0) : 0.0,
                   bestD ? mb / ((double)bestD / 1000000.0) : 0.0);
            fflush(stdout);

            UF2_freeCCtx(cctx);
            UF2_freeDCtx(dctx);
        }
    }
    return 0;
}
