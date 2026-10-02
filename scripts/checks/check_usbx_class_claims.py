#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Re-derive the USBX class-driver claim in docs/SOUP/usbx.md from the build.

docs/SOUP/usbx.md is a certification artefact. Its "Class drivers used"
bullet is a claim about what cmake/usbx.cmake actually compiles, and it
drifted: it advertised CDC-ACM, HID and MSC as "device + host" when the
recipe globs nothing at all out of the vendored host-class tree, and it
omitted the DFU device class that five HIL apps depend on.

This gate derives the compiled set from the globs themselves and compares
it against a machine-readable marker in the SOUP record:

    <!-- usbx-class-claims: device=cdc_acm,dfu,hid,storage host=none -->

A glob that matches no vendored source on disk is a finding too, so a
class cannot be claimed by a pattern that quietly stopped resolving.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

from zig_package import package_dir

REPO_ROOT = Path(__file__).resolve().parents[2]

CMAKE_REL = "cmake/usbx.cmake"
SOUP_REL = "docs/SOUP/usbx.md"

# file(GLOB _VAR CONFIGURE_DEPENDS "${_RA8_USBX_DEV_CLS_SRC}/ux_device_class_x_*.c")
DEVICE_GLOB_RE = re.compile(
    r"file\(GLOB\s+(?P<var>[A-Za-z0-9_]+)[^)]*?"
    r"\$\{_RA8_USBX_DEV_CLS_SRC\}/(?P<pattern>ux_device_class_[a-z0-9_]+_\*\.c)",
    re.DOTALL,
)
HOST_GLOB_RE = re.compile(r"usbx_host_classes/src/([A-Za-z0-9_*.]+)")
# list(FILTER _VAR EXCLUDE REGEX ".*/ux_device_class_pima_storage_.*\.c$")
EXCLUDE_RE = re.compile(
    r"list\(\s*FILTER\s+(?P<var>[A-Za-z0-9_]+)\s+EXCLUDE\s+REGEX\s+"
    r'"(?P<regex>[^"]+)"',
    re.DOTALL,
)
CLASS_STEM_RE = re.compile(r"ux_device_class_([a-z0-9_]+)_\*\.c")

MARKER_RE = re.compile(
    r"<!--\s*usbx-class-claims:\s*device=(?P<device>[a-z0-9_,]+|none)"
    r"\s+host=(?P<host>[a-z0-9_,]+|none)\s*-->"
)

# Relative to the usbx package root (pinned in build.zig.zon, not in this repo).
DEV_CLS_SRC_REL = "common/usbx_device_classes/src"
HOST_CLS_SRC_REL = "common/usbx_host_classes/src"

# A collapsed read of the recipe must fail loudly rather than agree with an
# empty marker: the tree has four device classes today.
CLASS_FLOOR = 2


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:  # pragma: no cover - surfaced as a finding
        message = f"{path}: unreadable ({exc})"
        raise SystemExit(message) from exc


def compiled_device_classes(root: Path, usbx: Path) -> tuple[set[str], list[str]]:
    """Class stems the recipe really compiles out of the device-class tree.

    Resolves each ``file(GLOB)`` against the vendored sources on disk and
    then applies that variable's own ``list(FILTER ... EXCLUDE REGEX)``
    lines, the way CMake does. A class counts as compiled when at least
    one of its translation units survives, so the single-TU INQUIRY
    override does not read as dropping the whole MSC class, while a
    filter that removes every file does.

    Returns the surviving stems plus the globs left with no source.
    """
    text = _read(root / CMAKE_REL)
    excludes: dict[str, list[str]] = {}
    for match in EXCLUDE_RE.finditer(text):
        excludes.setdefault(match.group("var"), []).append(match.group("regex"))

    src = usbx / DEV_CLS_SRC_REL
    stems: set[str] = set()
    vacuous: list[str] = []
    for match in DEVICE_GLOB_RE.finditer(text):
        pattern = match.group("pattern")
        stem_match = CLASS_STEM_RE.match(pattern)
        if stem_match is None:  # pragma: no cover - pattern is anchored
            continue
        stem = stem_match.group(1)
        files = [f.as_posix() for f in sorted(src.glob(pattern))]
        if not files:
            vacuous.append(pattern)
            continue
        dropped = excludes.get(match.group("var"), [])
        kept = [f for f in files if not any(re.search(rx, f) for rx in dropped)]
        if kept:
            stems.add(stem)
        else:
            vacuous.append(f"{pattern} (every match filtered back out)")
    return stems, vacuous


def compiled_host_classes(root: Path) -> set[str]:
    """Class stems the recipe globs out of the vendored host-class tree."""
    text = _read(root / CMAKE_REL)
    stems: set[str] = set()
    for hit in HOST_GLOB_RE.findall(text):
        stem = hit.removeprefix("ux_host_class_").split("*")[0].strip("_.")
        if stem:
            stems.add(stem)
    return stems


