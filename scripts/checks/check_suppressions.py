#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Inventory every suppression and reconcile it against the review ledger.

Every recognized waiver row carries a durable two-part identity: a
``site_id`` naming the occurrence independent of line movement and a
``binding_sha256`` over exactly what a reviewer approved. The committed
ledger (``.github/suppression-review-ledger.tsv`` with its rationale and
batch authorities) binds each site to a reviewed decision; only a ``retain``
row with an exact binding match marks a suppression approved, and nothing in
this tool generates approval. ``--check`` stays nonzero while any site is
unreviewed, carries an unremediated fix decision, or any integrity finding
fires; exit 2 means the scan itself stopped being trustworthy. An exit-zero
``--inventory`` means only that report generation succeeded.

Usage::

    python3 scripts/checks/check_suppressions.py --selftest
    python3 scripts/checks/check_suppressions.py --inventory --format json
    python3 scripts/checks/check_suppressions.py --inventory --format markdown
    python3 scripts/checks/check_suppressions.py --check
    python3 scripts/checks/check_suppressions.py --ledger-candidates
    python3 scripts/checks/check_suppressions.py --list-files
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from suppression_ledger import candidate_rows
from suppression_model import Inventory, Suppression
from suppression_scan import MIN_REPOSITORY_FILES, git_paths, scan_repository
from suppression_selftest import run_selftest

REPO_ROOT = Path(__file__).resolve().parents[2]
MAX_CHECK_DETAILS = 80
INTEGRITY_CODES = frozenset(
    {
        "git-enumeration",
        "invalid-text-encoding",
        "duplicate-fingerprint",
        "duplicate-site-identity",
        "baseline-owner-mismatch",
        "baseline-ceiling-integrity",
        "baseline-growth",
        "duplicate-baseline-row",
        "duplicate-coverage-mask",
        "duplicate-mcdc-deactivation",
        "malformed-baseline-row",
        "malformed-coverage-mask",
        "malformed-mcdc-deactivation",
        "malformed-mcdc-macro",
        "malformed-native-test-skip",
        "missing-baseline-file",
        "missing-baseline-ceilings",
        "missing-baseline-consumer",
        "missing-baseline-provenance",
        "missing-baseline-total",
        "mcdc-owner-mismatch",
        "stale-baseline-path",
        "stranded-branch-marker",
        "stranded-line-marker",
        "stale-baseline-total",
        "unknown-baseline-file",
        "unknown-mcdc-macro",
        "unexpected-family-count",
        "unpaired-mcdc-deactivation",
        "vacuous-family",
        "missing-family",
        "malformed-heredoc",
        "malformed-ruff-config",
        "malformed-tool-config",
        "malformed-ansible-lint-config",
        "malformed-ci-parity-workflow",
        "malformed-doxygen-config",
        "malformed-generated-marker",
        "generated-marker-without-body",
        "non-substantive-waiver-reason",
        "self-generated-provenance",
        "symlinked-generated-provenance",
        "missing-generated-provenance",
        "malformed-gitignore-scope-marker",
        "malformed-global-exclusion-config",
        "duplicate-file-size-waiver",
        "untracked-generated-provenance",
        "unterminated-cmake-bracket",
        "checker-scope-ast",
        "checker-scope-authority-count",
        "checker-scope-value-count",
        "checker-scope-value-digest",
        "checker-scope-reason-digest",
        "checker-classification-digest",
        "checker-census-floor",
        "checker-authority-mutation",
        "checker-authority-rebinding",
        "condition-dependent-authority",
        "inline-scope-literal",
        "non-authority-shape-mismatch",
        "stale-checker-classification",
        "unclassified-checker-constant",
        "missing-checker-scope-authority",
        "unresolved-checker-scope-authority",
        "checker-nonfatal-ast",
        "checker-nonfatal-declaration-count",
        "missing-nonfatal-authority",
        "missing-nonfatal-declaration",
        "active-nonfatal-constant",
        "active-checker-nonfatal-invocation",
        "nonfatal-informational-count",
        "unexpected-governance-count",
        "python-tokenize",
        "read-error",
        "unterminated-comment",
        "unterminated-html-comment",
        "unterminated-heredoc",
        "unterminated-line-comment-splice",
        "unterminated-shell-quote",
        "vacuous-baseline-files",
        "vacuous-baseline-rows",
        "unterminated-shell-arithmetic",
        "unterminated-string",
        "unsafe-symlink",
        "vacuous-files",
        "vacuous-inventory",
        "missing-review-ledger",
        "malformed-review-ledger",
        "ledger-duplicate-batch",
        "ledger-duplicate-site",
        "ledger-schema-mismatch",
        "ledger-unknown-reference",
        "ledger-batch-mismatch",
        "ledger-state-conflict",
        "ledger-binding-mismatch",
        "ledger-stale-site",
        "ledger-resolved-still-present",
    }
)


def _parser() -> argparse.ArgumentParser:
    """Build the explicit-mode command-line parser."""
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--selftest", action="store_true", help="run both-direction fixtures")
    modes.add_argument(
        "--inventory",
        action="store_true",
        help="emit the ledger-reconciled inventory",
    )
    modes.add_argument("--check", action="store_true", help="fail on concerns or findings")
    modes.add_argument(
        "--ledger-candidates",
        action="store_true",
        help="print unreviewed ledger candidate rows for every live site",
    )
    modes.add_argument(
        "--list-files",
        action="store_true",
        help="list JSON-escaped scan paths (use -z for exact NUL-delimited paths)",
    )
    parser.add_argument("--format", choices=("json", "markdown"), help="inventory output format")
    parser.add_argument("-z", "--null", action="store_true", help="NUL-terminate --list-files")
    return parser


