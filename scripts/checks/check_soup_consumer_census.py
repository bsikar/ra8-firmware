#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every consumer count a SOUP record states shall be re-derived from the tree.

A SOUP record's "how widely is this used here" sentence is the number a reader
trusts when deciding how much of the firmware a vendored component sits under.
Those numbers were transcribed by hand and then left: ``docs/SOUP/threadx.md``
claimed 45 example applications against a tree that held 47, and
``sbom_registry.py`` restated the same 45 in its component description.
Nothing recomputed either one, so both aged quietly and a stale count read
exactly like a current one.

Each stated count now sits behind a machine-readable census marker::

    <!-- consumer-census: key=threadx total=47 hw_validated=39 c6=6 -->

This gate re-derives every field from ``examples/**/CMakeLists.txt`` -- the same
``USES`` clause the build itself consumes -- and fails on any disagreement. It
also asserts each number in the marker appears literally in the document body,
so the prose a person actually reads cannot drift away from the marker that is
being checked.

The derivation parses ``USES`` the way ``ra8_add_app()`` does: the clause runs
until the next recognised keyword or the closing paren, so a multi-line ``USES``
(``wifi_hal_join``) counts the same as a single-line one.

An app does not have to call ``ra8_add_app()`` in its own CMakeLists. Two do
not: ``c6_camera_livestream`` and ``c6_camera_mjpeg`` each ``include()`` a
shared ``c6_camera_server.cmake`` and delegate to the
``c6_camera_server_add_app()`` wrapper that declares ``USES`` on their behalf.
A sweep that reads only each app's own file walks past them and reports a
clean, self-consistent under-count, which is the defect this gate exists to
prevent. So the derivation follows one ``include()`` hop into a wrapper that
calls ``ra8_add_app()`` and attributes its clause to the including app, and
``UNRESOLVED`` makes an app whose declaration it cannot find anywhere a
finding rather than a silent zero.

Run::

    check_soup_consumer_census.py             # scan every SOUP record
    check_soup_consumer_census.py --selftest  # prove both directions

Exit 0 when every stated count matches the tree, 1 on any finding, 2 when the
sweep collapses below a floor (a read that saw nothing must not report success).
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from selftest_assert import expect, report

REPO_ROOT = Path(__file__).resolve().parents[2]

SOUP_DIR = "docs/SOUP"
"""Where the records this gate reads live."""

APPS_DIR = "examples"
"""The tree the counts are derived from."""

MARKER_RE = re.compile(r"<!--\s*consumer-census:\s*(?P<fields>[^>]*?)\s*-->")
"""The census marker. One per stated component, any number per document."""

TIER_FIELDS = {
    "total": "",
    "hw_validated": "/hw_validated/",
    "c6": "/c6/",
    "unsupported": "/_unsupported/",
}
"""Marker field -> the path fragment an app must contain to be counted in it.
``total`` counts every app that declares the component at all."""

APP_FLOOR = 100
"""Fewest app CMakeLists a healthy sweep may see. The tree held far more when
this gate landed; a read finding fewer has collapsed (wrong cwd, sparse
checkout) and is fatal rather than clean."""

USES_STOP_WORDS = (
    ")",
    "LIBS",
    "OFF_TARGET_LIBS",
    "NSC_SRCS",
    "EXTRA_SRCS",
    "AUX_SRCS",
    "SRAM_TEXT",
)
"""Keywords that end a ``USES`` clause, mirroring ra8_add_app's argument list."""

TOKEN_RE = re.compile(r"\A[a-z0-9_]+\Z")
"""A middleware name. Anything else in the clause is punctuation or a variable."""

ADD_APP_RE = re.compile(r"\bra8_add_app\s*\(")
"""The call that actually declares an app. An app CMakeLists either makes this
call itself or reaches it through a wrapper it includes."""

INCLUDE_RE = re.compile(r'\binclude\s*\(\s*"?([^")\s]+)"?')
"""An ``include()`` target. Only ones resolving to a file in the tree matter."""

CMAKE_VAR_RE = re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*\}")
"""A CMake variable reference inside an include path, e.g. ``${_d}``."""


def uses_of(text: str) -> set[str]:
    """Return the middleware named in every ``USES`` clause of one CMakeLists."""
    found: set[str] = set()
    for match in re.finditer(r"\bUSES\b", text):
        tail = text[match.end() :]
        stop = len(tail)
        for word in USES_STOP_WORDS:
            index = tail.find(word)
            if index != -1:
                stop = min(stop, index)
        clause = " ".join(line.split("#", 1)[0] for line in tail[:stop].splitlines())
        found.update(token for token in clause.split() if TOKEN_RE.match(token))
    return found


