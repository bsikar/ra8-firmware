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
  instead of a per-product carve-out: ``apps/shared_libs/mdl/tests/`` and
  ``tools/ra8_emulator/tests/`` are test code exactly as ``tests/`` is.

Headers carry no row. Inline code in a header is measured through the TUs that
include it, and a header row would double-count it against whichever TU
happened to be compiled first.
"""

from __future__ import annotations

import re
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
# project (the mdl core is built by BOTH the host suite and the mdl host
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
    MeasurementProject("mdl", "apps/host/mdl", ("apps/shared_libs/mdl",), 50),
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

# Exact production adapters whose host build cannot coexist with the default
# implementation in one coverage image. Reflow v2 implements the same public
# symbols as v1 and is selected only by the firmware composition option; moving
# it from libs/ into apps/shared_libs must not change that platform constraint.
PLATFORM_CROSS_ONLY_UNITS: frozenset[str] = frozenset(
    {"apps/shared_libs/reflow/v2/src/reflow_v2.cpp"}
)


def is_firmware_composition(rel: str, firmware_dirs: tuple[str, ...]) -> bool:
    """True when `rel` is only ever linked into a cross-compiled image.

    ``examples/`` is firmware by root. Under ``apps/`` the root answers
    nothing -- mdl is a host program and the e-reader is a TrustZone
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
    if rel in PLATFORM_CROSS_ONLY_UNITS:
        return REASON_PLATFORM
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
    # The apps/shared_libs migration moved 82 production TUs out of libs/ and
    # into apps/ without changing the tree-wide census. Rebalance both root
    # floors together so the move cannot turn either root's guard vacuous.
    "libs": 315,  # measured 362
    "examples": 300,  # measured 370
    "tools": 110,  # measured 134
    "apps": 120,  # measured 150
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


# ---------------------------------------------------------------------------
# The requirement this gate is the executable form of
#
# REQ-SAFE-017 in ``docs/qualification/SRS.md`` carries NUMBERS, and a number
# in a requirements document drifts from the gate the moment one of the two is
# edited alone: the row claimed a universal 90/90 floor while the checker
# enforced a shrink-only ratchet with a 90% line / 80% branch entry floor and
# explicit UNMEASURED rows (#844). The tie below turns that drift into a gate
# failure instead of a discovery. The requirement must STATE the floors the
# checker enforces, name the baseline that carries the per-unit rows, and keep
# the UNMEASURED disposition visible; change a floor on either side and the
# other side fails until it says so too.
# ---------------------------------------------------------------------------

#: The requirement whose numbers ``check_tree_coverage.py`` enforces.
REQUIREMENT_ID = "REQ-SAFE-017"


def srs_text() -> str:
    """Read the requirements document REQ-SAFE-017 lives in."""
    return (REPO_ROOT / "docs" / "qualification" / "SRS.md").read_text(encoding="utf-8")


def requirement_row(text: str) -> str:
    """Return the REQ-SAFE-017 table row of an SRS document, or ``""``.

    Args:
        text: A whole SRS document.

    Returns:
        The single stripped table row, or the empty string when the document
        states the requirement nowhere -- itself a finding, because floors no
        requirement states are floors nobody agreed to.
    """
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(f"| {REQUIREMENT_ID}"):
            return stripped
    return ""


def _stated_floors(row: str) -> tuple[int, int] | None:
    """The ``>= N% line / M% branch`` entry floor the requirement states."""
    match = re.search(r">=\s*(\d+)%\s+line\s*/\s*(\d+)%\s+branch", row)
    return (int(match.group(1)), int(match.group(2))) if match else None


def requirement_claim_failures(text: str, line_floor: int, branch_floor: int) -> list[str]:
    """Name every way REQ-SAFE-017 and the enforced contract disagree.

    Args:
        text: The SRS document, from ``srs_text()`` or a selftest fixture.
        line_floor: The line floor the checker actually enforces.
        branch_floor: The branch floor the checker actually enforces.

    Returns:
        One message per disagreement; empty when the stated requirement and
        the executable gate are one policy.
    """
    row = requirement_row(text)
    if not row:
        return [
            f"{REQUIREMENT_ID} has no row in docs/qualification/SRS.md: "
            "the coverage floors state no requirement"
        ]
    out = [
        f"{REQUIREMENT_ID} must name {token} so the claim points at the authority that holds it"
        for token in (
            "`.github/tree-coverage-baseline.txt`",
            "`scripts/checks/check_tree_coverage.py`",
            "UNMEASURED",
        )
        if token not in row
    ]
    stated = _stated_floors(row)
    if stated is None:
        out.append(
            f"{REQUIREMENT_ID} states no '>= N% line / M% branch' entry floor; "
            f"the gate enforces {line_floor}% line / {branch_floor}% branch"
        )
    elif stated != (line_floor, branch_floor):
        out.append(
            f"{REQUIREMENT_ID} states {stated[0]}% line / {stated[1]}% branch; "
            f"the gate enforces {line_floor}% line / {branch_floor}% branch"
        )
    if re.search(r"\b\d{1,3}/\d{1,3}\b", row):
        out.append(
            f"{REQUIREMENT_ID} carries a bare N/M coverage ratio: state each floor with its "
            "metric and its unit, which is the ambiguity that let the claim drift"
        )
    return out


