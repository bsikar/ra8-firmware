# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

# Show tools help menu
default:
    @echo "TOOLS - Developer Utilities & Desktop Apps"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just tools::list                             List discovered compiled host tools"
    @echo "  just tools::build                            Build all host developer tools"
    @echo "  just tools::build_one <tool>                 Build one compiled host tool"
    @echo "  just tools::clean                            Remove all tool build artifacts"
    @echo "  just tools::viewer                           Build native macOS rabook_viewer GUI"
    @echo "  just tools::view <path>                      Open a .rabook file in the viewer"
    @echo "  just tools::mcp                              Run Model Context Protocol server selftest"
    @echo "  just tools::mcp_server                       Start the Model Context Protocol server"
    @echo "  just tools::jlink_w4 <bin> <base> [device]   Emit, but do not run, a J-Link w4 script"
    @echo "  just tools::books                            Recompile EPUB library into .rabook files"
    @echo "  just tools::rabook_golden                    Regenerate rabook parity fixtures"
    @echo "  just tools::rabook_fixture                   Regenerate the gray8 rabook fixture"
    @echo "  just tools::bluenoise_update                 Regenerate the blue-noise dither mask"
    @echo "  just tools::bench_cache                      Run full memory/glyph cache benchmark sweep"
    @echo "  just tools::build_cache_bench                Build cache_bench standalone executable"
    @echo "  just tools::build_reader_vmem                Build reader_vmem standalone executable"
    @echo "  just tools::build_glyph_bench                Build glyph_bench standalone executable"
    @echo ""

# Remove all tool build artifacts
clean tool="all":
    bash scripts/builders/build_host_tools.sh clean "{{ tool }}"

# List every discovery-managed compiled host tool
list:
    bash scripts/builders/build_host_tools.sh list

# Build every discovered host developer tool; any missing dependency fails loud
build tool="all":
    bash scripts/builders/build_host_tools.sh build "{{ tool }}"

# Build one discovered host developer tool
build_one tool:
    bash scripts/builders/build_host_tools.sh build "{{ tool }}"

alias build-one := build_one

# Build the native macOS/POSIX document viewer
viewer:
    bash scripts/builders/host_cmake.sh tools/rabook_viewer tools/rabook_viewer/build -DCMAKE_BUILD_TYPE=Release

# Build and open a .rabook document in the viewer (use arrows to turn pages)
view file headless="0": viewer
    #!/usr/bin/env bash
    set -euo pipefail
    extra=()
    [ "{{ headless }}" = "1" ] && extra=(--headless --dump-ppm /tmp/ra8_view.ppm)
    tools/rabook_viewer/build/rabook_viewer "{{ file }}" "${extra[@]}"

# Self-test the Model Context Protocol dev server (tools/mcp)
mcp:
    python3 tools/mcp/src/ra8_mcp.py --selftest

# Start the Model Context Protocol development server.
mcp_server:
    python3 tools/mcp/src/ra8_mcp.py

# Emit a J-Link commander script to stdout; this recipe never executes J-Link.
jlink_w4 binary base_address device="R7KA8D2KF_CPU0":
    python3 scripts/gen/gen_jlink_w4.py \
        {{ quote(binary) }} \
        {{ quote(base_address) }} \
        --device {{ quote(device) }}

# Recompile the e-reader content library *.epub -> *.rabook
books:
    bash scripts/builders/books.sh

