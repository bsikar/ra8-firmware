#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Verify every first-party Rust source with rustfmt, Clippy, and Cargo tests."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from git_environment import trusted_git_executable
from lint_targets import is_build_output_path

CONTRACT = ".rust-test-contract.json"


def repo_root() -> Path:
    """Return the repository root containing this checker."""
    return Path(__file__).resolve().parents[2]


def rust_sources(root: Path | None = None) -> list[Path]:
    """Enumerate present first-party Rust sources."""
    base = repo_root() if root is None else root
    if root is not None:
        return sorted(path for path in base.rglob("*.rs") if not is_build_output_path(str(path)))
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted Git executable
        [
            trusted_git_executable(),
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            "*.rs",
        ],
        cwd=base,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        message = "git ls-files failed while enumerating Rust sources"
        raise RuntimeError(message)
    return sorted(
        base / rel
        for rel in proc.stdout.split("\0")
        if rel and (base / rel).is_file() and not is_build_output_path(rel)
    )


def crate_for(source: Path, boundary: Path) -> Path | None:
    """Find the nearest Cargo crate owning ``source`` within ``boundary``."""
    current = source.parent
    while current == boundary or boundary in current.parents:
        if (current / "Cargo.toml").is_file():
            return current
        if current == boundary:
            break
        current = current.parent
    return None


def crates_and_errors(base: Path, sources: list[Path]) -> tuple[list[Path], list[str]]:
    """Return every owning crate plus sources that have no manifest."""
    errors: list[str] = []
    crates: set[Path] = set()
    for source in sources:
        crate = crate_for(source, base)
        if crate is None:
            errors.append(f"Rust source has no Cargo.toml ancestor: {source.relative_to(base)}")
        else:
            crates.add(crate)
    return sorted(crates), errors


def contract_errors(crate: Path) -> tuple[list[str], int, set[Path]]:
    """Validate exact source coverage, lockfile presence, and test floor."""
    errors: list[str] = []
    path = crate / CONTRACT
    if not path.is_file():
        return [f"missing {CONTRACT}"], 0, set()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"invalid {CONTRACT}: {exc}"], 0, set()
    covered = data.get("covered_sources")
    floor = data.get("minimum_tests")
    if (
        not isinstance(covered, list)
        or not covered
        or not all(isinstance(item, str) for item in covered)
    ):
        errors.append("covered_sources must be a non-empty string list")
        covered = []
    if not isinstance(floor, int) or isinstance(floor, bool) or floor < 1:
        errors.append("minimum_tests must be a positive integer")
        floor = 0
    declared: set[Path] = set()
    for rel in covered:
        candidate = crate / rel
        try:
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(crate.resolve())
            declared.add(resolved)
        except (OSError, ValueError):
            errors.append(f"covered source is missing or outside crate: {rel}")
    owned = {
        path.resolve()
        for path in crate.rglob("*.rs")
        if not is_build_output_path(str(path.relative_to(crate)))
    }
    errors.extend(
        f"orphan Rust source is not in covered_sources: {source.relative_to(crate)}"
        for source in sorted(owned - declared)
    )
    errors.extend(
        f"covered source is not a Rust source: {source.relative_to(crate)}"
        for source in sorted(declared - owned)
    )
    if not (crate / "Cargo.lock").is_file():
        errors.append("missing Cargo.lock required by --locked")
    return errors, floor, declared


