# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
# Module-side build wrapper.

RA8_REPO_ROOT := $(realpath $(CURDIR)/../../../../..)
ELF := $(CURDIR)/build/hello_module.elf

.PHONY: all clean

all: $(ELF)

$(ELF):
	cmake -S . -B build \
		-DCMAKE_TOOLCHAIN_FILE=$(RA8_REPO_ROOT)/cmake/toolchain-ra8d2.cmake \
		-DCMAKE_BUILD_TYPE=MinSizeRel
	cmake --build build

clean:
	rm -rf build
