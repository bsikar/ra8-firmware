# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The tools this server exposes, and the JSON schema that advertises them.

One function per tool, plus the :data:`TOOLS` table mapping the advertised
name to its schema and handler.  Implementation and schema live in the same
file on purpose: a tool whose schema drifts from its handler is invisible to
the server -- the client is told about a parameter nothing reads, or a
parameter the handler requires is never sent -- and nothing else in the
protocol layer can detect that.
"""

from __future__ import annotations

import subprocess
from typing import Any

from mcp_util import (
    REPO_ROOT,
    discover_apps,
    read_text,
    require_app,
    run_command,
    which,
)


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
        "simulate: make sim-" + app["name"] + "     (tools/ra8_emulator emulator)",
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
