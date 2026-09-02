# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Adversarial assertions for MC/DC suppression-control binding."""

from __future__ import annotations

from selftest_assert import expect
from suppression_model import Inventory

EXPECTED_MCDC_FUNCTION_ROWS = 5
EXPECTED_MALFORMED_MCDC_MACRO_ROWS = 3


def assert_mcdc_macro_binding(inventory: Inventory, failures: list[str]) -> None:
    """Assert each same-line macro owns its literal and invalid calls fail."""
    function_rows = [
        item
        for item in inventory.suppressions
        if item.family == "mcdc-deactivation"
        and item.path == "malformed_mcdc.c"
        and item.scope == "function"
    ]
    expect(
        len(function_rows) == EXPECTED_MCDC_FUNCTION_ROWS
        and [item.reason for item in function_rows]
        == [
            "fixture function invariant",
            "valid after invalid",
            "valid before invalid",
            "first distinct reason",
            "second distinct reason",
        ],
        "must fire: each same-line MC/DC macro owns its exact literal reason",
        failures,
    )
    malformed_macros = [item for item in inventory.findings if item.code == "malformed-mcdc-macro"]
    expect(
        len(malformed_macros) == EXPECTED_MALFORMED_MCDC_MACRO_ROWS,
        "must fire: invalid macros on either side cannot borrow a valid reason",
        failures,
    )
