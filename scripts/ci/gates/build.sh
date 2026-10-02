#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/build.sh -- Things that have to compile, link or generate cleanly.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: build-cross, build-cross-union, sbom, soup-upstream, roadmap-stats

# --- build-cross ----------------------------------------------------------
# T5-02: RA8_STRICT_TOOLCHAIN=1 promotes toolchain-ra8d2.cmake's version
# mismatch warning to a hard error, so a runner with a skewed arm-gcc fails
# loudly instead of silently shipping version-divergent miniz codegen.
#
# RA8_BUILD_SHARDS/RA8_BUILD_SHARD (both read by all_examples.sh, and passed
# through from the workflow's matrix rather than as CLI arguments so the
# workflow step stays the bare `--gate build-cross` driver ci-parity requires)
# build only a stride slice of the app list. Unset -- the local suite -- builds
# everything, exactly as before. Whatever the split, the build-cross-union gate
# below proves the shards covered the tree.
gate_build_cross() (
  set -e
  use_pinned_arm_toolchain
  require_cmd arm-none-eabi-gcc
  RA8_STRICT_TOOLCHAIN=1 bash scripts/builders/all_examples.sh
  # Headroom canary: the cross-build is the only place an app is actually
  # linked, so it is the only place the image can be measured. The checker is
  # fail-closed, so a missing ELF here fails the gate rather than passing quietly.
  # --all walks .github/image-headroom-ceilings.tsv, so pinning another app is a
  # one-line change to that file and never an edit to this gate.
  python3 scripts/checks/check_image_headroom.py --all
)

# --- build-cross-union ----------------------------------------------------
# Proves the sharded cross-build was WHOLE. Sharding a gate across parallel
# jobs silently weakens it unless something checks the union: a shard that was
# skipped, cancelled, or sliced to nothing emits no error, and the stack-usage
# aggregate downstream would just measure fewer .su files and still clear its
# floor on the shards that did run -- a gate quietly checking less than it
# claims, which is this tree's most-repeated defect.
#
# The checker re-derives the app list itself instead of trusting the manifest
# a shard wrote (a broken discovery would otherwise agree with itself), and
# fails on a missing shard, an unbuilt app, or an app claimed twice. Its
# --selftest runs first and asserts both directions, so a detector that
# stopped matching cannot pass as a clean gate.
#
# Unsharded (the local suite) this is N=1 and still a real check: it proves
# every discovered app reached the build.
gate_build_cross_union() (
  set -e
  bash scripts/builders/check_build_shard_union.sh --selftest
  bash scripts/builders/check_build_shard_union.sh --shards "${RA8_BUILD_SHARDS:-1}"
)

# --- sbom -----------------------------------------------------------------
# Supply-chain provenance gate. Fails when the committed CycloneDX SBOM
# (docs/sbom/ra8-firmware.cdx.json) is stale or either canonical third-party
# root drifted from the registry -- an uncatalogued SOUP directory, or a
# version macro that disagrees with the recorded version.
# The --check pass is only worth its status because the SHA-256 digests it
# compares are RE-DERIVED from both third-party roots on every run. They used
# to be hand-transcribed literals in sbom_registry.py -- present on 4 of 23
# components, absent from the one that had actually drifted -- so --check
# compared a constant with itself and appending a line to a vendored source
# still printed "SBOM matches the tree" with status 0. --selftest runs
# FIRST and proves the digest fires on a mutated byte and stays quiet on an
# unchanged tree, so a detector that stopped detecting cannot pass as clean.
gate_sbom() (
  set -e
  python3 scripts/gen/gen_sbom.py --selftest
  python3 scripts/gen/gen_sbom.py --check
  # The generator above owns docs/sbom/ra8-firmware.cdx.json and nothing else.
  # The two markdown inventories (THIRD_PARTY_LICENSES.md, docs/SOUP/README.md)
  # are hand-maintained, yet both claimed to be generated from the registry, so
  # nothing noticed a component catalogued in one and missing from the other,
  # an orphan inventory row, or a dangling docs/SOUP link. This checker
  # compares the registry against those two files -- two independently
  # maintained artifacts, never a value with itself -- and refuses to pass a
  # collapsed scan. --selftest runs FIRST and proves it fires on seeded drift
  # and stays quiet on an agreeing tree.
  python3 scripts/checks/check_soup_inventory.py --selftest
  python3 scripts/checks/check_soup_inventory.py
)

# --- soup-upstream --------------------------------------------------------
# The other half of the provenance claim. The sbom gate above re-derives
# a digest over each vendored tree, which proves only that the tree has not
# changed since the SBOM was regenerated -- a tree that was already wrong at
# vendor-in hashes faithfully and reports clean forever. This gate compares
# every vendored file against the blob SHA-1 its UPSTREAM project publishes for
# the pinned revision, recorded in docs/sbom/upstream/*.manifest by a real
# fetch. Two hashes from two projects, so nothing is compared with itself.
#
# Offline by construction: the manifests are committed, so a push does not
# depend on twenty upstream hosts being reachable. The networked half is the
# weekly soup-upstream-refresh gate, which re-fetches and catches what this one
# structurally cannot -- a tag that moved under a pin.
#
# --selftest FIRST, and it drives run_check() itself against a scratch git
# repository: a mutated blob, a lost file, an undeclared patch, a collapsed
# scan. This claim was asserted in three places and checked by nothing, so
# every tree passed it -- including one that had drifted.
# The patch-series checker completes the offline proof for intentional
# deviations: reverse to the recorded upstream blobs, then replay to exactly
# the ready-to-build vendored bytes (or verify fetched pin/series linkage).
gate_soup_upstream() (
  set -e
  python3 scripts/checks/check_soup_upstream.py --selftest
  python3 scripts/checks/check_soup_upstream.py
  python3 scripts/checks/check_third_party_patches.py --selftest
  python3 scripts/checks/check_third_party_patches.py
)

# --- roadmap-stats --------------------------------------------------------
gate_roadmap_stats() (
  set -e
  # The closed HAL completion record remains certification evidence, so a
  # missing ROADMAP.md is a failure, not a skip. This gate used to `echo "no
  # docs/ROADMAP.md -- skipping"` and return 0, so a `git mv` of that one
  # file would have turned the gate green forever while checking nothing --
  # the same shape as every other finding under the gate-honesty epic.
  if [[ ! -f docs/ROADMAP.md ]]; then
    echo "ERROR: docs/ROADMAP.md is missing; the roadmap-stats gate has" >&2
    echo "       nothing to check and must not report success." >&2
    echo "       Restore the file, or delete this gate and its registry row." >&2
    return 1
  fi
  bash scripts/builders/roadmap_stats.sh --check
)
