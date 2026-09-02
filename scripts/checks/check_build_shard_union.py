#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: cross-build shards covered every firmware configuration exactly once.

``scripts/builders/all_examples.sh`` builds a stride slice of the canonical
configuration matrix when ``RA8_BUILD_SHARDS``/``RA8_BUILD_SHARD`` are set, so the
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

* This checker re-runs structural discovery itself (:func:`discover_apps`)
  rather than trusting the ``all-configs.txt`` a shard wrote. A shard vouching for its
  own idea of what the tree contains proves nothing -- if its discovery broke,
  its manifest and its self-report break together and agree.
* Every shard ``1..N`` must have left a manifest. A missing one is a shard
  that did not run.
* The union of the manifests must equal the discovered set exactly: a missing
  configuration FAILS (it was never built), and one claimed by two shards
  FAILS (the stride is broken, so some other configuration is missing too).
* ``all-configs.txt`` must agree with the re-derived set, which catches the shards
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

#: The full execution matrix, written identically by every shard.
ALL_CONFIGS_NAME = "all-configs.txt"

RC_OK = 0
RC_VIOLATION = 1
MIN_EXAMPLE_PATH_PARTS = 2


def discover_apps(repo_root: Path) -> list[str]:
    """Independently derive every required firmware build configuration.

    This structural walk deliberately does not import ``ra8_apps.py``, the
    execution authority. It scans both examples and standalone board products,
    then independently requires the e-reader's normal and Non-Secure XIP
    configurations. A defect in the execution enumerator therefore cannot make
    this proof agree with the same omission.

    :param repo_root: Repository root to discover under.
    :returns: Sorted app names.
    """
    configs: set[str] = set()
    examples = repo_root / "examples"
    if examples.is_dir():
        for main_c in examples.rglob("main.c"):
            if main_c.parent.name != "src":
                continue
            app_dir = main_c.parent.parent
            if not (app_dir / "CMakeLists.txt").is_file():
                continue
            rel = app_dir.relative_to(examples)
            if len(rel.parts) < MIN_EXAMPLE_PATH_PARTS or rel.parts[0] == "shared":
                continue
            configs.add("::".join(rel.parts))

    board_root = repo_root / "apps" / "board" / "stand_alone"
    if board_root.is_dir():
        for main_c in board_root.rglob("main.c"):
            if main_c.parent.name != "src":
                continue
            app_dir = main_c.parent.parent
            if not (app_dir / "CMakeLists.txt").is_file():
                continue
            rel = app_dir.relative_to(board_root)
            name = "ra8d2-ereader" if rel.as_posix() == "ereader" else rel.name
            identifier = f"board::stand_alone::{name}"
            configs.add(identifier)
            if rel.as_posix() == "ereader":
                configs.add(f"{identifier}@ns-xip")
    return sorted(configs)


def read_manifest(path: Path) -> list[str]:
    """Read one newline-delimited manifest, dropping blank lines.

    :param path: Manifest file.
    :returns: The app names it lists, in file order.
    """
    if not path.is_file():
        return []
    return [ln.strip() for ln in path.read_text(encoding="ascii").splitlines() if ln.strip()]


def _audit_shard_contents(shard_files: list[Path], expected: list[str]) -> list[str]:
    problems: list[str] = []
    seen: dict[str, int] = {}
    for k, path in enumerate(shard_files, start=1):
        for app in read_manifest(path):
            if app in seen:
                problems.append(f"app '{app}' claimed by both shard {seen[app]} and shard {k}")
            else:
                seen[app] = k

    if sorted(seen.keys()) != expected:
        missing = set(expected) - set(seen.keys())
        extra = set(seen.keys()) - set(expected)
        if missing:
            problems.append(f"{len(missing)} firmware configuration(s) never built by any shard")
        if extra:
            problems.append(
                f"{len(extra)} configuration(s) claimed in manifests are not structural"
            )
    return problems


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
            "no firmware configurations discovered under examples/ or "
            "apps/board/stand_alone/ -- this checker "
            "cannot vouch for a union it has no truth to compare against.",
        ]
    if not shard_dir.is_dir():
        return RC_VIOLATION, [f"shard manifest directory {shard_dir} does not exist."]

    all_configs_file = shard_dir / ALL_CONFIGS_NAME
    if not all_configs_file.is_file():
        problems.append(f"missing {all_configs_file}")
    elif read_manifest(all_configs_file) != expected:
        problems.append(f"{ALL_CONFIGS_NAME} disagrees with fresh discovery")

    shard_files = [shard_dir / f"shard-{k}-of-{shards}.txt" for k in range(1, shards + 1)]
    problems.extend(
        f"missing shard manifest {path.name}" for path in shard_files if not path.is_file()
    )

    if problems:
        return RC_VIOLATION, problems

    problems.extend(_audit_shard_contents(shard_files, expected))
    if problems:
        return RC_VIOLATION, problems

    print(
        f"check_build_shard_union.py: {shards} shard(s) covered all "
        f"{len(expected)} firmware configuration(s) exactly once."
    )
    return RC_OK, []


