# mk/tools.mk -- host developer tools (native, NOT cross-compiled) and the host
# codegen helpers. Tool dirs (MEDIA_DL_DIR, RA8_VIEWER_DIR, BOARD_SIM_DIR) come
# from the top Makefile.
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

.PHONY: tools tools-help media_dl test-media_dl test-integration viewer view dl \
        mcp books rabook-golden-update bench-cache

# `make tools` -- build every compiled host tool in one go.
tools: media_dl viewer
	@echo "== board_sim =="
	$(CMAKE) -B $(BOARD_SIM_DIR)/build -S $(BOARD_SIM_DIR)
	$(CMAKE) --build $(BOARD_SIM_DIR)/build -j
	@echo "== mkbookimg =="
	$(CMAKE) -B $(ROOT)/tools/mkbookimg/build -S $(ROOT)/tools/mkbookimg
	$(CMAKE) --build $(ROOT)/tools/mkbookimg/build -j
	@echo "== cache/reader/glyph benches =="
	$(MAKE) -C $(ROOT)/tools/cache_bench CC="$(CC)"
	$(MAKE) -C $(ROOT)/tools/reader_vmem CC="$(CC)"
	$(MAKE) -C $(ROOT)/tools/glyph_bench CC="$(CC)"
	@echo "all host tools built"

tools-help:
	@echo "make tools  -- build every compiled host tool (native, not firmware)"
	@echo ""
	@echo "READER / DOWNLOADER (macOS)"
	@echo "  make media_dl                        build the comic/manga/manhwa downloader CLI"
	@echo "  make dl ARGS='--config S.conf ...'    build + run the downloader with ARGS"
	@echo "  make test-media_dl                   build + run the downloader unit tests (ctest)"
	@echo "  make test-integration [FMT='cbz ...'] pack synthetic pages in EVERY format + view each (end-to-end)"
	@echo "  make viewer                          build the native reader viewer"
	@echo "  make view FILE=<doc> [HEADLESS=1]    build the viewer + open a document (arrows page)"
	@echo ""
	@echo "OTHER HOST TOOLS"
	@echo "  make mcp                             self-test the MCP dev server (tools/mcp)"
	@echo "  make bench-cache                     build + run the L1/L2/L3 cache benches"
	@echo "  make books                           compile content/library/*.epub -> *.rabook"
	@echo ""
	@echo "board_sim (the emulator) is built on demand by 'make sim-<app>'  [make sim-help]"

# --- media_dl (downloader) --------------------------------------------------
media_dl:
	$(CMAKE) -B $(MEDIA_DL_DIR)/build -S $(MEDIA_DL_DIR)
	$(CMAKE) --build $(MEDIA_DL_DIR)/build -j
	@echo "  built $(MEDIA_DL_DIR)/build/media_dl"
	@echo "  run:  make dl ARGS='--config tools/media_dl/sites/manhwaus.conf --series <url> --chapters 2 --format cbz'"

# `make dl ARGS='...'` -- build media_dl and run it with ARGS. This is the
# make-native way to pass through flags (make does not forward bare '-- --flag'
# words cleanly, so pass them as a single ARGS='...' string). The ARGS guard runs
# BEFORE the build, so a bare `make dl` errors without building.
dl:
	@test -n "$(ARGS)" || { echo "usage: make dl ARGS='--config tools/media_dl/sites/<site>.conf --series <url> --chapters N --format cbz'"; exit 2; }
	$(MAKE) media_dl
	$(MEDIA_DL_DIR)/build/media_dl $(ARGS)

test-media_dl:
	$(CMAKE) -B $(MEDIA_DL_DIR)/build -S $(MEDIA_DL_DIR)
	$(CMAKE) --build $(MEDIA_DL_DIR)/build -j
	ctest --test-dir $(MEDIA_DL_DIR)/build --output-on-failure

# `make test-integration [FMT='cbz jof ...']` -- the cross-tool end-to-end gate:
# build media_dl + the viewer, package synthetic (non-copyright) pages into EVERY
# export format, then open each result in the viewer headless and assert a
# non-blank render. This is what catches "packages fine but the reader can't open
# it" (the 0x107 class). Formats needing an absent optional tool (rar) are
# skipped, not failed. Exit status = number of failed formats.
test-integration:
	bash $(MEDIA_DL_DIR)/tests/integration.sh $(FMT)

