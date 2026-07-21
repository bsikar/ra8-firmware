#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Ban silent error discards at TrustZone boot boundaries.

C23 makes an explicit ``(void)`` cast the sanctioned suppression for a
``[[nodiscard]]`` diagnostic, so ``-Wall -Wextra -Werror`` can never flag

  (void)ra8_cgc_init();   /* discarded right before BLXNS */

even though the callee is ``[[nodiscard]] ra8_err_t``.  That exact pattern
shipped a Secure boot that would BLXNS into the Non-secure image on a dead
clock tree (tracker issue #191, T2-05), and it recurred after being fixed
once because per-app boot files are copied from siblings.  The compiler is
structurally unable to police the void-cast; this gate is the backstop.

Rules (first-party C/C++ only):

  A. The TrustZone world-switch API family is must-handle EVERYWHERE: a
     ``(void)``-cast discard of any ``ra8_tz_secure_boot_*()`` call is
     banned in every first-party file.  A returned error from that family
     is a root-of-trust DENIAL or a rejected NS vector table -- ignoring
     it means treating an unauthenticated NS world as live.

  B. Inside a boot translation unit -- any ``.c`` that defines
     ``SystemInit`` or ``ra8_trustzone_init`` -- a ``(void)``-cast discard
     of ANY ``ra8_*()`` call is banned.  Every fallible step in those TUs
     gates the S->NS world switch; a tolerated failure must be an explicit
     ``if`` (halt, or a documented S-side fallback) or
     ``RA8_ERROR_CHECK`` / ``RA8_ERROR_CHECK_NO_ABORT``, never a silent
     cast.

Scope:
  C / C++ sources (.c .h .cpp .hpp) under libs/, src/, tests/, examples/,
  port/, tools/.  Vendored SOUP (libs/third_party/*) and generated font
  data (libs/fonts/*) are exempt, as are build trees.

Exemptions:
  * Comment lines / prose (a match whose cast sits inside a // or /*
    comment does not count).
  * Function-pointer discards like ``(void)ra8_trustzone_init;`` (no call
    parentheses) -- those silence -Wunused, they do not discard an error.
  * Any line carrying `TZ-DISCARD-OK: <reason>` (reason text required).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

ROOTS = ("libs", "src", "tests", "examples", "port", "tools")
EXTS = (".c", ".h", ".cpp", ".hpp")
EXEMPT_DIRS = ("/third_party/", "/fonts/")

# Rule A: the world-switch family, discarded anywhere.
FAMILY_RE = re.compile(r"\(\s*void\s*\)\s*(ra8_tz_secure_boot_[a-z0-9_]+)\s*\(")
# Rule B: any ra8_ call, discarded inside a boot TU.
ANY_RA8_RE = re.compile(r"\(\s*void\s*\)\s*(ra8_[a-z0-9_]+)\s*\(")
# Boot-TU marker: a definition (or same-file declaration) of one of the two
# canonical boot entry points every app's boot files provide.
BOOT_TU_RE = re.compile(
    r"^\s*void\s+(?:SystemInit|ra8_trustzone_init)\s*\(\s*void\s*\)", re.MULTILINE
)
WAIVER_RE = re.compile(r"TZ-DISCARD-OK:\s*\S")


def _is_comment_pos(line: str, pos: int) -> bool:
    """True if column `pos` sits inside a // or /* comment on this line."""
    stripped = line.lstrip()
    if stripped.startswith(("*", "//", "/*")):
        return True
    before = line[:pos]
    if "//" in before:
        return True
    # crude single-line /* ... */ check
    return "/*" in before and "*/" not in before


def discover() -> list[str]:
    """Every boot-boundary source file, for the whole-tree sweep."""
    out: list[str] = []
    for root in ROOTS:
        base = Path(root)
        if not base.is_dir():
            continue
        for ext in EXTS:
            for f in base.rglob(f"*{ext}"):
                s = str(f)
                if is_build_output_path(s) or any(d in s for d in EXEMPT_DIRS):
                    continue
                out.append(s)
    return out


def check_file(path: str) -> list[tuple[int, str, str]]:
    """Return (line, rule, snippet) findings for one file."""
    findings: list[tuple[int, str, str]] = []
    try:
        text = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return findings
    if "(void)" not in text.replace(" ", ""):
        return findings
    boot_tu = path.endswith(".c") and bool(BOOT_TU_RE.search(text))
    rules: list[tuple[re.Pattern[str], str]] = [(FAMILY_RE, "A")]
    if boot_tu:
        rules.append((ANY_RA8_RE, "B"))
    lines = text.splitlines()
    for i, raw in enumerate(lines, 1):
        # A discard split across lines: `(void)` alone, call on the next line.
        line = raw
        if re.search(r"\(\s*void\s*\)\s*$", raw) and i < len(lines):
            line = raw + lines[i]
        if WAIVER_RE.search(line):
            continue
        seen: set[tuple[int, int]] = set()
        for rule_re, rule in rules:
            for m in rule_re.finditer(line):
                span = (m.start(), m.end())
                if span in seen or _is_comment_pos(line, m.start()):
                    continue
                seen.add(span)
                findings.append((i, rule, raw.strip()[:100]))
    return findings


def main() -> int:
    """Fail when a TrustZone boot-boundary call discards its ra8_err_t.

    These call sites are the worst place in the tree to drop a status: a
    failed SAU or MPU configuration returns an error and then, ignored, hands
    control to the non-secure world with the boundary not actually in force.
    The build links, the board boots, and the isolation is simply absent.

    Returns 1 listing each discard, 0 when clean.
    """
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    files = args or discover()
    total = 0
    for path in sorted(set(files)):
        if (
            not path.endswith(EXTS)
            or is_build_output_path(path)
            or any(d in path for d in EXEMPT_DIRS)
        ):
            continue
        for ln, rule, snippet in check_file(path):
            what = (
                "world-switch result discarded" if rule == "A" else "boot-TU ra8_* result discarded"
            )
            print(
                f"{path}:{ln}: [rule {rule}] {what} -- handle the ra8_err_t "
                f"(halt or a documented fallback; RA8_ERROR_CHECK[_NO_ABORT]); "
                f"never (void)-cast it at a TrustZone boot boundary; {snippet}"
            )
            total += 1
    if total:
        print(
            f"\ncheck_tz_boundary_discard: {total} violation(s). A (void)-cast "
            f"silences [[nodiscard]] by ISO C23 rule, so -Werror cannot catch "
            f"these; check the result and fail safe instead (add "
            f"`TZ-DISCARD-OK: <reason>` only for a justified exception)."
        )
        return 1
    print(
        "check_tz_boundary_discard: clean -- no silent ra8_err_t discards at "
        "TrustZone boot boundaries."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
