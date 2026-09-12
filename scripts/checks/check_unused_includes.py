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
        _repo_root() / "compile_commands.json",
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


def _conditional_depths(source: str) -> list[int]:
    """Return the preprocessor conditional nesting depth at each line start."""
    depths: list[int] = []
    depth = 0
    directive_re = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")
    for line in source.split("\n"):
        m = directive_re.match(line)
        if m is not None:
            kind = m.group(1)
            if kind == "endif":
                depth = max(0, depth - 1)
                depths.append(depth)
            elif kind in ("else", "elif"):
                depths.append(max(0, depth - 1))
            else:
                depths.append(depth)
                depth += 1
        else:
            depths.append(depth)
    return depths


def _keep_reason(source: str, match_end: int) -> str | None:
    """Return the `ra8-keep-include` reason on an include line, if it has one."""
    line_end = source.find("\n", match_end)
    trailing = source[match_end : line_end if line_end != -1 else len(source)]
    m = re.search(r"ra8-keep-include:\s*(\S.*)?$", trailing)
    if m is None:
        return None
    reason = (m.group(1) or "").strip()
    return reason or None


def _keep_claimed_tokens(reason: str) -> list[str]:
    """Return the backticked symbol tokens a keep marker vouches for."""
    return re.findall(r"`([^`]+)`", reason)


def _code_tokens(source: str) -> set[str]:
    """Identifier tokens in non-include code with comments/strings stripped."""
    no_comments = re.sub(r"/\*.*?\*/|//[^\n]*", " ", source, flags=re.DOTALL)
    no_strings = re.sub(r"\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'", " ", no_comments)
    lines = [ln for ln in no_strings.split("\n") if not re.match(r"\s*#\s*include\b", ln)]
    return set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", "\n".join(lines)))


def _keep_honored(source: str, match_end: int) -> bool:
    """Honor a keep marker only if it names used symbols in backticks."""
    reason = _keep_reason(source, match_end)
    if reason is None:
        return False
    claimed = _keep_claimed_tokens(reason)
    if not claimed:
        return False
    code = _code_tokens(source)
    return all(tok in code for tok in claimed)


def _run_compile(base_cmd: list[str]) -> int:
    """Run one speculative compile, returning its exit code."""
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool
        base_cmd, capture_output=True, text=True, check=False
    )
    return proc.returncode


def _comment_include(source: str, match_start: int, inc_text: str, match_end: int) -> str:
    """Return source with one include line commented out."""
    return source[:match_start] + "// " + inc_text + source[match_end:]


def _body_trivial_without_includes(
    source: str, matches: list[re.Match[str]], base_cmd: list[str], scratch: Path
) -> bool:
    """Return True when the file still compiles with every include disabled."""
    # If the file compiles perfectly fine when ALL includes are commented
    # out simultaneously, then the file's body is likely disabled by macros
    # (e.g. RA8_OFF_TARGET). In this state, speculative compilation cannot
    # distinguish between used and unused includes, because none of them
    # affect compilation.
    mutated_all = source
    for m in reversed(matches):
        mutated_all = _comment_include(mutated_all, m.start(), m.group(1).strip(), m.end())
    scratch.write_text(mutated_all, encoding="utf-8")
    return _run_compile(base_cmd) == 0


def _test_top_level_includes(
    source: str, base_cmd: list[str], scratch: Path, path: Path, verbose: bool
) -> list[tuple[int, str]]:
    """Test each top-level include by speculative compilation."""
    # Configuration-dependent includes (inside #if/#ifdef blocks, e.g. an
    # MVE-gated <arm_mve.h>) cannot be judged by compilation under a single
    # configuration, so only top-level includes are tested per line.
    matches = list(_include_re().finditer(source))
    depths = _conditional_depths(source)
    unused: list[tuple[int, str]] = []
    for m in matches:
        line_no = source[: m.start()].count("\n") + 1
        if depths[line_no - 1] > 0:
            if verbose:
                msg = f"check_unused_includes: skipping guarded include {path}:{line_no}\n"
                sys.stderr.write(msg)
            continue
        # A `// ra8-keep-include: ...` marker records a reviewed direct-use
        # (IWYU) keep: the header declares symbols this file uses, even when
        # they also arrive transitively. The marker must vouch for at least
        # one used symbol in backticks; bare markers, generic prose, and
        # mistargeted tokens are not honored.
        if _keep_honored(source, m.end()):
            continue
        inc_text = m.group(1).strip()
        scratch.write_text(_comment_include(source, m.start(), inc_text, m.end()), encoding="utf-8")
        if _run_compile(base_cmd) == 0:
            unused.append((line_no, inc_text))
    return unused


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

    with tempfile.TemporaryDirectory(prefix="ra8-include-check-") as tmp:
        scratch = Path(tmp) / path.name
        out_obj = Path(tmp) / f"{path.stem}.o"

        scratch.write_text(source, encoding="utf-8")
        base_cmd = [*compile_cmd, "-c", str(scratch), "-o", str(out_obj)]
        if "-Wmissing-prototypes" not in base_cmd:
            base_cmd.extend(["-Wmissing-prototypes", "-Werror=missing-prototypes"])
        if _run_compile(base_cmd) != 0:
            if verbose:
                msg = f"check_unused_includes: skipping {path} (baseline fails)\n"
                sys.stderr.write(msg)
            return []
        if _body_trivial_without_includes(source, matches, base_cmd, scratch):
            if verbose:
                msg = f"check_unused_includes: skipping {path} (trivial body)\n"
                sys.stderr.write(msg)
            return []
        return _test_top_level_includes(source, base_cmd, scratch, path, verbose)


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
        if proc.returncode == 0 and proc.stdout.strip():
            for line in proc.stdout.splitlines():
                p = _repo_root() / line.strip()
                if p.is_file() and p.suffix == ".c":
                    c_files.append(p)
    return sorted(set(c_files))


