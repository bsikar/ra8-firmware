#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate docs/DRIVER_STATUS.md from tree truth alone.

The page this replaces was a hand-maintained snapshot of every HAL driver against
its FSP parity benchmark, and it rotted the way every hand-maintained inventory
in this tree has rotted: 33 of its 109 rows named a ``.c`` file that had since
been deleted, it still described ``libs/ra8_net/`` (removed), every rollup count
was low, and the sweep SHAs it cited had been rewritten out of history. A reader
could not tell which third of it was true.

So the page is DERIVED now, and it derives only what the tree can answer.

**A driver is a source family, not a file.** ``libs/ra8_hal/src/ra8_ceu.c``,
``ra8_ceu_init_regs.c`` and the headers ``ra8_ceu.h`` / ``ra8_ceu_api.h`` /
``ra8_ceu_types.h`` are one driver, ``ra8_ceu``. The family root is the shortest
base name no other base name is a prefix of, matched at an underscore boundary
so ``ra8_etha`` stays separate from ``ra8_eth``. Keying on the header stem
instead looked simpler and was wrong: ``ra8_ceu_api.h`` declares ``ra8_ceu_*``
functions, so 53 companion headers reported an API of zero and read as a
documentation gap that did not exist.

Derived per driver:

* **Sources** -- the ``.c`` files in the family.
* **Headers** -- the public headers in the family.
* **API** -- distinct ``<root>_*`` functions those headers declare, comments
  stripped first so a name that only appears in prose is not counted.
* **Host tests** -- files under ``tests/`` naming at least one of the driver's
  symbols. Matched by CONTENT, not by the ``tests/test_<driver>.c`` filename
  convention, because plenty of drivers are exercised from a differently-named
  suite and a filename-only rule would report those as untested.
* **Apps** -- app directories under ``examples/`` (and ``apps/``) naming one of
  its symbols.

What it deliberately does NOT carry, because nothing in the tree knows it: the
FSP parity class (feature-complete / partial / placeholder / scaffold), the
bench-validation verdict, audit dates and commit SHAs. Guessing any of those is
how the old page came to describe a tree that no longer existed. That status
lives in the issue tracker.

Scope is ``libs/ra8_hal`` -- the drivers. A library that reaches its hardware
through an injected bus vtable (the project's Dependency-Inversion seam) is not
mechanically distinguishable from any other library, so this generator does not
guess at one; ``libs/README.md`` indexes those.

Run ``python3 scripts/gen/gen_driver_status.py`` to refresh the committed page;
the ``artefact-freshness`` gate fails when it drifts. ``--selftest`` proves the
derivation fires in both directions on a throwaway tree.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ARTEFACT = "docs/DRIVER_STATUS.md"

#: The driver library. Everything on the page is derived from under here.
HAL_DIR = Path("libs") / "ra8_hal"

#: Roots scanned for callers. ``apps`` is included so a product that drives a
#: peripheral counts the same as an example; it is skipped when absent.
APP_ROOTS = ("examples", "apps")

#: Header families that are not drivers: the HUM-derived register layouts, and
#: module-private headers.
_NOT_A_DRIVER = ("_regs.h", "_internal.h")

#: An identifier in this project's namespace, used as a function.
_CALL_RE = re.compile(r"\b([a-z][a-z0-9_]*)\s*\(")

#: A C or C++ comment, stripped before a header's API surface is counted.
_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)

_SOURCE_SUFFIXES = (".c", ".cpp")


@dataclass(frozen=True)
class Driver:
    """One driver family and everything the tree says about it.

    Attributes:
        name: The family root, which is also the driver's symbol prefix.
        headers: Repo-relative POSIX paths of its public headers.
        sources: Repo-relative POSIX paths of the sources implementing it.
        api: Count of distinct ``<name>_*`` functions its headers declare.
        tests: Repo-relative POSIX paths of host tests naming one of its symbols.
        apps: App directory names whose sources name one of its symbols.
    """

    name: str
    headers: tuple[str, ...]
    sources: tuple[str, ...]
    api: int
    tests: tuple[str, ...]
    apps: tuple[str, ...]


def _read(path: Path) -> str:
    """Return ``path`` decoded permissively; unreadable bytes never abort a scan."""
    return path.read_text(encoding="utf-8", errors="replace")


def _extends(name: str, root: str) -> bool:
    """Return True when ``name`` is ``root`` or a child of it at an underscore.

    The boundary is what keeps ``ra8_etha`` out of the ``ra8_eth`` family: they
    are different peripherals whose names happen to share a prefix.
    """
    return name == root or name.startswith(f"{root}_")


