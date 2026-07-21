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
import re
import subprocess
import sys
from pathlib import Path
from shutil import which as _shutil_which
from typing import Any, Callable

# Repo root = two parents up from this file (tools/mcp/<this>).
REPO_ROOT = Path(__file__).resolve().parents[2]

SERVER_NAME = "ra8-firmware"
SERVER_VERSION = "1.0.0"

# Newest spec revision this server is known to interoperate with. When a client
# asks for a specific revision in ``initialize`` we echo theirs back (the spec
# lets the server propose its own, but echoing avoids needless renegotiation).
PROTOCOL_VERSION_DEFAULT = "2024-11-05"

# Upper bound on captured subprocess output returned to the client, so a
# runaway build log can never blow up the assistant's context window.
MAX_OUTPUT_CHARS = 12000

# JSON-RPC 2.0 standard error codes used by this server.
ERR_PARSE = -32700
ERR_INVALID_REQUEST = -32600
ERR_METHOD_NOT_FOUND = -32601
ERR_INVALID_PARAMS = -32602
ERR_INTERNAL = -32603


# ---------------------------------------------------------------------------
# Small shared helpers
# ---------------------------------------------------------------------------
def log(message: str) -> None:
    """Write a diagnostic line to stderr (never stdout, which is the wire)."""
    print(f"[ra8d2-mcp] {message}", file=sys.stderr, flush=True)


def truncate(text: str, limit: int = MAX_OUTPUT_CHARS) -> str:
    """Clamp ``text`` to ``limit`` characters, keeping the tail (most recent)."""
    if len(text) <= limit:
        return text
    head = "[... output truncated, showing the last bytes ...]\n"
    return head + text[-(limit - len(head)) :]


