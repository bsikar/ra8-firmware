#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_init_order_freshness.py -- gate docs/INIT_ORDER_AUDIT.md against a regenerate.

``docs/INIT_ORDER_AUDIT.md`` is a COMMITTED, GENERATED artefact: ``mk/docs.mk``
writes it with ``audit_init_order.py --report docs/INIT_ORDER_AUDIT.md``.
Nothing re-ran that generator and byte-compared the committed copy, so it
silently drifted -- it claimed 11 apps audited while the tree held 217, for as
long as the discovery glob was depth-capped (#190). A generated doc that
nothing regenerates is a claim with no mechanism behind it, and this one is
cited from ``docs/qualification/`` (#537).

This is the same "regenerate and gate" shape ``check_generated_artefacts.py``
uses for the MC/DC and Doxygen gap docs. That gate is ``slow`` because its
MC/DC half consumes the ``mcdc`` build output; this generator is hardware-free
and reads a sorted glob, so it is byte-stable across runs and lives in its own
``fast`` gate rather than that group.

The comparison is against the copy committed at ``HEAD`` (via ``git show``), so
a dirty working tree cannot mask drift. ``--selftest`` drives the verdict logic
in both directions and floors the real regenerate at ``audit_init_order``'s
app-discovery floor, so a collapsed generator fails instead of reporting a
clean, empty tree.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import audit_init_order
from selftest_assert import expect, report

REPO_ROOT = Path(__file__).resolve().parents[2]
ARTEFACT = "docs/INIT_ORDER_AUDIT.md"
REMEDIATION = (
    "Run `make audit-init` (or `python3 scripts/checks/audit_init_order.py "
    "--report docs/INIT_ORDER_AUDIT.md`) and commit docs/INIT_ORDER_AUDIT.md."
)


def regenerate(repo_root: Path) -> bytes:
    """Return the report ``audit_init_order`` would write for ``repo_root``.

    Built from the generator's own public helpers rather than by shelling out,
    so the gate and the generator cannot render differently.

    Args:
        repo_root: Repository root whose ``examples/`` tree is audited.

    Returns:
        The rendered Markdown report as ASCII bytes.
    """
    apps = audit_init_order.collect_apps(repo_root)
    audits = [audit_init_order.audit_app(app, main) for app, main in apps]
    return audit_init_order.render_markdown(audits, repo_root).encode("ascii")


def committed_at_head(repo_root: Path, rel: str) -> bytes | None:
    """Return the bytes of ``rel`` committed at ``HEAD``, or None when untracked.

    Args:
        repo_root: Repository whose ``HEAD`` is read.
        rel: Repo-relative POSIX path of the committed artefact.

    Returns:
        The committed bytes, or None when the path is not tracked at ``HEAD``.
    """
    result = subprocess.run(  # noqa: S603 -- fixed argv, no shell
        ["git", "-C", str(repo_root), "show", f"HEAD:{rel}"],  # noqa: S607 -- git from PATH
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout


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
        f"{len(fresh)}-byte regenerate. {REMEDIATION}"
    )


def check(repo_root: Path = REPO_ROOT) -> int:
    """Fail when the committed report differs from a fresh regenerate.

    Args:
        repo_root: Repository to audit and whose ``HEAD`` copy is compared.

    Returns:
        0 when the committed copy is byte-identical to the regenerate, 1
        otherwise.
    """
    fresh = regenerate(repo_root)
    verdict = drift(committed_at_head(repo_root, ARTEFACT), fresh)
    if verdict is None:
        print(f"check_init_order_freshness.py: clean -- {ARTEFACT} matches a fresh regenerate.")
        return 0
    sys.stderr.write(f"check_init_order_freshness.py: {ARTEFACT}: {verdict}\n")
    return 1


def selftest() -> int:
    """Prove the verdict fires on drift, stays quiet on a match, and is non-vacuous.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures: list[str] = []
    fresh = regenerate(REPO_ROOT)

    # Non-vacuity floor: the real generator must see the whole app tree, not a
    # collapsed glob. audit_init_order owns the floor; reuse it so a re-capped
    # discovery fails here instead of reporting a clean, tiny report.
    app_count = len(audit_init_order.collect_apps(REPO_ROOT))
    expect(
        app_count >= audit_init_order.APP_FLOOR,
        f"live discovery feeds the report {app_count} app(s) (floor {audit_init_order.APP_FLOOR})",
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
