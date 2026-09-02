# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Syntax-aware scanners for suppression and waiver directives."""

from __future__ import annotations

import hashlib
import re
import subprocess
import tomllib
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from suppression_baseline_scan import scan_baseline_repository
from suppression_build_controls import compiler_records
from suppression_c_control_scan import C_CONTROL_HINT_RE, scan_c_controls
from suppression_c_controls import scan_c_control_file
from suppression_catalog import (
    BINARY_SUFFIXES,
    KNOWN_CPPCHECK_RULES,
    REQUIRED_FAMILIES,
    SUPPORTED_HINT_RE,
    UNSUPPORTED_CATEGORIES,
    language,
    ownership,
)
from suppression_checker_nonfatal import scan_checker_nonfatal_controls
from suppression_checker_scope import scan_checker_scope_controls
from suppression_clang_tidy import scan_clang_tidy_config
from suppression_comment_lex import extract_comments
from suppression_control_scan import scan_control_file
from suppression_coverage_scan import scan_coverage_masks
from suppression_generated_markers import generated_records
from suppression_governance import scan_ci_parity_exemptions, scan_governance_file
from suppression_hardware_todo import scan_hardware_todo_controls
from suppression_identity import assign_identities
from suppression_inline_scan import (
    Recognition,
    _concerns,
    _scan_comments,
    _scan_raw_nolint,
    stranded_branch_findings,
)
from suppression_ledger import apply_ledger
from suppression_model import Finding, Inventory, Suppression
from suppression_shell_scan import shell_status_records, yaml_shell_block_status_records
from suppression_tool_controls import (
    configured_optional_tools,
    scan_tool_configs,
    scan_tool_sources,
)
from suppression_validate import (
    deduplicate,
    validate_cppcheck_anchors,
    validate_fingerprints,
    validate_regions,
)

MIN_REPOSITORY_FILES = 2000
MIN_SUPPRESSIONS = 50
MIN_FAMILY_COUNTS = {
    "coverage-mask": 2,
    "mcdc-deactivation": 77,
}
EXPECTED_EXACT_FAMILY_COUNTS = {
    "ansible-lint-config": 0,
    "other-language-control": 0,
    "security-analysis-control": 0,
    "test-control": 16,
}
GOVERNANCE_COUNT_CONTRACTS = {
    "ansible-lint-config": ("family", "ansible-lint-config", 0),
    "ci-parity-exemptions": ("family", "ci-parity-exemption", 3),
    "generated-artifacts": ("family", "generated-artifact", 10),
    "doxygen-controls": ("family", "documentation-control", 17),
    "security-analysis-controls": ("family", "security-analysis-control", 0),
    "other-language-controls": ("family", "other-language-control", 0),
    "gitignore-scope-exemptions": ("tool", "gitignore-scope", 1),
    "hardware-canned-stub-waivers": ("tool", "check-no-silent-stubs", 0),
    "checker-scope-values": ("family", "checker-scope-control", 3779),
    "checker-nonfatal-declarations": ("directive", "nonfatal-declaration", 10),
    "checker-nonfatal-invocations": ("directive", "nonfatal-invocation", 0),
    "vendor-encoding-exemptions": ("family", "encoding-exemption", 4),
}


@dataclass
class CppcheckReasonState:
    """Rationale association state for the central cppcheck suppression list."""

    in_section: bool = False
    section_notes: list[str] = field(default_factory=list)
    local_notes: list[str] = field(default_factory=list)
    frozen_reason: str = ""
    last_was_entry: bool = False

    def add_comment(self, note: str) -> None:
        """Consume one comment or paired section delimiter."""
        if note and set(note) <= {"-"}:
            if self.in_section:
                self.frozen_reason = " ".join(self.section_notes).strip()
                self.in_section = False
            else:
                self.in_section = True
                self.section_notes = []
                self.frozen_reason = ""
            self.local_notes = []
            self.last_was_entry = False
        elif self.in_section:
            self.section_notes.append(note)
        else:
            if self.last_was_entry:
                self.local_notes = []
                self.frozen_reason = ""
            self.local_notes.append(note)
            self.last_was_entry = False

    def reason_for_entry(self) -> str:
        """Freeze an open section and return the rationale for the next entry."""
        if self.in_section:
            self.frozen_reason = " ".join(self.section_notes).strip()
            self.in_section = False
        self.last_was_entry = True
        return self.frozen_reason or " ".join(self.local_notes).strip()