def run_command(argv: list[str], timeout: int) -> str:
    """Run ``argv`` from the repo root and return a formatted result block.

    The combined stdout+stderr is captured and truncated. The returned string
    leads with the exact command and its exit status so the assistant always
    sees whether the step actually succeeded.
    """
    pretty = " ".join(argv)
    try:
        proc = subprocess.run(  # noqa: S603  # trusted: fixed make/tool argv from internal registry
            argv,
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except FileNotFoundError as exc:
        return f"$ {pretty}\n[not run] executable not found: {exc}"
    except subprocess.TimeoutExpired:
        return f"$ {pretty}\n[timeout] exceeded {timeout}s and was killed"
    status = "ok" if proc.returncode == 0 else f"FAILED (exit {proc.returncode})"
    body = truncate(proc.stdout or "")
    return f"$ {pretty}\n[status] {status}\n\n{body}".rstrip() + "\n"


def read_text(path: Path, max_lines: int = 0) -> str:
    """Read a UTF-8 text file, optionally clamped to the first ``max_lines``."""
    text = path.read_text(encoding="utf-8", errors="replace")
    if max_lines > 0:
        lines = text.splitlines()
        if len(lines) > max_lines:
            kept = "\n".join(lines[:max_lines])
            return f"{kept}\n[... {len(lines) - max_lines} more lines ...]\n"
    return text


# ---------------------------------------------------------------------------
# Firmware app catalogue (mirrors the top-level Makefile discovery rules)
# ---------------------------------------------------------------------------
_DESCRIPTION_RX = re.compile(r'DESCRIPTION\s+"([^"]*)"')


def discover_apps() -> dict[str, dict[str, str]]:
    """Scan ``examples/`` for firmware apps the same way ``make apps`` does.

    An app is any directory holding a ``main.c`` one to three levels under a
    tier directory (``examples/<tier>/.../<app>/main.c``). The app name is the
    directory basename -- the exact token ``make <app>`` / ``make flash-<app>``
    expects. The description is parsed from the app's ``CMakeLists.txt``
    ``ra8_add_app(... DESCRIPTION "...")`` clause.
    """
    apps: dict[str, dict[str, str]] = {}
    examples = REPO_ROOT / "examples"
    if not examples.is_dir():
        return apps
    for main in sorted(examples.glob("*/**/main.c")):
        rel = main.relative_to(examples)
        if rel.parts and rel.parts[0] == "shared":
            continue
        if "build" in rel.parts:
            continue
        app_dir = main.parent
        name = app_dir.name
        group = str(app_dir.relative_to(examples).parent)
        description = ""
        cml = app_dir / "CMakeLists.txt"
        if cml.is_file():
            match = _DESCRIPTION_RX.search(cml.read_text(encoding="utf-8", errors="replace"))
            if match:
                description = match.group(1)
        apps[name] = {
            "name": name,
            "group": group,
            "dir": str(app_dir.relative_to(REPO_ROOT)),
            "description": description,
        }
    return apps


def require_app(name: str) -> dict[str, str]:
    """Resolve a firmware app name or raise ``ValueError`` if it is unknown.

    Validating against the discovered set keeps an arbitrary string from ever
    reaching ``make`` as a target, so a tool call cannot inject a foreign rule.
    """
    apps = discover_apps()
    app = apps.get(name)
    if app is None:
        sample = ", ".join(sorted(apps)[:12])
        msg = f"unknown app '{name}'. Use the list_apps tool. Examples: {sample} ..."
        raise ValueError(msg)
    return app


# ---------------------------------------------------------------------------
# Tool implementations -- each returns plain text shown to the assistant
# ---------------------------------------------------------------------------
def tool_list_apps(args: dict[str, Any]) -> str:
    """List discovered firmware apps, optionally filtered by a substring."""
    needle = str(args.get("filter", "")).lower()
    apps = discover_apps()
    rows = []
    for name in sorted(apps):
        app = apps[name]
        hay = f"{name} {app['group']} {app['description']}".lower()
        if needle and needle not in hay:
            continue
        rows.append(f"  {app['group']:<28} {name:<30} {app['description']}")
    header = f"{len(rows)} firmware app(s)" + (f" matching '{needle}'" if needle else "")
    body = "\n".join(rows) if rows else "  (none matched)"
    return (
        f"{header}\n"
        "  build: make <app> | flash: make flash-<app> | simulate: make sim-<app>\n\n"
        f"{'  TIER/GROUP':<30} {'APP':<30} DESCRIPTION\n{body}\n"
    )


def tool_app_info(args: dict[str, Any]) -> str:
    """Show details for one firmware app: location, description, boot files, README."""
    name = str(args.get("app", "")).strip()
    app = require_app(name)
    app_dir = REPO_ROOT / app["dir"]
    boot_files = [
        f
        for f in (
            "main.c",
            "vector_table.c",
            "system_init.c",
            "secure_exception.c",
            "trustzone_init.c",
            "trustzone_init.h",
            "linker_script.ld",
            "CMakeLists.txt",
            "Makefile",
            "README.md",
        )
        if (app_dir / f).is_file()
    ]
    out = [
        f"app:         {app['name']}",
        f"group/tier:  {app['group']}",
        f"directory:   {app['dir']}",
        f"description: {app['description'] or '(none)'}",
        f"files:       {', '.join(boot_files)}",
        "",
        "build:    make " + app["name"],
        "flash:    make flash-" + app["name"] + "   (local J-Link)",
        "simulate: make sim-" + app["name"] + "     (tools/board_sim emulator)",
    ]
    readme = app_dir / "README.md"
    if readme.is_file():
        out += ["", "--- README.md (first 60 lines) ---", read_text(readme, 60)]
    return "\n".join(out)


def tool_search_code(args: dict[str, Any]) -> str:
    """Search first-party source with ripgrep (falls back to grep -r)."""
    pattern = str(args.get("pattern", "")).strip()
    if not pattern:
        msg = "pattern is required"
        raise ValueError(msg)
    glob = str(args.get("glob", "")).strip()
    max_results = int(args.get("max_results", 80))
    if which("rg"):
        argv = [
            "rg",
            "--line-number",
            "--no-heading",
            "--max-count",
            "5",
            "-g",
            "!third_party",
            "-g",
            "!build",
        ]
        if glob:
            argv += ["-g", glob]
        argv += ["--", pattern, "libs", "src", "examples", "tests", "port", "scripts", "docs"]
    else:
        argv = [
            "grep",
            "-rnI",
            "--exclude-dir=third_party",
            "--exclude-dir=build",
            pattern,
            "libs",
            "src",
            "examples",
            "tests",
            "port",
            "scripts",
            "docs",
        ]
    result = run_command(argv, timeout=30)
    lines = result.splitlines()
    if len(lines) > max_results + 3:
        lines = [*lines[: max_results + 3], f"[... capped at {max_results} hits ...]"]
    return "\n".join(lines)


def tool_repo_overview(_args: dict[str, Any]) -> str:
    """Summarise the target hardware, key commands, and repo layout."""
    apps = discover_apps()
    return (
        "ra8-firmware -- bare-metal RA8D2 firmware (hand-written HAL, CMake + "
        "arm-none-eabi-gcc).\n\n"
        "TARGET: Renesas R7KA8D2KFLCAC -- Cortex-M85 @ 1 GHz (+ Helium) primary, "
        "Cortex-M33 @ 250 MHz secondary; 1 MB MRAM, 2 MB SRAM (ECC); EK-RA8D2 board.\n\n"
        f"APPS: {len(apps)} firmware apps under examples/<tier>/.../<app>/. "
        "Use list_apps / app_info.\n\n"
        "COMMON WORKFLOWS (also exposed as tools):\n"
        "  make <app>            cross-compile one app\n"
        "  make flash-<app>      build + flash via local J-Link\n"
        "  make sim-<app>        run the real .elf on the board_sim emulator\n"
        "  make test             host unit tests\n"
        "  make mcdc             DO-178C Level B MC/DC coverage report\n"
        "  make check|tidy|ascii|version|cppcheck   quality gates\n"
        "  make hil-flash APP=<app>   flash the Pi-attached HIL board\n\n"
        "AUTHORITATIVE DOCS are exposed as MCP resources (CLAUDE.md, STYLE_GUIDE, "
        "RING_AND_WORLD, CONTRIBUTING, the HUM chapter map, the app catalogue)."
    )


def tool_hum_lookup(args: dict[str, Any]) -> str:
    """Look up Hardware User's Manual chapter page ranges from the chapter map."""
    query = str(args.get("query", "")).strip().lower()
    chapter_map = REPO_ROOT / "docs" / "reference" / "CHAPTER_MAP.md"
    if not chapter_map.is_file():
        return "docs/reference/CHAPTER_MAP.md not found in this tree."
    lines = read_text(chapter_map).splitlines()
    if not query:
        return read_text(chapter_map, 60)
    hits = [ln for ln in lines if query in ln.lower()]
    if not hits:
        return f"No chapter-map entry matched '{query}'."
    return f"CHAPTER_MAP.md entries matching '{query}':\n" + "\n".join(hits[:40])


def tool_build_app(args: dict[str, Any]) -> str:
    """Cross-compile one firmware app via ``make <app>`` and return the log tail."""
    app = require_app(str(args.get("app", "")).strip())
    return run_command(["make", app["name"]], timeout=900)


def tool_run_tests(_args: dict[str, Any]) -> str:
    """Host-compile and run the unit-test suite via ``make test``."""
    return run_command(["make", "test"], timeout=900)


_GATES: dict[str, list[str]] = {
    "format-check": ["make", "check"],
    "tidy": ["make", "tidy"],
    "ascii": ["make", "ascii"],
    "version": ["make", "version"],
    "cppcheck": ["make", "cppcheck"],
    "check-annotations": ["make", "check-annotations"],
    "mcdc": ["make", "mcdc"],
    "cite-check": ["python3", "scripts/checks/cite_check.py", "--warn"],
    "ai-attribution": ["python3", "scripts/checks/check_no_ai_attribution.py"],
    "inclusive": ["python3", "scripts/checks/check_inclusive_terminology.py"],
}


def tool_quality_gate(args: dict[str, Any]) -> str:
    """Run one named quality gate (formatting, lint, ASCII, citations, ...)."""
    gate = str(args.get("gate", "")).strip()
    argv = _GATES.get(gate)
    if argv is None:
        msg = f"unknown gate '{gate}'. Choose one of: {', '.join(sorted(_GATES))}"
        raise ValueError(msg)
    timeout = 900 if gate in ("tidy", "cppcheck", "mcdc") else 300
    return run_command(argv, timeout=timeout)


def tool_coverage(args: dict[str, Any]) -> str:
    """Run the DO-178C Level B MC/DC coverage build (``make mcdc``) and return its tail.

    MC/DC (modified condition / decision coverage) is the certification bar this
    tree targets; the tail carries the per-decision summary and any gaps.
    """
    del args  # no arguments; signature kept uniform for the dispatcher
    return run_command(["make", "mcdc"], timeout=1200)


def tool_sim_app(args: dict[str, Any]) -> str:
    """Boot one app's real ``.elf`` on the board_sim Unicorn emulator -- no hardware.

    Runs ``scripts/sim/smoke.sh <app>`` headlessly: it builds the app + the
    emulator, runs the firmware, and asserts it reaches its run budget without
    faulting -- plus its real peripheral UART banner where known. Returns the
    per-app verdict + log tail. The single way to exercise an app without a board.
    """
    app = require_app(str(args.get("app", "")).strip())
    return run_command(["bash", "scripts/sim/smoke.sh", app["name"]], timeout=900)


def _capture(argv: list[str], timeout: int = 20) -> str:
    """Return the stripped stdout of ``argv`` (or a short error marker)."""
    try:
        proc = subprocess.run(  # noqa: S603  # trusted: fixed git/gh argv from internal callers
            argv,
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return f"[not run: {exc}]"
    return (proc.stdout or "").strip() or "(no output)"


def tool_git_status(args: dict[str, Any]) -> str:
    """Read-only repo state: current branch, working-tree status, recent commits, open PRs."""
    del args  # no arguments
    branch = _capture(["git", "rev-parse", "--abbrev-ref", "HEAD"])
    status = _capture(["git", "status", "--short", "--branch"])
    commits = _capture(["git", "log", "--oneline", "-8"])
    prs = (
        _capture(
            [
                "gh",
                "pr",
                "list",
                "--state",
                "open",
                "--limit",
                "10",
                "--json",
                "number,title,headRefName",
                "--template",
                "{{range .}}#{{.number}} {{.title}} ({{.headRefName}})\n{{end}}",
            ]
        )
        if which("gh")
        else "(gh not on PATH)"
    )
    return (
        f"branch:  {branch}\n\n"
        f"--- working tree ---\n{status}\n\n"
        f"--- recent commits ---\n{commits}\n\n"
        f"--- open PRs ---\n{prs}\n"
    )


def tool_hum_citation(args: dict[str, Any]) -> str:
    """Emit the HUM register-citation skeleton the project requires above every MMIO access."""
    chapter = str(args.get("chapter", "")).strip()
    section = str(args.get("section", "")).strip()
    page = str(args.get("page", "")).strip()
    if not chapter:
        msg = "chapter is required, e.g. '38.2.3' (use hum_lookup to find it)"
        raise ValueError(msg)
    sect = f' "{section}"' if section else ' "<section name>"'
    pg = f" p {page}" if page else " p <NNNN>"
    return (
        f"/* HUM Ch {chapter}{sect}{pg} */\n\n"
        "Place this comment IMMEDIATELY above the register read/write (CLAUDE.md\n"
        "External HUM Citations policy). Fill the section name + page range from\n"
        "docs/reference/CHAPTER_MAP.md (the hum_lookup tool / chapter-map resource).\n"
        "Cite the Hardware User's Manual, never an in-tree file:line."
    )


def tool_flash_app(args: dict[str, Any]) -> str:
    """Build and flash an app to a locally attached EK-RA8D2 (hardware write).

    Gated: with ``confirm`` false (the default) this only previews the exact
    command. Pass ``confirm`` true to actually program the board.
    """
    app = require_app(str(args.get("app", "")).strip())
    argv = ["make", f"flash-{app['name']}"]
    if not bool(args.get("confirm", False)):
        return (
            "[dry run] hardware write withheld. This would program a locally "
            "attached EK-RA8D2 via J-Link.\n"
            f"Command: {' '.join(argv)}\n"
            "Re-call flash_app with confirm=true to execute."
        )
    return run_command(argv, timeout=300)


_HIL_ACTIONS: dict[str, list[str]] = {
    "flash": ["make", "hil-flash"],
    "recover": ["make", "hil-recover"],
    "flash-retry": ["make", "hil-flash-retry"],
    "erase": ["make", "hil-erase"],
    "probe": ["make", "hil-probe"],
    "dlm-reset": ["make", "hil-dlm-reset"],
}


def tool_hil(args: dict[str, Any]) -> str:
    """Drive the Pi-attached hardware-in-the-loop rig (hardware action).

    Gated like flash_app: previews unless ``confirm`` is true. ``flash`` /
    ``recover`` / ``flash-retry`` require an ``app``; ``probe`` / ``erase`` /
    ``dlm-reset`` do not.
    """
    action = str(args.get("action", "")).strip()
    argv = list(_HIL_ACTIONS.get(action, []))
    if not argv:
        msg = f"unknown action '{action}'. Choose: {', '.join(sorted(_HIL_ACTIONS))}"
        raise ValueError(msg)
    if action in ("flash", "recover", "flash-retry"):
        app = require_app(str(args.get("app", "")).strip())
        argv.append(f"APP={app['name']}")
    if not bool(args.get("confirm", False)):
        return (
            "[dry run] HIL hardware action withheld.\n"
            f"Command: {' '.join(argv)}\n"
            "Re-call hil with confirm=true to execute."
        )
    return run_command(argv, timeout=600)


# ---------------------------------------------------------------------------
# Tool registry -- name, JSON-Schema input, handler. Descriptions are what the
# assistant reads to decide when to call each tool, so they carry real intent.
# ---------------------------------------------------------------------------
def _schema(props: dict[str, Any], required: list[str] | None = None) -> dict[str, Any]:
    return {"type": "object", "properties": props, "required": required or []}


TOOLS: list[dict[str, Any]] = [
    {
        "name": "list_apps",
        "description": "List discovered RA8D2 firmware apps (name, tier/group, "
        "description). Optional 'filter' substring narrows the list.",
        "inputSchema": _schema(
            {"filter": {"type": "string", "description": "case-insensitive substring filter"}}
        ),
        "handler": tool_list_apps,
    },
    {
        "name": "app_info",
        "description": "Details for one firmware app: directory, description, which "
        "boot files are present, build/flash/sim commands, README head.",
        "inputSchema": _schema({"app": {"type": "string", "description": "app name"}}, ["app"]),
        "handler": tool_app_info,
    },
    {
        "name": "repo_overview",
        "description": "One-shot orientation: target hardware, app count, the common "
        "build/test/flash/HIL workflows, and where the docs live.",
        "inputSchema": _schema({}),
        "handler": tool_repo_overview,
    },
    {
        "name": "search_code",
        "description": "Search first-party source (libs, src, examples, tests, "
        "port, scripts, docs) for a regex pattern. Skips third_party and build.",
        "inputSchema": _schema(
            {
                "pattern": {"type": "string", "description": "regex to search for"},
                "glob": {"type": "string", "description": "optional file glob, e.g. *.c"},
                "max_results": {"type": "integer", "description": "hit cap (default 80)"},
            },
            ["pattern"],
        ),
        "handler": tool_search_code,
    },
    {
        "name": "hum_lookup",
        "description": "Look up Hardware User's Manual chapter page ranges from "
        "docs/reference/CHAPTER_MAP.md (useful for register citations).",
        "inputSchema": _schema(
            {"query": {"type": "string", "description": "chapter number or section keyword"}}
        ),
        "handler": tool_hum_lookup,
    },
    {
        "name": "build_app",
        "description": "Cross-compile one firmware app (make <app>) and return the "
        "build log tail with the real exit status.",
        "inputSchema": _schema({"app": {"type": "string", "description": "app name"}}, ["app"]),
        "handler": tool_build_app,
    },
    {
        "name": "run_tests",
        "description": "Host-compile and run the unit-test suite (make test).",
        "inputSchema": _schema({}),
        "handler": tool_run_tests,
    },
    {
        "name": "quality_gate",
        "description": "Run one named quality gate: format-check, tidy, ascii, "
        "version, cppcheck, check-annotations, mcdc, cite-check, "
        "ai-attribution, inclusive.",
        "inputSchema": _schema({"gate": {"type": "string", "description": "gate name"}}, ["gate"]),
        "handler": tool_quality_gate,
    },
    {
        "name": "flash_app",
        "description": "Build and flash an app to a locally attached EK-RA8D2 via "
        "J-Link. HARDWARE WRITE -- previews unless confirm=true.",
        "inputSchema": _schema(
            {
                "app": {"type": "string", "description": "app name"},
                "confirm": {"type": "boolean", "description": "true to actually flash"},
            },
            ["app"],
        ),
        "handler": tool_flash_app,
    },
    {
        "name": "hil",
        "description": "Drive the Pi-attached HIL rig: action in flash, recover, "
        "flash-retry, erase, probe, dlm-reset. HARDWARE -- previews "
        "unless confirm=true. flash/recover/flash-retry need an app.",
        "inputSchema": _schema(
            {
                "action": {"type": "string", "description": "HIL action"},
                "app": {"type": "string", "description": "app name (for flash actions)"},
                "confirm": {"type": "boolean", "description": "true to actually run"},
            },
            ["action"],
        ),
        "handler": tool_hil,
    },
    {
        "name": "sim_app",
        "description": "Boot one app's real .elf on the board_sim Unicorn emulator (no "
        "hardware): build + run headless, assert it reaches its run budget "
        "without faulting plus its peripheral UART banner. Returns the verdict.",
        "inputSchema": _schema({"app": {"type": "string", "description": "app name"}}, ["app"]),
        "handler": tool_sim_app,
    },
    {
        "name": "coverage",
        "description": "Run the DO-178C Level B MC/DC coverage build (make mcdc) and return "
        "the per-decision summary tail.",
        "inputSchema": _schema({}),
        "handler": tool_coverage,
    },
    {
        "name": "git_status",
        "description": "Read-only repo state: current branch, working-tree status, the last "
        "few commits, and open GitHub PRs.",
        "inputSchema": _schema({}),
        "handler": tool_git_status,
    },
    {
        "name": "hum_citation",
        "description": "Emit the HUM register-citation comment skeleton the project requires "
        "immediately above every MMIO access. Args: chapter (e.g. 38.2.3), "
        "optional section + page.",
        "inputSchema": _schema(
            {
                "chapter": {"type": "string", "description": "HUM chapter, e.g. 38.2.3"},
                "section": {"type": "string", "description": "section name (optional)"},
                "page": {"type": "string", "description": "page or range, e.g. 2181 (optional)"},
            },
            ["chapter"],
        ),
        "handler": tool_hum_citation,
    },
]

TOOL_INDEX: dict[str, dict[str, Any]] = {t["name"]: t for t in TOOLS}


# ---------------------------------------------------------------------------
# Resource registry -- read-only project context exposed by stable URIs
# ---------------------------------------------------------------------------
def _resource_doc(rel_path: str) -> Callable[[], str]:
    def reader() -> str:
        path = REPO_ROOT / rel_path
        if not path.is_file():
            return f"{rel_path} not found in this tree."
        return read_text(path)

    return reader


def _resource_catalogue() -> str:
    return tool_list_apps({})


RESOURCES: list[dict[str, Any]] = [
    {
        "uri": "ra8d2://doc/claude-md",
        "name": "CLAUDE.md",
        "description": "Project rules for AI assistants (the most-violated rules).",
        "mimeType": "text/markdown",
        "reader": _resource_doc("CLAUDE.md"),
    },
    {
        "uri": "ra8d2://doc/style-guide",
        "name": "docs/STYLE_GUIDE.md",
        "description": "Authoritative C23 + Doxygen style guide.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/STYLE_GUIDE.md"),
    },
    {
        "uri": "ra8d2://doc/ring-and-world",
        "name": "docs/RING_AND_WORLD.md",
        "description": "Architectural-ring + TrustZone-world tagging system.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/RING_AND_WORLD.md"),
    },
    {
        "uri": "ra8d2://doc/contributing",
        "name": "CONTRIBUTING.md",
        "description": "Contributor workflow and gate reference.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("CONTRIBUTING.md"),
    },
    {
        "uri": "ra8d2://reference/chapter-map",
        "name": "HUM chapter map",
        "description": "Hardware User's Manual chapter-to-page ranges for citations.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/reference/CHAPTER_MAP.md"),
    },
    {
        "uri": "ra8d2://doc/hil",
        "name": "docs/HIL_SUITE.md",
        "description": "Hardware-in-the-loop suite + how each app is verified in CI.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/HIL_SUITE.md"),
    },
    {
        "uri": "ra8d2://doc/ai-attribution",
        "name": "docs/AI_ATTRIBUTION_POLICY.md",
        "description": "The zero-AI-attribution policy enforced across the tree.",
        "mimeType": "text/markdown",
        "reader": _resource_doc("docs/AI_ATTRIBUTION_POLICY.md"),
    },
    {
        "uri": "ra8d2://apps/catalogue",
        "name": "Firmware app catalogue",
        "description": "Live list of every discovered firmware app.",
        "mimeType": "text/plain",
        "reader": _resource_catalogue,
    },
]

