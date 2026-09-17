# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Declarative syntax catalog for repository suppression directives."""

from __future__ import annotations

import re
from pathlib import Path

from lint_coverage_rules import EXEMPT_PREFIXES, EXT_CLASS, PATH_CLASS

VENDOR_PREFIXES = tuple(
    prefix for prefix, reason in EXEMPT_PREFIXES if "vendored" in reason or "SOUP" in reason
)
GENERATED_PREFIXES = tuple(
    prefix for prefix, reason in EXEMPT_PREFIXES if "generated" in reason or "emitted" in reason
)
GENERATED_CLASSES = frozenset({"generated-source"})
BINARY_SUFFIXES = frozenset(suffix for suffix, cls in EXT_CLASS.items() if cls == "binary")
REQUIRED_FAMILIES = frozenset(
    {
        "ansible",
        "baseline-ratchet",
        "encoding-exemption",
        "clang-tidy",
        "compiler",
        "checker-nonfatal-control",
        "checker-scope-control",
        "ci-parity-exemption",
        "coverage",
        "coverage-mask",
        "cppcheck",
        "ctest",
        "formatter",
        "generated-artifact",
        "documentation-control",
        "lint-control",
        "mcdc-deactivation",
        "project-policy",
        "python",
        "shellcheck",
        "shell-status",
        "ansible-lint-config",
        "security-analysis-control",
        "other-language-control",
        "test-control",
        "workflow",
    }
)
THREADX_FIRST_PARTY_FILES = frozenset(
    {
        "port/threadx/inc/ra8_threadx.h",
        "port/threadx/inc/tx_user.h",
        "port/threadx/src/cortex_m85/tx_systick_ready.c",
        "port/threadx/src/cortex_m85/tx_systick_retune.c",
    }
)

KNOWN_PROJECT_MARKERS = frozenset(
    {
        "AI-OK",
        "ATTR-OK",
        "C23HDR-OK",
        "CITES-OK",
        "FILE-SIZE-OK",
        "LEGACY-OK",
        "MAGIC-OK",
        "PATHREF-OK",
        "TZ-DISCARD-OK",
        "WAVE-OK",
    }
)

KNOWN_CPPCHECK_RULES = frozenset(
    {
        "comparePointers",
        "constParameterCallback",
        "constParameterPointer",
        "constVariablePointer",
        "duplicateExpression",
        "knownConditionTrueFalse",
        "missingInclude",
        "redundantAssignment",
        "unassignedVariable",
        "unknownMacro",
        "unreadVariable",
        "unusedFunction",
        "unusedScopedObject",
        "unusedStructMember",
        "unusedVariable",
        "variableScope",
        *{
            f"misra-c2012-{rule}"
            for rule in (
                "7.4",
                "8.4",
                "8.9",
                "9.2",
                "9.5",
                "11.1",
                "11.2",
                "11.3",
                "11.5",
                "11.6",
                "11.8",
                "12.1",
                "15.5",
                "17.3",
                "17.7",
                "18.4",
                "18.8",
                "21.3",
                "21.6",
                "21.16",
                "22.10",
            )
        },
    }
)

C_FAMILY_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".m", ".mm"}
)
PYTHON_SUFFIXES = frozenset({".py", ".pyi"})
HASH_COMMENT_SUFFIXES = frozenset(
    {".bash", ".cmake", ".ini", ".just", ".mk", ".sh", ".toml", ".yaml", ".yml", ".zsh"}
)
HASH_COMMENT_NAMES = frozenset(
    {"CMakeLists.txt", "Dockerfile", "GNUmakefile", "Justfile", "Makefile", "justfile"}
)
BUILD_CONTROL_SUFFIXES = frozenset(
    {".bash", ".cmake", ".just", ".mk", ".sh", ".yaml", ".yml", ".zsh"}
)
BUILD_CONTROL_NAMES = HASH_COMMENT_NAMES