def _write_tree(root: Path, apps: list[str], *, ereader: bool = False) -> None:
    """Materialise a throwaway examples/ tree of buildable apps.

    :param root: Fake repo root.
    :param apps: App directory names to create.
    """
    for app in apps:
        d = root / "examples" / "tier" / app
        src = d / "src"
        src.mkdir(parents=True, exist_ok=True)
        (src / "main.c").write_text("int main(void){return 0;}\n", encoding="ascii")
        (d / "CMakeLists.txt").write_text("add_executable(test src/main.c)\n", encoding="ascii")
    if ereader:
        d = root / "apps" / "board" / "stand_alone" / "ereader"
        (d / "src").mkdir(parents=True, exist_ok=True)
        (d / "src" / "main.c").write_text("void main(void) {}\n", encoding="ascii")
        (d / "CMakeLists.txt").write_text(
            "add_executable(ereader src/main.c)\n",
            encoding="ascii",
        )


def _shard_manifests(root: Path, shards: int, slices: list[list[str]]) -> None:
    """Write per-shard manifests plus all-configs.txt into a fake tree.

    :param root: Fake repo root.
    :param shards: Declared shard count.
    :param slices: Per-shard configuration lists, index 0 == shard 1.
    """
    d = root / SHARD_SUBDIR
    d.mkdir(parents=True, exist_ok=True)
    every = sorted({a for s in slices for a in s})
    (d / ALL_CONFIGS_NAME).write_text("".join(f"{a}\n" for a in every), encoding="ascii")
    for i, names in enumerate(slices, start=1):
        (d / f"shard-{i}-of-{shards}.txt").write_text(
            "".join(f"{a}\n" for a in names),
            encoding="ascii",
        )


def _selftest_cases() -> int:
    """Run the complete and malformed shard-manifest fixtures."""
    cases: tuple[tuple[str, list[str], bool, int, list[list[str]], bool], ...] = (
        # (label, example names, ereader, shards, slices, expect_pass)
        (
            "complete examples plus board variants",
            ["a", "b"],
            True,
            2,
            [
                ["board::stand_alone::ra8d2-ereader", "tier::a"],
                ["board::stand_alone::ra8d2-ereader@ns-xip", "tier::b"],
            ],
            True,
        ),
        ("complete 1-way", ["a", "b"], False, 1, [["tier::a", "tier::b"]], True),
        ("a shard built nothing", ["a", "b"], False, 2, [["tier::a"], []], False),
        ("an app fell through", ["a", "b"], False, 2, [["tier::a"], []], False),
        ("an app built twice", ["a", "b"], False, 2, [["tier::a"], ["tier::a"]], False),
        (
            "an unknown app appeared",
            ["a", "b"],
            False,
            2,
            [["tier::a"], ["tier::b", "ghost"]],
            False,
        ),
        (
            "e-reader XIP configuration omitted",
            [],
            True,
            1,
            [["board::stand_alone::ra8d2-ereader"]],
            False,
        ),
    )
    failures = 0
    for label, tree, ereader, shards, slices, expect_pass in cases:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _write_tree(root, tree, ereader=ereader)
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

    return failures


def _selftest_boundaries() -> int:
    """Prove missing manifests and an empty discovery cannot pass vacuously."""
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        _write_tree(root, ["a"])
        rc, _ = check_union(root, 1)
        ok = rc == RC_VIOLATION
        print(f"  [{'ok' if ok else 'FAIL'}] absent manifest dir: expected to fire, rc={rc}")
        failures += 0 if ok else 1

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / SHARD_SUBDIR).mkdir(parents=True)
        rc, _ = check_union(root, 1)
        ok = rc == RC_VIOLATION
        print(f"  [{'ok' if ok else 'FAIL'}] empty tree: expected to fire, rc={rc}")
        failures += 0 if ok else 1
    return failures


def selftest() -> int:
    """Assert the union checker fires on breaks and stays quiet when complete.

    :returns: 0 when every case behaves, 1 otherwise.
    """
    failures = _selftest_cases() + _selftest_boundaries()

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
