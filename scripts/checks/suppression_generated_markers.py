# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Canonical generated/file-size waiver grammar shared by both gates."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from suppression_catalog import ownership
from suppression_model import Finding, Suppression

HEAD_SCAN_LINES = 40
_COMMENT = r"(?:#|//|/\*+|\*)"
_GENERATED_RE = re.compile(
    rf"^\s*{_COMMENT}\s*@generated\s+by\s+"
    r"(?P<generator>[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)+)\s+"
    r"(?P<reason>\S.*?)\s*(?:\*/)?\s*$",
    re.IGNORECASE,
)
_FILE_SIZE_RE = re.compile(rf"^\s*{_COMMENT}\s*FILE-SIZE-OK\s*:\s*(?P<reason>\S.*?)\s*(?:\*/)?\s*$")
_GENERATED_HINT_RE = re.compile(rf"^\s*{_COMMENT}\s*@generated\b", re.IGNORECASE)
_FILE_SIZE_HINT_RE = re.compile(rf"^\s*{_COMMENT}\s*FILE-SIZE-OK\b", re.IGNORECASE)


@dataclass(frozen=True)
class HeadWaiver:
    """One canonical head marker and its bound provenance."""

    line: int
    kind: str
    generator: str
    reason: str


def parse_head_waivers(text: str) -> tuple[list[HeadWaiver], list[Finding]]:
    """Parse exact standalone comment markers in the first forty lines."""
    waivers: list[HeadWaiver] = []
    findings: list[Finding] = []
    for line_no, raw in enumerate(text.splitlines()[:HEAD_SCAN_LINES], start=1):
        generated = _GENERATED_RE.fullmatch(raw)
        file_size = _FILE_SIZE_RE.fullmatch(raw)
        if generated is not None:
            generator = generated.group("generator")
            normalized = PurePosixPath(generator)
            if normalized.is_absolute() or ".." in normalized.parts:
                findings.append(
                    Finding("malformed-generated-marker", "unsafe generator path", line=line_no)
                )
                continue
            reason = generated.group("reason").strip()
            if not _substantive_reason(reason):
                findings.append(
                    Finding(
                        "non-substantive-waiver-reason",
                        "generated marker rationale must contain a letter or digit",
                        line=line_no,
                    )
                )
            else:
                waivers.append(HeadWaiver(line_no, "generated", generator, reason))
        elif file_size is not None:
            reason = file_size.group("reason").strip()
            if not _substantive_reason(reason):
                findings.append(
                    Finding(
                        "non-substantive-waiver-reason",
                        "file-size rationale must contain a letter or digit",
                        line=line_no,
                    )
                )
            else:
                waivers.append(HeadWaiver(line_no, "file-size", "", reason))
        elif _GENERATED_HINT_RE.match(raw) or _FILE_SIZE_HINT_RE.match(raw):
            findings.append(
                Finding(
                    "malformed-generated-marker",
                    "directive-like waiver does not match the canonical standalone grammar",
                    line=line_no,
                )
            )
    if len(waivers) > 1:
        findings.append(
            Finding(
                "duplicate-file-size-waiver",
                "a file head may carry only one generated/file-size waiver",
                line=waivers[1].line,
            )
        )
    return waivers, findings


def _substantive_reason(reason: str) -> bool:
    """Reject empty and punctuation/delimiter-only waiver rationales."""
    return any(character.isalnum() for character in reason)


def effective_head_waiver(
    text: str,
    *,
    tracked_paths: frozenset[str] | None = None,
    artifact_path: str = "",
) -> tuple[HeadWaiver | None, list[Finding]]:
    """Return the sole valid waiver, rejecting untracked generated provenance."""
    waivers, findings = parse_head_waivers(text)
    if len(waivers) != 1:
        return None, findings
    waiver = waivers[0]
    if waiver.kind == "generated" and artifact_path and waiver.generator == artifact_path:
        findings.append(
            Finding(
                "self-generated-provenance",
                "generated artifact must name a distinct generator",
                line=waiver.line,
            )
        )
        return None, findings
    if waiver.kind == "generated" and not _has_substantive_generated_body(text):
        findings.append(
            Finding(
                "generated-marker-without-body",
                "generated marker has no substantive generated body",
                line=waiver.line,
            )
        )
        return None, findings
    if (
        waiver.kind == "generated"
        and tracked_paths is not None
        and waiver.generator not in tracked_paths
    ):
        findings.append(
            Finding(
                "untracked-generated-provenance",
                f"generator is not tracked: {waiver.generator}",
                line=waiver.line,
            )
        )
        return None, findings
    return waiver, findings