def git_paths(root: Path) -> tuple[list[str], list[Finding]]:
    """Enumerate present tracked and nonignored untracked repository files."""
    proc = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],  # noqa: S607 -- fixed repository Git census
        cwd=root,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        message = proc.stderr.decode("utf-8", errors="replace").strip()
        return [], [Finding("git-enumeration", message or "git ls-files failed")]
    if proc.stderr:
        message = proc.stderr.decode("utf-8", errors="replace").strip()
        return [], [Finding("git-enumeration", message or "git ls-files emitted diagnostics")]
    return decode_git_paths(proc.stdout, root)


def decode_git_paths(data: bytes, _root: Path) -> tuple[list[str], list[Finding]]:
    """Decode Git's NUL list and reject names that are not valid UTF-8."""
    try:
        decoded = data.decode("utf-8", errors="strict").split("\0")
    except UnicodeDecodeError as exc:
        return [], [Finding("git-enumeration", f"non-UTF-8 repository path: {exc}")]
    paths = [path for path in decoded if path]
    return sorted(paths), []


def _read_text(root: Path, rel: str) -> tuple[str | None, Finding | None]:
    """Read one candidate as UTF-8 text, classifying binary data explicitly."""
    path = root / rel
    try:
        resolved = path.resolve(strict=True)
        resolved.relative_to(root)
    except (OSError, ValueError) as exc:
        if isinstance(exc, ValueError):
            failure = Finding("unsafe-symlink", "path resolves outside repository")
        else:
            failure = Finding("read-error", str(exc))
        return None, failure
    if path.suffix.lower() in BINARY_SUFFIXES:
        return None, None
    try:
        with resolved.open("rb") as handle:
            prefix = handle.read(8192)
            if b"\0" in prefix:
                return None, None
            data = prefix + handle.read()
    except OSError as exc:
        return None, Finding("read-error", str(exc))
    try:
        return data.decode("utf-8"), None
    except UnicodeDecodeError as exc:
        return None, Finding("invalid-text-encoding", str(exc))


def _ruff_config_record(path: str, line: int, recognition: Recognition) -> Suppression:
    """Build one source-located Ruff central-configuration inventory row."""
    return Suppression(
        path,
        line,
        1,
        "python",
        "ruff",
        recognition.rule,
        recognition.directive,
        recognition.scope,
        recognition.reason,
        "central-config",
        ownership(path),
        _concerns(recognition.rule, recognition.reason),
    )


def _toml_section_lines(text: str, name: str) -> list[tuple[int, str]]:
    """Return source-located lines from one exact TOML table."""
    result: list[tuple[int, str]] = []
    active = False
    for line_no, raw in enumerate(text.splitlines(), start=1):
        stripped = raw.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            active = stripped == f"[{name}]"
        elif active:
            result.append((line_no, raw))
    return result


def _ruff_list_entries(text: str, section: str, key: str) -> list[tuple[int, str, str]]:
    """Return line, string value, and rationale from one Ruff string array."""
    result: list[tuple[int, str, str]] = []
    active = False
    pattern = re.compile(r'^\s*"(?P<value>[^"]+)"\s*,?\s*(?:#\s*(?P<reason>.*))?$')
    for line_no, raw in _toml_section_lines(text, section):
        stripped = raw.strip()
        if stripped == f"{key} = [":
            active = True
        elif active and stripped == "]":
            break
        elif active and (match := pattern.fullmatch(raw)) is not None:
            result.append((line_no, match.group("value"), (match.group("reason") or "").strip()))
    return result


def _ruff_per_file_entries(text: str) -> list[tuple[int, str, str, str]]:
    """Return line, path, rule, and rationale for single-rule Ruff file waivers."""
    pattern = re.compile(
        r'^\s*"(?P<path>[^"]+)"\s*=\s*\[\s*"(?P<rule>[^"]+)"\s*\]'
        r"\s*(?:#\s*(?P<reason>.*))?$"
    )
    result: list[tuple[int, str, str, str]] = []
    for line_no, raw in _toml_section_lines(text, "tool.ruff.lint.per-file-ignores"):
        if (match := pattern.fullmatch(raw)) is not None:
            result.append(
                (
                    line_no,
                    match.group("path"),
                    match.group("rule"),
                    (match.group("reason") or "").strip(),
                )
            )
    return result


