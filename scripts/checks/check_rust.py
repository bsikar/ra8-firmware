#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Verify every first-party Rust source with explicit lint, format, and test modes."""

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
RUST_TEST_ATTR_RE = re.compile(r"(?m)^\s*#\s*\[\s*(?:test\s*\]|cfg\s*\([^]]*\btest\b[^]]*\)\s*\])")


def blank_preserving_lines(text: str) -> str:
    """Replace non-newline characters with spaces."""
    return "".join("\n" if char == "\n" else " " for char in text)


def consume_block_comment(text: str, start: int) -> int:
    """Return the end of one possibly nested Rust block comment."""
    index = start + 2
    depth = 1
    while index < len(text) and depth:
        if text.startswith("/*", index):
            depth += 1
            index += 2
        elif text.startswith("*/", index):
            depth -= 1
            index += 2
        else:
            index += 1
    return index


def consume_quoted(text: str, start: int, quote: str) -> int:
    """Return the end of one escaped Rust string or character literal."""
    index = start + 1
    while index < len(text):
        if text[index] == "\\" and index + 1 < len(text):
            index += 2
        elif text[index] == quote:
            return index + 1
        else:
            index += 1
    return index


def raw_string_end(text: str, start: int) -> int | None:
    """Return the end of a Rust raw string beginning at ``start``."""
    match = re.match(r'r(#{0,255})"', text[start:])
    if match is None:
        return None
    terminator = '"' + match.group(1)
    content_start = start + len(match.group(0))
    end = text.find(terminator, content_start)
    return len(text) if end < 0 else end + len(terminator)


def without_rust_comments_and_strings(text: str) -> str:
    """Blank comments and string/character literals while preserving newlines."""
    output: list[str] = []
    index = 0
    while index < len(text):
        end = index
        if text.startswith("//", index):
            newline = text.find("\n", index)
            end = len(text) if newline < 0 else newline
        elif text.startswith("/*", index):
            end = consume_block_comment(text, index)
        elif text[index] == "r":
            end = raw_string_end(text, index) or index
        elif text[index] == '"':
            end = consume_quoted(text, index, '"')
        elif text[index] == "'" and re.match(r"'(?:\\.|[^\\'\n])'", text[index:]):
            end = consume_quoted(text, index, "'")
        if end > index:
            output.append(blank_preserving_lines(text[index:end]))
            index = end
        else:
            output.append(text[index])
            index += 1
    return "".join(output)


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


def package_policy_errors(
    cargo: str, crate: Path, package_license: str, allowed_dependencies: set[str]
) -> list[str]:
    """Validate package license and direct dependencies through Cargo metadata."""
    proc = run(
        [cargo, "metadata", "--locked", "--offline", "--no-deps", "--format-version", "1"],
        crate,
    )
    if proc.returncode != 0:
        return [f"cargo metadata --locked --offline failed\n{proc.stdout}{proc.stderr}"]
    try:
        metadata = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        return [f"cargo metadata returned invalid JSON: {exc}"]
    manifest = (crate / "Cargo.toml").resolve()
    packages = [
        package
        for package in metadata.get("packages", [])
        if Path(package.get("manifest_path", "")).resolve() == manifest
    ]
    if len(packages) != 1:
        return ["cargo metadata did not identify exactly one package for Cargo.toml"]
    package = packages[0]
    errors: list[str] = []
    if package.get("license") != package_license:
        errors.append(
            f"Cargo.toml license {package.get('license')!r} does not match "
            f"contract package_license {package_license!r}"
        )
    actual_dependencies = {dependency["name"] for dependency in package.get("dependencies", [])}
    if actual_dependencies != allowed_dependencies:
        errors.append(
            "direct dependency names do not match allowed_dependencies: "
            f"expected {sorted(allowed_dependencies)}, got {sorted(actual_dependencies)}"
        )
    return errors


