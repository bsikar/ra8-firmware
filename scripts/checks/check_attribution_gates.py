#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Hold every write to a PRC4-gated security-attribution register inside a PRCR window.

WHY THIS EXISTS
===============
The RA8D2 puts its TrustZone security-attribution registers behind PRC4 of the
PRCR write-protect register (HUM Ch 13.1 Table 13.1, p 520-521).  A store to
one of them while PRC4 is locked is **discarded silently**: no bus fault, no
status flag, no error return.  The attribution simply stays at its reset value
and the calling code reports success.

That exact defect has now been found three times in this tree:

  * RA8FW-254  -- ``ra8_bkup_security_apply`` wrote BBFSAR / VBRSABAR / VBRPABARS /
             VBRPABARNS ungated.
  * #759c -- ``ra8_tz_partition_apply`` wrote SRAMSABARn ungated, via
             ``ra8_sram_set_boundary``.
  * #759d -- ``internal_apply_security`` wrote SRAMSAR / SRAMESAR / SRAMSABARn
             ungated.

Each was fixed by hand, each time by someone who happened to read the register
table.  Nothing stopped the fourth one being written tomorrow, which is
the dual-image scaffold's item (a): the register block had no guard that
every attribution writer goes
through the protection helper.  This checker is that guard.

WHAT IT ENFORCES, PRECISELY
---------------------------
  * In ``libs/``, any assignment whose target names a PRC4-gated attribution
    register (see ``MEMBER_REGISTERS`` / ``ACCESSOR_REGISTERS``) must sit lexically inside an
    ``RA8_PROTECTED_WRITE(...)`` block.
  * Reads are not judged.  Only a store can be silently dropped; a read of a
    locked register returns its real contents.
  * The unlock value is not judged.  Which ``k_ra8_prcr_unlock_*`` group a
    given register needs is a register-table question this checker does not
    re-litigate; being inside *some* window is what it measures.

THE DOCUMENTED LEAVES
---------------------
``libs/ra8_hal/src/ra8_sram_security.c`` holds three public setters --
``ra8_sram_set_security``, ``ra8_sram_set_ecc_security``,
``ra8_sram_set_boundary`` -- that write one register each and document, in
``ra8_sram.h``, a ``@pre`` that the caller already holds PRC4 open.  They are
deliberately gate-free so a caller programming several banks pays for one
window rather than four, and they are listed in ``ALLOWED_LEAVES`` by file and
function name.  Adding a name to that list is a deliberate act: it moves the
obligation onto the leaf's caller, and the leaf's own ``@pre`` has to say so.

SCOPE, HONESTLY
---------------
This is lexical, not a dataflow analysis.  It sees the window a write is
written inside, not the window that is open when the write executes:

  * A helper function called from inside a window passes only if it is an
    allowed leaf; the checker cannot see the caller's window.  That is the
    conservative direction -- it asks for a note, not for a silent pass.
  * Conversely a write inside a window that some macro re-locks early would
    still pass.  Nothing in the tree does that, and the helper itself re-locks
    only on scope exit.

ZIG SOURCES (RA8FW-546)
-----------------------
``.zig`` files under ``libs/`` are held to the same rule.  The Zig window is
``prcr.open(...)`` followed by a ``defer`` that closes it, the counterpart of the
macro's scope-exit relock: every line from the ``prcr.open(`` call to the end
of its enclosing block counts as gated.  Flagged Zig stores are
``x.SRAMSAR = v`` / ``x.SRAMSABAR[i] = v`` (an identifier, ``]`` or ``)`` must
precede the dot, so a ``.{ .BUSSAR = v }`` struct literal is not judged) and
``ra8_bkup_bbfsar().* = v``.  Leaves are named by ``(path, fn)`` exactly as for
C.  A store routed through an MMIO helper call is not seen, the same lexical
limit the C side has.

The five hand-rolled ``trustzone_init.c`` app forks are **out of scope**: they
write PRCR_S with raw hex literals rather than the helper, and reconciling them
against the library path is the dual-image scaffold's item (b).  Scoping this checker to ``libs/``
is what lets it pass today instead of failing open on work that has not
happened yet.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SEARCH_ROOTS = ("libs",)

