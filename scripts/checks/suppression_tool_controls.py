# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Recognize formatter, documentation, and tool-specific lint controls."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

import yaml
from suppression_catalog import C_FAMILY_SUFFIXES, ownership
from suppression_comment_lex import Comment
from suppression_model import Finding, Suppression

CLANG_FORMAT_RE = re.compile(r"^clang-format (?P<control>off|on)(?::\s*(?P<reason>\S.*)?)?$")
YAMLLINT_RE = re.compile(
    r"^yamllint (?P<control>disable-file|disable-line|disable|enable)"
    r"(?P<rules>(?: rule:[a-z0-9_-]+)*)$"
)
HADOLINT_RE = re.compile(
    r"^hadolint (?P<global>global )?ignore\s*=\s*"
    r"(?P<rules>(?:DL|SC)\d{4}(?:\s*,\s*(?:DL|SC)\d{4})*)"
    r"(?:\s+#\s*(?P<reason>\S.*))?$"
)
CMAKE_FORMAT_RE = re.compile(r"^(?P<tool>cmake-format|cmf): (?P<control>off|on)(?P<tail>[^\n]*)$")
CMAKE_LINT_RE = re.compile(
    r"^cmake-lint: [ \t]*(?P<options>disable=[A-Z]\d{4}(?:,[A-Z]\d{4})*"
    r"(?:[ \t]+disable=[A-Z]\d{4}(?:,[A-Z]\d{4})*)*)$"
)
PRETTIER_RE = re.compile(r"^prettier-ignore$")
MARKDOWNLINT_RE = re.compile(
    r"^markdownlint-(?P<control>disable-file|enable-file|disable-line|"
    r"disable-next-line|disable|enable|capture|restore)"
    r"(?P<rules>(?: (?:MD\d{3}|[a-z][a-z0-9-]*))*)$"
)
MARKDOWNLINT_CONFIGURE_RE = re.compile(r"^markdownlint-configure-file\s+(?P<config>\{.*\})$")
MARKDOWNLINT_RULE_RE = re.compile(r"^(?:MD\d{3}|[a-z][a-z0-9-]*)$")
DOXYGEN_COND_RE = re.compile(
    r"^(?P<prefix>@|\\)(?:(?P<start>cond)(?:\s+(?P<label>\S+))?|(?P<end>endcond))$"
)
IWYU_RE = re.compile(r"^IWYU pragma: (?P<command>[a-z_]+)(?P<argument>.*)$")

PRETTIER_CONFIGS = frozenset(
    {
        ".prettierrc",
        ".prettierrc.json",
        ".prettierrc.yaml",
        ".prettierrc.yml",
        ".prettierrc.toml",
        "prettier.config.js",
        "prettier.config.cjs",
        "prettier.config.mjs",
    }
)
MARKDOWNLINT_CONFIG_PREFIXES = (".markdownlint", ".markdownlint-cli2")
DOXYGEN_INPUT_ROOTS = frozenset(
    {"apps", "coprocessor", "docs", "examples", "libs", "port", "scripts", "tools"}
)
DOXYGEN_SUFFIXES = frozenset({".c", ".h", ".cpp", ".hpp", ".md", ".dox", ".py"})
DOXYGEN_EXCLUDED_PARTS = frozenset(
    {"build", "docs/doxygen", "docs/doxygen_theme", "tests", "third_party"}
)
IWYU_NO_ARGUMENT = frozenset(
    {
        "always_keep",
        "associated",
        "begin_exports",
        "begin_keep",
        "end_exports",
        "end_keep",
        "export",
        "keep",
    }
)
IWYU_QUOTED_ARGUMENT = frozenset({"friend", "no_forward_declare"})


def _concerns(rule: str, reason: str, *, reason_required: bool) -> tuple[str, ...]:
    """Return review concerns for one recognized tool control."""
    concerns: list[str] = []
    if rule == "*":
        concerns.append("broad-rule")
    if reason_required and not reason:
        concerns.append("blank-reason")
    return tuple(concerns)


@dataclass(frozen=True)
class ToolRecognition:
    """Normalized fields produced by one tool-control recognizer."""

    family: str
    tool: str
    rule: str
    directive: str
    scope: str
    reason: str
    provenance: str = "inline-comment"
    reason_required: bool = True


