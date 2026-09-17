#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the immutable pre-commit's Python runs on the interpreter it pins.

``scripts/git/pre-commit`` deliberately pins its interpreter to the trusted
system one, ``/usr/bin/python3``, and refuses to run without it.  That path is
Python 3.9.6 on the declared macOS host, while ``pyproject.toml`` declares
``requires-python >= 3.11`` and Ruff's ``target-version`` is ``py311``.  Every
Python-authoring rule in this repository therefore aims two minor versions
above the interpreter the hook itself uses, and nothing measures the gap.

That gap has already cost a commit: #839 recorded ``git commit`` dying in the
immutable hook validator with ``TypeError: unsupported operand type(s) for |:
'types.GenericAlias' and 'NoneType'`` because a module-level ``X | None`` type
alias in ``hook_runtime_selftest.py`` is EVALUATED at import time.  The repair
(``Optional[...]`` plus a comment naming macOS 3.9) is a comment, not a gate,
so the next 3.11-only construct in the same module set breaks committing again
on that host and CI stays green, because CI runs the managed 3.11 interpreter.

Three rules, over exactly the modules the hook executes under its pinned
interpreter:

1. every one parses at ``HOOK_PYTHON_FLOOR``, which catches the syntax-level
   additions (``match``, ``except*``, PEP 695 aliases);
2. every module file carries ``from __future__ import annotations``, so an
   ``X | None`` written in an annotation stays a string instead of being
   evaluated;
3. no module- or class-level type alias is built with an EVALUATED PEP 604
   union, which is the #839 mechanism exactly and is invisible to rule 1.

The module set is DERIVED from ``scripts/git/pre-commit``: the scripts it
invokes, the first-party modules those import transitively, and the inline
``-c`` programs it feeds the same interpreter.  A hardcoded list would go stale
silently, which is the defect class this file exists to close, so the floors
below refuse a derivation that collapsed instead of reporting a clean tree.

``HOOK_PYTHON_FLOOR`` records what the tree does today; it is not a claim that
3.9 is the right floor forever.  Raising it (requiring a newer interpreter on
the macOS host) or removing it (running hooks only in the supported Linux
container) is the owner's call, and this gate is the place that value lives.

Run::

    check_hook_python_floor.py              # check the derived module set
    check_hook_python_floor.py --selftest   # prove both directions

Exit 0 when every checked source holds all three rules, 1 with the offenders
listed, 2 when the derivation collapsed below its floors.
"""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from selftest_assert import expect, report

REPO_ROOT = Path(__file__).resolve().parents[2]
PRE_COMMIT = REPO_ROOT / "scripts" / "git" / "pre-commit"
OWNER_PYTHON = "/usr/bin/python3"
HOOK_PYTHON_FLOOR = (3, 9)
FLOOR_TEXT = f"{HOOK_PYTHON_FLOOR[0]}.{HOOK_PYTHON_FLOOR[1]}"

# Anti-vacuous floors: the derivation reads a 900-line shell file, and a
# regex that stops matching would otherwise report a clean set of nothing.
ENTRY_FLOOR = 3
MODULE_FLOOR = 6
INLINE_FLOOR = 3

_ENTRY_RE = re.compile(r'"\$OWNER_PYTHON"\s+-I\s+"\$CONTROL_DIR/([A-Za-z0-9_./-]+)"')
_INLINE_RE = re.compile(r'"\$OWNER_PYTHON"\s+-I\s+-c\s+\'([^\']*)\'')
_INLINE_VAR_RE = re.compile(r'"\$OWNER_PYTHON"\s+-I\s+-c\s+"\$([A-Za-z_][A-Za-z0-9_]*)"')
_HEREDOC_RE = re.compile(r"read -r -d '' ([A-Za-z_][A-Za-z0-9_]*) <<'PY'\n(.*?)\nPY\n", re.DOTALL)

# A BitOr is arithmetic far more often than it is a type union, so a type
# alias is recognised by an operand only a type expression carries: a bare
# ``None``, or a subscripted generic.
_GENERIC_NAMES = frozenset({"tuple", "list", "dict", "set", "frozenset", "type", "Optional"})


def _entry_scripts(hook_text: str) -> tuple[str, ...]:
    """Return the repo-relative scripts the hook runs under its pinned Python."""
    return tuple(dict.fromkeys(_ENTRY_RE.findall(hook_text)))


def _inline_programs(hook_text: str) -> tuple[tuple[str, str], ...]:
    """Return the ``-c`` programs the hook feeds its pinned Python, labelled."""
    found: list[tuple[str, str]] = []
    for index, source in enumerate(_INLINE_RE.findall(hook_text), start=1):
        found.append((f"<pre-commit -c #{index}>", source))
    heredocs = dict(_HEREDOC_RE.findall(hook_text))
    for name in dict.fromkeys(_INLINE_VAR_RE.findall(hook_text)):
        source = heredocs.get(name)
        if source is not None:
            found.append((f"<pre-commit ${name}>", source))
    return tuple(found)


def _resolve_module(module: str, sibling_dir: Path) -> Path | None:
    """Map a dotted import to a first-party file, or None when it is not ours."""
    relative = module.replace(".", "/") + ".py"
    for candidate in (REPO_ROOT / relative, sibling_dir / relative):
        if candidate.is_file():
            return candidate
    return None


def _imported_modules(tree: ast.AST) -> tuple[str, ...]:
    """Return every absolute dotted name the module imports."""
    names: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and node.module and not node.level:
            # `from scripts.checks import hook_parity_mutations` names the
            # module in the alias, not in `node.module`; taking only the
            # latter dropped two of the validator's five boundary modules.
            names.append(node.module)
            names.extend(f"{node.module}.{alias.name}" for alias in node.names)
        elif isinstance(node, ast.Import):
            names.extend(alias.name for alias in node.names)
    return tuple(names)


def _module_closure(entries: tuple[str, ...]) -> tuple[Path, ...]:
    """Walk the hook's entry scripts and their transitive first-party imports."""
    pending = [REPO_ROOT / entry for entry in entries]
    seen: dict[Path, None] = {}
    while pending:
        path = pending.pop()
        if path in seen or not path.is_file():
            continue
        seen[path] = None
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for module in _imported_modules(tree):
            resolved = _resolve_module(module, path.parent)
            if resolved is not None and resolved not in seen:
                pending.append(resolved)
    return tuple(sorted(seen))


