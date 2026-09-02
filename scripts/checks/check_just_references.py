#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate literal, first-party ``just`` command references.

The recipe surface comes from both ``just --summary`` and the recursive JSON
dump.  The former is the user-visible public recipe list; the latter supplies
module entry points and aliases that the summary deliberately omits.  Keeping
the two views in agreement prevents this check from going green because one
side of the comparison silently became empty.

Authored documentation, Just help, scripts, and workflow/configuration YAML are
scanned.  Generated files and vendored dependencies are deliberately excluded.
Arguments may be placeholders, but a recipe name itself must remain literal:
dynamic recipe construction cannot be validated and has repeatedly hidden
stale migration-era commands.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Never

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SELF = Path(__file__).resolve()

MIN_PUBLIC_RECIPES = 150
MIN_MODULES = 20
MIN_ALIASES = 10
MIN_SCOPED_FILES = 500
MIN_LITERAL_REFERENCES = 500

AUTHORED_DOC_SUFFIXES = {".md", ".mdx", ".rst"}
AUTOMATION_SUFFIXES = {".just", ".py", ".sh", ".yaml", ".yml"}
EXCLUDED_PARTS = {
    ".git",
    ".venv",
    "build",
    "generated",
    "node_modules",
    "third_party",
    "vendor",
}

RECIPE_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_-]*(?:::[A-Za-z0-9_][A-Za-z0-9_-]*)*")
QUOTED_JUST_RE = re.compile(r"(['\"])just\1\s*,\s*(['\"])(?P<word>[^'\"]+)\2")
DIRECT_RE = re.compile(r"^\s*(?:(?:[-*+>]\s+|\d+[.)]\s+)|(?:[-]\s+)?run:\s+|@|exec\s+)?just\b")
HELP_ECHO_RE = re.compile(r"^\s*@echo\s+(['\"])(?P<text>.*)\1\s*$")

OPTIONS_WITH_VALUE = {"-d", "-f", "--justfile", "--working-directory"}
FLAG_OPTIONS = {
    "--check",
    "--dry-run",
    "--fmt",
    "--quiet",
    "--unstable",
    "--unsorted",
}
WORD_DELIMITERS = " \t\r\n`'\",;|&()[]"

STANDALONE_SURFACES = {"infra/hil-cache.just": frozenset({"check", "apply"})}
ROOT_JUSTFILES = frozenset({"justfile"})


@dataclass(frozen=True)
class Surface:
    """Invocable names from the recursive Just module tree."""

    recipes: frozenset[str]
    aliases: frozenset[str]
    modules: frozenset[str]

    @property
    def invocable(self) -> frozenset[str]:
        """Return recipes, aliases, and modules with a default entry point."""
        return self.recipes | self.aliases | self.modules


@dataclass(frozen=True)
class Reference:
    """One command-shaped reference extracted from authored text."""

    recipe: str
    column: int
    dynamic_suffix: bool = False
    justfile: str | None = None


class ReferenceCheckError(RuntimeError):
    """Raised when the authoritative Just surface cannot be checked."""


def _fail(message: str) -> Never:
    raise ReferenceCheckError(message)