def _row(path: str, line: int, column: int, item: ToolRecognition) -> Suppression:
    """Build one normalized tool-control row."""
    owner = ownership(path)
    return Suppression(
        path,
        line,
        column,
        item.family,
        item.tool,
        item.rule,
        item.directive,
        item.scope,
        item.reason,
        item.provenance,
        owner,
        _concerns(
            item.rule,
            item.reason,
            reason_required=item.reason_required and owner != "vendor",
        ),
    )


def _is_c_family(path: str) -> bool:
    """Return whether clang-format, Doxygen, and IWYU comments are active."""
    return Path(path).suffix.lower() in C_FAMILY_SUFFIXES


def _is_cmake(path: str) -> bool:
    """Return whether a path is parsed by cmakelang."""
    item = Path(path)
    return item.name == "CMakeLists.txt" or item.suffix.lower() == ".cmake"


def _is_dockerfile(path: str) -> bool:
    """Return whether a path uses Dockerfile comment directives."""
    name = Path(path).name
    return name == "Dockerfile" or name.startswith("Dockerfile.")


def _is_doxygen_input(path: str) -> bool:
    """Return whether Doxyfile parses this first-party source path."""
    item = Path(path)
    if item.suffix.lower() not in DOXYGEN_SUFFIXES:
        return False
    if path != "README.md" and (not item.parts or item.parts[0] not in DOXYGEN_INPUT_ROOTS):
        return False
    normalized = path.replace("\\", "/")
    return not any(part.startswith("build-") for part in item.parts) and not any(
        part in item.parts or normalized.startswith(f"{part}/") for part in DOXYGEN_EXCLUDED_PARTS
    )


def _is_doxygen_control_path(path: str) -> bool:
    """Return whether Doxygen syntax is authored here or vendor-owned."""
    return _is_doxygen_input(path) or (
        ownership(path) == "vendor" and Path(path).suffix.lower() in DOXYGEN_SUFFIXES
    )


def configured_optional_tools(paths: list[str]) -> frozenset[str]:
    """Return optional comment-driven tools configured by the scanned tree."""
    names = {Path(path).name for path in paths}
    active: set[str] = set()
    if names & PRETTIER_CONFIGS:
        active.add("prettier")
    if any(name.startswith(MARKDOWNLINT_CONFIG_PREFIXES) for name in names):
        active.add("markdownlint")
    return frozenset(active)


def _clang_format(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize a real clang-format region delimiter in C-family source."""
    if not _is_c_family(path) or (match := CLANG_FORMAT_RE.fullmatch(body)) is None:
        return None
    control = match.group("control").lower()
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "formatter",
            "clang-format",
            "layout",
            f"clang-format {control}",
            "region-start" if control == "off" else "region-end",
            match.group("reason") or "",
            reason_required=control == "off",
        ),
    )


def _yamllint(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize an active yamllint inline control in YAML source."""
    if Path(path).suffix.lower() not in {".yaml", ".yml"}:
        return None
    if (match := YAMLLINT_RE.fullmatch(body)) is None:
        return None
    control = match.group("control")
    rule_tokens = re.findall(r"rule:([a-z0-9_-]+)", match.group("rules"))
    rules = ",".join(sorted(rule_tokens)) or "*"
    if control == "disable-file" and comment.line != 1:
        return None
    scope = {
        "disable-file": "file",
        "disable-line": "line",
        "disable": "region-start",
        "enable": "region-end",
    }[control]
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "lint-control",
            "yamllint",
            rules,
            f"yamllint {control}",
            scope,
            "",
            reason_required=control != "enable",
        ),
    )


def _hadolint(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize an active Hadolint line or file ignore in a Dockerfile."""
    if not _is_dockerfile(path) or (match := HADOLINT_RE.fullmatch(body)) is None:
        return None
    scope = "file" if match.group("global") else "next-instruction"
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "lint-control",
            "hadolint",
            re.sub(r"\s+", "", match.group("rules")).upper(),
            "hadolint ignore",
            scope,
            match.group("reason") or "",
        ),
    )


def _cmake_format(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize a real cmake-format/cmf region delimiter."""
    if not _is_cmake(path) or (match := CMAKE_FORMAT_RE.fullmatch(body)) is None:
        return None
    control = match.group("control")
    reason = match.group("tail").strip()
    if reason.startswith("--"):
        reason = reason.removeprefix("--").strip()
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "formatter",
            "cmake-format",
            "layout",
            f"{match.group('tool')} {control}",
            "region-start" if control == "off" else "region-end",
            reason,
            reason_required=control == "off",
        ),
    )


