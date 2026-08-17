# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The coverage census: what is enrolled, what measures it, why a row is unmeasured.

``check_tree_coverage.py`` is the ENFORCER -- it reads the measured traces and
judges them against the committed baseline. This module is the MODEL it judges
against: which translation units are enrolled, which host projects measure
them, what an unmeasured row is allowed to say, and how small any one root may
legitimately get before the enumeration itself is the defect.

Keeping the two apart is the same split ``lint_coverage_rules.py`` /
``check_lint_coverage.py`` and ``tier_layers.py`` / ``check_tier_imports.py``
already use here: the tables below are the part a human edits when a new
measurement project or a new source root lands, and that edit is reviewable
without reading the trace plumbing.

ONE CENSUS
----------
Every first-party ``.c`` / ``.cc`` / ``.cpp`` under ``libs/``, ``src/``,
``port/``, ``tools/``, ``apps/`` and ``examples/`` is enrolled -- firmware,
platform, host tool and product alike. There is one quality bar for the tree
and no tier gets a softer one, so there is no per-tier scope list to fall out
of date. The enumeration itself comes from ``lint_targets.first_party_paths``,
i.e. from ``git ls-files``, so a directory added tomorrow is enrolled the day
it lands.

Only three things are subtracted, and each is subtracted somewhere else first:

* vendored SOUP and generated tables -- ``lint_targets`` already drops
  ``libs/third_party/``, ``libs/ra8_fonts/``, ``tools/vela/generated/`` and
  ``port/threadx/``;
* the individually registered generated sources in
  ``lint_coverage_rules.PATH_CLASS`` -- a protobuf-c codec is its generator's
  output, not hand-authored code;
* test sources. A file under a ``tests/`` directory is the INSTRUMENT, not the
  thing measured, and the ``tests/`` root is already outside the census by the
  same reasoning. Applying it at any depth is what keeps the rule uniform
  instead of a per-product carve-out: ``apps/shared/media_dl/tests/`` and
  ``tools/ra8_emulator/tests/`` are test code exactly as ``tests/`` is.

Headers carry no row. Inline code in a header is measured through the TUs that
include it, and a header row would double-count it against whichever TU
happened to be compiled first.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_coverage_rules import PATH_CLASS
from lint_targets import first_party_paths

REPO_ROOT = Path(__file__).resolve().parents[2]

#: The five first-party source roots. ``tests/`` is deliberately absent: it is
#: the instrument. Trailing slashes so a root can never prefix-match a sibling.
CENSUS_ROOTS: tuple[str, ...] = (
    "libs/",
    "port/",
    "tools/",
    "apps/",
    "examples/",
)

#: Translation-unit suffixes. Headers are excluded by construction.
CENSUS_SUFFIXES: tuple[str, ...] = (".c", ".cc", ".cpp", ".cxx")

#: A path COMPONENT that marks test code wherever it appears.
TEST_DIR_COMPONENT = "tests"

#: The ``lint_coverage_rules`` class whose members are a generator's output.
GENERATED_CLASS = "generated-source"


def root_of(rel: str) -> str:
    """Return the census root a repo-relative path belongs to, without its slash."""
    return rel.split("/", 1)[0]


def is_test_source(rel: str) -> bool:
    """True when `rel` sits under a ``tests/`` directory at any depth."""
    return TEST_DIR_COMPONENT in rel.split("/")[:-1]


def in_census(rel: str) -> bool:
    """True when `rel` is an enrolled first-party translation unit.

    The caller is expected to have filtered SOUP, generated tables and build
    output already (``lint_targets`` does), so this adds only the three
    subtractions this module owns: root, generated registry, test source.
    """
    if not rel.startswith(CENSUS_ROOTS) or not rel.endswith(CENSUS_SUFFIXES):
        return False
    if PATH_CLASS.get(rel) == GENERATED_CLASS:
        return False
    return not is_test_source(rel)


