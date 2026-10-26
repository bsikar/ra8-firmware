# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

# Show check help menu
default:
    @echo "PRE-COMMIT VERIFICATION"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just checks::local                  Run pre-commit checks natively (macOS/Linux)"

# Local pre-commit verification natively (macOS/Linux)
local:
    "{{ just_executable() }}" zig::test
    bash tests/run_tests.sh

# RA8FW-618 slice 2: enforce the portable rows in libs/README.md
portability:
    python3 tools/check_portability.py --self-test
    python3 tools/check_portability.py