def _run_just(*args: str) -> str:
    just_bin = shutil.which("just")
    if just_bin is None:
        _fail("just is required to validate command references")
    proc = subprocess.run(  # noqa: S603 -- fixed executable and arguments
        [just_bin, *args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or "unknown error"
        _fail(f"just {' '.join(args)} failed: {detail}")
    return proc.stdout


def _surface_difference(
    summary: frozenset[str], dump_recipes: frozenset[str]
) -> tuple[list[str], list[str]]:
    """Return names missing from summary and names absent from the dump."""
    return sorted(dump_recipes - summary), sorted(summary - dump_recipes)


def _surface_from_dump(dump: dict[str, object]) -> Surface:
    """Build qualified names from a recursive dump on every supported Just.

    Just 1.40 omits ``module_path`` from child dump nodes, while newer
    releases populate it. The containing ``modules`` dictionary is the stable
    source of each path segment, so traversal carries the qualified path
    explicitly and treats newer metadata only as a consistency check.
    """
    recipes: set[str] = set()
    aliases: set[str] = set()
    modules: set[str] = set()

    def walk(module: dict[str, object], module_path: str) -> None:
        reported_path = module.get("module_path")
        if reported_path is not None and str(reported_path) != module_path:
            _fail(
                f"invalid module path metadata: expected {module_path or '<root>'}, "
                f"got {reported_path}"
            )
        prefix = f"{module_path}::" if module_path else ""
        module_recipes = module.get("recipes")
        module_aliases = module.get("aliases")
        child_modules = module.get("modules")
        if not isinstance(module_recipes, dict):
            _fail(f"invalid recipe dump below {module_path or '<root>'}")
        if not isinstance(module_aliases, dict) or not isinstance(child_modules, dict):
            _fail(f"invalid alias/module dump below {module_path or '<root>'}")
        for name, value in module_recipes.items():
            if not isinstance(value, dict):
                _fail(f"invalid recipe metadata for {prefix}{name}")
            if not value.get("private", False):
                recipes.add(f"{prefix}{name}")
        for name in module_aliases:
            aliases.add(f"{prefix}{name}")
        for child_name, child in child_modules.items():
            if not isinstance(child, dict):
                _fail(f"invalid child module below {module_path or '<root>'}")
            child_path = f"{module_path}::{child_name}" if module_path else str(child_name)
            if child.get("first") is not None:
                modules.add(child_path)
            walk(child, child_path)

    walk(dump, "")
    return Surface(frozenset(recipes), frozenset(aliases), frozenset(modules))


def load_surface() -> Surface:
    """Load and cross-check the public recipe, alias, and module surfaces."""
    summary = frozenset(_run_just("--summary").split())
    raw_dump = json.loads(_run_just("--dump", "--dump-format", "json"))
    if not isinstance(raw_dump, dict):
        _fail("invalid root recipe dump")
    surface = _surface_from_dump(raw_dump)

    if summary != surface.recipes:
        missing, extra = _surface_difference(summary, surface.recipes)
        _fail(
            f"just surface disagreement: missing from summary={missing}, absent from dump={extra}"
        )
    if len(summary) < MIN_PUBLIC_RECIPES:
        _fail(f"public recipe census collapsed: {len(summary)} < {MIN_PUBLIC_RECIPES}")
    if len(surface.modules) < MIN_MODULES:
        _fail(f"module census collapsed: {len(surface.modules)} < {MIN_MODULES}")
    if len(surface.aliases) < MIN_ALIASES:
        _fail(f"alias census collapsed: {len(surface.aliases)} < {MIN_ALIASES}")
    return Surface(summary, surface.aliases, surface.modules)


def _is_excluded(path: Path) -> bool:
    return any(part in EXCLUDED_PARTS or part.startswith("build-") for part in path.parts)


def _is_authored_doc(path: Path) -> bool:
    """Return whether a path has a supported documentation suffix, case-insensitively."""
    return path.suffix.lower() in AUTHORED_DOC_SUFFIXES


def scoped_files() -> list[Path]:
    """Return first-party authored docs and automation files."""
    git_bin = shutil.which("git") or "git"
    proc = subprocess.run(  # noqa: S603 -- fixed argv, trusted tool path
        [git_bin, "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    paths: list[Path] = []
    for raw in proc.stdout.splitlines():
        rel = Path(raw)
        if _is_excluded(rel):
            continue
        suffix = rel.suffix.lower()
        if rel.name == "justfile" or _is_authored_doc(rel) or suffix in AUTOMATION_SUFFIXES:
            path = REPO_ROOT / rel
            if path.is_file():
                paths.append(path)
    if SELF.is_file() and SELF not in paths:
        paths.append(SELF)
    paths = sorted(set(paths))
    if len(paths) < MIN_SCOPED_FILES:
        _fail(f"reference scope collapsed: {len(paths)} < {MIN_SCOPED_FILES} files")
    return paths


def _logical_lines(text: str) -> Iterable[tuple[int, str]]:
    """Join shell continuations while retaining the first physical line number."""
    pending = ""
    start = 1
    for number, line in enumerate(text.splitlines(), 1):
        if not pending:
            start = number
        if line.rstrip().endswith("\\"):
            pending += line.rstrip()[:-1] + " "
            continue
        yield start, pending + line
        pending = ""
    if pending:
        yield start, pending


def _read_word(text: str, position: int) -> tuple[str, int]:
    while position < len(text) and text[position].isspace():
        position += 1
    if position >= len(text):
        return "", position
    if text[position] in "'\"":
        quote = text[position]
        position += 1
        start = position
        while position < len(text):
            if text[position] == quote and text[position - 1] != "\\":
                return text[start:position], position + 1
            position += 1
        return text[start:], position
    start = position
    while position < len(text) and text[position] not in WORD_DELIMITERS:
        position += 1
    return text[start:position], position


def _reference_after_just(line: str, position: int) -> Reference | None:
    """Parse the fixed recipe word after a conventional ``just`` token."""
    cursor = position
    justfile = None
    while True:
        word_start = cursor
        word, cursor = _read_word(line, cursor)
        if not word:
            return None
        if word in OPTIONS_WITH_VALUE:
            value, cursor = _read_word(line, cursor)
            if word in {"-f", "--justfile"}:
                justfile = value
            continue
        valued_option = next(
            (option for option in OPTIONS_WITH_VALUE if word.startswith(f"{option}=")),
            None,
        )
        if valued_option is not None:
            if valued_option in {"-f", "--justfile"}:
                justfile = word.partition("=")[2]
            continue
        if word in FLAG_OPTIONS:
            continue
        if word.startswith("-"):
            return None
        match = RECIPE_RE.match(word)
        if match is None:
            return None
        recipe = match.group(0)
        suffix = word[match.end() :]
        dynamic = suffix.startswith(("::{", "::$"))
        while word_start < len(line) and line[word_start].isspace():
            word_start += 1
        return Reference(recipe, word_start, dynamic, justfile)


def _invocable_for_reference(reference: Reference, root_surface: Surface) -> frozenset[str]:
    """Resolve a reference against its explicit, statically approved Justfile."""
    if reference.justfile is None:
        return root_surface.invocable
    normalized = Path(reference.justfile).as_posix()
    while normalized.startswith("./"):
        normalized = normalized[2:]
    if normalized in ROOT_JUSTFILES:
        return root_surface.invocable
    return STANDALONE_SURFACES.get(normalized, frozenset())


def _strong_command_context(
    path: Path, line: str, position: int, *, in_doc_code_fence: bool
) -> bool:
    prefix = line[:position]
    suffix = path.suffix.lower()
    if "#" in prefix and (suffix in {".sh", ".just"} or in_doc_code_fence):
        return False
    direct_automation = suffix in {".sh", ".just", ".yaml", ".yml"} and bool(DIRECT_RE.match(line))
    direct_doc = in_doc_code_fence and bool(DIRECT_RE.match(line))
    markdown_list = _is_authored_doc(path) and bool(
        re.match(r"^\s*(?:[-*+>]\s+|\d+[.)]\s+)just\b", line)
    )
    # Odd delimiter-run parity means the command is inside `...`, ``...``, etc.
    inline_code = _is_authored_doc(path) and len(re.findall(r"`+", prefix)) % 2 == 1
    # User-facing echo/print strings frequently indent the displayed command.
    display_match = re.search(r"(?:echo|printf|print)\b.*['\"](?P<label>[^'\"]*)$", prefix)
    displayed_command = False
    if display_match is not None:
        label = display_match.group("label").strip().lower()
        displayed_command = not label or bool(
            re.search(r"(?:command|local|run|usage|use|via):$", label)
        )
    return direct_automation or direct_doc or markdown_list or inline_code or displayed_command


def _array_command_context(line: str, position: int) -> bool:
    """Distinguish an argv literal from unrelated string collections."""
    prefix = line[:position]
    return bool(
        prefix.rstrip().endswith("[")
        or re.search(r"subprocess\.[A-Za-z_]+\s*\([^\]\n]*$", prefix)
        or re.search(r"(?:argv|cmd|command)[A-Za-z0-9_]*\s*=\s*[\[(][^\]\n]*$", prefix)
    )


def references_in_line(
    path: Path, line: str, *, in_doc_code_fence: bool = False
) -> list[Reference]:
    """Extract command-shaped literal references, excluding natural-language 'just'."""
    refs: list[Reference] = []
    occupied: list[tuple[int, int]] = []
    for match in QUOTED_JUST_RE.finditer(line):
        if not _array_command_context(line, match.start()):
            continue
        word = match.group("word")
        recipe_match = RECIPE_RE.match(word)
        if recipe_match is None or word.startswith("-"):
            continue
        recipe = recipe_match.group(0)
        suffix = word[recipe_match.end() :]
        refs.append(Reference(recipe, match.start("word"), suffix.startswith(("::{", "::$"))))
        occupied.append(match.span())

    for match in re.finditer(r"(?<!\.)\bjust\b", line):
        if any(start <= match.start() < end for start, end in occupied):
            continue
        if match.end() >= len(line) or not line[match.end()].isspace():
            continue
        ref = _reference_after_just(line, match.end())
        if ref is None:
            continue
        if (
            ref.dynamic_suffix
            or "::" in ref.recipe
            or _strong_command_context(
                path, line, match.start(), in_doc_code_fence=in_doc_code_fence
            )
        ):
            refs.append(ref)
    return refs


def duplicate_help_findings(path: Path, text: str) -> list[str]:
    """Reject exact command duplicates within one Just help menu."""
    seen: dict[str, int] = {}
    findings: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        match = HELP_ECHO_RE.match(line)
        if match is None:
            continue
        help_text = match.group("text").replace('\\"', '"')
        command_match = re.search(r"\bjust\s+(.+?)(?:\s{2,}|$)", help_text)
        if command_match is None:
            continue
        command = "just " + " ".join(command_match.group(1).split())
        if command in seen:
            findings.append(
                f"{path.relative_to(REPO_ROOT)}:{number}: duplicate help command "
                f"{command!r} (first at line {seen[command]})"
            )
        else:
            seen[command] = number
    return findings


def check_paths(paths: Iterable[Path], surface: Surface) -> tuple[list[str], int]:
    """Check references and help duplication for the supplied files."""
    findings: list[str] = []
    reference_count = 0
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        if path.name == "justfile" or path.suffix == ".just":
            findings.extend(duplicate_help_findings(path, text))
        in_doc_code_fence = False
        for number, line in _logical_lines(text):
            stripped = line.lstrip()
            if _is_authored_doc(path) and stripped.startswith(("```", "~~~")):
                in_doc_code_fence = not in_doc_code_fence
                continue
            for ref in references_in_line(path, line, in_doc_code_fence=in_doc_code_fence):
                reference_count += 1
                rel = path.relative_to(REPO_ROOT)
                if ref.dynamic_suffix:
                    findings.append(
                        f"{rel}:{number}: dynamic Just recipe name after {ref.recipe!r}; "
                        "use a fixed recipe and pass the value as an argument"
                    )
                elif ref.recipe not in _invocable_for_reference(ref, surface):
                    findings.append(
                        f"{rel}:{number}: unknown Just recipe/module/alias {ref.recipe!r}"
                    )
    return findings, reference_count


def _reference_selftest_failures() -> tuple[list[str], int]:
    """Return failures for command extraction and surface resolution cases."""
    surface = Surface(
        frozenset({"apps::build", "quality::local::gate", "search"}),
        frozenset({"apps::compile-commands"}),
        frozenset({"apps", "apps::host"}),
    )
    path = REPO_ROOT / "fixture.md"
    just_word = "just"
    cases = (
        (f"`{just_word} apps::build <app>`", [], "recipe with placeholder argument"),
        (f"run `{just_word} apps::compile-commands`", [], "alias"),
        (f"`{just_word} apps`", [], "module default"),
        (f"it is {just_word} enough for now", [], "natural language"),
        (f"`{just_word} apps::missing foo`", ["unknown"], "stale namespaced recipe"),
        (f"run `{just_word} missing`", ["unknown"], "stale root recipe"),
        (
            'print(f"' + just_word + ' apps::host::{name}")',
            ["dynamic"],
            "dynamic recipe",
        ),
        (f"`{just_word} apps::host::<command>`", [], "documented module placeholder"),
        (
            f"`{just_word} --justfile ./justfile quality::local::gate lint-just`",
            [],
            "justfile option",
        ),
        (
            f"`{just_word} --justfile infra/hil-cache.just check`",
            [],
            "approved standalone recipe",
        ),
        (
            f"`{just_word} --justfile infra/hil-cache.just missing`",
            ["unknown"],
            "unknown standalone recipe",
        ),
        (
            f'subprocess.run(["{just_word}", "apps::build", "blink"])',
            [],
            "argv command",
        ),
    )
    failures: list[str] = []
    for line, expected, label in cases:
        refs = references_in_line(path, line)
        found: list[str] = []
        for ref in refs:
            if ref.dynamic_suffix:
                found.append("dynamic")
            elif ref.recipe not in _invocable_for_reference(ref, surface):
                found.append("unknown")
        if found != expected:
            failures.append(f"{label}: expected {expected}, got {found} from {refs}")
    uppercase = REPO_ROOT / "fixture.MD"
    uppercase_refs = references_in_line(uppercase, "`just missing`")
    if len(uppercase_refs) != 1 or uppercase_refs[0].recipe != "missing":
        failures.append(f"uppercase Markdown command escaped: {uppercase_refs}")
    if not _is_authored_doc(uppercase) or _is_authored_doc(REPO_ROOT / "fixture.txt"):
        failures.append("case-insensitive authored-document scope drifted")
    return failures, len(cases) + 2


def _help_selftest_failures() -> tuple[list[str], int]:
    """Return failures for exact-duplicate help detection."""
    duplicate = duplicate_help_findings(
        REPO_ROOT / "fixture.just",
        '@echo "  just apps::build <app>  Build one"\n@echo "  just apps::build <app>  Build it"\n',
    )
    overload = duplicate_help_findings(
        REPO_ROOT / "fixture.just",
        '@echo "  just apps::build <app>  Build one"\n'
        '@echo "  just apps::build <app> clean=1  Rebuild one"\n',
    )
    failures: list[str] = []
    if len(duplicate) != 1:
        failures.append(f"duplicate help: expected one finding, got {duplicate}")
    if overload:
        failures.append(f"distinct help forms were treated as duplicates: {overload}")

    return failures, 2


def _surface_selftest_failures() -> tuple[list[str], int]:
    """Prove both comparison directions and old/new dump traversal."""
    failures: list[str] = []
    cases = (
        (frozenset({"a"}), frozenset({"a"}), ([], []), "equal views"),
        (frozenset({"a"}), frozenset({"a", "b"}), (["b"], []), "summary omission"),
        (frozenset({"a", "b"}), frozenset({"a"}), ([], ["b"]), "dump omission"),
    )
    for summary, dump, expected, label in cases:
        actual = _surface_difference(summary, dump)
        if actual != expected:
            failures.append(f"{label}: expected {expected}, got {actual}")

    child: dict[str, object] = {
        "recipes": {"gate": {"private": False}},
        "aliases": {"check": {}},
        "modules": {},
        "first": "gate",
    }
    old_dump: dict[str, object] = {
        "recipes": {"root": {"private": False}},
        "aliases": {},
        "modules": {"quality": child},
    }
    new_child = {**child, "module_path": "quality"}
    new_dump = {**old_dump, "module_path": "", "modules": {"quality": new_child}}
    expected_surface = Surface(
        frozenset({"root", "quality::gate"}),
        frozenset({"quality::check"}),
        frozenset({"quality"}),
    )
    for dump, label in (
        (old_dump, "Just 1.40 dump without module_path"),
        (new_dump, "newer Just dump with module_path"),
    ):
        actual_surface = _surface_from_dump(dump)
        if actual_surface != expected_surface:
            failures.append(f"{label}: expected {expected_surface}, got {actual_surface}")

    return failures, len(cases) + 2


def selftest() -> int:
    """Exercise positive, negative, placeholder, and duplicate cases."""
    reference_failures, reference_cases = _reference_selftest_failures()
    help_failures, help_cases = _help_selftest_failures()
    surface_failures, surface_cases = _surface_selftest_failures()
    failures = reference_failures + help_failures + surface_failures
    if failures:
        for failure in failures:
            print(f"selftest: check_just_references.py FAIL: {failure}", file=sys.stderr)
        return 1
    print(
        "selftest: check_just_references.py OK "
        f"({reference_cases + help_cases + surface_cases} cases)"
    )
    return 0


def main() -> int:
    """Run the live reference audit or its detector selftest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="run detector selftests")
    args = parser.parse_args()
    if args.selftest:
        return selftest()

    try:
        surface = load_surface()
        paths = scoped_files()
        findings, reference_count = check_paths(paths, surface)
    except (json.JSONDecodeError, OSError, ReferenceCheckError, subprocess.SubprocessError) as exc:
        print(f"check-just-references: ERROR: {exc}", file=sys.stderr)
        return 1
    if reference_count < MIN_LITERAL_REFERENCES:
        print(
            "check-just-references: ERROR: literal-reference census collapsed: "
            f"{reference_count} < {MIN_LITERAL_REFERENCES}",
            file=sys.stderr,
        )
        return 1
    if findings:
        for finding in findings:
            print(finding, file=sys.stderr)
        print(
            f"check-just-references: FAIL ({len(findings)} finding(s), "
            f"{reference_count} references in {len(paths)} files)",
            file=sys.stderr,
        )
        return 1
    print(
        f"Just references clean ({reference_count} references, {len(paths)} files, "
        f"{len(surface.recipes)} recipes, {len(surface.modules)} modules, "
        f"{len(surface.aliases)} aliases)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
