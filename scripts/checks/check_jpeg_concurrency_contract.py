#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a thread-safety claim must be backed by the unit's own state (#893).

``libs/ra8_jpeg/inc/ra8_jpeg_sw.h`` advertised ``ra8_jpeg_sw_decode()`` as
"Thread-safe (re-entrant): all state lives on the caller's stack" and
``ra8_jpeg_sw_encode()`` as "Thread-safe", while both keep their whole working
set in shared statics. The documentation is honest again, but nothing in the
tree tied either claim to the code, so the contradiction survived every gate
for the claim's whole life and would come back the same way.

This is the mechanisable half of that issue: the claim is checked against the
sources rather than reviewed. Two rules, over each public entry point the
header declares and the translation unit that defines it:

* **R1 unsupported claim** -- a doc block that claims thread safety for an
  entry point whose defining TU carries mutable ``static`` state (file-scope,
  or inside that function). ``static const`` tables are immutable and exempt.
* **R2 silent contract** -- an entry point whose defining TU carries mutable
  ``static`` state and whose doc block says nothing either way. Silence is how
  the next caller inherits an assumption instead of a contract.

Scope is the ``ra8_jpeg`` unit, the one the issue names. Widening it to every
first-party public header is a bigger baseline and its own change; the rules
here are unit-agnostic, so that widening is a scope edit, not a rewrite.

Exit status:
  * 0 -- every claim in scope is backed by the sources
  * 1 -- one or more findings (printed as ``<rule>: <detail>``)
  * 2 -- the scan collapsed below its floors, which would otherwise pass
         vacuously
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from selftest_assert import expect, report


def _unit() -> tuple[str, str]:
    """Return the scanned unit: its public header and its source directory."""
    return "libs/ra8_jpeg/inc/ra8_jpeg_sw.h", "libs/ra8_jpeg/src"


def _floors() -> tuple[int, int, int]:
    """Return the vacuity floors: entry points, mutable statics, claims.

    Four public entry points, seven mutable statics and one surviving
    thread-safety claim were measured on this unit when the gate landed. A scan
    that finds fewer has stopped matching, which is this repository's dominant
    defect class, so ``main`` exits 2 rather than reporting agreement.
    """
    return 3, 4, 1


def _patterns() -> dict[str, re.Pattern[str]]:
    """Return the compiled declaration, definition and claim patterns."""
    return {
        "static": re.compile(
            r"^\s*static\s+(?!const\b)(?!inline\b)"
            r"(?P<type>[A-Za-z_][\w\s*]*?)\b(?P<name>[A-Za-z_]\w*)\s*"
            r"(?P<arr>\[[^;]*\])?\s*(?P<tail>=|;)"
        ),
        "decl": re.compile(r"\b(?P<name>[a-z_]\w*)\s*\("),
        "not_safe": re.compile(r"not\s+thread-safe", re.IGNORECASE),
        "safe": re.compile(r"thread-safe", re.IGNORECASE),
    }


def mutable_statics(root: Path, src_rel: str) -> dict[str, list[tuple[str, int]]]:
    """Return, per TU filename, every mutable ``static`` object as (name, line)."""
    pats = _patterns()
    found: dict[str, list[tuple[str, int]]] = {}
    for path in sorted((root / src_rel).glob("*.c")):
        rows: list[tuple[str, int]] = []
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = pats["static"].match(line)
            if match is not None and "(" not in match.group("name"):
                rows.append((match.group("name"), number))
        found[path.name] = rows
    return found


def defining_tu(root: Path, src_rel: str, entry: str) -> str | None:
    """Return the TU filename that defines @p entry, or None when unresolved."""
    needle = re.compile(rf"^[A-Za-z_][\w\s*]*\b{re.escape(entry)}\s*\(")
    for path in sorted((root / src_rel).glob("*.c")):
        for line in path.read_text(encoding="utf-8").splitlines():
            if needle.match(line) and not line.rstrip().endswith(";"):
                return path.name
    return None