def vendored_host_class_files(usbx: Path) -> int:
    """How many host-class sources the package ships, compiled or not."""
    src = usbx / HOST_CLS_SRC_REL
    return len(sorted(src.glob("*.c"))) if src.is_dir() else 0


def stated_claim(root: Path) -> tuple[set[str], set[str]] | None:
    """The marker's device/host sets, or None when the marker is absent."""
    match = MARKER_RE.search(_read(root / SOUP_REL))
    if match is None:
        return None

    def split(raw: str) -> set[str]:
        return set() if raw == "none" else {p for p in raw.split(",") if p}

    return split(match.group("device")), split(match.group("host"))


def prose_without_markers(root: Path) -> str:
    """The SOUP prose with the machine markers removed, so only claims remain."""
    return MARKER_RE.sub("", _read(root / SOUP_REL))


def scan(root: Path, usbx: Path) -> list[str]:
    """Compare what the recipe compiles against what the SOUP entry claims."""
    device, vacuous = compiled_device_classes(root, usbx)
    host = compiled_host_classes(root)

    findings: list[str] = [
        f"{CMAKE_REL}: glob {glob} matches no source under the usbx package's {DEV_CLS_SRC_REL}" for glob in vacuous
    ]
    if len(device) < CLASS_FLOOR and not vacuous:
        findings.append(
            f"{CMAKE_REL}: only {len(device)} device class(es) parsed out of the "
            f"recipe; the parser is reading it wrong (floor {CLASS_FLOOR})"
        )

    claim = stated_claim(root)
    if claim is None:
        findings.append(
            f"{SOUP_REL}: no '<!-- usbx-class-claims: device=... host=... -->' "
            "marker, so the class-driver bullet is unchecked"
        )
        return findings

    said_device, said_host = claim
    if said_device != device:
        findings.append(
            f"{SOUP_REL}: marker says device={_fmt(said_device)} but the recipe "
            f"compiles {_fmt(device)}"
        )
    if said_host != host:
        findings.append(
            f"{SOUP_REL}: marker says host={_fmt(said_host)} but the recipe compiles {_fmt(host)}"
        )

    prose = prose_without_markers(root)
    if not host and re.search(r"device\s*\+\s*host", prose):
        count = vendored_host_class_files(usbx)
        findings.append(
            f"{SOUP_REL}: the prose still claims a class is used 'device + host' "
            f"while the recipe compiles zero of the {count} vendored host-class "
            "sources"
        )
    flattened_prose = prose.lower().replace("-", "").replace("_", "")
    findings.extend(
        f"{SOUP_REL}: device class {stem} is compiled but named nowhere in the prose"
        for stem in sorted(device)
        if stem.replace("_", "") not in flattened_prose
    )
    return findings


def _fmt(stems: set[str]) -> str:
    return ",".join(sorted(stems)) if stems else "none"


def _seed(root: Path, *, marker: str, prose: str, classes: tuple[str, ...]) -> None:
    src = _fixture_usbx(root) / DEV_CLS_SRC_REL
    src.mkdir(parents=True, exist_ok=True)
    for stem in classes:
        (src / f"ux_device_class_{stem}_entry.c").write_text("/* fixture */\n")
    (_fixture_usbx(root) / HOST_CLS_SRC_REL).mkdir(parents=True, exist_ok=True)
    (_fixture_usbx(root) / HOST_CLS_SRC_REL / "ux_host_class_hub_entry.c").write_text("/* f */\n")
    (root / "cmake").mkdir(parents=True, exist_ok=True)
    globs = "\n".join(
        f"file(GLOB _RA8_USBX_{stem.upper()}_SOURCES CONFIGURE_DEPENDS "
        f'"${{_RA8_USBX_DEV_CLS_SRC}}/ux_device_class_{stem}_*.c")'
        for stem in classes
    )
    (root / CMAKE_REL).write_text(globs + "\n")
    (root / "docs/SOUP").mkdir(parents=True, exist_ok=True)
    (root / SOUP_REL).write_text(f"{marker}\n\n{prose}\n")


def _fixture_usbx(root: Path) -> Path:
    """Where a fixture plants its stand-in for the usbx package."""
    return root / "usbx"


GOOD_MARKER = "<!-- usbx-class-claims: device=cdc_acm,dfu host=none -->"
GOOD_PROSE = "Class drivers used: CDC-ACM (device), DFU (device)."
FIXTURE_CLASSES = ("cdc_acm", "dfu")


def _run_case(expect_clean: bool, **seed: object) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _seed(root, **seed)  # type: ignore[arg-type]
        found = scan(root, _fixture_usbx(root))
        ok = (not found) if expect_clean else bool(found)
        return ok, "; ".join(found)