def _cmake_lint(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize cmake-lint's sole inline pragma: disable=<codes>."""
    if not _is_cmake(path) or (match := CMAKE_LINT_RE.fullmatch(body)) is None:
        return None
    rules = sorted(
        rule
        for option in match.group("options").split()
        for rule in option.removeprefix("disable=").split(",")
    )
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "lint-control",
            "cmake-lint",
            ",".join(rules),
            "cmake-lint disable",
            "enclosing-block",
            "",
        ),
    )


def _markdownlint_config(body: str) -> dict[str, object] | None:
    """Parse one valid markdownlint-configure-file JSON object."""
    if (match := MARKDOWNLINT_CONFIGURE_RE.fullmatch(body)) is None:
        return None
    try:
        config = json.loads(match.group("config"))
    except json.JSONDecodeError:
        return None
    valid = isinstance(config, dict) and all(
        MARKDOWNLINT_RULE_RE.fullmatch(rule) is not None and isinstance(value, (bool, dict))
        for rule, value in config.items()
    )
    return config if valid else None


def valid_tool_control(path: str, body: str, active_tools: frozenset[str] = frozenset()) -> bool:
    """Return whether directive-like text is a valid configured control."""
    return (
        Path(path).suffix.lower() == ".md"
        and "markdownlint" in active_tools
        and _markdownlint_config(body) is not None
    )


def _markdownlint_configure(
    path: str,
    comment: Comment,
    body: str,
    active_tools: frozenset[str],
) -> Suppression | None:
    """Inventory disabled rules and non-empty option relaxations."""
    if "markdownlint" not in active_tools or (config := _markdownlint_config(body)) is None:
        return None
    relaxed = sorted(
        rule
        for rule, value in config.items()
        if value is False or (isinstance(value, dict) and bool(value))
    )
    if not relaxed:
        return None
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "lint-control",
            "markdownlint",
            ",".join(relaxed),
            "markdownlint configure-file",
            "file",
            "",
        ),
    )


def _prettier_markdown(
    path: str, comment: Comment, body: str, active_tools: frozenset[str]
) -> Suppression | None:
    """Recognize configured Markdown-only Prettier/markdownlint controls."""
    if Path(path).suffix.lower() != ".md":
        return None
    if "prettier" in active_tools and PRETTIER_RE.fullmatch(body) is not None:
        return _row(
            path,
            comment.line,
            comment.column,
            ToolRecognition(
                "formatter",
                "prettier",
                "layout",
                "prettier-ignore",
                "next-node",
                "",
            ),
        )
    if configured := _markdownlint_configure(path, comment, body, active_tools):
        return configured
    if "markdownlint" not in active_tools or (match := MARKDOWNLINT_RE.fullmatch(body)) is None:
        return None
    control = match.group("control")
    if control in {"capture", "restore"} and match.group("rules"):
        return None
    rules = ",".join(match.group("rules").split()) or "*"
    reason_required = control.startswith("disable")
    scope = (
        "file"
        if control.endswith("-file")
        else "line"
        if control.endswith("-line")
        else "next-line"
        if control == "disable-next-line"
        else "region-start"
        if control == "disable"
        else "region-end"
        if control == "enable"
        else "state-capture"
        if control == "capture"
        else "state-restore"
        if control == "restore"
        else "state"
    )
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "lint-control",
            "markdownlint",
            rules,
            f"markdownlint {control}",
            scope,
            "",
            reason_required=reason_required,
        ),
    )


def _doxygen(
    path: str, comment: Comment, body: str, *, plain_source: bool = False
) -> Suppression | None:
    """Recognize a Doxygen conditional-documentation region delimiter."""
    suffix = Path(path).suffix.lower()
    if not _is_doxygen_control_path(path) or (suffix == ".md" and not plain_source):
        return None
    if suffix == ".py":
        if not body.startswith("#"):
            return None
        body = body[1:].strip()
    if (match := DOXYGEN_COND_RE.fullmatch(body)) is None:
        return None
    control = "cond" if match.group("start") else "endcond"
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "documentation",
            "doxygen",
            "conditional-doc",
            f"doxygen {control}",
            "region-start" if control == "cond" else "region-end",
            "",
            reason_required=control == "cond",
        ),
    )


