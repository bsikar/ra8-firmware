#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural and formatting checker for GNU ld linker scripts.

WHY THIS EXISTS INSTEAD OF AN OFF-THE-SHELF LINTER
==================================================
There is no linter for the GNU ld script language. Nothing on the scale of
cmake-lint, yamllint or actionlint exists for `.ld`: the language is defined
only by the ld manual and its bison grammar, it has no published style guide,
and the one adjacent tool -- `ld --verbose` -- validates a script solely as a
side effect of attempting a real link, so it needs a full object set and a
target toolchain and reports nothing about the file as a file.

Leaving the type unenforced was not an option (84 first-party scripts decide
where every byte of this firmware lands), so this file enforces what IS
mechanically checkable about a linker script without linking it:

  LD001  licence header      -- SPDX-License-Identifier and a Copyright line.
  LD002  ENTRY declared      -- exactly one ENTRY(symbol) outside comments.
  LD003  MEMORY block        -- a MEMORY {} block declaring >= 1 region, each
                                with both ORIGIN and LENGTH.
  LD004  region closure      -- every region named by an output-section
                                placement (`> REGION`, `AT> REGION`) is one of
                                the regions MEMORY declares. A typo here is an
                                ld hard error, but ONLY for an app something
                                actually links; this reaches the scripts no
                                job builds.
  LD005  formatting          -- 7-bit ASCII, LF endings, a final newline, no
                                tab indentation, no trailing whitespace.
  LD006  symbol closure      -- every g_ra8_ls_* symbol a first-party C source
                                references is defined by at least one
                                first-party linker script. That catches a link
                                error which would otherwise surface only in
                                whichever app happens to pull the TU in -- and
                                for a script no CI job links, never at all.

The REVERSE direction (a script defines a g_ra8_ls_* nothing in C names) is
deliberately NOT a finding, and that is a statement about what is enforceable
rather than an exemption. A linker script legitimately exports boundary
symbols with no C consumer at all: g_ra8_ls_exidx_start/end delimit the
unwind table for the runtime, g_ra8_ls_noinit_start is documented in
ra8_crashlog.h purely as a GDB inspection point, and several are read only by
ASSERT() expressions elsewhere in the script or by whoever is reading the .map
file. "Unused" for such a symbol is not decidable from the source tree, so a
rule asserting it would be guessing -- it fired on 28 healthy symbols when
tried. What IS decidable is the direction above, and that is what runs.

LD006 is whole-tree, so it is reported once rather than per file.

Run with --selftest to prove the rules fire on a deliberately malformed script
and stay quiet on a legal-but-tricky one.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

SYMBOL_PREFIX = "g_ra8_ls_"
EXCLUDED_PREFIXES = ("libs/third_party/", "libs/fonts/")

# A comment in an ld script is /* ... */ only -- there is no line-comment form.
_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    """Blank out comment bodies, preserving newlines so line numbers hold."""

    def blank(m: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))

    return _COMMENT.sub(blank, text)


