#!/usr/bin/env bash
# shellcheck shell=bash
# shellcheck disable=SC2034  # sourced config fragment: every RA8_UNICORN_* is consumed by the sourcing script, not here.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/unicorn_pin.sh -- the ONE source of truth for the pinned Unicorn
# CPU-emulator version that every ra8_emulator environment must link.
#
# WHY THIS FILE EXISTS
# --------------------
# tools/ra8_emulator boots the real cross-compiled firmware .elf on Unicorn (QEMU's
# core as a library). Different Unicorn versions DECODE Armv8.1-M instructions
# differently -- notably the Helium/MVE store family the Cortex-M85 executes.
# So the emulator's verdict for an identical .elf depends on which Unicorn is
# installed. If that version is unpinned, "the same commit passes here and
# faults there" is structurally guaranteed, which is exactly the #354 skew:
# the self-hosted runner ran a source-built 2.1.4 while the dev box + the
# devcontainer ran 2.0.1, and 2.0.1 raises spurious EXCP_NOCP on MVE stores.
#
# Pinning makes the decode identical everywhere ra8_emulator runs. 2.1.4 is the
# pin because it is what CI -- the authority (docs/TOOLCHAIN.md) -- already
# links, it decodes the MVE stores real M85 silicon executes, and the ra8_emulator
# smoke / matrix / EIL suite is green on it.
#
# This file is SOURCED by scripts/ci/install_unicorn.sh (which builds + installs
# the pin) and by scripts/checks/check_unicorn_version.sh (which FAILS the
# ra8_emulator gates when the runtime Unicorn is not the pin). The devcontainer
# duplicates the version + sha256 as Docker ARGs (.devcontainer/Dockerfile);
# keep the two in step when moving the pin, the same way the arm-gcc pin is
# duplicated between the Dockerfile and cmake/toolchain-ra8d2.cmake.

# The pinned upstream release (major.minor.patch, as UC_VERSION_* reports it).
RA8_UNICORN_VERSION="2.1.4"

# Reproducible source: the upstream GitHub release tarball, verified by sha256
# so a corrupted, swapped, or regenerated download FAILS LOUDLY rather than
# silently building a different emulator. Mirrors the arm-gcc URL+sha256 pin.
RA8_UNICORN_TARBALL_URL="https://github.com/unicorn-engine/unicorn/archive/refs/tags/${RA8_UNICORN_VERSION}.tar.gz"
RA8_UNICORN_TARBALL_SHA256="ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4"

# Install prefix. /usr/local matches how the runner already carries the pin
# (/usr/local/lib/libunicorn.so.2), so ldconfig resolves it ahead of any stale
# distro package and ra8_emulator's find_library(unicorn) picks it with no
# CMake change. Override with RA8_UNICORN_PREFIX for a per-user install.
RA8_UNICORN_PREFIX="${RA8_UNICORN_PREFIX:-/usr/local}"
