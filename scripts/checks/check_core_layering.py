#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce non-negotiable first-party library architecture boundaries.

The checker protects four independent invariants:

* ``ra8_core`` is the foundation and cannot include another library;
* a library cannot include another library's ``*_internal.h`` contract;
* Domain code cannot include HAL or board-device contracts;
* reusable libraries cannot include hosted-OS headers. POSIX, Windows, and
  similar dependencies belong under ``port/`` and bind through a portable
  interface instead of leaking into ``libs/``.

The second rule is what keeps implementation seams real. A wrapper backend in
another module must implement a public, platform-neutral backend contract; it
cannot compile only because a broad include path exposes a sibling's ``src/``.
The fourth rule intentionally allows ISO C headers such as ``string.h`` and
``stdlib.h``. It rejects only hosted I/O, filesystem, threading, socket, and
process headers whose presence makes a reusable library OS-specific.

Run::

    check_core_layering.py                  # scan every first-party library
    check_core_layering.py path/to/file.c   # scan listed library files
    check_core_layering.py --selftest       # prove every rule fires/stays quiet

Exit 0 on a clean scan, 1 on a boundary violation, and 2 when a full sweep or
the header-ownership oracle collapses below its non-vacuity floor.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path
from selftest_assert import expect, report

REPO_ROOT = Path(__file__).resolve().parents[2]
LIBS_ROOT = REPO_ROOT / "libs"

FOUNDATION_LIB = "ra8_core"
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")
INCLUDE_RE = re.compile(r'#\s*include\s*([<"])([^">]+)[">]')
EXCLUDE_FRAGMENTS = ("/third_party/", "/ra8_fonts/")
INTERNAL_HEADER_SUFFIX = "_internal.h"
DOMAIN_TAG = "[Ring 4 / Domain]"

# Hosted implementation details that never belong in a reusable library.
# ISO C headers remain legal; the port adapters themselves live outside libs/.
HOSTED_HEADERS = frozenset(
    {
        "arpa/inet.h",
        "dirent.h",
        "dlfcn.h",
        "fcntl.h",
        "glob.h",
        "mach/mach.h",
        "netdb.h",
        "poll.h",
        "pthread.h",
        "pwd.h",
        "semaphore.h",
        "spawn.h",
        "stdio.h",
        "sys/mman.h",
        "sys/socket.h",
        "sys/stat.h",
        "sys/statvfs.h",
        "sys/syscall.h",
        "sys/types.h",
        "sys/wait.h",
        "syslog.h",
        "unistd.h",
        "windows.h",
        "winsock2.h",
    }
)
HOSTED_HEADER_PREFIXES = ("arpa/", "mach/", "netinet/", "sys/", "windows/")


def _is_device_module(module: str) -> bool:
    """Whether ``module`` owns a hardware- or board-specific contract."""
    return module == "ra8_hal" or module.startswith("ra8_board_")


# A library header is ``libs/<module>/<inc|src>/...``.
_MIN_LIB_PATH_PARTS = 2

# Measured 2026-08-14: more than 850 first-party C/C++ files under libs/ after
# excluding SOUP/generated trees. A floor makes an empty or narrowed scan fatal.
FILE_FLOOR = 700

# Measured 2026-08-14: more than 425 distinct first-party library header names.
# Every ownership decision depends on this oracle, so it has its own floor.
OWNER_FLOOR = 340

# Program name plus the one accepted selftest option.
SELFTEST_ARG_COUNT = 2


def _excluded(path: Path) -> bool:
    """Whether ``path`` is build output, SOUP, or generated font data."""
    text = str(path).replace("\\", "/")
    return is_build_output_path(text) or any(fragment in text for fragment in EXCLUDE_FRAGMENTS)


def _header_owners() -> dict[str, set[str]]:
    """Map each first-party library header basename to its owning module(s)."""
    owners: dict[str, set[str]] = {}
    for header in LIBS_ROOT.rglob("*.h"):
        if _excluded(header):
            continue
        rel = header.relative_to(LIBS_ROOT).parts
        if len(rel) < _MIN_LIB_PATH_PARTS or rel[1] not in ("inc", "src"):
            continue
        owners.setdefault(header.name, set()).add(rel[0])
    return owners


def _is_source(path: Path) -> bool:
    """Whether ``path`` has a first-party C/C++ source/header suffix."""
    return path.suffix in SOURCE_SUFFIXES