NOLINT_RE = re.compile(
    r"^NOLINT(?P<scope>NEXTLINE|BEGIN|END)?(?:\((?P<rules>[^)]*)\))?"
    r"(?P<tail>.*)$"
)
NOLINT_RAW_RE = re.compile(
    r"NOLINT(?P<scope>NEXTLINE|BEGIN|END)?"
    r"(?:\((?P<rules>[^)\r\n]*)\)|\[(?P<bracket_rules>[^]\r\n]*)\])?"
    r"(?![A-Za-z])"
)
NOQA_RE = re.compile(
    r"^noqa(?:\s*:\s*(?P<rules>[A-Za-z0-9_.-]+(?:\s*,\s*[A-Za-z0-9_.-]+)*))?"
    r"(?P<tail>\s*(?:(?:--|#|-\s).*)?|\s*\(.*\))$",
    re.IGNORECASE,
)
TYPE_IGNORE_RE = re.compile(
    r"^type\s*:\s*ignore(?:\[(?P<rules>[^]]*)\])?"
    r"(?P<tail>\s*(?:--.*|#.*)?)$",
    re.IGNORECASE,
)
PYTHON_COVERAGE_RE = re.compile(
    r"^pragma\s*:\s*no\s+cover(?P<tail>\s*(?:--.*)?)$",
    re.IGNORECASE,
)
PYLINT_RE = re.compile(
    r"^pylint\s*:\s*(?P<control>disable|enable|skip-file)"
    r"(?:\s*=\s*(?P<rules>[A-Za-z0-9_.-]+(?:\s*,\s*[A-Za-z0-9_.-]+)*))?"
    r"(?P<tail>\s*(?:--.*)?)$",
    re.IGNORECASE,
)
MYPY_RE = re.compile(
    r"^mypy\s*:\s*(?P<control>ignore-errors|disable-error-code|enable-error-code)"
    r"(?:\s*=\s*[\"']?\[?(?P<rules>[A-Za-z0-9_.-]+(?:\s*,\s*[A-Za-z0-9_.-]+)*)"
    r"\]?[\"']?)?(?P<tail>\s*(?:--.*)?)$",
    re.IGNORECASE,
)
PYRIGHT_RE = re.compile(
    r"^pyright\s*:\s*(?:(?P<ignore>ignore)(?:\[(?P<rules>[A-Za-z0-9_.-]+"
    r"(?:\s*,\s*[A-Za-z0-9_.-]+)*)\])?|(?P<setting>report[A-Za-z0-9_]+)"
    r"\s*=\s*(?P<value>true|false|none|information|warning|error))"
    r"(?P<tail>\s*(?:--.*)?)$",
    re.IGNORECASE,
)
BANDIT_RE = re.compile(
    r"^(?:(?P<nosec>nosec)(?:\s+(?P<nosec_rules>[A-Za-z0-9_.-]+"
    r"(?:\s*,\s*[A-Za-z0-9_.-]+)*))?|bandit\s*:\s*skip\s*=\s*"
    r"(?P<bandit_rules>[A-Za-z0-9_.-]+(?:\s*,\s*[A-Za-z0-9_.-]+)*))"
    r"(?P<tail>\s*(?:--.*)?)$",
    re.IGNORECASE,
)
PYTHON_FORMATTER_RE = re.compile(
    r"^(?:(?P<ruff>ruff\s*:\s*noqa)(?:\s*:\s*(?P<ruff_rules>[A-Za-z0-9_.-]+"
    r"(?:\s*,\s*[A-Za-z0-9_.-]+)*))?|(?P<fmt>fmt)\s*:\s*(?P<fmt_control>off|on|skip)|"
    r"(?P<isort>isort)\s*:\s*(?P<isort_control>off|on|skip|skip_file))"
    r"(?P<tail>\s*(?:--.*)?)$",
    re.IGNORECASE,
)
SHELLCHECK_RE = re.compile(
    r"^shellcheck\s+(?P<control>disable|enable|source|source-path|shell|external-sources)"
    r"\s*=\s*(?P<value>\S+(?:\s*,\s*\S+)*)"
    r"(?P<tail>\s*(?:#.*)?)$",
    re.IGNORECASE,
)
SHELL_STATUS_MASK_RE = re.compile(r"\|\|\s*(?:true\b|:(?![A-Za-z0-9_]))")
SHELLCHECK_GLOBAL_EXCLUDE_RE = re.compile(
    r"(?:^|\s)(?:-e|--exclude)(?:=|\s+)(?P<rules>SC\d+(?:\s*,\s*SC\d+)*)"
)
COVERAGE_RE = re.compile(
    r"^(?P<marker>(?:GCOVR|LCOV)_EXCL(?:_BR)?_(?:LINE|START|STOP))"
    r"(?P<tail>\s*(?:(?:--|:).*)?)$"
)
CPPCHECK_RE = re.compile(
    r"^cppcheck-suppress(?P<scope>-(?:begin|end|file))?\s+"
    r"\[?(?P<rule>[A-Za-z0-9_.-]+)\]?(?P<tail>\s*(?:(?:--|;).*)?)$"
)
ALLOC_ALLOW_RE = re.compile(
    r"\balloc-allow\s*:\s*(?P<reason>\S(?:.*\S)?)\s*$",
    re.IGNORECASE,
)
PROJECT_MARKER_RE = re.compile(
    r"^(?P<marker>[A-Z][A-Z0-9_-]*-OK)"
    r"(?:(?:\s*:\s*|\s+--\s*)(?P<reason>.*))?$"
)
UNKNOWN_DIRECTIVE_RE = re.compile(
    r"^(?:NOLINT[A-Z]*(?:\(|\s*(?:--|$))|noqa(?:\s*:|\s*--|\s*$)|"
    r"type\s*:\s*ignore\b|pragma\s*:\s*no\s+cover\b|"
    r"pylint\s*:|mypy\s*:|pyright\s*:|nosec\b|bandit\s*:|"
    r"ruff\s*:\s*noqa|fmt\s*:|isort\s*:|shellcheck\s+\S+|"
    r"(?:GCOVR|LCOV)_EXCL[A-Z_]*(?:\s*--|\s*$)|cppcheck-suppress\S*|"
    r"clang-format\s+(?:off|on)\b|yamllint\s+(?:disable|enable)|"
    r"hadolint\s+(?:global\s+)?ignore\s*=|cmake-(?:lint|format)\s*:|"
    r"shfmt\s*:|prettier-ignore\b|markdownlint-[A-Za-z-]+|"
    r"(?:@|\\)(?:cond|endcond)\b|IWYU\s+pragma\s*:|"
    r"[A-Z][A-Z0-9_-]*-OK(?:\s*:|\s+--|\s*$))"
)
WARNING_FLAG_RE = re.compile(r"(?<![A-Za-z0-9_])(?P<flag>-Wno-[A-Za-z0-9_.=+-]+)(?![A-Za-z0-9_])")
BLANKET_WARNING_RE = re.compile(r"(?<![A-Za-z0-9_])(?P<flag>-w)(?![A-Za-z0-9_])")
SUPPORTED_HINT_RE = re.compile(
    r"(?:NOLINT|noqa|type\s*:\s*ignore|pragma\s*:\s*no\s+cover|"
    r"pylint\s*:|mypy\s*:|pyright\s*:|nosec\b|bandit\s*:|"
    r"ruff\s*:\s*noqa|fmt\s*:|isort\s*:|shellcheck|(?:GCOVR|LCOV)_EXCL|"
    r"cppcheck-suppress|clang-format|(?:@|\\)(?:cond|endcond)\b|"
    r"IWYU\s+pragma\s*:|[A-Z][A-Z0-9_-]*-OK|-Wno-|"
    r"(?<![A-Za-z0-9_])-w(?![A-Za-z0-9_]))",
    re.IGNORECASE,
)