def contract_errors(cargo: str, crate: Path) -> tuple[list[str], int, set[Path]]:
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
    package_license = data.get("package_license")
    allowed_dependencies = data.get("allowed_dependencies")
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
    if not isinstance(package_license, str) or not package_license:
        errors.append("package_license must be a non-empty SPDX license expression")
        package_license = ""
    if not isinstance(allowed_dependencies, list) or not all(
        isinstance(item, str) and item for item in allowed_dependencies
    ):
        errors.append("allowed_dependencies must be a string list")
        allowed_dependencies = []
    elif len(allowed_dependencies) != len(set(allowed_dependencies)):
        errors.append("allowed_dependencies must not contain duplicates")
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
    errors.extend(
        "production Rust source contains inline tests; move them under tests/: "
        f"{source.relative_to(crate.resolve())}"
        for source in sorted(declared)
        if "src" in source.relative_to(crate.resolve()).parts
        and RUST_TEST_ATTR_RE.search(
            without_rust_comments_and_strings(source.read_text(encoding="utf-8"))
        )
    )
    if not (crate / "Cargo.lock").is_file():
        errors.append("missing Cargo.lock required by --locked")
    elif package_license:
        errors.extend(
            package_policy_errors(cargo, crate, package_license, set(allowed_dependencies))
        )
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


