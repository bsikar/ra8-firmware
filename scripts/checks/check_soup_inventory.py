#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the hand-written SOUP inventories agree with the component registry.

Two of this repository's three SOUP catalogues are hand-maintained Markdown:
``THIRD_PARTY_LICENSES.md`` (the aggregated attribution inventory) and
``docs/SOUP/README.md`` (the qualification index).  Both files told the reader
they were "generated and validated by ``scripts/gen/gen_sbom.py``" -- and
neither was.  ``gen_sbom.py --write`` writes exactly one artifact, the SBOM at
``docs/sbom/ra8-firmware.cdx.json``; ``--check`` validates registry against
tree against SBOM.  Nothing read either Markdown file, so nothing could notice
when one stopped describing the tree (#631).

That is why a vendored component could be missing from the inventory entirely
(doxygen-awesome, #629), why a host-tool pin could sit in the licence file with
no registry entry and therefore no OSV query (vela, #628), and why the index
could link a ``docs/SOUP/*.md`` that had been deleted.  Each was found by a
person reading prose, which is not a gate.

What this checker asserts
-------------------------

R1  Every registry component's in-tree path is named in
    ``THIRD_PARTY_LICENSES.md``.  A component may be named by its own path or
    by an ancestor directory of at least two path segments -- the co-processor
    firmware is catalogued as ``coprocessor/esp32c6/`` rather than by the
    per-component subdirectory.  One segment is NOT enough: ``docs/`` must not
    stand in for ``docs/doxygen_theme``, which is precisely how #629 hid.

R2  Every vendored path named in the inventory table belongs to a registry
    component.  An inventory row with no registry entry is a component the
    SBOM, the digests and the OSV scan never see.

R3  Every ``docs/SOUP/*.md`` the index links exists.

R4  Every ``docs/SOUP/*.md`` in the directory is linked by the index.  A
    qualification record no index reaches is one no auditor finds.

Each rule is a cross-check between two independently maintained artifacts, so
nothing here compares a value with itself.  The floors below make a collapsed
parse an error rather than a clean run: this gate exists because a check that
silently stopped seeing its subject is this tree's dominant defect class.

Run::

    check_soup_inventory.py             # the gate
    check_soup_inventory.py --selftest  # prove every rule fires and stays quiet

Exit 0 when the catalogues agree, 1 on a disagreement, 2 when the scan itself
collapsed.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "gen"))

from sbom_registry import REGISTRY  # noqa: E402  # import needs the path set above

LICENSES_REL = "THIRD_PARTY_LICENSES.md"
SOUP_DIR_REL = "docs/SOUP"
INDEX_REL = "docs/SOUP/README.md"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_VACUOUS = 2

# Floors, not targets.  Today's tree carries 20 registry components, 19 doc
# links and 19 qualification records; a parse that returns fewer than this has
# stopped reading its subject, whatever it reports about agreement.
MIN_COMPONENTS = 15
MIN_INVENTORY_PATHS = 15
MIN_INDEX_LINKS = 15
MIN_SOUP_DOCS = 15

# A path in this repository's prose is always a code span and always carries a
# separator; a bare word in backticks is a macro or a filename, not a location.
_CODE_SPAN_RE = re.compile(r"`([^`\n]+)`")
_MD_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+\.md)\)")
_SECTION_RE = re.compile(r"^##\s+(.*?)\s*$", re.MULTILINE)

# Roots a vendored component can live under.  Used only to decide which paths
# in the inventory table are component claims (R2); R1 needs no such filter.
VENDOR_PREFIXES = ("libs/", "apps/", "coprocessor/", "tools/", "docs/")

# A component location is a directory or a vendored data file, never one of this
# repository's own Markdown records and never a glob.  The inventory prose cites
# `docs/SOUP/<name>.md` beside the rows it describes, and the scope section
# writes directory families as globs; reading either as a component claim would
# fail every row that documents itself.
NOT_A_COMPONENT_SUFFIX = (".md",)


def code_span_paths(text: str) -> set[str]:
    """Every code span in ``text`` that names a repository path."""
    found: set[str] = set()
    for match in _CODE_SPAN_RE.finditer(text):
        span = match.group(1).strip()
        if "/" not in span or " " in span or span.startswith(("http", "<")):
            continue
        found.add(span.rstrip("/"))
    return found


def section_text(text: str, heading: str) -> str:
    """The body of the ``## <heading>`` section, or "" when it is absent."""
    marks = [(m.group(1), m.start(), m.end()) for m in _SECTION_RE.finditer(text)]
    for index, (title, _start, body_start) in enumerate(marks):
        if title.lower() != heading.lower():
            continue
        end = marks[index + 1][1] if index + 1 < len(marks) else len(text)
        return text[body_start:end]
    return ""


def naming_candidates(path: str) -> set[str]:
    """``path`` plus every ancestor of at least two segments.

    Two segments is the floor on purpose.  ``coprocessor/esp32c6/`` is how the
    licence file catalogues the C6 firmware, and that is a real, specific
    location; ``docs/`` standing in for ``docs/doxygen_theme`` is not.
    """
    parts = path.split("/")
    return {"/".join(parts[:n]) for n in range(2, len(parts) + 1)}


def registry_path_failures(components: tuple, licenses_text: str) -> list[str]:
    """R1: a registry component the attribution inventory never names."""
    named = code_span_paths(licenses_text)
    failures: list[str] = []
    for comp in components:
        if naming_candidates(comp.path) & named:
            continue
        failures.append(
            f"{comp.key}: registry path '{comp.path}' is named nowhere in "
            f"{LICENSES_REL}; the component ships with no attribution row."
        )
    return failures


def inventory_claimed_paths(licenses_text: str) -> set[str]:
    """Every vendored path the ``## Inventory`` table claims."""
    body = section_text(licenses_text, "Inventory")
    return {
        p
        for p in code_span_paths(body)
        if p.startswith(VENDOR_PREFIXES)
        and "*" not in p
        and not p.endswith(NOT_A_COMPONENT_SUFFIX)
    }


def orphan_inventory_failures(components: tuple, licenses_text: str) -> list[str]:
    """R2: an inventory row naming a path no registry component owns."""
    registered = {comp.path for comp in components}
    return [
        f"{LICENSES_REL}: inventory row '{path}' has no entry in "
        "scripts/gen/sbom_registry.py, so it reaches neither the SBOM nor the OSV scan."
        for path in sorted(inventory_claimed_paths(licenses_text) - registered)
    ]


def index_doc_links(index_text: str) -> set[str]:
    """Every ``docs/SOUP/*.md`` sibling the index links."""
    return {
        target
        for target in _MD_LINK_RE.findall(index_text)
        if "/" not in target and target != "README.md"
    }


def index_link_failures(index_text: str, present_docs: set[str]) -> list[str]:
    """R3: an index link pointing at a qualification record that is gone."""
    return [
        f"{INDEX_REL}: links '{name}', which does not exist under {SOUP_DIR_REL}/."
        for name in sorted(index_doc_links(index_text) - present_docs)
    ]


def orphan_doc_failures(index_text: str, present_docs: set[str]) -> list[str]:
    """R4: a qualification record the index never reaches."""
    return [
        f"{SOUP_DIR_REL}/{name}: qualification record is not linked from {INDEX_REL}."
        for name in sorted(present_docs - index_doc_links(index_text))
    ]


def vacuity_failures(
    components: tuple, licenses_text: str, index_text: str, present_docs: set[str]
) -> list[str]:
    """Report every input that collapsed below its floor."""
    measured = (
        ("registry components", len(components), MIN_COMPONENTS),
        ("inventory paths", len(inventory_claimed_paths(licenses_text)), MIN_INVENTORY_PATHS),
        ("index doc links", len(index_doc_links(index_text)), MIN_INDEX_LINKS),
        ("SOUP records on disk", len(present_docs), MIN_SOUP_DOCS),
    )
    return [
        f"{label}: scan found {count}, floor is {floor} -- the scan collapsed, "
        "so 'no disagreement' would mean nothing."
        for label, count, floor in measured
        if count < floor
    ]


def soup_docs_on_disk(root: Path) -> set[str]:
    """Every qualification record under ``docs/SOUP/`` except the index."""
    soup_dir = root / SOUP_DIR_REL
    if not soup_dir.is_dir():
        return set()
    return {p.name for p in soup_dir.glob("*.md") if p.name != "README.md"}


def collect_failures(components: tuple, root: Path) -> tuple[list[str], list[str]]:
    """Run every rule, returning (vacuity failures, agreement failures)."""
    licenses_text = (root / LICENSES_REL).read_text(encoding="utf-8")
    index_text = (root / INDEX_REL).read_text(encoding="utf-8")
    docs = soup_docs_on_disk(root)
    vacuity = vacuity_failures(components, licenses_text, index_text, docs)
    failures = [
        *registry_path_failures(components, licenses_text),
        *orphan_inventory_failures(components, licenses_text),
        *index_link_failures(index_text, docs),
        *orphan_doc_failures(index_text, docs),
    ]
    return vacuity, failures


def run_check(root: Path = REPO_ROOT, components: tuple = REGISTRY) -> int:
    """Cross-check both Markdown catalogues against the registry."""
    vacuity, failures = collect_failures(components, root)
    for line in vacuity:
        print(f"check_soup_inventory: FATAL -- {line}", file=sys.stderr)
    if vacuity:
        return EXIT_VACUOUS
    for line in failures:
        print(f"  FAIL {line}", file=sys.stderr)
    if failures:
        print(
            f"check_soup_inventory: {len(failures)} catalogue disagreement(s). "
            "Update the registry and the Markdown together.",
            file=sys.stderr,
        )
        return EXIT_FAIL
    print(
        f"check_soup_inventory: {len(components)} registry components agree with "
        f"{LICENSES_REL} and {INDEX_REL} "
        f"({len(soup_docs_on_disk(root))} qualification records, all indexed)."
    )
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Parse arguments and dispatch to the check or the selftest."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="prove every rule both ways")
    args = parser.parse_args(argv)
    if args.selftest:
        from soup_inventory_selftest import run_selftest  # noqa: PLC0415  # selftest-only import

        return run_selftest()
    return run_check()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
