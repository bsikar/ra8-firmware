# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show board apps help menu
default:
    @echo "BOARD APPS - On-Target Firmware Applications"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::board::build <app|all>            Build a standalone board application"
    @echo "  just apps::board::clean <app|all>            Remove board application build artifacts"
    @echo "  just apps::board::list                       List all discovered board applications"
    @echo "  just apps::board::search <keyword>           Search board applications by keyword"
    @echo "  just apps::board::new <name>                 Scaffold a new board application"
    @echo ""

# List all standalone board applications
list:
    #!/usr/bin/env bash
    set -euo pipefail
    echo "BOARD APPS DISCOVERED:"
    for dir in apps/board/stand_alone/*; do
        if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
            app_name=$(basename "$dir")
            [ "$app_name" = "ereader" ] && app_name="ra8d2-ereader"
            echo "  - $app_name"
        fi
    done

# Search board applications by keyword
search keyword="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ keyword }}" ]; then
        echo "Error: Please specify a keyword to search."
        echo "Usage: just apps::board::search <keyword>"
        exit 1
    fi
    echo "SEARCH RESULTS IN BOARD APPS:"
    found=0
    for dir in apps/board/stand_alone/*; do
        if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
            app_name=$(basename "$dir")
            [ "$app_name" = "ereader" ] && app_name="ra8d2-ereader"
            if echo "$app_name" | grep -qi "{{ keyword }}"; then
                echo "  - $app_name"
                found=1
            fi
        fi
    done
    if [ "$found" -eq 0 ]; then
        echo "  (No matches found)"
    fi

# Build a standalone board application
build app="all":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ app }}" = "all" ]; then
        for dir in apps/board/stand_alone/*; do
            if [ -d "$dir" ] && [ -f "$dir/CMakeLists.txt" ]; then
                app_name=$(basename "$dir")
                # Special mapping for ereader
                [ "$app_name" = "ereader" ] && app_name="ra8d2-ereader"
                "{{ just_executable() }}" apps::build "$app_name"
            fi
        done
    else
        app_name="{{ app }}"
        [ "$app_name" = "ereader" ] && app_name="ra8d2-ereader"
        "{{ just_executable() }}" apps::build "$app_name"
    fi

# Clean board application build artifacts
clean app="all":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ app }}" = "all" ]; then
        rm -rf apps/board/stand_alone/*/build
    else
        rm -rf "apps/board/stand_alone/{{ app }}/build"
    fi

# Scaffold a new board application
new name="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ name }}" ]; then
        echo "Error: Please specify a name for the new board application."
        echo "Usage: just apps::board::new <name>"
        exit 1
    fi
    python3 scripts/dev/scaffold.py app "{{ name }}"