def _included_wrappers(app: Path, root: Path) -> list[Path]:
    """Return the files ``app`` includes that themselves call ``ra8_add_app()``.

    An include path is written against CMake variables the build expands
    (``${_d}`` is the discovered repo root, ``${CMAKE_CURRENT_SOURCE_DIR}`` the
    app directory). Rather than emulate CMake, take the literal tail after the
    last variable and look for it under the repo root and under the app.
    """
    wrappers: list[Path] = []
    for raw in INCLUDE_RE.findall(app.read_text(errors="replace")):
        tail = CMAKE_VAR_RE.split(raw)[-1].lstrip("/")
        if not tail.endswith(".cmake"):
            continue
        for base in (root, app.parent):
            candidate = base / tail
            if candidate.is_file() and ADD_APP_RE.search(
                candidate.read_text(errors="replace")
            ):
                wrappers.append(candidate)
                break
    return wrappers


def declared_uses(app: Path, root: Path) -> tuple[set[str], bool]:
    """Return (middleware ``app`` declares, whether its declaration was found).

    The second element is False for an app that neither calls ``ra8_add_app()``
    nor reaches a wrapper that does: its ``USES`` is somewhere this sweep cannot
    read, so counting it as zero would under-report silently.
    """
    text = app.read_text(errors="replace")
    found = uses_of(text)
    resolved = bool(ADD_APP_RE.search(text))
    for wrapper in _included_wrappers(app, root):
        found |= uses_of(wrapper.read_text(errors="replace"))
        resolved = True
    return found, resolved


def app_files(root: Path) -> list[Path]:
    """Return every CMakeLists under ``examples/`` that stands for an app.

    Every app reaches ``ra8_add_app()`` somehow: by naming it, by including
    ``cmake/ra8_add_app.cmake``, or by including a wrapper that calls it. A
    file mentioning none of those is a subdirectory aggregator, not an app.
    """
    apps = sorted((root / APPS_DIR).rglob("CMakeLists.txt"))
    return [
        p
        for p in apps
        if "ra8_add_app" in p.read_text(errors="replace") or _included_wrappers(p, root)
    ]


def consumers(root: Path, key: str) -> list[Path]:
    """Return every app under ``examples/`` that declares ``key``, wrappers followed."""
    return [p for p in app_files(root) if key in declared_uses(p, root)[0]]


def unresolved_apps(root: Path) -> list[Path]:
    """Return the apps whose ``USES`` declaration this sweep could not locate."""
    return [p for p in app_files(root) if not declared_uses(p, root)[1]]


def app_count(root: Path) -> int:
    """Return how many app CMakeLists the sweep saw at all, for the floor."""
    return len(app_files(root))


def parse_marker(fields: str) -> tuple[dict[str, int], str | None, str | None]:
    """Return (counts, key, error) for one marker's field list."""
    counts: dict[str, int] = {}
    key: str | None = None
    for item in fields.split():
        if "=" not in item:
            return counts, key, f"field {item!r} is not name=value"
        name, _, value = item.partition("=")
        if name == "key":
            key = value
            continue
        if name not in TIER_FIELDS:
            return counts, key, f"unknown field {name!r} (known: {' '.join(TIER_FIELDS)})"
        if not value.isdigit():
            return counts, key, f"field {name}={value!r} is not a count"
        counts[name] = int(value)
    if key is None:
        return counts, key, "no key= field; the marker must name its component"
    if "total" not in counts:
        return counts, key, f"key={key} states no total="
    return counts, key, None


# A count is "stated in prose" only where the sentence is talking about
# consumers. Searching the whole document for the bare number was unsound: a
# netxduo total of 7 was satisfied by "IEC 61508-3 Section 7.4.2.12" in the
# boilerplate, so the prose half of this check passed on a document that never
# tells the reader the number at all. That is the self-agreeing gate this epic
# exists to remove, and it was in the gate itself.
CONSUMER_NOUN_RE = re.compile(
    r"\b(app|apps|application|applications|consumer|consumers|example|examples"
    r"|demo|demos|image|images|target|targets)\b",
    re.IGNORECASE,
)

WORD_NUMBERS = {
    1: "one", 2: "two", 3: "three", 4: "four", 5: "five", 6: "six", 7: "seven",
    8: "eight", 9: "nine", 10: "ten", 11: "eleven", 12: "twelve",
}

# A sentence, loosely: prose is wrapped, so a bare newline does not end one,
# but a blank line or a bullet boundary does.
_SENTENCE_SPLIT_RE = re.compile(r"(?:\n\s*\n|(?<=[.:;])\s+|\n\s*[-*]\s)")


