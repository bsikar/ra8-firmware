#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The C-era integration script took "$(cmake)" and used it exactly once, for
# `cmake -E sha256sum`. This workspace has no cmake, so the zig build hands
# the script this wrapper instead, which maps that one invocation onto
# coreutils sha256sum.
set -euo pipefail
if [ $# -eq 3 ] && [ "$1" = "-E" ] && [ "$2" = "sha256sum" ]; then
  exec sha256sum "$3"
fi
echo "cmake_sha256sum.sh: only '-E sha256sum <file>' is supported" >&2
exit 2