# ---------------------------------------------------------------------------
# Every document that states these floors, not only the one requirement
# ---------------------------------------------------------------------------
#
# ``requirement_claim_failures`` above ties ONE row in ONE document to the
# enforced floors. It is not the only place the numbers are written down:
# ``docs/COVERAGE.md`` states the entry floor twice and
# ``docs/qualification/SVP.md`` 5.1 states it once, and neither was read by
# anything. Lower ``LINE_FLOOR_PCT`` and both keep claiming the old number
# with the gate green, which is the same defect #844 reported about the SRS
# row, one document over.
#
# So the tie is a SITE LIST, not a single row, and each declared site must
# state the floors in the one unambiguous form: every number with its metric
# and its unit. A bare ``N/M`` beside the word floor is a finding wherever it
# appears in these documents -- that shorthand is exactly what let "90/90"
# mean a met universal floor in one reader's head and a 90% line / 80% branch
# entry floor in another's.
#
# LIMIT, stated rather than implied: the scope is the declared list. A NEW
# document that states a floor pair is not tied until its path is added here,
# and nothing in this module sweeps the tree for one. A repo-wide sweep of
# tracked markdown is the next slice; it needs a tracked-markdown enumerator
# this module does not have and must not grow a second copy of.
# ---------------------------------------------------------------------------

#: Documents that state the coverage floors in prose. Each one is read on
#: every gate run and must agree with the floors the checker enforces.
FLOOR_CLAIM_DOCS: tuple[str, ...] = (
    "docs/COVERAGE.md",
    "docs/qualification/SRS.md",
    "docs/qualification/SVP.md",
)

#: The one form a floor claim may take: each number with its metric and unit.
FLOOR_CLAIM_RE = re.compile(r"(\d{1,3})%\s*line\s*/\s*(\d{1,3})%\s*branch")

#: A line that is talking about the entry floor at all.
FLOOR_SENTENCE_RE = re.compile(r"\bfloor\b|\benters? at\b")

#: The ambiguous shorthand: a bare pair with neither metric nor unit.
BARE_PAIR_RE = re.compile(r"(?<![\w.])(\d{1,3})/(\d{1,3})(?![\w.%])")


def policy_doc_texts() -> dict[str, str | None]:
    """Read every declared floor-claim document.

    Returns:
        One entry per :data:`FLOOR_CLAIM_DOCS` path, mapped to its text, or to
        ``None`` when the file cannot be read -- a missing claim site is a
        finding, not an absence of one.
    """
    out: dict[str, str | None] = {}
    for rel in FLOOR_CLAIM_DOCS:
        try:
            out[rel] = (REPO_ROOT / rel).read_text(encoding="utf-8")
        except OSError:
            out[rel] = None
    return out


def _stated_pair_failures(rel: str, text: str, floors: tuple[int, int]) -> list[str]:
    """Every floor pair a document states that is not the enforced pair.

    Matched over the whole document, not line by line: these are wrapped prose
    files, and ``docs/qualification/SVP.md`` 5.1 states its claim across a line
    break. A tie that only reads single lines is a tie a re-wrap switches off.
    """
    out: list[str] = []
    for match in FLOOR_CLAIM_RE.finditer(text):
        stated = (int(match.group(1)), int(match.group(2)))
        if stated == floors:
            continue
        number = text.count("\n", 0, match.start()) + 1
        out.append(
            f"{rel}:{number} states {stated[0]}% line / {stated[1]}% branch; "
            f"the gate enforces {floors[0]}% line / {floors[1]}% branch"
        )
    return out