#: (path relative to the repo root, function name) pairs whose writes are
#: deliberately ungated because the function documents a ``@pre`` that its
#: caller holds the PRC4 window open.
ALLOWED_LEAVES = frozenset(
    {
        ("libs/ra8_hal/src/ra8_sram_security.c", "ra8_sram_set_security"),
        ("libs/ra8_hal/src/ra8_sram_security.c", "ra8_sram_set_ecc_security"),
        ("libs/ra8_hal/src/ra8_sram_security.c", "ra8_sram_set_boundary"),
    }
)

#: Registers reached as a member of the CPSCU / bus register block.
MEMBER_REGISTERS = ("SRAMSAR", "SRAMESAR", "SRAMSABAR", "BUSSAR")

#: Registers reached through a ``ra8_*_<name>()`` address accessor.
ACCESSOR_REGISTERS = ("bbfsar", "vbrsabar", "vbrpabars", "vbrpabarns")

_ASSIGN = r"(?:\|=|&=|\^=|=)(?!=)"

#: ``cpscu->SRAMSAR = x`` and ``cpscu->SRAMSABAR[bank] = x``.  A struct field
#: that merely shares a name is not matched: the arrow and the register's own
#: upper-case spelling are both required.
MEMBER_WRITE_RE = re.compile(
    r"->\s*(" + "|".join(MEMBER_REGISTERS) + r")\s*(?:\[[^\]]*\])?\s*" + _ASSIGN
)

#: ``*ra8_bkup_bbfsar() = x``.  The call parentheses are required, which is
#: what separates the accessor write from ``cfg->bbfsar = *ra8_bkup_bbfsar()``,
#: a read into a same-named config field.
ACCESSOR_WRITE_RE = re.compile(
    r"\*\s*[A-Za-z_][A-Za-z0-9_]*(" + "|".join(ACCESSOR_REGISTERS) + r")\s*\(\s*\)\s*" + _ASSIGN
)

PROTECTED_RE = re.compile(r"\bRA8_PROTECTED_WRITE\s*\(")

#: Zig ``cpscu.SRAMSAR = x``; the dot follows a name, ``]`` or ``)``.
ZIG_MEMBER_WRITE_RE = re.compile(
    r"[A-Za-z0-9_\])]\s*\.\s*(" + "|".join(MEMBER_REGISTERS) + r")\s*(?:\[[^\]]*\])?\s*" + _ASSIGN
)

#: Zig ``ra8_bkup_bbfsar().* = x``.
ZIG_ACCESSOR_WRITE_RE = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*(" + "|".join(ACCESSOR_REGISTERS) + r")\s*\(\s*\)\s*\.\*\s*" + _ASSIGN
)

#: Zig window opener: ``prcr.open(...)``, closed by a ``defer`` at scope exit.
ZIG_PROTECTED_RE = re.compile(r"\bprcr\s*\.\s*open\s*\(")

#: Zig function definition, nested or not: ``fn name(``.
ZIG_FN_RE = re.compile(r"\bfn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")

#: Generated trees that are never first-party source.
SKIPPED_DIRS = frozenset({".zig-cache", "zig-cache", "zig-out"})

#: Attribute sequences that may precede a definition's return type.
ATTRIBUTE_RE = re.compile(r"\[\[[^\]]*\]\]")

#: Control-flow words that look like a call but never name a function.
NOT_A_DEFINITION = frozenset({"if", "for", "while", "switch", "return", "sizeof", "do", "else"})

CALL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def definition_name(line: str) -> str | None:
    """Return the function name a column-0 definition line opens, if any."""
    stripped = ATTRIBUTE_RE.sub(" ", line)
    for found in CALL_RE.finditer(stripped):
        name = found.group(1)
        if name in NOT_A_DEFINITION:
            return None
        return name
    return None


