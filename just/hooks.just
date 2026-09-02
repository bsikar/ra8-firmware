# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set working-directory := ".."

# Run the staged pre-commit policy through authoritative CI gate definitions
pre-commit:
    #!/bin/bash -p
    set -euo pipefail

    # The executable hook owns the only supported entry point. It mirrors the
    # active index into a clean private repository before this recipe is ever
    # parsed, so every whole-tree and --staged check rules on the same commit
    # candidate. Direct worktree invocation would reintroduce mixed views.
    [[ "${RA8_STAGED_HOOK_SNAPSHOT:-0}" == "1" ]] || {
        echo "[FAIL] pre-commit must run through scripts/git/pre-commit." >&2
        exit 1
    }
    git diff --quiet --no-ext-diff || { echo "[FAIL] snapshot worktree is dirty." >&2; exit 1; }
    [[ -z "$(git ls-files --others --exclude-standard)" ]] || \
        { echo "[FAIL] snapshot contains untracked paths." >&2; exit 1; }

    run_gate() {
        local gate="$1"
        "{{ just_executable() }}" quality::local::gate "$gate"
    }

    # These rules are intentionally index-aware. The full-tree CI counterparts
    # cannot replace them: a commit can add a compound decision or an MC/DC
    # marker while unrelated worktree edits mask what is actually staged.
    python3 scripts/checks/check_mcdc_block.py --staged
    python3 scripts/checks/check_new_compound_has_mcdc.py --staged
    python3 scripts/checks/check_obsolete_standards.py --staged

    # Gate bodies remain defined exactly once in scripts/ci/gates/. This list
    # records only which existing, hardware-free gates protect a commit.
    gates=(
        ascii
        copyright
        since
        format
        pre-commit-checks
        shebangs
        entry-points
        annotations
        doc-attachment
        toolchain-parity
        lint-py-shell
        lint-just
        cite-check
        hil-eil-parity
        roadmap-stats
        sbom
        soup-upstream
    )
    for gate in "${gates[@]}"; do
        run_gate "$gate"
    done

    # The old hook ran these whole-tree analyses whenever a production C/H file
    # was staged. Keep that trigger, but call the registered gate definitions
    # instead of copying their arguments and include policy into this file.
    staged_c=0
    while IFS= read -r -d '' path; do
        case "$path" in
            *.c | *.h)
                case "$path" in
                    */third_party/* | */build/* | */_deps/*) ;;
                    *) staged_c=1 ;;
                esac
                ;;
        esac
    done < <(git diff --cached --name-only --diff-filter=ACMR -z)
    if [[ "$staged_c" == "1" ]]; then
        run_gate tidy
        run_gate cppcheck
    fi

    # stack_usage_check.py --allow-empty was removed from pre-commit on
    # purpose: without build-cross output it can prove nothing. The full
    # pre-push `just ci` run builds the firmware before its stack-usage gate.

# Upload LFS objects, validate pushed commit metadata, then run the full CI suite
pre-push remote url:
    #!/bin/bash -p
    set -euo pipefail

    push_refs="$(cat)"

    if command -v git-lfs >/dev/null 2>&1; then
        printf '%s\n' "$push_refs" | git lfs pre-push "{{ remote }}" "{{ url }}"
    fi

    # The commit-msg hook owns the attribution/terminology grammar. Reuse it
    # for every commit entering the remote instead of maintaining a second
    # regex list here.
    /bin/bash -p scripts/git/commit-msg --selftest
    while read -r _local_ref local_oid _remote_ref remote_oid; do
        [[ -z "$local_oid" ]] && continue
        [[ "$local_oid" == "0000000000000000000000000000000000000000" ]] && continue
        if [[ "$remote_oid" == "0000000000000000000000000000000000000000" ]]; then
            commits="$(git rev-list "$local_oid" --not --remotes="{{ remote }}")"
            [[ -n "$commits" ]] || commits="$local_oid"
        else
            commits="$(git rev-list "$remote_oid..$local_oid")"
        fi
        while IFS= read -r sha; do
            [[ -n "$sha" ]] || continue
            message="$(mktemp)"
            git log -1 --format=%B "$sha" >"$message"
            identity="$(git log -1 --format='%an %ae %cn %ce' "$sha")"
            if ! COMMIT_IDENTITY="$identity" /bin/bash -p scripts/git/commit-msg "$message"; then
                rm -f "$message"
                echo "[FAIL] pre-push: commit $sha violates commit-message policy." >&2
                exit 1
            fi
            rm -f "$message"
        done <<<"$commits"
    done <<<"$push_refs"

    if [[ "${SKIP_CI_PUSH:-0}" == "1" ]]; then
        echo "[skip] pre-push: SKIP_CI_PUSH=1 -- NOT running 'just ci'." >&2
        exit 0
    fi

    echo "[pre-push] running 'just ci' (the complete registered CI suite)..." >&2
    echo "           emergency bypass: SKIP_CI_PUSH=1 git push" >&2
    set +e
    "{{ just_executable() }}" --justfile "$(git rev-parse --show-toplevel)/justfile" \
        --working-directory "$(git rev-parse --show-toplevel)" ci
    ci_rc=$?
    set -e
    if [[ "$ci_rc" -eq 3 ]]; then
        echo "[FAIL] pre-push: 'just ci' was ABORTED (exit 3 = UNKNOWN)." >&2
        echo "       No verdict was produced; rerun the push with the session open." >&2
        exit 1
    fi
    if [[ "$ci_rc" -ne 0 ]]; then
        echo "[FAIL] pre-push: 'just ci' failed -- push blocked." >&2
        echo "       Fix the red gates, or use SKIP_CI_PUSH=1 only for an emergency." >&2
        exit 1
    fi
