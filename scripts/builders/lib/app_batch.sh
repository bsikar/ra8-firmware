#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Shared, NUL-safe selected-app planning and bounded dispatch for the firmware
# batch builder. Source this file; call ra8_app_batch_selftest for both-direction
# mutation coverage without compiling firmware.

# Populated by ra8_app_batch_read_nul and ra8_app_batch_resolve.
RA8_APP_BATCH_ITEMS=()

ra8_app_batch_read_nul() {
  RA8_APP_BATCH_ITEMS=()
  local item=""
  while IFS= read -r -d '' item; do
    RA8_APP_BATCH_ITEMS+=("$item")
    item=""
  done
  if [[ -n "$item" ]]; then
    echo "app-batch: selected input must be NUL-terminated" >&2
    return 2
  fi
}

ra8_app_batch_require_census() {
  local count="$1" floor="$2"
  if [[ ! "$count" =~ ^[0-9]+$ || ! "$floor" =~ ^[0-9]+$ || "$floor" -lt 1 ]]; then
    echo "app-batch: count and floor must be non-negative/positive integers" >&2
    return 2
  fi
  if ((count < floor)); then
    echo "app-batch: selected census collapsed to $count app(s) (floor $floor)" >&2
    return 1
  fi
}

# Resolve aliases/slash forms to stable identifiers and reject duplicates after
# resolution. Otherwise `blink` plus its namespaced ID could satisfy a census
# floor while compiling the same firmware twice.
ra8_app_batch_resolve() {
  local helper="$1" selector canonical previous=""
  local -a resolved=() sorted=()
  for selector in "${RA8_APP_BATCH_ITEMS[@]}"; do
    [[ -n "$selector" ]] || {
      echo "app-batch: empty app selector" >&2
      return 2
    }
    canonical="$(python3 "$helper" id "$selector")" || return 2
    resolved+=("$canonical")
  done
  if ((${#resolved[@]} == 0)); then
    RA8_APP_BATCH_ITEMS=()
    return 0
  fi
  while IFS= read -r canonical; do
    sorted+=("$canonical")
  done < <(printf '%s\n' "${resolved[@]}" | LC_ALL=C sort)
  for canonical in "${sorted[@]}"; do
    if [[ "$canonical" == "$previous" ]]; then
      echo "app-batch: duplicate selected app after resolution: $canonical" >&2
      return 2
    fi
    previous="$canonical"
  done
  RA8_APP_BATCH_ITEMS=("${sorted[@]}")
}

ra8_app_batch_parallel_available() {
  printf '' | xargs -P 2 -n 1 true >/dev/null 2>&1
}

# Read NUL-delimited records on stdin and append one intact record to the fixed
# command argv per worker. Whitespace, quotes, and shell metacharacters are data.
ra8_app_batch_dispatch_nul() {
  local jobs="$1" item
  shift
  if [[ ! "$jobs" =~ ^[0-9]+$ || "$jobs" -lt 1 ]]; then
    echo "app-batch: invalid parallel width '$jobs'" >&2
    return 2
  fi
  if ra8_app_batch_parallel_available; then
    xargs -0 -P "$jobs" -n 1 "$@"
    return
  fi
  while IFS= read -r -d '' item; do
    "$@" "$item" || return
  done
}

_ra8_app_batch_selftest_selection() {
  local helper="$1"
  local failed=0

  ra8_app_batch_read_nul < <(printf '%s\0' alpha 'two words')
  if [[ "${#RA8_APP_BATCH_ITEMS[@]}" -ne 2 || "${RA8_APP_BATCH_ITEMS[1]}" != "two words" ]]; then
    echo "app-batch selftest: NUL/whitespace input was not preserved" >&2
    failed=1
  fi
  if ra8_app_batch_read_nul < <(printf 'not-terminated') 2>/dev/null; then
    echo "app-batch selftest: unterminated selected input was accepted" >&2
    failed=1
  fi

  ra8_app_batch_require_census 2 2 >/dev/null || failed=1
  if ra8_app_batch_require_census 1 2 >/dev/null 2>&1; then
    echo "app-batch selftest: collapsed selected census was accepted" >&2
    failed=1
  fi
  if ra8_app_batch_require_census 0 1 >/dev/null 2>&1; then
    echo "app-batch selftest: zero selected apps were accepted" >&2
    failed=1
  fi

  ra8_app_batch_read_nul < <(printf '%s\0' blink ereader)
  if ! ra8_app_batch_resolve "$helper" || [[ "${#RA8_APP_BATCH_ITEMS[@]}" -ne 2 ]]; then
    echo "app-batch selftest: authoritative selected-app resolution failed" >&2
    failed=1
  fi
  ra8_app_batch_read_nul < <(printf '%s\0' blink 'ek_ra8d2::hw_validated::hil::blink')
  if ra8_app_batch_resolve "$helper" >/dev/null 2>&1; then
    echo "app-batch selftest: alias duplicate was accepted" >&2
    failed=1
  fi

  return "$failed"
}

_ra8_app_batch_selftest_dispatch() {
  local actual expected probe_dir worker
  local failed=0
  expected=$'[alpha]\n[two words]'
  actual="$(printf '%s\0' alpha 'two words' |
    ra8_app_batch_dispatch_nul 1 printf '[%s]\n')" || failed=1
  if [[ "$actual" != "$expected" ]]; then
    echo "app-batch selftest: dispatcher did not preserve one NUL record per argv" >&2
    failed=1
  fi
  if printf 'probe\0' | ra8_app_batch_dispatch_nul 0 true >/dev/null 2>&1; then
    echo "app-batch selftest: dispatcher accepted a zero width" >&2
    failed=1
  fi

  # Two workers must rendezvous, then finish before the third starts. This
  # deadlocks/fails if the pool becomes serial and deliberately fires when the
  # same fixture is widened to three, proving both sides of the bound.
  if ra8_app_batch_parallel_available; then
    probe_dir="$(mktemp -d)"
    # shellcheck disable=SC2016  # this program is interpreted by the worker shell
    worker='item="$2"; dir="$1"; if [[ "$item" == gamma ]]; then
      [[ -e "$dir/done.alpha" || -e "$dir/done.beta" ]]
    else
      touch "$dir/start.$item"
      other=alpha; [[ "$item" == alpha ]] && other=beta
      for _ in {1..200}; do [[ -e "$dir/start.$other" ]] && break; sleep 0.01; done
      [[ -e "$dir/start.$other" ]] || exit 8
      sleep 0.2
      touch "$dir/done.$item"
    fi'
    if ! printf '%s\0' alpha beta gamma |
      ra8_app_batch_dispatch_nul 2 bash -c "$worker" _ "$probe_dir"; then
      echo "app-batch selftest: the two-worker bounded dispatcher failed" >&2
      failed=1
    fi
    rm -f "$probe_dir"/*
    if printf '%s\0' alpha beta gamma |
      ra8_app_batch_dispatch_nul 3 bash -c "$worker" _ "$probe_dir" >/dev/null 2>&1; then
      echo "app-batch selftest: widened-pool must-fire fixture did not fire" >&2
      failed=1
    fi
    rm -rf "$probe_dir"
  fi

  return "$failed"
}

ra8_app_batch_selftest() {
  local helper="$1" failed=0
  _ra8_app_batch_selftest_selection "$helper" || failed=1
  _ra8_app_batch_selftest_dispatch || failed=1
  if ((failed)); then
    echo "app-batch selftest: FAIL" >&2
    return 1
  fi
  echo "app-batch selftest: PASS (census, zero, aliases, NUL/whitespace, bounded pool)"
}