def strip_comments(text: str) -> str:
    """Blank out comment and string bodies, preserving line structure."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif ch == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i))
            i = j
        elif ch in "\"'":
            quote = ch
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def protected_lines(text: str) -> set[int]:
    """Return the 1-based line numbers lexically inside an RA8_PROTECTED_WRITE body."""
    inside: set[int] = set()
    for match in PROTECTED_RE.finditer(text):
        # Step over the macro's own argument list.
        i = match.end() - 1
        depth = 0
        while i < len(text):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        # Then find the body brace that follows it.
        brace = text.find("{", i)
        if brace == -1:
            continue
        depth = 0
        j = brace
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        first = text.count("\n", 0, brace) + 1
        last = text.count("\n", 0, min(j, len(text) - 1)) + 1
        inside.update(range(first, last + 1))
    return inside


def enclosing_functions(text: str) -> dict[int, str]:
    """Map each 1-based line number to the name of the function it sits in."""
    lines = text.split("\n")
    owner: dict[int, str] = {}
    depth = 0
    pending: str | None = None
    current: str | None = None
    for idx, line in enumerate(lines, start=1):
        if depth == 0 and line and not line[0].isspace():
            found = definition_name(line)
            if found is not None:
                pending = found
        opens = line.count("{")
        closes = line.count("}")
        if depth == 0 and opens > 0 and pending is not None:
            current = pending
            pending = None
        depth += opens - closes
        if depth <= 0:
            depth = 0
            current = None
        owner[idx] = current or ""
    return owner


def zig_protected_lines(text: str) -> set[int]:
    """Lines from each ``prcr.open(`` to the end of its enclosing Zig block."""
    inside: set[int] = set()
    for match in ZIG_PROTECTED_RE.finditer(text):
        depth = 0
        j = match.start()
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth < 0:
                    break
            j += 1
        first = text.count("\n", 0, match.start()) + 1
        last = text.count("\n", 0, min(j, len(text) - 1)) + 1
        inside.update(range(first, last + 1))
    return inside


def zig_enclosing_functions(text: str) -> dict[int, str]:
    """Map each 1-based line to the innermost Zig ``fn`` whose body holds it."""
    owner: dict[int, str] = {}
    stack: list[tuple[str, int]] = []
    depth = 0
    pending: str | None = None
    for idx, line in enumerate(text.split("\n"), start=1):
        found = ZIG_FN_RE.search(line)
        if found is not None:
            pending = found.group(1)
        for ch in line:
            if ch == "{":
                depth += 1
                if pending is not None:
                    stack.append((pending, depth))
                    pending = None
            elif ch == "}":
                if stack and stack[-1][1] == depth:
                    stack.pop()
                depth = max(depth - 1, 0)
        if pending is not None and line.rstrip().endswith(";"):
            pending = None
        owner[idx] = stack[-1][0] if stack else ""
    return owner


def audit_text(text: str, zig: bool = False) -> list[tuple[int, str, str]]:
    """Return (line, register, enclosing function) for every ungated write."""
    code = strip_comments(text)
    if zig:
        gated = zig_protected_lines(code)
        owners = zig_enclosing_functions(code)
        patterns = (ZIG_MEMBER_WRITE_RE, ZIG_ACCESSOR_WRITE_RE)
    else:
        gated = protected_lines(code)
        owners = enclosing_functions(code)
        patterns = (MEMBER_WRITE_RE, ACCESSOR_WRITE_RE)
    findings: list[tuple[int, str, str]] = []
    for idx, line in enumerate(code.split("\n"), start=1):
        if idx in gated:
            continue
        match = patterns[0].search(line) or patterns[1].search(line)
        if match:
            findings.append((idx, match.group(1), owners.get(idx, "")))
    return findings


def sources() -> list[Path]:
    """Every C and Zig source under the search roots, sorted for stable output."""
    found: list[Path] = []
    for root in SEARCH_ROOTS:
        for pattern in ("*.c", "*.zig"):
            found.extend(
                path
                for path in (REPO_ROOT / root).rglob(pattern)
                if not SKIPPED_DIRS.intersection(path.parts)
            )
    return sorted(found)


def selftest() -> int:
    """Prove the checker catches an ungated write and accepts a gated one."""
    ungated = """
ra8_err_t apply(void)
{
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  cpscu->SRAMSAR = sar;
  return k_ra8_ok;
}
"""
    gated = """
ra8_err_t apply(void)
{
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_sar)
  {
    cpscu->SRAMSAR = sar;
    for (uint8_t b = 0U; b < 4U; ++b) {
      cpscu->SRAMSABAR[b] = off[b];
    }
  }
  return k_ra8_ok;
}
"""
    read_only = """
void get(ra8_bkup_security_config_t* cfg)
{
  cfg->bbfsar = *ra8_bkup_bbfsar();
}
"""
    commented = """
void apply(void)
{
  /* cpscu->SRAMSAR = sar; is what the old code did */
  RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_sar) { cpscu->SRAMSAR = sar; }
}
"""
    accessor = """
