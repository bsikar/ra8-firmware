#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""A stand-in for the ``gh`` executable, driven entirely by ``FAKE_GH_MODE``.

The doctor probes have to distinguish three genuinely different situations,
and a real ``gh`` on a developer machine can only ever be in one of them at a
time. This program is put on ``PATH`` as ``gh`` so a test can choose:

* ``ok`` -- authenticated, and the token carries the ``project`` scope.
* ``noscope`` -- authenticated, but without ``project``. Board mutations are
  impossible from this host and the tool must say so specifically.
* ``error`` -- not authenticated. It exits 1 and prints a token-shaped string
  on stderr, which is what proves the redactor sits in the output path.

A fourth situation, ``gh`` not installed at all, is produced by leaving this
program off ``PATH`` entirely rather than by a mode.

The token-shaped strings below are literal fakes, chosen to match the redactor
pattern and nothing else. They authenticate nothing anywhere.

Exit 0 in the ``ok`` and ``noscope`` modes, 1 in ``error`` mode and for any
argv this stand-in does not implement.
"""

from __future__ import annotations

import os
import sys

EXIT_OK = 0
EXIT_FAIL = 1

FAKE_TOKEN = "gho_FAKEFAKEFAKEFAKEFAKE00000000000000"  # noqa: S105 -- a literal fake, matched only by the redactor

VERSION_LINES = (
    "gh version 2.99.0 (2026-01-01)",
    "https://github.com/cli/cli/releases/tag/v2.99.0",
)

SCOPES_WITH_PROJECT = "'gist', 'project', 'read:org', 'repo', 'workflow'"
SCOPES_WITHOUT_PROJECT = "'gist', 'read:org', 'repo', 'workflow'"


def _emit_auth(scopes: str) -> int:
    """Print an authenticated ``gh auth status`` block on stderr.

    Real gh writes this block to stderr on the versions this repository has
    seen, so the stand-in does the same and the parser is exercised against the
    stream it will actually meet.

    Args:
        scopes: The quoted, comma-separated scope list to report.

    Returns:
        The process exit status.
    """
    print("github.com", file=sys.stderr)
    print("  - Logged in to github.com as fixture-user", file=sys.stderr)
    print("  - Git operations protocol: https", file=sys.stderr)
    print(f"  - Token: {FAKE_TOKEN}", file=sys.stderr)
    print(f"  - Token scopes: {scopes}", file=sys.stderr)
    return EXIT_OK


def _emit_error() -> int:
    """Print an unauthenticated failure carrying a token-shaped string.

    Returns:
        The process exit status.
    """
    print(f"error: the token {FAKE_TOKEN} is invalid or has been revoked", file=sys.stderr)
    print("You are not logged into any GitHub hosts. Run gh auth login.", file=sys.stderr)
    return EXIT_FAIL


def main(argv: list[str]) -> int:
    """Answer the two commands the doctor probes actually issue.

    Args:
        argv: Arguments without the program name.

    Returns:
        The process exit status.
    """
    mode = os.environ.get("FAKE_GH_MODE", "ok")
    if argv[:1] == ["--version"]:
        if mode == "error":
            print("fake gh: version query failed", file=sys.stderr)
            return EXIT_FAIL
        for line in VERSION_LINES:
            print(line)
        return EXIT_OK
    if argv[:2] == ["auth", "status"]:
        if mode == "error":
            return _emit_error()
        if mode == "noscope":
            return _emit_auth(SCOPES_WITHOUT_PROJECT)
        return _emit_auth(SCOPES_WITH_PROJECT)
    print(f"fake gh: unimplemented invocation: {' '.join(argv)}", file=sys.stderr)
    return EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