RESOURCE_INDEX: dict[str, dict[str, Any]] = {r["uri"]: r for r in RESOURCES}


# ---------------------------------------------------------------------------
# Prompt registry -- reusable review flows the assistant can invoke by name.
# Each ``template`` is filled with the call's arguments by handle_prompts_get.
# ---------------------------------------------------------------------------
PROMPTS: list[dict[str, Any]] = [
    {
        "name": "audit_register_access",
        "description": "Audit a direct MMIO register access for a valid HUM citation + style.",
        "arguments": [
            {"name": "code", "description": "the register read/write line(s)", "required": True}
        ],
        "template": (
            "Audit this RA8D2 register access against the project rules: every direct "
            "register read/write MUST be immediately preceded by a HUM citation comment "
            '`/* HUM Ch X.Y "section" p NNNN */`, must go through an inline accessor '
            "(never a macro address), and must be pure 7-bit ASCII. Report each violation "
            "with a concrete fix.\n\n```c\n{code}\n```"
        ),
    },
    {
        "name": "mcdc_vectors",
        "description": "Write minimal MC/DC test vectors for a compound boolean decision.",
        "arguments": [
            {"name": "decision", "description": "the C boolean decision", "required": True}
        ],
        "template": (
            "Write the minimal (N+1) MC/DC test vectors for this decision, demonstrating "
            "that each condition independently affects the outcome, formatted as the "
            "project's `@par MC/DC:` Doxygen block.\n\n```c\n{decision}\n```"
        ),
    },
]