def _ruff_config_records(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory Ruff global ignores, per-file ignores, and path exclusions."""
    if path != "pyproject.toml":
        return [], []
    try:
        parsed = tomllib.loads(text)
        ruff = parsed["tool"]["ruff"]
        lint = ruff["lint"]
    except (KeyError, TypeError, tomllib.TOMLDecodeError) as exc:
        return [], [Finding("malformed-ruff-config", str(exc), path)]

    excludes = _ruff_list_entries(text, "tool.ruff", "extend-exclude")
    ignores = _ruff_list_entries(text, "tool.ruff.lint", "ignore")
    per_file = _ruff_per_file_entries(text)
    records = [
        _ruff_config_record(
            path,
            line,
            Recognition("python", "ruff", value, "ruff extend-exclude", "path-pattern", reason),
        )
        for line, value, reason in excludes
    ]
    records.extend(
        _ruff_config_record(
            path,
            line,
            Recognition("python", "ruff", value, "ruff ignore", "repository", reason),
        )
        for line, value, reason in ignores
    )
    records.extend(
        _ruff_config_record(
            path,
            line,
            Recognition("python", "ruff", rule, "ruff per-file-ignore", f"file:{target}", reason),
        )
        for line, target, rule, reason in per_file
    )

    expected_excludes = ruff.get("extend-exclude", [])
    expected_ignores = lint.get("ignore", [])
    expected_per_file = lint.get("per-file-ignores", {})
    observed_per_file: dict[str, list[str]] = {}
    for _, target, rule, _ in per_file:
        observed_per_file.setdefault(target, []).append(rule)
    if (
        [value for _, value, _ in excludes] != expected_excludes
        or [value for _, value, _ in ignores] != expected_ignores
        or observed_per_file != expected_per_file
    ):
        message = "source-location parser does not cover every active Ruff waiver"
        return records, [Finding("malformed-ruff-config", message, path)]
    return records, []


def _unsupported_counts(path: str, text: str, counts: Counter[str]) -> None:
    """Count hints for declared phase-one gaps without claiming recognition."""
    item = Path(path)
    scanner_dir = Path("scripts") / "checks"
    if item.parent == scanner_dir and (
        item.name.startswith("suppression_") or item.name == "check_suppressions.py"
    ):
        return
    for name, pattern in UNSUPPORTED_CATEGORIES:
        counts[name] += len(pattern.findall(text))


def _append_unsupported(inventory: Inventory, counts: Counter[str]) -> None:
    """Make every known absent recognizer an explicit non-clean check result."""
    if not UNSUPPORTED_CATEGORIES:
        return
    for name, _ in UNSUPPORTED_CATEGORIES:
        count = counts[name]
        message = f"phase-one recognizer absent; {count} unverified text hint(s)"
        inventory.findings.append(Finding("unsupported-category", f"{name}: {message}"))
    message = "phase-one raw hint census excludes its scanner sources to avoid regex self-hits"
    inventory.findings.append(Finding("scanner-self-exemption", message))


def _cppcheck_list(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Parse the central cppcheck suppressions-list format."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    state = CppcheckReasonState()
    for line_no, raw in enumerate(text.splitlines(), start=1):
        stripped = raw.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            note = stripped[1:].strip()
            state.add_comment(note)
            continue
        parts = stripped.split(":", 2)
        rule = parts[0].strip()
        if rule not in KNOWN_CPPCHECK_RULES:
            findings.append(Finding("malformed-cppcheck-list", stripped, path, line_no))
            continue
        target = parts[1].strip() if len(parts) > 1 else "*"
        scope = ":".join(parts[1:]).strip() if len(parts) > 1 else "repository"
        reason = state.reason_for_entry()
        concerns = _concerns(rule, reason)
        records.append(
            Suppression(
                path,
                line_no,
                1,
                "cppcheck",
                "cppcheck",
                rule,
                "suppressions-list",
                scope,
                reason,
                "central-list",
                ownership(path),
                concerns,
            )
        )
        if len(parts) > 1 and not target:
            findings.append(Finding("malformed-cppcheck-list", "empty target", path, line_no))
    if state.in_section:
        findings.append(Finding("malformed-cppcheck-list", "unterminated rationale section", path))
    return records, findings


def _nonvacuity(inventory: Inventory) -> None:
    """Reject collapsed enumeration or recognizer coverage as malformed."""
    if inventory.files_scanned < MIN_REPOSITORY_FILES:
        files = inventory.files_scanned
        message = f"only {files} files enumerated; floor is {MIN_REPOSITORY_FILES}"
        inventory.findings.append(Finding("vacuous-files", message))
    if len(inventory.suppressions) < MIN_SUPPRESSIONS:
        count = len(inventory.suppressions)
        message = f"only {count} suppressions found; floor is {MIN_SUPPRESSIONS}"
        inventory.findings.append(Finding("vacuous-inventory", message))
    seen = {item.family for item in inventory.suppressions}
    exact_expected = set(EXPECTED_EXACT_FAMILY_COUNTS)
    for family in sorted(REQUIRED_FAMILIES - seen - exact_expected):
        inventory.findings.append(Finding("missing-family", family))
    counts = Counter(item.family for item in inventory.suppressions)
    for family, expected in EXPECTED_EXACT_FAMILY_COUNTS.items():
        if counts[family] != expected:
            message = f"{family}={counts[family]}; audited exact contract is {expected}"
            inventory.findings.append(Finding("unexpected-family-count", message))
    for family, floor in MIN_FAMILY_COUNTS.items():
        if counts[family] < floor:
            message = f"{family}={counts[family]}; audited floor is {floor}"
            inventory.findings.append(Finding("vacuous-family", message))


def _governance_count_contracts(inventory: Inventory) -> None:
    """Lock audited live and zero-live semantic governance populations."""
    for label, (attribute, value, expected) in GOVERNANCE_COUNT_CONTRACTS.items():
        count = sum(getattr(item, attribute) == value for item in inventory.suppressions)
        if count != expected:
            message = f"{label}={count}; audited semantic contract is {expected}"
            inventory.findings.append(Finding("unexpected-governance-count", message))


def _scan_lexed_path(
    inventory: Inventory,
    rel: str,
    text: str,
    c_family: bool,
    active_tools: frozenset[str],
) -> None:
    """Run comment, compiler, shell, and C-control recognizers after lexing."""
    comments, findings = extract_comments(rel, text)
    inventory.findings.extend(Finding(item.code, item.message, rel, item.line) for item in findings)
    if c_family:
        records, findings = _scan_raw_nolint(rel, text)
        inventory.suppressions.extend(records)
        inventory.findings.extend(findings)
        # Interior lines of a multi-line block comment look like an unfinished
        # statement, so the detector is told where the comments are.
        comment_lines = frozenset(
            line
            for item in comments
            for line in range(item.line, item.line + item.text.count("\n") + 1)
        )
        inventory.findings.extend(stranded_branch_findings(rel, text, comment_lines))
    records, findings = _scan_comments(rel, comments, active_tools, include_nolint=not c_family)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)
    inventory.suppressions.extend(compiler_records(rel, text))
    if c_family:
        records, findings = scan_c_controls(rel, text, comments)
        inventory.suppressions.extend(records)
        inventory.findings.extend(findings)
    records, findings = shell_status_records(rel, text)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)
    records, findings = yaml_shell_block_status_records(rel, text)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)


def _scan_text_path(
    inventory: Inventory,
    rel: str,
    text: str,
    unsupported: Counter[str],
    active_tools: frozenset[str],
) -> None:
    """Run every syntax recognizer over one decoded repository file."""
    inventory.text_files += 1
    _unsupported_counts(rel, text, unsupported)
    records, findings = scan_c_control_file(rel, text)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)
    first_line = text.partition("\n")[0]
    c_family = language(rel, first_line) == "c-family"
    needs_lexing = (
        not c_family
        or SUPPORTED_HINT_RE.search(text) is not None
        or text.rstrip(" \t\n").endswith("\\")
        or C_CONTROL_HINT_RE.search(text) is not None
    )
    if needs_lexing:
        _scan_lexed_path(inventory, rel, text, c_family, active_tools)
    if rel == ".cppcheck-suppressions":
        records, findings = _cppcheck_list(rel, text)
        inventory.suppressions.extend(records)
        inventory.findings.extend(findings)
    for config_scanner in (_ruff_config_records, scan_tool_configs, scan_tool_sources):
        records, findings = config_scanner(rel, text)
        inventory.suppressions.extend(records)
        inventory.findings.extend(findings)
    records, findings = scan_clang_tidy_config(rel, text)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)
    records, findings = scan_control_file(rel, text)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)

    records, findings = scan_coverage_masks(rel, text)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)


def _scan_repository_governance(inventory: Inventory, root: Path, paths: list[str]) -> None:
    """Run controls whose truth depends on repository-wide bindings/callers."""
    for scanner in (
        scan_ci_parity_exemptions,
        scan_hardware_todo_controls,
        scan_checker_scope_controls,
        scan_checker_nonfatal_controls,
    ):
        records, findings = scanner(root, paths)
        inventory.suppressions.extend(records)
        inventory.findings.extend(findings)


def _vendor_encoding_row(root: Path, rel: str, finding: Finding) -> Suppression | Finding:
    """Bind one vendored undecodable file to its exact blob hash."""
    try:
        blob = hashlib.sha256((root / rel).read_bytes()).hexdigest()
    except OSError as exc:
        return Finding("read-error", str(exc), rel)
    return Suppression(
        rel,
        1,
        1,
        "encoding-exemption",
        "suppression-scanner",
        "invalid-text-encoding",
        "vendor-encoding-exemption",
        f"blob:sha256:{blob}",
        f"vendored legacy text is not decoded or scanned: {finding.message}",
        "vendor-boundary",
        "vendor",
        (),
    )


@dataclass(frozen=True)
class _ScanContext:
    """Per-run scan inputs shared by every candidate path."""

    root: Path
    unsupported: Counter[str]
    active_tools: frozenset[str]
    tracked_paths: frozenset[str]


def _scan_one_path(inventory: Inventory, context: _ScanContext, rel: str) -> None:
    """Read and scan one repository candidate through every recognizer."""
    root = context.root
    text, finding = _read_text(root, rel)
    if finding is not None:
        if finding.code == "invalid-text-encoding" and ownership(rel) == "vendor":
            row = _vendor_encoding_row(root, rel, finding)
            if isinstance(row, Suppression):
                inventory.suppressions.append(row)
                inventory.binary_files += 1
            else:
                inventory.findings.append(row)
            return
        inventory.findings.append(Finding(finding.code, finding.message, rel, finding.line))
        return
    if text is None:
        inventory.binary_files += 1
        return
    _scan_text_path(inventory, rel, text, context.unsupported, context.active_tools)
    records, findings = scan_governance_file(rel, text)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)
    records, findings = generated_records(rel, text, context.tracked_paths, root)
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)


def scan_paths(root: Path, paths: list[str], *, enforce_floors: bool = False) -> Inventory:
    """Scan explicit repo-relative paths through the production recognizers."""
    inventory = Inventory(files_scanned=len(paths))
    try:
        resolved_root = root.resolve(strict=True)
    except OSError as exc:
        inventory.findings.append(Finding("read-error", str(exc)))
        return inventory
    unsupported: Counter[str] = Counter()
    active_tools = configured_optional_tools(paths)
    tracked_paths = frozenset(paths)
    context = _ScanContext(resolved_root, unsupported, active_tools, tracked_paths)
    for rel in paths:
        _scan_one_path(inventory, context, rel)
    if enforce_floors:
        _scan_repository_governance(inventory, resolved_root, paths)
    records, findings = scan_baseline_repository(
        resolved_root, paths, enforce_floors=enforce_floors
    )
    inventory.suppressions.extend(records)
    inventory.findings.extend(findings)
    deduplicate(inventory)
    validate_fingerprints(inventory)
    validate_regions(inventory)
    validate_cppcheck_anchors(inventory, resolved_root)
    assign_identities(inventory, resolved_root)
    _append_unsupported(inventory, unsupported)
    if enforce_floors:
        apply_ledger(inventory, resolved_root)
        _governance_count_contracts(inventory)
        _nonvacuity(inventory)
    return inventory


def scan_repository(root: Path) -> tuple[Inventory, list[str]]:
    """Enumerate and scan the live repository with non-vacuity guards."""
    paths, findings = git_paths(root)
    inventory = scan_paths(root, paths, enforce_floors=True) if paths else Inventory()
    inventory.findings.extend(findings)
    return inventory, paths
