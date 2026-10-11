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
