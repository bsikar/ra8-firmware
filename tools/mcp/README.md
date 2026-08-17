<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# tools/mcp

A Model Context Protocol server that exposes this tree's real workflows -- the
app catalogue, the build / test / quality gates, code search, the
hardware-in-the-loop rig and the authoritative project docs -- to any MCP-aware
assistant, so it reads what the repo can do from the repo instead of
re-deriving it from loose markdown each session. `ra8_mcp.py` is the entry point
a client launches; it is not meant to be run by hand.

It has zero third-party dependencies: the MCP stdio transport is
newline-delimited JSON-RPC over the Python standard library alone, so there is
nothing to install and nothing to keep in step with an upstream release.

Three things are worth knowing before changing anything in here.

**stdout is the protocol stream.** Every diagnostic goes to stderr; a stray
print on stdout corrupts the session rather than producing a readable error.

**The hardware-touching tools are gated.** Anything that would write to a board
or drive the bench rig previews the exact command it would run and refuses to
act without an explicit confirmation argument, and app names are validated
against the discovered catalogue before they reach `make`, so a tool call
cannot inject a foreign target.

**A tool's schema and its handler live in the same file on purpose.** When the
two disagree the server still starts and still advertises the tool, and the
mismatch surfaces only when a client happens to call it. That is why the
in-process self-test cross-checks them, and why a new tool should gain an
assertion there rather than trusting review to catch a schema drift.

The repo ships a project-scoped `.mcp.json`, so a client honouring project MCP
config picks the server up when it opens this directory; a client with only a
global config needs the same entry with an absolute path to the script. The
server locates the repo root from its own file path, so the launch directory
does not matter.
