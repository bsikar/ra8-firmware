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
  LD007  option-setting      -- no phantom data-flash, and every option-setting
                                address matches the HUM (see OPTION_SETTING_ADDR).
  LD008  option completeness -- the option-setting family is ALL-OR-NOTHING and
                                CLOSED: a script either declares none of it (the
                                CPU1 / non-secure-image scripts) or declares every
                                word the HUM lists, with the matching output
                                section for each; and it may not place an
                                `.option_setting_*` section outside that family.
  LD009  fits the silicon    -- no MEMORY region declared inside the on-chip
                                SRAM window (0x22000000 .. 0x221A0000, i.e.
                                k_ra8_mem_sram_size = 1664 KiB) may extend past
                                that end. LD003/LD004 prove a region is declared
                                and closed; only this proves it is real memory,
                                the one enforcement a 0-byte placeholder no CI
                                job links can have (an ASSERT there never fires,
                                #544).

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

WHY LD008 IS ALL-OR-NOTHING RATHER THAN PER-DEVICE
==================================================
The obvious rule here would be "check the emitted OFS words against the target
device's feature set". That rule is the bug. Issue #223 deleted the OFS3 family
from the four RA8P1 app scripts because Renesas FSP's `BSP_FEATURE_BSP_HAS_OFS3`
is 0 for ra8p1 -- but the RA8P1 Hardware User's Manual (R01UH1064EJ0130 Ch 7.2.6
p 288, Ch 7.2.7 p 290) documents OFS3, OFS3_SEC and OFS3_SEL at the same
addresses and with the same WDT1 bit fields as the RA8D2. A device-feature table
would have had to encode that same wrong premise to pass, and would then have
enforced it (#516).

So LD008 asserts a structural invariant that needs no device knowledge: the
option-setting block is indivisible. Every RA8 script either owns the option
bytes and declares the COMPLETE family, or owns none of them and declares
nothing -- and that is exactly how the tree partitions: the boot scripts are
complete, the CPU1 / non-secure-image scripts are empty, and nothing sits in
between. (No count is quoted here on purpose: it would drift with every added
script and rot into a lie. `--list-files` reports the live scope, and
OPTION_SETTING_FILE_FLOOR below is the number that is actually enforced.)
A script at 23-of-26 is the defect signature in both directions: it catches a
word deleted from one script and not its siblings, AND a word added to one
script that the HUM does not list. The authority is OPTION_SETTING_ADDR, derived
from the HUM, so no constant is ever compared against itself.

Both option-setting rules carry a vacuity floor (OPTION_SETTING_FILE_FLOOR):
if the PROVIDE spelling ever changes, these rules would match zero files and
report a clean tree forever. Matching nothing is a failure, not a pass.

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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "dev"))

from git_environment import isolated_git_environment, trusted_git_executable
from linker_script_fixtures import MALFORMED, OFS_BAD, OFS_GOOD, TRICKY

SYMBOL_PREFIX = "g_ra8_ls_"
EXCLUDED_PREFIXES = ("libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/")

# A comment in an ld script is /* ... */ only -- there is no line-comment form.
_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    """Blank out comment bodies, preserving newlines so line numbers hold."""

    def blank(m: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))

    return _COMMENT.sub(blank, text)


