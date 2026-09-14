# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "QUALITY & CI (REMOTE)"
    @echo ""
    @echo "  just quality::remote::gate <name> <ssh-host>"
    @echo ""
    @echo "Copies the current worktree (including local edits and non-ignored"
    @echo "untracked files) to an isolated temporary checkout on the SSH host"
    @echo "and runs the same registered local gate there."

gate name host:
    #!/usr/bin/env bash
    set -euo pipefail

    if [[ -z "{{ name }}" || -z "{{ host }}" ]]; then
        echo "usage: just quality::remote::gate <name> <ssh-host>" >&2
        exit 2
    fi

    min_free_kb=$((8 * 1024 * 1024))
    remote_free_kb="$(ssh "{{ host }}" "df -Pk / | awk 'NR==2 {print \$4}'")"
    if [[ ! "$remote_free_kb" =~ ^[0-9]+$ ]] || ((remote_free_kb < min_free_kb)); then
        echo "error: {{ host }} has ${remote_free_kb:-unknown} KiB free on /; " \
            "need at least $min_free_kb KiB before copying the worktree" >&2
        exit 1
    fi

    remote_root="$(ssh "{{ host }}" 'mktemp -d /tmp/ra8-quality-remote.XXXXXX')"
    cleanup() {
        ssh "{{ host }}" "rm -rf -- '$remote_root'" >/dev/null 2>&1 || true
    }
    trap cleanup EXIT

    file_count="$(git ls-files --cached --others --exclude-standard -z | tr '\0' '\n' | wc -l | tr -d ' ')"
    echo "==> remote gate target: {{ name }} on {{ host }}"
    echo "==> staging $file_count tracked/non-ignored files into $remote_root"
    echo "==> transfer started; heartbeat prints every 10 seconds"
    transfer_started="$(date +%s)"
    (
        git ls-files --cached --others --exclude-standard -z \
            | COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata \
                --null --files-from=- --create \
            | ssh "{{ host }}" "tar --no-xattrs --extract --directory='$remote_root'"
    ) &
    transfer_pid=$!
    while kill -0 "$transfer_pid" 2>/dev/null && [[ "$(ps -o stat= -p "$transfer_pid" 2>/dev/null)" != Z* ]]; do
        echo "    [remote gate] transfer active ($(( $(date +%s) - transfer_started ))s elapsed)"
        sleep 10
    done
    set +e
    wait "$transfer_pid"
    transfer_status=$?
    set -e
    if ((transfer_status != 0)); then
        echo "error: worktree transfer failed (exit $transfer_status)" >&2
        exit "$transfer_status"
    fi
    echo "==> transfer complete in $(( $(date +%s) - transfer_started ))s"

    echo "==> remote checkout ready; starting gate (heartbeat every 30 seconds)"
    gate_started="$(date +%s)"
    (
        ssh "{{ host }}" "cd '$remote_root' && git init -q && git add --all && just quality::local::gate '{{ name }}'"
    ) &
    gate_pid=$!
    while kill -0 "$gate_pid" 2>/dev/null && [[ "$(ps -o stat= -p "$gate_pid" 2>/dev/null)" != Z* ]]; do
        echo "    [remote gate] {{ name }} active ($(( $(date +%s) - gate_started ))s elapsed)"
        sleep 30
    done
    set +e
    wait "$gate_pid"
    gate_status=$?
    set -e
    echo "==> remote gate finished in $(( $(date +%s) - gate_started ))s (exit $gate_status)"
    exit "$gate_status"
