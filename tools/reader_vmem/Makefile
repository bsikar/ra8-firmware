# tools/reader_vmem/Makefile -- host build of the #147/#162 reader workload
# driver. Links the REAL firmware Layer-1/Layer-2 page cache (ra_vmem +
# ra_vsource) so the emitted trace is what the production cache actually sees.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

CC      ?= cc
CFLAGS  ?= -std=gnu23 -O2 -g -Wall -Wextra -Wno-unused-parameter
ROOT    := ../..
INCS    := -I$(ROOT)/libs/ra_core/inc -I$(ROOT)/libs/ra_mem/inc
SRC     := reader_vmem.c \
           $(ROOT)/libs/ra_mem/src/ra_vmem.c \
           $(ROOT)/libs/ra_mem/src/ra_vsource.c

reader_vmem: $(SRC)
	$(CC) $(CFLAGS) $(INCS) $(SRC) -o $@

.PHONY: run clean
run: reader_vmem
	./reader_vmem reader_vmem.trace

clean:
	rm -f reader_vmem reader_vmem.trace
