#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate every first-party Markdown link, anchor, and repository path."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import markdown_reference_selftest
import markdown_references as core


def main() -> int:
    """Run the live Markdown audit, inventory mode, or detector selftest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="run detector selftests")
    parser.add_argument("--inventory", type=Path, help="write deterministic JSON evidence")
    args = parser.parse_args()
    if args.selftest:
        return markdown_reference_selftest.selftest()
    try:
        findings, counts, inventory = core.check_tree(core.REPO_ROOT)
    except (core.CheckError, OSError, subprocess.SubprocessError, UnicodeError) as exc:
        print(f"check_markdown_references.py: ERROR: {exc}", file=sys.stderr)
        return 2
    if args.inventory is not None:
        payload = {"schema": 1, "counts": counts, "inventory": inventory}
        rendered = json.dumps(payload, indent=2, sort_keys=True) + "\n"
        args.inventory.write_text(rendered, encoding="ascii")
    if findings:
        for finding in sorted(findings, key=lambda item: (item.path, item.line, item.kind)):
            print(finding.render(), file=sys.stderr)
        print(
            f"check_markdown_references.py: FAIL ({len(findings)} findings; {counts})",
            file=sys.stderr,
        )
        return 1
    print(f"Markdown references clean ({counts})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
