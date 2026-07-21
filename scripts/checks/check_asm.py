#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural and formatting checker for hand-written GNU assembler sources.

WHY THIS EXISTS INSTEAD OF AN OFF-THE-SHELF LINTER
==================================================
There is no linter for GNU `as`. This was searched for rather than assumed, and
the conclusion is worth writing down so the next person does not repeat it:

  * No tool in the shape of cmake-lint / yamllint / shellcheck exists for
    assembly. The language is whatever the target's `as` accepts, there is no
    published style guide to encode, and the analysers that do read assembly
    (objdump, radare2, Ghidra) consume ASSEMBLED objects, not source text.
  * `as` / `gcc -c` does validate syntax -- but only for one target, with that
    target's toolchain present and the right `-I` and `-D` set. The two files
    here are for DIFFERENT architectures: `port/threadx/.../*.S` is Armv8.1-M
    Thumb and `esp32/boot/start.S` is rv32imac. The tree provisions
    arm-none-eabi only, so an assemble-based gate could check one file and
    would have to SKIP the other -- and a gate that skips is the exact defect
    this suite exists to eliminate.
  * Where assembling IS possible it is already happening: the ThreadX port file
    is assembled by the `build-cross` gate as part of every ThreadX app. The
    RISC-V file is assembled by nothing in CI, because no riscv toolchain is
    installed, so structural checking is its ONLY coverage.

So this enforces what is mechanically checkable about assembly source as text,
in the same spirit as `check_linker_scripts.py` (which exists because no linter
for GNU ld script exists either):

  AS001  licence header    -- SPDX-License-Identifier and a Copyright line.
                              check-copyright.py's EXTENSIONS set covers
                              .c/.h/.cpp/.hpp/.cmake/.sh/.py and has never
                              included .S, so assembly was exempt from the
                              header rule the whole tree otherwise follows.
  AS002  explicit section  -- a `.section` / `.text` / `.data` directive before
                              the first label or instruction. Falling into the
                              assembler's default section is how hand-written
                              startup code silently lands somewhere the linker
                              script does not place.
  AS003  exported symbols  -- every `.globl` / `.global` symbol must have a
                              `.type`, a `.size`, and an actual label. Each
                              half is load-bearing: without `.type ...,%function`
                              the ARM linker does not set the Thumb bit and
                              indirect calls to the symbol land one byte off in
                              Arm state; without `.size` the symbol has length
                              zero, so `--gc-sections` may discard it and
                              debuggers cannot attribute a backtrace to it; and
                              a `.globl` with no label exports nothing at all.
  AS004  unified syntax    -- ARM sources must declare `.syntax unified`. The
                              pre-UAL divided syntax is still the assembler
                              DEFAULT, and it silently changes how flag-setting
                              mnemonics are parsed. Applied only to files that
                              carry an ARM-specific directive, so the RISC-V
                              source is not held to an ARM rule.
  AS005  formatting        -- 7-bit ASCII, LF endings, a final newline, no tab
                              indentation, no trailing whitespace.

WHAT IS DELIBERATELY NOT ENFORCED
---------------------------------
Instruction-level style (operand column, mnemonic case, register naming) is not
checked. The two files follow different house styles inherited from their
upstreams -- the ThreadX port keeps Express Logic's deep indentation so it can
be diffed against the vendor original, which is a real maintenance property.
Imposing one layout would mean rewriting a file whose value is that it diffs
cleanly, so the rule would cost more than it protects. That is a decision, not
an oversight; AS005 still holds the parts that cannot be justified either way.

Run with --selftest to prove every rule fires on a deliberately malformed
source and stays quiet on a legal one.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- trusted: fixed git argv
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
)

EXCLUDED_PREFIXES = ("libs/third_party/", "libs/fonts/")

# A tree that has assembly cannot legitimately have none. Same trip-wire as
# every other gate here: an empty scan must fail, never pass.
FILE_FLOOR = 1

# Directives that mark a source as ARM, and so subject to AS004.
_ARM_MARKERS = (".thumb_func", ".eabi_attribute", ".cpu", ".fpu", ".arm", ".thumb")

# Directives that establish an output section.
_SECTION_DIRECTIVES = (".section", ".text", ".data", ".bss", ".rodata")

_EXPORT_RE = re.compile(r"^\s*\.(?:globl|global)\s+([A-Za-z_.$][\w.$]*)")
_TYPE_RE = re.compile(r"^\s*\.type\s+([A-Za-z_.$][\w.$]*)")
_SIZE_RE = re.compile(r"^\s*\.size\s+([A-Za-z_.$][\w.$]*)")
_LABEL_RE = re.compile(r"^\s*([A-Za-z_.$][\w.$]*)\s*:")
_DIRECTIVE_RE = re.compile(r"^\s*\.")
# A line whose first non-space character is `#` is a C preprocessor directive
# (#ifdef / #include / #endif), which both of these sources use. It is NOT an
# instruction. `#` is not treated as a comment marker anywhere else here,
# because on ARM it introduces an immediate operand (`mov r0, #1`) -- but an
# immediate can never be the first token on a line, so anchoring to the start
# of the line separates the two cases exactly.
_CPP_RE = re.compile(r"^\s*#")


