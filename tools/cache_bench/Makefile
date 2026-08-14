# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# tools/cache_bench/Makefile -- host build of the #147 eviction benchmark and
# the #208 block/frame-size sweep. The sweep drives the REAL ra8_vmem/ra8_vsource
# page cache and the REAL ra8_book_chunked RBKC reader (with miniz for genuine
# zlib chunk streams), so those firmware TUs are compiled in directly.
# miniz is SOUP: it is built as its own object with -w -fno-strict-aliasing,
# matching how tests/CMakeLists.txt compiles it (gcc miscompiles its inflate
# under strict aliasing at -O2).

CC      ?= cc
CFLAGS  ?= -std=gnu23 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter
ROOT    := ../..
INCS    := -I$(ROOT)/libs/ra8_core/inc \
           -I$(ROOT)/libs/ra8_mem/inc \
           -I$(ROOT)/libs/ra8_book/inc \
           -I$(ROOT)/libs/third_party/miniz
SRC     := $(wildcard *.c) \
           $(ROOT)/libs/ra8_mem/src/ra8_vmem.c \
           $(ROOT)/libs/ra8_mem/src/ra8_keycache.c \
           $(ROOT)/libs/ra8_mem/src/ra8_vsource.c \
           $(ROOT)/libs/ra8_book/src/ra8_book.c \
           $(ROOT)/libs/ra8_book/src/ra8_book_chunked.c
HDR     := $(wildcard *.h)
MINIZ   := miniz_host.o

cache_bench: $(SRC) $(HDR) $(MINIZ)
	$(CC) $(CFLAGS) $(INCS) $(SRC) $(MINIZ) -o $@

$(MINIZ): $(ROOT)/libs/third_party/miniz/miniz.c
	$(CC) $(CFLAGS) -w -fno-strict-aliasing -c $< -o $@

.PHONY: run sweep clean
run: cache_bench
	./cache_bench

sweep: cache_bench
	./cache_bench --sweep-block

clean:
	rm -f cache_bench $(MINIZ)
