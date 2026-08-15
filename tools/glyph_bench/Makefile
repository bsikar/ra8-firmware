# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# tools/glyph_bench/Makefile -- host build of the #147/#164 glyph-cache budget
# sweep. Links the REAL Layer-3 glyph atlas (ra8_glyph_atlas + ra8_keycache) so
# the measured hit rate is the production cache's behaviour.

CC      ?= cc
CFLAGS  ?= -std=gnu23 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter
CPPFLAGS += -DRA8_OFF_TARGET
ROOT    := ../..
INCS    := -I$(ROOT)/libs/ra8_core/inc -I$(ROOT)/libs/ra8_io/inc \
           -I$(ROOT)/libs/ra8_mem/inc -I$(ROOT)/port/posix/inc \
           -I$(ROOT)/port/posix/src
SRC     := glyph_bench.c \
           $(ROOT)/libs/ra8_core/src/ra8_log.c \
           $(ROOT)/libs/ra8_io/src/ra8_io_stream.c \
           $(ROOT)/libs/ra8_mem/src/ra8_glyph_atlas.c \
           $(ROOT)/libs/ra8_mem/src/ra8_keycache.c \
           $(ROOT)/port/posix/src/ra8_io_stream_posix.c

glyph_bench: $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCS) $(SRC) -o $@

.PHONY: run test clean
run: glyph_bench
	./glyph_bench

test: glyph_bench
	bash tests/integration.sh ./glyph_bench

clean:
	rm -f glyph_bench
