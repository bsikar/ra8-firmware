#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction selftest for ``check_soup_inventory.py``.

Every rule is proved twice: on a fixture where the catalogues agree (the
checker must stay quiet) and on one carrying exactly one seeded disagreement
(the checker must fire).  A detector is only worth its exit status when both
halves are shown, which is why this file exists rather than a "it passed on
the real tree" claim (#531, #631).

The fixtures are synthetic: a miniature registry, a miniature licence file and
a miniature index written into a scratch directory.  Running the rules against
the real tree would prove only that today's tree is clean, which is the
assertion under test rather than a test of it.
"""

from __future__ import annotations

import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import check_soup_inventory as checker

EXIT_OK = 0
EXIT_FAIL = 1


@dataclass(frozen=True)
class FakeComponent:
    """The two registry fields the inventory rules read."""

    key: str
    path: str


BASE_COMPONENTS = (
    FakeComponent("threadx", "libs/third_party/threadx"),
    FakeComponent("miniz", "apps/shared_libs/third_party/miniz"),
    FakeComponent("esp-hosted-mcu", "coprocessor/esp32c6/esp-hosted-mcu"),
    FakeComponent("doxygen-awesome", "docs/doxygen_theme"),
)

THEME_ROW = "| doxygen-awesome | 2.4.2 | MIT | `docs/doxygen_theme/` | <https://example.invalid> |\n"
MINIZ_ROW = "| miniz | 3.0.2 | MIT | `apps/shared_libs/third_party/miniz/` | <https://example.invalid> |"
GHOST_ROW = "| ghost | 1.0 | MIT | `libs/third_party/ghost/` | <https://example.invalid> |"
ESP_LINK = "| esp-hosted | [esp-hosted.md](esp-hosted.md) |"
BLE_LINK = "| BLE patch | [ble_patch_image.md](ble_patch_image.md) |"

BASE_LICENSES = f"""# Third-Party License Inventory

## Scope

Vendored SOUP under `libs/third_party/` and `apps/shared_libs/third_party/`.

## Inventory

| Component | Version | License | In-tree path | Upstream |
| --- | --- | --- | --- | --- |
| ThreadX | 6.5.0 | MIT | `libs/third_party/threadx/` | <https://example.invalid> |
{MINIZ_ROW}
{THEME_ROW}
## Co-processor firmware

- Built from `coprocessor/esp32c6/` and flashed onto the companion part.
"""

BASE_INDEX = f"""# SOUP Catalog

## Index

| Library | Doc |
| --- | --- |
| ThreadX | [threadx.md](threadx.md) |
| miniz | [miniz.md](miniz.md) |
{ESP_LINK}

