#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Create one exclusive, no-follow Git-hook completion proof."""

from __future__ import annotations

import os
import sys
from pathlib import Path

EXPECTED_ARGC = 2


def write_proof(path: Path, token: str) -> None:
    """Write one ASCII token without replacing or following an existing path."""
    token.encode("ascii")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags, 0o600)
    try:
        with os.fdopen(descriptor, "w", encoding="ascii", closefd=False) as output:
            output.write(f"{token}\n")
            output.flush()
    finally:
        os.close(descriptor)


def main() -> int:
    """Read one private token from stdin and create the requested proof."""
    if len(sys.argv) != EXPECTED_ARGC:
        print("usage: write-proof.py <path>  # token on stdin", file=sys.stderr)
        return 2
    token = sys.stdin.readline().removesuffix("\n")
    if not token or sys.stdin.read(1):
        print("write-proof.py: stdin must contain one non-empty token line", file=sys.stderr)
        return 2
    try:
        write_proof(Path(sys.argv[1]), token)
    except (OSError, UnicodeError) as exc:
        print(f"write-proof.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