def _has_substantive_generated_body(text: str) -> bool:
    """Require code/data beyond blank lines and standalone comment prose."""
    without_blocks = _without_block_comments(text)
    for raw in without_blocks.splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", "//", "*")):
            continue
        return True
    return False


def _without_block_comments(text: str) -> str:
    """Remove closed or unterminated C block comments without executing a lexer."""
    parts: list[str] = []
    cursor = 0
    while cursor < len(text):
        start = text.find("/*", cursor)
        if start < 0:
            parts.append(text[cursor:])
            break
        parts.append(text[cursor:start])
        end = text.find("*/", start + 2)
        if end < 0:
            break
        cursor = end + 2
    return "".join(parts)


def _same_file_identity(artifact: Path, generator: Path) -> bool:
    """Return whether two resolved paths name one filesystem object."""
    if artifact == generator:
        return True
    try:
        artifact_stat = artifact.stat()
        generator_stat = generator.stat()
    except OSError:
        return False
    return (artifact_stat.st_dev, artifact_stat.st_ino) == (
        generator_stat.st_dev,
        generator_stat.st_ino,
    )


def _generator_identity_finding(
    artifact: Path, generator_rel: str, repo_root: Path
) -> Finding | None:
    """Reject symlinked, missing, out-of-repo, nonregular, and self-identical generators."""
    generator = repo_root / generator_rel
    if generator.is_symlink():
        return Finding(
            "symlinked-generated-provenance",
            f"generator must be a regular repository file, not a symlink: {generator_rel}",
        )
    try:
        resolved = generator.resolve(strict=True)
        resolved.relative_to(repo_root.resolve(strict=True))
        regular = resolved.is_file()
    except (OSError, ValueError):
        resolved = None
        regular = False
    if resolved is None or not regular:
        return Finding(
            "missing-generated-provenance",
            f"generator is not a regular repository file: {generator_rel}",
        )
    if _same_file_identity(artifact.resolve(), resolved):
        return Finding(
            "self-generated-provenance",
            f"generator and artifact share one file identity: {generator_rel}",
        )
    return None


def generated_records(
    path: str,
    text: str,
    tracked_paths: frozenset[str],
    repo_root: Path,
) -> tuple[list[Suppression], list[Finding]]:
    """Inventory one canonical generated artifact, never marker prose/templates."""
    waiver, findings = effective_head_waiver(text, tracked_paths=tracked_paths, artifact_path=path)
    if waiver is None or waiver.kind != "generated":
        return [], [Finding(item.code, item.message, path, item.line) for item in findings]
    identity_problem = _generator_identity_finding(repo_root / path, waiver.generator, repo_root)
    if identity_problem is not None:
        findings.append(Finding(identity_problem.code, identity_problem.message, line=waiver.line))
        return [], [Finding(item.code, item.message, path, item.line) for item in findings]
    record = Suppression(
        path,
        waiver.line,
        1,
        "generated-artifact",
        "file-size",
        "generated-source",
        "@generated by",
        f"file:{path}",
        waiver.reason,
        f"generator:{waiver.generator}",
        ownership(path),
        (),
    )
    located = [Finding(item.code, item.message, path, item.line) for item in findings]
    return [record], located


def path_has_effective_waiver(
    path: Path,
    repo_root: Path,
    tracked_paths: frozenset[str],
) -> bool:
    """Apply the shared grammar for the file-size gate's real filesystem path."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    try:
        artifact_path = (
            path.resolve(strict=True).relative_to(repo_root.resolve(strict=True)).as_posix()
        )
    except ValueError:
        artifact_path = path.name
    except OSError:
        return False
    waiver, findings = effective_head_waiver(
        text, tracked_paths=tracked_paths, artifact_path=artifact_path
    )
    if waiver is None or findings:
        return False
    if waiver.kind == "file-size":
        return True
    return _generator_identity_finding(path, waiver.generator, repo_root) is None
