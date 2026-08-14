# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Stable keys and ratchet partitioning for function-documentation debt."""

from __future__ import annotations


def function_key(row: tuple) -> str:
    """Return the stable baseline key for one function-gap row."""
    source, _line, name, missing, _severity = row
    return f"{source}\t{name}\t{';'.join(missing)}"


def partition_function_gaps(rows: list[tuple], baseline: set[str]) -> tuple[list[tuple], list[str]]:
    """Return new gap rows and stale baseline keys for the ratchet verdict."""
    keyed = {function_key(row): row for row in rows if row[3]}
    new_rows = [keyed[key] for key in sorted(set(keyed) - baseline)]
    stale = sorted(baseline - set(keyed))
    return new_rows, stale