def documented_entries(root: Path, header_rel: str) -> list[tuple[str, str]]:
    """Return every documented public entry point as (name, doc-block text)."""
    pats = _patterns()
    text = (root / header_rel).read_text(encoding="utf-8")
    entries: list[tuple[str, str]] = []
    for chunk in text.split("/**")[1:]:
        block, _, trailer = chunk.partition("*/")
        declaration = trailer.split(";")[0]
        if "(" not in declaration:
            continue
        names = [m.group("name") for m in pats["decl"].finditer(declaration)]
        public = [name for name in names if name.startswith("ra8_")]
        if public:
            entries.append((public[0], block))
    return entries


def claim_failures(root: Path, header_rel: str, src_rel: str) -> list[str]:
    """Return every thread-safety claim the unit's own sources do not support."""
    pats = _patterns()
    statics = mutable_statics(root, src_rel)
    failures: list[str] = []
    for entry, block in documented_entries(root, header_rel):
        tu = defining_tu(root, src_rel, entry)
        if tu is None:
            continue
        shared = statics.get(tu, [])
        disclaimed = pats["not_safe"].search(block) is not None
        claimed = pats["safe"].search(pats["not_safe"].sub("", block)) is not None
        if not shared:
            continue
        witness = ", ".join(f"`{name}`:{line}" for name, line in shared)
        if claimed:
            failures.append(
                f"R1 unsupported claim: {header_rel}: `{entry}()` claims thread "
                f"safety, but {src_rel}/{tu} carries mutable static state "
                f"({witness})"
            )
        elif not disclaimed:
            failures.append(
                f"R2 silent contract: {header_rel}: `{entry}()` says nothing "
                f"about concurrency, but {src_rel}/{tu} carries mutable static "
                f"state ({witness}); state the serialisation requirement"
            )
    return failures