def stated_in_prose(count: int, prose: str) -> bool:
    """True when ``count`` appears in a sentence that is about consumers.

    Both spellings count: "7" and "seven". The consumer noun must sit in the
    same sentence, so a section number or a version string elsewhere in the
    record cannot satisfy a consumer count.
    """
    word = WORD_NUMBERS.get(count)
    forms = rf"\b{count}\b" if word is None else rf"(\b{count}\b|\b{word}\b)"
    number_re = re.compile(forms, re.IGNORECASE)
    for sentence in _SENTENCE_SPLIT_RE.split(prose):
        if number_re.search(sentence) and CONSUMER_NOUN_RE.search(sentence):
            return True
    return False


def check_marker(doc: Path, root: Path, fields: str, prose: str) -> list[str]:
    """Return the findings for one census marker, checked against the document's prose.

    ``prose`` is the body with every marker stripped out: a marker must not
    satisfy its own prose-appearance check by quoting its own number.
    """
    counts, key, error = parse_marker(fields)
    if error is not None:
        return [f"{doc.name}: {error}"]
    paths = [str(p) for p in consumers(root, str(key))]
    findings: list[str] = []
    for name, count in sorted(counts.items()):
        fragment = TIER_FIELDS[name]
        actual = sum(1 for p in paths if fragment in p) if fragment else len(paths)
        if actual != count:
            findings.append(
                f"{doc.name}: key={key} {name}={count} but the tree holds {actual}; "
                f"re-derive the marker and the prose together"
            )
        elif not stated_in_prose(count, prose):
            findings.append(
                f"{doc.name}: key={key} {name}={count} matches the tree but the number "
                "appears nowhere in the prose; the marker is checking a claim no reader sees"
            )
    return findings


def scan(root: Path) -> tuple[list[str], int, int]:
    """Return (findings, markers_seen, apps_seen) for the SOUP records under ``root``."""
    findings: list[str] = []
    seen = 0
    soup = root / SOUP_DIR
    for doc in sorted(soup.glob("*.md")) if soup.is_dir() else []:
        body = doc.read_text(errors="replace")
        prose = MARKER_RE.sub(" ", body)
        for match in MARKER_RE.finditer(body):
            seen += 1
            findings.extend(check_marker(doc, root, match.group("fields"), prose))
    if seen:
        findings.extend(
            f"UNRESOLVED: {p.parent.relative_to(root)} declares an app but neither calls "
            "ra8_add_app() nor includes a wrapper that does; every census count is "
            "under-reporting it"
            for p in unresolved_apps(root)
        )
    return findings, seen, app_count(root)


def _seed(root: Path) -> None:
    """Build a throwaway tree: three direct apps plus one that delegates.

    App ``d`` never calls ``ra8_add_app()`` itself; it includes a shared wrapper
    that does, exactly like ``c6_camera_livestream``. A sweep that reads only
    each app's own file counts two consumers here instead of three.
    """
    for name, uses in (("a", "demolib"), ("b", "demolib other"), ("c", "other")):
        app = root / APPS_DIR / "ek_ra8d2" / "hw_validated" / name
        app.mkdir(parents=True)
        (app / "CMakeLists.txt").write_text(f"ra8_add_app(\n  NAME {name}\n  USES {uses}\n)\n")

    shared = root / APPS_DIR / "ek_ra8d2" / "common" / "server"
    shared.mkdir(parents=True)
    (shared / "server.cmake").write_text(
        "function(server_add_app)\n"
        "  ra8_add_app(\n    NAME ${APP_NAME}\n    USES demolib\n    LIBS shared\n  )\n"
        "endfunction()\n"
    )
    delegating = root / APPS_DIR / "ek_ra8d2" / "hw_validated" / "d"
    delegating.mkdir(parents=True)
    (delegating / "CMakeLists.txt").write_text(
        'include("${_d}/cmake/ra8_add_app.cmake")\n'
        'include("${_d}/examples/ek_ra8d2/common/server/server.cmake")\n'
        "server_add_app(NAME d)\n"
    )
    (root / SOUP_DIR).mkdir(parents=True)


def _seed_unreadable(root: Path) -> Path:
    """Add an app whose declaration is nowhere this sweep can read it."""
    orphan = root / APPS_DIR / "ek_ra8d2" / "hw_validated" / "e"
    orphan.mkdir(parents=True)
    (orphan / "CMakeLists.txt").write_text(
        'include("${_d}/cmake/ra8_add_app.cmake")\n'
        "generated_add_app(NAME e)\n"
    )
    return orphan


def _write_doc(root: Path, marker: str, prose: str) -> Path:
    """Write the fixture SOUP record and return its path."""
    doc = root / SOUP_DIR / "demolib.md"
    doc.write_text(f"# demolib\n\n{marker}\n\n{prose}\n")
    return doc