def census_paths(paths: list[str] | None = None) -> list[str]:
    """Every enrolled translation unit, sorted.

    Args:
        paths: Repo-relative candidates to filter. Defaults to the tracked
            first-party tree; the parameter exists so a selftest can drive the
            rule with a fixture instead of the live checkout.

    Returns:
        The enrolled repo-relative paths, sorted.
    """
    if paths is None:
        paths = first_party_paths(CENSUS_SUFFIXES)
    return sorted(rel for rel in paths if in_census(rel))


# ---------------------------------------------------------------------------
# MEASUREMENT PROJECTS -- the host builds that produce execution data.
#
# Each one is configured with ``RA8_COVERAGE=ON``, built, run under ctest, and
# reported by ``scripts/report/tree_coverage.sh`` into one gcovr trace. The
# traces are then merged, so a translation unit compiled by more than one
# project (the media_dl core is built by BOTH the host suite and the media_dl
# form) carries the union of what every project executed rather than whichever
# number the last sweep happened to produce.
#
# ``subsumes`` names a source root whose own coverage-capable listfile is
# configured as a SUBDIRECTORY of this project rather than on its own. It is
# not decoration: ``unclaimed_coverage_projects`` below fails when a listfile
# declares ``option(RA8_COVERAGE ...)`` and no project claims it, which is what
# stops a new measurable project from being added and silently never measured.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class MeasurementProject:
    """One host build that produces coverage data for the census."""

    name: str
    """Trace file stem and build subdirectory name."""

    cmake_dir: str
    """Repo-relative directory handed to ``cmake -S``."""

    subsumes: tuple[str, ...]
    """Coverage-capable source roots this project configures as subdirectories."""

    min_files: int
    """Non-vacuity floor: census units this project's own report must carry. A
    project whose build silently stopped instrumenting reports a handful of
    files and the merged total still looks healthy, so the floor is per
    project rather than on the merge."""

    @property
    def claimed_dirs(self) -> tuple[str, ...]:
        """Every source root whose coverage option this project is responsible for."""
        return (self.cmake_dir, *self.subsumes)


PROJECTS: tuple[MeasurementProject, ...] = (
    # Measured 480 census units when the gate landed.
    MeasurementProject("host-tests", "tests", (), 400),
    # Measured 57 census units when the gate landed.
    MeasurementProject("media-dl", "apps/stand_alone/media_dl", ("apps/shared/media_dl",), 50),
)

#: The declaration a listfile makes when it can emit coverage data.
COVERAGE_OPTION_DECLARATION = "option(RA8_COVERAGE"


def coverage_capable_dirs(listfiles: dict[str, str]) -> list[str]:
    """Return the directories whose listfiles declare the coverage option.

    Args:
        listfiles: Repo-relative listfile path -> its text.

    Returns:
        The owning directories, sorted and de-duplicated.
    """
    found = {
        rel.rsplit("/", 1)[0]
        for rel, text in listfiles.items()
        if COVERAGE_OPTION_DECLARATION in text
    }
    return sorted(found)


def unclaimed_coverage_projects(dirs: list[str]) -> list[str]:
    """Return coverage-capable directories no measurement project claims.

    A directory is claimed when it IS a project's claimed root or sits under
    one. Anything left over can produce coverage data that nothing collects,
    which is how a whole product stays invisible while the gate reports a
    clean tree.
    """
    claimed = {d for project in PROJECTS for d in project.claimed_dirs}
    prefixes = tuple(f"{d}/" for d in sorted(claimed))
    return [d for d in dirs if d not in claimed and not d.startswith(prefixes)]


# ---------------------------------------------------------------------------
# WHY A UNIT IS UNMEASURED -- four classes, each derived from the tree.
#
# An unmeasured unit gets an EXPLICIT row rather than being absent, so nothing
# is silently missing, and the reason is a class the checker can re-derive
# instead of prose a human can write anything into. A row whose reason does not
# match what the tree says is a stale baseline, not a waiver.
# ---------------------------------------------------------------------------

REASON_FIRMWARE = "firmware-composition"
"""Only ever cross-compiled into an image: ``examples/`` and the firmware
products under ``apps/``. There is no host process to run and no exit status to
read, so no host coverage build can reach it."""