def _annotation_nodes(tree: ast.AST) -> set[int]:
    """Return the ids of every annotation expression in the tree."""
    marked: set[int] = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            arguments = (
                list(node.args.posonlyargs) + list(node.args.args) + list(node.args.kwonlyargs)
            )
            marked.update(id(arg.annotation) for arg in arguments if arg.annotation)
            if node.returns:
                marked.add(id(node.returns))
        elif isinstance(node, ast.AnnAssign):
            marked.add(id(node.annotation))
    return marked


def _is_type_union(node: ast.BinOp) -> bool:
    """Report whether a ``|`` expression is a type union rather than arithmetic."""
    for inner in ast.walk(node):
        if isinstance(inner, ast.Constant) and inner.value is None:
            return True
        if isinstance(inner, ast.Subscript):
            target = inner.value
            if isinstance(target, ast.Name) and target.id in _GENERIC_NAMES:
                return True
            if isinstance(target, ast.Attribute) and target.attr in _GENERIC_NAMES:
                return True
    return False


def _has_postponed_annotations(tree: ast.Module) -> bool:
    """Report whether the module defers annotation evaluation to strings."""
    return any(
        isinstance(node, ast.ImportFrom)
        and node.module == "__future__"
        and any(alias.name == "annotations" for alias in node.names)
        for node in tree.body
    )


def _union_findings(tree: ast.Module, label: str, postponed: bool) -> list[str]:
    """Return one finding per PEP 604 union that is evaluated at import time."""
    annotations = _annotation_nodes(tree)
    findings: list[str] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.BinOp) or not isinstance(node.op, ast.BitOr):
            continue
        inside_annotation = id(node) in annotations
        if inside_annotation and postponed:
            continue
        if not inside_annotation and not _is_type_union(node):
            continue
        where = "annotation" if inside_annotation else "type alias"
        findings.append(
            f"{label}:{node.lineno}: PEP 604 union in an evaluated {where}; "
            f"Python {FLOOR_TEXT} raises TypeError at import"
        )
    return findings


def _source_findings(label: str, source: str, *, require_postponed: bool) -> list[str]:
    """Check one source string against all three rules and return its findings."""
    try:
        ast.parse(source, filename=label, feature_version=HOOK_PYTHON_FLOOR)
    except SyntaxError as exc:
        return [f"{label}:{exc.lineno}: not valid Python {FLOOR_TEXT} syntax: {exc.msg}"]
    tree = ast.parse(source, filename=label)
    postponed = _has_postponed_annotations(tree)
    findings: list[str] = []
    if require_postponed and not postponed:
        findings.append(
            f"{label}:1: missing `from __future__ import annotations`, so every "
            "annotation in this hook-path module is evaluated at import"
        )
    findings.extend(_union_findings(tree, label, postponed))
    return findings


def _relative(path: Path) -> str:
    """Return a repo-relative label for a checked file."""
    return str(path.relative_to(REPO_ROOT))


def _checked_sources(hook_text: str) -> tuple[tuple[Path, ...], tuple[tuple[str, str], ...]]:
    """Return the derived module closure and inline programs, in that order."""
    return _module_closure(_entry_scripts(hook_text)), _inline_programs(hook_text)


