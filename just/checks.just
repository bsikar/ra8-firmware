# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

# Show check help menu
default:
    @echo "PRE-COMMIT VERIFICATION"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just checks::local                  Run pre-commit checks natively (macOS/Linux)"
    @echo "  just checks::devcontainer           Run pre-commit checks in the pinned devcontainer"

# Local pre-commit verification natively (macOS/Linux)
local:
    "{{ just_executable() }}" quality::local::gate format
    "{{ just_executable() }}" quality::local::gate tidy
    "{{ just_executable() }}" quality::local::gate unit-tests

# Focused pre-commit verification against the current writable tree. Gate
# bodies stay in scripts/ci.sh; this recipe only selects the three developer

# feedback gates and the helper owns all container transport.
devcontainer:
    bash scripts/ci/devcontainer_run.sh -- bash -uc 'just quality::local::gate format && just quality::local::gate tidy && just quality::local::gate unit-tests'
