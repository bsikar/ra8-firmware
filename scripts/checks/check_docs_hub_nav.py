#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Nav coverage gate for the documentation hub (ADR-0005, #900).

Two failure modes, both fail-closed:

1. A nav entry points at a Markdown file that is not on disk. The hub's own
   ``mkdocs build --strict`` catches this too, but this check needs neither
   the generator nor the network, so CI can report it in a second.
2. A Markdown file under ``docs/`` is reachable by neither the nav nor the
   ``exclude_docs`` list. That is the failure worth having a gate for: a new
   manual lands, nothing errors, and the page is simply never published.

Run it directly or through ``just docs::hub_check``.
"""

from __future__ import annotations

import fnmatch
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CONFIG = ROOT / "mkdocs.yml"
DOCS_DIR_DEFAULT = "docs"


def _load_config() -> dict:
    """Parse mkdocs.yml, preferring PyYAML but never requiring it.

    The gate has to run on a bare checkout with no venv provisioned, so the
    fallback reads only the two keys this check needs rather than pretending
    to be a YAML parser.
    """
    text = CONFIG.read_text(encoding="utf-8")
    try:
        import yaml  # noqa: PLC0415
    except ImportError:
        return _load_config_fallback(text)
    return yaml.safe_load(text)


def _load_config_fallback(text: str) -> dict:
    docs_dir = DOCS_DIR_DEFAULT
    match = re.search(r"^docs_dir:\s*(\S+)\s*$", text, re.MULTILINE)
    if match:
        docs_dir = match.group(1)

    nav_paths: list[str] = []
    in_nav = False
    for line in text.splitlines():
        if re.match(r"^nav:\s*$", line):
            in_nav = True
            continue
        if in_nav and line and not line.startswith((" ", "\t", "#")):
            in_nav = False
        if in_nav:
            found = re.search(r"([A-Za-z0-9_./-]+\.md)\s*$", line)
            if found:
                nav_paths.append(found.group(1))

    excludes: list[str] = []
    in_exclude = False
    for line in text.splitlines():
        if re.match(r"^exclude_docs:\s*\|", line):
            in_exclude = True
            continue
        if in_exclude:
            if line.startswith((" ", "\t")) and line.strip():
                excludes.append(line.strip())
            elif line.strip():
                in_exclude = False

    return {"docs_dir": docs_dir, "_nav_paths": nav_paths, "exclude_docs": "\n".join(excludes)}


def _nav_paths(config: dict) -> list[str]:
    if "_nav_paths" in config:
        return config["_nav_paths"]

    paths: list[str] = []

    def walk(node: object) -> None:
        if isinstance(node, str):
            if node.endswith(".md"):
                paths.append(node)
        elif isinstance(node, list):
            for item in node:
                walk(item)
        elif isinstance(node, dict):
            for value in node.values():
                walk(value)

    walk(config.get("nav", []))
    return paths


def _excluded(rel: str, patterns: list[str]) -> bool:
    for pattern in patterns:
        if pattern.endswith("/"):
            if rel.startswith(pattern) or f"/{pattern}" in f"/{rel}":
                return True
        elif fnmatch.fnmatch(rel, pattern) or fnmatch.fnmatch(pathlib.PurePath(rel).name, pattern):
            return True
    return False


def main() -> int:
    if not CONFIG.is_file():
        print(f"check_docs_hub_nav: {CONFIG} not found", file=sys.stderr)
        return 1

    config = _load_config()
    docs_dir = ROOT / str(config.get("docs_dir", DOCS_DIR_DEFAULT))
    nav = _nav_paths(config)
    patterns = [
        line.strip()
        for line in str(config.get("exclude_docs", "") or "").splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]

    missing = [entry for entry in nav if not (docs_dir / entry).is_file()]

    on_disk = sorted(
        str(path.relative_to(docs_dir)) for path in docs_dir.rglob("*.md") if path.is_file()
    )
    in_nav = set(nav)
    orphans = [rel for rel in on_disk if rel not in in_nav and not _excluded(rel, patterns)]

    duplicates = sorted({entry for entry in nav if nav.count(entry) > 1})

    failed = False
    if missing:
        failed = True
        print("check_docs_hub_nav: nav entries with no file on disk:", file=sys.stderr)
        for entry in missing:
            print(f"  {docs_dir.name}/{entry}", file=sys.stderr)

    if orphans:
        failed = True
        print(
            "check_docs_hub_nav: Markdown under docs/ that the hub would never "
            "publish (add it to the nav in mkdocs.yml, or to exclude_docs with "
            "a reason):",
            file=sys.stderr,
        )
        for rel in orphans:
            print(f"  {docs_dir.name}/{rel}", file=sys.stderr)

    if duplicates:
        failed = True
        print("check_docs_hub_nav: duplicated nav entries:", file=sys.stderr)
        for entry in duplicates:
            print(f"  {docs_dir.name}/{entry}", file=sys.stderr)

    if failed:
        return 1

    print(
        f"check_docs_hub_nav: OK -- {len(nav)} nav entries, "
        f"{len(on_disk)} Markdown files under {docs_dir.name}/, no orphans."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