def _family_roots(names: set[str]) -> list[str]:
    """Reduce ``names`` to the roots no other name is a prefix of."""
    return sorted(n for n in names if not any(m != n and _extends(n, m) for m in names))


def _public_headers(root: Path) -> list[Path]:
    """Return the public driver headers under ``libs/ra8_hal/inc``, sorted."""
    inc = root / HAL_DIR / "inc"
    if not inc.is_dir():
        return []
    return sorted(p for p in inc.glob("*.h") if not any(p.name.endswith(s) for s in _NOT_A_DRIVER))


def _sources(root: Path) -> list[Path]:
    """Return the implementation files under ``libs/ra8_hal/src``, sorted."""
    src = root / HAL_DIR / "src"
    if not src.is_dir():
        return []
    return sorted(p for p in src.iterdir() if p.suffix in _SOURCE_SUFFIXES)


def _owner(name: str, ordered_roots: list[str]) -> str | None:
    """Return the longest root owning ``name``, or None when none does."""
    return next((r for r in ordered_roots if _extends(name, r)), None)


def _source_owner(stem: str, ordered_roots: list[str]) -> str | None:
    """Return the family owning a source named ``stem``, or None.

    Four sources predate the ``ra8_`` file-naming convention -- ``adc.c``,
    ``adc_selfdiag.c``, ``gpio.c``, ``timer.c`` -- while defining ``ra8_*``
    symbols, so a bare name that matches nothing is retried prefixed. One that
    still matches nothing is left alone rather than guessed into a family.
    """
    return _owner(stem, ordered_roots) or _owner(f"ra8_{stem}", ordered_roots)


def driver_roots(root: Path) -> list[str]:
    """Return every driver family root the HAL defines.

    The public headers define the families: they reduce correctly, because
    ``ra8_ceu`` is a prefix of its ``ra8_ceu_api`` and ``ra8_ceu_types``
    companions. A source belonging to none of them forms a family of its own, so
    an implementation with no public header still appears.
    """
    hdr_roots = _family_roots({h.stem for h in _public_headers(root)})
    ordered = sorted(hdr_roots, key=len, reverse=True)
    orphans = {p.stem for p in _sources(root) if _source_owner(p.stem, ordered) is None}
    return sorted(set(hdr_roots) | set(_family_roots(orphans)))


def _api_count(root: Path, headers: list[str], name: str) -> int:
    """Count the distinct ``<name>_*`` functions ``headers`` declare."""
    found: set[str] = set()
    for rel in headers:
        text = _COMMENT_RE.sub(" ", _read(root / rel))
        found |= {m for m in _CALL_RE.findall(text) if m.startswith(f"{name}_")}
    return len(found)


def _scan_callers(root: Path, roots: list[str]) -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """Attribute host tests and app directories to the drivers they name.

    Each file is read once and its symbols mapped to owners, so the cost is one
    pass over the tree rather than one pass per driver.

    Args:
        root: Repository root being scanned.
        roots: Every driver family root.

    Returns:
        ``(tests, apps)`` -- for each root, the test paths and the app directory
        names that name at least one of its symbols.
    """
    ordered = sorted(roots, key=len, reverse=True)
    tests: dict[str, set[str]] = {r: set() for r in roots}
    apps: dict[str, set[str]] = {r: set() for r in roots}

    def _app_directory(path: Path, base: Path) -> str | None:
        for parent in path.parents:
            if parent == base.parent:
                break
            if (parent / "CMakeLists.txt").is_file():
                return parent.name
        return None

    def _walk(base: Path, sink: dict[str, set[str]], label: str) -> None:
        if not base.is_dir():
            return
        for path in sorted(base.rglob("*")):
            if path.suffix not in _SOURCE_SUFFIXES or not path.is_file():
                continue
            key = (
                path.relative_to(root).as_posix() if label == "rel" else _app_directory(path, base)
            )
            if key is None:
                continue
            for symbol in set(_CALL_RE.findall(_read(path))):
                owner = _owner(symbol, ordered)
                if owner is not None:
                    sink[owner].add(key)

    _walk(root / "tests", tests, "rel")
    for app_root in APP_ROOTS:
        _walk(root / app_root, apps, "dir")
    return tests, apps


