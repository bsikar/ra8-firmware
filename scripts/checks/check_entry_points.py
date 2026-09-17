#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every first-party entry point uses its own build domain's contract.

Two domains, two contracts, one enforced boundary (#707):

* **Hosted** -- ``tests/`` and ``tools/`` run under an OS that reads an exit
  status, so ISO C applies: ``int main(void)`` or ``int main(int, char**)``.
  Both host compilers enforce this themselves; this gate exists so the
  spelling cannot drift back to ``int32_t`` where nothing would notice.
* **Freestanding** -- ``examples/`` and ``port/`` are bare metal
  reached from ``Reset_Handler``. There is no process and no exit status, so
  the entry point is ``void main(void)``, declared once in
  ``libs/ra8_core/inc/ra8_boot_entry.h``.

``apps/`` -- the products tier -- is the one root that carries BOTH, so it
cannot be classified by its name: see ``FIRMWARE_APPS`` below.

WHY A GATE RATHER THAN TRUSTING THE COMPILER

The compiler catches a *disagreement it can see*. It could not see this one.
``int32_t`` is ``long int`` on arm-none-eabi and ``int`` on the host, so
``int32_t main(void)`` meant a different function type on each side; on the
chip that is ``-Werror=main``, which 208 example files silenced with a local
``#pragma GCC diagnostic ignored "-Wmain"``. Worse, the declaration lived in
sixteen copy-pasted ``extern int32_t main(void);`` lines in vector tables --
a different translation unit from the definition, so roughly thirty apps had
drifted into declaring one type and defining another with nothing to notice.

The single declaration in ``ra8_boot_entry.h`` is what makes the compiler
able to check; requiring firmware entry points to INCLUDE it is what makes
the check actually happen, because GCC exempts ``main`` from
``-Wmissing-prototypes``. This gate enforces the include, the spelling, and
the absence of any renewed suppression.

WHAT IS DELIBERATELY NOT ENFORCED

Type *agreement* between declaration and definition -- that is the
compiler's job once the include is present, and it does it better. This gate
is textual on purpose (see ``funcsize_c.py`` for the same reasoning): it must
cover translation units that never reach a ``compile_commands.json``,
including apps excluded from the unified cross-configure.

``LLVMFuzzerTestOneInput`` harnesses have no ``main`` at all, and the
Cortex-M33 ``cpu1_main`` is ``static`` and never named ``main``; neither is
an entry point by this gate's definition and neither is scanned.

Run::

    python3 scripts/checks/check_entry_points.py            # scan the tree
    python3 scripts/checks/check_entry_points.py --selftest # both directions
    python3 scripts/checks/check_entry_points.py --list     # inventory
    python3 scripts/checks/check_entry_points.py FILE...    # pre-commit mode

Exit 0 if clean, 1 on findings or a failing selftest, 2 on a broken scan.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lint_targets import (  # needs the sys.path line above
    firmware_app_dirs,
    first_party_paths,
)
from selftest_assert import expect, report  # needs the sys.path line above

REPO_ROOT = Path(__file__).resolve().parents[2]

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

BOOT_HEADER = "ra8_boot_entry.h"
BOOT_HEADER_REL = "libs/ra8_core/inc/ra8_boot_entry.h"

# Roots whose entry points are reached from Reset_Handler rather than from a C
# runtime. Derived from where the build actually cross-compiles: the app
# discovery in the top-level CMakeLists globs examples/, and port/ is RTOS glue
# compiled into firmware images. The Ring 5 secure substrate that used to sit
# under src/ is now libs/ra8_secure_app -- pure library code with no entry
# point, so libs/ needs no row here (#724).
FIRMWARE_ROOTS = ("examples/", "port/")
HOSTED_ROOTS = ("tests/", "tools/", "apps/")

# ...and the one root where the name settles nothing. `apps/` is the PRODUCTS
# tier and it carries both domains: the mdl CLI is a host program the C
# runtime starts and whose exit status something reads, while the e-reader is a
# two-image TrustZone composition reached from Reset_Handler. Classifying the
# whole root either way is wrong for half of it -- calling the e-reader hosted
# demands `int main(void)` of a freestanding image and drops the
# `ra8_boot_entry.h` include that makes the cross-TU check happen at all, which
# is precisely the #707 hole.
#
# So the firmware products are NAMED here, and `check_firmware_apps()` re-derives
# the same set from the tree on every whole-tree run. The declaration is what
# keeps classification textual (this gate must reach translation units that
# never appear in a compile database); the derivation is what stops the
# declaration from drifting -- a new firmware product nobody listed FAILS, and a
# listed path that stopped being one FAILS too. Neither half is load-bearing
# alone.
FIRMWARE_APPS = ("apps/board/stand_alone/ereader/",)

# Measured 2026-08-15 on dev @ ad515de20: 234 firmware entry points, 637
# hosted ones. A tree this size cannot legitimately fall to a handful; an
# enumeration that collapses reports a clean tree because it looked at
# almost nothing. Lower these deliberately, with a reason.
FIRMWARE_FLOOR = 150
HOSTED_FLOOR = 400

# Named paths the scan must still reach. A floor catches a total collapse; this
# catches a subtler one where a root stops being enumerated but the others keep
# the count above the floor.
MUST_DISCOVER = (
    "apps/board/stand_alone/ereader/src/main.c",
    "libs/ra8_core/inc/ra8_boot_entry.h",
)

SUFFIXES = (".c", ".cpp", ".h")

# `void main(void)` / `int main(void)` / `int main(int argc, char **argv)`.
# Anchored at column 0: an entry point is never nested or indented, and the
# anchor keeps the pattern off `static int main_loop(...)` and friends.
MAIN_DEF_RE = re.compile(r"^(?P<ret>[A-Za-z_][A-Za-z0-9_ ]*?)\s+main\s*\((?P<args>[^)]*)\)\s*$")
MAIN_DECL_RE = re.compile(r"^\s*(?:extern\s+)?[A-Za-z_][A-Za-z0-9_ ]*?\s+main\s*\([^)]*\)\s*;\s*$")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
RETURN_VALUE_RE = re.compile(r"^\s*return\s+[^;]+;")
SUPPRESSION_RE = re.compile(
    r'#\s*pragma\s+(?:GCC|clang)\s+diagnostic\s+ignored\s+"-Wmain(?:-return-type)?"'
)
HOSTED_ARGS_OK = (
    "void",
    "int argc, char** argv",
    "int argc, char **argv",
    "int argc, char *argv[]",
    "int argc, char* argv[]",
    "",
)


class ScanError(RuntimeError):
    """The scan stopped being a measurement."""


def domain_of(rel: str, firmware_apps: tuple[str, ...] = FIRMWARE_APPS) -> str | None:
    """Which build domain owns this path, or None if it is neither.

    Nested test build units are hosted even when they verify a firmware
    product. A named firmware product is then tested before the general
    ``apps/`` hosted root.
    """
    if "tests" in rel.split("/"):
        return "hosted"
    if rel.startswith(firmware_apps):
        return "firmware"
    if rel.startswith(HOSTED_ROOTS):
        return "hosted"
    if rel.startswith(FIRMWARE_ROOTS):
        return "firmware"
    return None


def find_main(lines: list[str]) -> tuple[int, str, str] | None:
    """Locate a ``main`` DEFINITION: signature line, return type, arguments."""
    for i, line in enumerate(lines):
        match = MAIN_DEF_RE.match(line.rstrip())
        if not match:
            continue
        ret = " ".join(match.group("ret").split())
        if ret in {"return", "case", "else"} or ret.startswith("//"):
            continue
        # A definition is followed by `{`; a declaration ends in `;` and so
        # never matches the pattern above, but a K&R-era prototype might.
        nxt = next(
            (lines[j].strip() for j in range(i + 1, min(i + 3, len(lines))) if lines[j].strip()), ""
        )
        if not (line.rstrip().endswith("{") or nxt.startswith("{")):
            continue
        return i, ret, " ".join(match.group("args").split())
    return None


def body_returns_a_value(lines: list[str], sig: int) -> int | None:
    """1-based line of the first value-returning `return` in main's body."""
    depth = 0
    started = False
    for i in range(sig, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if lines[i].count("{"):
            started = True
        if started and depth <= 0:
            return None
        if started and depth >= 1 and RETURN_VALUE_RE.match(lines[i]):
            return i + 1
    return None


def copied_main_declarations(rel: str, lines: list[str]) -> list[str]:
    """Reject declarations copied outside the one authoritative header."""
    if rel == BOOT_HEADER_REL:
        return []
    return [
        f"{rel}:{i + 1}: duplicates the main() declaration outside "
        f'{BOOT_HEADER_REL}. Include "{BOOT_HEADER}" instead so every '
        f"firmware definition is checked against one authoritative type."
        for i, line in enumerate(lines)
        if MAIN_DECL_RE.match(line)
    ]


def check_file(
    rel: str, text: str, firmware_apps: tuple[str, ...] = FIRMWARE_APPS
) -> tuple[list[str], str | None]:
    """Findings for one file, plus the domain of any entry point it defines."""
    findings: list[str] = []
    lines = text.splitlines()
    findings.extend(copied_main_declarations(rel, lines))

    for i, line in enumerate(lines):
        if SUPPRESSION_RE.search(line):
            findings.append(
                f"{rel}:{i + 1}: -Wmain suppression. The firmware lane is "
                f"-ffreestanding, so the diagnostic does not apply; a "
                f"suppression here is hiding something else."
            )

    found = find_main(lines)
    if found is None:
        return findings, None
    sig, ret, args = found
    domain = domain_of(rel, firmware_apps)

    if domain is None:
        findings.append(
            f"{rel}:{sig + 1}: defines main() but is under neither a hosted "
            f"root {HOSTED_ROOTS} nor a firmware root {FIRMWARE_ROOTS}, so no "
            f"entry-point contract applies to it. Classify it by moving it, "
            f"or extend the roots in this checker."
        )
        return findings, None

    if domain == "firmware":
        if ret != "void" or args != "void":
            findings.append(
                f"{rel}:{sig + 1}: firmware entry point is "
                f"`{ret} main({args})`; the freestanding contract is "
                f"`void main(void)` (see {BOOT_HEADER})."
            )
        included = {m.group(1) for m in (INCLUDE_RE.match(line) for line in lines) if m}
        if BOOT_HEADER not in included:
            findings.append(
                f"{rel}:{sig + 1}: firmware entry point does not include "
                f'"{BOOT_HEADER}". Without it the compiler never compares this '
                f"definition against the shared declaration, which is exactly "
                f"how ~30 of these drifted out of agreement."
            )
        bad = body_returns_a_value(lines, sig)
        if bad is not None:
            findings.append(
                f"{rel}:{bad}: `return <value>;` inside a `void main`. "
                f"A freestanding entry point has nothing to return to."
            )
    elif ret != "int" or args not in HOSTED_ARGS_OK:
        findings.append(
            f"{rel}:{sig + 1}: hosted entry point is `{ret} main({args})`; "
            f"ISO C requires `int main(void)` or `int main(int, char**)`."
        )

    return findings, domain


def scan(paths: list[str]) -> tuple[list[str], dict[str, int]]:
    """Scan every path, returning findings and a per-domain count."""
    findings: list[str] = []
    counts = {"hosted": 0, "firmware": 0}
    for rel in paths:
        try:
            text = (REPO_ROOT / rel).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        file_findings, domain = check_file(rel, text)
        findings.extend(file_findings)
        if domain:
            counts[domain] += 1
    return findings, counts


def check_shared_declaration() -> list[str]:
    """The one declaration must still exist, and stay freestanding-guarded.

    Everything else in this gate is downstream of this file. Delete the
    declaration and every firmware entry point silently loses the
    cross-translation-unit check that is the whole point of #707; drop the
    guard and every hosted TU that includes the header stops compiling with
    `conflicting types for 'main'`.
    """
    rel = BOOT_HEADER_REL
    try:
        text = (REPO_ROOT / rel).read_text(encoding="utf-8")
    except OSError:
        return [f"{rel}: missing. It holds the one declaration of main()."]

    findings = []
    if not re.search(r"^void main\(void\);$", text, re.MULTILINE):
        findings.append(
            f"{rel}: no `void main(void);` declaration. Firmware entry points "
            f"include this header so the compiler can compare their definition "
            f"against it; without the declaration nothing is checked."
        )
    if "__STDC_HOSTED__" not in text:
        findings.append(
            f"{rel}: the main() declaration is not guarded by "
            f"`__STDC_HOSTED__ == 0`. Unguarded, every hosted TU including "
            f"this header fails with `conflicting types for 'main'`."
        )
    return findings


def check_firmware_apps(paths: list[str] | None = None) -> list[str]:
    """``FIRMWARE_APPS`` must still name exactly the firmware products present.

    ``lint_targets.firmware_app_dirs()`` derives the set from what the build
    does with a directory -- an app dir under ``apps/`` holding both a linker
    script and a ``vector_table.c`` is linked into an image, not started by a C
    runtime. Both directions of a disagreement are silent failures, which is
    why this is a hard error rather than a warning:

    * a firmware product missing from the tuple is classified HOSTED, so the
      gate demands ISO ``int main`` of a freestanding image and stops requiring
      the ``ra8_boot_entry.h`` include;
    * a stale entry classifies nothing at all, and goes on reporting success
      forever.

    ``paths`` defaults to the whole tracked tree deliberately: this gate's own
    scan carries only C/C++ sources, and a linker script is half the evidence.
    Pass a list only from a selftest fixture.
    """
    derived = tuple(f"{d}/" for d in firmware_app_dirs(paths))
    if derived == FIRMWARE_APPS:
        return []
    missing = [
        f"{d}: a firmware product (linker script + vector_table.c) that "
        f"FIRMWARE_APPS does not name, so this gate classifies it as a hosted "
        f"program and applies the wrong entry-point contract to it. Add it to "
        f"FIRMWARE_APPS in check_entry_points.py."
        for d in derived
        if d not in FIRMWARE_APPS
    ]
    stale = [
        f"{d}: named in FIRMWARE_APPS but no longer a firmware product (no "
        f"linker script + vector_table.c pair). A stale entry classifies "
        f"nothing and reports success forever; drop it."
        for d in FIRMWARE_APPS
        if d not in derived
    ]
    return missing + stale


def enforce_floors(counts: dict[str, int], paths: list[str]) -> None:
    """Raise unless the sweep still reached enough to be a measurement."""
    if counts["firmware"] < FIRMWARE_FLOOR:
        msg = (
            f"{counts['firmware']} firmware entry point(s) found, floor is "
            f"{FIRMWARE_FLOOR}. The scan stopped reaching them; a clean tree "
            f"is NOT the explanation."
        )
        raise ScanError(msg)
    if counts["hosted"] < HOSTED_FLOOR:
        msg = (
            f"{counts['hosted']} hosted entry point(s) found, floor is "
            f"{HOSTED_FLOOR}. The scan stopped reaching them."
        )
        raise ScanError(msg)
    missing = [p for p in MUST_DISCOVER if p not in set(paths)]
    if missing:
        msg = f"the scan no longer reaches: {', '.join(missing)}"
        raise ScanError(msg)


def _selftest_quiet(failures: list[str]) -> None:
    """MUST STAY QUIET: conforming entry points produce no finding."""
    good_fw = '#include "ra8_boot_entry.h"\nvoid main(void)\n{\n  for (;;) {\n  }\n}\n'
    quiet, domain = check_file("examples/x/src/main.c", good_fw)
    expect(not quiet, f"a conforming firmware entry point is silent (got {quiet})", failures)
    expect(domain == "firmware", f"classified firmware (got {domain})", failures)

    good_hosted = "int main(void)\n{\n  return 0;\n}\n"
    quiet, domain = check_file("tests/misc/src/test_x.c", good_hosted)
    expect(not quiet, f"a conforming hosted entry point is silent (got {quiet})", failures)
    expect(domain == "hosted", f"classified hosted (got {domain})", failures)

    good_product_fw = '#include "ra8_boot_entry.h"\nvoid main(void)\n{\n  for (;;) {\n  }\n}\n'
    quiet, domain = check_file("apps/board/stand_alone/ereader/src/main.c", good_product_fw)
    expect(not quiet, f"a conforming firmware PRODUCT is silent (got {quiet})", failures)
    expect(
        domain == "firmware",
        f"a named firmware product classifies firmware (got {domain})",
        failures,
    )

    quiet, domain = check_file("apps/host/mdl/src/main.c", "int main(void)\n{\n  return 0;\n}\n")
    expect(not quiet, f"a conforming hosted PRODUCT is silent (got {quiet})", failures)
    expect(domain == "hosted", f"an unnamed product stays hosted (got {domain})", failures)

    quiet, domain = check_file("apps/board/stand_alone/ereader/tests/src/test_main.c", good_hosted)
    expect(not quiet, f"a firmware product's hosted test stays silent (got {quiet})", failures)
    expect(domain == "hosted", f"a nested product test is hosted (got {domain})", failures)

    argv_main = "int main(int argc, char** argv)\n{\n  return 0;\n}\n"
    expect(
        not check_file("tools/t/src/main.c", argv_main)[0],
        "hosted int main(int, char**) is silent",
        failures,
    )

    expect(
        not check_file("libs/ra8_core/src/x.c", "static int main_loop(void)\n{\n}\n")[0],
        "a function merely NAMED like main is not an entry point",
        failures,
    )
    expect(
        not check_file(BOOT_HEADER_REL, "void main(void);\n")[0],
        "the one declaration in the shared header is silent",
        failures,
    )


def _selftest_fires(failures: list[str]) -> None:
    """MUST FIRE: every rule catches its own violation."""
    fires = {
        "firmware entry point returning int": (
            "examples/x/src/main.c",
            '#include "ra8_boot_entry.h"\nint main(void)\n{\n  return 0;\n}\n',
        ),
        "firmware entry point missing the shared header": (
            "examples/x/src/main.c",
            "void main(void)\n{\n}\n",
        ),
        "value-returning return inside void main": (
            "examples/x/src/main.c",
            '#include "ra8_boot_entry.h"\nvoid main(void)\n{\n  return 1;\n}\n',
        ),
        "hosted entry point spelled int32_t": (
            "tests/misc/src/test_x.c",
            "int32_t main(void)\n{\n  return 0;\n}\n",
        ),
        "a renewed -Wmain suppression": (
            "examples/x/src/main.c",
            '#include "ra8_boot_entry.h"\n'
            '#pragma GCC diagnostic ignored "-Wmain"\n'
            "void main(void)\n{\n}\n",
        ),
        "an entry point in an unclassifiable root": (
            "libs/ra8_core/src/x.c",
            "int main(void)\n{\n  return 0;\n}\n",
        ),
        "a copied main declaration outside the shared header": (
            "examples/x/src/vector_table.c",
            "extern int32_t main(void);\n",
        ),
        "a firmware PRODUCT written to the hosted contract": (
            "apps/board/stand_alone/ereader/src/main.c",
            '#include "ra8_boot_entry.h"\nint main(void)\n{\n  return 0;\n}\n',
        ),
        "a hosted PRODUCT written to the freestanding contract": (
            "apps/host/mdl/src/main.c",
            "void main(void)\n{\n}\n",
        ),
    }
    for label, (rel, text) in fires.items():
        expect(bool(check_file(rel, text)[0]), f"MUST FIRE: {label}", failures)


def _selftest_scope(failures: list[str]) -> None:
    """The scan still reaches what it must, and still excludes what it must."""
    live = discover()
    expect(
        any(p.startswith("examples/") for p in live), "the live scan reaches examples/", failures
    )
    expect(any(p.startswith("tests/") for p in live), "the live scan reaches tests/", failures)
    expect(
        all(not p.startswith(("libs/third_party/", "apps/shared_libs/third_party/")) for p in live),
        "the live scan excludes vendored SOUP",
        failures,
    )

    # The floor is a testable predicate, not just an inline branch.
    collapsed_rejected = False
    try:
        enforce_floors({"hosted": 0, "firmware": 0}, list(MUST_DISCOVER))
    except ScanError:
        collapsed_rejected = True
    expect(collapsed_rejected, "MUST FIRE: a collapsed scan is rejected", failures)

    expect(
        not check_shared_declaration(),
        "the live shared declaration is present and guarded",
        failures,
    )

    # The products tier carries both domains, and the live tree must still say
    # so -- if apps/ ever held only hosted products this gate's extra rule
    # would be dead weight nobody would notice.
    expect(
        not check_firmware_apps(),
        "FIRMWARE_APPS agrees with the products actually in the tree",
        failures,
    )
    expect(
        bool(check_firmware_apps(["libs/ra8_core/src/x.c"])),
        "MUST FIRE: a stale FIRMWARE_APPS entry is rejected",
        failures,
    )
    expect(
        bool(
            check_firmware_apps(
                [
                    "apps/board/stand_alone/ereader/src/vector_table.c",
                    "apps/board/stand_alone/ereader/linker_script.ld",
                    "apps/board/stand_alone/invented/src/vector_table.c",
                    "apps/board/stand_alone/invented/invented.ld",
                ]
            )
        ),
        "MUST FIRE: an unlisted firmware product is rejected",
        failures,
    )
    expect(
        domain_of("apps/board/stand_alone/ereader/src/main.c", ()) == "hosted",
        "without its FIRMWARE_APPS entry the e-reader would be misclassified",
        failures,
    )


def selftest() -> int:
    """Assert both directions: the rules fire, and they stay quiet."""
    print("check_entry_points.py --selftest")
    failures: list[str] = []
    _selftest_quiet(failures)
    _selftest_fires(failures)
    _selftest_scope(failures)
    return report(failures)


def discover() -> list[str]:
    """Every first-party C/C++ path, from git ls-files."""
    return first_party_paths(SUFFIXES)


def main(argv: list[str]) -> int:
    """CLI entry point; see the module docstring for the modes."""
    args = argv[1:]
    if "--selftest" in args:
        return selftest()

    explicit = [a for a in args if not a.startswith("-")]
    paths = explicit or discover()

    findings, counts = scan(paths)
    if not explicit:
        findings = check_shared_declaration() + check_firmware_apps() + findings

    if "--list" in args:
        for rel in paths:
            text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="ignore")
            found = find_main(text.splitlines())
            if found:
                print(f"{domain_of(rel) or 'UNCLASSIFIED':<13}{rel}")
        return EXIT_OK

    if not explicit:
        # The floors describe a whole-tree sweep; an explicit file list from
        # the pre-commit hook legitimately narrows to one file.
        try:
            enforce_floors(counts, paths)
        except ScanError as exc:
            print(f"check_entry_points.py: FATAL -- {exc}", file=sys.stderr)
            return EXIT_CONFIG

    if findings:
        print(
            f"check_entry_points.py: {len(findings)} entry-point violation(s):\n",
            file=sys.stderr,
        )
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        print(
            "\nFirmware entry points are `void main(void)` and include "
            f'"{BOOT_HEADER}"; hosted ones use ISO `int main(...)`. '
            "See docs/STYLE_GUIDE.md.",
            file=sys.stderr,
        )
        return EXIT_FAIL

    print(
        f"check_entry_points.py: {counts['firmware']} firmware + "
        f"{counts['hosted']} hosted entry point(s), no findings."
    )
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv))