def _rel(path: Path) -> str:
    """Return a stable repository-relative display path where possible."""
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _module_for(path: Path) -> str | None:
    """Return the immediate module beneath ``libs/``, or None outside it."""
    if not path.is_relative_to(LIBS_ROOT):
        return None
    rel = path.relative_to(LIBS_ROOT).parts
    return rel[0] if rel else None


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    """Expand explicit paths, or enumerate every first-party library file."""
    args = list(arg_paths)
    if args:
        candidates: list[Path] = []
        for raw in args:
            path = Path(raw)
            if not path.is_absolute():
                path = REPO_ROOT / path
            if path.is_dir():
                for suffix in SOURCE_SUFFIXES:
                    candidates.extend(path.rglob("*" + suffix))
            elif _is_source(path):
                candidates.append(path)
    else:
        candidates = []
        for suffix in SOURCE_SUFFIXES:
            candidates.extend(LIBS_ROOT.rglob("*" + suffix))
    return sorted(
        path for path in candidates if path.is_relative_to(LIBS_ROOT) and not _excluded(path)
    )


def _is_hosted_header(name: str) -> bool:
    """Whether an angle-bracket include names a hosted-OS implementation API."""
    folded = name.lower()
    return folded in HOSTED_HEADERS or folded.startswith(HOSTED_HEADER_PREFIXES)


def _scan_text(
    text: str,
    rel_path: str,
    module: str,
    owners: dict[str, set[str]],
) -> list[tuple[str, str, int, str, tuple[str, ...]]]:
    """Return architecture findings from one module-owned translation unit."""
    findings: list[tuple[str, str, int, str, tuple[str, ...]]] = []
    is_domain = DOMAIN_TAG in text
    for lineno, line in enumerate(text.splitlines(), 1):
        match = INCLUDE_RE.search(line)
        if match is None:
            continue
        opener, included = match.groups()
        name = Path(included).name
        modules = owners.get(name, set())
        other_owners = modules - {module}

        if module == FOUNDATION_LIB and other_owners and FOUNDATION_LIB not in modules:
            findings.append(
                ("CORE_UPWARD", rel_path, lineno, included, tuple(sorted(other_owners)))
            )

        if name.endswith(INTERNAL_HEADER_SUFFIX) and other_owners and module not in modules:
            findings.append(
                ("CROSS_INTERNAL", rel_path, lineno, included, tuple(sorted(other_owners)))
            )

        device_owners = tuple(sorted(owner for owner in other_owners if _is_device_module(owner)))
        if is_domain and device_owners:
            findings.append(("DOMAIN_DEVICE", rel_path, lineno, included, device_owners))

        if opener == "<" and _is_hosted_header(included):
            findings.append(("HOSTED_LIB", rel_path, lineno, included, ()))
    return findings


def _scan_targets(
    targets: Iterable[Path], owners: dict[str, set[str]]
) -> list[tuple[str, str, int, str, tuple[str, ...]]]:
    """Read and scan every target, reporting unreadable files as findings."""
    findings: list[tuple[str, str, int, str, tuple[str, ...]]] = []
    for path in targets:
        module = _module_for(path)
        if module is None:
            continue
        try:
            source = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            findings.append(("READ_ERROR", _rel(path), 0, "unreadable source", ()))
            continue
        findings.extend(_scan_text(source, _rel(path), module, owners))
    return findings


@dataclass(frozen=True)
class _SelftestCase:
    """One synthetic source and its expected self-test label."""

    source: str
    rel_path: str
    module: str
    label: str


def _expect_rule(
    rule: str, case: _SelftestCase, owners: dict[str, set[str]], failures: list[str]
) -> None:
    """Require one synthetic include to trigger the selected rule."""
    findings = _scan_text(case.source, case.rel_path, case.module, owners)
    expect(any(finding[0] == rule for finding in findings), case.label, failures)


def _expect_portable(
    case: _SelftestCase,
    owners: dict[str, set[str]],
    failures: list[str],
) -> None:
    """Require one synthetic portable include set to remain finding-free."""
    expect(not _scan_text(case.source, case.rel_path, case.module, owners), case.label, failures)


def _selftest_violations(owners: dict[str, set[str]], failures: list[str]) -> None:
    """Prove each forbidden dependency class is detected."""
    _expect_rule(
        "CORE_UPWARD",
        _SelftestCase(
            '#include "ra8_gpio.h"\n',
            "libs/ra8_core/x.c",
            "ra8_core",
            "ra8_core upward dependency fires",
        ),
        owners,
        failures,
    )
    _expect_rule(
        "CROSS_INTERNAL",
        _SelftestCase(
            '#include "ra8_io_private_internal.h"\n',
            "libs/ra8_ftl/x.c",
            "ra8_ftl",
            "cross-library internal header fires",
        ),
        owners,
        failures,
    )
    _expect_rule(
        "DOMAIN_DEVICE",
        _SelftestCase(
            '[Ring 4 / Domain]\n#include "ra8_gpio.h"\n',
            "libs/ra8_book/x.c",
            "ra8_book",
            "Domain dependency on a device contract fires",
        ),
        owners,
        failures,
    )
    _expect_rule(
        "HOSTED_LIB",
        _SelftestCase(
            "#include <dirent.h>\n",
            "libs/ra8_book/x.c",
            "ra8_book",
            "hosted filesystem header in a library fires",
        ),
        owners,
        failures,
    )


