# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show shared apps help menu
default:
    @echo "SHARED LIBS - App-Level Shared Libraries"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::shared::list                      List available shared libraries"
    @echo "  just apps::shared::build <lib|all>           Build a shared library (or 'all')"
    @echo "  just apps::shared::clean <lib|all>           Remove shared library build artifacts"
    @echo "  just apps::shared::new <name>                Scaffold a new shared library in apps/shared_libs"
    @echo "  just apps::shared::test <lib|all>            Run unit tests for a shared library"
    @echo ""

# Build a shared library (or 'all')
build lib="all":
    bash scripts/builders/build_shared_libs.sh "{{ lib }}"

# Clean shared library build artifacts
clean lib="all":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ lib }}" = "all" ]; then
        rm -rf apps/shared_libs/*/build
    else
        rm -rf "apps/shared_libs/{{ lib }}/build"
    fi

# Scaffold a new shared library
new name="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ name }}" ]; then
        echo "Error: Please specify a name for the new shared library."
        echo "Usage: just apps::shared::new <name>"
        exit 1
    fi
    python3 scripts/dev/scaffold.py shared_lib "{{ name }}"

# Run tests for a shared library (or 'all')
test lib="all":
    #!/usr/bin/env bash
    set -euo pipefail

    # We leverage the global firmware test suite for shared_libs because
    # they require the ra8_core_hal hardware mocks and unified coverage.
    "{{ just_executable() }}" tests::build

    if [ "{{ lib }}" = "all" ]; then
        echo "==> Testing all shared libraries..."
        bash tests/run_tests.sh -R "test_(ra8_)?(reflow|rabook|xml|css|svg|rar|unarch|shelf|manga|gfx|ui|widget|tile|zoom|longstrip|jpeg|webp|jof|img_arena|glyph|touch|vin|ov5640|mdl|app_media|epub|book|comic|format)"
    else
        if [ ! -d "apps/shared_libs/{{ lib }}" ]; then
            echo "Error: Shared lib '{{ lib }}' not found."
            exit 1
        fi
        echo "==> Testing apps/shared_libs/{{ lib }}"
        bash tests/run_tests.sh -R "test_.*{{ lib }}"
    fi

# List available shared libraries
list:
    #!/usr/bin/env bash
    echo "AVAILABLE SHARED LIBRARIES:"
    for dir in apps/shared_libs/*; do
        if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
            echo "  $(basename "$dir")"
        fi
    done