def _bare_pair_failures(rel: str, text: str, floors: tuple[int, int]) -> list[str]:
    """Every entry-floor sentence that states a bare ``N/M`` instead of the form.

    Line by line on purpose: the anchor is the word ``floor`` (or ``enters
    at``) in the same breath as the pair, which is what separates a floor claim
    from the pass-count ratios (``689/689``, ``118/118``) these same documents
    carry about something else entirely.
    """
    return [
        f"{rel}:{number} states an entry floor as a bare N/M ratio: write each number "
        f"with its metric and unit ({floors[0]}% line / {floors[1]}% branch), because "
        "that shorthand is what let this claim drift"
        for number, raw in enumerate(text.splitlines(), start=1)
        if FLOOR_SENTENCE_RE.search(raw) and BARE_PAIR_RE.search(raw)
    ]


def _doc_claim_failures(rel: str, text: str, floors: tuple[int, int]) -> list[str]:
    """Name every disagreement between one claim site and the enforced floors."""
    out = _stated_pair_failures(rel, text, floors) + _bare_pair_failures(rel, text, floors)
    if not FLOOR_CLAIM_RE.search(text):
        out.append(
            f"{rel} is declared to state the coverage floors and states none in the "
            f"'{floors[0]}% line / {floors[1]}% branch' form: a claim site that stops "
            "claiming stops being tied"
        )
    return out


def floor_claim_failures(
    docs: dict[str, str | None], line_floor: int, branch_floor: int
) -> list[str]:
    """Name every way a declared claim site and the enforced floors disagree.

    Args:
        docs: Claim-site text, from :func:`policy_doc_texts` or a fixture.
        line_floor: The line floor the checker actually enforces.
        branch_floor: The branch floor the checker actually enforces.

    Returns:
        One message per disagreement; empty when every declared document
        states the floors this gate enforces, in the unambiguous form.
    """
    floors = (line_floor, branch_floor)
    out: list[str] = []
    for rel in FLOOR_CLAIM_DOCS:
        text = docs.get(rel)
        if text is None:
            out.append(
                f"{rel} is declared to state the coverage floors and could not be read: "
                "the floors this gate enforces are claimed nowhere it can check"
            )
            continue
        out += _doc_claim_failures(rel, text, floors)
    return out


def _claim_fixture(line_floor: int, branch_floor: int) -> dict[str, str | None]:
    """A minimal claim-site set that states the floors correctly."""
    claim = f"prose\nA new unit enters at the {line_floor}% line / {branch_floor}% branch floor.\n"
    return dict.fromkeys(FLOOR_CLAIM_DOCS, claim)


def floor_claim_selftest_failures(line_floor: int, branch_floor: int) -> list[str]:
    """Prove the site tie holds on the committed documents and fires on drift.

    The quiet case is every LIVE claim site, so a floor edited in the checker
    without the documents (or a document edited without the checker) fails the
    selftest that runs in every CI leg, not only the coverage leg, which needs
    a measurement to reach a verdict at all.

    Args:
        line_floor: The enforced line floor.
        branch_floor: The enforced branch floor.

    Returns:
        One message per assertion that did not hold; empty when all held.
    """
    live = policy_doc_texts()
    good = _claim_fixture(line_floor, branch_floor)
    out: list[str] = []
    if floor_claim_failures(live, line_floor, branch_floor):
        out.append("every committed claim site must state the floors this gate enforces")
    if not floor_claim_failures(live, line_floor + 1, branch_floor):
        out.append("a line floor no claim site states must fire")
    if not floor_claim_failures(live, line_floor, branch_floor - 1):
        out.append("a branch floor no claim site states must fire")
    if floor_claim_failures(good, line_floor, branch_floor):
        out.append("a claim site stating the enforced floors must stay quiet")
    for rel in FLOOR_CLAIM_DOCS:
        silent = dict(good)
        silent[rel] = "prose with no floor claim at all\n"
        if not floor_claim_failures(silent, line_floor, branch_floor):
            out.append(f"{rel} dropping its floor claim must fire")
        missing = dict(good)
        missing[rel] = None
        if not floor_claim_failures(missing, line_floor, branch_floor):
            out.append(f"{rel} going missing must fire")
    bare = dict(good)
    bare[FLOOR_CLAIM_DOCS[0]] = (
        f"A new unit enters at {line_floor}/{branch_floor}, the "
        f"{line_floor}% line / {branch_floor}% branch floor.\n"
    )
    if not floor_claim_failures(bare, line_floor, branch_floor):
        out.append("a bare N/M entry floor must fire even beside the explicit form")
    counted = dict(good)
    counted[FLOOR_CLAIM_DOCS[0]] = "The unit gate passed 689/689 in 8.66 s.\n" + str(
        good[FLOOR_CLAIM_DOCS[0]]
    )
    if floor_claim_failures(counted, line_floor, branch_floor):
        out.append("a pass-count ratio on a line about no floor must stay quiet")
    return out
