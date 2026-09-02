#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_roadmap_dashboard_freshness.py -- gate docs/ROADMAP_DASHBOARD.md against a regenerate.

``docs/ROADMAP_DASHBOARD.md`` is a COMMITTED, GENERATED historical artefact:
``just docs::dashboard`` writes it with ``scripts/report/roadmap_dashboard.py``, which
renders it purely from the closed evidence in ``docs/ROADMAP.md``. Current work
is tracked in GitHub issues and the project board. Previously, nothing
re-ran that generator and byte-compared the committed copy against ``HEAD``, so
it could silently drift out of step with ``ROADMAP.md`` the same way
``docs/INIT_ORDER_AUDIT.md`` did (#537) -- a generated doc that nothing
regenerates is a claim with no mechanism behind it.

This is the same "regenerate and gate" shape ``check_init_order_freshness.py``
uses. Like that generator, this one is hardware-free and reads a committed
markdown file, so it is byte-stable across runs and lives in its own ``fast``
gate rather than the ``slow`` ``artefact-freshness`` group that consumes build
output.

The comparison is against the tracked candidate bytes. In CI that is the clean
checkout; the pre-commit policy runs inside its staged snapshot. Comparing to
``HEAD`` would make a corrected generated document fail until after the commit
that this gate is meant to guard. ``--selftest`` drives the verdict logic in
both directions and floors the real regenerate at ``DRIVER_FLOOR`` drivers, so
a collapsed parser fails instead of reporting a clean, empty dashboard.

Scope note: the generator also writes ``docs/badges/*.svg`` from the same
source; this gate owns the markdown dashboard named in #537. The badges are a
separate artefact and out of scope here.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "report"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

import roadmap_dashboard
from git_environment import isolated_git_environment, trusted_git_executable
from selftest_assert import expect, report

REPO_ROOT = Path(__file__).resolve().parents[2]
ARTEFACT = "docs/ROADMAP_DASHBOARD.md"
SOURCE = "docs/ROADMAP.md"

# Non-vacuity floor: the historical record contains 45 drivers. A parse that
# sees fewer has collapsed (an empty or truncated ROADMAP.md, or a broken
# heading regex), and a dashboard rendered from it would be quietly wrong. The
# floor is below the archived count so its only job is to reject a vacuous scan.
DRIVER_FLOOR = 30

REMEDIATION = "Run `just docs::dashboard` and commit docs/ROADMAP_DASHBOARD.md."


def _git(repo_root: Path, *args: str) -> subprocess.CompletedProcess[bytes] | None:
    """Run a fixed Git operation with the resolved executable and no shell."""
    git_bin = trusted_git_executable()
    return subprocess.run(  # noqa: S603 -- resolved executable, fixed argv, no shell
        [git_bin, "-C", str(repo_root), *args],
        capture_output=True,
        check=False,
    )


def count_drivers(repo_root: Path) -> int:
    """Return how many drivers the generator parses from ``repo_root``'s ROADMAP.md.

    Args:
        repo_root: Repository root whose ``docs/ROADMAP.md`` is parsed.

    Returns:
        The number of ``### <driver>`` sections the generator recognises.
    """
    text = (repo_root / SOURCE).read_text(encoding="utf-8")
    return len(roadmap_dashboard.parse_roadmap(text))


def regenerate(repo_root: Path) -> bytes:
    """Return the dashboard ``roadmap_dashboard`` would write for ``repo_root``.

    Built from the generator's own public helpers (``parse_roadmap`` +
    ``render_dashboard``) rather than by shelling out, so the gate and the
    generator cannot render differently, and encoded ``utf-8`` to match the
    bytes the generator writes to disk.

    Args:
        repo_root: Repository root whose ``docs/ROADMAP.md`` is rendered.

    Returns:
        The rendered Markdown dashboard as bytes.
    """
    text = (repo_root / SOURCE).read_text(encoding="utf-8")
    drivers = roadmap_dashboard.parse_roadmap(text)
    return roadmap_dashboard.render_dashboard(drivers).encode("utf-8")


def tracked_candidate(repo_root: Path, rel: str) -> bytes | None:
    """Return tracked candidate bytes for ``rel``, or None when unavailable.

    Args:
        repo_root: Repository whose ``HEAD`` is read.
        rel: Repo-relative POSIX path of the committed artefact.

    Returns:
        Candidate bytes, or None when the path is untracked or absent.
    """
    result = _git(repo_root, "ls-files", "--error-unmatch", "--", rel)
    path = repo_root / rel
    if result is None or result.returncode != 0 or not path.is_file():
        return None
    return path.read_bytes()


def drift(committed: bytes | None, fresh: bytes) -> str | None:
    """Describe how ``committed`` differs from ``fresh``, or None when identical.

    This is the verdict the gate turns into its exit code; the selftest drives
    it in both directions.

    Args:
        committed: Bytes committed at ``HEAD``, or None when untracked.
        fresh: Bytes the generator just produced.

    Returns:
        A one-line drift description, or None when the two are byte-identical.
    """
    if committed is None:
        return f"committed copy is not tracked at HEAD; cannot verify freshness. {REMEDIATION}"
    if committed == fresh:
        return None
    return (
        f"stale: committed {len(committed)} bytes differ from the "
        f"{len(fresh)}-byte regenerate of {SOURCE}. {REMEDIATION}"
    )


def check(repo_root: Path = REPO_ROOT) -> int:
    """Fail when the committed dashboard differs from a fresh regenerate.

    Args:
        repo_root: Repository to render and whose ``HEAD`` copy is compared.

    Returns:
        0 when the committed copy is byte-identical to the regenerate, 1
        otherwise.
    """
    fresh = regenerate(repo_root)
    verdict = drift(tracked_candidate(repo_root, ARTEFACT), fresh)
    if verdict is None:
        print(
            f"check_roadmap_dashboard_freshness.py: clean -- {ARTEFACT} "
            f"matches a fresh regenerate of {SOURCE}."
        )
        return 0
    sys.stderr.write(f"check_roadmap_dashboard_freshness.py: {ARTEFACT}: {verdict}\n")
    return 1


def _check_candidate_cases(failures: list[str]) -> None:
    """Prove candidate tracking reads working bytes and rejects untracked files."""
    with isolated_git_environment(), tempfile.TemporaryDirectory() as raw_tmp:
        root = Path(raw_tmp)
        (root / "docs").mkdir()
        candidate = root / ARTEFACT
        candidate.write_bytes(b"before\n")
        init = _git(root, "init", "-q")
        add = _git(root, "add", ARTEFACT)
        git_ready = (
            init is not None and init.returncode == 0 and add is not None and add.returncode == 0
        )
        expect(git_ready, "the selftest Git fixture initializes", failures)
        if git_ready:
            expect(
                tracked_candidate(root, ARTEFACT) == b"before\n",
                "a tracked candidate is readable before commit",
                failures,
            )
            candidate.write_bytes(b"after\n")
            expect(
                tracked_candidate(root, ARTEFACT) == b"after\n",
                "the checker reads candidate bytes instead of stale HEAD bytes",
                failures,
            )
            expect(
                tracked_candidate(root, "docs/untracked.md") is None,
                "an untracked candidate is rejected",
                failures,
            )


def selftest() -> int:
    """Prove the verdict fires on drift, stays quiet on a match, and is non-vacuous.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures: list[str] = []
    fresh = regenerate(REPO_ROOT)

    # Non-vacuity floor: the real parse must see the whole roadmap, not a
    # collapsed heading scan. A dashboard rendered from an empty parse would
    # otherwise read as clean.
    drivers = count_drivers(REPO_ROOT)
    expect(
        drivers >= DRIVER_FLOOR,
        f"live parse feeds the dashboard {drivers} driver(s) (floor {DRIVER_FLOOR})",
        failures,
    )
    expect(regenerate(REPO_ROOT) == fresh, "the generator is byte-deterministic", failures)

    # Both directions of the verdict the gate turns into its exit code.
    expect(
        drift(fresh, fresh) is None,
        "a committed copy equal to the regenerate is clean",
        failures,
    )
    expect(
        drift(fresh + b"tampered\n", fresh) is not None,
        "a drifted committed copy is reported",
        failures,
    )
    expect(drift(None, fresh) is not None, "an untracked committed copy is reported", failures)

    _check_candidate_cases(failures)

    return report(failures)


def main() -> int:
    """Dispatch to the freshness check or its selftest.

    Returns:
        The exit code of whichever mode ran.
    """
    if "--selftest" in sys.argv[1:]:
        return selftest()
    return check()


if __name__ == "__main__":
    sys.exit(main())