def _marker_and_prose_cases() -> list[tuple[str, bool, str]]:
    """Cases where the recipe is fixed and the record is what varies."""
    specs = [
        ("a marker matching the recipe is quiet", True, GOOD_MARKER, GOOD_PROSE),
        (
            "a class the recipe compiles but the marker omits fires",
            False,
            "<!-- usbx-class-claims: device=cdc_acm host=none -->",
            GOOD_PROSE,
        ),
        (
            "a class the marker claims but the recipe drops fires",
            False,
            "<!-- usbx-class-claims: device=cdc_acm,dfu,hid host=none -->",
            GOOD_PROSE + " HID (device).",
        ),
        (
            "a 'device + host' prose claim with no host class compiled fires",
            False,
            GOOD_MARKER,
            "Class drivers used: CDC-ACM (device + host), DFU (device).",
        ),
        (
            "a compiled class named nowhere in the prose fires",
            False,
            GOOD_MARKER,
            "Class drivers used: CDC-ACM (device).",
        ),
        ("a missing marker fires", False, "(no marker here)", GOOD_PROSE),
    ]
    cases: list[tuple[str, bool, str]] = []
    for name, expect_clean, marker, prose in specs:
        ok, detail = _run_case(expect_clean, marker=marker, prose=prose, classes=FIXTURE_CLASSES)
        cases.append((name, ok, detail))
    return cases


def _host_glob_case() -> tuple[str, bool, str]:
    """A recipe that really globs the host tree must contradict host=none."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _seed(root, marker=GOOD_MARKER, prose=GOOD_PROSE, classes=FIXTURE_CLASSES)
        with (root / CMAKE_REL).open("a", encoding="utf-8") as handle:
            handle.write(
                "file(GLOB _H CONFIGURE_DEPENDS "
                '"${_RA8_USBX_VENDOR_DIR}/common/usbx_host_classes/src/'
                'ux_host_class_hub_*.c")\n'
            )
        found = scan(root, _fixture_usbx(root))
        return (
            "a host-class glob the marker calls none fires",
            any("host=" in f for f in found),
            "; ".join(found),
        )


def _vacuous_glob_case() -> tuple[str, bool, str]:
    """A glob whose sources vanished must not pass as a satisfied claim."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _seed(root, marker=GOOD_MARKER, prose=GOOD_PROSE, classes=FIXTURE_CLASSES)
        for stale in (_fixture_usbx(root) / DEV_CLS_SRC_REL).glob("ux_device_class_dfu_*.c"):
            stale.unlink()
        found = scan(root, _fixture_usbx(root))
        return (
            "a glob matching no vendored source fires",
            any("matches no source" in f for f in found),
            "; ".join(found),
        )


def _filtered_empty_case() -> tuple[str, bool, str]:
    """A filter that removes every match drops the class, unlike one TU."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _seed(root, marker=GOOD_MARKER, prose=GOOD_PROSE, classes=FIXTURE_CLASSES)
        with (root / CMAKE_REL).open("a", encoding="utf-8") as handle:
            handle.write(
                "list(FILTER _RA8_USBX_DFU_SOURCES EXCLUDE REGEX "
                '".*/ux_device_class_dfu_.*\\.c$")\n'
            )
        found = scan(root, _fixture_usbx(root))
        return (
            "a filter removing every match of a class fires",
            any("filtered back out" in f for f in found),
            "; ".join(found),
        )


def _single_tu_filter_case() -> tuple[str, bool, str]:
    """The real INQUIRY shape: one TU filtered, the class still compiled."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _seed(root, marker=GOOD_MARKER, prose=GOOD_PROSE, classes=FIXTURE_CLASSES)
        src = _fixture_usbx(root) / DEV_CLS_SRC_REL
        (src / "ux_device_class_dfu_inquiry.c").write_text("/* fixture */\n")
        with (root / CMAKE_REL).open("a", encoding="utf-8") as handle:
            handle.write(
                "list(FILTER _RA8_USBX_DFU_SOURCES EXCLUDE REGEX "
                '".*/ux_device_class_dfu_inquiry\\.c$")\n'
            )
        found = scan(root, _fixture_usbx(root))
        return (
            "one filtered TU leaves the class compiled and the gate quiet",
            not found,
            "; ".join(found),
        )


def selftest() -> int:
    """Run every planted case and report which assertions failed."""
    cases = _marker_and_prose_cases()
    cases.append(_host_glob_case())
    cases.append(_vacuous_glob_case())
    cases.append(_filtered_empty_case())
    cases.append(_single_tu_filter_case())

    failed = 0
    for name, ok, detail in cases:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}")
        if not ok:
            failed += 1
            if detail:
                print(f"         {detail}")
    if failed:
        print(f"SELFTEST FAILED: {failed} assertion(s)", file=sys.stderr)
        return 1
    print(f"selftest: {len(cases)} assertion(s) passed")
    return 0


def main() -> int:
    """Run the gate, or its selftest, and print whatever it found."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    usbx = package_dir("usbx", args.root)
    findings = scan(args.root, usbx)
    if findings:
        print(
            f"{Path(__file__).name}: {len(findings)} USBX class-claim finding(s):",
            file=sys.stderr,
        )
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    device, _ = compiled_device_classes(args.root, usbx)
    print(
        f"{Path(__file__).name}: class-driver claim current "
        f"({len(device)} device class(es) compiled, host side not compiled)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