# Regenerate the host/device rabook byte-parity fixtures and format the headers
rabook_golden:
    #!/usr/bin/env bash
    set -euo pipefail
    formatter="${CLANG_FORMAT:-}"
    if [ -z "$formatter" ]; then
        if command -v clang-format-22 >/dev/null 2>&1; then
            formatter="$(command -v clang-format-22)"
        elif command -v clang-format >/dev/null 2>&1; then
            formatter="$(command -v clang-format)"
        fi
    fi
    if [ -z "$formatter" ]; then
        echo "rabook_golden: clang-format-22 or clang-format is required" >&2
        exit 127
    fi
    fixture_dir="apps/shared_libs/rabook_compile/tests/inc"
    python3 scripts/gen/rabook_parity_gen.py \
        tests/fixtures/rabook_parity \
        "$fixture_dir/rabook_parity_fixture.h" \
        examples/ek_ra8d2/hw_pending/compile_on_m33/inc/parity_fixture.h
    python3 scripts/gen/rabook_parity_gen.py --realbook \
        tests/fixtures/rabook_realbook \
        "$fixture_dir/rabook_realbook_fixture.h"
    python3 scripts/gen/rabook_parity_gen.py --downscale \
        "$fixture_dir/rabook_downscale_parity_fixture.h"
    python3 scripts/gen/rabook_parity_gen.py --color \
        "$fixture_dir/rabook_color_parity_fixture.h"
    "$formatter" -i \
        "$fixture_dir/rabook_parity_fixture.h" \
        examples/ek_ra8d2/hw_pending/compile_on_m33/inc/parity_fixture.h \
        "$fixture_dir/rabook_realbook_fixture.h" \
        "$fixture_dir/rabook_downscale_parity_fixture.h" \
        "$fixture_dir/rabook_color_parity_fixture.h"
    python3 scripts/checks/check_comment_format.py --fix \
        "$fixture_dir/rabook_parity_fixture.h" \
        examples/ek_ra8d2/hw_pending/compile_on_m33/inc/parity_fixture.h \
        "$fixture_dir/rabook_realbook_fixture.h" \
        "$fixture_dir/rabook_downscale_parity_fixture.h" \
        "$fixture_dir/rabook_color_parity_fixture.h" >/dev/null

# Regenerate the ereader_rabook full-resolution gray8 fixture
rabook_fixture:
    #!/usr/bin/env bash
    set -euo pipefail
    formatter="${CLANG_FORMAT:-}"
    if [ -z "$formatter" ]; then
        if command -v clang-format-22 >/dev/null 2>&1; then
            formatter="$(command -v clang-format-22)"
        elif command -v clang-format >/dev/null 2>&1; then
            formatter="$(command -v clang-format)"
        fi
    fi
    if [ -z "$formatter" ]; then
        echo "rabook_fixture: clang-format-22 or clang-format is required" >&2
        exit 127
    fi
    fixture="examples/ek_ra8d2/hw_validated/hil/ereader_rabook/inc/rabook_gray8_fixture.h"
    python3 scripts/gen/rabook_gray8_fixture.py
    "$formatter" -i "$fixture"

# Regenerate the committed blue-noise dither threshold mask
bluenoise_update:
    #!/usr/bin/env bash
    set -euo pipefail
    formatter="${CLANG_FORMAT:-}"
    if [ -z "$formatter" ]; then
        if command -v clang-format-22 >/dev/null 2>&1; then
            formatter="$(command -v clang-format-22)"
        elif command -v clang-format >/dev/null 2>&1; then
            formatter="$(command -v clang-format)"
        fi
    fi
    if [ -z "$formatter" ]; then
        echo "bluenoise_update: clang-format-22 or clang-format is required" >&2
        exit 127
    fi
    mask="libs/ra8_gfx/src/ra8_gfx_dither_mask_internal.h"
    python3 scripts/gen/gen_bluenoise_mask.py
    "$formatter" -i "$mask"

# Build cache_bench host executable
build_cache_bench:
    "{{ just_executable() }}" tools::build_one cache_bench

alias build-cache-bench := build_cache_bench

# Build reader_vmem host executable
build_reader_vmem:
    "{{ just_executable() }}" tools::build_one reader_vmem

alias build-reader-vmem := build_reader_vmem

# Build glyph_bench host executable
build_glyph_bench:
    "{{ just_executable() }}" tools::build_one glyph_bench

alias build-glyph-bench := build_glyph_bench

# Run the full cache benchmark toolchain (reader_vmem, cache_bench, glyph_bench)
bench_cache: build_cache_bench build_reader_vmem build_glyph_bench
    #!/usr/bin/env bash
    set -euo pipefail
    echo "==== reader_vmem: drive real ra8_vmem, emit trace ===="
    tools/reader_vmem/build/reader_vmem tools/reader_vmem/build/reader_vmem.trace
    echo "==== cache_bench: replay reader trace (SLRU) ===="
    tools/cache_bench/build/cache_bench reader=tools/reader_vmem/build/reader_vmem.trace
    echo "==== cache_bench: block/frame-size sweep ===="
    tools/cache_bench/build/cache_bench --sweep-block
    echo "==== glyph_bench: glyph-cache budget sweep ===="
    tools/glyph_bench/build/glyph_bench

alias bench-cache := bench_cache
