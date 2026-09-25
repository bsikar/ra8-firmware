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
                                SRAM window (k_ra8_mem_sram_base ..
                                + k_ra8_mem_sram_size = 1664 KiB) may extend
                                past that end. LD003/LD004 prove a region is
                                declared and closed; only this proves it is real
                                memory, the one enforcement a 0-byte placeholder
                                no CI job links can have (an ASSERT there never
                                fires, #544).
  LD010  named region agrees -- a MEMORY region NAMED for a device memory
                                region (MRAM, MRAM_CPU1, ITCM, DTCM, SRAM,
                                SRAM_CPU1, SDRAM) must actually lie inside that
                                region's window as libs/ra8_core/inc/ra8_device.h
                                declares it. LD009 judges by ADDRESS (anything
                                landing in the SRAM window), so it cannot see an
                                ITCM region parked at 0x30000000, a "SRAM" at the
                                MRAM base, or an ITCM twice the size of the TCM
                                the silicon has. The bounds are READ from
                                ra8_device.h, never restated here, which is the
                                point: that header calls itself the single source
                                of truth the linker scripts mirror, and until
                                this rule existed nothing in the tree read it.

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


# The device memory map, READ from the header that declares itself its single
# source of truth rather than restated here. Copying the numbers is what this
# rule set is fixing: libs/ra8_core/inc/ra8_device.h says the enums below are
# "the runtime mirror of the MEMORY { } block in every app's linker_script.ld;
# keep the two in lock-step", and nothing in the tree read them, so nothing
# enforced the lock-step either (issue #1048).
DEVICE_HEADER = pathlib.Path(__file__).resolve().parents[2] / "libs/ra8_core/inc/ra8_device.h"

# One enum row: `k_ra8_mem_sram_base = 0x22000000U, /**< ... */`. Only the
# k_ra8_mem_* family is read; the device-id and feature enums are not a map.
_DEVICE_MEM_ROW = re.compile(r"^\s*(k_ra8_mem_\w+)\s*=\s*(0x[0-9A-Fa-f_]+|\d+)U?\s*,", re.MULTILINE)

# Every name LD009/LD010 resolve. Requiring the whole set (rather than reading
# whatever happens to be there) is what makes a renamed or deleted enum a loud
# failure instead of a rule that quietly stops judging that region.
DEVICE_MEM_REQUIRED = (
    "k_ra8_mem_mram_base",
    "k_ra8_mem_mram_size",
    "k_ra8_mem_itcm_base",
    "k_ra8_mem_itcm_size",
    "k_ra8_mem_dtcm_base",
    "k_ra8_mem_dtcm_size",
    "k_ra8_mem_sram_base",
    "k_ra8_mem_sram_size",
    "k_ra8_mem_sdram_base",
)


def parse_device_memory_map(text: str) -> dict[str, int]:
    """Parse the `k_ra8_mem_*` enum rows of ra8_device.h into name -> value.

    Takes the header TEXT (not a path) so the selftest can feed it a synthetic
    header and prove both directions. Raises ValueError when any name in
    ``DEVICE_MEM_REQUIRED`` is absent: a silently-empty map would disarm every
    rule built on it, which is the failure mode this whole file exists to avoid.
    """
    found = {name: int(value.replace("_", ""), 0) for name, value in _DEVICE_MEM_ROW.findall(text)}
    missing = [name for name in DEVICE_MEM_REQUIRED if name not in found]
    if missing:
        msg = f"ra8_device.h: memory-map enum(s) missing: {', '.join(missing)}"
        raise ValueError(msg)
    return found


DEVICE_MEM = parse_device_memory_map(DEVICE_HEADER.read_text(encoding="utf-8"))

# MEMORY-region name -> (base enum, size enum or None). A region named for a
# device region is held to that region's extent by LD010. MRAM_CPU1 / SRAM_CPU1
# are the second-core slices carved out of the same physical array, so they are
# judged against the same window rather than given one of their own. SDRAM is
# external and the header carries no size for it, so only its base is pinned.
DEVICE_REGIONS = {
    "MRAM": ("k_ra8_mem_mram_base", "k_ra8_mem_mram_size"),
    "MRAM_CPU1": ("k_ra8_mem_mram_base", "k_ra8_mem_mram_size"),
    "ITCM": ("k_ra8_mem_itcm_base", "k_ra8_mem_itcm_size"),
    "DTCM": ("k_ra8_mem_dtcm_base", "k_ra8_mem_dtcm_size"),
    "SRAM": ("k_ra8_mem_sram_base", "k_ra8_mem_sram_size"),
    "SRAM_CPU1": ("k_ra8_mem_sram_base", "k_ra8_mem_sram_size"),
    "SDRAM": ("k_ra8_mem_sdram_base", None),
}

# On-chip system SRAM extent, identical on RA8D2 and RA8P1: 1664 KiB = SRAM0
# 1024 KiB + SRAM1 640 KiB, so SRAM_WINDOW_END is the first address past the
# array. Both numbers come from DEVICE_MEM, so the array can only be resized in
# one place.
SRAM_WINDOW_BASE = DEVICE_MEM["k_ra8_mem_sram_base"]
SRAM_WINDOW_SIZE = DEVICE_MEM["k_ra8_mem_sram_size"]
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


def region_extent(code: str, name: str) -> tuple[int, int, int] | None:
    """Statically evaluated (origin, length, line) of one MEMORY region.

    None when the region is absent, lacks ORIGIN/LENGTH (LD003 already reports
    that), or spells either as an expression ``eval_size`` cannot evaluate --
    a symbolic ``ORIGIN(SRAM)`` is skipped rather than guessed at.
    """
    decl = re.search(
        rf"^\s*{re.escape(name)}\s*(\([rwxail!]+\))?\s*:([^\n]*)$",
        code,
        re.MULTILINE,
    )
    if not decl:
        return None
    om = re.search(r"ORIGIN\s*=\s*([^,\n]+)", decl.group(2))
    lm = re.search(r"LENGTH\s*=\s*([^,\n]+)", decl.group(2))
    if not (om and lm):
        return None
    origin = eval_size(om.group(1))
    length = eval_size(lm.group(1))
    if origin is None or length is None:
        return None
    return origin, length, code[: decl.start()].count("\n") + 1


def _check_sram_fit(path: pathlib.Path, code: str, regions: set[str]) -> list[Finding]:
    """LD009 -- a region inside the SRAM window may not run past the array end.

    Only a region whose ORIGIN is statically evaluable AND lands inside
    [SRAM_WINDOW_BASE, SRAM_WINDOW_END) is judged -- SRAM, the NOINIT slice, and
    the NS_SRAM placeholder. The 0x32.. non-secure alias and every off-SRAM
    region fall outside the window and keep their own ASSERTs.
    """
    findings: list[Finding] = []
    for name in sorted(regions):
        extent = region_extent(code, name)
        if extent is None:
            continue
        origin, length, decl_line = extent
        if not (SRAM_WINDOW_BASE <= origin < SRAM_WINDOW_END):
            continue
        end = origin + length
        if end > SRAM_WINDOW_END:
            findings.append(
                Finding(
                    path,
                    decl_line,
                    "LD009",
                    f"region '{name}' spans 0x{origin:08X}..0x{end:08X}, "
                    f"{end - SRAM_WINDOW_END} bytes past the end of on-chip SRAM "
                    f"(0x{SRAM_WINDOW_END:08X}); the array is 1664 KiB "
                    f"(k_ra8_mem_sram_size)",
                )
            )
    return findings


def _check_device_region_fit(path: pathlib.Path, code: str, regions: set[str]) -> list[Finding]:
    """LD010 -- a region named for a device memory region must lie inside it.

    LD009 judges by ADDRESS: it catches anything that lands in the SRAM window
    and runs off the end. This rule judges by NAME, which is the direction
    LD009 cannot see -- an ITCM parked at 0x30000000, a region called SRAM at
    the MRAM base, an ITCM twice the size of the TCM the silicon has. Bounds
    come from DEVICE_MEM (parsed from ra8_device.h), never from a literal here.

    Overrun inside the SRAM window stays LD009's finding so one defect is not
    reported twice; this rule reports overrun only for the windows LD009 does
    not judge (MRAM and the two TCMs).
    """
    findings: list[Finding] = []
    for name in sorted(regions):
        window = DEVICE_REGIONS.get(name)
        if window is None:
            continue
        extent = region_extent(code, name)
        if extent is None:
            continue
        origin, length, decl_line = extent
        base_key, size_key = window
        base = DEVICE_MEM[base_key]
        if size_key is None:
            if origin != base:
                findings.append(
                    Finding(
                        path,
                        decl_line,
                        "LD010",
                        f"region '{name}' starts at 0x{origin:08X}, but {base_key} "
                        f"(libs/ra8_core/inc/ra8_device.h) puts that window at "
                        f"0x{base:08X}",
                    )
                )
            continue
        size = DEVICE_MEM[size_key]
        end = base + size
        if not (base <= origin < end):
            findings.append(
                Finding(
                    path,
                    decl_line,
                    "LD010",
                    f"region '{name}' starts at 0x{origin:08X}, outside the "
                    f"0x{base:08X}..0x{end:08X} window {base_key} / {size_key} "
                    f"declare (libs/ra8_core/inc/ra8_device.h)",
                )
            )
            continue
        if base != SRAM_WINDOW_BASE and origin + length > end:
            findings.append(
                Finding(
                    path,
                    decl_line,
                    "LD010",
                    f"region '{name}' spans 0x{origin:08X}..0x{origin + length:08X}, "
                    f"{origin + length - end} bytes past the 0x{end:08X} end of the "
                    f"window {base_key} / {size_key} declare "
                    f"(libs/ra8_core/inc/ra8_device.h)",
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
    LD002 ENTRY, LD003 MEMORY, LD004 region closure, LD009 SRAM fit, LD010
    named-region agreement with ra8_device.h, LD007 option-setting addresses,
    LD008 option-setting completeness.
    """
    findings, text = _check_formatting(path, raw)
    findings += _check_licence(path, text.splitlines())
    code = strip_comments(text)
    findings += _check_entry(path, code)
    memory_findings, regions = _check_memory(path, code)
    findings += memory_findings
    findings += _check_region_closure(path, code, regions)
    findings += _check_sram_fit(path, code, regions)
    findings += _check_device_region_fit(path, code, regions)
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


def selftest() -> int:
    """Run linker-script fixtures without inheriting the caller's repository.

    The assertions live in ``linker_script_selftests``, imported HERE rather
    than at module scope: that module imports this one, so a top-level import
    would be a cycle, and the plain scan would pay to build fixtures it never
    runs.
    """
    from linker_script_selftests import run_selftests

    with isolated_git_environment():
        return run_selftests()


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