def _markdown_escape(value: object) -> str:
    """Escape one scalar for a Markdown table cell."""
    return str(value).replace("|", "\\|").replace("\n", " ")


def _render_summary(inventory: Inventory) -> list[str]:
    """Render deterministic Markdown summary bullets."""
    families = inventory.family_counts()
    lines = [
        f"- Files scanned: {inventory.files_scanned}",
        f"- Text files: {inventory.text_files}",
        f"- Binary files: {inventory.binary_files}",
        f"- Suppressions: {len(inventory.suppressions)}",
        f"- Scanner findings: {len(inventory.findings)}",
    ]
    if families:
        lines.append(
            "- Families: " + ", ".join(f"{key}={value}" for key, value in families.items())
        )
    return lines


def _render_markdown(inventory: Inventory) -> str:
    """Render the non-authoritative phase-one Markdown inventory."""
    lines = [
        "# Suppression inventory",
        "",
        *_render_summary(inventory),
        "",
    ]
    lines.extend(
        [
            "| Path | Line | Family | Tool | Rule | Scope | Owner | Reason | Concerns |",
            "|---|---:|---|---|---|---|---|---|---|",
        ]
    )
    ordered = sorted(inventory.suppressions, key=lambda item: (item.path, item.line, item.column))
    for item in ordered:
        values = (
            item.path,
            item.line,
            item.family,
            item.tool,
            item.rule,
            item.scope,
            item.owner,
            item.reason,
            ", ".join(item.concerns),
        )
        lines.append("| " + " | ".join(_markdown_escape(value) for value in values) + " |")
    lines.extend(["", "## Scanner findings", ""])
    if not inventory.findings:
        lines.append("None.")
    else:
        for finding in sorted(
            inventory.findings, key=lambda item: (item.path, item.line, item.code)
        ):
            location = f"{finding.path}:{finding.line}" if finding.path else "repository"
            lines.append(f"- `{finding.code}` at `{location}`: {_markdown_escape(finding.message)}")
    return "\n".join(lines) + "\n"


def _integrity_failed(inventory: Inventory) -> bool:
    """Return whether scanner evidence is malformed or vacuous."""
    return any(finding.code in INTEGRITY_CODES for finding in inventory.findings)


def _review_count(inventory: Inventory) -> int:
    """Count scanner findings and per-row review concerns.

    Concerns on an approved row do not count again: the ledger binding the
    approval hashes the concerns, so any change reopens the review instead.
    """
    pending = sum(item.disposition != "approved" for item in inventory.suppressions)
    return (
        len(inventory.findings)
        + sum(
            len(item.concerns) for item in inventory.suppressions if item.disposition != "approved"
        )
        + pending
    )


def _concern_line(item: Suppression, concern: str) -> str:
    """Format one suppression concern for check-mode diagnostics."""
    return f"{item.path}:{item.line}: {concern}: {item.family}/{item.rule}"


def _run_check(inventory: Inventory) -> int:
    """Print bounded diagnostics and return clean, debt, or malformed status."""
    details = [
        f"{item.path or 'repository'}:{item.line}: {item.code}: {item.message}"
        for item in inventory.findings
    ]
    details.extend(
        _concern_line(item, concern)
        for item in inventory.suppressions
        if item.disposition != "approved"
        for concern in item.concerns
    )
    details.extend(
        f"{item.path}:{item.line}: governance-pending: {item.fingerprint}"
        for item in inventory.suppressions
        if item.disposition != "approved"
    )
    for detail in details[:MAX_CHECK_DETAILS]:
        print(detail)
    if len(details) > MAX_CHECK_DETAILS:
        print(f"... {len(details) - MAX_CHECK_DETAILS} additional item(s) omitted")
    print(
        f"check_suppressions.py: {len(inventory.suppressions)} suppression(s), "
        f"{_review_count(inventory)} review item(s)"
    )
    if _integrity_failed(inventory):
        return 2
    return 1 if details else 0


def _run_list_files(*, null: bool) -> int:
    """List exact scan candidates and reject a collapsed Git enumeration."""
    paths, findings = git_paths(REPO_ROOT)
    if findings:
        for finding in findings:
            print(f"check_suppressions.py: FATAL -- {finding.message}", file=sys.stderr)
        return 2
    if len(paths) < MIN_REPOSITORY_FILES:
        print(
            f"check_suppressions.py: FATAL -- only {len(paths)} file(s); "
            f"floor is {MIN_REPOSITORY_FILES}",
            file=sys.stderr,
        )
        return 2
    if null:
        sys.stdout.buffer.write(b"\0".join(path.encode("utf-8") for path in paths) + b"\0")
    else:
        print("\n".join(json.dumps(path, ensure_ascii=True) for path in paths))
    return 0


def main(argv: list[str] | None = None) -> int:
    """Run the selected public scanner mode."""
    parser = _parser()
    args = parser.parse_args(argv)
    if args.format is not None and not args.inventory:
        parser.error("--format requires --inventory")
    if args.null and not args.list_files:
        parser.error("--null requires --list-files")
    if args.selftest:
        return run_selftest()
    if args.list_files:
        return _run_list_files(null=args.null)
    inventory, _ = scan_repository(REPO_ROOT)
    if args.ledger_candidates:
        print("site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref")
        for row in candidate_rows(inventory):
            print(row)
        return 2 if _integrity_failed(inventory) else 0
    if args.inventory:
        if (args.format or "json") == "json":
            print(json.dumps(inventory.as_dict(), indent=2, sort_keys=True, ensure_ascii=True))
        else:
            print(_render_markdown(inventory), end="")
        return 2 if _integrity_failed(inventory) else 0
    return _run_check(inventory)


if __name__ == "__main__":
    raise SystemExit(main())
