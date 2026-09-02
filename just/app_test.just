# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "APP TESTS - Host Application Unit & Integration Tests"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::test::run <app|all>               Run unit tests for a host application"
    @echo "  just apps::test::integration                 Run integration tests combining tools"
    @echo "  just apps::test::clean <app|all>             Remove test build artifacts"
    @echo "  just apps::test::list                        List testable host applications"
    @echo "  just apps::test::search <keyword>            Search testable host applications"
    @echo ""

# List all testable host applications
list:
    #!/usr/bin/env bash
    set -euo pipefail
    echo "TESTABLE HOST APPS DISCOVERED:"
    for dir in apps/host/*; do
        if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
            echo "  - $(basename "$dir")"
        fi
    done

# Search testable host applications by keyword
search keyword="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ keyword }}" ]; then
        echo "Error: Please specify a keyword to search."
        echo "Usage: just apps::test::search <keyword>"
        exit 1
    fi
    echo "SEARCH RESULTS IN TESTABLE HOST APPS:"
    found=0
    for dir in apps/host/*; do
        if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
            app_name=$(basename "$dir")
            if echo "$app_name" | grep -qi "{{ keyword }}"; then
                echo "  - $app_name"
                found=1
            fi
        fi
    done
    if [ "$found" -eq 0 ]; then
        echo "  (No matches found)"
    fi

# Remove test build artifacts
clean app="all":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ app }}" = "all" ]; then
        rm -rf apps/host/*/build
    else
        rm -rf "apps/host/{{ app }}/build"
    fi

# Run tests for a host application (or 'all')
run app="all":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ app }}" = "all" ]; then
        for dir in apps/host/*; do
            if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
                echo "==> Testing $dir"
                bash scripts/builders/host_cmake.sh "$dir" "$dir/build"
                ctest --test-dir "$dir/build" --output-on-failure
            fi
        done
    else
        if [ ! -d "apps/host/{{ app }}" ]; then
            echo "Error: Host app '{{ app }}' not found."
            exit 1
        fi
        bash scripts/builders/host_cmake.sh "apps/host/{{ app }}" "apps/host/{{ app }}/build"
        ctest --test-dir "apps/host/{{ app }}/build" --output-on-failure
    fi

# Run integration tests combining mdl, mkbookimg, rabook_viewer, and ra8_reader
integration:
    bash scripts/builders/host_cmake.sh apps/host/mdl apps/host/mdl/build -DBUILD_INTEGRATION_TESTS=ON
    ctest --test-dir apps/host/mdl/build -R integration --output-on-failure
