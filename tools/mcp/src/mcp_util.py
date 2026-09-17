# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shared plumbing for the MCP server: subprocess capture, file reads, app discovery.

Everything here is deliberately dependency-free -- the server ships with zero
third-party packages so it runs against a bare ``python3`` -- and everything
that reaches the outside world is bounded: :data:`MAX_OUTPUT_CHARS` caps what a
runaway build log can hand back to a client, and :func:`run_command` always
carries a timeout.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from shutil import which as _shutil_which

# Repo root = three parents up from this file (tools/mcp/src/<this>).
REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "dev"))

import ra8_apps as _ra8_apps  # noqa: E402  # authoritative app catalogue lives in scripts/dev

# Upper bound on captured subprocess output returned to the client, so a
# runaway build log can never blow up the assistant's context window.
MAX_OUTPUT_CHARS = 12000


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
        proc = subprocess.run(  # noqa: S603  # trusted: fixed just/tool argv from internal registry
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
# Firmware app catalogue (adapts the authoritative scripts/dev/ra8_apps.py data)
# ---------------------------------------------------------------------------
def _mcp_app(app: dict[str, str]) -> dict[str, str]:
    """Adapt one authoritative app record to the MCP field names."""
    return {
        "id": _ra8_apps.app_id(app),
        "name": app["name"],
        "group": app["group"],
        "dir": app["rel_dir"],
        "description": app["desc"],
        "toolchain": app["toolchain"],
    }


def discover_apps() -> list[dict[str, str]]:
    """Return every app from the authoritative catalogue without basename loss."""
    return [_mcp_app(app) for app in _ra8_apps.get_apps()]


def require_app(name: str) -> dict[str, str]:
    """Resolve a firmware app name or raise ``ValueError`` if it is unknown.

    Validating against the discovered set keeps an arbitrary string from ever
    reaching ``just`` as a recipe argument, so a tool call cannot inject a
    foreign application name.
    """
    resolved = _ra8_apps.find_app(name)
    app = _mcp_app(resolved) if resolved is not None else None
    if app is None:
        sample = ", ".join(candidate["id"] for candidate in discover_apps()[:12])
        msg = f"unknown app '{name}'. Use the list_apps tool. Examples: {sample} ..."
        raise ValueError(msg)
    return app


def which(name: str) -> bool:
    """Return True if ``name`` resolves on PATH."""
    return _shutil_which(name) is not None
