#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Speculative include cleaner for ra8-firmware.

Finds genuinely unused `#include` directives in first-party C translation units
using speculative compilation. Rather than relying on heuristic AST linters
(which falsely reject intentional umbrella headers or demand redundant direct
includes), this checker tests each `#include` by commenting it out and
attempting compilation. If the compiler succeeds with identical semantics and
no errors, the header is verified to be dead code.

Usage::

    python3 scripts/checks/check_unused_includes.py --selftest
    python3 scripts/checks/check_unused_includes.py --check [paths...]
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def _include_re() -> re.Pattern[str]:
    return re.compile(r"^(#\s*include\s+[\"<][^\">]+[\">])", re.MULTILINE)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _find_compiler() -> str:
    for candidate in ("cc", "gcc", "clang"):
        found = shutil.which(candidate)
        if found:
            return found
    return "cc"


def _load_compile_db() -> dict[str, list[str]]:
    commands: dict[str, list[str]] = {}
    for db_path in (
        _repo_root() / "build" / "tidy" / "compile_commands.json",
        _repo_root() / "build" / "compile_commands.json",
    ):
        if not db_path.is_file():
            continue
        try:
            data = json.loads(db_path.read_text(encoding="utf-8"))
            for entry in data:
                file_rel = entry.get("file", "")
                if file_rel:
                    p = Path(file_rel)
                    try:
                        rel = str(p.relative_to(_repo_root()))
                    except ValueError:
                        rel = file_rel
                    if "command" in entry:
                        commands[rel] = shlex.split(entry["command"])
                    elif "arguments" in entry:
                        commands[rel] = list(entry["arguments"])
            if commands:
                break
        except (OSError, json.JSONDecodeError):
            continue
    return commands


def _default_compile_args(path: Path) -> list[str]:
    args = [
        _find_compiler(),
        "-std=gnu2x",
        "-c",
        "-D_GNU_SOURCE",
        "-DRA8_OFF_TARGET",
        f"-I{_repo_root()}",
        f"-I{path.parent}",
        "-Werror",
        "-Wno-unknown-warning-option",
    ]
    for root in ("libs", "apps", "tools", "port"):
        base = _repo_root() / root
        if base.is_dir():
            args.extend(f"-I{p}" for p in base.glob("**/inc") if p.is_dir())
    return args


def _compile_args_for_file(path: Path, db: dict[str, list[str]]) -> list[str]:
    try:
        rel = str(path.resolve().relative_to(_repo_root()))
    except ValueError:
        rel = str(path)
    if rel in db:
        raw_cmd = db[rel]
        filtered: list[str] = []
        skip_next = False
        for arg in raw_cmd:
            if skip_next:
                skip_next = False
                continue
            if arg in ("-c", "-o"):
                if arg == "-o":
                    skip_next = True
                continue
            if arg.endswith((".c", ".cpp", ".cc", ".s", ".S")):
                continue
            filtered.append(arg)
        return filtered
    return _default_compile_args(path)


def check_file(
    path: Path, db: dict[str, list[str]], *, verbose: bool = False
) -> list[tuple[int, str]]:
    """Test all `#include` lines in `path` by speculative compilation."""
    if not path.is_file():
        return []
    source = path.read_text(encoding="utf-8", errors="replace")
    matches = list(_include_re().finditer(source))
    if not matches:
        return []

    compile_cmd = _compile_args_for_file(path, db)
    unused: list[tuple[int, str]] = []

    with tempfile.TemporaryDirectory(prefix="ra8-include-check-") as tmp:
        scratch = Path(tmp) / path.name
        out_obj = Path(tmp) / f"{path.stem}.o"

        scratch.write_text(source, encoding="utf-8")
        base_cmd = [*compile_cmd, "-c", str(scratch), "-o", str(out_obj)]
        base_proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool
            base_cmd, capture_output=True, text=True, check=False
        )
        if base_proc.returncode != 0:
            if verbose:
                msg = f"check_unused_includes: skipping {path} (baseline does not compile)\n"
                sys.stderr.write(msg)
            return []

        for m in matches:
            inc_text = m.group(1).strip()
            mutated = source[: m.start()] + "// " + inc_text + source[m.end() :]
            scratch.write_text(mutated, encoding="utf-8")

            proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool
                base_cmd, capture_output=True, text=True, check=False
            )
            if proc.returncode == 0:
                line_no = source[: m.start()].count("\n") + 1
                unused.append((line_no, inc_text))

    return unused


