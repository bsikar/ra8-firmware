#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exact-argv adapter for the deliberately non-variadic Just facade."""

from __future__ import annotations

import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from typing import NoReturn

Adapter = Callable[[list[str]], list[str]]
PLAN_ARGC = 3
START_ARGC = 4
STATUS_ARGC = 1
PHASE_ARGC = 2
BOARD_SELECTOR_ARGC = 2
BOARD_NO_ARG_ACTIONS = {
    "board_focus": ("focus",),
    "board_quick_wins": ("quick-wins",),
}
BOARD_SELECTOR_ACTIONS = {
    "board_track": "track",
    "board_epic": "epic",
    "board_issue": "issue",
}


def fail(message: str) -> NoReturn:
    """Raise one consistently constructed adapter error."""
    error = ValueError(message)
    raise error


def _optional(flag: str, value: str) -> list[str]:
    """Return one optional flag/value pair without shell reconstruction."""
    return [] if not value else [flag, value]


def _doctor(argv: list[str]) -> list[str]:
    """Translate the zero-argument doctor shape."""
    if argv:
        fail("doctor takes no Just arguments")
    return ["doctor"]


def _plan(argv: list[str]) -> list[str]:
    """Translate one explicit plan output mode."""
    if len(argv) != PLAN_ARGC:
        fail("plan requires notes, mode, and output slots")
    notes, mode, output = argv
    result = ["plan", notes]
    modes = {"summary": "--summary", "commands": "--emit-commands"}
    if mode in modes:
        result.append(modes[mode])
    elif mode == "json":
        if not output:
            fail("JSON output path is empty")
        result.extend(["--json", output])
    elif mode != "default":
        fail("unknown plan output mode")
    return result


def _start(argv: list[str]) -> list[str]:
    """Translate the fixed start preview/execute shape."""
    if len(argv) != START_ARGC:
        fail("start requires identifier, ref, root, and execute slots")
    identifier, ref, root, execute = argv
    result = ["start", identifier, "--ref", ref, *_optional("--ws-root", root)]
    if execute == "true":
        result.append("--execute")
    elif execute != "false":
        fail("invalid execute value")
    return result


def _status(argv: list[str]) -> list[str]:
    """Translate the fixed status shape."""
    if len(argv) != STATUS_ARGC:
        fail("status requires one root slot")
    return ["status", *_optional("--ws-root", argv[0])]


def _phase(action: str, argv: list[str]) -> list[str]:
    """Translate ready or landed without a generic flag tail."""
    if len(argv) != PHASE_ARGC:
        fail(f"{action} requires identifier and root slots")
    result = [action, argv[0], *_optional("--ws-root", argv[1])]
    if action == "ready":
        result.append("--run-ci")
    return result


def build_argv(argv: list[str]) -> list[str]:
    """Translate one fixed Just recipe shape into the public CLI argv."""
    if not argv:
        fail("missing Just action")
    board_tail = BOARD_NO_ARG_ACTIONS.get(argv[0])
    if board_tail is not None:
        if len(argv) != 1:
            fail(f"{argv[0]} takes no Just arguments")
        return ["board", *board_tail]
    board_view = BOARD_SELECTOR_ACTIONS.get(argv[0])
    if board_view is not None:
        if len(argv) != BOARD_SELECTOR_ARGC:
            fail(f"{argv[0]} requires one argument")
        return ["board", board_view, argv[1]]
    action, values = argv[0], argv[1:]
    adapters: dict[str, Adapter] = {
        "doctor": _doctor,
        "plan": _plan,
        "start": _start,
        "status": _status,
        "ready": lambda items: _phase("ready", items),
        "landed": lambda items: _phase("landed", items),
    }
    adapter = adapters.get(action)
    if adapter is None:
        fail(f"invalid Just action: {action}")
    return adapter(values)


def main(argv: list[str]) -> int:
    """Run isolated Python with the exact translated argv."""
    try:
        translated = build_argv(argv)
    except ValueError as exc:
        print(f"work Just adapter: {exc}", file=sys.stderr)
        return 2
    if translated[0] == "board":
        entrypoint = Path(__file__).resolve().with_name("work_board.py")
        result = subprocess.run(  # noqa: S603
            [sys.executable, "-I", str(entrypoint), *translated[1:]], check=False
        )
        return result.returncode

    if translated[0] == "board_move":
        entrypoint = Path(__file__).resolve().with_name("work_board_move.py")
        result = subprocess.run(  # noqa: S603
            ["/usr/bin/python3", "-I", str(entrypoint), translated[1], translated[2]], check=False
        )
        return result.returncode

    entrypoint = Path(__file__).resolve().with_name("work.py")
    result = subprocess.run(  # noqa: S603 -- fixed interpreter, entrypoint, and data argv
        [sys.executable, "-I", str(entrypoint), *translated], check=False
    )
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