def _selftest_portable(owners: dict[str, set[str]], failures: list[str]) -> None:
    """Prove portable and same-module dependencies remain accepted."""
    _expect_portable(
        _SelftestCase(
            '#include <string.h>\n#include "ra8_err.h"\n',
            "libs/ra8_book/x.c",
            "ra8_book",
            "portable ISO C and public lower-layer includes stay quiet",
        ),
        owners,
        failures,
    )
    _expect_portable(
        _SelftestCase(
            '#include "ra8_io_private_internal.h"\n',
            "libs/ra8_io/x.c",
            "ra8_io",
            "same-module internal header stays quiet",
        ),
        owners,
        failures,
    )
    _expect_portable(
        _SelftestCase(
            '[Ring 4 / Domain]\n#include "ra8_err.h"\n',
            "libs/ra8_book/x.c",
            "ra8_book",
            "Domain dependency on a portable lower contract stays quiet",
        ),
        owners,
        failures,
    )


def _selftest() -> int:
    """Prove all four rules fire and portable/same-module includes stay quiet."""
    print("check_core_layering.py --selftest")
    failures: list[str] = []
    owners = {
        "ra8_err.h": {"ra8_core"},
        "ra8_gpio.h": {"ra8_hal"},
        "ra8_io_private_internal.h": {"ra8_io"},
    }
    _selftest_violations(owners, failures)
    _selftest_portable(owners, failures)
    return report(failures)


def _report(findings: list[tuple[str, str, int, str, tuple[str, ...]]]) -> None:
    """Print actionable diagnostics grouped by stable rule key."""
    print(f"check_core_layering.py: {len(findings)} architecture violation(s):\n", file=sys.stderr)
    for rule, path, lineno, included, owners in findings:
        location = f"{path}:{lineno}" if lineno > 0 else path
        suffix = f" (owned by {', '.join(owners)})" if owners else ""
        print(f"  [{rule}] {location} includes {included}{suffix}", file=sys.stderr)
    print(
        "\nCORE_UPWARD: move the dependency above ra8_core or move a genuinely\n"
        "shared contract into the foundation. CROSS_INTERNAL: publish a narrow\n"
        "backend/adapter contract under the owner library's inc/ directory.\n"
        "DOMAIN_DEVICE: invert the device dependency behind a portable interface\n"
        "and bind it in the application composition root.\n"
        "HOSTED_LIB: move the OS operation under port/ and inject it through a\n"
        "platform-neutral interface. Do not add an include path or local shim\n"
        "that hides the same dependency.",
        file=sys.stderr,
    )


def main(argv: list[str]) -> int:
    """Scan first-party libraries, or run the detector's bidirectional selftest."""
    if len(argv) == SELFTEST_ARG_COUNT and argv[1] == "--selftest":
        return _selftest()
    if "--selftest" in argv[1:]:
        print("check_core_layering.py: --selftest accepts no paths", file=sys.stderr)
        return 2

    paths = argv[1:]
    targets = _enumerate_targets(paths)
    if not paths and len(targets) < FILE_FLOOR:
        print(
            f"check_core_layering.py: FATAL -- only {len(targets)} library file(s) "
            f"in scope, floor is {FILE_FLOOR}. A collapsed sweep is not clean.",
            file=sys.stderr,
        )
        return 2
    if not targets:
        print("check_core_layering.py: no library files to scan", file=sys.stderr)
        return 0

    owners = _header_owners()
    if len(owners) < OWNER_FLOOR:
        print(
            f"check_core_layering.py: FATAL -- header ownership map holds only "
            f"{len(owners)} header(s), floor is {OWNER_FLOOR}. A collapsed oracle "
            "cannot establish layering.",
            file=sys.stderr,
        )
        return 2

    findings = _scan_targets(targets, owners)
    if findings:
        _report(findings)
    else:
        print(
            f"check_core_layering.py: {len(targets)} library file(s) scanned; "
            "foundation, module privacy, Domain/device, and hosted-API boundaries are clean."
        )
    return int(bool(findings))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
