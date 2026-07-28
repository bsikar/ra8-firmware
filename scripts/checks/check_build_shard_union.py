#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the cross-build shards together covered every app, exactly once.

``scripts/builders/all_examples.sh`` builds a stride slice of the discovered
app list when ``RA8_BUILD_SHARDS``/``RA8_BUILD_SHARD`` are set, so the
``build-cross`` gate can fan out across parallel CI jobs. That fan-out is only
sound if the shards, taken together, build the same set the unsharded gate
would have built. Nothing about a parallel matrix guarantees that: a shard
whose job was skipped, cancelled, or silently slice-computed to the empty set
produces no output and no error, and the downstream ``stack-usage`` aggregate
would simply measure fewer ``.su`` files and still clear its floor on the
shards that DID run. That is a gate quietly checking less than it claims --
the failure mode this repository has hit repeatedly -- so it gets its own
check rather than an assumption.

The proof is a set comparison against an INDEPENDENTLY re-derived truth:

* This checker re-runs app discovery itself (:func:`discover_apps`) rather
  than trusting the ``all-apps.txt`` a shard wrote. A shard vouching for its
  own idea of what the tree contains proves nothing -- if its discovery broke,
  its manifest and its self-report break together and agree.
* Every shard ``1..N`` must have left a manifest. A missing one is a shard
  that did not run.
* The union of the manifests must equal the discovered set exactly: a missing
  app FAILS (it was never built), and an app claimed by two shards FAILS (the
  stride is broken, so some other app is missing too).
* ``all-apps.txt`` must agree with the re-derived set, which catches the shards
  having discovered a *different* tree than this checker sees.

Run::

    check_build_shard_union.py --shards N   # the gate
    check_build_shard_union.py --selftest   # prove it still detects violations

Exit 0 when the shards cover the tree exactly, 1 (naming every discrepancy)
otherwise.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: Where all_examples.sh writes its per-shard manifests.
SHARD_SUBDIR = Path("build") / "build_all_examples" / ".shard"

#: The full discovered set, written identically by every shard.
ALL_APPS_NAME = "all-apps.txt"

RC_OK = 0
RC_VIOLATION = 1


def discover_apps(repo_root: Path) -> list[str]:
    """Re-derive the buildable app list exactly as all_examples.sh does.

    Mirrors the shell discovery: every ``examples/**/main.c`` whose directory
    also holds a ``Makefile``, excluding ``examples/host/`` (macOS-only dev
    tools, never cross-compiled), keyed by the bare directory name and sorted
    byte-wise. ``sorted()`` on ``str`` is a C-collation sort, matching the
    ``LC_ALL=C sort`` the shell pins for exactly this reason.

    :param repo_root: Repository root to discover under.
    :returns: Sorted app names.
    """
    examples = repo_root / "examples"
    if not examples.is_dir():
        return []
    apps = set()
    for main_c in examples.rglob("main.c"):
        app_dir = main_c.parent
        rel = app_dir.relative_to(repo_root).as_posix()
        if rel.startswith("examples/host/"):
            continue
        if not (app_dir / "Makefile").is_file():
            continue
        apps.add(app_dir.name)
    return sorted(apps)


def read_manifest(path: Path) -> list[str]:
    """Read one newline-delimited manifest, dropping blank lines.

    :param path: Manifest file.
    :returns: The app names it lists, in file order.
    """
    if not path.is_file():
        return []
    return [ln.strip() for ln in path.read_text(encoding="ascii").splitlines() if ln.strip()]


def check_union(repo_root: Path, shards: int) -> tuple[int, list[str]]:
    """Compare the union of the shard manifests against a fresh discovery.

    :param repo_root: Repository root holding the shard directory.
    :param shards: The number of shards that were scheduled.
    :returns: ``(exit_code, problem_lines)``.
    """
    problems: list[str] = []
    shard_dir = repo_root / SHARD_SUBDIR
    expected = discover_apps(repo_root)
    if not expected:
        return RC_VIOLATION, [
            f"no apps discovered under {repo_root / 'examples'} -- this checker "
            "cannot vouch for a union it has no truth to compare against.",
        ]
    if not shard_dir.is_dir():
        return RC_VIOLATION, [f"shard manifest directory {shard_dir} does not exist."]

    seen: dict[str, list[int]] = {}
    for k in range(1, shards + 1):
        manifest = shard_dir / f"shard-{k}-of-{shards}.txt"
        names = read_manifest(manifest)
        if not names:
            problems.append(
                f"shard {k}/{shards}: manifest {manifest.name} is missing or empty "
                "-- that shard never built anything.",
            )
            continue
        for name in names:
            seen.setdefault(name, []).append(k)

    missing = sorted(set(expected) - set(seen))
    extra = sorted(set(seen) - set(expected))
    duplicated = sorted(n for n, ks in seen.items() if len(ks) > 1)

    problems.extend(f"app '{name}' was discovered but no shard built it." for name in missing)
    problems.extend(f"app '{name}' was built but is not in the discovered set." for name in extra)
    problems.extend(
        f"app '{name}' was built by shards {seen[name]} -- the stride is broken."
        for name in duplicated
    )

    all_apps = read_manifest(shard_dir / ALL_APPS_NAME)
    if all_apps and sorted(all_apps) != expected:
        shard_only = sorted(set(all_apps) - set(expected))
        here_only = sorted(set(expected) - set(all_apps))
        problems.append(
            f"the shards discovered a different tree than this checker: "
            f"{len(shard_only)} app(s) only they saw {shard_only[:5]}, "
            f"{len(here_only)} only this checker saw {here_only[:5]}.",
        )

    if problems:
        return RC_VIOLATION, problems
    print(
        f"check_build_shard_union.py: {shards} shard(s) covered all "
        f"{len(expected)} discovered app(s) exactly once.",
    )
    return RC_OK, []