REASON_PLATFORM = "platform-cross-only"
"""Platform code (``libs/``, ``src/``, ``port/``) that no host coverage build
compiles at all -- board boot code, RTOS/USB stack ports, and drivers with no
host double. It is compiled only by the ARM toolchain."""

REASON_HOSTED = "hosted-no-coverage-build"
"""Host-side tool or product code whose CMake project is not wired into any
measurement project. This is the one class that is pure debt: the code IS host
executable, so the fix is to add the project to ``PROJECTS``, not to keep the
row."""

REASON_COMPILED = "compiled-not-executed"
"""A measurement project COMPILED the unit and no test ever executed it, so
gcov wrote a .gcno and never a .gcda. Usually a static-archive member no test
binary pulls in. Named separately because it is invisible to a report-driven
gate -- the unit simply does not appear -- which is how three of these sat
outside a floor advertised as having no allowlist."""

REASONS: tuple[str, ...] = (
    REASON_FIRMWARE,
    REASON_PLATFORM,
    REASON_HOSTED,
    REASON_COMPILED,
)

PLATFORM_ROOTS: tuple[str, ...] = ("libs/", "src/", "port/")
HOSTED_ROOTS: tuple[str, ...] = ("tools/", "apps/")
FIRMWARE_ROOTS: tuple[str, ...] = ("examples/",)


def is_firmware_composition(rel: str, firmware_dirs: tuple[str, ...]) -> bool:
    """True when `rel` is only ever linked into a cross-compiled image.

    ``examples/`` is firmware by root. Under ``apps/`` the root answers
    nothing -- media_dl is a host program and the e-reader is a TrustZone
    image -- so the discriminator is ``lint_targets.firmware_app_dirs()``: an
    app directory carrying BOTH a linker script and a vector table.
    """
    if rel.startswith(FIRMWARE_ROOTS):
        return True
    return any(rel.startswith(f"{d}/") for d in firmware_dirs)


def structural_reason(rel: str, *, compiled: bool, firmware_dirs: tuple[str, ...]) -> str:
    """Return the one reason class the tree says an unmeasured `rel` may carry.

    Args:
        rel: Repo-relative census path with no execution data.
        compiled: Whether a measurement project's build compiled it anyway.
        firmware_dirs: ``lint_targets.firmware_app_dirs()`` for this tree.

    Returns:
        One member of ``REASONS``.
    """
    if compiled:
        return REASON_COMPILED
    if is_firmware_composition(rel, firmware_dirs):
        return REASON_FIRMWARE
    if rel.startswith(PLATFORM_ROOTS):
        return REASON_PLATFORM
    return REASON_HOSTED


# ---------------------------------------------------------------------------
# NON-VACUITY FLOORS
#
# A checker that enumerates nothing reports a clean tree because it looked at
# nothing -- the dominant defect class in this repository. One floor per root,
# so a collapse confined to a SINGLE root still fails: a tree-wide total would
# stay comfortably above its floor while ``tools/`` silently dropped to zero.
#
# Each floor is set well under the population measured when the gate landed, so
# ordinary deletion never trips it and a broken enumeration always does.
# ---------------------------------------------------------------------------

ROOT_CENSUS_FLOORS: dict[str, int] = {
    "libs": 385,  # measured 440
    "examples": 300,  # measured 370
    "tools": 110,  # measured 134
    "apps": 50,  # measured 64
    "port": 28,  # measured 35
}

MEASURED_FLOOR = 440
"""Census units carrying execution data. Measured 507 when the gate landed; a
drop past this means the measurement, not the tests, came apart."""


def census_floor_failures(paths: list[str]) -> list[str]:
    """Return one message per root whose census fell below its floor."""
    counts = dict.fromkeys(ROOT_CENSUS_FLOORS, 0)
    for rel in paths:
        root = root_of(rel)
        if root in counts:
            counts[root] += 1
    return [
        f"census for root {root}/ collapsed to {counts[root]} unit(s), floor is {floor}"
        for root, floor in sorted(ROOT_CENSUS_FLOORS.items())
        if counts[root] < floor
    ]
