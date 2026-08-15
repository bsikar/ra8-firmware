# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# tools/reader_vmem/Makefile -- host build of the #147/#162 reader workload
# driver. Links the REAL firmware Layer-1/Layer-2 page cache (ra8_vmem +
# ra8_vsource) so the emitted trace is what the production cache actually sees.

CC      ?= cc
CFLAGS  ?= -std=gnu23 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter
CPPFLAGS ?=
ROOT    := ../..
INCS    := -I. -I$(ROOT)/libs/ra8_core/inc -I$(ROOT)/libs/ra8_mem/inc
MAX_BUDGET ?= 1024
CPPFLAGS += -DREADER_VMEM_MAX_BUDGET=$(MAX_BUDGET)
SRC     := reader_vmem.c reader_vmem_host.c reader_vmem_workspace.c \
           $(ROOT)/libs/ra8_mem/src/ra8_vmem.c \
           $(ROOT)/libs/ra8_mem/src/ra8_keycache.c \
           $(ROOT)/libs/ra8_mem/src/ra8_vsource.c
TEST_SRC := tests/reader_vmem_workspace_test.c reader_vmem_workspace.c

reader_vmem: $(SRC) reader_vmem_internal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCS) $(SRC) -o $@

reader_vmem_workspace_test: $(TEST_SRC) reader_vmem_internal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCS) $(TEST_SRC) -o $@

.PHONY: run test clean
run: reader_vmem
	./reader_vmem reader_vmem.trace

test: reader_vmem reader_vmem_workspace_test
	bash tests/integration.sh ./reader_vmem ./reader_vmem_workspace_test $(MAX_BUDGET)

clean:
	rm -f reader_vmem reader_vmem_workspace_test reader_vmem.trace
