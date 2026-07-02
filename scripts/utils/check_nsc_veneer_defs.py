#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every RA_NSC_VENEER declared in ra_nsc.h shall have a definition.

An ``RA_NSC_VENEER`` (a ``cmse_nonsecure_entry`` Secure-Gateway veneer) is the
ONLY path a Non-Secure caller has into the Secure world.  A declaration in the
public header ``libs/ra_nsc/inc/ra_nsc.h`` with no matching definition under
``libs/ra_nsc/src/`` advertises an NS->S entry point that does not exist: it
misleads Non-Secure integrators about the available trust-boundary surface, and
because a veneer with no caller also has no link error, such a phantom decl can
sit in the header indefinitely (this is how ``ra_nsc_key_import`` and
``ra_nsc_trng_read`` accumulated -- see security-trustzone-audit F16/A4).

This gate parses every ``RA_NSC_VENEER`` function declaration out of the header
and fails if any lacks a definition in the ``ra_nsc`` sources.  There is no
allowlist: either implement the veneer or delete the declaration.

Run::

    check_nsc_veneer_defs.py

Exit status: 0 if every declared veneer has a definition, 1 otherwise.
"""

import re
import sys
from pathlib import Path

# Repo root: this file is scripts/utils/check_nsc_veneer_defs.py .
REPO_ROOT = Path(__file__).resolve().parents[2]
HEADER = REPO_ROOT / "libs" / "ra_nsc" / "inc" / "ra_nsc.h"
SRC_DIR = REPO_ROOT / "libs" / "ra_nsc" / "src"

# A declaration:  [[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_foo(...
# We only need the function name that follows the RA_NSC_VENEER return type.
DECL_RE = re.compile(r"RA_NSC_VENEER\s+\w[\w\s\*]*?\b(ra_nsc_\w+)\s*\(")


def declared_veneers(header: Path) -> list[str]:
    """Return the veneer function names declared in the header, in order."""
    text = header.read_text(encoding="utf-8")
    seen: list[str] = []
    for name in DECL_RE.findall(text):
        if name not in seen:
            seen.append(name)
    return seen


def is_defined(name: str, sources: list[Path]) -> bool:
    """Whether a definition of ``name`` exists in any ra_nsc source file.

    A definition in a .c is the function signature followed (eventually) by a
    body; a bare declaration lives only in the header, so any ``name(`` at
    statement start in a source file is a definition.
    """
    # `<type> name(`  where the line is not a call (calls are indented / have a
    # preceding token like `= ` or `return `).  Requiring `ra_err_t` before the
    # name matches every veneer's definition and never a call site.
    def_re = re.compile(r"\bra_err_t\s+" + re.escape(name) + r"\s*\(")
    return any(def_re.search(src.read_text(encoding="utf-8")) for src in sources)


def main() -> int:
    if not HEADER.is_file():
        print(f"check_nsc_veneer_defs.py: header not found: {HEADER}", file=sys.stderr)
        return 1
    sources = sorted(SRC_DIR.glob("*.c"))
    veneers = declared_veneers(HEADER)
    missing = [name for name in veneers if not is_defined(name, sources)]

    hdr = HEADER.relative_to(REPO_ROOT)
    src = SRC_DIR.relative_to(REPO_ROOT)
    if missing:
        print("check_nsc_veneer_defs.py: RA_NSC_VENEER declared without a definition:")
        for name in missing:
            print(f"  {name}: declared in {hdr}, no definition in {src}/")
        print("Fix each at the root -- implement the veneer, or delete the declaration.")
        print("A phantom NS->S entry point in the public header is a trust hazard.")
        return 1

    count = len(veneers)
    print(f"check_nsc_veneer_defs.py: PASS -- all {count} RA_NSC_VENEER declaration(s) defined.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