# --- ra8_viewer (reader) ----------------------------------------------------
# `viewer` only BUILDS; `view FILE=<doc>` builds and OPENS a document.
viewer:
	$(CMAKE) -B $(RA8_VIEWER_DIR)/build -S $(RA8_VIEWER_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(RA8_VIEWER_DIR)/build -j
	@echo "  built $(RA8_VIEWER_DIR)/build/ra8_viewer"
	@if [ -n "$(FILE)" ]; then \
		echo "  note: 'viewer' only builds. To OPEN a document use: make view FILE=$(FILE)"; \
	fi

# `make view FILE=<doc>` -- build the viewer and open FILE in a window (Left/
# Right arrows turn pages). HEADLESS=1 renders page 0 to a PPM and exits. The
# FILE guard runs BEFORE the build, so a bare `make view` errors without building.
view:
	@test -n "$(FILE)" || { echo "usage: make view FILE=<doc> [HEADLESS=1]"; exit 2; }
	$(MAKE) viewer
	$(RA8_VIEWER_DIR)/build/ra8_viewer "$(FILE)" \
		$(if $(filter-out 0,$(HEADLESS)),--headless --dump-ppm /tmp/ra8_view.ppm,)

# --- other host tools -------------------------------------------------------
# `make mcp` -- self-test the Model Context Protocol dev server (tools/mcp).
mcp:
	@python3 $(ROOT)/tools/mcp/ra8_mcp.py --selftest

# `make books` -- regenerate content/compiled/*.rabook from content/library/*.epub
# (Git LFS) plus the manifest header. See tools/epub_compile/.
books:
	bash scripts/build_books.sh

# `make bench-cache` -- the #147/#160/#208 cache-bench toolchain: build + run the
# host tools that exercise the real L1/L2/L3 caches. CC is forwarded so CI can
# pin a C23 compiler.
bench-cache:
	$(MAKE) -C $(ROOT)/tools/cache_bench  CC="$(CC)"
	$(MAKE) -C $(ROOT)/tools/reader_vmem  CC="$(CC)"
	$(MAKE) -C $(ROOT)/tools/glyph_bench  CC="$(CC)"
	@echo ""
	@echo "==== reader_vmem: drive real ra8_vmem, emit a cache_bench trace ===="
	$(ROOT)/tools/reader_vmem/reader_vmem $(ROOT)/tools/reader_vmem/reader_vmem.trace
	@echo ""
	@echo "==== cache_bench: replay the reader trace (SLRU confirmation) ===="
	$(ROOT)/tools/cache_bench/cache_bench reader=$(ROOT)/tools/reader_vmem/reader_vmem.trace
	@echo ""
	@echo "==== cache_bench: block/frame-size sweep (#208 chunk-size knee) ===="
	$(ROOT)/tools/cache_bench/cache_bench --sweep-block
	@echo ""
	@echo "==== glyph_bench: glyph-cache budget sweep ===="
	$(ROOT)/tools/glyph_bench/glyph_bench
	@echo "==== bench-cache done ===="

# `make rabook-golden-update` -- regenerate the #151/#213 byte-identity parity
# fixtures from the desktop reference tools (needs Pillow), then clang-format them.
RABOOK_PARITY_SRC        ?= tests/fixtures/rabook_parity
RABOOK_PARITY_FIXTURE    ?= tests/rabook_parity_fixture.h
RABOOK_PARITY_M33        ?= examples/ek_ra8d2/hw_pending/compile_on_m33/parity_fixture.h
RABOOK_REALBOOK_SRC      ?= tests/fixtures/rabook_realbook
RABOOK_REALBOOK_FIXTURE  ?= tests/rabook_realbook_fixture.h
RABOOK_DOWNSCALE_FIXTURE ?= tests/rabook_downscale_parity_fixture.h
rabook-golden-update:
	python3 scripts/utils/rabook_parity_gen.py $(RABOOK_PARITY_SRC) $(RABOOK_PARITY_FIXTURE) $(RABOOK_PARITY_M33)
	python3 scripts/utils/rabook_parity_gen.py --realbook $(RABOOK_REALBOOK_SRC) $(RABOOK_REALBOOK_FIXTURE)
	python3 scripts/utils/rabook_parity_gen.py --downscale $(RABOOK_DOWNSCALE_FIXTURE)
	$(CLANG_FORMAT) -i $(RABOOK_PARITY_FIXTURE) $(RABOOK_PARITY_M33) $(RABOOK_REALBOOK_FIXTURE) $(RABOOK_DOWNSCALE_FIXTURE)
