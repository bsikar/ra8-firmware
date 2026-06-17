<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# `tools/mcp` -- Model Context Protocol server for this repo

A small [Model Context Protocol](https://modelcontextprotocol.io) (MCP) server
that exposes this firmware tree's real workflows -- the app catalogue, build /
test / quality gates, the hardware-in-the-loop rig, code search, and the
authoritative project docs -- to any MCP-aware assistant. The point is to give
the assistant *live, structured* context about what the repo can actually do,
instead of it re-deriving everything from loose markdown each session.

It is a single self-contained script, `ra8d2_mcp.py`, with **zero third-party
dependencies**: it speaks the MCP stdio transport (newline-delimited JSON-RPC
2.0) using only the Python standard library, so there is nothing to install.
That matches the rest of the repo's hand-written, minimal-dependency tooling.

## Quick check

```sh
make mcp                          # runs the in-process self-test
python3 tools/mcp/ra8d2_mcp.py --selftest
```

The server itself is not meant to be run by hand -- an MCP client launches it
and talks JSON-RPC over stdin/stdout. Diagnostics go to stderr so they never
corrupt the protocol stream.

## What it exposes

### Tools (actions the assistant can call)

| Tool | What it does | Hardware? |
|------|--------------|-----------|
| `list_apps` | List discovered firmware apps (optional substring filter) | no |
| `app_info` | One app's directory, description, boot files, README head | no |
| `repo_overview` | Target MCU, app count, common workflows, where docs live | no |
| `search_code` | Regex search across first-party source (skips `third_party`) | no |
| `hum_lookup` | Hardware User's Manual chapter page ranges (for citations) | no |
| `build_app` | `make <app>` cross-compile, returns the log tail | no |
| `run_tests` | `make test` host unit tests | no |
| `quality_gate` | one of: format-check, tidy, ascii, version, cppcheck, check-annotations, mcdc, cite-check, ai-attribution, inclusive | no |
| `flash_app` | `make flash-<app>` to a local J-Link board | **yes** |
| `hil` | drive the Pi rig: flash / recover / flash-retry / erase / probe / dlm-reset | **yes** |

The two hardware tools are **gated**: they preview the exact command they would
run and refuse to touch a board unless called with `confirm: true`. App names
are validated against the discovered catalogue before reaching `make`, so a
tool call can never inject a foreign target.

### Resources (read-only context by stable URI)

| URI | Content |
|-----|---------|
| `ra8d2://doc/claude-md` | the project's AI-assistant rules |
| `ra8d2://doc/style-guide` | `docs/STYLE_GUIDE.md` |
| `ra8d2://doc/ring-and-world` | `docs/RING_AND_WORLD.md` |
| `ra8d2://doc/contributing` | `CONTRIBUTING.md` |
| `ra8d2://reference/chapter-map` | HUM chapter-to-page map |
| `ra8d2://apps/catalogue` | live list of every firmware app |

## Wiring it into an MCP client

The repo ships a project-scoped `.mcp.json` at its root, so a client that
honors project MCP config will pick the server up automatically when opened in
this directory:

```json
{
  "mcpServers": {
    "ra8d2-firmware": {
      "command": "python3",
      "args": ["tools/mcp/ra8d2_mcp.py"]
    }
  }
}
```

For a client that uses a global config file (for example, a desktop MCP host
<!-- AI-OK: integration instructions name the MCP host product -->
such as Claude Desktop), add the same `mcpServers` entry but give an **absolute**
path to the script so it resolves regardless of the launch directory:

```json
{
  "mcpServers": {
    "ra8d2-firmware": {
      "command": "python3",
      "args": ["/abs/path/to/ra8d2-firmware/tools/mcp/ra8d2_mcp.py"]
    }
  }
}
```

The server locates the repo root from its own file path, so it works no matter
what working directory the client launches it from.

## Extending it

Add a tool by writing a `tool_*` handler that takes an arguments dict and
returns text, then append an entry to the `TOOLS` list (name, description,
JSON-Schema input, handler). Add a resource by appending to `RESOURCES`. The
`--selftest` path covers the protocol surface; add an assertion there for any
new tool so `make mcp` keeps guarding it.