PROMPT_INDEX: dict[str, dict[str, Any]] = {p["name"]: p for p in PROMPTS}


def handle_prompts_list() -> dict[str, Any]:
    """Answer MCP `prompts/list` with the catalogue's public fields.

    Deliberately projects each entry rather than returning it whole: `template`
    is server-side detail, and shipping it would leak the prompt text to every
    client that merely enumerates.

    Returns:
        `{"prompts": [...]}` with name, description and arguments per entry, in
        PROMPTS order.
    """
    listed = [
        {"name": p["name"], "description": p["description"], "arguments": p["arguments"]}
        for p in PROMPTS
    ]
    return {"prompts": listed}


def handle_prompts_get(params: dict[str, Any]) -> dict[str, Any]:
    """Answer MCP `prompts/get` by substituting arguments into a template.

    Every declared argument is substituted, and a missing one becomes "" rather
    than an error -- so a partially-filled prompt renders with a gap instead of
    failing the call. Only declared arguments are passed to `format()`, so an
    undeclared extra in `params` is ignored, and a `{placeholder}` in the
    template with no matching declared argument raises.

    Args:
        params: JSON-RPC params; "name" selects the prompt, "arguments" is an
            optional name -> value mapping whose values are coerced to str.

    Returns:
        `{"description": ..., "messages": [...]}` with one user-role text
        message.

    Raises:
        ValueError: No prompt by that name, or the template referenced a
            placeholder that is not a declared argument. `dispatch` converts
            this into a JSON-RPC error response.
    """
    name = str(params.get("name", ""))
    prompt = PROMPT_INDEX.get(name)
    if prompt is None:
        msg = f"unknown prompt: {name}"
        raise ValueError(msg)
    arguments = params.get("arguments") or {}
    try:
        text = prompt["template"].format(
            **{a["name"]: str(arguments.get(a["name"], "")) for a in prompt["arguments"]}
        )
    except (KeyError, IndexError) as exc:
        msg = f"bad prompt arguments: {exc}"
        raise ValueError(msg) from exc
    return {
        "description": prompt["description"],
        "messages": [{"role": "user", "content": {"type": "text", "text": text}}],
    }


