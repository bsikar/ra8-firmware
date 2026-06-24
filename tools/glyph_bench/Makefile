# tools/glyph_bench/Makefile -- host build of the #147/#164 glyph-cache budget
# sweep. Links the REAL Layer-3 glyph atlas (ra_glyph_atlas + ra_keycache) so
# the measured hit rate is the production cache's behaviour.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

CC      ?= cc
CFLAGS  ?= -std=gnu23 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter
ROOT    := ../..
INCS    := -I$(ROOT)/libs/ra_core/inc -I$(ROOT)/libs/ra_mem/inc
SRC     := glyph_bench.c \
           $(ROOT)/libs/ra_mem/src/ra_glyph_atlas.c \
           $(ROOT)/libs/ra_mem/src/ra_keycache.c

glyph_bench: $(SRC)
	$(CC) $(CFLAGS) $(INCS) $(SRC) -o $@

.PHONY: run clean
run: glyph_bench
	./glyph_bench

clean:
	rm -f glyph_bench
