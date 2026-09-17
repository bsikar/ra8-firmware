# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "QUALITY & CI (REMOTE)"
    @echo ""
    @echo "  just quality::remote::gate <name> <ssh-host>  Run one registered gate remotely"
    @echo "  just quality::remote::run <ssh-host>          Run the full CI suite remotely"
    @echo ""
    @echo "Copies the current worktree (including local edits and non-ignored"
    @echo "untracked files) to an isolated temporary checkout on the SSH host"
    @echo "and runs the same registered gate or full suite there."

run host:
    #!/usr/bin/env bash
    set -euo pipefail
    "{{ just_executable() }}" quality::remote::gate __full__ {{ quote(host) }}

gate name host:
    #!/usr/bin/env bash
    set -euo pipefail

    remote_name={{ quote(name) }}
    remote_host={{ quote(host) }}
    if [[ -z "$remote_name" || -z "$remote_host" ]]; then
        echo "usage: just quality::remote::gate <name> <ssh-host>" >&2
        exit 2
    fi

    remote_shell="$(./.venv/bin/python3 scripts/dev/fleet.py remote-shell "$remote_host")"
    read -r -a remote_ssh <<<"$(./.venv/bin/python3 scripts/dev/fleet.py ssh-target "$remote_host")"
    remote_tar="${remote_shell%/bin/bash -s}ionice -c3 /usr/bin/tar"
    printf -v remote_gate_arg '%q' "$remote_name"
    remote_history_message="$(git log -1 --format=%B HEAD)"
    printf -v remote_history_message '%q' "$remote_history_message"
    printf -v remote_history_environment \
        'GIT_AUTHOR_NAME=%q GIT_AUTHOR_EMAIL=%q GIT_COMMITTER_NAME=%q GIT_COMMITTER_EMAIL=%q' \
        "$(git log -1 --format=%an HEAD)" \
        "$(git log -1 --format=%ae HEAD)" \
        "$(git log -1 --format=%cn HEAD)" \
        "$(git log -1 --format=%ce HEAD)"
    remote_prepare="git init -q && git add --all && git -c user.email=ci@localhost -c user.name=ci commit --quiet --no-verify -m 'remote CI transport snapshot' && $remote_history_environment git commit --quiet --allow-empty --no-verify -m $remote_history_message"

    if [[ "$remote_shell" == wsl* ]]; then
        # win-ci terminates SSH in Windows and then starts this root, empty-
        # environment WSL shell.  It must explicitly enter the dev slice: its
        # profile supplies the container cgroup parent and parallelism cap, and
        # ra8-dev puts the host-side Docker client below the CI runners.
        remote_profile="/etc/profile.d/ra8-dev-slice.sh"
        remote_launcher="/usr/local/bin/ra8-dev"
        remote_preflight="test -r $remote_profile && test -x $remote_launcher && command -v docker >/dev/null && command -v ionice >/dev/null"
        if [[ "$remote_name" == "__full__" ]]; then
            gate_label="full CI suite"
            gate_command="/bin/bash -p scripts/ci.sh"
        else
            gate_label="$remote_name"
            gate_command="/bin/bash -p scripts/ci.sh --gate $remote_gate_arg --container"
        fi
    else
        # A plain Linux verification box has no dev slice.  Keep its archive
        # extraction and gate processes at idle I/O priority and low CPU
        # priority, and bound a suite to two build jobs by default.
        remote_profile=""
        remote_jobs="${RA8_REMOTE_MAX_JOBS:-2}"
        if [[ ! "$remote_jobs" =~ ^[1-9][0-9]*$ ]]; then
            echo "error: RA8_REMOTE_MAX_JOBS must be a positive decimal integer" >&2
            exit 2
        fi
        remote_launcher="env RA8_MAX_JOBS=$remote_jobs CMAKE_BUILD_PARALLEL_LEVEL=$remote_jobs nice -n 19 ionice -c3"
        remote_preflight="command -v ionice >/dev/null"
        if [[ "$remote_name" == "__full__" ]]; then
            gate_label="full CI suite"
            gate_command="/bin/bash -p scripts/ci.sh --native"
        else
            gate_label="$remote_name"
            gate_command="/bin/bash -p scripts/ci.sh --gate $remote_gate_arg"
        fi
    fi

    if ! printf '%s\n' "$remote_preflight" | "${remote_ssh[@]}" "$remote_shell"; then
        echo "error: $remote_host is not provisioned for isolated remote CI" >&2
        if [[ "$remote_shell" == wsl* ]]; then
            echo "       expected $remote_profile, $remote_launcher, Docker, and ionice in WSL" >&2
        else
            echo "       expected ionice on the Linux target" >&2
        fi
        exit 1
    fi

    min_free_kb=$((8 * 1024 * 1024))
    read -r _ _ _ remote_free_kb _ _ < <(
        printf 'df -Pk /\n' | "${remote_ssh[@]}" "$remote_shell" | tail -n 1
    )
    if [[ ! "$remote_free_kb" =~ ^[0-9]+$ ]] || ((remote_free_kb < min_free_kb)); then
            echo "error: $remote_host has ${remote_free_kb:-unknown} KiB free on /; " \
            "need at least $min_free_kb KiB before copying the worktree" >&2
        exit 1
    fi

    manifest_file="$(mktemp /tmp/ra8-quality-remote-manifest.XXXXXX)"
    git ls-files --cached --others --exclude-standard -z >"$manifest_file"
    remote_root="$(printf 'mktemp -d /tmp/ra8-quality-remote.XXXXXX\n' | "${remote_ssh[@]}" "$remote_shell")"
    stop_job() {
        local pid="$1"
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            pkill -TERM -P "$pid" 2>/dev/null || true
            kill "$pid" 2>/dev/null || true
        fi
    }
    cleanup_remote() {
        local pid
        for pid in "${transfer_pid:-}" "${gate_pid:-}"; do
            stop_job "$pid"
        done
        if printf 'rm -rf -- %q\n! test -e %q\n' "$remote_root" "$remote_root" | \
            "${remote_ssh[@]}" "$remote_shell" >/dev/null 2>&1; then
            echo "==> remote cleanup complete: $remote_root"
        else
            echo "warning: could not verify remote cleanup: $remote_root" >&2
        fi
        rm -f -- "$manifest_file"
    }
    trap cleanup_remote EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    file_count="$(git ls-files --cached --others --exclude-standard -z | tr '\0' '\n' | wc -l | tr -d ' ')"
    echo "==> remote gate target: $gate_label on $remote_host"
    echo "==> staging $file_count tracked/non-ignored files into $remote_root"
    echo "==> transfer started; heartbeat prints every 10 seconds"
    transfer_started="$(date +%s)"
    local_tar=(nice -n 19 tar)
    if command -v ionice >/dev/null 2>&1; then
        local_tar=(ionice -c3 nice -n 19 tar)
    fi
    (
        python3 -c 'import sys; paths=set(path for path in sys.stdin.buffer.read().split(b"\0") if path); paths.update(b"/".join(path.split(b"/")[:index]) for path in tuple(paths) for index in range(1,len(path.split(b"/")))); sys.stdout.buffer.write(b"\0".join(sorted(paths))+b"\0")' <"$manifest_file" \
            | COPYFILE_DISABLE=1 "${local_tar[@]}" --no-xattrs --no-mac-metadata \
                --null --files-from=- --create --no-recursion \
            | "${remote_ssh[@]}" "$remote_tar --no-xattrs --extract --directory=$remote_root"
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
        {
            printf 'cd %q\n' "$remote_root"
            printf '%s\n' "$remote_prepare"
            if [[ -n "$remote_profile" ]]; then
                printf 'source %q\n' "$remote_profile"
            fi
            printf 'exec %s %s\n' "$remote_launcher" "$gate_command"
        } | "${remote_ssh[@]}" "$remote_shell"
    ) &
    gate_pid=$!
    while kill -0 "$gate_pid" 2>/dev/null && [[ "$(ps -o stat= -p "$gate_pid" 2>/dev/null)" != Z* ]]; do
        echo "    [remote gate] $gate_label active ($(( $(date +%s) - gate_started ))s elapsed)"
        sleep 30
    done
    set +e
    wait "$gate_pid"
    gate_status=$?
    set -e
    echo "==> remote gate finished in $(( $(date +%s) - gate_started ))s (exit $gate_status)"
    exit "$gate_status"
