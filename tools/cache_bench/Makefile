# tools/cache_bench/Makefile -- host build of the #147 eviction benchmark.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

CC      ?= cc
CFLAGS  ?= -std=gnu23 -O2 -g -Wall -Wextra -Wno-unused-parameter
SRC     := $(wildcard *.c)
HDR     := $(wildcard *.h)

cache_bench: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $@

.PHONY: run clean
run: cache_bench
	./cache_bench

clean:
	rm -f cache_bench