void apply(void)
{
  *ra8_bkup_vbrsabar() = cfg->saba;
}
"""
    zig_ungated = """
pub fn apply(cpscu: *volatile Cpscu, sar: u32) void {
    cpscu.SRAMSAR = sar;
}
"""
    zig_gated = """
pub fn apply(cpscu: *volatile Cpscu, off: [4]u32) void {
    const window = prcr.open(.sar);
    defer window.close();
    for (off, 0..) |o, b| {
        cpscu.SRAMSABAR[b] = o;
    }
}
fn after(cpscu: *volatile Cpscu) void {
    cpscu.BUSSAR = 0;
}
"""
    zig_misc = """
const cfg: Cpscu = .{ .BUSSAR = 0, .SRAMSAR = 1 };
fn get(cfg: *Cfg) void {
    // cpscu.SRAMSAR = 0; was the old code
    cfg.bbfsar = ra8_bkup_bbfsar().*;
    const ok = cpscu.SRAMSAR == 0;
    _ = ok;
}
"""
    zig_accessor = """
const Unit = struct {
    pub fn apply(v: u32) void {
        ra8_bkup_vbrsabar().* = v;
    }
};
"""
    cases = [
        (
            "a Zig ungated write is caught",
            [f[1:] for f in audit_text(zig_ungated, zig=True)] == [("SRAMSAR", "apply")],
        ),
        (
            "a Zig write ends its window at the block close",
            [f[1:] for f in audit_text(zig_gated, zig=True)] == [("BUSSAR", "after")],
        ),
        (
            "a Zig literal, read, compare or comment is not judged",
            audit_text(zig_misc, zig=True) == [],
        ),
        (
            "the Zig accessor form is caught in a nested fn",
            [f[1:] for f in audit_text(zig_accessor, zig=True)] == [("vbrsabar", "apply")],
        ),
        ("an ungated write is caught", [f[1] for f in audit_text(ungated)] == ["SRAMSAR"]),
        ("a gated write passes", audit_text(gated) == []),
        ("a read is not judged", audit_text(read_only) == []),
        ("a commented-out write is not judged", audit_text(commented) == []),
        ("the accessor form is caught", [f[1] for f in audit_text(accessor)] == ["vbrsabar"]),
        (
            "the enclosing function is reported",
            [f[2] for f in audit_text(ungated)] == ["apply"],
        ),
    ]
    bad = [name for name, ok in cases if not ok]
    for name, ok in cases:
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}")
    if bad:
        print(f"{Path(__file__).name}: selftest FAILED")
        return 1
    print(f"{Path(__file__).name}: selftest passed ({len(cases)} case(s))")
    return 0


def main() -> int:
    """Run the attribution-gate sweep, or the checker's own selftest."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="run the checker's own test cases")
    args = parser.parse_args()
    if args.selftest:
        return selftest()

    scanned = 0
    allowed = 0
    failures: list[tuple[str, int, str, str]] = []
    for path in sources():
        rel = path.relative_to(REPO_ROOT).as_posix()
        scanned += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        for line, register, func in audit_text(text, zig=path.suffix == ".zig"):
            if (rel, func) in ALLOWED_LEAVES:
                allowed += 1
                continue
            failures.append((rel, line, register, func))

    name = Path(__file__).name
    if failures:
        print(f"\n{name}: security-attribution write(s) outside a PRCR window\n")
        for rel, line, register, func in failures:
            where = func or "file scope"
            print(f"  {rel}:{line}: {register} written in {where}")
        print(
            "\nThese registers sit behind PRCR PRC4 (HUM Ch 13.1 Table 13.1,\n"
            "p 520-521). Stored to with PRC4 locked they are discarded silently:\n"
            "no fault, no flag, and the attribution keeps its reset value while\n"
            "the caller reports success. That is RA8FW-254. Wrap the write:\n"
            "\n    RA8_PROTECTED_WRITE(k_ra8_prcr_unlock_sar) { ... }\n"
            "\nor in Zig: const window = prcr.open(...); defer window.close();\n"
            "\nIf the function is deliberately a gate-free leaf whose caller holds\n"
            "the window, document that as a @pre and add it to ALLOWED_LEAVES in\n"
            "this checker, so the obligation is recorded rather than lost.\n"
        )
        return 1

    print(
        f"{name}: {scanned} source(s) scanned, every attribution write gated "
        f"({allowed} documented leaf write(s) allowed)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