def _valid_iwyu_argument(command: str, argument: str) -> bool:
    """Validate the documented argument form for one case-sensitive pragma."""
    if command in IWYU_NO_ARGUMENT:
        return not argument
    if command in IWYU_QUOTED_ARGUMENT:
        return re.fullmatch(r' "[^"\n]+"', argument) is not None
    if command == "no_include":
        return re.fullmatch(r' (?:"[^"\n]+"|<[^>\n]+>)', argument) is not None
    if command == "private":
        return not argument or (
            re.fullmatch(r', include (?:"[^"\n]+"|<[^>\n]+>)', argument) is not None
        )
    return False


def _iwyu(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize an include-what-you-use pragma in C-family source."""
    if not _is_c_family(path) or (match := IWYU_RE.fullmatch(body)) is None:
        return None
    command = match.group("command")
    if not _valid_iwyu_argument(command, match.group("argument")):
        return None
    scope = {
        "begin_exports": "region-start",
        "end_exports": "region-end",
        "begin_keep": "region-start",
        "end_keep": "region-end",
    }.get(
        command,
        "file"
        if command in {"always_keep", "friend", "no_forward_declare", "no_include", "private"}
        else "line",
    )
    rule = (
        "exports"
        if command in {"begin_exports", "end_exports"}
        else "keep-region"
        if command in {"begin_keep", "end_keep"}
        else command
    )
    return _row(
        path,
        comment.line,
        comment.column,
        ToolRecognition(
            "include-analysis",
            "iwyu",
            rule,
            f"IWYU pragma: {command}",
            scope,
            "",
            reason_required=scope != "region-end",
        ),
    )


COMMENT_RECOGNIZERS = (
    _clang_format,
    _yamllint,
    _hadolint,
    _cmake_format,
    _cmake_lint,
    _doxygen,
    _iwyu,
)


def recognize_tool_comment(
    path: str,
    comment: Comment,
    body: str,
    active_tools: frozenset[str] = frozenset(),
) -> Suppression | None:
    """Recognize one tool control from a syntax-extracted comment."""
    found = next((item for fn in COMMENT_RECOGNIZERS if (item := fn(path, comment, body))), None)
    return found or _prettier_markdown(path, comment, body, active_tools)


def tool_control_finding(
    path: str,
    comment: Comment,
    body: str,
    active_tools: frozenset[str] = frozenset(),
) -> Finding | None:
    """Fail closed on directive-like text that is not active tool syntax."""
    if Path(path).suffix.lower() == ".py" and body.startswith("#"):
        body = body[1:].strip()
    folded = body.casefold()
    if valid_tool_control(path, body, active_tools):
        return None
    if folded == "prettier-ignore" and "prettier" not in active_tools:
        return Finding("inactive-tool-control", "Prettier is not configured", path, comment.line)
    if folded.startswith("markdownlint-") and "markdownlint" not in active_tools:
        return Finding(
            "inactive-tool-control", "markdownlint is not configured", path, comment.line
        )
    directive_like = (
        re.match(r"^clang-format\s+(?:off|on)(?:\s|:|$)", body, re.IGNORECASE)
        or re.match(
            r"^yamllint\s+(?:disable-file|disable-line|disable|enable)(?:\s|$)",
            body,
            re.IGNORECASE,
        )
        or re.match(r"^hadolint\s+(?:global\s+)?ignore\s*=", body, re.IGNORECASE)
        or re.match(r"^(?:cmake-format|cmf)\s*:", body, re.IGNORECASE)
        or re.match(r"^cmake-lint\s*:", body, re.IGNORECASE)
        or re.match(r"^shfmt\s*:", body, re.IGNORECASE)
        or re.match(r"^prettier-ignore(?:\s|$)", body, re.IGNORECASE)
        or re.match(
            r"^markdownlint-(?:disable-file|enable-file|disable-line|"
            r"disable-next-line|disable|enable|capture|restore|configure-file)(?:\s|$)",
            body,
            re.IGNORECASE,
        )
        or re.match(r"^IWYU\s+pragma\s*:", body, re.IGNORECASE)
        or (
            _is_doxygen_control_path(path)
            and Path(path).suffix.lower() != ".md"
            and re.match(r"^(?:@|\\)(?:cond|endcond)(?:\s|$)", body, re.IGNORECASE)
        )
    )
    if directive_like:
        return Finding("malformed-tool-control", body, path, comment.line)
    return None


def _markdown_visible_lines(text: str) -> list[str]:
    """Blank Markdown fenced code while preserving line numbering."""
    visible: list[str] = []
    fence = ""
    html_comment = False
    for raw in text.splitlines():
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})", raw)
        if fence:
            if re.match(rf"^ {{0,3}}{re.escape(fence[0])}{{{len(fence)},}}\s*$", raw):
                fence = ""
            visible.append("")
        elif html_comment:
            html_comment = "-->" not in raw
            visible.append("")
        elif marker:
            fence = marker.group(1)
            visible.append("")
        elif "<!--" in raw:
            html_comment = "-->" not in raw[raw.find("<!--") + 4 :]
            visible.append("")
        else:
            visible.append(raw)
    return visible


def _plain_source_reason(path: str, previous: str) -> str:
    """Extract a precise rationale immediately above a plain Doxygen command."""
    stripped = previous.strip()
    if Path(path).suffix.lower() == ".md":
        match = re.fullmatch(r"<!--\s*Suppression rationale:\s*(\S.*?)\s*-->", stripped)
    else:
        match = re.fullmatch(
            r"(?:/\*\*?|//)\s*Suppression rationale:\s*(\S.*?)(?:\s*\*/)?",
            stripped,
        )
    return match.group(1) if match else ""


def scan_tool_sources(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory Doxygen conditionals in its plain Markdown and dox inputs."""
    if not _is_doxygen_input(path) or Path(path).suffix.lower() not in {".md", ".dox"}:
        return [], []
    source_lines = text.splitlines()
    lines = _markdown_visible_lines(text) if path.endswith(".md") else source_lines
    records: list[Suppression] = []
    findings: list[Finding] = []
    for line_no, raw in enumerate(lines, start=1):
        body = raw.strip()
        if not body:
            continue
        comment = Comment(line_no, len(raw) - len(raw.lstrip()) + 1, body)
        if (record := _doxygen(path, comment, body, plain_source=True)) is not None:
            reason = _plain_source_reason(path, source_lines[line_no - 2]) if line_no > 1 else ""
            if reason:
                record = Suppression(
                    record.path,
                    record.line,
                    record.column,
                    record.family,
                    record.tool,
                    record.rule,
                    record.directive,
                    record.scope,
                    reason,
                    record.provenance,
                    record.owner,
                    _concerns(
                        record.rule,
                        reason,
                        reason_required=record.scope == "region-start",
                    ),
                )
            records.append(record)
        elif body.casefold().startswith(("@cond", "@endcond", "\\cond", "\\endcond")):
            findings.append(Finding("malformed-tool-control", body, path, line_no))
    return records, findings


def _preceding_reason(lines: list[str], line: int) -> str:
    """Collect the contiguous explanatory comment block above one config row."""
    notes: list[str] = []
    for raw in reversed(lines[: line - 1]):
        stripped = raw.strip()
        if not stripped:
            if notes:
                break
            continue
        if not stripped.startswith("#"):
            break
        note = stripped[1:].strip()
        if note:
            notes.append(note)
    return " ".join(reversed(notes))


def _line_for(lines: list[str], pattern: re.Pattern[str], start: int = 1) -> int:
    """Return the first matching one-based source line at or after start."""
    for line_no, raw in enumerate(lines[start - 1 :], start=start):
        if pattern.fullmatch(raw):
            return line_no
    return 0


def _yaml_config(path: str, text: str) -> tuple[object, list[Finding]]:
    """Parse one YAML tool config and fail closed on malformed data."""
    try:
        return yaml.safe_load(text), []
    except yaml.YAMLError as exc:
        return None, [Finding("malformed-tool-config", str(exc), path)]


def _yamllint_disabled_record(
    path: str, rule: object, lines: list[str], findings: list[Finding]
) -> Suppression | None:
    """Build one source-located disabled-rule row."""
    pattern = re.compile(rf"^\s*{re.escape(str(rule))}:\s*false\s*(?:#.*)?$")
    line = _line_for(lines, pattern)
    if not line:
        findings.append(Finding("malformed-tool-config", f"cannot locate {rule}: false", path))
        return None
    return _row(
        path,
        line,
        1,
        ToolRecognition(
            "lint-control",
            "yamllint",
            str(rule),
            "yamllint rule disabled",
            "repository",
            _preceding_reason(lines, line),
            provenance="central-config",
        ),
    )


def _yamllint_truthy_record(
    path: str, truthy: object, lines: list[str], findings: list[Finding]
) -> Suppression | None:
    """Build the source-located truthy-key narrowing row when active."""
    if not isinstance(truthy, dict) or truthy.get("check-keys") is not False:
        return None
    line = _line_for(lines, re.compile(r"^\s*check-keys:\s*false\s*(?:#.*)?$"))
    if not line:
        message = "cannot locate truthy check-keys: false"
        findings.append(Finding("malformed-tool-config", message, path))
        return None
    reason = _preceding_reason(lines, line)
    if not reason:
        parent_line = _line_for(lines, re.compile(r"^\s*truthy:\s*(?:#.*)?$"))
        if not parent_line:
            findings.append(Finding("malformed-tool-config", "cannot locate truthy rule", path))
        else:
            reason = _preceding_reason(lines, parent_line)
    return _row(
        path,
        line,
        1,
        ToolRecognition(
            "lint-control",
            "yamllint",
            "truthy/check-keys",
            "yamllint check-keys false",
            "repository",
            reason,
            provenance="central-config",
        ),
    )


def _yamllint_config(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory disabled yamllint rules and narrowed truthy-key checking."""
    if path != ".yamllint.yaml":
        return [], []
    parsed, findings = _yaml_config(path, text)
    if findings:
        return [], findings
    if not isinstance(parsed, dict) or not isinstance(parsed.get("rules"), dict):
        return [], [Finding("malformed-tool-config", "yamllint rules table missing", path)]
    lines = text.splitlines()
    rules = parsed["rules"]
    records = [
        record
        for rule, config in rules.items()
        if config is False
        if (record := _yamllint_disabled_record(path, rule, lines, findings)) is not None
    ]
    if (record := _yamllint_truthy_record(path, rules.get("truthy"), lines, findings)) is not None:
        records.append(record)
    return records, findings


def _hadolint_config(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory every central Hadolint ignored rule with source rationale."""
    if path != ".hadolint.yaml":
        return [], []
    parsed, findings = _yaml_config(path, text)
    if findings:
        return [], findings
    ignored = parsed.get("ignored", []) if isinstance(parsed, dict) else None
    if not isinstance(ignored, list) or not all(
        isinstance(rule, str) and re.fullmatch(r"(?:DL|SC)\d{4}", rule) for rule in ignored
    ):
        return [], [Finding("malformed-tool-config", "invalid hadolint ignored list", path)]
    lines = text.splitlines()
    records: list[Suppression] = []
    for rule in ignored:
        pattern = re.compile(rf"^\s*-\s*{re.escape(rule)}\s*(?:#.*)?$")
        line = _line_for(lines, pattern)
        if not line:
            findings.append(Finding("malformed-tool-config", f"cannot locate {rule}", path))
            continue
        reason = _preceding_reason(lines, line)
        records.append(
            _row(
                path,
                line,
                1,
                ToolRecognition(
                    "lint-control",
                    "hadolint",
                    rule,
                    "hadolint ignored",
                    "repository",
                    reason,
                    provenance="central-config",
                ),
            )
        )
    return records, findings


def _editorconfig(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory shfmt's real ignore mechanism: EditorConfig path sections."""
    if Path(path).name != ".editorconfig":
        return [], []
    lines = text.splitlines()
    section = ""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for line_no, raw in enumerate(lines, start=1):
        stripped = raw.strip()
        if re.fullmatch(r"\[[^\]]+\]|\[\[(?:shell|bash|zsh)\]\]", stripped):
            section = stripped
            continue
        match = re.fullmatch(
            r"(?i:ignore)\s*=\s*(?P<value>true|false|unset)(?:\s*[#;].*)?", stripped
        )
        if match is not None and match.group("value") == "true":
            if not section:
                findings.append(
                    Finding("malformed-tool-config", "shfmt ignore has no section", path, line_no)
                )
                continue
            records.append(
                _row(
                    path,
                    line_no,
                    1,
                    ToolRecognition(
                        "formatter",
                        "shfmt",
                        section,
                        "EditorConfig ignore=true",
                        "path-pattern",
                        _preceding_reason(lines, line_no),
                        provenance="central-config",
                    ),
                )
            )
        elif match is None and re.match(r"^ignore\s*=", stripped, re.IGNORECASE):
            findings.append(
                Finding(
                    "malformed-tool-config", f"inactive shfmt property: {stripped}", path, line_no
                )
            )
    return records, findings


def scan_tool_configs(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory supported repository-global tool-control files."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for parser in (_yamllint_config, _hadolint_config, _editorconfig):
        parsed_records, parsed_findings = parser(path, text)
        records.extend(parsed_records)
        findings.extend(parsed_findings)
    return records, findings
