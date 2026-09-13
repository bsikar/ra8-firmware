# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline gh stand-in for real-Just board explorer tests."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def main() -> int:
    """Answer only the two exact read families used by the board explorer."""
    fixtures = Path(os.environ["FAKE_BOARD_FIXTURES"])
    log = Path(os.environ["FAKE_BOARD_LOG"])
    with log.open("a", encoding="ascii") as handle:
        json.dump(sys.argv[1:], handle, ensure_ascii=True)
        handle.write("\n")
    args = sys.argv[1:]
    if args[:2] == ["project", "item-list"] and "--query" in args:
        sys.stdout.write((fixtures / "board_snapshot.json").read_text(encoding="ascii"))
        return 0
    if args[:2] == ["issue", "view"]:
        sys.stdout.write((fixtures / "issue_detail.json").read_text(encoding="ascii"))
        return 0
    print(f"unexpected fake gh argv: {args!r}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