# Every formerly unsupported governance family now has a typed parser. Keep
# this compatibility tuple empty so old callers cannot silently revive a raw
# substring census as an authority.
UNSUPPORTED_CATEGORIES: tuple[tuple[str, re.Pattern[str]], ...] = ()


def ownership(path: str) -> str:
    """Classify ownership from the repository's canonical path evidence."""
    if PATH_CLASS.get(path) in GENERATED_CLASSES:
        return "generated"
    if path in THREADX_FIRST_PARTY_FILES:
        return "first-party"
    if path.startswith(VENDOR_PREFIXES):
        return "vendor"
    if path.startswith("port/threadx/") and Path(path).suffix.lower() in C_FAMILY_SUFFIXES:
        return "vendor"
    if path.startswith(GENERATED_PREFIXES):
        return "generated"
    return "first-party"


def language(path: str, first_line: str = "") -> str:
    """Classify comment syntax from path and an optional shebang."""
    item = Path(path)
    if item.suffix.lower() in C_FAMILY_SUFFIXES:
        return "c-family"
    if item.suffix.lower() in PYTHON_SUFFIXES:
        return "python"
    if (
        item.name in HASH_COMMENT_NAMES
        or item.name.startswith("Dockerfile.")
        or item.suffix.lower() in HASH_COMMENT_SUFFIXES
    ):
        return "hash"
    if first_line.startswith("#!") and any(word in first_line for word in ("sh", "bash", "zsh")):
        return "hash"
    return "text"


def is_build_control(path: str, first_line: str = "") -> bool:
    """Return whether compiler warning flags in this file can be active."""
    item = Path(path)
    if item.name in BUILD_CONTROL_NAMES or item.suffix.lower() in BUILD_CONTROL_SUFFIXES:
        return True
    return first_line.startswith("#!") and any(word in first_line for word in ("sh", "bash", "zsh"))


def is_shell_control(path: str, first_line: str = "") -> bool:
    """Return whether hash comments follow shell word-boundary rules."""
    item = Path(path)
    suffix = item.suffix.lower()
    if suffix in {".bash", ".sh", ".zsh"}:
        return True
    if suffix == ".just" or item.name == "justfile":
        return True
    return first_line.startswith("#!") and any(word in first_line for word in ("sh", "bash", "zsh"))
