#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""A Model Context Protocol (MCP) server for this firmware repo.

This server gives an MCP-aware assistant live, structured context about the
ra8-firmware tree -- the firmware app catalogue, build / test / quality
workflows, the hardware-in-the-loop (HIL) rig, code search, and the project's
authoritative docs -- so the assistant can reason about what the repo can
actually *do* instead of guessing from loose markdown files.

Design goals
------------
  * Zero third-party dependencies. It speaks the MCP stdio transport
    (newline-delimited JSON-RPC 2.0) using only the Python standard library,
    so it runs anywhere ``python3`` exists with nothing to ``pip install``.
    That matches the repo's hand-written, minimal-dependency ethos.
  * Read-only by default. Every tool that only inspects the tree (catalogue,
    search, docs) runs freely. Every tool that touches real hardware (flash,
    HIL) is gated behind an explicit ``confirm`` flag and otherwise returns a
    dry-run preview of the exact command it would run.
  * Honest output. Build / test / gate tools shell out to the repo's real
    ``make`` targets and helper scripts, capture the combined output, and
    return the tail verbatim with the real exit status.

Transport
---------
MCP stdio framing is one JSON-RPC message per line on stdin / stdout. All
human-readable logging goes to stderr so it never corrupts the protocol
stream. Run ``python3 tools/mcp/ra8_mcp.py --selftest`` to exercise the
dispatcher in-process without a client (used by ``make mcp``).

The methods implemented are the subset a tools+resources server needs:
``initialize``, ``ping``, ``tools/list``, ``tools/call``, ``resources/list``,
``resources/read``, plus empty ``prompts/list`` /
``resources/templates/list`` replies for clients that probe them.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_protocol import (
    ERR_INVALID_REQUEST,
    ERR_PARSE,
    SERVER_NAME,
    SERVER_VERSION,
    _error,
    dispatch,
)
from mcp_selftest import selftest
from mcp_util import REPO_ROOT, log


# ---------------------------------------------------------------------------
# stdio serve loop and self-test
# ---------------------------------------------------------------------------
def serve() -> int:
    """Run the newline-delimited JSON-RPC loop on stdin/stdout until EOF."""
    log(f"serving {SERVER_NAME} {SERVER_VERSION} on stdio (root {REPO_ROOT})")
    out = sys.stdout
    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError as exc:
            out.write(json.dumps(_error(None, ERR_PARSE, f"parse error: {exc}")) + "\n")
            out.flush()
            continue
        if not isinstance(request, dict):
            out.write(
                json.dumps(_error(None, ERR_INVALID_REQUEST, "request must be an object")) + "\n"
            )
            out.flush()
            continue
        response = dispatch(request)
        if response is not None:
            out.write(json.dumps(response) + "\n")
            out.flush()
    log("stdin closed, exiting")
    return 0


def main(argv: list[str]) -> int:
    """Run the server, or the in-process self-test with `--selftest`.

    With no flags this blocks in `serve()` reading JSON-RPC from stdin until
    EOF, which is the normal mode: an MCP client spawns this as a subprocess and
    speaks the stdio transport to it. Running it from a terminal looks like a
    hang -- it is waiting for a request.

    `--selftest` exercises the dispatcher without a client or a board, so it is
    safe to run anywhere and is what CI invokes.

    Args:
        argv: Argument list WITHOUT the program name (callers pass
            `sys.argv[1:]`).

    Returns:
        Process exit status: 0 on clean shutdown or a passing self-test, 1 on a
        failing one.
    """
    parser = argparse.ArgumentParser(description="MCP server for ra8-firmware.")
    parser.add_argument(
        "--selftest", action="store_true", help="run the in-process dispatcher self-test and exit"
    )
    options = parser.parse_args(argv)
    if options.selftest:
        return selftest()
    return serve()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
