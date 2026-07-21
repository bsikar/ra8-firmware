#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every insecure placeholder-crypto body shall be guarded fail-closed.

Several secure-side translation units ship an INSECURE placeholder body that is
only safe under host simulation or an explicitly-declared insecure dev/eval
image (issue #180):

  - src/secure_app/secure_trng.c        deterministic xorshift PRNG as a "TRNG"
  - src/secure_app/key_vault.c          plain secure-SRAM key store (no HW vault)
  - libs/ra8_hal/src/ra8_rsip_key_injection.c  non-cryptographic xorshift key-wrap
  - libs/ra8_hal/src/ra8_rsip_ecc.c       fiction-opcode ECDSA / ECDH / Ed25519 asym
  - libs/ra8_hal/src/ra8_rsip_cipher.c    fiction-opcode AES / ChaCha / key-install
  - libs/ra8_hal/src/ra8_rsip_rsa.c       fiction-opcode RSA sign / verify / enc / dec
  - libs/ra8_hal/src/ra8_rsip_asym.c      fiction-opcode hash / HMAC / key vault / wrap / KDF
  - libs/ra8_hal/src/ra8_rsip_devsec.c    fiction-register lifecycle / debug / tamper / DPA

Each such body MUST be wrapped in the guard::

    #if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)
        <insecure placeholder body>
    #else
        <fail-closed: every entry point returns a hard error, never k_ra8_ok>
    #endif

so a real production/HIL image that sets NEITHER flag compiles the fail-closed
#else and cannot silently ship the stub. This gate is the compile-time-of-CI
guarantee the audit asked for: it FAILS if, for any listed TU,

  1. the guard `#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)`
     is absent, or has no matching #else / #endif;
  2. the #else branch is not fail-closed (no `#error` and no `k_ra8_err_` return);
  3. the TU's insecure signature token escapes the guarded region (appears before
     the guard, in the #else, or after the #endif) -- i.e. an insecure body that
     is not actually behind the guard.

There is no allowlist: either guard the insecure body fail-closed, or delete it
in favour of a real backend.

Run::

    check_stub_crypto_guarded.py

Exit status: 0 if every stub TU is guarded fail-closed, 1 otherwise.
"""

import re
import sys
from pathlib import Path

# Repo root: this file is scripts/checks/check_stub_crypto_guarded.py .
REPO_ROOT = Path(__file__).resolve().parents[2]

# Each stub TU -> a distinctive token that appears ONLY in its insecure body
# (never in the fail-closed #else nor in surrounding prose). The token must live
# inside the guarded #if region; if it escapes, the insecure body is unguarded.
STUB_TUS = {
    "src/secure_app/secure_trng.c": "internal_xorshift64",
    "src/secure_app/key_vault.c": "s_vault",
    "libs/ra8_hal/src/ra8_rsip_key_injection.c": "ki_compute_mac",
    "libs/ra8_hal/src/ra8_rsip_ecc.c": "k_ra8_rsip_asym_op_eddsa_sign",
    "libs/ra8_hal/src/ra8_rsip_cipher.c": "internal_sym_run",
    "libs/ra8_hal/src/ra8_rsip_rsa.c": "internal_rsa_dispatch",
    "libs/ra8_hal/src/ra8_rsip_asym.c": "internal_hash_pull_digest",
    "libs/ra8_hal/src/ra8_rsip_devsec.c": "k_ra8_rsip_off_life_state",
}

_RE_IF = re.compile(r"^\s*#\s*if(n?def)?\b")
_RE_ELSE = re.compile(r"^\s*#\s*else\b")
_RE_ENDIF = re.compile(r"^\s*#\s*endif\b")
_RE_ERROR = re.compile(r"^\s*#\s*error\b")


def is_guard_open(line: str) -> bool:
    """Whether ``line`` is the stub-crypto guard opener (either flag order)."""
    return bool(
        re.match(r"^\s*#\s*if\b", line)
        and "RA8_INSECURE_STUB_CRYPTO" in line
        and "RA8_SIMULATOR_MODE" in line
    )


def find_guard_region(lines: list[str]) -> tuple[int, int, int] | None:
    """Locate the guard's (#if, #else, #endif) 0-based line indices.

    Returns ``(if_idx, else_idx, endif_idx)`` or ``None`` when the guard opener
    is absent or its matching #else / #endif cannot be resolved. Nesting of
    inner #if/#endif is tracked so a nested conditional does not confuse the
    match.
    """
    if_idx = next((i for i, ln in enumerate(lines) if is_guard_open(ln)), None)
    if if_idx is None:
        return None
    depth = 1
    else_idx = None
    for i in range(if_idx + 1, len(lines)):
        ln = lines[i]
        if _RE_IF.match(ln):
            depth += 1
        elif _RE_ENDIF.match(ln):
            depth -= 1
            if depth == 0:
                return (if_idx, else_idx, i) if else_idx is not None else None
        elif _RE_ELSE.match(ln) and depth == 1:
            else_idx = i
    return None


def check_file(rel: str, token: str) -> list[str]:
    """Return a list of violation strings for one stub TU (empty when clean)."""
    path = REPO_ROOT / rel
    if not path.is_file():
        return [f"{rel}: file not found (expected an insecure stub TU here)"]
    lines = path.read_text(encoding="utf-8").splitlines()

    region = find_guard_region(lines)
    if region is None:
        return [
            f"{rel}: missing the stub-crypto guard "
            f"'#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)' "
            f"with a matching #else / #endif"
        ]
    if_idx, else_idx, endif_idx = region

    problems: list[str] = []

    # (2) the #else branch must be fail-closed: a compile-time #error or a hard
    #     k_ra8_err_ return (never k_ra8_ok).
    else_body = lines[else_idx + 1 : endif_idx]
    fail_closed = any(_RE_ERROR.match(ln) or "k_ra8_err_" in ln for ln in else_body)
    if not fail_closed:
        problems.append(
            f"{rel}: the #else branch is not fail-closed "
            f"(needs a #error or a k_ra8_err_* hard return, not k_ra8_ok)"
        )

    # (3) the insecure signature token must live INSIDE the guarded #if region
    #     and never escape it.
    hits = [i for i, ln in enumerate(lines) if token in ln]
    inside = [i for i in hits if if_idx < i < else_idx]
    escaped = [i for i in hits if not (if_idx < i < else_idx)]
    if not inside:
        problems.append(
            f"{rel}: insecure signature '{token}' not found inside the guarded "
            f"#if region (is the insecure body still present and guarded?)"
        )
    if escaped:
        where = ", ".join(f"line {i + 1}" for i in escaped)
        problems.append(
            f"{rel}: insecure signature '{token}' appears OUTSIDE the guard "
            f"({where}) -- the insecure body must be fully inside the "
            f"#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE) block"
        )

    return problems


def main() -> int:
    """Verify every placeholder-crypto TU is guarded fail-closed.

    These TUs hold deliberately insecure stand-ins for real crypto. The guard
    must make the ``#else`` half -- the branch taken when the real
    implementation is absent -- refuse to build or fail closed, so a
    misconfiguration cannot silently ship the placeholder as if it were the
    algorithm.

    Returns 1 listing each unguarded TU, 0 when all are fail-closed.
    """
    all_problems: list[str] = []
    for rel, token in STUB_TUS.items():
        all_problems.extend(check_file(rel, token))

    if all_problems:
        print("check_stub_crypto_guarded.py: insecure placeholder crypto not guarded fail-closed:")
        for p in all_problems:
            print(f"  {p}")
        print("Fix each at the root -- wrap the insecure body in")
        print("  #if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)")
        print("and make the #else fail closed (return k_ra8_err_* / #error), or replace")
        print("the placeholder with a real crypto backend.")
        return 1

    count = len(STUB_TUS)
    print(f"check_stub_crypto_guarded.py: PASS -- {count} stub crypto TU(s) guarded fail-closed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
