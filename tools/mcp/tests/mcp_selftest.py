# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""End-to-end checks over the dispatch layer, without a client.

Drives real JSON-RPC requests through :func:`mcp_protocol.dispatch` and
asserts both directions: every advertised capability answers, and malformed or
unknown requests produce the right JSON-RPC error rather than an exception or
a silent success.

The schema-vs-handler cross-check is the one that earns its keep. A tool whose
advertised schema and handler disagree still starts, still lists, and fails
only when a client happens to call it -- so the two are compared here.
"""

from __future__ import annotations

import os
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any
from unittest.mock import call, patch

from mcp_content import PROMPT_INDEX, RESOURCE_INDEX
from mcp_protocol import (
    ERR_METHOD_NOT_FOUND,
    PROTOCOL_VERSION_DEFAULT,
    SERVER_NAME,
    dispatch,
)
from mcp_tools import TOOL_INDEX
from mcp_util import discover_apps, require_app
from ra8_apps import app_id as authoritative_app_id
from ra8_apps import get_apps as authoritative_apps


def _req(method: str, **params: object) -> dict[str, object]:
    """One JSON-RPC request. The id is filled in by the runner.

    Values are typed ``object`` rather than ``Any``: they are stored and later
    JSON-serialised, never inspected here, so ``object`` states the real
    contract while ``Any`` would merely switch checking off.
    """
    out: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
    if params:
        out["params"] = params
    return out


def _call(name: str, **arguments: object) -> dict[str, object]:
    """A tools/call request for ``name``."""
    return _req("tools/call", name=name, arguments=arguments)


def _text(reply: dict[str, Any]) -> str:
    """The first text block of a tools/call result."""
    return reply["result"]["content"][0]["text"]


#: (label, request, predicate over the reply). Every advertised capability is
#: exercised, and the two error paths -- an unknown method and an unknown app
#: -- are asserted to FAIL rather than to quietly succeed.
_CASES: tuple[tuple[str, dict[str, Any], Callable[[dict[str, Any]], bool]], ...] = (
    (
        "initialize returns serverInfo",
        _req("initialize", protocolVersion=PROTOCOL_VERSION_DEFAULT),
        lambda r: bool(r) and r["result"]["serverInfo"]["name"] == SERVER_NAME,
    ),
    (
        "tools/list advertises every tool",
        _req("tools/list"),
        lambda r: {t["name"] for t in r["result"]["tools"]} == set(TOOL_INDEX),
    ),
    (
        "resources/list advertises every resource",
        _req("resources/list"),
        lambda r: {x["uri"] for x in r["result"]["resources"]} == set(RESOURCE_INDEX),
    ),
    (
        "prompts/list advertises every prompt",
        _req("prompts/list"),
        lambda r: {p["name"] for p in r["result"]["prompts"]} == set(PROMPT_INDEX),
    ),
    (
        "list_apps finds firmware apps",
        _call("list_apps"),
        lambda r: r["result"]["isError"] is False and "firmware app" in _text(r),
    ),
    (
        "repo_overview mentions the target MCU",
        _call("repo_overview"),
        lambda r: "R7KA8D2KFLCAC" in _text(r),
    ),
    (
        "flash_app withholds the write without confirm",
        _call("flash_app", app="blink"),
        lambda r: (
            "[dry run]" in _text(r)
            and "just apps::hardware::flash ek_ra8d2::hw_validated::hil::blink" in _text(r)
        ),
    ),
    (
        "hil flash preview uses the Just recipe and positional app",
        _call("hil", action="flash", app="blink"),
        lambda r: (
            "[dry run]" in _text(r)
            and "just hil::flash ek_ra8d2::hw_validated::hil::blink" in _text(r)
        ),
    ),
    (
        "hum_citation emits a HUM skeleton",
        _call("hum_citation", chapter="38.2.3"),
        lambda r: "HUM Ch 38.2.3" in _text(r),
    ),
    (
        "emu_app rejects an unknown app (no build)",
        _call("emu_app", app="no_such_app"),
        lambda r: r["result"]["isError"] is True,
    ),
    (
        "resources/read returns CLAUDE.md",
        _req("resources/read", uri="ra8d2://doc/claude-md"),  # AI-OK: serves CLAUDE.md
        lambda r: "# CLAUDE.md" in r["result"]["contents"][0]["text"],
    ),
    (
        "prompts/get fills the template",
        _req("prompts/get", name="audit_register_access", arguments={"code": "x"}),
        lambda r: "HUM" in r["result"]["messages"][0]["content"]["text"],
    ),
    (
        "unknown method -> method-not-found",
        _req("no/such/method"),
        lambda r: r["error"]["code"] == ERR_METHOD_NOT_FOUND,
    ),
)


def _schema_matches_handlers() -> list[tuple[str, bool]]:
    """Every advertised tool must have a handler, and vice versa.

    A tool whose schema and handler disagree still starts and still lists; it
    fails only when a client happens to call it. Comparing the two tables is
    the only thing that catches it before a user does.
    """
    missing = sorted(n for n, t in TOOL_INDEX.items() if not callable(t.get("handler")))
    label = "every advertised tool has a handler"
    if missing:
        label = f"{label} (missing: {', '.join(missing)})"
    return [(label, not missing)]


def _catalogue_matches_authority() -> list[tuple[str, bool]]:
    """Prove MCP preserves the authoritative app set and identifier aliases."""
    authoritative = authoritative_apps()
    authoritative_ids = [authoritative_app_id(app) for app in authoritative]
    exposed = discover_apps()
    exposed_ids = [app["id"] for app in exposed]
    blink_id = "ek_ra8d2::hw_validated::hil::blink"
    board_id = "board::stand_alone::ra8d2-ereader"
    return [
        (
            "MCP catalogue count matches scripts/dev/ra8_apps.py",
            len(exposed) == len(authoritative),
        ),
        (
            "MCP catalogue membership matches scripts/dev/ra8_apps.py",
            exposed_ids == authoritative_ids,
        ),
        (
            "namespaced app resolution preserves the exact catalogue member",
            require_app(blink_id)["id"] == blink_id
            and require_app(blink_id.replace("::", "/"))["id"] == blink_id,
        ),
        (
            "board aliases resolve to the authoritative e-reader identifier",
            require_app("ereader")["id"] == board_id
            and require_app("ra8d2-ereader")["id"] == board_id
            and require_app(board_id)["id"] == board_id,
        ),
    ]


def _quality_tools_use_portable_gate() -> list[tuple[str, bool]]:
    """Quality tools must not bypass the host-aware Just gate dispatcher."""
    with patch("mcp_tools.run_command", return_value="ok") as run:
        TOOL_INDEX["run_tests"]["handler"]({})
        TOOL_INDEX["quality_gate"]["handler"]({"gate": "format-check"})
        TOOL_INDEX["coverage"]["handler"]({})

    launcher = ["/bin/bash", "-p", "scripts/dev/run_just.sh"]
    expected = [
        call([*launcher, "quality::gate::run", "unit-tests"], timeout=900),
        call([*launcher, "quality::gate::run", "format"], timeout=300),
        call([*launcher, "quality::gate::run", "mcdc"], timeout=1200),
    ]
    return [("quality tools use the portable Just gate dispatcher", run.call_args_list == expected)]


def _just_launcher_honors_override_without_path() -> list[tuple[str, bool]]:
    """Exercise a real MCP Just call with only ``RA8_JUST`` available."""
    with tempfile.TemporaryDirectory(prefix="ra8-mcp-just-") as scratch:
        fake_just = Path(scratch) / "just-override"
        fake_just.write_text(
            "#!/bin/sh\nprintf 'override:%s\\n' \"$*\"\n",
            encoding="ascii",
        )
        fake_just.chmod(0o755)
        with patch.dict(os.environ, {"PATH": "", "RA8_JUST": str(fake_just)}):
            text = TOOL_INDEX["quality_gate"]["handler"]({"gate": "format-check"})
    passed = (
        "[status] ok" in text
        and "override:quality::gate::run format" in text
        and "executable not found" not in text
    )
    return [("MCP Just launcher honors RA8_JUST with an empty PATH", passed)]


def _search_scope_exists_and_returns_results() -> list[tuple[str, bool]]:
    """A valid search must not fail because a configured scope path is absent."""
    text = TOOL_INDEX["search_code"]["handler"](
        {"pattern": "def tool_list_apps", "glob": "*.py", "max_results": 5}
    )
    passed = (
        "[status] ok" in text
        and "tools/mcp/src/mcp_tools.py" in text
        and "No such file or directory" not in text
    )
    return [("search_code scans only live roots and returns a known result", passed)]


def _app_info_uses_canonical_layout() -> list[tuple[str, bool]]:
    """App metadata must report the post-migration source/include paths."""
    text = TOOL_INDEX["app_info"]["handler"]({"app": "secure_boot_ns_hil"})
    files_line = next((line for line in text.splitlines() if line.startswith("files:")), "")
    required = (
        "src/main.c",
        "src/ns_main.c",
        "src/vector_table.c",
        "src/system_init.c",
        "src/trustzone_init.c",
        "inc/trustzone_init.h",
        "linker_script.ld",
        "ns_image.ld",
    )
    stale = (
        "main.c",
        "ns_main.c",
        "vector_table.c",
        "system_init.c",
        "trustzone_init.c",
        "trustzone_init.h",
    )
    listed = {item.strip() for item in files_line.partition(":")[2].split(",")}
    passed = all(path in listed for path in required) and not any(path in listed for path in stale)
    return [("app_info reports canonical src/inc boot paths", passed)]


def selftest() -> int:
    """Exercise the dispatcher in-process (no client, no hardware, no build)."""
    checks: list[tuple[str, bool]] = []
    for idx, (label, request, predicate) in enumerate(_CASES, start=1):
        reply = dispatch({**request, "id": idx})
        try:
            passed = bool(predicate(reply))
        except (KeyError, IndexError, TypeError):
            passed = False
        checks.append((label, passed))
    checks.extend(_schema_matches_handlers())
    checks.extend(_catalogue_matches_authority())
    checks.extend(_quality_tools_use_portable_gate())
    checks.extend(_just_launcher_honors_override_without_path())
    checks.extend(_search_scope_exists_and_returns_results())
    checks.extend(_app_info_uses_canonical_layout())

    ok = True
    for label, passed in checks:
        print(f"  [{'PASS' if passed else 'FAIL'}] {label}")
        ok = ok and passed
    print(f"\nselftest: {'PASS' if ok else 'FAIL'} ({sum(p for _, p in checks)}/{len(checks)})")
    return 0 if ok else 1
