# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#

set working-directory := ".."

# Show examples help menu
default:
    @echo "EXAMPLES - Firmware Examples and Reference Code"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just apps::example::build <name|all>         Build a firmware example (or 'all')"
    @echo "  just apps::example::list                     List all firmware examples"
    @echo "  just apps::example::search <keyword>         Search firmware examples by keyword"
    @echo ""

# Build a firmware example (or 'all')
build app="all":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ "{{ app }}" = "all" ]; then
        bash scripts/builders/all_examples.sh
    else
        "{{ just_executable() }}" apps::build "{{ app }}"
    fi

# List all discovered examples
list:
    python3 scripts/dev/ra8_apps.py list --all

# Search examples by keyword
search keyword="":
    #!/usr/bin/env bash
    if [ -z "{{ keyword }}" ]; then
        echo "Error: Please specify a keyword to search."
        echo "Usage: just apps::example::search <keyword>"
        exit 1
    fi
    python3 scripts/dev/ra8_apps.py list "{{ keyword }}"

# Scaffold a new example application
new name="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "{{ name }}" ]; then
        echo "Error: Please specify a name for the new example."
        echo "Usage: just apps::example::new <name>"
        exit 1
    fi
    python3 scripts/dev/scaffold.py example "{{ name }}"
