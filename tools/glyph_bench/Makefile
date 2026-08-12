# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# tools/glyph_bench/Makefile -- host build of the #147/#164 glyph-cache budget
# sweep. Links the REAL Layer-3 glyph atlas (ra8_glyph_atlas + ra8_keycache) so
# the measured hit rate is the production cache's behaviour.

CC      ?= cc
CFLAGS  ?= -std=gnu23 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter
ROOT    := ../..
INCS    := -I$(ROOT)/libs/ra8_core/inc -I$(ROOT)/libs/ra8_mem/inc
SRC     := glyph_bench.c \
           $(ROOT)/libs/ra8_mem/src/ra8_glyph_atlas.c \
           $(ROOT)/libs/ra8_mem/src/ra8_keycache.c

glyph_bench: $(SRC)
	$(CC) $(CFLAGS) $(INCS) $(SRC) -o $@

.PHONY: run clean
run: glyph_bench
	./glyph_bench

clean:
	rm -f glyph_bench
