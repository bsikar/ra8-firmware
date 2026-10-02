#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/lib/history.sh -- which repository the commit-message gates read,
# and which commit range they read from it.
#
# SOURCED, NEVER EXECUTED. Sourced by scripts/ci.sh the same way as
# parallelism.sh / tool_env.sh / arm_toolchain.sh, and used by the gate bodies
# in scripts/ci/gates/ (hygiene.sh's commit-message gates, manual.sh).
#
# It lives here rather than in scripts/ci.sh because ci.sh is the GATE REGISTRY
# and had grown two lines past the 1000-line cap that scripts/checks/
# check_file_size.py enforces (RA8FW-362). This family is the largest block in that
# file that is neither the registry, the single entry point, nor the gate-dir
# sourcing loop, and it has one subject, so it moved whole and unedited: the
# six function bodies below are byte-identical to the ones ci.sh carried, and
# every caller still calls them by the same names.
#
# The whole guarded block is idempotent so any number of scripts can source it.
if [ -z "${_RA8_HISTORY_SH:-}" ]; then
  _RA8_HISTORY_SH=1

  # The repository whose HISTORY the commit-message gates read.
  #
  # Normally the current directory -- but NOT under run_suite_on_snapshot, which
  # runs every gate inside a `git archive` snapshot that was turned into a repo
  # by `git init`. That snapshot holds exactly ONE synthetic commit
  # ("ci.sh snapshot of HEAD") and none of the host's objects, so a gate reading
  # history there sees no real commit message at all.
  #
  # Splitting the two sources is deliberate and is what makes snapshot mode
  # still mean something for these gates:
  #
  #   * the CHECKER SCRIPTS come from the snapshot (cwd), so the suite gates the
  #     committed HEAD's version of scripts/git/commit-msg and friends rather
  #     than whatever is dirty in the working tree;
  #   * the HISTORY comes from the host repo, because commit messages are git
  #     metadata that `git archive` cannot carry and no snapshot can synthesise.
  #
  # Exporting RA8_CI_COMMIT_RANGE into the snapshot instead (the other candidate
  # for the snapshot fix) does not work: the range names SHAs the snapshot's fresh object
  # store does not contain, so `git rev-list` dies with "Invalid revision range".
  # Making it work would mean importing the host object store into every
  # snapshot, which is precisely the independence run_suite_on_snapshot documents
  # it wants.
  ci_history_repo() {
    printf '%s\n' "${RA8_CI_HISTORY_REPO:-$PWD}"
  }

  # Run one read-only history query under the normal hostile-Git scrub while
  # trusting only the explicit repository selected above.  Container host mode
  # runs as root against a read-only, host-owned bind mount; the scrub rejects
  # every global/system config, so safe.directory must be part of this exact
  # process contract rather than inherited from the image or operator.
  ci_history_git() (
    local repo="$1" next git_bin
    local -a trust_env=()
    shift
    install_sanitized_git_environment
    next="$GIT_CONFIG_COUNT"
    git_bin="${RA8_TRUSTED_GIT-}"
    if [[ ! -x "$git_bin" ]]; then
      echo "ERROR: sanitized Git environment did not provide a trusted Git binary." >&2
      return 1
    fi
    trust_env=(
      "GIT_CONFIG_COUNT=$((next + 1))"
      "GIT_CONFIG_KEY_${next}=safe.directory"
      "GIT_CONFIG_VALUE_${next}=$repo"
    )
    env "${trust_env[@]}" "$git_bin" -C "$repo" "$@"
  )

  # Number of commits reachable from HEAD in the given repository.
  ci_history_depth() {
    ci_history_git "$1" rev-list --count HEAD 2>/dev/null || printf '0\n'
  }

  # Refuse to scan a repository that has no real history to scan.
  #
  # This is the guard. A commit-message gate pointed at the synthetic
  # one-commit snapshot reports PASS having read nothing but "ci.sh snapshot of
  # HEAD" -- the exact "gate that cannot see the thing it audits and says PASS"
  # CLAUDE.md bans. Fail loudly instead, the same way require_cmd does for an
  # absent tool.
  #
  # The invariant asserted here is history DEPTH (> 1 commit), not the span of
  # the resolved range, even though the snapshot fix was worded as the latter. A range spanning
  # exactly one commit is perfectly legal -- pushing a single commit produces
  # `HEAD~1..HEAD` -- so failing on a one-commit span would reject the most
  # ordinary push there is. What is never legal is the gate reading a repository
  # that HAS no history, and that is the condition that actually distinguishes
  # the snapshot from a real checkout. commit_range_selftest asserts both
  # directions of exactly this distinction.
  ci_require_real_history() {
    local repo="$1" depth
    depth="$(ci_history_depth "$repo")"
    if [[ "$depth" -le 1 ]]; then
      echo "ERROR: this gate reads commit messages, but the repository at" >&2
      echo "       '$repo' contains $depth commit(s) -- there is no real" >&2
      echo "       history here to scan." >&2
      echo "       This is the false-green: a synthetic 'git init'" >&2
      echo "       snapshot has one commit, so the gate would report PASS" >&2
      echo "       having read no real commit message at all." >&2
      echo "       Under the suite runner, RA8_CI_HISTORY_REPO must point at" >&2
      echo "       a real-history repository; otherwise the snapshot is used." >&2
      return 1
    fi
  }

  # The commit range a message-scanning gate should cover.
  #
  # Derived from the GitHub event payload when running on a runner, so the range
  # logic lives here instead of being duplicated as inline expression bash in two
  # workflows. Falls back to the local upstream..HEAD, then HEAD~1..HEAD. A
  # before..head range never rots the way a hardcoded floor SHA does: a history
  # rewrite orphans the floor and silently empties the range, turning the gate
  # into a no-op.
  #
  # Every git query resolves against ci_history_repo(), not the cwd, so the range
  # describes the same repository the gates go on to read.
  ci_commit_range() {
    if [[ -n "${RA8_CI_COMMIT_RANGE:-}" ]]; then
      printf '%s\n' "$RA8_CI_COMMIT_RANGE"
      return 0
    fi

    local repo head="" base=""
    repo="$(ci_history_repo)"
    if [[ -n "${GITHUB_EVENT_PATH:-}" && -f "${GITHUB_EVENT_PATH}" ]]; then
      head="$(python3 -c '
import json, os
ev = json.load(open(os.environ["GITHUB_EVENT_PATH"]))
pr = ev.get("pull_request") or {}
print((pr.get("head") or {}).get("sha") or os.environ.get("GITHUB_SHA") or "")
' 2>/dev/null || true)"
      base="$(python3 -c '
import json, os
ev = json.load(open(os.environ["GITHUB_EVENT_PATH"]))
pr = ev.get("pull_request") or {}
print((pr.get("base") or {}).get("sha") or ev.get("before") or "")
' 2>/dev/null || true)"
    fi
    [[ -z "$head" ]] && head="${GITHUB_SHA:-HEAD}"

    # A base absent locally (force-push, shallow clone, the all-zero "new
    # branch" sentinel) is unusable -- fall back rather than error out.
    if [[ -z "$base" ]] || ! ci_history_git "$repo" cat-file -e "${base}^{commit}" 2>/dev/null; then
      base="$(ci_history_git "$repo" rev-parse --verify --quiet '@{upstream}' 2>/dev/null)" || base=""
    fi
    # After a FORCE push the event's before-sha was rewritten out of existence,
    # and the @{upstream} fallback resolves to the freshly-pushed head itself --
    # a head..head range spanning nothing, which ci_report_commit_range then
    # rejects. A push event always carries at least its tip commit, so drop the
    # degenerate base and let the head~1 fallback scan that one commit instead.
    # workflow_dispatch deliberately keeps base == head: a manual re-run has
    # nothing new to scan, and rejecting that vacuity is the whole point.
    if [[ "${GITHUB_EVENT_NAME:-}" == "push" && -n "$base" ]] &&
      [[ "$(ci_history_git "$repo" rev-parse --verify --quiet "${base}^{commit}" 2>/dev/null)" == "$(ci_history_git "$repo" rev-parse --verify --quiet "${head}^{commit}" 2>/dev/null)" ]]; then
      base=""
    fi
    if [[ -z "$base" ]] || ! ci_history_git "$repo" cat-file -e "${base}^{commit}" 2>/dev/null; then
      base="$(ci_history_git "$repo" rev-parse --verify --quiet "${head}~1" 2>/dev/null)" || base=""
    fi

    if [[ -n "$base" ]]; then
      printf '%s..%s\n' "$base" "$head"
    else
      printf '%s\n' "$head"
    fi
  }

  # Announce the resolved range a message-scanning gate is about to cover, WITH
  # the commit count, and reject the vacuous zero-commit case (#357 Path 2).
  #
  # On workflow_dispatch a manual re-run resolves the range to head..head -- the
  # checked-out branch's upstream equals its head, so base == head and the range
  # spans zero commits. The gate would then scan nothing and report PASS, so a
  # "green" from a hand re-run means "examined nothing", not "history is clean".
  # A PR always supplies a real base, and a push resolves to at least head~1..head
  # (ci_commit_range drops a base that degenerated to the head after a force
  # push), so a zero-commit range is only ever this dispatch degeneracy. Print
  # the count in EVERY case so a run that examined commits and found nothing is
  # visibly distinct from one that examined none, and FAIL loudly on the zero
  # case rather than passing vacuously -- the same fail-on-missing-input
  # discipline require_cmd applies to an absent tool.
  ci_report_commit_range() {
    local repo="$1" range="$2" count
    count="$(ci_history_git "$repo" rev-list --count "$range" 2>/dev/null || printf '0')"
    echo "Scanning $count commit message(s) in: $range (history repo: $repo)"
    if [[ "${count:-0}" -eq 0 ]]; then
      echo "::error::commit-metadata gate examined 0 commits -- range '$range'" >&2
      echo "       is empty (base resolved equal to head). This is the" >&2
      echo "       workflow_dispatch head..head vacuity: a manual re-run" >&2
      echo "       has nothing new to scan, so a green here would mean 'examined" >&2
      echo "       nothing', not 'history is clean'. Failing loudly instead." >&2
      return 1
    fi
    return 0
  }
fi
