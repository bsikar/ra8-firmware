# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

default:
    @echo "INFRA (REMOTE)"
    @echo ""
    @echo "  just infra::remote::reconcile_apply <ssh-host>   Restart the reconciler service"
    @echo "  just infra::remote::reconcile_status <ssh-host>  Show reconciler status and logs"
    @echo "  just infra::remote::logs <ssh-host>              Follow reconciler logs"
    @echo "  just infra::remote::status <ssh-host>            Show prepared host service state"
    @echo "  just infra::remote::dashboard                    Show runner and host health dashboard"

# Reconcile from the prepared infrastructure control host.
reconcile_apply host:
    #!/usr/bin/env bash
    set -euo pipefail
    echo "==> remote infra reconcile: {{ host }}"
    echo "==> starting ra8-fleet-reconcile.service; follow logs with:"
    echo "    just infra::remote::logs {{ host }}"
    ssh "{{ host }}" 'sudo systemctl restart ra8-fleet-reconcile.service'
    ssh "{{ host }}" 'sudo systemctl status ra8-fleet-reconcile.service --no-pager --full'

# Show the control host's reconciler state and recent result.
reconcile_status host:
    ssh "{{ host }}" 'systemctl status ra8-fleet-reconcile.service --no-pager --full; echo "== recent logs =="; sudo journalctl -u ra8-fleet-reconcile.service -n 40 --no-pager'

# Follow reconciliation logs live.
logs host:
    ssh "{{ host }}" 'sudo journalctl -fu ra8-fleet-reconcile.service'

# Show the prepared host's fleet service state.
status host:
    ssh "{{ host }}" 'systemctl is-active ra8-fleet-reconcile.service; sudo systemctl status ra8-fleet-reconcile.service --no-pager --full'