Aggregated inventory: [../../THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md).
"""

BASE_DOCS = ("threadx.md", "miniz.md", "esp-hosted.md")

# The fixtures are deliberately small, so every case lowers the production
# floors rather than inflating fixtures to satisfy them.  The floor LOGIC keeps
# its own must-fire cases below.
FIXTURE_FLOORS = (2, 2, 2, 2)
FLOOR_NAMES = ("MIN_COMPONENTS", "MIN_INVENTORY_PATHS", "MIN_INDEX_LINKS", "MIN_SOUP_DOCS")


def _write_tree(root: Path, licenses: str, index: str, docs: tuple[str, ...]) -> None:
    """Materialise one fixture tree under ``root``."""
    (root / checker.LICENSES_REL).write_text(licenses, encoding="utf-8")
    soup = root / checker.SOUP_DIR_REL
    soup.mkdir(parents=True, exist_ok=True)
    (soup / "README.md").write_text(index, encoding="utf-8")
    for name in docs:
        (soup / name).write_text(f"# {name}\n", encoding="utf-8")


def _set_floors(values: tuple[int, ...]) -> tuple[int, ...]:
    """Install ``values`` as the module floors; return the previous ones."""
    previous = tuple(getattr(checker, name) for name in FLOOR_NAMES)
    for name, value in zip(FLOOR_NAMES, values, strict=True):
        setattr(checker, name, value)
    return previous


def _agreement_cases() -> tuple[tuple[str, tuple, str, str, tuple[str, ...], bool], ...]:
    """(label, components, licences, index, docs, must_fire) for R1..R4."""
    no_theme = BASE_LICENSES.replace(THEME_ROW, "")
    dir_only = no_theme.replace(
        "Vendored SOUP under", "Vendored assets also live under `docs/`; SOUP under"
    )
    orphan_row = BASE_LICENSES.replace(MINIZ_ROW, f"{MINIZ_ROW}\n{GHOST_ROW}")
    dangling = BASE_INDEX.replace(ESP_LINK, f"{ESP_LINK}\n{BLE_LINK}")
    orphan_docs = (*BASE_DOCS, "orphan.md")
    return (
        ("agreeing catalogues stay quiet", BASE_COMPONENTS, BASE_LICENSES, BASE_INDEX, BASE_DOCS, False),
        ("R1 component named by no row fires", BASE_COMPONENTS, no_theme, BASE_INDEX, BASE_DOCS, True),
        ("R1 one-segment ancestor is not a naming", BASE_COMPONENTS, dir_only, BASE_INDEX, BASE_DOCS, True),
        ("R2 orphan inventory row fires", BASE_COMPONENTS, orphan_row, BASE_INDEX, BASE_DOCS, True),
        ("R3 dangling index link fires", BASE_COMPONENTS, BASE_LICENSES, dangling, BASE_DOCS, True),
        ("R4 unindexed record fires", BASE_COMPONENTS, BASE_LICENSES, BASE_INDEX, orphan_docs, True),
    )


def _run_agreement_case(case: tuple, scratch: Path) -> tuple[bool, str]:
    """Run one agreement case with lowered floors; return (passed, detail)."""
    label, components, licenses, index, docs, must_fire = case
    root = scratch / label.replace(" ", "_")
    root.mkdir(parents=True, exist_ok=True)
    _write_tree(root, licenses, index, docs)
    previous = _set_floors(FIXTURE_FLOORS)
    try:
        vacuity, failures = checker.collect_failures(components, root)
    finally:
        _set_floors(previous)
    if vacuity:
        return False, f"{label}: fixture collapsed unexpectedly: {vacuity[0]}"
    if bool(failures) != must_fire:
        want = "fire" if must_fire else "stay quiet"
        return False, f"{label}: expected the checker to {want}, it did not ({failures})"
    return True, label


def _vacuity_cases() -> tuple[tuple[str, tuple, str, bool], ...]:
    """(label, components, licences, must_fire) for the non-vacuity floors."""
    renamed_section = BASE_LICENSES.replace("## Inventory", "## Inventory (moved)")
    return (
        ("floors quiet on a populated scan", BASE_COMPONENTS, BASE_LICENSES, False),
        ("collapsed registry fires", (), BASE_LICENSES, True),
        ("renamed inventory section fires", BASE_COMPONENTS, renamed_section, True),
    )


def _run_vacuity_case(case: tuple, scratch: Path) -> tuple[bool, str]:
    """Run one floor case against lowered fixture floors; return (passed, detail)."""
    label, components, licenses, must_fire = case
    root = scratch / label.replace(" ", "_")
    root.mkdir(parents=True, exist_ok=True)
    _write_tree(root, licenses, BASE_INDEX, BASE_DOCS)
    previous = _set_floors(FIXTURE_FLOORS)
    try:
        vacuity, _ = checker.collect_failures(components, root)
    finally:
        _set_floors(previous)
    if bool(vacuity) != must_fire:
        want = "report vacuity" if must_fire else "stay quiet"
        return False, f"{label}: expected the floors to {want}, they did not ({vacuity})"
    return True, label


def run_selftest() -> int:
    """Prove every rule and every floor, in both directions."""
    passed: list[str] = []
    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="soup-inventory-selftest-") as tmp:
        scratch = Path(tmp)
        for case in _agreement_cases():
            ok, detail = _run_agreement_case(case, scratch)
            (passed if ok else problems).append(detail)
        for case in _vacuity_cases():
            ok, detail = _run_vacuity_case(case, scratch)
            (passed if ok else problems).append(detail)
    for line in problems:
        print(f"  FAIL {line}", file=sys.stderr)
    total = len(passed) + len(problems)
    if problems:
        print(
            f"check_soup_inventory --selftest: {len(problems)} of {total} cases failed",
            file=sys.stderr,
        )
        return EXIT_FAIL
    print(f"check_soup_inventory --selftest: {total}/{total} cases pass (both directions).")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(run_selftest())
