# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Read-only ``gh`` probes, and the templates ``work plan`` emits for a human to run.

Two jobs, one module, and the split between them is the point.

The probe half is what ``work doctor`` calls. It runs ``gh --version`` and
``gh auth status`` and nothing else, and it reports three-valued results:
:data:`STATE_OK`, :data:`STATE_DEGRADED` and :data:`STATE_UNAVAILABLE`. The
distinction that matters is between "gh is here and its token lacks the
``project`` scope" and "gh is not here, or could not answer at all". Collapsing
those two into one failure is how an agent ends up believing a board mutation
is impossible when the real problem is that it is standing on the wrong host.

The template half never runs anything. ``work plan --emit-commands`` renders a
shell script to stdout for a person to read and run themselves, from a host
whose token carries the scope. Every project, field and option id in that
script is discovered BY NAME at run time through ``gh api graphql``; there is
not one hardcoded node id anywhere, because a pasted id is a fact about one
board on one day and it fails silently when it stops being true.

Nothing in this module writes anything, anywhere.
"""

from __future__ import annotations

from dataclasses import dataclass
from shutil import which

from work_git import ToolMissingError, WorkError, run_process

#: The probe answered and everything it needs is present.
STATE_OK = "OK"

#: The probe answered, but something it found limits what is possible.
STATE_DEGRADED = "DEGRADED"

#: The probe could not answer at all.
STATE_UNAVAILABLE = "UNAVAILABLE"

#: The OAuth scope a GitHub Projects mutation needs.
PROJECT_SCOPE = "project"

_SCOPES_MARKER = "Token scopes:"


@dataclass(frozen=True)
class Probe:
    """One three-valued readiness answer."""

    name: str
    state: str
    detail: str


def gh_executable() -> str | None:
    """Return the resolved path to ``gh``, or None when it is not installed.

    Returns:
        An absolute path, or None.
    """
    return which("gh")


def probe_version() -> Probe:
    """Report whether ``gh`` is installed and which version answered.

    Returns:
        :data:`STATE_OK` with the version line, or :data:`STATE_UNAVAILABLE`
        naming which of "not installed" or "did not answer" applies.
    """
    found = gh_executable()
    if found is None:
        return Probe("gh", STATE_UNAVAILABLE, "gh is not installed on PATH")
    try:
        done = run_process([found, "--version"], timeout=20)
    except (ToolMissingError, WorkError) as exc:
        return Probe("gh", STATE_UNAVAILABLE, f"gh --version did not answer: {exc}")
    if not done.ok:
        return Probe("gh", STATE_UNAVAILABLE, f"gh --version exited {done.returncode}")
    first = (done.stdout.strip().splitlines() or [""])[0]
    return Probe("gh", STATE_OK, first)


def parse_scopes(text: str) -> list[str] | None:
    """Extract the token scope list from ``gh auth status`` output.

    Args:
        text: Combined stdout and stderr of ``gh auth status``.

    Returns:
        The scope names in the order reported, or None when no scope line was
        present at all. An empty list is a real answer and means the token
        reported no scopes.
    """
    for line in text.splitlines():
        if _SCOPES_MARKER not in line:
            continue
        tail = line.split(_SCOPES_MARKER, 1)[1]
        return [item.strip().strip("'\"") for item in tail.split(",") if item.strip().strip("'\"")]
    return None


def probe_auth() -> Probe:
    """Report whether a ``gh`` token is present and whether it can mutate a board.

    Returns:
        :data:`STATE_OK` when the token carries :data:`PROJECT_SCOPE`,
        :data:`STATE_DEGRADED` when it authenticates without that scope, and
        :data:`STATE_UNAVAILABLE` when gh is missing or the status command
        could not be trusted to answer.
    """
    found = gh_executable()
    if found is None:
        return Probe("gh auth", STATE_UNAVAILABLE, "gh is not installed, so no token can be read")
    try:
        done = run_process([found, "auth", "status", "--hostname", "github.com"], timeout=30)
    except (ToolMissingError, WorkError) as exc:
        return Probe("gh auth", STATE_UNAVAILABLE, f"gh auth status did not answer: {exc}")
    combined = f"{done.stdout}\n{done.stderr}"
    scopes = parse_scopes(combined)
    if not done.ok and scopes is None:
        detail = _first_useful_line(combined) or f"gh auth status exited {done.returncode}"
        return Probe("gh auth", STATE_UNAVAILABLE, f"not authenticated: {detail}")
    if scopes is None:
        return Probe("gh auth", STATE_UNAVAILABLE, "gh auth status reported no token scopes line")
    if PROJECT_SCOPE not in scopes:
        detail = (
            f"token scopes are [{', '.join(scopes)}] with no {PROJECT_SCOPE} scope. "
            "Board mutations must be run from a host whose token has it."
        )
        return Probe("gh auth", STATE_DEGRADED, detail)
    return Probe("gh auth", STATE_OK, f"token scopes are [{', '.join(scopes)}]")


def _first_useful_line(text: str) -> str:
    """Return the first non-empty line of ``text``, for a one-line diagnostic.

    Args:
        text: Redacted combined output.

    Returns:
        The first non-empty stripped line, or an empty string.
    """
    for line in text.splitlines():
        if line.strip():
            return line.strip()
    return ""