# Continuously display GitHub runner, ARC, host, and process health.
dashboard:
    #!/usr/bin/env bash
    set -euo pipefail
    command -v gh >/dev/null || { echo "error: gh is required" >&2; exit 1; }
    trap 'printf "\n"; exit 0' INT TERM
    data_dir="$(mktemp -d)"
    cleanup() { rm -rf -- "$data_dir"; }
    trap cleanup EXIT INT TERM
    arc_ssh_argv=()
    if arc_ssh_target="$(./.venv/bin/python scripts/dev/fleet.py ssh-target k3s-pve 2>/dev/null)"; then
        read -r -a arc_ssh_argv <<<"$arc_ssh_target"
    fi
    while :; do
        : >"$data_dir/jobs"
        : >"$data_dir/jobs.status"
        : >"$data_dir/checks"
        : >"$data_dir/arc"
        head_sha="$(gh run list --branch dev --limit 20 --json headSha,createdAt --jq 'sort_by(.createdAt) | last.headSha')"
        if [[ -z "$head_sha" ]]; then
            echo "error: could not determine the latest dev commit" >&2
            exit 1
        fi
        (
            runner_next="$data_dir/runners.next"
            if gh api --paginate repos/bsikar/ra8-firmware/actions/runners --jq \
                '.runners as $runners | ($runners | map(select(.name|startswith("ra8-ci-")) | .name) | sort) as $arc_names | $runners[] | . as $runner | [ $runner.name, (if ($runner.name|startswith("ra8-ci-")) then "ARC-" + ((( $arc_names | index($runner.name)) + 1) | tostring) elif ($runner.name|startswith("win-ci-")) then ($runner.name|.[0:24]) else ($runner.name|.[0:24]) end), $runner.status, (if $runner.status != "online" then "OFFLINE" elif $runner.busy then "BUSY" else "IDLE" end), (if ($runner.labels|any(.name == "hil")) then "HIL" elif ($runner.name|startswith("win-ci-")) then "WIN/WSL" elif ($runner.name|startswith("ra8-ci-")) then "ARC" else "OTHER" end), ($runner.labels|map(.name)|join(",")) ] | @tsv' \
                >"$runner_next" && [[ -s "$runner_next" ]]; then
                mv "$runner_next" "$data_dir/runners"
            elif [[ ! -s "$data_dir/runners" ]]; then
                echo "runner inventory unavailable; retrying" >"$data_dir/runners"
            fi
        ) &
        (
            set -o pipefail
            : >"$data_dir/active-runs"
            for state in queued in_progress; do
                gh api --paginate "repos/bsikar/ra8-firmware/actions/runs?status=$state&per_page=100" \
                    --jq '.workflow_runs[] | [.id, .name] | @tsv' >>"$data_dir/active-runs"
            done
            while IFS=$'\t' read -r run_id workflow; do
                [[ -n "$run_id" ]] || continue
                gh api --paginate "repos/bsikar/ra8-firmware/actions/runs/$run_id/jobs?per_page=100" \
                    --jq '.jobs[] | select((.status == "queued" or .status == "in_progress") and (.runner_name | type == "string") and (.runner_name | length > 0)) | [.runner_name, .name] | @tsv' \
                    | awk -F '\t' -v workflow="$workflow" '{ printf "%s\t%s / %s\n", $1, workflow, $2 }'
            done <"$data_dir/active-runs" >"$data_dir/jobs"
            echo ok >"$data_dir/jobs.status"
        ) >"$data_dir/jobs.log" 2>&1 || echo failed >"$data_dir/jobs.status" &
        gh api "repos/bsikar/ra8-firmware/commits/$head_sha/check-runs?per_page=100" \
            --jq '.check_runs[] | [.name,.status,(.conclusion // "-")] | @tsv' \
            >"$data_dir/checks" 2>&1 &
        if ((${#arc_ssh_argv[@]})); then
            "${arc_ssh_argv[@]}" \
                'sudo -n k3s kubectl get autoscalingrunnersets -n arc-runners ra8-ci -o custom-columns="MIN:.spec.minRunners,MAX:.spec.maxRunners,CURRENT:.status.currentRunners" --no-headers' \
                >"$data_dir/arc" 2>&1 &
        else
            echo "ARC capacity unavailable (fleet SSH target could not be resolved)" >"$data_dir/arc"
        fi
        wait || true
        arc_busy="$(awk -F '\t' '$5 == "ARC" && $4 == "BUSY" { busy++ } END { print busy + 0 }' "$data_dir/runners")"
        arc_capacity="$(awk '$1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ { printf "%s active / %s max", $3, $2 }' "$data_dir/arc")"
        [[ -n "$arc_capacity" ]] || arc_capacity="capacity unavailable"
        clear
        now="$(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo "RA8 CI RUNNER DASHBOARD    $now"
        echo "latest dev commit: ${head_sha:0:12}    refresh: 5s    exit: Ctrl-C"
        echo

        echo "== GitHub runners (ARC: ${arc_capacity}; ${arc_busy} busy) =="
        awk -F '\t' -v jobs_file="$data_dir/jobs" -v jobs_ok="$(cat "$data_dir/jobs.status")" '
            FILENAME == jobs_file { count[$1]++; job[$1] = $2; next }
            BEGIN { printf "%-26s %-10s %-10s %-8s %-42s %s\n", "NAME", "STATUS", "ASSIGNMENT", "SOURCE", "LABELS", "ACTIVE JOB" }
            { active = "-"; if ($3 == "online" && $4 == "BUSY") active = (jobs_ok == "ok" && count[$1] == 1) ? job[$1] : "UNKNOWN"; printf "%-26s %-10s %-10s %-8s %-42s %s\n", $2, $3, $4, $5, $6, active }' "$data_dir/jobs" "$data_dir/runners"
        echo

        echo "== latest commit checks =="
        awk -F '\t' '
            BEGIN { in_progress=queued=successful=skipped=failed=cancelled=0 }
            { if ($2 == "in_progress") in_progress++;
              else if ($2 == "queued") queued++;
              else if ($3 == "success") successful++;
              else if ($3 == "skipped") skipped++;
              else if ($3 == "failure") failed++;
              else if ($3 == "cancelled") cancelled++ }
            END { printf "%s in progress, %s successful, %s queued, %s skipped, %s failed, %s cancelled\n", in_progress, successful, queued, skipped, failed, cancelled }' "$data_dir/checks"
        echo
        awk -F '\t' 'BEGIN { printf "%-58s %-14s %-12s\n", "CHECK", "STATUS", "CONCLUSION" } { printf "%-58s %-14s %-12s\n", $1, $2, $3 }' "$data_dir/checks"
        sleep 5
    done