def _write(path: Path, text: str) -> None:
    """Write one selftest fixture file, creating its parents."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _fixture(root: Path, header: str, source: str) -> tuple[str, str]:
    """Materialise a fixture unit and return its (header, source dir) paths."""
    header_rel, src_rel = "inc/unit.h", "src"
    _write(root / header_rel, header)
    _write(root / src_rel / "codec.c", source)
    return header_rel, src_rel


def _stateful_source() -> str:
    """Return a fixture TU that carries one mutable and one immutable static."""
    return (
        "static const uint8_t s_table[4] = {0};\n"
        "static ctx_t s_ctx;\n"
        "ra8_err_t ra8_unit_decode(void)\n"
        "{\n"
        "  return use(&s_ctx);\n"
        "}\n"
        "ra8_err_t ra8_unit_probe(void)\n"
        "{\n"
        "  return 0;\n"
        "}\n"
    )


def _stateless_source() -> str:
    """Return a fixture TU with no mutable static state at all."""
    return (
        "static const char* s_tag = \"UNIT\";\n"
        "ra8_err_t ra8_unit_probe(void)\n"
        "{\n"
        "  return 0;\n"
        "}\n"
    )


def _selftest_rules(tmp: Path, failures: list[str]) -> None:
    """Prove both rules fire on their own breakage and stay quiet when clean."""
    # The rule is per translation unit: a file-scope mutable static is shared by
    # every entry point the TU defines, so the fixture keeps the disclaimed
    # stateful entry point on its own and proves a legal claim separately, in
    # the `stateless` case below.
    honest = (
        "/** @note Not thread-safe and not re-entrant: shared context. */\n"
        "ra8_err_t ra8_unit_decode(void);\n"
    )
    clean = tmp / "clean"
    header_rel, src_rel = _fixture(clean, honest, _stateful_source())
    expect(
        claim_failures(clean, header_rel, src_rel) == [],
        "clean fixture: a stateful entry point that states its contract passes",
        failures,
    )

    claimed = tmp / "r1"
    header_rel, src_rel = _fixture(
        claimed,
        honest.replace("/** @note Not thread-safe and not re-entrant: shared context. */",
                       "/** @note Thread-safe (re-entrant). */"),
        _stateful_source(),
    )
    hits = claim_failures(claimed, header_rel, src_rel)
    expect(
        any(hit.startswith("R1") and "ra8_unit_decode" in hit for hit in hits),
        "R1 fires when a stateful entry point claims thread safety",
        failures,
    )
    expect(
        any("s_ctx" in hit for hit in hits) and all("s_table" not in hit for hit in hits),
        "R1 witnesses the mutable static and exempts the const table",
        failures,
    )

    silent = tmp / "r2"
    header_rel, src_rel = _fixture(
        silent,
        "/** @note Single-threaded boot path. */\nra8_err_t ra8_unit_decode(void);\n",
        _stateful_source(),
    )
    hits = claim_failures(silent, header_rel, src_rel)
    expect(
        any(hit.startswith("R2") and "ra8_unit_decode" in hit for hit in hits),
        "R2 fires when a stateful entry point says nothing about concurrency",
        failures,
    )

    stateless = tmp / "stateless"
    header_rel, src_rel = _fixture(
        stateless,
        "/** @note Thread-safe and re-entrant. */\nra8_err_t ra8_unit_probe(void);\n",
        _stateless_source(),
    )
    expect(
        claim_failures(stateless, header_rel, src_rel) == [],
        "a claim stays legal when its TU holds no mutable static state",
        failures,
    )

    undefined = tmp / "undefined"
    header_rel, src_rel = _fixture(
        undefined,
        "/** @note Thread-safe. */\nra8_err_t ra8_unit_elsewhere(void);\n",
        _stateful_source(),
    )
    expect(
        claim_failures(undefined, header_rel, src_rel) == [],
        "an entry point defined outside this unit is not judged here",
        failures,
    )


def _selftest_scope(tmp: Path, failures: list[str]) -> None:
    """Prove the scan resolves entry points, statics and the floors."""
    fixture = tmp / "scope"
    header_rel, src_rel = _fixture(
        fixture,
        "/** @note Not thread-safe. */\nra8_err_t ra8_unit_decode(void);\n",
        _stateful_source(),
    )
    entries = [name for name, _ in documented_entries(fixture, header_rel)]
    expect(entries == ["ra8_unit_decode"], "documented entry points resolve", failures)
    expect(
        defining_tu(fixture, src_rel, "ra8_unit_decode") == "codec.c",
        "the defining TU resolves from its definition, not a declaration",
        failures,
    )
    statics = mutable_statics(fixture, src_rel)["codec.c"]
    expect([name for name, _ in statics] == ["s_ctx"], "only mutable statics count", failures)
    min_entries, min_statics, _ = _floors()
    expect(
        len(entries) < min_entries and len(statics) < min_statics,
        "the fixture sits below the real floors (so the floors are load-bearing)",
        failures,
    )


def selftest() -> int:
    """Run the both-direction rule proofs and the scope/floor proofs."""
    print("check_jpeg_concurrency_contract.py --selftest")
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        _selftest_rules(tmp, failures)
        _selftest_scope(tmp, failures)
    return report(failures)


def main(argv: list[str]) -> int:
    """Run the selftest, or check the ra8_jpeg unit against its own sources."""
    if "--selftest" in argv:
        return selftest()

    root = Path(__file__).resolve().parents[2]
    header_rel, src_rel = _unit()
    pats = _patterns()
    entries = documented_entries(root, header_rel)
    statics = mutable_statics(root, src_rel)
    total_statics = sum(len(rows) for rows in statics.values())
    claims = [
        name
        for name, block in entries
        if pats["safe"].search(pats["not_safe"].sub("", block)) is not None
    ]
    min_entries, min_statics, min_claims = _floors()
    if len(entries) < min_entries or total_statics < min_statics or len(claims) < min_claims:
        sys.stderr.write(
            "check_jpeg_concurrency_contract.py: scan collapsed "
            f"({len(entries)} entry point(s), {total_statics} mutable static(s), "
            f"{len(claims)} claim(s)); floors are "
            f"{min_entries}/{min_statics}/{min_claims}.\n"
        )
        return 2

    failures = claim_failures(root, header_rel, src_rel)
    if failures:
        sys.stderr.write(
            "check_jpeg_concurrency_contract.py: thread-safety claim(s) the "
            "sources do not support:\n"
        )
        for failure in failures:
            sys.stderr.write(f"  {failure}\n")
        sys.stderr.write(f"\n{len(failures)} finding(s); baseline is zero.\n")
        return 1

    print(
        f"check_jpeg_concurrency_contract.py: {len(entries)} public entry point(s), "
        f"{total_statics} mutable static(s) across {len(statics)} TU(s), "
        f"{len(claims)} thread-safety claim(s) -- all backed by the sources."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