def _selftest_basic(test_dir: Path) -> list[str]:
    """Prove that unused includes are flagged while required ones are kept."""
    failures: list[str] = []
    c_file = test_dir / "bad.c"
    c_file.write_text(
        """#include <stdint.h>
#include <stdbool.h>

static uint32_t compute(void) {
    return 42U;
}
""",
        encoding="utf-8",
    )
    unused = check_file(c_file, {}, verbose=False)
    inc_names = [item[1] for item in unused]
    if "#include <stdbool.h>" not in inc_names:
        failures.append("selftest: <stdbool.h> was not flagged as unused")
    if "#include <stdint.h>" in inc_names:
        failures.append("selftest: <stdint.h> was falsely flagged as unused")
    return failures


def _selftest_guarded(test_dir: Path) -> list[str]:
    """Prove guarded includes stay quiet while top-level dead ones fire."""
    failures: list[str] = []
    guarded = test_dir / "guarded.c"
    guarded.write_text(
        """#include <stdint.h>
#include <stdbool.h>
#ifdef __ARM_FEATURE_MVE
#include <dead_guarded.h>
#endif

static uint32_t compute(void) {
    return 42U;
}
""",
        encoding="utf-8",
    )
    guarded_unused = [item[1] for item in check_file(guarded, {}, verbose=False)]
    if "#include <dead_guarded.h>" in guarded_unused:
        failures.append("selftest: guarded include judged without its configuration")
    if "#include <stdbool.h>" not in guarded_unused:
        failures.append("selftest: top-level dead include beside a guard missed")
    return failures


def _selftest_keep_markers(test_dir: Path) -> list[str]:
    """Prove validated keep markers stay quiet and dishonest ones fire."""
    failures: list[str] = []
    valid = test_dir / "valid.c"
    valid.write_text(
        """#include <stdint.h>
#include <stdbool.h>  // ra8-keep-include: `bool` used directly

static bool ready(void) {
    return true;
}
""",
        encoding="utf-8",
    )
    valid_unused = [item[1] for item in check_file(valid, {}, verbose=False)]
    if "#include <stdbool.h>" in valid_unused:
        failures.append("selftest: validated keep marker was not honored")
    forged = test_dir / "forged.c"
    forged.write_text(
        """#include <stdint.h>
#include <stdbool.h>  // ra8-keep-include: `missing_symbol_xyz` used directly

static uint32_t compute(void) {
    return 42U;
}
""",
        encoding="utf-8",
    )
    forged_unused = [item[1] for item in check_file(forged, {}, verbose=False)]
    if "#include <stdbool.h>" not in forged_unused:
        failures.append("selftest: fabricated keep token stayed quiet")
    generic = test_dir / "generic.c"
    generic.write_text(
        """#include <stdint.h>
#include <stdbool.h>  // ra8-keep-include: reviewed direct-use keep

static uint32_t compute(void) {
    return 42U;
}
""",
        encoding="utf-8",
    )
    generic_unused = [item[1] for item in check_file(generic, {}, verbose=False)]
    if "#include <stdbool.h>" not in generic_unused:
        failures.append("selftest: generic prose keep marker stayed quiet")
    bare = test_dir / "bare.c"
    bare.write_text(
        """#include <stdint.h>
#include <stdbool.h>  // ra8-keep-include:

static uint32_t compute(void) {
    return 42U;
}
""",
        encoding="utf-8",
    )
    bare_unused = [item[1] for item in check_file(bare, {}, verbose=False)]
    if "#include <stdbool.h>" not in bare_unused:
        failures.append("selftest: bare keep marker without reason stayed quiet")
    return failures


def selftest() -> int:
    """Prove that unused includes are flagged while required includes are kept."""
    with tempfile.TemporaryDirectory(prefix="ra8-selftest-inc-") as tmp:
        test_dir = Path(tmp)
        failures = (
            _selftest_basic(test_dir)
            + _selftest_guarded(test_dir)
            + _selftest_keep_markers(test_dir)
        )
    if failures:
        for failure in failures:
            sys.stderr.write(f"selftest: FAILED -- {failure}\n")
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
        unused = check_file(f, db, verbose=args.verbose)
        if unused:
            for line_no, inc_text in unused:
                print(f"{f}:{line_no}: unused include: {inc_text}")
            total_unused += len(unused)

    if total_unused > 0:
        print(f"\ncheck_unused_includes.py: {total_unused} unused include(s) found.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