def _write_tree(root: Path, apps: list[str]) -> None:
    """Materialise a throwaway examples/ tree of buildable apps.

    :param root: Fake repo root.
    :param apps: App directory names to create.
    """
    for app in apps:
        d = root / "examples" / "tier" / app
        d.mkdir(parents=True, exist_ok=True)
        (d / "main.c").write_text("int main(void){return 0;}\n", encoding="ascii")
        (d / "Makefile").write_text("all:\n", encoding="ascii")


def _shard_manifests(root: Path, shards: int, slices: list[list[str]]) -> None:
    """Write per-shard manifests plus all-apps.txt into a fake tree.

    :param root: Fake repo root.
    :param shards: Declared shard count.
    :param slices: Per-shard app-name lists, index 0 == shard 1.
    """
    d = root / SHARD_SUBDIR
    d.mkdir(parents=True, exist_ok=True)
    every = sorted({a for s in slices for a in s})
    (d / ALL_APPS_NAME).write_text("".join(f"{a}\n" for a in every), encoding="ascii")
    for i, names in enumerate(slices, start=1):
        (d / f"shard-{i}-of-{shards}.txt").write_text(
            "".join(f"{a}\n" for a in names),
            encoding="ascii",
        )


def selftest() -> int:
    """Assert the union check fires on every break and stays quiet when whole.

    A completeness checker that stopped detecting an incomplete union would
    report success forever, so both directions are asserted here and CI runs
    this before trusting a clean verdict.

    :returns: 0 when every case behaves, 1 otherwise.
    """
    cases: list[tuple[str, list[str], int, list[list[str]], bool]] = [
        # (label, tree apps, shards, slices, expect_pass)
        ("complete 2-way stride", ["a", "b", "c", "d"], 2, [["a", "c"], ["b", "d"]], True),
        ("complete 1-way", ["a", "b"], 1, [["a", "b"]], True),
        ("a shard built nothing", ["a", "b", "c", "d"], 2, [["a", "c"], []], False),
        ("an app fell through", ["a", "b", "c", "d"], 2, [["a", "c"], ["b"]], False),
        ("an app built twice", ["a", "b", "c"], 2, [["a", "c"], ["b", "c"]], False),
        ("an unknown app appeared", ["a", "b"], 2, [["a"], ["b", "ghost"]], False),
    ]
    failures = 0
    for label, tree, shards, slices, expect_pass in cases:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_tree(root, tree)
            _shard_manifests(root, shards, slices)
            rc, problems = check_union(root, shards)
            ok = (rc == RC_OK) if expect_pass else (rc == RC_VIOLATION)
            verdict = "ok" if ok else "FAIL"
            want = "pass" if expect_pass else "fire"
            print(f"  [{verdict}] {label}: expected to {want}, rc={rc}")
            if not ok:
                failures += 1
                for p in problems:
                    print(f"          {p}")

    # A missing shard directory must fail rather than vacuously pass.
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        _write_tree(root, ["a"])
        rc, _ = check_union(root, 1)
        ok = rc == RC_VIOLATION
        print(f"  [{'ok' if ok else 'FAIL'}] absent manifest dir: expected to fire, rc={rc}")
        failures += 0 if ok else 1

    # An empty tree must fail rather than report "0 of 0 covered".
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / SHARD_SUBDIR).mkdir(parents=True)
        rc, _ = check_union(root, 1)
        ok = rc == RC_VIOLATION
        print(f"  [{'ok' if ok else 'FAIL'}] empty tree: expected to fire, rc={rc}")
        failures += 0 if ok else 1

    if failures:
        print(f"check_build_shard_union.py --selftest: {failures} case(s) FAILED", file=sys.stderr)
        return RC_VIOLATION
    print("check_build_shard_union.py --selftest: all cases pass.")
    return RC_OK


def main(argv: list[str]) -> int:
    """Entry point.

    :param argv: Command-line arguments without the program name.
    :returns: Process exit status.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--shards",
        type=int,
        default=None,
        help="how many shards were scheduled (must match the manifest names)",
    )
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.shards is None or args.shards < 1:
        parser.error("--shards must be a positive integer")

    rc, problems = check_union(args.repo_root, args.shards)
    if problems:
        print(
            "check_build_shard_union.py: the cross-build shards did NOT cover the tree",
            file=sys.stderr,
        )
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(
            "\n  Every app must be built by exactly one shard. Do NOT relax this to\n"
            "  make a red matrix pass: an unbuilt app is an unchecked app, and the\n"
            "  stack-usage aggregate downstream would still clear its floor on the\n"
            "  shards that did run.",
            file=sys.stderr,
        )
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