def _selftest_prose(root: Path, good: str, failures: list[str]) -> None:
    """Assert the prose half of the check, both directions.

    Its own bug is the reason these exist: searching the whole record for the
    bare number let "Section 7.4.2.12" stand in for a consumer count of 7.
    """
    for prose, want_finding, label in (
        ("No numerals here at all.", True, "a marker the prose does not state fires"),
        (
            "Per IEC 61508-3 Section 3.4.2.12 this record is accepted.",
            True,
            "a section number carrying the count does not satisfy the prose check",
        ),
        ("Three apps declare it.", False, "the count spelled as a word satisfies the check"),
        ("3 consumers declare it.", False, "the count as a digit satisfies the check"),
        (
            "Three trees are vendored.\n\nSome apps declare it.",
            True,
            "the count and the consumer noun must share a sentence",
        ),
    ):
        _write_doc(root, good, prose)
        findings, _, _ = scan(root)
        hit = any("appears nowhere in the prose" in f for f in findings)
        expect(hit if want_finding else not findings, label, failures)


def selftest() -> int:
    """Prove the gate fires on a stale count and stays quiet on a current one."""
    failures: list[str] = []
    good = "<!-- consumer-census: key=demolib total=3 hw_validated=3 -->"
    prose = "Three apps declare it: 3 of them under hw_validated."
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _seed(root)

        _write_doc(root, good, prose)
        findings, seen, _ = scan(root)
        expect(not findings, "a current census is quiet", failures)
        expect(seen == 1, "the marker is discovered", failures)

        expect(
            any(p.parent.name == "d" for p in consumers(root, "demolib")),
            "an app that declares USES through an included wrapper is counted",
            failures,
        )

        _write_doc(root, "<!-- consumer-census: key=demolib total=2 -->", "Two: 2 apps.")
        findings, _, _ = scan(root)
        expect(
            any("the tree holds 3" in f for f in findings),
            "the count a wrapper-blind sweep would report fires",
            failures,
        )

        _write_doc(root, "<!-- consumer-census: key=demolib total=4 -->", "Four: 4 apps.")
        findings, _, _ = scan(root)
        expect(any("the tree holds 3" in f for f in findings), "a stale total fires", failures)

        _write_doc(root, "<!-- consumer-census: key=demolib total=3 hw_validated=1 -->", prose)
        findings, _, _ = scan(root)
        expect(any("the tree holds 3" in f for f in findings), "a stale tier count fires", failures)

        _selftest_prose(root, good, failures)

        _write_doc(root, "<!-- consumer-census: key=demolib total=2 tier=2 -->", prose)
        findings, _, _ = scan(root)
        expect(any("unknown field" in f for f in findings), "an unknown field fires", failures)

        _write_doc(root, "<!-- consumer-census: total=2 -->", prose)
        findings, _, _ = scan(root)
        expect(any("no key=" in f for f in findings), "a keyless marker fires", failures)

        _write_doc(root, "<!-- consumer-census: key=demolib hw_validated=2 -->", prose)
        findings, _, _ = scan(root)
        expect(any("states no total=" in f for f in findings), "a totalless marker fires", failures)

        orphan = _seed_unreadable(root)
        _write_doc(root, good, prose)
        findings, _, _ = scan(root)
        expect(
            any("UNRESOLVED" in f for f in findings),
            "an app whose declaration cannot be located fires",
            failures,
        )
        (orphan / "CMakeLists.txt").unlink()
        orphan.rmdir()

        (root / SOUP_DIR / "demolib.md").unlink()
        findings, seen, _ = scan(root)
        expect(seen == 0 and not findings, "a tree with no markers is quiet", failures)
    return report(failures)


def main(argv: list[str]) -> int:
    """Re-derive every stated consumer count and compare it with the tree.

    Returns 0 when every count matches, 1 on findings, 2 when the sweep
    collapsed below ``APP_FLOOR``.
    """
    ap = argparse.ArgumentParser(description="Re-derive SOUP consumer counts from the tree")
    ap.add_argument("--selftest", action="store_true", help="assert both directions")
    args = ap.parse_args(argv[1:])
    if args.selftest:
        return selftest()

    findings, seen, apps = scan(REPO_ROOT)
    if apps < APP_FLOOR:
        print(
            f"check_soup_consumer_census.py: FATAL -- only {apps} app CMakeLists discovered "
            f"under {APPS_DIR}/, floor is {APP_FLOOR}. A collapsed read reports success "
            "because it saw nothing.",
            file=sys.stderr,
        )
        return 2
    if findings:
        print(f"\n{len(findings)} consumer-census finding(s):\n", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(
        f"check_soup_consumer_census.py: {seen} census marker(s) re-derived from "
        f"{apps} app CMakeLists, every stated count current."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
