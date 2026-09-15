#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ruff: noqa: ANN401,D103,EM101,EM102,PLR0911,TRY003,TRY004
"""Validate the navigable, user-facing surface of the repository Justfile.

The checker renders the root ``just`` screen and every zero-argument module
screen, extracts the displayed ``just ...`` entries, and treats them as edges
in a menu graph. Every public recipe, alias, and module must be reachable from
the root screen. Public recipe bodies are not executed; the existing reference
checker owns literal command extraction from authored help text, while this
checker owns the rendered runtime menu surface and module-entry contract.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from scripts.checks import check_just_references as references


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent.parent


def _just_dump(repo_root: Path) -> dict[str, Any]:
    """Load Just's recursive machine-readable recipe surface."""
    just_bin = shutil.which("just")
    if just_bin is None:
        raise RuntimeError("just is required to validate navigation")
    proc = subprocess.run(  # noqa: S603 -- resolved executable and fixed argv
        [just_bin, "--dump", "--dump-format", "json"],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or "unknown error"
        raise RuntimeError(f"just --dump failed: {detail}")
    loaded = json.loads(proc.stdout)
    if not isinstance(loaded, dict):
        raise RuntimeError("just returned a non-object recipe dump")
    return loaded


def _modules(dump: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    """Return every module and its recursive dump node."""
    found: list[tuple[str, dict[str, Any]]] = []

    def walk(node: dict[str, Any], prefix: str) -> None:
        children = node.get("modules")
        if not isinstance(children, dict):
            raise RuntimeError(f"invalid module dump below {prefix or '<root>'}")
        for name, child in children.items():
            if not isinstance(child, dict):
                raise RuntimeError(f"invalid child module below {prefix or '<root>'}")
            path = f"{prefix}::{name}" if prefix else str(name)
            if child.get("first") is not None:
                found.append((path, child))
            walk(child, path)

    walk(dump, "")
    return found


def _body_line(row: Any) -> str:
    """Flatten one Just dump body row, retaining literal shell text."""
    if isinstance(row, str):
        return row
    if not isinstance(row, list):
        return ""
    pieces: list[str] = []
    for piece in row:
        if isinstance(piece, str):
            pieces.append(piece)
        elif isinstance(piece, list) and piece and piece[0] == "variable":
            pieces.append("<value>")
        elif isinstance(piece, list) and piece and piece[0] == "call":
            pieces.append("<call>")
    return "".join(pieces)


def _parameter_value(parameter: dict[str, Any]) -> str:
    """Choose a harmless value for a required Just parameter."""
    name = str(parameter.get("name", "value")).lower()
    pattern = parameter.get("pattern")
    if pattern and isinstance(pattern, str):
        if "on" in pattern and "off" in pattern:
            return "off"
        if "all" in pattern:
            return "all"
    if any(token in name for token in ("host", "ssh", "target")):
        return "navigation-host"
    if any(token in name for token in ("app", "lib", "tool", "file", "path", "output")):
        return "navigation-probe"
    if any(token in name for token in ("duration", "seconds", "wait", "port", "jobs")):
        return "1"
    if any(token in name for token in ("confirm", "force", "check", "strict", "skip", "detached")):
        return "0"
    return "navigation-probe"


def _argv_for(recipe: dict[str, Any]) -> list[str]:
    """Build arguments for dry-run rendering without invoking a recipe."""
    argv: list[str] = []
    parameters = recipe.get("parameters", [])
    if not isinstance(parameters, list):
        raise RuntimeError(f"invalid parameter metadata for {recipe.get('name')}")
    for parameter in parameters:
        if not isinstance(parameter, dict) or parameter.get("flag"):
            continue
        if parameter.get("default") is not None:
            continue
        argv.append(_parameter_value(parameter))
    return argv


def _public_recipes(dump: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    """Return every public qualified recipe, excluding module defaults."""
    found: list[tuple[str, dict[str, Any]]] = []

    def walk(node: dict[str, Any], prefix: str) -> None:
        recipes = node.get("recipes")
        children = node.get("modules")
        if not isinstance(recipes, dict) or not isinstance(children, dict):
            raise RuntimeError(f"invalid recipe dump below {prefix or '<root>'}")
        for name, recipe in recipes.items():
            if name == "default" or not isinstance(recipe, dict) or recipe.get("private"):
                continue
            qualified = f"{prefix}::{name}" if prefix else str(name)
            found.append((qualified, recipe))
        for name, child in children.items():
            if not isinstance(child, dict):
                raise RuntimeError(f"invalid child module below {prefix or '<root>'}")
            child_path = f"{prefix}::{name}" if prefix else str(name)
            walk(child, child_path)

    walk(dump, "")
    return found


def _recipe_and_alias_surfaces(
    dump: dict[str, Any],
) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
    """Return qualified recipe metadata and qualified alias targets."""
    recipes: dict[str, dict[str, Any]] = {}
    aliases: dict[str, str] = {}

    def walk(node: dict[str, Any], prefix: str) -> None:
        raw_recipes = node.get("recipes")
        raw_aliases = node.get("aliases")
        children = node.get("modules")
        if not isinstance(raw_recipes, dict) or not isinstance(raw_aliases, dict):
            raise RuntimeError(f"invalid recipe/alias dump below {prefix or '<root>'}")
        if not isinstance(children, dict):
            raise RuntimeError(f"invalid module dump below {prefix or '<root>'}")
        for name, recipe in raw_recipes.items():
            if name != "default" and isinstance(recipe, dict) and not recipe.get("private"):
                recipes[f"{prefix}::{name}" if prefix else str(name)] = recipe
        for name, alias in raw_aliases.items():
            if not isinstance(alias, dict) or not isinstance(alias.get("target"), str):
                raise RuntimeError(f"invalid alias metadata for {prefix}::{name}")
            aliases[f"{prefix}::{name}" if prefix else str(name)] = (
                f"{prefix}::{alias['target']}" if prefix else alias["target"]
            )
        for name, child in children.items():
            if not isinstance(child, dict):
                raise RuntimeError(f"invalid child module below {prefix or '<root>'}")
            walk(child, f"{prefix}::{name}" if prefix else str(name))

    walk(dump, "")
    return recipes, aliases


def _has_confirmation(recipe: dict[str, Any]) -> bool:
    """Return whether Just requires interactive confirmation for this recipe."""
    attributes = recipe.get("attributes", [])
    return isinstance(attributes, list) and any(
        isinstance(attribute, dict) and "confirm" in attribute for attribute in attributes
    )


def _source_name(node: dict[str, Any]) -> str:
    """Return the Just source path recorded for a module dump node."""
    source = node.get("source")
    if isinstance(source, str):
        return Path(source).name
    return "the module Justfile"


def _screen_commands(output: str) -> set[str]:
    """Extract command names displayed by one rendered Just help screen."""
    commands: set[str] = set()
    screen_path = Path("rendered-screen.just")
    for line in output.splitlines():
        for reference in references.references_in_line(screen_path, line):
            if not reference.dynamic_suffix:
                commands.add(reference.recipe)
    return commands


def _screen_recipe(node: dict[str, Any]) -> dict[str, Any] | None:
    """Return a module's default recipe metadata, if it has one."""
    recipes = node.get("recipes")
    if not isinstance(recipes, dict):
        return None
    default = recipes.get("default")
    return default if isinstance(default, dict) else None


def _run_screen(just_bin: str, repo_root: Path, module: str | None) -> tuple[set[str], str | None]:
    """Render one help screen; return commands or a concise execution error."""
    argv = [] if module is None else [module]
    try:
        proc = subprocess.run(  # noqa: S603 -- resolved executable and fixed argv
            [just_bin, *argv],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except subprocess.TimeoutExpired:
        return set(), "screen did not finish within 10 seconds"
    output = "\n".join(part for part in (proc.stdout, proc.stderr) if part)
    if proc.returncode != 0:
        detail = output.strip().splitlines()
        return set(), detail[-1] if detail else f"just exited {proc.returncode}"
    return _screen_commands(output), None


def _navigation_findings(
    modules: set[str],
    recipes: set[str],
    aliases: set[str],
    screens: dict[str, set[str]],
    broken: dict[str, str],
) -> list[str]:
    """Return dangling menu entries and public commands unreachable from root."""
    public = modules | recipes | aliases
    reachable: set[str] = set()
    queue = ["<root>"]
    visited_screens: set[str] = set()
    findings: list[str] = []
    while queue:
        screen = queue.pop(0)
        if screen in visited_screens:
            continue
        visited_screens.add(screen)
        for command in sorted(screens.get(screen, set())):
            if command not in public:
                findings.append(
                    f"dangling menu entry `{command}` on `{screen}`; no public Just "
                    "recipe, alias, or module matches it"
                )
                continue
            if command not in reachable:
                reachable.add(command)
            if command in modules:
                if command in broken:
                    findings.append(
                        f"`just {command}` is listed but its screen is unavailable: "
                        f"{broken[command]}"
                    )
                elif command not in visited_screens and command in screens:
                    queue.append(command)

    findings.extend(
        f"unreachable public command `{command}`; no displayed menu path from `just`"
        for command in sorted(public - reachable)
    )
    return findings


def check_navigation(repo_root: Path, dump: dict[str, Any]) -> list[str]:
    """Render all screens and require every public Just command to be reachable."""
    just_bin = shutil.which("just")
    if just_bin is None:
        return ["just is required to render navigation screens"]
    module_nodes = dict(_modules(dump))
    recipes, aliases = _recipe_and_alias_surfaces(dump)
    modules = set(module_nodes)
    screens: dict[str, set[str]] = {}
    broken: dict[str, str] = {}
    root_commands, root_error = _run_screen(just_bin, repo_root, None)
    if root_error is not None:
        broken["<root>"] = root_error
    screens["<root>"] = root_commands
    for module, node in module_nodes.items():
        default = _screen_recipe(node)
        if default is None:
            broken[module] = (
                f"{_source_name(node)} has no default recipe; add a zero-argument help screen"
            )
            continue
        parameters = default.get("parameters", [])
        if parameters:
            names = ", ".join(
                str(parameter.get("name", "?"))
                for parameter in parameters
                if isinstance(parameter, dict)
            )
            broken[module] = (
                f"{_source_name(node)} default requires argument(s) `{names}`; "
                "keep it zero-argument and move execution to a named recipe"
            )
            continue
        commands, error = _run_screen(just_bin, repo_root, module)
        if error is not None:
            broken[module] = f"{_source_name(node)} returned an error: {error}"
        screens[module] = commands
    findings = _navigation_findings(modules, set(recipes), set(aliases), screens, broken)
    if root_error is not None:
        findings.insert(0, f"root `just` screen is unavailable: {root_error}")
    return findings


def check_dry_runs(repo_root: Path, dump: dict[str, Any]) -> list[str]:
    """Render every public recipe and alias without executing recipe bodies."""
    just_bin = shutil.which("just")
    if just_bin is None:
        return ["just is required to validate recipe navigation"]
    recipes, aliases = _recipe_and_alias_surfaces(dump)
    invocable = sorted(recipes.keys() | aliases.keys())
    findings: list[str] = []
    for name in invocable:
        target = aliases.get(name, name)
        recipe = recipes.get(target)
        if recipe is None:
            findings.append(f"{name}: alias target {target!r} is not a public recipe")
            continue
        if _has_confirmation(recipe):
            continue
        argv = _argv_for(recipe)
        proc = subprocess.run(  # noqa: S603 -- resolved executable and generated safe argv
            [just_bin, "--dry-run", name, *argv],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        if proc.returncode != 0:
            detail = proc.stderr.strip().splitlines()
            message = detail[-1] if detail else "Just rejected the command"
            findings.append(f"{name}: dry-run navigation failed: {message}")
    return findings


def _selftest() -> int:
    """Exercise reachability, aliases, dangling entries, and broken screens."""
    failures: list[str] = []
    complete = _navigation_findings(
        modules={"A"},
        recipes={"A::B", "A::C", "A::D"},
        aliases={"A::alias"},
        screens={"<root>": {"A"}, "A": {"A::B", "A::C", "A::D", "A::alias"}},
        broken={},
    )
    if complete:
        failures.append("a complete rendered menu graph was rejected")

    missing = _navigation_findings(
        modules={"A"},
        recipes={"A::B", "A::C", "A::D"},
        aliases=set(),
        screens={"<root>": {"A"}, "A": {"A::B", "A::C"}},
        broken={},
    )
    if not any("unreachable public command `A::D`" in finding for finding in missing):
        failures.append("unreachable menu command was not reported")
    if any("A::B" in finding or "A::C" in finding for finding in missing):
        failures.append("reachable menu commands were reported as missing")

    dangling = _navigation_findings(
        modules={"A"},
        recipes={"A::B"},
        aliases=set(),
        screens={"<root>": {"A"}, "A": {"A::B", "A::ghost"}},
        broken={},
    )
    if not any("dangling menu entry `A::ghost`" in finding for finding in dangling):
        failures.append("dangling menu entry was not reported")

    broken = _navigation_findings(
        modules={"A"},
        recipes=set(),
        aliases=set(),
        screens={"<root>": {"A"}},
        broken={"A": "default screen failed"},
    )
    if not any("screen is unavailable" in finding for finding in broken):
        failures.append("broken module screen was not reported")
    if failures:
        for failure in failures:
            print(f"selftest: check_just_navigation.py FAIL: {failure}", file=sys.stderr)
        return 1
    print("selftest: check_just_navigation.py OK (reachability cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="run detector selftests")
    args = parser.parse_args()
    if args.selftest:
        return _selftest()
    repo_root = _repo_root()
    try:
        dump = _just_dump(repo_root)
        findings = check_navigation(repo_root, dump)
        findings.extend(check_dry_runs(repo_root, dump))
    except (OSError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
        print(f"check-just-navigation: ERROR: {exc}", file=sys.stderr)
        return 1
    if findings:
        print("check-just-navigation: missing/non-navigable items:", file=sys.stderr)
        for finding in findings:
            print(f"  - {finding}", file=sys.stderr)
        print(f"check-just-navigation: FAIL ({len(findings)} finding(s))", file=sys.stderr)
        return 1
    print("Just navigation clean: every public command is reachable from rendered menus")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
