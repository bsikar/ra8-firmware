# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

# Show check help menu
default:
    @echo "PRE-COMMIT VERIFICATION"
    @echo ""
    @echo "COMMANDS:"
    @echo "  just checks::local                           Run pre-commit checks natively (macOS/Linux)"
    @echo "  just checks::devcontainer                    Run pre-commit checks in the pinned devcontainer"
    @echo "  just checks::suppressions [format]           Inventory lint/policy suppressions for review"

# Emit the deterministic, deliberately non-authoritative suppression inventory.
# A successful inventory means report generation worked; it does not approve a
# waiver. `--check` remains nonzero until every unsupported family is parsed and

# every row has a reviewed disposition.
suppressions format="markdown":
    python3 scripts/checks/check_suppressions.py --selftest >&2
    python3 scripts/checks/check_suppressions.py --inventory --format "{{ format }}"

alias suppression-audit := suppressions

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
