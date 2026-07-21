#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Provision a root-of-trust public key into ra8_rot.c.

Rewrites the single `s_rot_root_pubkey[...]` initialiser in ra8_rot.c from a C
header emitted by `tools/rot_sign.py keygen --pubkey-c`. The provisioning
ceremony (scripts/secrets/rot_provision.sh) and the re-key flow (scripts/secrets/rot_keystore.py
rekey) both call this, so the patch logic lives in exactly one place -- two
copies of it drifting is how a board gets provisioned with a key that does not
match the one images are signed with.

The rewrite is in place and unconditional: ra8_rot.c is overwritten with no
backup, and the array it replaces is matched by a regex on the exact
declaration text. Renaming or reformatting that declaration in ra8_rot.c makes
the match fail rather than silently patch the wrong thing.

Usage:
    python3 tools/rot_patch_pubkey.py <ra8_rot.c> <pubkey-c-header>
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

_ARGC = 3
_ARRAY_RE = re.compile(
    r"static const uint8_t s_rot_root_pubkey\[k_ra8_rot_pubkey_bytes\] = \{.*?\n\};",
    re.DOTALL,
)


def patch(rot_c: str, header: str) -> None:
    """Replace the s_rot_root_pubkey array in rot_c with the header's bytes."""
    lines = Path(header).read_text(encoding="ascii").splitlines()
    body = "\n".join(line for line in lines if line.strip().startswith("0x"))
    if not body:
        sys.exit(f"no 0x.. byte lines found in {header}")
    new_array = (
        "static const uint8_t s_rot_root_pubkey[k_ra8_rot_pubkey_bytes] = {\n" + body + "\n};"
    )
    src = Path(rot_c).read_text(encoding="ascii")
    patched, n = _ARRAY_RE.subn(new_array, src, count=1)
    if n != 1:
        sys.exit(f"expected exactly one s_rot_root_pubkey definition, found {n}")
    Path(rot_c).write_text(patched, encoding="ascii")
    print(f"patched {rot_c}")


def main(argv: list[str]) -> int:
    """CLI entry: patch(argv[1] = ra8_rot.c, argv[2] = pubkey-c header)."""
    if len(argv) != _ARGC:
        sys.stderr.write("usage: rot_patch_pubkey.py <ra8_rot.c> <pubkey-c-header>\n")
        return 2
    patch(argv[1], argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
