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

from collections.abc import Callable
from typing import Any

from mcp_content import PROMPT_INDEX, RESOURCE_INDEX
from mcp_protocol import (
    ERR_METHOD_NOT_FOUND,
    PROTOCOL_VERSION_DEFAULT,
    SERVER_NAME,
    dispatch,
)
from mcp_tools import TOOL_INDEX


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
        lambda r: "[dry run]" in _text(r),
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
        lambda r: "CLAUDE" in r["result"]["contents"][0]["text"],  # AI-OK: served doc is CLAUDE.md
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

    ok = True
    for label, passed in checks:
        print(f"  [{'PASS' if passed else 'FAIL'}] {label}")
        ok = ok and passed
    print(f"\nselftest: {'PASS' if ok else 'FAIL'} ({sum(p for _, p in checks)}/{len(checks)})")
    return 0 if ok else 1
