#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a HAL driver shall not guard bare CPU asm on RA8_SIMULATOR_MODE.

Issue #293 (following #238) migrated every host-compatibility CPU primitive out
of the HAL peripheral drivers and onto ONE shared seam:

  - ``libs/ra8_hal/inc/ra8_hw_intrinsics.h``  (target inline asm / host decls)
  - ``tests/mocks/ra8_host_asm_stub.c``       (the host-safe definitions)

A driver that needs ``wfi`` / ``dsb`` / ``isb`` / ``nop`` / the ``cpsie i`` /
``cpsid i`` gate / the post-reset spin now calls ``ra8_hw_wfi()`` and friends;
it carries NO ``#ifdef RA8_SIMULATOR_MODE`` of its own -- the seam owns the
host/target divergence. That keeps coverage and MC/DC landing on the real
shipping path instead of a compiled-out detour, exactly as #238 intended.

This gate keeps a NEW guard class from creeping back in. It FAILS if any
translation unit under ``libs/ra8_hal/src/`` contains an inline-asm statement
(``__asm`` / ``__asm__``) inside a preprocessor conditional whose controlling
expression references ``RA8_SIMULATOR_MODE`` -- in EITHER branch. Comment text
is stripped first, so prose that merely mentions ``__asm__`` never trips it.

Scope note: this is deliberately limited to ``libs/ra8_hal/src/`` -- the HAL
peripheral drivers. Boot code (``system_init.c``, ``vector_table.c``,
``trustzone_init.c``), the core runtime (``ra8_core``), and the TrustZone /
secure-boot flow legitimately host target-only asm (reset vectors, fault
handlers, SAU bring-up, ``msr msp_ns``) that has no host code path to drive and
is not a peripheral-driver short-circuit; those are out of scope by design.

There is no allowlist: route the primitive through ``ra8_hw_intrinsics.h`` (add
a new one there and to the host stub if it is genuinely missing), never behind a
fresh in-driver ``#ifdef RA8_SIMULATOR_MODE``.

Run::

    check_no_driver_asm_guard.py

Exit status: 0 if every driver is clean, 1 otherwise.
"""

import re
import sys
from pathlib import Path

# Repo root: this file is scripts/checks/check_no_driver_asm_guard.py .
REPO_ROOT = Path(__file__).resolve().parents[2]

# The HAL peripheral drivers -- the only scope this gate governs.
DRIVER_DIR = REPO_ROOT / "libs" / "ra8_hal" / "src"

_RE_IF = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b(.*)$")
_RE_ELIF = re.compile(r"^\s*#\s*elif\b(.*)$")
_RE_ENDIF = re.compile(r"^\s*#\s*endif\b")
_RE_ASM = re.compile(r"(?<![A-Za-z0-9_])__asm(__)?(?![A-Za-z0-9_])")
_SIM = "RA8_SIMULATOR_MODE"


def strip_comments(text: str) -> list[str]:
    """Blank out C ``/* */`` and ``//`` comments, preserving line count.

    Preprocessor directives never live inside comments, so a line-preserving
    strip lets the conditional walk and the asm scan share one clean view
    without a false positive from a doc block that mentions ``__asm__``.
    """
    out: list[str] = []
    in_block = False
    for line in text.splitlines():
        buf: list[str] = []
        i = 0
        n = len(line)
        while i < n:
            two = line[i : i + 2]
            if in_block:
                if two == "*/":
                    in_block = False
                    i += 2
                else:
                    i += 1
                continue
            if two == "/*":
                in_block = True
                i += 2
                continue
            if two == "//":
                break
            buf.append(line[i])
            i += 1
        out.append("".join(buf))
    return out


def check_file(path: Path) -> list[str]:
    """Return violation strings for one driver TU (empty when clean)."""
    rel = path.relative_to(REPO_ROOT).as_posix()
    lines = strip_comments(path.read_text(encoding="utf-8"))

    # Stack of booleans: does this open conditional's region reference the
    # simulator flag (in its #if / any #elif)? A True anywhere on the stack
    # means the current line compiles under a sim-conditioned region.
    stack: list[bool] = []
    problems: list[str] = []

    for idx, line in enumerate(lines, start=1):
        m_if = _RE_IF.match(line)
        if m_if is not None:
            stack.append(_SIM in m_if.group(2))
            continue
        m_elif = _RE_ELIF.match(line)
        if m_elif is not None:
            if stack:
                stack[-1] = stack[-1] or (_SIM in m_elif.group(1))
            continue
        if _RE_ENDIF.match(line):
            if stack:
                stack.pop()
            continue
        # #else keeps the frame's sim-reference flag: both branches of a
        # `#ifdef RA8_SIMULATOR_MODE` are sim-conditioned regions.
        if _RE_ASM.search(line) and any(stack):
            problems.append(
                f"{rel}:{idx}: inline asm '{line.strip()}' sits inside a "
                f"{_SIM} conditional -- route it through "
                f"libs/ra8_hal/inc/ra8_hw_intrinsics.h instead"
            )
    return problems


def main() -> int:
    """Fail any HAL driver that guards bare CPU asm on RA8_SIMULATOR_MODE.

    A missing driver directory exits 1 rather than 0. That is deliberate: this
    gate has a single hardcoded scan root, so the directory vanishing means
    the tree moved under it, and the one thing it must not do is report a
    clean sweep of somewhere that does not exist.

    Returns 0 when every driver routes its CPU primitives through
    ra8_hw_intrinsics.h, 1 on a violation or a missing driver directory.
    """
    if not DRIVER_DIR.is_dir():
        print(f"check_no_driver_asm_guard.py: driver dir not found: {DRIVER_DIR}")
        return 1

    drivers = sorted(DRIVER_DIR.glob("*.c"))
    all_problems: list[str] = []
    for path in drivers:
        all_problems.extend(check_file(path))

    if all_problems:
        print("check_no_driver_asm_guard.py: a HAL driver guards bare asm on RA8_SIMULATOR_MODE:")
        for p in all_problems:
            print(f"  {p}")
        print("Fix at the root -- call the ra8_hw_* primitive from")
        print("  libs/ra8_hal/inc/ra8_hw_intrinsics.h")
        print("(add a new one there plus its host body in")
        print(" tests/mocks/ra8_host_asm_stub.c if it does not exist yet).")
        return 1

    print(
        f"check_no_driver_asm_guard.py: PASS -- {len(drivers)} HAL driver TU(s) "
        f"carry no RA8_SIMULATOR_MODE-guarded asm."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
