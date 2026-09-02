# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Non-authority category vocabulary for checker-constant classification."""

from __future__ import annotations


def non_authority_categories() -> dict[str, str]:
    """Return the reviewed category names and their human-readable meanings."""
    return {
        "cli-default": ("Argparse default, environment-variable name, or tool executable name."),
        "derived-runtime": (
            "Value computed from the environment (repository root paths, argv, os state) "
            "carrying no authored policy list."
        ),
        "detector-pattern": (
            "Pattern or token set the checker searches FOR; it is the detection subject, "
            "guarded by selftest fixtures and baselines, and selects or exempts no input."
        ),
        "diagnostic-text": ("Human-facing message, label, or report text used only in output."),
        "exit-code": ("Process exit status value."),
        "numeric-format": (
            "Algorithm-internal numeric constant (index, width, radix, buffer size, parse "
            "limit) that gates no scan population."
        ),
        "parser-token": (
            "Syntax or structure token used to parse an input format; it neither selects "
            "inputs nor defines a waiver."
        ),
        "selftest-fixture": ("Fixture data consumed only by a selftest."),
    }