def collect(root: Path) -> list[Driver]:
    """Return every driver the HAL defines, sorted by name."""
    roots = driver_roots(root)
    ordered = sorted(roots, key=len, reverse=True)
    headers: dict[str, list[str]] = {r: [] for r in roots}
    sources: dict[str, list[str]] = {r: [] for r in roots}
    for path in _public_headers(root):
        owner = _owner(path.stem, ordered)
        if owner is not None:
            headers[owner].append(path.relative_to(root).as_posix())
    for path in _sources(root):
        owner = _source_owner(path.stem, ordered)
        if owner is not None:
            sources[owner].append(path.relative_to(root).as_posix())
    tests, apps = _scan_callers(root, roots)
    return [
        Driver(
            name=name,
            headers=tuple(headers[name]),
            sources=tuple(sources[name]),
            api=_api_count(root, headers[name], name),
            tests=tuple(sorted(tests[name])),
            apps=tuple(sorted(apps[name])),
        )
        for name in roots
    ]


_HEADER = """<!-- GENERATED by scripts/gen/gen_driver_status.py -- do not edit by hand. -->

# Driver status

Every driver in `libs/ra8_hal`, and what the tree says about it. Regenerate with:

```sh
python3 scripts/gen/gen_driver_status.py
```

The `artefact-freshness` gate fails when the committed copy drifts from a fresh
run, so this page cannot rot the way its hand-maintained predecessor did -- that
one ended up naming 33 deleted source files and a library that no longer existed.

**It carries only what is derivable.** Status beyond tree-derivable facts -- FSP
parity, whether a driver has run on real silicon, when it was last audited -- is
written down nowhere the generator can read, so it is not guessed at here. That
belongs in the [issue tracker](https://github.com/bsikar/ra8-firmware/issues).

A driver is a source FAMILY: `ra8_ceu.c` and `ra8_ceu_init_regs.c`, with the
headers `ra8_ceu.h`, `ra8_ceu_api.h` and `ra8_ceu_types.h`, are one row. The
family root is the shortest header name no other is a prefix of, matched at an
underscore boundary so `ra8_etha` stays separate from `ra8_eth`. A source whose
name predates the `ra8_` file convention (`adc.c`, `gpio.c`) is matched by its
prefixed name too; one that matches no header family gets a row of its own.

Where each column comes from:

- **Src** / **Hdr** -- files in the family under `libs/ra8_hal/src` and
  `libs/ra8_hal/inc`. The HUM-derived `*_regs.h` layouts and module-private
  `*_internal.h` headers are not drivers and are excluded.
- **API** -- distinct `<driver>_*` functions those headers declare, comments
  stripped first.
- **Tests** -- files under `tests/` naming at least one of the driver's symbols.
  Matched by content rather than filename, so a driver exercised from a
  differently-named suite is not reported as untested.
- **Apps** -- app directories under `examples/` (and `apps/`, when present)
  naming one of its symbols.

A driver with no sources is a contract declared but not implemented here. One
with no tests and no apps is exactly the gap this page exists to make visible.

"""


def _summary(drivers: list[Driver]) -> str:
    """Return the one-line rollup above the table."""
    tested = sum(1 for d in drivers if d.tests)
    used = sum(1 for d in drivers if d.apps)
    untouched = sum(1 for d in drivers if not d.tests and not d.apps)
    headerless = sum(1 for d in drivers if not d.sources)
    return (
        f"{len(drivers)} drivers: {tested} named by a host test, {used} named by an "
        f"app, {untouched} by neither, {headerless} declared but not implemented here.\n"
    )


def render(drivers: list[Driver]) -> str:
    """Return the full page text for ``drivers``, newline-terminated."""
    rows = ["| Driver | Src | Hdr | API | Tests | Apps |", "|---|---:|---:|---:|---:|---:|"]
    rows += [
        f"| `{d.name}` | {len(d.sources)} | {len(d.headers)} | {d.api} | "
        f"{len(d.tests)} | {len(d.apps)} |"
        for d in drivers
    ]
    return _HEADER + _summary(drivers) + "\n" + "\n".join(rows) + "\n"


def write(root: Path) -> int:
    """Rewrite the committed page from ``root``; return 0 on success."""
    page = render(collect(root))
    target = root / ARTEFACT
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(page, encoding="ascii")
    print(f"gen_driver_status.py: wrote {ARTEFACT} ({len(page)} bytes)")
    return 0


