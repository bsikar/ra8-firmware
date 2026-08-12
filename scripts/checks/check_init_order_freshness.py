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

import difflib
import os
import subprocess
import sys
import tempfile
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
GENERATOR = "scripts/checks/audit_init_order.py"

# How much of the unified diff the verdict prints. Enough to name the drifting
# apps without turning a wholesale regeneration into a wall of log.
MAX_DIFF_LINES = 24


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


def diff_excerpt(committed: bytes, fresh: bytes) -> str:
    """Return a bounded unified diff from the committed copy to the regenerate.

    Args:
        committed: Bytes committed at ``HEAD``.
        fresh: Bytes the generator just produced.

    Returns:
        At most ``MAX_DIFF_LINES`` lines of unified diff, with a trailing count
        of whatever was suppressed.
    """
    lines = list(
        difflib.unified_diff(
            committed.decode("ascii", errors="replace").splitlines(),
            fresh.decode("ascii", errors="replace").splitlines(),
            fromfile=f"{ARTEFACT} (committed at HEAD)",
            tofile=f"{ARTEFACT} (fresh regenerate)",
            lineterm="",
        )
    )
    shown = lines[:MAX_DIFF_LINES]
    if len(lines) > MAX_DIFF_LINES:
        shown.append(f"    ... {len(lines) - MAX_DIFF_LINES} further diff line(s) suppressed")
    return "\n".join(shown)


def drift(committed: bytes | None, fresh: bytes) -> str | None:
    """Describe how ``committed`` differs from ``fresh``, or None when identical.

    This is the verdict the gate turns into its exit code; the selftest drives
    it in both directions.

    The description carries a real diff, not just the two byte counts. Drift in
    this report is routinely SIZE-IDENTICAL: deleting one line from an example's
    file header moves an init call from ``L402`` to ``L401``, which is the same
    width, so the old wording read "committed 31141 bytes differ from the
    31141-byte regenerate" and looked like a paradox. Three separate green-up
    attempts diagnosed that as generator nondeterminism instead of the ordinary
    staleness it was. A verdict that cannot be acted on is a verdict that gets
    explained away, so the drift names itself now.

    Args:
        committed: Bytes committed at ``HEAD``, or None when untracked.
        fresh: Bytes the generator just produced.

    Returns:
        A drift description, or None when the two are byte-identical.
    """
    if committed is None:
        return f"committed copy is not tracked at HEAD; cannot verify freshness. {REMEDIATION}"
    if committed == fresh:
        return None
    return (
        f"stale: committed {len(committed)} bytes differ from the "
        f"{len(fresh)}-byte regenerate. {REMEDIATION}\n"
        f"{diff_excerpt(committed, fresh)}"
    )


def generate_via_cli(repo_root: Path, env_overrides: dict[str, str]) -> bytes:
    """Run the generator's own CLI under ``env_overrides`` and return its report.

    Args:
        repo_root: Repository root passed through to the generator.
        env_overrides: Environment entries layered over the current environment.

    Returns:
        The bytes the CLI wrote to its ``--report`` path.

    Raises:
        RuntimeError: When the CLI wrote no report at all.
    """
    env = dict(os.environ)
    env.update(env_overrides)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "INIT_ORDER_AUDIT.md"
        subprocess.run(  # noqa: S603 -- fixed argv, no shell
            [sys.executable, str(repo_root / GENERATOR), "--report", str(out)],
            cwd=str(repo_root),
            env=env,
            capture_output=True,
            check=False,
        )
        if not out.is_file():
            msg = f"{GENERATOR} wrote no report under {env_overrides}"
            raise RuntimeError(msg)
        return out.read_bytes()


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


def selftest_generator_stability(fresh: bytes, failures: list[str]) -> None:
    """Assert the generator is non-vacuous and stable across environments.

    Args:
        fresh: The bytes ``regenerate`` just produced for ``REPO_ROOT``.
        failures: Accumulator every ``expect`` here appends its misses to.
    """
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

    # ...and deterministic ACROSS environments, not merely within one process.
    # Repeating regenerate() in the same interpreter cannot see a locale-
    # dependent collation, a hash-seed-dependent iteration order or anything
    # else the environment fixes once at start-up, so on its own it licenses a
    # "cross-machine nondeterminism" theory it can never refute. Two CLI runs
    # under deliberately different settings can. The CLI is also what
    # `make audit-init` and mk/docs.mk invoke, so this pins the second half of
    # the contract too: the bytes the gate DEMANDS are the bytes the documented
    # remediation PRODUCES. Nothing proved that before -- regenerate() builds
    # the report from the generator's helpers, and a divergence there would
    # have left the gate asking for a file no command in the tree could write.
    cli_c = generate_via_cli(REPO_ROOT, {"LC_ALL": "C", "PYTHONHASHSEED": "0"})
    cli_utf8 = generate_via_cli(REPO_ROOT, {"LC_ALL": "C.UTF-8", "PYTHONHASHSEED": "1"})
    expect(
        cli_c == cli_utf8,
        "the generator's CLI renders identically under two locales and hash seeds",
        failures,
    )
    expect(
        cli_c == fresh,
        "the CLI's report is byte-identical to the bytes this gate demands",
        failures,
    )


def selftest_verdict_directions(fresh: bytes, failures: list[str]) -> None:
    """Assert the drift verdict fires, stays quiet, and localises what drifted.

    Args:
        fresh: The bytes ``regenerate`` just produced for ``REPO_ROOT``.
        failures: Accumulator every ``expect`` here appends its misses to.
    """
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

    # The drift that actually happens here is SIZE-IDENTICAL -- an init call
    # sliding from L402 to L401 when an unrelated commit deletes a header line.
    # Assert the verdict both FIRES on it and LOCALISES it; a message that only
    # reports two equal byte counts is what sent three green-up attempts after
    # a nondeterminism that was never there.
    same_size = fresh.replace(b"(rank 100)", b"(rank 101)", 1)
    expect(
        len(same_size) == len(fresh) and same_size != fresh,
        "the mutated copy is the same size as the regenerate but not equal to it",
        failures,
    )
    size_verdict = drift(same_size, fresh)
    expect(size_verdict is not None, "a size-identical drift is still reported", failures)
    expect(
        size_verdict is not None and "rank 101" in size_verdict,
        "the verdict shows the differing line, not just the two byte counts",
        failures,
    )


def selftest() -> int:
    """Prove the verdict fires on drift, stays quiet on a match, and is non-vacuous.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures: list[str] = []
    fresh = regenerate(REPO_ROOT)
    selftest_generator_stability(fresh, failures)
    selftest_verdict_directions(fresh, failures)
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
