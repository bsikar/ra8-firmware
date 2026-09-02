# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Fail-closed review ledger binding every suppression site to a decision.

Three committed authorities drive the ledger:

* ``.github/suppression-review-rationales.yml`` -- the closed rationale
  vocabulary. Each category names its allowed ledger state, concrete
  applicability criteria, required evidence kinds, and an optional
  revalidation trigger. Categories are vocabulary, never approval.
* ``.github/suppression-review-ledger.tsv`` -- exactly one row per live
  occurrence: ``site_id  binding_sha256  state  rationale_id  batch_id
  evidence_ref``. States: ``unreviewed`` (bootstrap only), ``retain``,
  ``fix-required``, ``resolved``, ``superseded``.
* ``.github/suppression-review-batches.yml`` -- one record per review batch:
  authority, date, identity schema, assigned row count, and the digest of
  the batch's ordered ledger rows.

The gate rejects live sites missing from the ledger, ledger rows whose site
is gone, binding drift under ``retain``, duplicate identities, unknown
categories/states/batches, unsorted rows, batch count or digest mismatches,
blank rationale or evidence on reviewed rows, active ``unreviewed`` or
``fix-required`` rows, and identity-schema drift. Only a ``retain`` row with
an exact binding match marks a row approved; nothing here generates
approval.
"""

from __future__ import annotations

import hashlib
import re
from collections.abc import Hashable
from dataclasses import dataclass, replace
from pathlib import Path

import yaml
from suppression_identity import IDENTITY_SCHEMA_VERSION
from suppression_model import Finding, Inventory

RATIONALES_PATH = ".github/suppression-review-rationales.yml"
LEDGER_PATH = ".github/suppression-review-ledger.tsv"
BATCHES_PATH = ".github/suppression-review-batches.yml"
LEDGER_COLUMNS = 6
LEDGER_STATES = frozenset({"unreviewed", "retain", "fix-required", "resolved", "superseded"})
ACTIVE_STATES = frozenset({"unreviewed", "retain", "fix-required"})
_HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
_SAMPLE_LIMIT = 5


@dataclass(frozen=True)
class LedgerRow:
    """One reviewed occurrence decision."""

    line: int
    site_id: str
    binding_sha256: str
    state: str
    rationale_id: str
    batch_id: str
    evidence_ref: str


class DuplicateKeySafeLoader(yaml.SafeLoader):
    """Safe YAML loader that rejects a repeated mapping key instead of dropping it.

    PyYAML's mapping construction keeps the LAST value bound to a repeated
    key and reports nothing, so a second ``id:`` inside a batch record, or a
    second top-level ``batches:`` block, silently replaces the reviewed
    authority before any application validation runs. Rejecting the document
    at parse time is the only place that decision is still visible.
    """

    def construct_mapping(self, node: yaml.MappingNode, deep: bool = False) -> dict:
        """Construct one mapping, rejecting any explicitly repeated key.

        Merge keys are skipped, so YAML's documented ``<<`` override semantics
        keep working: only keys written twice in the same mapping are refused.
        """
        seen: set[object] = set()
        for key_node, _value_node in node.value:
            if key_node.tag == "tag:yaml.org,2002:merge":
                continue
            key = self.construct_object(key_node, deep=deep)
            if not isinstance(key, Hashable):
                continue
            if key in seen:
                context = "while constructing a mapping"
                problem = f"duplicate key {key!r}"
                raise yaml.constructor.ConstructorError(
                    context, node.start_mark, problem, key_node.start_mark
                )
            seen.add(key)
        return super().construct_mapping(node, deep=deep)


def _load_document(text: str) -> object:
    """Parse one YAML document with duplicate keys rejected at parse time."""
    loader = DuplicateKeySafeLoader(text)
    try:
        return loader.get_single_data()
    finally:
        loader.dispose()


def _load_yaml(root: Path, rel: str) -> tuple[dict | None, Finding | None]:
    """Load one committed YAML authority, failing closed on any defect."""
    try:
        data = _load_document((root / rel).read_text(encoding="utf-8"))
    except OSError:
        return None, Finding("missing-review-ledger", f"{rel} is absent", rel)
    except yaml.YAMLError as exc:
        return None, Finding("malformed-review-ledger", str(exc), rel)
    if not isinstance(data, dict):
        return None, Finding("malformed-review-ledger", "document is not a mapping", rel)
    return data, None


def load_rationales(root: Path) -> tuple[dict[str, dict], list[Finding]]:
    """Parse the rationale vocabulary and validate every category contract."""
    data, problem = _load_yaml(root, RATIONALES_PATH)
    if problem is not None:
        return {}, [problem]
    findings: list[Finding] = []
    categories = data.get("categories")
    if not isinstance(categories, dict) or not categories:
        return {}, [Finding("malformed-review-ledger", "no categories", RATIONALES_PATH)]
    result: dict[str, dict] = {}
    for name, spec in categories.items():
        if not isinstance(spec, dict):
            findings.append(
                Finding(
                    "malformed-review-ledger", f"category {name} is not a mapping", RATIONALES_PATH
                )
            )
            continue
        state = spec.get("state")
        applicability = spec.get("applicability")
        evidence = spec.get("evidence")
        if (
            state not in LEDGER_STATES - {"unreviewed"}
            or not isinstance(applicability, str)
            or not applicability.strip()
            or not isinstance(evidence, list)
            or not evidence
        ):
            findings.append(
                Finding(
                    "malformed-review-ledger",
                    f"category {name} needs a reviewed state, applicability, and evidence kinds",
                    RATIONALES_PATH,
                )
            )
            continue
        result[name] = spec
    return result, findings


def load_ledger(root: Path) -> tuple[list[LedgerRow], list[Finding]]:
    """Parse the ledger rows, rejecting malformed shape or ordering."""
    try:
        text = (root / LEDGER_PATH).read_text(encoding="utf-8")
    except OSError:
        return [], [Finding("missing-review-ledger", f"{LEDGER_PATH} is absent", LEDGER_PATH)]
    findings: list[Finding] = []
    rows: list[LedgerRow] = []
    lines = text.splitlines()
    expected_header = "site_id\tbinding_sha256\tstate\trationale_id\tbatch_id\tevidence_ref"
    if not lines or lines[0] != expected_header:
        findings.append(Finding("malformed-review-ledger", "missing header row", LEDGER_PATH, 1))
        return [], findings
    for line_no, raw in enumerate(lines[1:], start=2):
        parts = raw.split("\t")
        if len(parts) != LEDGER_COLUMNS:
            findings.append(
                Finding("malformed-review-ledger", "wrong column count", LEDGER_PATH, line_no)
            )
            continue
        row = LedgerRow(line_no, *parts)
        if not _HEX64_RE.match(row.site_id) or not _HEX64_RE.match(row.binding_sha256):
            findings.append(
                Finding("malformed-review-ledger", "identity is not 64 hex", LEDGER_PATH, line_no)
            )
            continue
        if row.state not in LEDGER_STATES:
            findings.append(
                Finding(
                    "malformed-review-ledger", f"unknown state {row.state}", LEDGER_PATH, line_no
                )
            )
            continue
        rows.append(row)
    ordered = [row.site_id for row in rows]
    if ordered != sorted(ordered):
        findings.append(
            Finding("malformed-review-ledger", "rows are not sorted by site_id", LEDGER_PATH)
        )
    seen: set[str] = set()
    for row in rows:
        if row.site_id in seen:
            findings.append(
                Finding(
                    "ledger-duplicate-site", f"duplicate site {row.site_id}", LEDGER_PATH, row.line
                )
            )
        seen.add(row.site_id)
    return rows, findings


def load_batches(root: Path) -> tuple[dict[str, dict], list[Finding]]:
    """Parse the batch records and validate their schema fields."""
    data, problem = _load_yaml(root, BATCHES_PATH)
    if problem is not None:
        return {}, [problem]
    findings: list[Finding] = []
    batches = data.get("batches")
    if not isinstance(batches, list):
        return {}, [Finding("malformed-review-ledger", "no batches list", BATCHES_PATH)]
    result: dict[str, dict] = {}
    required = ("id", "authority", "date", "identity_schema", "assigned_rows", "rows_sha256")
    for record in batches:
        if not isinstance(record, dict) or any(key not in record for key in required):
            findings.append(
                Finding("malformed-review-ledger", "batch record missing fields", BATCHES_PATH)
            )
            continue
        if record["identity_schema"] != IDENTITY_SCHEMA_VERSION:
            findings.append(
                Finding(
                    "ledger-schema-mismatch",
                    f"batch {record['id']} reviewed under {record['identity_schema']}; "
                    f"live schema is {IDENTITY_SCHEMA_VERSION}",
                    BATCHES_PATH,
                )
            )
        identity = str(record["id"])
        if identity in result:
            findings.append(
                Finding(
                    "ledger-duplicate-batch",
                    f"batch identity {identity} is recorded more than once",
                    BATCHES_PATH,
                )
            )
            continue
        result[identity] = record
    return result, findings


def _sample(values: list[str]) -> str:
    """Render a bounded sample list for one aggregate finding."""
    shown = ", ".join(values[:_SAMPLE_LIMIT])
    more = len(values) - min(len(values), _SAMPLE_LIMIT)
    return shown + (f" (+{more} more)" if more > 0 else "")


def _batch_findings(rows: list[LedgerRow], batches: dict[str, dict]) -> list[Finding]:
    """Verify per-batch row counts and ordered-row digests."""
    findings: list[Finding] = []
    by_batch: dict[str, list[LedgerRow]] = {}
    for row in rows:
        if row.state == "unreviewed":
            continue
        if row.batch_id not in batches:
            findings.append(
                Finding(
                    "ledger-unknown-reference",
                    f"unknown batch {row.batch_id or '(blank)'}",
                    LEDGER_PATH,
                    row.line,
                )
            )
            continue
        by_batch.setdefault(row.batch_id, []).append(row)
    for batch_id, record in batches.items():
        members = by_batch.get(batch_id, [])
        if len(members) != record["assigned_rows"]:
            findings.append(
                Finding(
                    "ledger-batch-mismatch",
                    f"batch {batch_id} has {len(members)} rows; record says "
                    f"{record['assigned_rows']}",
                    BATCHES_PATH,
                )
            )
        payload = "\n".join(
            f"{row.site_id}\t{row.binding_sha256}\t{row.state}"
            f"\t{row.rationale_id}\t{row.evidence_ref}"
            for row in members
        ).encode("utf-8")
        digest = hashlib.sha256(payload).hexdigest()
        if digest != record["rows_sha256"]:
            findings.append(
                Finding(
                    "ledger-batch-mismatch",
                    f"batch {batch_id} rows digest {digest} != recorded {record['rows_sha256']}",
                    BATCHES_PATH,
                )
            )
    return findings


def _portable_evidence_values(row: LedgerRow) -> tuple[dict[str, str], Finding | None]:
    """Parse the three required portable-test evidence fields."""
    required = ("test-name", "passing-counterpart", "registered-gate")
    values: dict[str, str] = {}
    duplicates: set[str] = set()
    for part in row.evidence_ref.split():
        key, separator, value = part.partition(":")
        if key not in required:
            continue
        if key in values:
            duplicates.add(key)
        if separator and value:
            values[key] = value
    missing = [key for key in required if not values.get(key)]
    if missing or duplicates:
        detail = []
        if missing:
            detail.append(f"missing {', '.join(missing)}")
        if duplicates:
            detail.append(f"duplicate {', '.join(sorted(duplicates))}")
        return {}, Finding(
            "malformed-review-ledger",
            "portable prerequisite evidence is incomplete: " + "; ".join(detail),
            LEDGER_PATH,
            row.line,
        )
    return values, None


def _test_reference_is_live(root: Path, reference: str) -> bool:
    """Return whether ``path::function`` identifies a current test function."""
    target, separator, symbol = reference.partition("::")
    try:
        text = (root / target).read_text(encoding="utf-8")
    except OSError:
        return False
    return bool(
        separator
        and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol)
        and re.search(rf"\bdef\s+{re.escape(symbol)}\s*\(", text)
    )


def _portable_gate_fails_closed(root: Path) -> bool:
    """Return whether the work-harness gate forbids prerequisite skips."""
    try:
        gate_text = (root / "scripts/ci/gates/tests.sh").read_text(encoding="utf-8")
        helper_text = (root / "tools/work/tests/fixtures/work_testlib.py").read_text(
            encoding="utf-8"
        )
    except OSError:
        return False
    gate_match = re.search(r"gate_work_harness\(\) \((.*?)^\)", gate_text, re.MULTILINE | re.DOTALL)
    gate_body = gate_match.group(1) if gate_match is not None else ""
    gate_contract = (
        "require_cmd bash" in gate_body
        and "require_cmd sh" in gate_body
        and "RA8_WORK_HARNESS_REGISTERED_GATE=1" in gate_body
        and "tools/work/src/work.py --selftest" in gate_body
    )
    helper_contract = (
        'os.environ.get(REGISTERED_GATE_ENV) == "1"' in helper_text
        and "raise RuntimeError(message)" in helper_text
    )
    return gate_contract and helper_contract


def _portable_test_evidence_findings(row: LedgerRow, root: Path) -> list[Finding]:
    """Validate the fail-closed evidence contract for a portable test skip."""
    values, malformed = _portable_evidence_values(row)
    if malformed is not None:
        return [malformed]

    findings = [
        Finding(
            "ledger-unknown-reference",
            f"portable prerequisite {key} does not name a live test: {values[key]}",
            LEDGER_PATH,
            row.line,
        )
        for key in ("test-name", "passing-counterpart")
        if not _test_reference_is_live(root, values[key])
    ]

    gate = values["registered-gate"]
    if gate != "work-harness":
        findings.append(
            Finding(
                "ledger-unknown-reference",
                f"portable prerequisite names unknown registered gate {gate}",
                LEDGER_PATH,
                row.line,
            )
        )
        return findings
    if not _portable_gate_fails_closed(root):
        findings.append(
            Finding(
                "ledger-invalid-gate-contract",
                "work-harness gate can skip a portable prerequisite instead of failing closed",
                LEDGER_PATH,
                row.line,
            )
        )
    return findings


def _reviewed_row_findings(
    row: LedgerRow, rationales: dict[str, dict], root: Path
) -> list[Finding]:
    """Validate rationale, state compatibility, and evidence on one row."""
    findings: list[Finding] = []
    if row.state == "unreviewed":
        if row.rationale_id or row.batch_id:
            findings.append(
                Finding(
                    "malformed-review-ledger",
                    "unreviewed rows carry no rationale or batch",
                    LEDGER_PATH,
                    row.line,
                )
            )
        return findings
    spec = rationales.get(row.rationale_id)
    if spec is None:
        findings.append(
            Finding(
                "ledger-unknown-reference",
                f"unknown rationale {row.rationale_id or '(blank)'}",
                LEDGER_PATH,
                row.line,
            )
        )
        return findings
    completed = row.state in {"resolved", "superseded"} and spec["state"] == "fix-required"
    if spec["state"] != row.state and not completed:
        findings.append(
            Finding(
                "ledger-state-conflict",
                f"rationale {row.rationale_id} allows state {spec['state']}, row says {row.state}",
                LEDGER_PATH,
                row.line,
            )
        )
    if not row.evidence_ref.strip():
        findings.append(
            Finding(
                "malformed-review-ledger", "reviewed row has blank evidence", LEDGER_PATH, row.line
            )
        )
    if row.rationale_id == "portable-test-prerequisite-boundary":
        findings.extend(_portable_test_evidence_findings(row, root))
    return findings


def _reconcile_row(inventory: Inventory, live: dict[str, int], row: LedgerRow) -> list[Finding]:
    """Reconcile one ledger row against the live inventory, fail closed."""
    findings: list[Finding] = []
    index = live.get(row.site_id)
    if row.state in ACTIVE_STATES:
        if index is None:
            findings.append(
                Finding(
                    "ledger-stale-site",
                    f"{row.state} site no longer exists: {row.site_id}",
                    LEDGER_PATH,
                    row.line,
                )
            )
        elif row.state == "retain":
            item = inventory.suppressions[index]
            if item.binding_sha256 != row.binding_sha256:
                findings.append(
                    Finding(
                        "ledger-binding-mismatch",
                        f"retained site {row.site_id} content changed "
                        f"({item.path}: reviewed {row.binding_sha256[:12]}, "
                        f"live {item.binding_sha256[:12]})",
                        LEDGER_PATH,
                        row.line,
                    )
                )
            else:
                inventory.suppressions[index] = replace(item, disposition="approved")
    elif index is not None:
        findings.append(
            Finding(
                "ledger-resolved-still-present",
                f"{row.state} site is still present: {row.site_id}",
                LEDGER_PATH,
                row.line,
            )
        )
    if row.state == "superseded":
        replacements = [
            part.removeprefix("replaced-by:")
            for part in row.evidence_ref.split()
            if part.startswith("replaced-by:")
        ]
        if not replacements or any(rep not in live for rep in replacements):
            findings.append(
                Finding(
                    "ledger-unknown-reference",
                    f"superseded site {row.site_id} names no live replacement",
                    LEDGER_PATH,
                    row.line,
                )
            )
    return findings


def apply_ledger(inventory: Inventory, root: Path) -> None:
    """Reconcile the committed ledger against the live inventory, fail closed."""
    rationales, findings = load_rationales(root)
    rows, row_findings = load_ledger(root)
    batches, batch_findings = load_batches(root)
    findings.extend(row_findings)
    findings.extend(batch_findings)
    if any(item.code == "missing-review-ledger" for item in findings):
        inventory.findings.extend(findings)
        return
    findings.extend(_batch_findings(rows, batches))
    for row in rows:
        findings.extend(_reviewed_row_findings(row, rationales, root))
    live = {item.site_id: index for index, item in enumerate(inventory.suppressions)}
    by_site = {row.site_id: row for row in rows}
    missing = sorted(site for site in live if site not in by_site)
    if missing:
        findings.append(
            Finding(
                "ledger-missing-site",
                f"{len(missing)} live suppression(s) absent from the ledger: {_sample(missing)}",
            )
        )
    unreviewed = [row for row in rows if row.state == "unreviewed"]
    if unreviewed:
        findings.append(
            Finding(
                "ledger-unreviewed",
                f"{len(unreviewed)} ledger row(s) await review: "
                f"{_sample([row.site_id for row in unreviewed])}",
            )
        )
    fix_required = [row for row in rows if row.state == "fix-required"]
    if fix_required:
        findings.append(
            Finding(
                "ledger-fix-required",
                f"{len(fix_required)} site(s) carry an unremediated fix decision: "
                f"{_sample([row.site_id for row in fix_required])}",
            )
        )
    for row in rows:
        findings.extend(_reconcile_row(inventory, live, row))
    inventory.findings.extend(findings)


def candidate_rows(inventory: Inventory) -> list[str]:
    """Emit bootstrap candidates for live sites the ledger does not know.

    Candidates are always ``unreviewed``: generation can never approve.
    """
    return [
        f"{item.site_id}\t{item.binding_sha256}\tunreviewed\t\t\t"
        for item in sorted(inventory.suppressions, key=lambda row: row.site_id)
    ]
