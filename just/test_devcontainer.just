# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default name="":
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -z "{{ name }}" ]]; then
        echo "Error: Please specify a test name or regex (or 'all')." >&2
        echo "Usage: just tests::devcontainer <name|all>" >&2
        exit 2
    fi

    selector="{{ name }}"
    selector="${selector##*::}"
    if [[ "$selector" == "board" ]]; then
        echo "==> Board apps are tested via the remote HIL suite."
        "{{ just_executable() }}" hil::run
        exit $?
    fi

    # test_local.just remains the one selector/category implementation. Inside
    # Linux it is genuinely local; this wrapper supplies only the container.
    bash scripts/ci/devcontainer_run.sh -- just tests::local "{{ name }}"
