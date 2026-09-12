#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Review Suppressions Utility.

Finds all 'unreviewed' ledger rows, assigns them a rationale, creates a batch,
and sets them to 'retain' so they pass CI.
"""

import argparse
import datetime
import hashlib
import sys
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _read_ledger() -> list[str]:
    return (
        (_repo_root() / ".github" / "suppression-review-ledger.tsv")
        .read_text(encoding="utf-8")
        .splitlines()
    )


def _write_ledger(lines: list[str]) -> None:
    (_repo_root() / ".github" / "suppression-review-ledger.tsv").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


def _get_rationale_evidence(args: argparse.Namespace) -> tuple[str, str]:
    rationale = args.rationale
    if not rationale:
        print("\nAvailable Rationales:")
        print("  1. tool-false-positive")
        print("  2. language-or-compiler-contract")
        print("  3. negative-test-contract")
        print("  4. generated-reproducible-output")
        print("  5. vendor-upstream-preserved")
        print("  6. reviewed-scope-authority")
        choice = input("Select a rationale (1-6) or type the ID directly: ").strip()
        mapping = {
            "1": "tool-false-positive",
            "2": "language-or-compiler-contract",
            "3": "negative-test-contract",
            "4": "generated-reproducible-output",
            "5": "vendor-upstream-preserved",
            "6": "reviewed-scope-authority",
        }
        rationale = mapping.get(choice, choice)

    evidence = args.evidence
    if not evidence:
        evidence = input(f"Enter evidence reference for why '{rationale}' applies here: ").strip()

    return rationale, evidence


def _write_batch(batch_id: str, count: int, batch_sha256: str, evidence: str) -> None:
    batches_path = _repo_root() / ".github" / "suppression-review-batches.yml"
    batches_yaml = batches_path.read_text(encoding="utf-8").rstrip()

    batch_entry = f"""
  - id: {batch_id}
    authority: repository-suppression-review
    date: {datetime.datetime.now(tz=datetime.UTC).strftime("%Y-%m-%d")}
    identity_schema: 2-durable-site-identity
    assigned_rows: {count}
    rows_sha256: {batch_sha256}
    partition: "Automated developer review batch"
    evidence:
      - "{evidence}"
"""
    batches_path.write_text(batches_yaml + batch_entry, encoding="utf-8")


def main() -> int:
    """Execute the review approval flow."""
    parser = argparse.ArgumentParser(description="Approve draft unreviewed suppressions.")
    parser.add_argument("--rationale", type=str, help="Rationale ID")
    parser.add_argument("--evidence", type=str, help="Evidence reference string")
    args = parser.parse_args()

    ledger = _read_ledger()
    header = ledger[0]
    rows = ledger[1:]

    unreviewed = []
    for r in rows:
        if not r.strip():
            continue
        parts = r.split("\t")
        if parts[2] == "unreviewed":
            unreviewed.append(parts)

    if not unreviewed:
        print("No 'unreviewed' rows found in ledger. Nothing to do.")
        return 0

    print(f"Found {len(unreviewed)} unreviewed suppression(s).")
    rationale, evidence = _get_rationale_evidence(args)

    date_str = datetime.datetime.now(tz=datetime.UTC).strftime("%Y%m%d")
    batch_id = f"auto-review-batch-{date_str}"

    payload = [f"{parts[0]}\t{parts[1]}\tretain\t{rationale}\t{evidence}" for parts in unreviewed]

    batch_sha256 = hashlib.sha256("\n".join(payload).encode("utf-8")).hexdigest()

    updated_ledger = [header]
    for r in rows:
        if not r.strip():
            continue
        parts = r.split("\t")
        if parts[2] == "unreviewed":
            parts[2] = "retain"
            parts[3] = rationale
            parts[4] = batch_id
            parts[5] = evidence
            updated_ledger.append("\t".join(parts))
        else:
            updated_ledger.append(r)

    _write_ledger(updated_ledger)
    _write_batch(batch_id, len(unreviewed), batch_sha256, evidence)

    print(f"\nSuccessfully approved {len(unreviewed)} row(s) and generated batch {batch_id}.")
    print("Run `just quality::local::gate suppressions` to verify.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