def _seed_tree(root: Path) -> None:
    """Build a throwaway HAL with the shapes the derivation has to get right."""
    inc = root / HAL_DIR / "inc"
    src = root / HAL_DIR / "src"
    inc.mkdir(parents=True)
    src.mkdir(parents=True)
    # One family spread over three headers and two sources.
    (inc / "ra8_foo.h").write_text('#pragma once\n#include "ra8_foo_api.h"\n', encoding="ascii")
    (inc / "ra8_foo_api.h").write_text(
        "#pragma once\nra8_err_t ra8_foo_init(void);\nra8_err_t ra8_foo_read(int x);\n"
        "/* ra8_foo_ghost() only appears in prose. */\n",
        encoding="ascii",
    )
    (src / "ra8_foo.c").write_text("void ra8_foo_init(void) {}\n", encoding="ascii")
    (src / "ra8_foo_dma.c").write_text("void ra8_foo_dma(void) {}\n", encoding="ascii")
    # A near-miss name that must NOT fold into ra8_foo.
    (inc / "ra8_fooa.h").write_text("#pragma once\nra8_err_t ra8_fooa_init(void);\n", "ascii")
    (src / "ra8_fooa.c").write_text("void ra8_fooa_init(void) {}\n", encoding="ascii")
    # Declared, never implemented here.
    (inc / "ra8_bar.h").write_text("#pragma once\nra8_err_t ra8_bar_init(void);\n", "ascii")
    # Not drivers.
    (inc / "ra8_foo_regs.h").write_text("#pragma once\n", encoding="ascii")
    (inc / "ra8_foo_internal.h").write_text("#pragma once\n", encoding="ascii")

    tests = root / "tests" / "src"
    tests.mkdir(parents=True)
    (tests / "test_something_else.c").write_text("void t(void) { ra8_foo_init(); }\n", "ascii")

    app = root / "examples" / "tier" / "blinky"
    src = app / "src"
    src.mkdir(parents=True)
    (app / "CMakeLists.txt").write_text("add_executable(blinky src/main.c)\n", encoding="ascii")
    (src / "main.c").write_text("int main(void) { ra8_foo_read(1); }\n", encoding="ascii")


def _selftest_cases(root: Path) -> list[tuple[str, bool]]:
    """Return one ``(label, passed)`` tuple per asserted derivation property."""
    _seed_tree(root)
    # ra8_foo_init + ra8_foo_read. ra8_foo_ghost() appears only in a comment,
    # so an API count of 2 rather than 3 is what proves prose is not counted.
    expect_api = 2
    expect_sources = 2  # ra8_foo.c + ra8_foo_dma.c
    expect_headers = 2  # ra8_foo.h + ra8_foo_api.h
    by_name = {d.name: d for d in collect(root)}
    page = render(list(by_name.values()))
    return [
        (
            "companion headers fold into one family",
            set(by_name) == {"ra8_foo", "ra8_fooa", "ra8_bar"},
        ),
        ("register and internal headers are not drivers", "ra8_foo_regs" not in by_name),
        ("a near-miss prefix stays its own driver", by_name["ra8_fooa"].sources != ()),
        ("the family collects every source", len(by_name["ra8_foo"].sources) == expect_sources),
        (
            "the family collects every public header",
            len(by_name["ra8_foo"].headers) == expect_headers,
        ),
        ("API spans the family and excludes prose names", by_name["ra8_foo"].api == expect_api),
        ("a header with no source is still listed", by_name["ra8_bar"].sources == ()),
        (
            "a differently-named host test still counts",
            by_name["ra8_foo"].tests == ("tests/src/test_something_else.c",),
        ),
        ("an app naming a symbol is attributed", by_name["ra8_foo"].apps == ("blinky",)),
        (
            "a driver nothing names reports neither",
            not by_name["ra8_bar"].tests and not by_name["ra8_bar"].apps,
        ),
        ("the page is pure ASCII", page.isascii()),
        ("the page names every driver", all(f"`{n}`" in page for n in by_name)),
    ]


def selftest() -> int:
    """Prove the derivation fires in both directions on a throwaway tree."""
    with tempfile.TemporaryDirectory() as tmp:
        cases = _selftest_cases(Path(tmp))
    failures = 0
    for label, passed in cases:
        print(f"  [{'ok' if passed else 'FAIL'}] {label}")
        failures += 0 if passed else 1
    if failures:
        sys.stderr.write(f"gen_driver_status.py --selftest: {failures} case(s) failed.\n")
        return 1
    print(f"gen_driver_status.py --selftest: all {len(cases)} cases pass.")
    return 0


def main() -> int:
    """Parse arguments and dispatch to the generator or its selftest."""
    parser = argparse.ArgumentParser(description="Generate docs/DRIVER_STATUS.md from the tree.")
    parser.add_argument("--selftest", action="store_true", help="run the self-test and exit")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    return write(REPO_ROOT)


if __name__ == "__main__":
    raise SystemExit(main())