def repo_files(root: pathlib.Path, pattern: str) -> list[pathlib.Path]:
    """Tracked files matching a git pathspec, minus the vendored prefixes.

    Uses ``git ls-files`` rather than glob so untracked build artefacts and
    ignored copies are never scanned -- a stale generated .ld in a build tree
    would otherwise be held to the same rules as an authored one.
    """
    out = subprocess.run(  # noqa: S603  # fixed argv, no shell
        ["git", "-C", str(root), "ls-files", pattern],  # noqa: S607  # git from PATH is intended
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    return [root / p for p in out if not p.startswith(EXCLUDED_PREFIXES)]


class Finding:
    """One linker-script rule violation, identified by its LDxxx code.

    ``code`` is the stable identity the selftest asserts on, so message
    wording can be improved without disarming the test that proves the rule
    still fires.
    """

    def __init__(self, path: pathlib.Path, line: int, code: str, msg: str) -> None:
        """Record one finding; all four fields are required and none is derived."""
        self.path, self.line, self.code, self.msg = path, line, code, msg

    def __str__(self) -> str:
        """Render as ``path:line: [CODE] message`` -- editor-jumpable."""
        return f"{self.path}:{self.line}: [{self.code}] {self.msg}"


def _check_formatting(path: pathlib.Path, raw: bytes) -> tuple[list[Finding], str]:
    """LD005 -- encoding, line endings, indentation, trailing whitespace.

    Returns the findings and the decoded text, because the decode is where a
    non-ASCII byte is discovered and the rest of the checks need the result.
    """
    findings: list[Finding] = []
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        bad_line = raw[: exc.start].count(b"\n") + 1
        findings.append(Finding(path, bad_line, "LD005", f"non-ASCII byte 0x{raw[exc.start]:02x}"))
        text = raw.decode("ascii", errors="replace")

    if b"\r\n" in raw:
        line = raw.split(b"\r\n")[0].count(b"\n") + 1
        findings.append(Finding(path, line, "LD005", "CRLF line ending"))
    if raw and not raw.endswith(b"\n"):
        findings.append(Finding(path, text.count("\n") + 1, "LD005", "no final newline"))

    for i, ln in enumerate(text.splitlines(), 1):
        if ln.startswith("\t") or re.match(r"^ *\t", ln):
            findings.append(
                Finding(
                    path, i, "LD005", "tab indentation (this tree indents ld scripts with spaces)"
                )
            )
        if ln != ln.rstrip():
            findings.append(Finding(path, i, "LD005", "trailing whitespace"))
    return findings, text


def _check_licence(path: pathlib.Path, lines: list[str]) -> list[Finding]:
    """LD001 -- SPDX identifier and copyright line in the file head."""
    head = "\n".join(lines[:60])
    findings: list[Finding] = []
    if "SPDX-License-Identifier:" not in head:
        findings.append(
            Finding(path, 1, "LD001", "no SPDX-License-Identifier in the first 60 lines")
        )
    if not re.search(r"Copyright \(c\) \d{4}", head):
        findings.append(
            Finding(path, 1, "LD001", "no 'Copyright (c) <year>' line in the first 60 lines")
        )
    return findings


def _check_entry(path: pathlib.Path, code: str) -> list[Finding]:
    """LD002 -- exactly one ENTRY(symbol) declaration."""
    entries = re.findall(r"\bENTRY\s*\(\s*([A-Za-z_.$][\w.$]*)\s*\)", code)
    if not entries:
        return [Finding(path, 1, "LD002", "no ENTRY(symbol) declaration")]
    if len(entries) > 1:
        return [Finding(path, 1, "LD002", f"{len(entries)} ENTRY declarations, expected exactly 1")]
    return []


def _check_memory(path: pathlib.Path, code: str) -> tuple[list[Finding], set[str]]:
    """LD003 -- a MEMORY block whose every region carries ORIGIN and LENGTH.

    Also returns the declared region names, which LD004 needs to decide
    whether an output section lands somewhere that exists.
    """
    findings: list[Finding] = []
    regions: set[str] = set()
    mem_blocks = re.findall(r"\bMEMORY\s*\{(.*?)\n\}", code, re.DOTALL)
    if not mem_blocks:
        findings.append(Finding(path, 1, "LD003", "no MEMORY { } block"))
    for block in mem_blocks:
        for m in re.finditer(r"^\s*([A-Za-z_][\w]*)\s*(\([rwxail!]+\))?\s*:", block, re.MULTILINE):
            regions.add(m.group(1))
        if not regions:
            findings.append(Finding(path, 1, "LD003", "MEMORY block declares no regions"))
    for name in sorted(regions):
        decl = re.search(
            rf"^\s*{re.escape(name)}\s*(\([rwxail!]+\))?\s*:([^\n]*)$",
            code,
            re.MULTILINE,
        )
        if decl and not ("ORIGIN" in decl.group(2) and "LENGTH" in decl.group(2)):
            line = code[: decl.start()].count("\n") + 1
            findings.append(
                Finding(path, line, "LD003", f"region '{name}' lacks ORIGIN and/or LENGTH")
            )
    return findings, regions


def _check_region_closure(path: pathlib.Path, code: str, regions: set[str]) -> list[Finding]:
    """LD004 -- every output section is placed in a region MEMORY declares."""
    findings: list[Finding] = []
    for m in re.finditer(r"(?:AT)?>\s*([A-Za-z_][\w]*)", code):
        name = m.group(1)
        if name in regions:
            continue
        line = code[: m.start()].count("\n") + 1
        findings.append(
            Finding(path, line, "LD004", f"output section placed in undeclared region '{name}'")
        )
    return findings


# Real RA8D2 option-setting layout, HUM Ch 7 Figure 7.1 p 279 (secure aliases;
# OFS1/OFS3/BPS/PBPS are listed at the Non-secure alias 0x12.., the secure alias
# below addresses the same cell). This is the authority the linker scripts are
# checked against, and it is the same table scripts/gen has no business owning:
# a wrong option-byte address silently programs the wrong OTP cell (#391).
OPTION_SETTING_ADDR = {
    "OFS0_ADDR": 0x02C9F040,
    "OFS1_ADDR": 0x02C9F4C0,
    "OFS2_ADDR": 0x02C9F044,
    "OFS3_ADDR": 0x02C9F4C4,
    "SAS_ADDR": 0x02C9F074,
    "OFS1_SEC_ADDR": 0x02C9F0C0,
    "OFS1_SEL_ADDR": 0x02C9F120,
    "OFS3_SEC_ADDR": 0x02C9F0C4,
    "OFS3_SEL_ADDR": 0x02C9F124,
    "BPS_ADDR": 0x02C9F600,
    "BPS_SEC_ADDR": 0x02C9F200,
    "OTP_FSBLCTRL0_ADDR": 0x02E07600,
    "OTP_FSBLCTRL1_ADDR": 0x02E07604,
    "OTP_FSBLCTRL2_ADDR": 0x02E07608,
    "OTP_SAMR_ADDR": 0x02E07614,
    "OTP_SACC00_ADDR": 0x02E07620,
    "OTP_SACC10_ADDR": 0x02E07630,
    "OTP_SACC01_ADDR": 0x02E07640,
    "OTP_SACC11_ADDR": 0x02E07650,
    "OTP_SACC02_ADDR": 0x02E07660,
    "OTP_SACC12_ADDR": 0x02E07670,
    "OTP_SACC03_ADDR": 0x02E07680,
    "OTP_SACC13_ADDR": 0x02E07690,
    "OTP_PBPS_ADDR": 0x02E17780,
    "OTP_PBPS_SEC_ADDR": 0x02E17700,
    "OTP_ZHUK_ADDR": 0x02E17920,
}


def _check_option_setting(path: pathlib.Path, code: str) -> list[Finding]:
    """LD007 -- no phantom data-flash, and option-setting addresses match the HUM.

    The RA8D2 has no general-purpose data-flash / EEPROM array: 0x27000000 (the
    conventional RA-family data-flash base) faults on this silicon (#397). And
    the option bytes must land on their true addresses (#391) or the flasher
    programs the wrong OTP cell. Only scripts that actually declare the
    option-setting words are address-checked; every RA8 script is phantom-checked.
    """
    findings: list[Finding] = []
    for m in re.finditer(r"\bDATA_FLASH\b|0x2700_?0000", code):
        line = code[: m.start()].count("\n") + 1
        findings.append(
            Finding(
                path,
                line,
                "LD007",
                "phantom data-flash 0x27000000 -- the RA8D2 has no such region (#397)",
            )
        )
    for name, expect in OPTION_SETTING_ADDR.items():
        m = re.search(r"PROVIDE\(\s*" + re.escape(name) + r"\s*=\s*(0x[0-9A-Fa-f_]+)\s*\)", code)
        if not m:
            continue
        got = int(m.group(1).replace("_", ""), 16)
        if got != expect:
            line = code[: m.start()].count("\n") + 1
            findings.append(
                Finding(
                    path,
                    line,
                    "LD007",
                    f"{name} = {m.group(1)}, expected 0x{expect:08X} (HUM Ch 7 Figure 7.1 p 279)",
                )
            )
    return findings


def check_file(path: pathlib.Path, raw: bytes) -> list[Finding]:
    """Every linker-script rule, one function per finding code.

    The rule list is the call sequence below: LD005 formatting, LD001 licence,
    LD002 ENTRY, LD003 MEMORY, LD004 region closure, LD007 option-setting layout.
    """
    findings, text = _check_formatting(path, raw)
    findings += _check_licence(path, text.splitlines())
    code = strip_comments(text)
    findings += _check_entry(path, code)
    memory_findings, regions = _check_memory(path, code)
    findings += memory_findings
    findings += _check_region_closure(path, code, regions)
    findings += _check_option_setting(path, code)
    return findings


def defined_symbols(text: str) -> set[str]:
    """Linker symbols this script DEFINES, in any of the three spellings.

    Recognises ``sym = expr;``, ``PROVIDE(sym = expr)`` and
    ``PROVIDE_HIDDEN(sym = expr)`` alike, since all three make the symbol
    available to C and the closure check must not care which was used.

    Runs on the comment-blanked view, so a symbol named only in a comment is
    not counted as defined.
    """
    code = strip_comments(text)
    found: set[str] = set()
    # `sym = expr;`, `PROVIDE(sym = expr)`, `PROVIDE_HIDDEN(sym = expr)`
    for m in re.finditer(rf"\b({SYMBOL_PREFIX}\w+)\s*=", code):
        found.add(m.group(1))
    return found


def referenced_symbols(text: str) -> set[str]:
    """Linker symbols a C/C++ file REFERENCES, by prefix match.

    Comments are dropped first so a symbol discussed in prose does not count
    as a use -- otherwise documenting a symbol would keep it alive in the
    closure check forever.
    """
    # Drop C comments so a symbol named only in prose does not count as a use.
    stripped = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    stripped = re.sub(r"//[^\n]*", " ", stripped)
    return set(re.findall(rf"\b{SYMBOL_PREFIX}\w+", stripped))


def closure_problems(defined: dict[str, list[str]], referenced: dict[str, list[str]]) -> list[str]:
    """Pure half of LD006 so --selftest can drive it without a repo."""
    problems: list[str] = []
    for sym, users in sorted(referenced.items()):
        if sym not in defined:
            problems.append(
                f"[LD006] '{sym}' is referenced by C but no linker script "
                f"defines it (first use: {users[0]})"
            )
    return problems


def check_symbol_closure(root: pathlib.Path) -> list[str]:
    """LD006 -- cross-check symbols defined in .ld files against their uses in C.

    A whole-tree question by nature: a symbol is defined in one file and used
    in another, so unlike the per-file rules this cannot be answered from a
    staged subset and always scans everything.

    Returns one message per problem; an empty list means the closure holds.
    """
    defined: dict[str, list[str]] = {}
    for p in repo_files(root, "*.ld"):
        for s in defined_symbols(p.read_text(encoding="ascii", errors="replace")):
            defined.setdefault(s, []).append(str(p.relative_to(root)))

    referenced: dict[str, list[str]] = {}
    for pattern in ("*.c", "*.h", "*.cpp", "*.hpp"):
        for p in repo_files(root, pattern):
            text = p.read_text(encoding="ascii", errors="replace")
            if SYMBOL_PREFIX not in text:
                continue
            for s in referenced_symbols(text):
                referenced.setdefault(s, []).append(str(p.relative_to(root)))

    return closure_problems(defined, referenced)


# ---------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------
MALFORMED = """\
/* A linker script with no licence header. */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS
{
\t.text : { *(.text) } > FLSAH
    .data : { *(.data) } > RAM
}
"""

# Legal but deliberately awkward: ENTRY and a bogus region name appear inside
# comments, a region name is a substring of another, and the header sits below
# a long banner. Nothing here is a real finding.
TRICKY = """\
/*
 * A perfectly legal script that mentions ENTRY(bogus_reset) and a
 * region called NOWHERE inside this comment, and places nothing in
 * either. It also talks about > NOWHERE as prose.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 64K
    RAM_EXT (rwx) : ORIGIN = 0x60000000, LENGTH = 8M
}

SECTIONS
{
    .text : { *(.text) } > RAM_EXT
    .data : { *(.data) } > RAM AT> RAM_EXT
}
"""


# Option-setting layout: a phantom data-flash region plus a wrong OFS0 address
# (both must draw LD007), and a correct-address twin that must stay silent.
OFS_BAD = """\
/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K
    DATA_FLASH (rw) : ORIGIN = 0x27000000, LENGTH = 16K
    OFS_CFG (r) : ORIGIN = 0x02C9F000, LENGTH = 2K
}

PROVIDE(OFS0_ADDR = 0x0300A100);

SECTIONS
{
    .text : { *(.text) } > MRAM
    .option_setting_ofs0 OFS0_ADDR : { KEEP(*(.option_setting_ofs0)) } > OFS_CFG
}
"""

OFS_GOOD = """\
/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K
    OFS_CFG (r) : ORIGIN = 0x02C9F000, LENGTH = 2K
}

PROVIDE(OFS0_ADDR = 0x02C9F040);

SECTIONS
{
    .text : { *(.text) } > MRAM
    .option_setting_ofs0 OFS0_ADDR : { KEEP(*(.option_setting_ofs0)) } > OFS_CFG
}
"""


def _selftest_option_setting() -> int:
    """LD007 fires on the phantom region and a wrong OFS0 address, quiet on the twin."""
    rc = 0
    with tempfile.TemporaryDirectory() as td:
        bad = pathlib.Path(td) / "ofs_bad.ld"
        bad.write_bytes(OFS_BAD.encode())
        codes = {f.code for f in check_file(bad, bad.read_bytes())}
        if "LD007" not in codes:
            print("SELFTEST FAIL: ofs_bad.ld did not report LD007")
            rc = 1
        else:
            print("selftest: ofs_bad.ld -> LD007 (phantom + wrong OFS0) OK")

        good = pathlib.Path(td) / "ofs_good.ld"
        good.write_bytes(OFS_GOOD.encode())
        ld007 = [f for f in check_file(good, good.read_bytes()) if f.code == "LD007"]
        if ld007:
            print("SELFTEST FAIL: ofs_good.ld should have no LD007 but reported:")
            for f in ld007:
                print(f"    {f}")
            rc = 1
        else:
            print("selftest: ofs_good.ld -> no LD007 OK")
    return rc


def _selftest_fixtures() -> int:
    """The two whole-file fixtures: every code must fire, nothing may over-fire."""
    rc = 0
    with tempfile.TemporaryDirectory() as td:
        bad = pathlib.Path(td) / "malformed.ld"
        bad.write_bytes(MALFORMED.encode())
        got = check_file(bad, bad.read_bytes())
        codes = {f.code for f in got}
        expected = {"LD001", "LD002", "LD003", "LD004", "LD005"}
        missing = expected - codes
        if missing:
            print(f"SELFTEST FAIL: malformed.ld did not report {sorted(missing)}")
            for f in got:
                print(f"    got: {f}")
            rc = 1
        else:
            print(f"selftest: malformed.ld -> {len(got)} findings {sorted(codes)} OK")

        good = pathlib.Path(td) / "tricky.ld"
        good.write_bytes(TRICKY.encode())
        got = check_file(good, good.read_bytes())
        if got:
            print("SELFTEST FAIL: tricky.ld should be clean but reported:")
            for f in got:
                print(f"    {f}")
            rc = 1
        else:
            print("selftest: tricky.ld -> 0 findings OK")
    return rc


def _selftest_symbol_scan() -> tuple[int, set[str], set[str]]:
    """LD006 halves: a symbol named only in a comment is neither defined nor used."""
    rc = 0
    ld_text = (
        "/* mentions g_ra8_ls_in_comment_only, which is NOT a definition */\n"
        "g_ra8_ls_alpha = .;\n"
        "PROVIDE(g_ra8_ls_beta = 0x20000000);\n"
    )
    c_text = (
        "/* prose naming g_ra8_ls_prose_only must not count as a use */\n"
        "// nor g_ra8_ls_slash_comment\n"
        "extern uint32_t g_ra8_ls_alpha;\n"
        "extern uint32_t g_ra8_ls_missing;\n"
    )
    got_def = defined_symbols(ld_text)
    if got_def != {"g_ra8_ls_alpha", "g_ra8_ls_beta"}:
        print(f"SELFTEST FAIL: defined_symbols -> {sorted(got_def)}")
        rc = 1
    else:
        print("selftest: defined_symbols ignores comment mentions OK")

    got_ref = referenced_symbols(c_text)
    if got_ref != {"g_ra8_ls_alpha", "g_ra8_ls_missing"}:
        print(f"SELFTEST FAIL: referenced_symbols -> {sorted(got_ref)}")
        rc = 1
    else:
        print("selftest: referenced_symbols ignores comment mentions OK")
    return rc, got_def, got_ref


def _selftest_closure(got_def: set[str], got_ref: set[str]) -> int:
    """LD006 closure, both directions: fires on a gap, silent when resolved."""
    rc = 0
    defined = {s: ["fake.ld"] for s in got_def}
    referenced = {s: ["fake.c"] for s in got_ref}
    problems = closure_problems(defined, referenced)
    if len(problems) != 1 or "g_ra8_ls_missing" not in problems[0]:
        print(f"SELFTEST FAIL: closure_problems -> {problems}")
        rc = 1
    else:
        print("selftest: closure fires on an undefined symbol OK")

    if closure_problems({"g_ra8_ls_a": ["x.ld"]}, {"g_ra8_ls_a": ["x.c"]}):
        print("SELFTEST FAIL: closure fired on a fully-resolved symbol")
        rc = 1
    else:
        print("selftest: closure quiet when every symbol resolves OK")
    return rc


def selftest() -> int:
    """Assert every finding code fires, and that none of them over-fires."""
    rc = _selftest_fixtures()
    rc |= _selftest_option_setting()
    scan_rc, got_def, got_ref = _selftest_symbol_scan()
    return rc | scan_rc | _selftest_closure(got_def, got_ref)


def main() -> int:
    """Check every tracked linker script, or run the selftest / scope listing.

    Note the asymmetry: the per-file LD001-LD005 rules honour a positional
    path list, but the LD006 symbol closure always scans the whole tree
    because a definition and its use live in different files. Passing paths
    therefore narrows part of this gate and not all of it.

    ``--list-files`` prints the scope and exits 0 for check_lint_coverage.py.

    Returns 0 when clean, 1 on any finding or a failing selftest.
    """
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true", help="assert both directions")
    # Scope introspection for check_lint_coverage.py: print what this gate
    # would scan, so the coverage gate can ask rather than restate the scope.
    ap.add_argument("--list-files", action="store_true", help="print the scanned file list")
    ap.add_argument("paths", nargs="*", help="scripts to check (default: all tracked)")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    root = pathlib.Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],  # noqa: S607  # git from PATH is intended
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )

    paths = [pathlib.Path(p) for p in args.paths] or repo_files(root, "*.ld")
    if not paths:
        print("ERROR: no linker scripts found; refusing to report success.", file=sys.stderr)
        return 1

    if args.list_files:
        print("\n".join(sorted(str(p.relative_to(root)) for p in paths)))
        return 0

    findings: list[Finding] = []
    for p in paths:
        findings.extend(check_file(p, p.read_bytes()))

    problems = check_symbol_closure(root) if not args.paths else []

    for f in findings:
        print(f)
    for pr in problems:
        print(pr)

    total = len(findings) + len(problems)
    if total:
        print(f"\n{total} linker-script finding(s) in {len(paths)} file(s)")
        return 1
    print(f"linker scripts clean ({len(paths)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