def repo_files(root: pathlib.Path, pattern: str) -> list[pathlib.Path]:
    """Live candidate files matching a git pathspec, minus vendored prefixes.

    The candidate is the worktree, not merely the index: an unstaged move must
    drop the deleted source and include its untracked destination. Ignored build
    artefacts stay excluded, so a stale generated .ld in a build tree is never
    held to the same rules as an authored one.
    """
    argv = [
        "git",
        "-C",
        str(root),
        "ls-files",
        "--cached",
        "--others",
        "--exclude-standard",
        "--",
        pattern,
    ]
    out = subprocess.run(  # noqa: S603  # fixed git argv, no shell
        argv,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    return [root / p for p in out if not p.startswith(EXCLUDED_PREFIXES) and (root / p).is_file()]


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


# On-chip system SRAM extent, identical on RA8D2 and RA8P1 and mirrored by
# k_ra8_mem_sram_size (libs/ra8_core/inc/ra8_device.h): 1664 KiB = SRAM0 1024 KiB
# + SRAM1 640 KiB, so SRAM_WINDOW_END is the first address past the array.
SRAM_WINDOW_BASE = 0x22000000
SRAM_WINDOW_SIZE = 0x001A0000  # k_ra8_mem_sram_size: 1664 KiB, both parts.
SRAM_WINDOW_END = SRAM_WINDOW_BASE + SRAM_WINDOW_SIZE

_SIZE_UNIT = {"": 1, "K": 1024, "M": 1024 * 1024}
# A size/address expression this checker can evaluate statically: one or more
# `<number><K|M?>` terms joined by + or -, e.g. `1024K - 256`, `640K`, `0x22100000`.
_SIZE_TERM = re.compile(r"([+-]?)\s*(0x[0-9A-Fa-f_]+|\d+)\s*([KM]?)")
_SIZE_WHOLE = re.compile(r"(0x[0-9A-Fa-f_]+|\d+)\s*[KM]?(\s*[+-]\s*(0x[0-9A-Fa-f_]+|\d+)\s*[KM]?)*")


def eval_size(expr: str) -> int | None:
    """Evaluate an ORIGIN/LENGTH literal, or None when it is not static.

    Handles hex, decimal and K/M suffixes joined by + or - (``1024K - 256``). A
    symbolic ``ORIGIN()`` reference fails the whole-string match and returns
    None, so LD009 skips a region it cannot bound rather than guessing.
    """
    text = expr.strip()
    if not text or not _SIZE_WHOLE.fullmatch(text):
        return None
    total = 0
    for sign, num, unit in _SIZE_TERM.findall(text):
        total += (-1 if sign == "-" else 1) * int(num.replace("_", ""), 0) * _SIZE_UNIT[unit]
    return total


def _check_sram_fit(path: pathlib.Path, code: str, regions: set[str]) -> list[Finding]:
    """LD009 -- a region inside the SRAM window may not run past the array end.

    Only a region whose ORIGIN is statically evaluable AND lands inside
    [SRAM_WINDOW_BASE, SRAM_WINDOW_END) is judged -- SRAM, the NOINIT slice, and
    the NS_SRAM placeholder. The 0x32.. non-secure alias and every off-SRAM
    region fall outside the window and keep their own ASSERTs.
    """
    findings: list[Finding] = []
    for name in sorted(regions):
        decl = re.search(
            rf"^\s*{re.escape(name)}\s*(\([rwxail!]+\))?\s*:([^\n]*)$",
            code,
            re.MULTILINE,
        )
        if not decl:
            continue
        om = re.search(r"ORIGIN\s*=\s*([^,\n]+)", decl.group(2))
        lm = re.search(r"LENGTH\s*=\s*([^,\n]+)", decl.group(2))
        if not (om and lm):
            continue  # LD003 already reports a region missing ORIGIN/LENGTH.
        origin = eval_size(om.group(1))
        length = eval_size(lm.group(1))
        if origin is None or length is None:
            continue
        if not (SRAM_WINDOW_BASE <= origin < SRAM_WINDOW_END):
            continue
        end = origin + length
        if end > SRAM_WINDOW_END:
            line = code[: decl.start()].count("\n") + 1
            findings.append(
                Finding(
                    path,
                    line,
                    "LD009",
                    f"region '{name}' spans 0x{origin:08X}..0x{end:08X}, "
                    f"{end - SRAM_WINDOW_END} bytes past the end of on-chip SRAM "
                    f"(0x{SRAM_WINDOW_END:08X}); the array is 1664 KiB "
                    f"(k_ra8_mem_sram_size)",
                )
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


# A script that owns the option bytes must carry every word in the family; one
# that does not own them carries none. The complete population is comfortably
# into the sixties, so this floor sits far below a healthy tree -- low enough
# never to fight normal churn, high enough that a PROVIDE rename which silently
# stopped LD007/LD008 matching anything cannot slip past as a clean run.
OPTION_SETTING_FILE_FLOOR = 40


def option_section(name: str) -> str:
    """Map a PROVIDE symbol to the output section that word lands in.

    ``OFS3_SEC_ADDR`` -> ``.option_setting_ofs3_sec``. Deriving the section name
    from OPTION_SETTING_ADDR rather than keeping a second hand-written list is
    deliberate: two lists would drift, and the drift would disarm the rule.
    """
    return ".option_setting_" + name.removesuffix("_ADDR").lower()


def declared_option_words(code: str) -> set[str]:
    """The option-setting PROVIDE names this script declares (comment-blanked)."""
    return {
        name
        for name in OPTION_SETTING_ADDR
        if re.search(r"PROVIDE\(\s*" + re.escape(name) + r"\s*=", code)
    }


def placed_option_sections(code: str) -> set[str]:
    """Every ``.option_setting_*`` output section this script places."""
    return set(re.findall(r"^\s*(\.option_setting_\w+)", code, re.MULTILINE))


def _check_option_completeness(path: pathlib.Path, code: str) -> list[Finding]:
    """LD008 -- the option-setting family is all-or-nothing, and closed.

    See the module docstring for why this is structural rather than per-device.
    """
    findings: list[Finding] = []
    declared = declared_option_words(code)
    placed = placed_option_sections(code)
    known_sections = {option_section(n) for n in OPTION_SETTING_ADDR}

    # Closed: an .option_setting_* section outside the HUM family is a word the
    # silicon does not have, whichever part this script targets.
    for stray in sorted(placed - known_sections):
        line = next(
            (i for i, ln in enumerate(code.splitlines(), 1) if stray in ln),
            1,
        )
        findings.append(
            Finding(
                path,
                line,
                "LD008",
                f"unknown option-setting section '{stray}' -- not a word the HUM lists",
            )
        )

    if not declared and not (placed & known_sections):
        return findings  # Owns no option bytes at all -- legitimate, nothing to complete.

    missing_provides = sorted(set(OPTION_SETTING_ADDR) - declared)
    if missing_provides:
        findings.append(
            Finding(
                path,
                1,
                "LD008",
                f"declares {len(declared)}/{len(OPTION_SETTING_ADDR)} option-setting words; "
                f"the family is all-or-nothing, missing PROVIDE: {', '.join(missing_provides)}",
            )
        )

    missing_sections = sorted(known_sections - placed)
    if missing_sections:
        findings.append(
            Finding(
                path,
                1,
                "LD008",
                f"places {len(placed & known_sections)}/{len(known_sections)} option-setting "
                f"sections; missing: {', '.join(missing_sections)}",
            )
        )
    return findings


def check_file(path: pathlib.Path, raw: bytes) -> list[Finding]:
    """Every linker-script rule, one function per finding code.

    The rule list is the call sequence below: LD005 formatting, LD001 licence,
    LD002 ENTRY, LD003 MEMORY, LD004 region closure, LD009 SRAM fit, LD007
    option-setting addresses, LD008 option-setting completeness.
    """
    findings, text = _check_formatting(path, raw)
    findings += _check_licence(path, text.splitlines())
    code = strip_comments(text)
    findings += _check_entry(path, code)
    memory_findings, regions = _check_memory(path, code)
    findings += memory_findings
    findings += _check_region_closure(path, code, regions)
    findings += _check_sram_fit(path, code, regions)
    findings += _check_option_setting(path, code)
    findings += _check_option_completeness(path, code)
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


def _synth_option_script(omit: tuple[str, ...] = (), stray: str = "") -> str:
    """Build a syntactically real .ld declaring the option family minus `omit`.

    Generated from OPTION_SETTING_ADDR so the "complete" case cannot rot as the
    HUM table grows -- the point of that case is that LD008 stays SILENT on a
    complete script, which is only meaningful if "complete" tracks the table.
    """
    names = [n for n in OPTION_SETTING_ADDR if n not in omit]
    provides = "\n".join(f"PROVIDE({n} = 0x{OPTION_SETTING_ADDR[n]:08X});" for n in names)
    sections = "\n".join(
        f"    {option_section(n)} {n} : {{ KEEP(*({option_section(n)})) }} > OFS_CFG" for n in names
    )
    if stray:
        sections += f"\n    {stray} 0x02C9F800 : {{ KEEP(*({stray})) }} > OFS_CFG"
    return (
        "/*\n * Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n */\n\n"
        "ENTRY(Reset_Handler)\n\n"
        "MEMORY\n{\n"
        "    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K\n"
        "    OFS_CFG (r) : ORIGIN = 0x02C9F000, LENGTH = 2K\n"
        "    OFS_OTP (r) : ORIGIN = 0x02E07000, LENGTH = 68K\n}\n\n"
        f"{provides}\n\n"
        "SECTIONS\n{\n"
        "    .text : { *(.text) } > MRAM\n"
        f"{sections}\n}}\n"
    )


# The exact trio #223 deleted from the four RA8P1 app scripts. LD008 exists to
# make that deletion impossible to land again, so the selftest reproduces it
# rather than an invented omission.
OFS3_FAMILY = ("OFS3_ADDR", "OFS3_SEC_ADDR", "OFS3_SEL_ADDR")

# Below this many words, OPTION_SETTING_ADDR has plainly been gutted and every
# LD008 case built from it would pass without asserting anything.
MIN_OPTION_WORDS = 20


def _selftest_option_completeness() -> int:
    """LD008 fires on a partial family and on a stray section, silent when complete."""
    rc = 0
    # Anchor: an emptied or OFS3-less table would make every case below vacuous.
    missing = [n for n in OFS3_FAMILY if n not in OPTION_SETTING_ADDR]
    n_words = len(OPTION_SETTING_ADDR)
    if missing or n_words < MIN_OPTION_WORDS:
        print(f"SELFTEST FAIL: OPTION_SETTING_ADDR lost {missing or 'entries'}; LD008 vacuous")
        return 1
    print(f"selftest: OPTION_SETTING_ADDR has {n_words} words incl. the OFS3 family OK")

    with tempfile.TemporaryDirectory() as td:
        cases = [
            ("partial.ld", _synth_option_script(omit=OFS3_FAMILY), True, "OFS3 family cut (#223)"),
            ("complete.ld", _synth_option_script(), False, "complete family"),
            ("stray.ld", _synth_option_script(stray=".option_setting_ofs4"), True, "phantom ofs4"),
        ]
        for fname, text, want_fire, label in cases:
            p = pathlib.Path(td) / fname
            p.write_bytes(text.encode())
            ld008 = [f for f in check_file(p, p.read_bytes()) if f.code == "LD008"]
            if want_fire and not ld008:
                print(f"SELFTEST FAIL: {fname} ({label}) did not report LD008")
                rc = 1
            elif not want_fire and ld008:
                print(f"SELFTEST FAIL: {fname} ({label}) should have no LD008 but reported:")
                for f in ld008:
                    print(f"    {f}")
                rc = 1
            else:
                verdict = "LD008" if want_fire else "no LD008"
                print(f"selftest: {fname} -> {verdict} ({label}) OK")

        # A script owning no option bytes at all must stay silent -- that is the
        # CPU1 / non-secure-image shape, 64 files in this tree.
        none = pathlib.Path(td) / "none.ld"
        none.write_bytes(TRICKY.encode())
        if [f for f in check_file(none, none.read_bytes()) if f.code == "LD008"]:
            print("SELFTEST FAIL: a script with no option-setting block reported LD008")
            rc = 1
        else:
            print("selftest: none.ld -> no LD008 (owns no option bytes) OK")
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


def _selftest_worktree_inventory() -> int:
    """Candidate scope includes an unstaged move target and drops its source."""
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        subprocess.run(  # noqa: S603 -- fixed Git argv and private fixture path
            [trusted_git_executable(), "init", "-q", str(root)],
            check=True,
        )
        old = root / "old.c"
        old.write_text("int old_symbol;\n", encoding="utf-8")
        subprocess.run(  # noqa: S603 -- fixed Git argv and private fixture path
            [trusted_git_executable(), "-C", str(root), "add", "old.c"],
            check=True,
        )
        old.unlink()
        new = root / "new.c"
        new.write_text("int new_symbol;\n", encoding="utf-8")

        got = [path.relative_to(root).as_posix() for path in repo_files(root, "*.c")]
        if got != ["new.c"]:
            print(f"SELFTEST FAIL: worktree move inventory -> {got}")
            return 1
        print("selftest: worktree move drops deleted source and includes destination OK")
    return 0


def _sram_fixture(ns_sram_len: str) -> str:
    """A board-shaped script whose NS_SRAM placeholder is sized `ns_sram_len`.

    The other rows are load-bearing: SRAM ``1024K - 256`` exercises subtraction,
    NOINIT sits at the top of SRAM (must stay silent), and NS_SRAM_RUN at 0x32..
    is the non-secure alias that must fall OUTSIDE the window LD009 judges.
    """
    return (
        "/*\n * Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n */\n\n"
        "ENTRY(Reset_Handler)\n\n"
        "MEMORY\n{\n"
        "    SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 1024K - 256\n"
        "    NOINIT (rw) : ORIGIN = 0x220FFF00, LENGTH = 256\n"
        f"    NS_SRAM (rwx) : ORIGIN = 0x22100000, LENGTH = {ns_sram_len}\n"
        "    NS_SRAM_RUN (rwx) : ORIGIN = 0x32100000, LENGTH = 512K\n}\n"
    )


def _selftest_sram_fit() -> int:
    """LD009 fires on a region past the SRAM end, silent when every region fits."""
    rc = 0
    # Anchor: a collapsed evaluator or a zeroed window constant would make every
    # case below vacuous. Compare only named constants and eval_size results,
    # never a bare literal, encoding the real bank arithmetic (1M + 640K = 1664K).
    if eval_size("1664K") != SRAM_WINDOW_SIZE or eval_size("1024K") != eval_size("1M"):
        print("SELFTEST FAIL: eval_size unit / SRAM-window anchor is wrong")
        return 1
    if eval_size("1M - 384K") != eval_size("640K") or eval_size("ORIGIN(SRAM)") is not None:
        print("SELFTEST FAIL: eval_size mis-handled subtraction or a symbolic expr")
        return 1

    with tempfile.TemporaryDirectory() as td:
        # 1024K overruns to 0x22200000 (the #544 defect); 640K lands exactly on
        # 0x221A0000, proving the bound is inclusive.
        for tag, ns_len, want in (("overrun.ld", "1024K", True), ("fits.ld", "640K", False)):
            p = pathlib.Path(td) / tag
            p.write_bytes(_sram_fixture(ns_len).encode())
            ld009 = [f for f in check_file(p, p.read_bytes()) if f.code == "LD009"]
            if want and (len(ld009) != 1 or "NS_SRAM" not in ld009[0].msg):
                print(f"SELFTEST FAIL: {tag} expected one LD009 on NS_SRAM, got {ld009}")
                rc = 1
            elif not want and ld009:
                print(f"SELFTEST FAIL: {tag} should have no LD009 but reported {ld009}")
                rc = 1
            else:
                print(f"selftest: {tag} -> {'LD009 on NS_SRAM' if want else 'no LD009'} OK")
    return rc


def _selftest_body() -> int:
    """Assert every finding code fires, and that none of them over-fires."""
    rc = _selftest_fixtures()
    rc |= _selftest_option_setting()
    rc |= _selftest_option_completeness()
    rc |= _selftest_sram_fit()
    scan_rc, got_def, got_ref = _selftest_symbol_scan()
    return rc | scan_rc | _selftest_closure(got_def, got_ref) | _selftest_worktree_inventory()


def selftest() -> int:
    """Run linker-script fixtures without inheriting the caller's repository."""
    with isolated_git_environment():
        return _selftest_body()


def scan(paths: list[pathlib.Path]) -> tuple[list[Finding], int]:
    """Run every per-file rule, and count scripts with the complete option family.

    The count is what the vacuity floor is asserted against, so it is produced
    by the same pass that applies the rules rather than by a second walk that
    could drift from it.
    """
    findings: list[Finding] = []
    complete = 0
    for p in paths:
        raw = p.read_bytes()
        findings.extend(check_file(p, raw))
        code = strip_comments(raw.decode("utf-8", errors="replace"))
        if declared_option_words(code) == set(OPTION_SETTING_ADDR):
            complete += 1
    return findings, complete


def option_floor_breached(complete: int) -> bool:
    """Vacuity floor for LD007/LD008 -- report and fail when they match too little.

    Both rules hinge on matching a ``PROVIDE`` spelling, which makes them the
    two most able to fail OPEN: a rename would leave them matching nothing and
    reporting a clean tree forever. Matching almost nothing is a gate failure,
    not a pass.
    """
    if complete >= OPTION_SETTING_FILE_FLOOR:
        return False
    print(
        f"ERROR: only {complete} script(s) declare the complete option-setting "
        f"family, below the floor of {OPTION_SETTING_FILE_FLOOR}. Either the "
        f"PROVIDE spelling changed (LD007/LD008 now match nothing and enforce "
        f"nothing) or the option bytes were mass-deleted. Refusing to report success.",
        file=sys.stderr,
    )
    return True


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
        subprocess.run(  # noqa: S603 -- fixed Git authority and constant read-only query
            [trusted_git_executable(), "rev-parse", "--show-toplevel"],
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

    findings, complete = scan(paths)

    # The floor is only meaningful on a whole-tree run; a positional path list
    # legitimately narrows the scan to a handful of files.
    if not args.paths and option_floor_breached(complete):
        return 1

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