def strip_comments(text: str) -> str:
    """Blank out /* */ and // comment bodies, preserving line structure.

    `#` is NOT treated as a comment marker: it introduces an immediate operand
    on ARM (`mov r0, #1`) and a preprocessor directive on both targets. Blanking
    it would erase real code.
    """

    def blank(match: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", match.group(0))

    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", blank, text)


class Finding:
    """One rule violation, reported as path:line: [CODE] message."""

    def __init__(self, rel: str, line: int, code: str, msg: str) -> None:
        self.rel, self.line, self.code, self.msg = rel, line, code, msg

    def __str__(self) -> str:
        return f"{self.rel}:{self.line}: [{self.code}] {self.msg}"


def _check_format(raw: bytes, add) -> str:
    """AS005, operating on the raw bytes. Returns the decoded text."""
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        add(raw[: exc.start].count(b"\n") + 1, "AS005", f"non-ASCII byte 0x{raw[exc.start]:02x}")
        text = raw.decode("ascii", errors="replace")
    if b"\r\n" in raw:
        add(raw.split(b"\r\n")[0].count(b"\n") + 1, "AS005", "CRLF line ending")
    if raw and not raw.endswith(b"\n"):
        add(text.count("\n") + 1, "AS005", "no final newline")
    for num, line in enumerate(text.splitlines(), start=1):
        if line.startswith("\t") or re.match(r"^ *\t", line):
            add(num, "AS005", "tab indentation (use spaces)")
        if line != line.rstrip():
            add(num, "AS005", "trailing whitespace")
    return text


def _check_exports(code_lines: list[str], add) -> None:
    """AS003: every exported symbol has a type, a size and a label."""
    exports: dict[str, int] = {}
    typed: set[str] = set()
    sized: set[str] = set()
    labels: set[str] = set()
    for num, line in enumerate(code_lines, start=1):
        match = _EXPORT_RE.match(line)
        if match:
            exports.setdefault(match.group(1), num)
        match = _TYPE_RE.match(line)
        if match:
            typed.add(match.group(1))
        match = _SIZE_RE.match(line)
        if match:
            sized.add(match.group(1))
        match = _LABEL_RE.match(line)
        if match and not _DIRECTIVE_RE.match(line):
            labels.add(match.group(1))
    for sym, num in sorted(exports.items(), key=lambda kv: kv[1]):
        if sym not in labels:
            add(num, "AS003", f"'{sym}' is exported but no label defines it in this file")
        if sym not in typed:
            add(num, "AS003", f"'{sym}' is exported without a `.type {sym}, %function` directive")
        if sym not in sized:
            add(num, "AS003", f"'{sym}' is exported without a `.size {sym}, . - {sym}` directive")


def check_file(rel: str, raw: bytes) -> list[Finding]:
    """Every finding for one assembly source."""
    findings: list[Finding] = []

    def add(line: int, code: str, msg: str) -> None:
        findings.append(Finding(rel, line, code, msg))

    text = _check_format(raw, add)

    # AS001 -- licence header, read from the full text (it lives in a comment).
    if "SPDX-License-Identifier" not in text:
        add(1, "AS001", "no SPDX-License-Identifier in the file header")
    if "Copyright" not in text:
        add(1, "AS001", "no Copyright line in the file header")

    code = strip_comments(text)
    code_lines = code.splitlines()

    # AS002 -- an explicit section before the first label or instruction.
    section_at = None
    for num, line in enumerate(code_lines, start=1):
        stripped = line.strip()
        if not stripped or _CPP_RE.match(line):
            continue
        if stripped.startswith(_SECTION_DIRECTIVES):
            section_at = num
            break
        if _LABEL_RE.match(line) and not _DIRECTIVE_RE.match(line):
            add(num, "AS002", f"label '{stripped}' precedes any .section directive")
            break
        if not _DIRECTIVE_RE.match(line):
            add(num, "AS002", "instruction precedes any .section directive")
            break
    if section_at is None and not any(
        ln.strip().startswith(_SECTION_DIRECTIVES) for ln in code_lines
    ):
        add(1, "AS002", "no .section / .text / .data directive anywhere in the file")

    # AS004 -- unified syntax, for ARM sources only.
    if any(marker in code for marker in _ARM_MARKERS) and ".syntax unified" not in code:
        add(1, "AS004", "ARM source does not declare `.syntax unified` (divided is the default)")

    _check_exports(code_lines, add)
    return findings


def targets() -> list[str]:
    """Every first-party assembly source, repo-relative and sorted."""
    proc = subprocess.run(
        ["git", "ls-files", "-z", "*.S", "*.s"],  # noqa: S607 -- git from PATH is intended
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return sorted(
        rel for rel in proc.stdout.split("\0") if rel and not rel.startswith(EXCLUDED_PREFIXES)
    )


def _expect(cond: bool, label: str, failures: list[str]) -> None:
    print(f"  [{'ok' if cond else 'FAIL'}] {label}")
    if not cond:
        failures.append(label)


GOOD = b"""/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
    .section .text.boot, "ax", @progbits
    .syntax unified
    .thumb_func
    .globl  _start
    .type   _start, %function
_start:
    b       _start
    .size   _start, . - _start
"""

BAD = b"    .globl  _start\n_start:\n\tb       _start   \n    .thumb_func\n"

# The ThreadX port opens with #ifdef/#include before its .section; AS002 must
# read those as preprocessor lines, not as instructions.
HEADER = b"/*\n * SPDX-License-Identifier: MIT\n * Copyright (c) 2026 x\n */\n"
CPP_BEFORE_SECTION = HEADER + b'#ifdef TX_INCLUDE_USER_DEFINE_FILE\n#include "tx_user.h"\n#endif\n'


def selftest() -> int:
    """Assert every rule fires on a malformed source and stays quiet on a good one."""
    print("check_asm.py --selftest")
    failures: list[str] = []

    quiet = check_file("good.S", GOOD)
    _expect(not quiet, f"a conforming source yields no findings (got {quiet})", failures)

    loud = check_file("bad.S", BAD)
    codes = {f.code for f in loud}
    for code, why in (
        ("AS001", "missing licence header fires"),
        ("AS002", "a label before any .section fires"),
        ("AS003", "an export with no .type / .size fires"),
        ("AS004", "an ARM source without .syntax unified fires"),
        ("AS005", "tab indentation and trailing whitespace fire"),
    ):
        _expect(code in codes, why, failures)

    # Each half of AS003 independently.
    no_size = GOOD.replace(b"    .size   _start, . - _start\n", b"")
    _expect(
        any("without a `.size" in f.msg for f in check_file("x.S", no_size)),
        "dropping only .size fires AS003",
        failures,
    )
    no_type = GOOD.replace(b"    .type   _start, %function\n", b"")
    _expect(
        any("without a `.type" in f.msg for f in check_file("x.S", no_type)),
        "dropping only .type fires AS003",
        failures,
    )
    no_label = GOOD.replace(b"_start:\n", b"")
    _expect(
        any("no label defines it" in f.msg for f in check_file("x.S", no_label)),
        "dropping only the label fires AS003",
        failures,
    )

    # AS004 must NOT fire on a non-ARM source.
    riscv = GOOD.replace(b"    .syntax unified\n", b"").replace(b"    .thumb_func\n", b"")
    _expect(
        not any(f.code == "AS004" for f in check_file("rv.S", riscv)),
        "a non-ARM source is not held to the ARM .syntax rule",
        failures,
    )

    # A preprocessor directive is not an instruction: the ThreadX port opens
    # with #ifdef/#include before its .section, and AS002 must not fire on it.
    cpp = CPP_BEFORE_SECTION + GOOD.split(b"*/\n", 1)[1]
    _expect(
        not any(f.code == "AS002" for f in check_file("cpp.S", cpp)),
        "a #ifdef / #include before .section is not an instruction (AS002 quiet)",
        failures,
    )
    _expect(
        any(f.code == "AS002" for f in check_file("i.S", HEADER + b"    nop\n")),
        "a real instruction before .section still fires AS002",
        failures,
    )

    # A comment mentioning a directive must not satisfy a rule.
    commented = GOOD.replace(b"    .section .text.boot", b"    /* .section */\n    .section .x")
    _expect(
        not any(f.code == "AS002" for f in check_file("c.S", commented)),
        "a commented-out directive does not confuse the section scan",
        failures,
    )

    # Missing final newline.
    _expect(
        any("no final newline" in f.msg for f in check_file("n.S", GOOD.rstrip(b"\n"))),
        "a missing final newline fires AS005",
        failures,
    )

    # Non-ASCII.
    _expect(
        any("non-ASCII" in f.msg for f in check_file("u.S", GOOD.replace(b"MIT", b"MIT\xc2\xa9"))),
        "a non-ASCII byte fires AS005",
        failures,
    )

    if failures:
        print(f"\nSELFTEST FAILED: {len(failures)} assertion(s)", file=sys.stderr)
        for item in failures:
            print(f"  {item}", file=sys.stderr)
        return 1
    print("selftest: all assertions held (both directions).")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Structural checker for GNU assembler sources")
    ap.add_argument("--selftest", action="store_true", help="assert every rule, both directions")
    ap.add_argument("--list-files", action="store_true", help="print the scanned file list")
    args = ap.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    files = targets()
    if args.list_files:
        print("\n".join(files))
        return 0

    if len(files) < FILE_FLOOR:
        print(
            f"check_asm.py: FATAL -- {len(files)} assembly source(s) found, floor is "
            f"{FILE_FLOOR}. An empty scan reports success because it saw nothing.",
            file=sys.stderr,
        )
        return 2

    findings: list[Finding] = []
    for rel in files:
        findings.extend(check_file(rel, (REPO_ROOT / rel).read_bytes()))
    if findings:
        print(
            f"\n{len(findings)} finding(s) in {len(files)} assembly source(s):\n", file=sys.stderr
        )
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(f"check_asm.py: {len(files)} assembly source(s), no findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