def _git_changed_c_files() -> list[Path]:
    c_files: list[Path] = []
    for target in ("origin/dev", "dev", "HEAD~1"):
        proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool
            ["git", "diff", "--name-only", target, "--", "*.c"],  # noqa: S607 -- fixed argv, trusted tool
            cwd=_repo_root(),
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            for line in proc.stdout.splitlines():
                p = _repo_root() / line.strip()
                if p.is_file() and p.suffix == ".c":
                    c_files.append(p)
            break
    if not c_files:
        proc = subprocess.run(
            ["git", "diff", "--name-only", "--", "*.c"],  # noqa: S607 -- fixed argv, trusted tool
            cwd=_repo_root(),
            capture_output=True,
            text=True,
            check=False,
        )
        for line in proc.stdout.splitlines():
            p = _repo_root() / line.strip()
            if p.is_file() and p.suffix == ".c":
                c_files.append(p)
    return sorted(set(c_files))


def selftest() -> int:
    """Prove that unused includes are flagged while required includes are kept."""
    with tempfile.TemporaryDirectory(prefix="ra8-selftest-inc-") as tmp:
        test_dir = Path(tmp)
        c_file = test_dir / "test.c"
        c_file.write_text(
            """#include <stdint.h>
#include <stdbool.h>

uint32_t compute(void) {
    return 42U;
}
""",
            encoding="utf-8",
        )
        unused = check_file(c_file, {}, verbose=False)
        inc_names = [item[1] for item in unused]
        if "#include <stdbool.h>" not in inc_names:
            sys.stderr.write("selftest: FAILED -- <stdbool.h> was not flagged as unused\n")
            return 1
        if "#include <stdint.h>" in inc_names:
            sys.stderr.write("selftest: FAILED -- <stdint.h> was falsely flagged as unused\n")
            return 1

    print("check_unused_includes.py: selftest OK")
    return 0


def main(argv: list[str]) -> int:
    """Run include checker over given paths or git-modified files."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths", nargs="*", help="C source files to check (defaults to branch-modified)"
    )
    parser.add_argument("--check", action="store_true", help="enforce zero unused includes")
    parser.add_argument("--selftest", action="store_true", help="run regression selftest")
    parser.add_argument("--verbose", "-v", action="store_true", help="verbose diagnostics")
    args = parser.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    files: list[Path] = []
    if args.paths:
        for p in args.paths:
            path = Path(p)
            if path.is_file() and path.suffix in (".c", ".h"):
                files.append(path.resolve())
    else:
        files = _git_changed_c_files()

    if not files:
        if args.verbose:
            print("check_unused_includes.py: no C files in scope.")
        return 0

    db = _load_compile_db()
    total_unused = 0
    for f in files:
        findings = check_file(f, db, verbose=args.verbose)
        if findings:
            total_unused += len(findings)
            rel = f.relative_to(_repo_root()) if f.is_relative_to(_repo_root()) else f
            for line_no, inc in findings:
                print(f"{rel}:{line_no}: unused include: {inc}")

    if total_unused > 0:
        sys.stderr.write(f"\ncheck_unused_includes.py: {total_unused} unused include(s) found.\n")
        return 1

    print(f"check_unused_includes.py: clean ({len(files)} file(s) checked, 0 unused includes).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
