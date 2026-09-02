# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Syntax-aware compiler and allocation suppression inventory."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from check_no_dynamic_alloc import allocation_symbols
from suppression_catalog import ALLOC_ALLOW_RE, C_FAMILY_SUFFIXES, ownership
from suppression_comment_lex import Comment, extract_comments
from suppression_model import Finding, Suppression

PRAGMA_HINT_RE = re.compile(
    r"^\s*#\s*pragma\s+(?P<tool>GCC|clang)\s+diagnostic\b",
    re.IGNORECASE,
)
PRAGMA_KIND_RE = re.compile(
    r"^\s*#\s*pragma\s+(?P<tool>GCC|clang)\s+diagnostic\s+"
    r"(?P<kind>push|pop|ignored|warning|error)\b(?P<tail>.*)$"
)
PRAGMA_CONTROL_RE = re.compile(
    r"^\s*#\s*pragma\s+(?P<tool>GCC|clang)\s+diagnostic\s+"
    r'(?P<kind>ignored|warning|error)\s+"(?P<flag>-W[A-Za-z0-9_.=+\-]+)"\s*'
    r"(?:(?://.*)|(?:/\*.*\*/\s*))?$"
)
PRAGMA_OPERATOR_HINT_RE = re.compile(r"\b_Pragma\b")
PRAGMA_OPERATOR_RE = re.compile(r'\b_Pragma\s*\(\s*"(?P<payload>(?:\\.|[^"\\])*)"\s*\)')
PRAGMA_PAYLOAD_HINT_RE = re.compile(
    r"^(?:GCC|clang)\s+diagnostic\b",
    re.IGNORECASE,
)
PRAGMA_PAYLOAD_KIND_RE = re.compile(
    r"^(?P<tool>GCC|clang)\s+diagnostic\s+"
    r"(?P<kind>push|pop|ignored|warning|error)\b(?P<tail>.*)$"
)
PRAGMA_PAYLOAD_CONTROL_RE = re.compile(
    r"^(?P<tool>GCC|clang)\s+diagnostic\s+(?P<kind>ignored|warning|error)\s+"
    r'"(?P<flag>-W[A-Za-z0-9_.=+\-]+)"$'
)
ATTRIBUTE_BLOCK_RE = re.compile(r"\[\[(?P<body>.*?)\]\]", re.DOTALL)
ATTRIBUTE_CONTROL_HINT_RE = re.compile(
    r"\bmaybe_unused\b|\bgnu::(?:__)?unused(?:__)?\b|\bclang::no_sanitize\b"
)
GNU_ATTRIBUTE_HINT_RE = re.compile(r"\b__attribute(?:__)?\b")
GNU_CONTROL_HINT_RE = re.compile(
    r"\b(?:__)?unused(?:__)?\b|\b(?:__)?no_sanitize(?:_[A-Za-z0-9_]+)?(?:__)?\b"
)
GNU_UNUSED_ITEM_RE = re.compile(r"^(?:__)?unused(?:__)?$")
GNU_SANITIZER_CALL_RE = re.compile(
    r"^(?:__)?no_sanitize(?:__)?\s*\((?P<args>.*)\)$",
    re.DOTALL,
)
GNU_SANITIZER_SUFFIX_RE = re.compile(
    r"^(?:__)?no_sanitize_(?P<rule>address|memory|thread|undefined)(?:__)?$"
)
STANDARD_MAYBE_RE = re.compile(r"^maybe_unused$")
STANDARD_GNU_UNUSED_RE = re.compile(r"^gnu::(?:__)?unused(?:__)?$")
STANDARD_CLANG_SANITIZER_RE = re.compile(
    r"^clang::no_sanitize\s*\((?P<args>.*)\)$",
    re.DOTALL,
)
SANITIZER_ARGS_RE = re.compile(
    r'^\s*"[A-Za-z0-9_.+\-]+"(?:\s*,\s*"[A-Za-z0-9_.+\-]+")*\s*$',
    re.DOTALL,
)
SANITIZER_NAME_RE = re.compile(r'"([A-Za-z0-9_.+\-]+)"')
NASA_RE = re.compile(r"\bRA8_NASA_RULE_3_OK\b")
NASA_CALL_RE = re.compile(r'RA8_NASA_RULE_3_OK\s*\(\s*"(?P<reason>(?:\\.|[^"\\])+)"\s*\)')
ALLOC_HINT_RE = re.compile(r"\balloc-allow\b", re.IGNORECASE)
C_HINT_RE = re.compile(
    r"#\s*pragma\s+(?:GCC|clang)\s+diagnostic|\b_Pragma\b|"
    r"__attribute(?:__)?|\[\[\s*(?:maybe_unused|gnu::(?:__)?unused|"
    r"clang::no_sanitize)|RA8_NASA_RULE_3_OK|alloc-allow",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class LogicalLine:
    """One backslash-spliced preprocessing line with its source location."""

    line: int
    offset: int
    code: str
    raw: str


@dataclass(frozen=True)
class ControlSpec:
    """Normalized identity fields for one parsed C-family control."""

    family: str
    tool: str
    rule: str
    directive: str
    scope: str
    provenance: str


@dataclass
class PragmaScan:
    """Mutable state shared by one diagnostic pragma scan."""

    path: str
    comments: list[Comment]
    records: list[Suppression]
    findings: list[Finding]
    depth: dict[str, int]


def _blank(out: list[str], raw: str, start: int, end: int) -> None:
    """Blank one non-code span while preserving newlines and offsets."""
    for index in range(start, min(end, len(raw))):
        if raw[index] not in "\r\n":
            out[index] = " "


def _raw_string(raw: str, index: int) -> tuple[str, int] | None:
    """Return a C++ raw-string terminator and body start at one offset."""
    if index > 0 and (raw[index - 1].isalnum() or raw[index - 1] == "_"):
        return None
    match = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t]{0,16})\(', raw[index:])
    if match is None:
        return None
    return ")" + match.group(1) + '"', index + match.end()


def _code_view(raw: str) -> str:
    """Blank C comments and literals without changing source coordinates."""
    out = list(raw)
    index = 0
    while index < len(raw):
        raw_string = _raw_string(raw, index)
        if raw_string is not None:
            terminator, body = raw_string
            end = raw.find(terminator, body)
            stop = len(raw) if end < 0 else end + len(terminator)
            _blank(out, raw, index, stop)
            index = stop
            continue
        if raw.startswith("//", index):
            end = raw.find("\n", index)
            stop = len(raw) if end < 0 else end
            _blank(out, raw, index, stop)
            index = stop
            continue
        if raw.startswith("/*", index):
            end = raw.find("*/", index + 2)
            stop = len(raw) if end < 0 else end + 2
            _blank(out, raw, index, stop)
            index = stop
            continue
        if raw[index] in {'"', "'"}:
            quote = raw[index]
            start = index
            index += 1
            while index < len(raw):
                if raw[index] == "\\" and index + 1 < len(raw):
                    index += 2
                    continue
                if raw[index] == quote:
                    index += 1
                    break
                if raw[index] == "\n":
                    break
                index += 1
            _blank(out, raw, start, index)
            continue
        index += 1
    return "".join(out)


def _logical_lines(raw: str, code: str) -> list[LogicalLine]:
    """Join preprocessing line splices while retaining the first location."""
    raw_lines = raw.splitlines(keepends=True)
    code_lines = code.splitlines(keepends=True)
    result: list[LogicalLine] = []
    offset = 0
    index = 0
    while index < len(code_lines):
        start = index
        raw_parts = [raw_lines[index]]
        code_parts = [code_lines[index]]
        while code_parts[-1].rstrip("\r\n").endswith("\\") and index + 1 < len(code_lines):
            index += 1
            raw_parts.append(raw_lines[index])
            code_parts.append(code_lines[index])
        joined_raw = re.sub(r"\\\r?\n", "", "".join(raw_parts))
        joined_code = re.sub(r"\\\r?\n", "", "".join(code_parts))
        result.append(LogicalLine(start + 1, offset, joined_code.rstrip(), joined_raw.rstrip()))
        offset += sum(len(part) for part in code_lines[start : index + 1])
        index += 1
    return result


def _line_column(raw: str, offset: int) -> tuple[int, int]:
    """Convert a source offset to a one-based line and column."""
    line = raw.count("\n", 0, offset) + 1
    start = raw.rfind("\n", 0, offset)
    return line, offset - start


def _concerns(path: str, rule: str, reason: str, *, required: bool) -> tuple[str, ...]:
    """Return first-party review concerns for one narrow control."""
    if ownership(path) != "first-party":
        return ()
    concerns: list[str] = []
    if required and not reason:
        concerns.append("blank-reason")
    if rule in {"*", "all", "-W"}:
        concerns.append("broad-rule")
    return tuple(concerns)


def _suppression(
    path: str,
    location: tuple[int, int],
    spec: ControlSpec,
    reason: str,
    *,
    reason_required: bool,
) -> Suppression:
    """Build one compiler-control row with ownership-aware review policy."""
    line, column = location
    return Suppression(
        path,
        line,
        column,
        spec.family,
        spec.tool,
        spec.rule,
        spec.directive,
        spec.scope,
        reason,
        spec.provenance,
        ownership(path),
        _concerns(path, spec.rule, reason, required=reason_required),
    )


def _comment_reason(comments: list[Comment], line: int) -> str:
    """Return a local diagnostic-pragmas rationale on one source line."""
    for comment in comments:
        if comment.line not in {line - 1, line}:
            continue
        text = comment.text.strip().lstrip("*").strip()
        if text.startswith("Suppression rationale:"):
            return text.removeprefix("Suppression rationale:").strip()
    return ""


def _record_pragma_region(
    scan: PragmaScan,
    logical: LogicalLine,
    tool: str,
    kind: str,
    column: int,
) -> None:
    """Record one exact diagnostic push/pop and update its tool-specific depth."""
    scope = "region-start" if kind == "push" else "region-end"
    spelling = "#pragma" if logical.raw.lstrip().startswith("#") else "_Pragma"
    spec = ControlSpec(
        "compiler",
        tool,
        "diagnostic-state",
        f"{spelling} {tool} diagnostic {kind}",
        scope,
        "compiler-pragma",
    )
    scan.records.append(
        _suppression(
            scan.path,
            (logical.line, column),
            spec,
            "",
            reason_required=False,
        )
    )
    if kind == "push":
        scan.depth[tool] += 1
    elif scan.depth[tool] == 0:
        scan.findings.append(Finding("unmatched-diagnostic-pop", tool, scan.path, logical.line))
    else:
        scan.depth[tool] -= 1


def _record_pragma_control(
    scan: PragmaScan,
    logical: LogicalLine,
    tool: str,
    kind: str,
    column: int,
) -> None:
    """Record one exact warning control or fail closed on malformed grammar."""
    control = PRAGMA_CONTROL_RE.fullmatch(logical.raw)
    if control is None:
        control = PRAGMA_PAYLOAD_CONTROL_RE.fullmatch(logical.raw)
    if control is None:
        scan.findings.append(
            Finding(
                "malformed-diagnostic-pragma",
                logical.raw.strip(),
                scan.path,
                logical.line,
            )
        )
        return
    flag = control.group("flag")
    spelling = "#pragma" if logical.raw.lstrip().startswith("#") else "_Pragma"
    reason = _comment_reason(scan.comments, logical.line)
    spec = ControlSpec(
        "compiler",
        tool,
        flag,
        f"{spelling} {tool} diagnostic {kind}",
        "diagnostic-state",
        "compiler-pragma",
    )
    scan.records.append(
        _suppression(
            scan.path,
            (logical.line, column),
            spec,
            reason,
            reason_required=kind == "ignored",
        )
    )
    if scan.depth[tool] == 0:
        scan.findings.append(Finding("unscoped-diagnostic-control", tool, scan.path, logical.line))


def _destringize_pragma(payload: str) -> str | None:
    """Apply the C _Pragma string-literal destringization rules."""
    if re.search(r'\\(?!["\\])', payload):
        return None
    return payload.replace('\\"', '"').replace("\\\\", "\\")


def _scan_pragma_operator_line(scan: PragmaScan, logical: LogicalLine) -> None:
    """Parse every diagnostic _Pragma operator on one logical source line."""
    for hint in PRAGMA_OPERATOR_HINT_RE.finditer(logical.code):
        tail = logical.raw[hint.start() :]
        operator = PRAGMA_OPERATOR_RE.match(logical.raw, hint.start())
        if operator is None:
            if re.search(r"(?:GCC|clang)\s+diagnostic", tail, re.IGNORECASE):
                scan.findings.append(
                    Finding("malformed-diagnostic-pragma", tail.strip(), scan.path, logical.line)
                )
            continue
        payload = _destringize_pragma(operator.group("payload"))
        if payload is None:
            if re.search(r"(?:GCC|clang)\s+diagnostic", tail, re.IGNORECASE):
                scan.findings.append(
                    Finding("malformed-diagnostic-pragma", tail.strip(), scan.path, logical.line)
                )
            continue
        if PRAGMA_PAYLOAD_HINT_RE.match(payload) is None:
            continue
        match = PRAGMA_PAYLOAD_KIND_RE.fullmatch(payload)
        if match is None:
            scan.findings.append(
                Finding("malformed-diagnostic-pragma", payload, scan.path, logical.line)
            )
            continue
        tool = match.group("tool")
        kind = match.group("kind")
        column = hint.start() + 1
        operator_line = LogicalLine(logical.line, logical.offset, payload, payload)
        if kind in {"push", "pop"}:
            if match.group("tail").strip():
                scan.findings.append(
                    Finding("malformed-diagnostic-pragma", payload, scan.path, logical.line)
                )
            else:
                _record_pragma_region(scan, operator_line, tool, kind, column)
            continue
        _record_pragma_control(scan, operator_line, tool, kind, column)


def _scan_pragmas(
    path: str, raw: str, code: str, comments: list[Comment]
) -> tuple[list[Suppression], list[Finding]]:
    """Parse GCC/Clang diagnostic state controls and their paired regions."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    scan = PragmaScan(path, comments, records, findings, {"GCC": 0, "clang": 0})
    for logical in _logical_lines(raw, code):
        _scan_pragma_operator_line(scan, logical)
        hint = PRAGMA_HINT_RE.match(logical.code)
        if hint is None:
            continue
        match = PRAGMA_KIND_RE.fullmatch(logical.code)
        if match is None:
            findings.append(
                Finding("malformed-diagnostic-pragma", logical.raw.strip(), path, logical.line)
            )
            continue
        tool = match.group("tool")
        kind = match.group("kind").lower()
        column = hint.start() + 1
        if kind not in {"push", "pop"}:
            _record_pragma_control(scan, logical, tool, kind, column)
        elif match.group("tail").strip():
            findings.append(
                Finding("malformed-diagnostic-pragma", logical.raw.strip(), path, logical.line)
            )
        else:
            _record_pragma_region(scan, logical, tool, kind, column)
    return records, findings


def _balanced_end(code: str, start: int) -> int | None:
    """Return the offset after the parenthesis matching ``start``."""
    depth = 0
    for index in range(start, len(code)):
        if code[index] == "(":
            depth += 1
        elif code[index] == ")":
            depth -= 1
            if depth == 0:
                return index + 1
    return None


def _attribute_items(code: str, raw: str, start: int, end: int) -> list[tuple[int, str, str]]:
    """Split one attribute list on top-level commas while preserving offsets."""
    items: list[tuple[int, str, str]] = []
    item_start = start
    depth = 0
    for index in range(start, end):
        if code[index] == "(":
            depth += 1
        elif code[index] == ")":
            depth = max(0, depth - 1)
        elif code[index] == "," and depth == 0:
            items.extend(_attribute_item(code, raw, item_start, index))
            item_start = index + 1
    items.extend(_attribute_item(code, raw, item_start, end))
    return items


def _attribute_item(code: str, raw: str, start: int, end: int) -> list[tuple[int, str, str]]:
    """Return one nonempty, left-trimmed attribute item."""
    code_part = code[start:end]
    leading = len(code_part) - len(code_part.lstrip())
    offset = start + leading
    if not code[offset:end].strip():
        return []
    return [(offset, code[offset:end].strip(), raw[offset:end].strip())]


def _macro_parameter(raw: str, offset: int, name: str) -> bool:
    """Return whether one identifier is a parameter of its enclosing macro."""
    line_start = raw.rfind("\n", 0, offset) + 1
    line_end = raw.find("\n", offset)
    if line_end < 0:
        line_end = len(raw)
    match = re.match(
        r"\s*#\s*define\s+[A-Za-z_]\w*\s*\((?P<params>[^)]*)\)",
        raw[line_start:line_end],
    )
    if match is None:
        return False
    params = {item.strip() for item in match.group("params").split(",")}
    return name in params


def _sanitizer_rule(raw: str, offset: int, args: str, *, macro_ok: bool) -> str | None:
    """Normalize an exact sanitizer string list or a live macro parameter."""
    if SANITIZER_ARGS_RE.fullmatch(args):
        return ",".join(SANITIZER_NAME_RE.findall(args))
    argument = args.strip()
    if macro_ok and re.fullmatch(r"[A-Za-z_]\w*", argument):
        return argument if _macro_parameter(raw, offset, argument) else None
    return None


def _compiler_attribute(
    path: str,
    raw: str,
    offset: int,
    rule: str,
    directive: str,
) -> Suppression:
    """Build one declaration-scoped compiler attribute inventory row."""
    tool = "language-attribute" if directive == "[[maybe_unused]]" else "compiler-attribute"
    return _suppression(
        path,
        _line_column(raw, offset),
        ControlSpec(
            "compiler",
            tool,
            rule,
            directive,
            "declaration",
            "compiler-attribute",
        ),
        "",
        reason_required=False,
    )


def _gnu_body_span(code: str, start: int) -> tuple[int, int, int] | None:
    """Return the GNU attribute body and full-expression end offsets."""
    cursor = start
    while cursor < len(code) and code[cursor].isspace():
        cursor += 1
    if cursor >= len(code) or code[cursor] != "(":
        return None
    outer = cursor
    cursor += 1
    while cursor < len(code) and code[cursor].isspace():
        cursor += 1
    if cursor >= len(code) or code[cursor] != "(":
        return None
    inner = cursor
    inner_end = _balanced_end(code, inner)
    outer_end = _balanced_end(code, outer)
    if inner_end is None or outer_end is None:
        return None
    if re.fullmatch(r"\s*\)", code[inner_end:outer_end]) is None:
        return None
    return inner + 1, inner_end - 1, outer_end


def _gnu_item(
    path: str, raw: str, offset: int, code_item: str, raw_item: str
) -> tuple[Suppression | None, Finding | None]:
    """Parse one GNU attribute item, failing closed on supported-name lookalikes."""
    rule: str | None = None
    directive = ""
    if GNU_UNUSED_ITEM_RE.fullmatch(raw_item):
        rule = "unused"
        directive = "__attribute__((unused))"
    else:
        suffix = GNU_SANITIZER_SUFFIX_RE.fullmatch(raw_item)
        call = GNU_SANITIZER_CALL_RE.fullmatch(raw_item)
        if suffix is not None:
            rule = suffix.group("rule")
            directive = "__attribute__((no_sanitize))"
        elif call is not None:
            rule = _sanitizer_rule(raw, offset, call.group("args"), macro_ok=True)
            directive = "__attribute__((no_sanitize))"
    if rule is not None:
        return _compiler_attribute(path, raw, offset, rule, directive), None
    if GNU_CONTROL_HINT_RE.search(code_item) is None:
        return None, None
    line, _ = _line_column(raw, offset)
    return None, Finding("malformed-gnu-attribute", raw_item, path, line)


def _scan_gnu_attributes(path: str, raw: str, code: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory exact GNU unused and sanitizer-disable attribute grammar."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for marker in GNU_ATTRIBUTE_HINT_RE.finditer(code):
        span = _gnu_body_span(code, marker.end())
        if span is None:
            lookahead = code[marker.end() : marker.end() + 512]
            if GNU_CONTROL_HINT_RE.search(lookahead) is not None:
                line, _ = _line_column(raw, marker.start())
                findings.append(Finding("malformed-gnu-attribute", marker.group(), path, line))
            continue
        body_start, body_end, _ = span
        for offset, code_item, raw_item in _attribute_items(code, raw, body_start, body_end):
            record, finding = _gnu_item(path, raw, offset, code_item, raw_item)
            if record is not None:
                records.append(record)
            if finding is not None:
                findings.append(finding)
    return records, findings


def _standard_item(
    path: str, raw: str, offset: int, code_item: str, raw_item: str
) -> tuple[Suppression | None, Finding | None]:
    """Parse one C23/C++ attribute item from the supported suppression family."""
    if STANDARD_MAYBE_RE.fullmatch(raw_item):
        return _compiler_attribute(path, raw, offset, "unused", "[[maybe_unused]]"), None
    if STANDARD_GNU_UNUSED_RE.fullmatch(raw_item):
        return _compiler_attribute(path, raw, offset, "unused", "[[gnu::unused]]"), None
    sanitizer = STANDARD_CLANG_SANITIZER_RE.fullmatch(raw_item)
    if sanitizer is not None:
        rule = _sanitizer_rule(raw, offset, sanitizer.group("args"), macro_ok=False)
        if rule is not None:
            return _compiler_attribute(
                path,
                raw,
                offset,
                rule,
                "[[clang::no_sanitize]]",
            ), None
    if ATTRIBUTE_CONTROL_HINT_RE.search(code_item) is None:
        return None, None
    line, _ = _line_column(raw, offset)
    code_name = (
        "malformed-maybe-unused"
        if re.search(r"\bmaybe_unused\b", code_item)
        else "malformed-standard-attribute"
    )
    return None, Finding(code_name, raw_item, path, line)


def _scan_standard_attributes(
    path: str, raw: str, code: str
) -> tuple[list[Suppression], list[Finding]]:
    """Inventory standard/scoped unused and sanitizer-disable attributes."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    spans: list[tuple[int, int]] = []
    for block in ATTRIBUTE_BLOCK_RE.finditer(code):
        spans.append(block.span())
        for offset, code_item, raw_item in _attribute_items(
            code, raw, block.start("body"), block.end("body")
        ):
            record, finding = _standard_item(path, raw, offset, code_item, raw_item)
            if record is not None:
                records.append(record)
            if finding is not None:
                findings.append(finding)
    for hint in ATTRIBUTE_CONTROL_HINT_RE.finditer(code):
        if any(start <= hint.start() < end for start, end in spans):
            continue
        line, _ = _line_column(raw, hint.start())
        code_name = (
            "malformed-maybe-unused"
            if hint.group().startswith("maybe_unused")
            else "malformed-standard-attribute"
        )
        findings.append(Finding(code_name, "invalid attribute", path, line))
    return records, findings


def _scan_nasa(path: str, raw: str, code: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory reason-bearing NASA Rule 3 function waivers."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for marker in NASA_RE.finditer(code):
        line, column = _line_column(raw, marker.start())
        line_start = raw.rfind("\n", 0, marker.start()) + 1
        if re.match(r"\s*#\s*define\b", code[line_start : marker.start()]):
            continue
        call = NASA_CALL_RE.match(raw, marker.start())
        reason = call.group("reason").strip() if call is not None else ""
        records.append(
            _suppression(
                path,
                (line, column),
                ControlSpec(
                    "project-policy",
                    "annotation-checker",
                    "NASA-P10-3",
                    "RA8_NASA_RULE_3_OK",
                    "function",
                    "annotation",
                ),
                reason,
                reason_required=True,
            )
        )
        if call is None:
            findings.append(
                Finding("malformed-nasa-rule-3-waiver", "reason string required", path, line)
            )
    return records, findings


def _scan_alloc_allow(
    path: str, code: str, comments: list[Comment]
) -> tuple[list[Suppression], list[Finding]]:
    """Inventory same-line, reasoned dynamic-allocation checker waivers."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    code_lines = code.splitlines()
    brace_depths: list[int] = []
    brace_depth = 0
    for source_line in code_lines:
        brace_depths.append(brace_depth)
        if not source_line.lstrip().startswith("#"):
            brace_depth += source_line.count("{") - source_line.count("}")
            brace_depth = max(0, brace_depth)
    for comment in comments:
        hint = ALLOC_HINT_RE.search(comment.text)
        if hint is None:
            continue
        match = ALLOC_ALLOW_RE.search(comment.text.strip())
        if match is None:
            findings.append(
                Finding("malformed-alloc-allow", comment.text.strip(), path, comment.line)
            )
            continue
        source_line = code_lines[comment.line - 1] if comment.line <= len(code_lines) else ""
        line_depth = brace_depths[comment.line - 1] if comment.line <= len(brace_depths) else 0
        symbols = allocation_symbols(source_line, line_depth)
        if not symbols:
            findings.append(
                Finding("orphan-alloc-allow", "no allocator call on this line", path, comment.line)
            )
            continue
        records.extend(
            _suppression(
                path,
                (comment.line, comment.column + hint.start()),
                ControlSpec(
                    "dynamic-allocation",
                    "check_no_dynamic_alloc.py",
                    symbol,
                    "alloc-allow",
                    "line",
                    "inline-comment",
                ),
                match.group("reason").strip(),
                reason_required=True,
            )
            for symbol in symbols
        )
    return records, findings


def scan_c_control_file(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Scan one C-family file through every assigned compiler-control grammar."""
    if Path(path).suffix.lower() not in C_FAMILY_SUFFIXES or C_HINT_RE.search(text) is None:
        return [], []
    code = _code_view(text)
    comments, _ = extract_comments(path, text)
    records: list[Suppression] = []
    findings: list[Finding] = []
    for scanner in (_scan_pragmas,):
        found, problems = scanner(path, text, code, comments)
        records.extend(found)
        findings.extend(problems)
    for scanner in (_scan_gnu_attributes, _scan_standard_attributes, _scan_nasa):
        found, problems = scanner(path, text, code)
        records.extend(found)
        findings.extend(problems)
    found, problems = _scan_alloc_allow(path, code, comments)
    records.extend(found)
    findings.extend(problems)
    return records, findings