def run(
    argv: list[str], cwd: Path, environment: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    """Run a fixed Cargo/Git command without a shell."""
    return subprocess.run(  # noqa: S603 -- caller supplies pinned tool plus fixed arguments
        argv, cwd=cwd, env=environment, capture_output=True, text=True, check=False
    )


def compiled_sources(target: Path, crate: Path) -> set[Path]:
    """Read rustc dep-info emitted by a clean no-run test compilation."""
    reached: set[Path] = set()
    for dep_info in target.rglob("*.d"):
        text = dep_info.read_text(encoding="utf-8", errors="replace").replace("\\\n", " ")
        for token in text.split(":", 1)[-1].split():
            candidate = Path(token)
            if candidate.suffix != ".rs":
                continue
            resolved = (
                candidate.resolve() if candidate.is_absolute() else (crate / candidate).resolve()
            )
            if resolved == crate.resolve() or crate.resolve() in resolved.parents:
                reached.add(resolved)
    return reached


def executed_test_count(output: str) -> int:
    """Count tests Cargo reports as actually passed, excluding ignored tests."""
    return sum(int(passed) for passed in re.findall(r"(?m)^test result: .*? (\d+) passed;", output))


def verify(cargo: str, base: Path) -> int:
    """Run all Rust quality checks and native tests beneath ``base``."""
    sources = rust_sources(None if base == repo_root() else base)
    if not sources:
        print("check_rust.py: FATAL -- no first-party Rust sources found", file=sys.stderr)
        return 2
    crates, errors = crates_and_errors(base, sources)
    for crate in crates:
        contract_findings, floor, declared = contract_errors(crate)
        errors.extend(f"{crate.relative_to(base)}: {item}" for item in contract_findings)
        if contract_findings:
            continue
        with tempfile.TemporaryDirectory() as target_tmp:
            environment = dict(os.environ)
            environment["CARGO_TARGET_DIR"] = target_tmp
            for argv in (
                [cargo, "fmt", "--check"],
                [cargo, "clippy", "--locked", "--all-targets", "--", "-D", "warnings"],
            ):
                proc = run(argv, crate, environment)
                if proc.returncode != 0:
                    command = " ".join(argv[1:])
                    errors.append(
                        f"{crate.relative_to(base)}: {command} failed\n{proc.stdout}{proc.stderr}"
                    )
            no_run = run([cargo, "test", "--locked", "--no-run"], crate, environment)
            if no_run.returncode != 0:
                errors.append(f"{crate.relative_to(base)}: cargo test --no-run failed")
            else:
                missing = sorted(declared - compiled_sources(Path(target_tmp), crate))
                errors.extend(
                    f"{crate.relative_to(base)}: covered Rust source was not compiled: "
                    f"{source.relative_to(crate)}"
                    for source in missing
                )
            proc = run([cargo, "test", "--locked"], crate, environment)
            combined = proc.stdout + proc.stderr
            if proc.returncode != 0:
                errors.append(f"{crate.relative_to(base)}: cargo test failed\n{combined}")
            count = executed_test_count(combined)
            if count < floor:
                crate_name = crate.relative_to(base)
                errors.append(
                    f"{crate_name}: executed Rust test count {count} is below floor {floor}"
                )
    if errors:
        print("check_rust.py: FAILED", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(
        f"check_rust.py: {len(sources)} source(s), {len(crates)} crate(s), all native tests passed."
    )
    return 0


def write_fixture(root: Path, passing: bool = True, floor: int = 1) -> None:
    """Create one dependency-free synthetic crate for checker selftests."""
    (root / "src").mkdir(parents=True, exist_ok=True)
    (root / "Cargo.toml").write_text(
        '[package]\nname = "rust-check-selftest"\nversion = "0.1.0"\nedition = "2024"\n',
        encoding="utf-8",
    )
    expected = "1" if passing else "2"
    (root / "src/lib.rs").write_text(
        "#[test]\n"
        "fn contract() {\n"
        "    let actual = 1;\n"
        f"    assert_eq!(actual, {expected});\n"
        "}\n",
        encoding="utf-8",
    )
    (root / CONTRACT).write_text(
        json.dumps({"covered_sources": ["src/lib.rs"], "minimum_tests": floor}) + "\n",
        encoding="utf-8",
    )


def selftest(cargo: str) -> int:
    """Prove passing and failing tests, floors, and source census both ways."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_fixture(root)
        if run([cargo, "generate-lockfile"], root).returncode != 0 or verify(cargo, root) != 0:
            failures.append("must-stay-quiet: compliant crate failed")
        write_fixture(root, passing=False)
        if verify(cargo, root) == 0:
            failures.append("must-fire: failing native Rust test was accepted")
        write_fixture(root, floor=2)
        if verify(cargo, root) == 0:
            failures.append("must-fire: test count below the contract floor was accepted")
        write_fixture(root)
        (root / "src/orphan.rs").write_text("pub const ORPHAN: bool = true;\n", encoding="utf-8")
        if verify(cargo, root) == 0:
            failures.append("must-fire: orphan Rust source was accepted")
        contract = root / CONTRACT
        raw = json.loads(contract.read_text(encoding="utf-8"))
        raw["covered_sources"].append("src/orphan.rs")
        contract.write_text(json.dumps(raw) + "\n", encoding="utf-8")
        if verify(cargo, root) == 0:
            failures.append("must-fire: listed but uncompiled Rust source was accepted")
        (root / "src/orphan.rs").unlink()
        write_fixture(root)
        source = root / "src/lib.rs"
        source.write_text(
            source.read_text(encoding="utf-8").replace("#[test]", "#[test]\n#[ignore]"),
            encoding="utf-8",
        )
        if verify(cargo, root) == 0:
            failures.append("must-fire: ignored Rust test inflated the execution floor")
    if failures:
        print("check_rust.py --selftest: FAILED", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("check_rust.py --selftest: OK (pass, failure, floor, and source census)")
    return 0


def main() -> int:
    """Parse the checker mode and execute it."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--list-files", action="store_true")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--require", action="store_true")
    args = parser.parse_args()
    if args.list_files:
        for source in rust_sources():
            print(source.relative_to(repo_root()))
        return 0
    cargo = os.environ.get("CARGO") or shutil.which("cargo")
    if cargo is None:
        if args.require or args.selftest or args.test:
            print("check_rust.py: FATAL -- cargo was not found", file=sys.stderr)
            return 2
        return 0
    if args.selftest:
        return selftest(cargo)
    if not args.test:
        parser.error("select --test, --selftest, or --list-files")
    return verify(cargo, repo_root())


if __name__ == "__main__":
    raise SystemExit(main())
