# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "CI GATES - Operations"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just quality::gate::list                     List all available CI gates"
    @echo "  just quality::gate::run <name>               Run one gate in the supported host environment"

list:
    #!/usr/bin/env python3
    import subprocess
    proc = subprocess.run(
        ["/bin/bash", "-p", "scripts/ci.sh", "--list-gates"],
        capture_output=True,
        text=True,
        check=True,
    )
    for line in proc.stdout.strip().splitlines():
        parts = line.split("\t")
        if len(parts) == 3:
            print(f"{parts[0]:<30} {parts[1]:<7} {parts[2]}")
        else:
            print(line)

# Run one registered gate against the current working tree. macOS needs the
# pinned Linux devcontainer for the CI toolchain and low-address host tests;

# Linux can execute the exact gate natively.
run name:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "$(uname -s)" == "Darwin" ]]; then
        "{{ just_executable() }}" quality::devcontainer::gate "{{ name }}"
    else
        "{{ just_executable() }}" quality::local::gate "{{ name }}"
    fi