def selftest() -> int:
    """Prove each rule fires on a broken source and stays quiet on a good one."""
    failures: list[str] = []
    print("check_hook_python_floor.py selftest")

    good = "from __future__ import annotations\nimport os\n\nflags = os.O_WRONLY | os.O_CREAT\n"
    expect(
        not _source_findings("<good>", good, require_postponed=True),
        "a postponed-annotation module with an integer bitmask is clean",
        failures,
    )
    syntax = (
        "from __future__ import annotations\n\n\n"
        "def f(x):\n    match x:\n        case 1:\n            return 2\n    return 0\n"
    )
    found_syntax = _source_findings("<m>", syntax, require_postponed=True)
    expect(
        any("not valid Python" in item for item in found_syntax),
        f"a match statement is rejected at the Python {FLOOR_TEXT} floor",
        failures,
    )
    alias = "from __future__ import annotations\n\nState = tuple[int, str] | None\n"
    found_alias = _source_findings("<a>", alias, require_postponed=True)
    expect(
        any("evaluated type alias" in item for item in found_alias),
        "an evaluated `X | None` type alias is rejected (the #839 mechanism)",
        failures,
    )
    repaired = (
        "from __future__ import annotations\n\n"
        "from typing import Optional\n\nState = Optional[tuple[int, str]]\n"
    )
    expect(
        not _source_findings("<r>", repaired, require_postponed=True),
        "the Optional[...] repair of that alias is accepted",
        failures,
    )
    unpostponed = "def f(x: int | None) -> None:\n    return None\n"
    found = _source_findings("<u>", unpostponed, require_postponed=True)
    expect(
        any("missing `from __future__" in item for item in found),
        "a module without postponed annotations is rejected",
        failures,
    )
    expect(
        any("evaluated annotation" in item for item in found),
        "a union in an evaluated annotation is rejected",
        failures,
    )

    hook_text = PRE_COMMIT.read_text(encoding="utf-8")
    entries = _entry_scripts(hook_text)
    modules, inline = _checked_sources(hook_text)
    expect(
        "scripts/checks/check_hook_parity.py" in entries
        and "scripts/git/write-proof.py" in entries,
        "the derivation finds the hook's real entry scripts",
        failures,
    )
    expect(
        any(_relative(path) == "scripts/checks/hook_runtime_selftest.py" for path in modules),
        "the closure reaches the module #839 actually died in",
        failures,
    )
    expect(
        len(entries) >= ENTRY_FLOOR
        and len(modules) >= MODULE_FLOOR
        and len(inline) >= INLINE_FLOOR,
        f"the live derivation clears its floors ({len(entries)}/{len(modules)}/{len(inline)})",
        failures,
    )
    empty_modules, empty_inline = _checked_sources("nothing invokes the owner interpreter here\n")
    expect(
        not _entry_scripts("") and not empty_modules and not empty_inline,
        "a hook that invokes nothing derives an empty set instead of passing",
        failures,
    )
    return report(failures)


def main(argv: list[str]) -> int:
    """Check every source the hook runs under its pinned interpreter."""
    if "--selftest" in argv[1:]:
        return selftest()
    hook_text = PRE_COMMIT.read_text(encoding="utf-8")
    entries = _entry_scripts(hook_text)
    modules, inline = _checked_sources(hook_text)
    if len(entries) < ENTRY_FLOOR or len(modules) < MODULE_FLOOR or len(inline) < INLINE_FLOOR:
        print(
            "check_hook_python_floor.py: FATAL -- derived "
            f"{len(entries)} entry script(s), {len(modules)} module(s), "
            f"{len(inline)} inline program(s); floors are "
            f"{ENTRY_FLOOR}/{MODULE_FLOOR}/{INLINE_FLOOR}. A collapsed derivation "
            "reports a clean hook path because it read almost nothing.",
            file=sys.stderr,
        )
        return 2

    findings: list[str] = []
    for path in modules:
        findings.extend(
            _source_findings(
                _relative(path), path.read_text(encoding="utf-8"), require_postponed=True
            )
        )
    for label, source in inline:
        findings.extend(_source_findings(label, source, require_postponed=False))

    if not findings:
        print(
            f"check_hook_python_floor.py: {len(modules)} module(s) and "
            f"{len(inline)} inline program(s) run by {OWNER_PYTHON} hold at "
            f"Python {FLOOR_TEXT}."
        )
        return 0

    print(
        f"check_hook_python_floor.py: {len(findings)} finding(s) that break "
        f"the hook on Python {FLOOR_TEXT}:\n",
        file=sys.stderr,
    )
    for item in findings:
        print(f"  {item}", file=sys.stderr)
    print(
        "\nThe pre-commit hook pins "
        f"{OWNER_PYTHON}, which is Python 3.9 on the declared macOS host. Use "
        "`Optional[...]`/`Union[...]` in an evaluated position, keep "
        "`from __future__ import annotations`, or raise HOOK_PYTHON_FLOOR "
        "deliberately.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