def quality(cargo: str, base: Path, *, run_clippy: bool) -> int:
    """Check rustfmt and optionally Clippy over every first-party crate."""
    sources = rust_sources(None if base == repo_root() else base)
    if not sources:
        print("check_rust.py: FATAL -- no first-party Rust sources found", file=sys.stderr)
        return 2
    crates, errors = crates_and_errors(base, sources)
    for crate in crates:
        with tempfile.TemporaryDirectory() as target_tmp:
            environment = dict(os.environ)
            environment["CARGO_TARGET_DIR"] = target_tmp
            commands = [[cargo, "fmt", "--check"]]
            if run_clippy:
                commands.append(
                    [
                        cargo,
                        "clippy",
                        "--locked",
                        "--all-targets",
                        "--all-features",
                        "--",
                        "-D",
                        "warnings",
                    ]
                )
            for argv in commands:
                proc = run(argv, crate, environment)
                if proc.returncode != 0:
                    errors.append(
                        f"{crate.relative_to(base)}: {' '.join(argv[1:])} failed\n"
                        f"{proc.stdout}{proc.stderr}"
                    )
    if errors:
        mode = "--lint" if run_clippy else "--check-format"
        print(f"check_rust.py {mode}: FAILED", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    mode = "--lint" if run_clippy else "--check-format"
    print(f"check_rust.py {mode}: {len(sources)} source(s), {len(crates)} crate(s) clean.")
    return 0


def lint(cargo: str, base: Path) -> int:
    """Run rustfmt and Clippy over every discovered first-party crate."""
    return quality(cargo, base, run_clippy=True)


def format_sources(cargo: str, base: Path) -> int:
    """Run rustfmt in place for every discovered first-party crate."""
    sources = rust_sources(None if base == repo_root() else base)
    if not sources:
        print("check_rust.py: FATAL -- no first-party Rust sources found", file=sys.stderr)
        return 2
    crates, errors = crates_and_errors(base, sources)
    for crate in crates:
        proc = run([cargo, "fmt"], crate)
        if proc.returncode != 0:
            errors.append(
                f"{crate.relative_to(base)}: cargo fmt failed\n{proc.stdout}{proc.stderr}"
            )
    if errors:
        print("check_rust.py --format: FAILED", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(f"check_rust.py --format: formatted {len(sources)} source(s) in {len(crates)} crate(s).")
    return 0


def verify(cargo: str, base: Path) -> int:
    """Run native Rust tests beneath ``base`` without duplicating lint/style work."""
    sources = rust_sources(None if base == repo_root() else base)
    if not sources:
        print("check_rust.py: FATAL -- no first-party Rust sources found", file=sys.stderr)
        return 2
    crates, errors = crates_and_errors(base, sources)
    for crate in crates:
        contract_findings, floor, declared = contract_errors(cargo, crate)
        errors.extend(f"{crate.relative_to(base)}: {item}" for item in contract_findings)
        if contract_findings:
            continue
        with tempfile.TemporaryDirectory() as target_tmp:
            environment = dict(os.environ)
            environment["CARGO_TARGET_DIR"] = target_tmp
            no_run = run(
                [cargo, "test", "--locked", "--all-features", "--no-run"], crate, environment
            )
            if no_run.returncode != 0:
                errors.append(
                    f"{crate.relative_to(base)}: cargo test --all-features --no-run failed"
                )
            else:
                missing = sorted(declared - compiled_sources(Path(target_tmp), crate))
                errors.extend(
                    f"{crate.relative_to(base)}: covered Rust source was not compiled: "
                    f"{source.relative_to(crate)}"
                    for source in missing
                )
            proc = run([cargo, "test", "--locked", "--all-features"], crate, environment)
            combined = proc.stdout + proc.stderr
            if proc.returncode != 0:
                errors.append(
                    f"{crate.relative_to(base)}: cargo test --all-features failed\n{combined}"
                )
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


def write_fixture(root: Path, passing: bool = True, floor: int = 2) -> None:
    """Create one dependency-free synthetic crate for checker selftests."""
    (root / "src").mkdir(parents=True, exist_ok=True)
    (root / "tests").mkdir(parents=True, exist_ok=True)
    (root / "Cargo.toml").write_text(
        '[package]\nname = "rust-check-selftest"\nversion = "0.1.0"\nedition = "2024"\n'
        'license = "MIT"\n'
        "\n[features]\ndefault = []\nprobe = []\n",
        encoding="utf-8",
    )
    (root / "src/lib.rs").write_text(
        "pub const fn contract_value() -> u32 {\n    1\n}\n",
        encoding="utf-8",
    )
    expected = "1" if passing else "2"
    (root / "tests/native.rs").write_text(
        "#[test]\n"
        "fn contract() {\n"
        f"    assert_eq!(rust_check_selftest::contract_value(), {expected});\n"
        "}\n"
        '\n#[cfg(feature = "probe")]\n'
        "#[test]\n"
        "fn feature_contract() {\n"
        "    assert_eq!(rust_check_selftest::contract_value(), 1);\n"
        "}\n",
        encoding="utf-8",
    )
    (root / CONTRACT).write_text(
        json.dumps(
            {
                "covered_sources": ["src/lib.rs", "tests/native.rs"],
                "minimum_tests": floor,
                "package_license": "MIT",
                "allowed_dependencies": [],
            }
        )
        + "\n",
        encoding="utf-8",
    )


def selftest_quality(cargo: str, root: Path, failures: list[str]) -> None:
    """Prove rustfmt and Clippy each fire, fix, and stay quiet independently."""
    write_fixture(root)
    if run([cargo, "generate-lockfile"], root).returncode != 0 or lint(cargo, root) != 0:
        failures.append("must-stay-quiet: compliant Rust quality fixture failed")
    source = root / "src/lib.rs"
    source.write_text("pub const fn contract_value()->u32 {1}\n", encoding="utf-8")
    if lint(cargo, root) == 0:
        failures.append("must-fire: rustfmt accepted unformatted production Rust")
    if format_sources(cargo, root) != 0 or lint(cargo, root) != 0:
        failures.append("must-stay-quiet: cargo fmt did not repair production Rust")
    source.write_text(
        source.read_text(encoding="utf-8")
        + '\n#[cfg(feature = "probe")]\n'
        + "pub fn clippy_probe(value: bool) -> bool {\n    value == true\n}\n",
        encoding="utf-8",
    )
    default_clippy = run(
        [cargo, "clippy", "--locked", "--all-targets", "--", "-D", "warnings"], root
    )
    if default_clippy.returncode != 0:
        failures.append("must-stay-quiet: feature-gated Clippy probe leaked into default features")
    if lint(cargo, root) == 0:
        failures.append("must-fire: Clippy omitted a warning behind an optional feature")


def selftest_lint(cargo: str) -> int:
    """Prove rustfmt and Clippy fire, repair, and stay quiet."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        selftest_quality(cargo, root, failures)
    if failures:
        print("check_rust.py --selftest-lint: FAILED", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("check_rust.py --selftest-lint: OK (rustfmt and Clippy both directions)")
    return 0


def selftest_placement(cargo: str, root: Path, failures: list[str]) -> None:
    """Prove production test attributes fire without matching inert prose."""
    cases = (
        (
            "\n#[cfg(test)]\nmod tests { #[test] fn inline() {} }\n",
            "must-fire: inline test in production Rust source was accepted",
        ),
        (
            '\n#[cfg(any(test, target_os = "linux"))]\nmod compound_tests {}\n',
            "must-fire: cfg(any(test, ...)) in production Rust was accepted",
        ),
        (
            '\n#[cfg(all(test, target_os = "linux"))]\nmod conjunctive_tests {}\n',
            "must-fire: cfg(all(test, ...)) in production Rust was accepted",
        ),
        (
            "\n#[test]\nfn bare_inline_test() {}\n",
            "must-fire: bare #[test] in production Rust source was accepted",
        ),
    )
    for source_suffix, message in cases:
        write_fixture(root)
        source = root / "src/lib.rs"
        source.write_text(
            source.read_text(encoding="utf-8") + source_suffix,
            encoding="utf-8",
        )
        if verify(cargo, root) == 0:
            failures.append(message)
    write_fixture(root)
    source = root / "src/lib.rs"
    source.write_text(
        source.read_text(encoding="utf-8")
        + '\npub const PROSE: &str = r#"#[cfg(all(test, unix))]"#;\n'
        + "/* #[test] fn documented_only() {} */\n",
        encoding="utf-8",
    )
    if verify(cargo, root) != 0:
        failures.append("must-stay-quiet: Rust comments/string payloads were treated as tests")


def selftest_tests(cargo: str) -> int:
    """Prove native tests, policy, placement, floors, and source census both ways."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_fixture(root)
        if run([cargo, "generate-lockfile"], root).returncode != 0:
            failures.append("must-stay-quiet: native Rust fixture lockfile generation failed")
        write_fixture(root)
        if verify(cargo, root) != 0:
            failures.append("must-stay-quiet: compliant native Rust test failed")
        contract = root / CONTRACT
        raw = json.loads(contract.read_text(encoding="utf-8"))
        raw["package_license"] = "Apache-2.0"
        contract.write_text(json.dumps(raw) + "\n", encoding="utf-8")
        if verify(cargo, root) == 0:
            failures.append("must-fire: Cargo license drift was accepted")
        write_fixture(root)
        raw = json.loads(contract.read_text(encoding="utf-8"))
        raw["allowed_dependencies"] = ["undeclared-probe"]
        contract.write_text(json.dumps(raw) + "\n", encoding="utf-8")
        if verify(cargo, root) == 0:
            failures.append("must-fire: direct dependency policy drift was accepted")
        write_fixture(root, passing=False)
        if verify(cargo, root) == 0:
            failures.append("must-fire: failing native Rust test was accepted")
        write_fixture(root, floor=3)
        if verify(cargo, root) == 0:
            failures.append("must-fire: test count below the contract floor was accepted")
        selftest_placement(cargo, root, failures)
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
        source = root / "tests/native.rs"
        source.write_text(
            source.read_text(encoding="utf-8").replace("#[test]", "#[test]\n#[ignore]"),
            encoding="utf-8",
        )
        if verify(cargo, root) == 0:
            failures.append("must-fire: ignored Rust test inflated the execution floor")
    if failures:
        print("check_rust.py --selftest-test: FAILED", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("check_rust.py --selftest-test: OK (tests, placement, floor, census)")
    return 0


def execute_mode(args: argparse.Namespace, cargo: str, parser: argparse.ArgumentParser) -> int:
    """Dispatch one validated Cargo-backed checker mode."""
    if args.selftest_lint:
        return selftest_lint(cargo)
    if args.selftest_test:
        return selftest_tests(cargo)
    selected = sum((args.lint, args.check_format, args.format, args.test))
    if selected != 1:
        parser.error(
            "select exactly one of --lint, --format, --test, --selftest-lint, "
            "--selftest-test, or --list-files"
        )
    handlers = {
        "lint": lambda: lint(cargo, repo_root()),
        "check_format": lambda: quality(cargo, repo_root(), run_clippy=False),
        "format": lambda: format_sources(cargo, repo_root()),
        "test": lambda: verify(cargo, repo_root()),
    }
    selected_name = next(name for name in handlers if getattr(args, name))
    return handlers[selected_name]()


def main() -> int:
    """Parse the checker mode and execute it."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest-lint", action="store_true")
    parser.add_argument("--selftest-test", action="store_true")
    parser.add_argument("--list-files", action="store_true")
    parser.add_argument("--lint", action="store_true")
    parser.add_argument("--check-format", action="store_true")
    parser.add_argument("--format", action="store_true")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--require", action="store_true")
    args = parser.parse_args()
    if args.list_files:
        for source in rust_sources():
            print(source.relative_to(repo_root()))
        return 0
    cargo = os.environ.get("CARGO") or shutil.which("cargo")
    if cargo is None:
        if (
            args.require
            or args.selftest_lint
            or args.selftest_test
            or args.lint
            or args.check_format
            or args.format
            or args.test
        ):
            print("check_rust.py: FATAL -- cargo was not found", file=sys.stderr)
            return 2
        return 0
    return execute_mode(args, cargo, parser)


if __name__ == "__main__":
    raise SystemExit(main())
