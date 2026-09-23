#!/usr/bin/env python3
"""Add uf-lzma2 to an lzbench checkout as its own codec, `uflzma2`, next to `fastlzma2`.

    git -C lzbench checkout 3d22c41
    python3 bench/lzbench/add_uflzma2.py lzbench
    mkdir -p lzbench/lz/uf-lzma2 && cp *.c *.h lzma_dec_*.S lzbench/lz/uf-lzma2/
    make -C lzbench -j

lzbench builds fast-lzma2 from its C files only, so its `fastlzma2` rows are the C
decoder. The `uflzma2` entry makes the same calls with the same compile flags, plus
the assembler LZMA decoder; the two produce identical compressed sizes, so the only
difference between their rows is how they are built.

The two libraries' internal symbols share names, so uf-lzma2 is linked into one
relocatable object with every symbol but the UF2_* API made local. The main
`lzbench:` link line is not edited: a recipe-less rule adds the object instead,
so this applies cleanly alongside other lzbench patches that edit that line.
Written against lzbench 3d22c41; it stops with an error naming the anchor if a
later lzbench no longer matches.
"""
import pathlib, sys
root = pathlib.Path(sys.argv[1])
def rep1(rel, old, new):
    p = root / rel; s = p.read_text()
    n = s.count(old)
    if n != 1: sys.exit(f"FALLO {rel}: ancla encontrada {n} veces: {old[:70]!r}")
    p.write_text(s.replace(old, new))

rep1("bench/codecs.h", "#ifndef BENCH_REMOVE_FASTLZMA2\n",
"""#ifndef BENCH_REMOVE_UFLZMA2
    int64_t lzbench_uflzma2_compress(char *inbuf, size_t insize, char *outbuf, size_t outsize, codec_options_t *codec_options);
    int64_t lzbench_uflzma2_decompress(char *inbuf, size_t insize, char *outbuf, size_t outsize, codec_options_t *codec_options);
#else
    #define lzbench_uflzma2_compress NULL
    #define lzbench_uflzma2_decompress NULL
#endif

#ifndef BENCH_REMOVE_FASTLZMA2
""")

rep1("bench/lz_codecs.cpp", "#endif // BENCH_REMOVE_FASTLZMA2\n",
"""#endif // BENCH_REMOVE_FASTLZMA2



#ifndef BENCH_REMOVE_UFLZMA2
#include "lz/uf-lzma2/uf-lzma2.h"

/* Same calls as the fastlzma2 entry above: the only difference between the two
   rows is the library version and its assembler LZMA decoder. */
int64_t lzbench_uflzma2_compress(char *inbuf, size_t insize, char *outbuf, size_t outsize, codec_options_t *codec_options)
{
    size_t ret = UF2_compressMt(outbuf, outsize, inbuf, insize, codec_options->level, codec_options->threads);
    if (UF2_isError(ret)) return 0;
    return ret;
}

int64_t lzbench_uflzma2_decompress(char *inbuf, size_t insize, char *outbuf, size_t outsize, codec_options_t *codec_options)
{
    size_t ret = UF2_decompressMt(outbuf, outsize, inbuf, insize, codec_options->threads);
    if (UF2_isError(ret)) return 0;
    return ret;
}
#endif // BENCH_REMOVE_UFLZMA2
""")

p = root / "bench/lzbench.h"; s = p.read_text()
line = next(l for l in s.split("\n") if l.lstrip().startswith('{ "fastlzma2",'))
new = line.replace('"fastlzma2",  "fastlzma2 1.0.1",        ', '"uflzma2",    "uf-lzma2 1.1.0",         ') \
          .replace("lzbench_fastlzma2_compress,  lzbench_fastlzma2_decompress,  ",
                   "lzbench_uflzma2_compress,    lzbench_uflzma2_decompress,    ")
if new == line or "uflzma2" not in new: sys.exit("FALLO lzbench.h: no pude derivar la fila")
rep1("bench/lzbench.h", line + "\n", line + "\n" + new + "\n")

rep1("Makefile", 'ifeq "$(DONT_BUILD_FASTLZMA2)" "1"\n',
"""# uf-lzma2 is fast-lzma2 renamed, so its internal symbols (LZMA2_*, RMF_*, xxhash...)
# collide with the fastlzma2 codec's. It is built with the same flags as fastlzma2
# plus its assembler LZMA decoder, linked into one relocatable object, and every
# symbol except the UF2_* API is made local.
ifeq "$(DONT_BUILD_UFLZMA2)" "1"
    DEFINES += -DBENCH_REMOVE_UFLZMA2
else
    UFLZMA2_SRC = $(wildcard lz/uf-lzma2/*.c)
    UFLZMA2_OBJ = $(UFLZMA2_SRC:.c=.uo)
    UFLZMA2_ARCH := $(shell uname -m)
    ifeq "$(UFLZMA2_ARCH)" "x86_64"
        UFLZMA2_ASM = lz/uf-lzma2/lzma_dec_x86_64.uo
        UFLZMA2_ASFLAGS = -DMS_x64_CALL=0
    endif
    ifneq ($(filter aarch64 arm64,$(UFLZMA2_ARCH)),)
        UFLZMA2_ASM = lz/uf-lzma2/lzma_dec_arm64.uo
    endif
    ifneq "$(UFLZMA2_ASM)" ""
        UFLZMA2_DEFS = -DLZMA2_DEC_OPT
    endif
    UFLZMA2_BUNDLE = lz/uf-lzma2/uf-lzma2-bundle.o
endif
ifeq "$(DISABLE_THREADING)" "1"
    UFLZMA2_FLAGS = -DUF2_SINGLETHREAD
endif

ifeq "$(DONT_BUILD_FASTLZMA2)" "1"
""")
rep1("Makefile", """	$(CC) $(CFLAGS) $(FASTLZMA2_FLAGS) -DNO_XXHASH $< -c -o $@
""", """	$(CC) $(CFLAGS) $(FASTLZMA2_FLAGS) -DNO_XXHASH $< -c -o $@

lz/uf-lzma2/%.uo : lz/uf-lzma2/%.c
	$(CC) $(CFLAGS) $(UFLZMA2_FLAGS) $(UFLZMA2_DEFS) -DNO_XXHASH $< -c -o $@

lz/uf-lzma2/%.uo : lz/uf-lzma2/%.S
	$(CC) $(UFLZMA2_ASFLAGS) $< -c -o $@

$(UFLZMA2_BUNDLE): $(UFLZMA2_OBJ) $(UFLZMA2_ASM)
	$(LD) -r -o $@.tmp $^
	objcopy -w --keep-global-symbol='UF2_*' $@.tmp $@
	rm -f $@.tmp

# A recipe-less rule adds a prerequisite without editing the main lzbench: line,
# which other patches (lz6's, for one) also edit. The link recipe uses $^, which
# collects the prerequisites of every rule for the target.
lzbench: $(UFLZMA2_BUNDLE)
""")
print("ok")
