#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Auto-Sync Utility.

Automatically updates internal cryptographic hashes and appends missing
suppressions to the ledger as 'unreviewed' drafts.
"""

import subprocess
import sys
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run(cmd: list[str]) -> bool:
    """Run a subprocess."""
    print(f"==> Running: {' '.join(cmd)}")
    proc = subprocess.run(cmd, cwd=_repo_root(), check=False)  # noqa: S603
    return proc.returncode == 0


def update_ledger() -> None:
    """Update ledger."""
    ledger_path = _repo_root() / ".github" / "suppression-review-ledger.tsv"

    # 1. Get current candidates
    proc = subprocess.run(
        [sys.executable, "scripts/checks/check_suppressions.py", "--ledger-candidates"],
        cwd=_repo_root(),
        capture_output=True,
        text=True,
        check=False,
    )

    if proc.returncode == 0 and not proc.stdout.strip():
        # Clean tree, no missing sites
        return

    # Check if there are candidates
    lines = proc.stdout.splitlines()
    if len(lines) <= 1:  # just the header
        return

    # Read existing ledger
    existing = ledger_path.read_text(encoding="utf-8").splitlines()
    header = existing[0]
    rows = existing[1:]

    # Parse existing into a dict
    ledger_dict = {}
    for r in rows:
        if not r.strip():
            continue
        parts = r.split("\t")
        ledger_dict[parts[0]] = r

    # Append new candidates ONLY if missing
    added = 0
    for line in lines[1:]:  # skip header
        if not line.strip():
            continue
        parts = line.split("\t")
        if parts[0] not in ledger_dict:
            ledger_dict[parts[0]] = line
            added += 1

    if added == 0:
        return

    print(f"==> Found {added} missing suppressions. Appending to ledger as 'unreviewed' drafts...")

    # Write back sorted
    new_content = [header]
    new_content.extend(ledger_dict[k] for k in sorted(ledger_dict.keys()))

    ledger_path.write_text("\n".join(new_content) + "\n", encoding="utf-8")
    print("==> Ledger updated. Run 'just quality::local::review-suppressions' to approve them.")


def main() -> None:
    """Main."""
    # 1. Bless Python checking script scopes
    run([sys.executable, "scripts/checks/suppression_checker_scope.py", "--update"])

    # 2. Update MC/DC coverage ratchets
    run([sys.executable, "scripts/checks/mcdc_compound_ratchet.py", "--update"])

    # 3. Format the tree (in case modifications broke formatting)
    run(["bash", "scripts/checks/format_tree.sh"])

    # 4. Sync the ledger with new unreviewed sites
    update_ledger()


if __name__ == "__main__":
    main()
