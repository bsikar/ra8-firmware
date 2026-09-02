# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show library help menu
default:
    @echo "LIB - Firmware Shared Libraries"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just libs::list                              List all discovered shared libraries"
    @echo "  just libs::search <keyword>                  Search shared libraries by keyword"
    @echo "  just libs::new <name>                        Scaffold a new shared library"
    @echo ""

# Scaffold a new shared library
new name="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ name }}" ]; then
        echo "Error: Please specify a name for the new library."
        echo "Usage: just libs::new <name>"
        exit 1
    fi
    python3 scripts/dev/scaffold.py lib "{{ name }}"

# List all shared libraries
list:
    python3 scripts/dev/list_libs.py

# Search shared libraries by keyword
search keyword="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ keyword }}" ]; then
        echo "Error: Please specify a keyword to search."
        echo "Usage: just libs::search <keyword>"
        exit 1
    fi
    python3 scripts/dev/list_libs.py "{{ keyword }}"
