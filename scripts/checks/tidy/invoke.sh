#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/checks/tidy/invoke.sh -- Running clang-tidy over one pass's file list.
#
# SOURCED, NEVER EXECUTED. See collect.sh for the loading contract.
#
# This is the only place a clang-tidy process is actually spawned. Every pass
# funnels through invoke_clang_tidy, which is what makes "0 files routed here"
# impossible to confuse with "this pass came back clean" -- an empty bucket
# says so out loud, once, in one place.
#
# Functions here: detect_jobs, split_tidy_args, invoke_clang_tidy,
# run_tidy_chunks

# ---------------------------------------------------------------------------
# Number of parallel clang-tidy processes -- the bounded canonical width
# (#328). clang_tidy.sh sources scripts/ci/lib/parallelism.sh before this
# helper, so ra8_max_jobs caps the fan-out (RA8_MAX_JOBS /
# CMAKE_BUILD_PARALLEL_LEVEL / host core count) instead of one process per core.
# ---------------------------------------------------------------------------
detect_jobs() {
  ra8_max_jobs
}

# ---------------------------------------------------------------------------
# Split the argument vector at the literal `--` separator.
#
# clang-tidy's own options come before it and the fixed compile command after,
# with the file list spliced in between -- the order clang-tidy requires. The
# result is published in the caller-visible globals below rather than returned,
# because a bash function can only return a status.
# ---------------------------------------------------------------------------
TIDY_ARG_LEAD=()
TIDY_ARG_TRAIL=()
TIDY_ARG_SEEN_SEP=false

split_tidy_args() {
  TIDY_ARG_LEAD=()
  TIDY_ARG_TRAIL=()
  TIDY_ARG_SEEN_SEP=false
  local arg
  for arg in "$@"; do
    if [[ "$arg" == "--" && "$TIDY_ARG_SEEN_SEP" == "false" ]]; then
      TIDY_ARG_SEEN_SEP=true
      continue
    fi
    if [[ "$TIDY_ARG_SEEN_SEP" == "true" ]]; then
      TIDY_ARG_TRAIL+=("$arg")
    else
      TIDY_ARG_LEAD+=("$arg")
    fi
  done
}

# ---------------------------------------------------------------------------
# Invoke clang-tidy over a file list, one process per file, `detect_jobs` at a
# time. Each process buffers its diagnostics to its own file so the parallel
# output never interleaves; the buffers are concatenated in list order after
# the batch drains. Returns non-zero if any file reported a violation.
#
# $1  clang-tidy binary
# $2  human-readable pass label (progress output only)
# $3  newline-separated file list
# $4+ arguments common to every invocation. A literal `--` splits clang-tidy's
#     own options (before) from the fixed compile command (after); the file
#     list is spliced in between, which is the order clang-tidy requires.
# ---------------------------------------------------------------------------
invoke_clang_tidy() {
  local clang_tidy="$1"
  local label="$2"
  local list="$3"
  shift 3

  split_tidy_args "$@"

  local count
  count=$(wc -l <"$list" | tr -d ' ')
  if [[ "$count" -eq 0 ]]; then
    # Say it out loud. An empty bucket printing nothing is indistinguishable
    # from a clean one, which is how a run over a fraction of the tree read as
    # a pass. The caller decides whether empty is legitimate (Objective-C off
    # macOS) or a bug; this just refuses to be silent about it.
    print_warning "  pass '$label': 0 file(s) -- nothing routed to this pass"
    return 0
  fi

  local nj
  nj="$(detect_jobs)"
  [[ "$nj" -lt 1 ]] && nj=1
  [[ "$nj" -gt "$count" ]] && nj="$count"
  print_status "  pass '$label': $count file(s) across $nj worker(s)"

  run_tidy_chunks "$clang_tidy" "$nj" "$list"
}

# ---------------------------------------------------------------------------
# Fan one pass's file list out over `nj` clang-tidy processes and collect the
# results. Split from invoke_clang_tidy so the "is this bucket empty?" decision
# and the parallel execution stay separately readable.
#
# $1  clang-tidy binary
# $2  worker count
# $3  newline-separated file list
#
# Reads the argument vector from the TIDY_ARG_* globals split_tidy_args filled.
# Returns non-zero if any worker reported a violation.
# ---------------------------------------------------------------------------
run_tidy_chunks() {
  local clang_tidy="$1"
  local nj="$2"
  local list="$3"

  local workdir
  workdir="$(mktemp -d)"

  # Round-robin the file list into `nj` chunks so each worker gets a mix of
  # cheap and expensive translation units. Each worker is one clang-tidy
  # process over its whole chunk, writing to its own buffer -- the buffers are
  # printed in chunk order afterwards, so parallel output never interleaves.
  awk -v n="$nj" -v d="$workdir" '{ print > (d "/chunk" (NR % n)) }' "$list"

  local idx=0
  local pids=()
  while [[ "$idx" -lt "$nj" ]]; do
    local chunk="$workdir/chunk$idx"
    if [[ -s "$chunk" ]]; then
      local chunk_files=()
      while IFS= read -r line || [[ -n "$line" ]]; do
        chunk_files+=("$line")
      done <"$chunk"
      if [[ "$TIDY_ARG_SEEN_SEP" == "true" ]]; then
        "$clang_tidy" ${TIDY_ARG_LEAD[@]+"${TIDY_ARG_LEAD[@]}"} ${chunk_files[@]+"${chunk_files[@]}"} -- ${TIDY_ARG_TRAIL[@]+"${TIDY_ARG_TRAIL[@]}"} \
          >"$workdir/out$idx" 2>&1 &
      else
        "$clang_tidy" ${TIDY_ARG_LEAD[@]+"${TIDY_ARG_LEAD[@]}"} ${chunk_files[@]+"${chunk_files[@]}"} >"$workdir/out$idx" 2>&1 &
      fi
      pids+=("$!")
    fi
    idx=$((idx + 1))
  done

  local ec=0
  local pid
  for pid in ${pids[@]+"${pids[@]}"}; do
    wait "$pid" || ec=1
  done

  idx=0
  while [[ "$idx" -lt "$nj" ]]; do
    [[ -s "$workdir/out$idx" ]] && cat "$workdir/out$idx"
    idx=$((idx + 1))
  done
  rm -rf "$workdir"

  return "$ec"
}
