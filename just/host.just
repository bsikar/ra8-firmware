# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show host apps help menu
default:
    @echo "HOST APPS - Standalone Desktop Applications"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::host::build <app|all>             Build a host application (or 'all')"
    @echo "  just apps::host::clean <app|all>             Remove host application build artifacts"
    @echo "  just apps::host::new <name>                  Scaffold a new host application"
    @echo "  just apps::host::run <app> args=\"\"           Run a host application CLI"
    @echo "  just apps::host::list                        List all discovered host applications"
    @echo "  just apps::host::search <keyword>            Search host applications by keyword"
    @echo ""

# List all standalone host applications
list:
    #!/usr/bin/env bash
    set -euo pipefail
    echo "HOST APPS DISCOVERED:"
    for dir in apps/host/*; do
        if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
            echo "  - $(basename "$dir")"
        fi
    done

# Search host applications by keyword
search keyword="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ keyword }}" ]; then
        echo "Error: Please specify a keyword to search."
        echo "Usage: just apps::host::search <keyword>"
        exit 1
    fi
    echo "SEARCH RESULTS IN HOST APPS:"
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

# Remove host application build artifacts
clean app="all":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ app }}" = "all" ]; then
        rm -rf apps/host/*/build
    else
        rm -rf "apps/host/{{ app }}/build"
    fi

# Build a host application (or 'all')
build app="all":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ app }}" = "all" ]; then
        for dir in apps/host/*; do
            if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
                echo "==> Building $dir"
                bash scripts/builders/host_cmake.sh "$dir" "$dir/build"
            fi
        done
    else
        if [ ! -d "apps/host/{{ app }}" ]; then
            echo "Error: Host app '{{ app }}' not found."
            exit 1
        fi
        bash scripts/builders/host_cmake.sh "apps/host/{{ app }}" "apps/host/{{ app }}/build"
    fi

# Run a host application with arguments
run app args="":
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" apps::host::build "{{ app }}"
    "apps/host/{{ app }}/build/{{ app }}" {{ args }}

# Scaffold a new host application
new name="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ name }}" ]; then
        echo "Error: Please specify a name for the new host application."
        echo "Usage: just apps::host::new <name>"
        exit 1
    fi
    python3 scripts/dev/scaffold.py host "{{ name }}"