# ---------------------------------------------------------------------------
# JSON-RPC dispatch
# ---------------------------------------------------------------------------
def _result(request_id: Any, payload: dict[str, Any]) -> dict[str, Any]:
    """Wrap `payload` as a JSON-RPC 2.0 success response.

    Args:
        request_id: The originating request's id, echoed verbatim -- the client
            matches responses by it, so it must not be normalised.
        payload: The method's result object.

    Returns:
        A response envelope carrying `result`.
    """
    return {"jsonrpc": "2.0", "id": request_id, "result": payload}


def _error(request_id: Any, code: int, message: str) -> dict[str, Any]:
    """Wrap a failure as a JSON-RPC 2.0 error response.

    This is protocol-level failure -- an unknown method, malformed JSON. A tool
    that runs and fails is NOT this: `handle_tools_call` returns a successful
    response carrying `isError: true`, so the model sees the failure text
    instead of the transport swallowing it.

    Args:
        request_id: The originating request's id, echoed verbatim. None when
            the request was unparseable enough to have no id.
        code: JSON-RPC error code (-32601 unknown method, -32603 internal).
        message: Human-readable text; reaches the client as-is.

    Returns:
        A response envelope carrying `error`.
    """
    return {"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}}


def handle_initialize(params: dict[str, Any]) -> dict[str, Any]:
    """Answer the MCP `initialize` handshake and declare server capabilities.

    Echoes the client's requested `protocolVersion` back rather than asserting
    the server's own, which keeps a client on a newer spec revision from being
    refused over a version string alone. The fallback applies only when the
    client omits the field or sends an empty one.

    All three capability objects are advertised empty: this server supports
    tools, resources and prompts but none of their optional sub-features (no
    list-changed notifications, no subscriptions).

    Args:
        params: JSON-RPC params from the client's initialize request.

    Returns:
        Protocol version, capability map, and server name/version.
    """
    requested = str(params.get("protocolVersion") or PROTOCOL_VERSION_DEFAULT)
    return {
        "protocolVersion": requested,
        "capabilities": {"tools": {}, "resources": {}, "prompts": {}},
        "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
    }


def handle_tools_list() -> dict[str, Any]:
    """Answer MCP `tools/list` with the catalogue's client-visible fields.

    Projects each entry to name, description and inputSchema; `handler` is a
    Python callable and is neither serialisable nor the client's business.

    Returns:
        `{"tools": [...]}` in TOOLS order.
    """
    listed = [
        {"name": t["name"], "description": t["description"], "inputSchema": t["inputSchema"]}
        for t in TOOLS
    ]
    return {"tools": listed}


def handle_tools_call(params: dict[str, Any]) -> dict[str, Any]:
    """Answer MCP `tools/call` by running the named handler.

    Every failure here is reported as a SUCCESSFUL JSON-RPC response carrying
    `isError: true`, never as a protocol error. That is the MCP contract for
    tool failure and it is what puts the message in front of the model: a
    transport-level error would be handled by the client and the model would
    only see that something went wrong, not what.

    Unknown tool, argument rejection (ValueError) and any other handler
    exception are therefore all caught. The catch-all is a deliberate RPC
    boundary -- an unhandled exception would otherwise kill the stdio loop and
    take the whole session down over one bad tool call -- and it logs the repr
    to stderr so the traceback-worthy detail is not lost.

    Args:
        params: JSON-RPC params; "name" selects the tool, "arguments" is
            forwarded to the handler unvalidated (schema enforcement is the
            handler's job).

    Returns:
        `{"content": [{"type": "text", ...}], "isError": bool}`.
    """
    name = str(params.get("name", ""))
    tool = TOOL_INDEX.get(name)
    if tool is None:
        return {"content": [{"type": "text", "text": f"unknown tool: {name}"}], "isError": True}
    arguments = params.get("arguments") or {}
    try:
        text = tool["handler"](arguments)
    except ValueError as exc:
        return {"content": [{"type": "text", "text": f"invalid request: {exc}"}], "isError": True}
    except Exception as exc:  # noqa: BLE001 -- RPC boundary: every handler error becomes a protocol error
        log(f"tool '{name}' raised: {exc!r}")
        return {"content": [{"type": "text", "text": f"tool error: {exc}"}], "isError": True}
    else:
        return {"content": [{"type": "text", "text": text}], "isError": False}


def handle_resources_list() -> dict[str, Any]:
    """Answer MCP `resources/list` with the catalogue's descriptive fields.

    Projects out `reader`, the callable that actually loads each resource's
    text. Listing is therefore cheap and touches no files -- content is only
    read when the client asks for a specific URI.

    Returns:
        `{"resources": [...]}` with uri, name, description and mimeType, in
        RESOURCES order.
    """
    listed = [
        {
            "uri": r["uri"],
            "name": r["name"],
            "description": r["description"],
            "mimeType": r["mimeType"],
        }
        for r in RESOURCES
    ]
    return {"resources": listed}


def handle_resources_read(params: dict[str, Any]) -> dict[str, Any]:
    """Answer MCP `resources/read` by invoking the URI's reader.

    Lookup is exact-match against the registered URI; there is no prefix or
    glob matching, so a client cannot reach a file the catalogue does not name.
    That is the read boundary for this server.

    The reader runs on every call with no caching, so the client always sees
    the file's current contents rather than a snapshot from server start.

    Args:
        params: JSON-RPC params; "uri" selects the resource.

    Returns:
        `{"contents": [{"uri", "mimeType", "text"}]}` -- a single-element list,
        as this server has no multi-part resources.

    Raises:
        ValueError: URI is not in the catalogue; `dispatch` turns it into a
            JSON-RPC error.
        OSError: The reader could not read its backing file -- a registered
            resource whose file has been moved or deleted.
    """
    uri = str(params.get("uri", ""))
    resource = RESOURCE_INDEX.get(uri)
    if resource is None:
        msg = f"unknown resource uri: {uri}"
        raise ValueError(msg)
    text = resource["reader"]()
    return {"contents": [{"uri": uri, "mimeType": resource["mimeType"], "text": text}]}


def dispatch(request: dict[str, Any]) -> dict[str, Any] | None:  # noqa: PLR0911  # JSON-RPC method router, splitting hurts readability
    """Route one JSON-RPC request. Returns a response, or None for notifications."""
    method = request.get("method")
    request_id = request.get("id")
    params = request.get("params") or {}

    # Notifications (no id) get no response.
    if request_id is None and isinstance(method, str) and method.startswith("notifications/"):
        return None

    try:
        if method == "initialize":
            return _result(request_id, handle_initialize(params))
        if method == "ping":
            return _result(request_id, {})
        if method == "tools/list":
            return _result(request_id, handle_tools_list())
        if method == "tools/call":
            return _result(request_id, handle_tools_call(params))
        if method == "resources/list":
            return _result(request_id, handle_resources_list())
        if method == "resources/read":
            return _result(request_id, handle_resources_read(params))
        if method == "prompts/list":
            return _result(request_id, handle_prompts_list())
        if method == "prompts/get":
            return _result(request_id, handle_prompts_get(params))
        if method == "resources/templates/list":
            return _result(request_id, {"resourceTemplates": []})
        return _error(request_id, ERR_METHOD_NOT_FOUND, f"method not found: {method}")
    except ValueError as exc:
        return _error(request_id, ERR_INVALID_PARAMS, str(exc))
    except Exception as exc:  # noqa: BLE001 -- RPC boundary: every handler error becomes a protocol error
        log(f"dispatch error for {method}: {exc!r}")
        return _error(request_id, ERR_INTERNAL, str(exc))


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


def selftest() -> int:
    """Exercise the dispatcher in-process (no client, no hardware, no build)."""
    checks: list[tuple[str, bool]] = []

    init = dispatch(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"protocolVersion": PROTOCOL_VERSION_DEFAULT},
        }
    )
    checks.append(
        (
            "initialize returns serverInfo",
            bool(init) and init["result"]["serverInfo"]["name"] == SERVER_NAME,
        )
    )

    tools = dispatch({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    names = {t["name"] for t in tools["result"]["tools"]}
    checks.append(("tools/list advertises every tool", names == set(TOOL_INDEX)))

    res = dispatch({"jsonrpc": "2.0", "id": 3, "method": "resources/list"})
    uris = {r["uri"] for r in res["result"]["resources"]}
    checks.append(("resources/list advertises every resource", uris == set(RESOURCE_INDEX)))

    call = dispatch(
        {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {"name": "list_apps", "arguments": {}},
        }
    )
    text = call["result"]["content"][0]["text"]
    checks.append(
        (
            "list_apps finds firmware apps",
            call["result"]["isError"] is False and "firmware app" in text,
        )
    )

    over = dispatch(
        {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "tools/call",
            "params": {"name": "repo_overview", "arguments": {}},
        }
    )
    checks.append(
        (
            "repo_overview mentions the target MCU",
            "R7KA8D2KFLCAC" in over["result"]["content"][0]["text"],
        )
    )

    flash = dispatch(
        {
            "jsonrpc": "2.0",
            "id": 6,
            "method": "tools/call",
            "params": {"name": "flash_app", "arguments": {"app": "blink"}},
        }
    )
    checks.append(
        (
            "flash_app withholds the write without confirm",
            "[dry run]" in flash["result"]["content"][0]["text"],
        )
    )

    doc = dispatch(
        {
            "jsonrpc": "2.0",
            "id": 7,
            "method": "resources/read",
            "params": {"uri": "ra8d2://doc/claude-md"},
        }
    )
    checks.append(
        ("resources/read returns CLAUDE.md", "CLAUDE" in doc["result"]["contents"][0]["text"])
    )

    unknown = dispatch({"jsonrpc": "2.0", "id": 8, "method": "no/such/method"})
    checks.append(
        ("unknown method -> method-not-found", unknown["error"]["code"] == ERR_METHOD_NOT_FOUND)
    )

    cite = dispatch(
        {
            "jsonrpc": "2.0",
            "id": 9,
            "method": "tools/call",
            "params": {"name": "hum_citation", "arguments": {"chapter": "38.2.3"}},
        }
    )
    checks.append(
        (
            "hum_citation emits a HUM skeleton",
            "HUM Ch 38.2.3" in cite["result"]["content"][0]["text"],
        )
    )

    badsim = dispatch(
        {
            "jsonrpc": "2.0",
            "id": 10,
            "method": "tools/call",
            "params": {"name": "sim_app", "arguments": {"app": "no_such_app"}},
        }
    )
    checks.append(
        ("sim_app rejects an unknown app (no build)", badsim["result"]["isError"] is True)
    )

    plist = dispatch({"jsonrpc": "2.0", "id": 11, "method": "prompts/list"})
    pnames = {p["name"] for p in plist["result"]["prompts"]}
    checks.append(("prompts/list advertises every prompt", pnames == set(PROMPT_INDEX)))

    pget = dispatch(
        {
            "jsonrpc": "2.0",
            "id": 12,
            "method": "prompts/get",
            "params": {"name": "audit_register_access", "arguments": {"code": "x"}},
        }
    )
    checks.append(
        (
            "prompts/get fills the template",
            "HUM" in pget["result"]["messages"][0]["content"]["text"],
        )
    )

    ok = True
    for label, passed in checks:
        print(f"  [{'PASS' if passed else 'FAIL'}] {label}")
        ok = ok and passed
    print(f"\nselftest: {'PASS' if ok else 'FAIL'} ({sum(p for _, p in checks)}/{len(checks)})")
    return 0 if ok else 1


def which(name: str) -> bool:
    """Return True if ``name`` resolves on PATH."""
    return _shutil_which(name) is not None


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
