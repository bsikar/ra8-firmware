#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Emit ONE ``compile_commands.json`` covering every cross-compiled first-party TU.

WHY THIS EXISTS
---------------
``clang_tidy.sh`` parses against the HOST compile database produced by the unit
test build. That database describes ``libs/``, ``src/``, ``tests/`` and
``tools/`` and nothing else: no ``-mcpu=cortex-m85``, no per-app include
directories, no vendored RTOS paths. Pointing clang-tidy at the firmware anyway
was measured, not guessed -- 135 findings across 96 files, every one a
``clang-diagnostic-error`` and not one an actionable style finding (#369).

The fix is not a wider glob, it is a database that actually describes how those
translation units compile. CMake already knows: it emits
``compile_commands.json`` from any configure. This script performs the
configures and merges their output into a single database keyed by real,
absolute source paths.

WHAT IT COVERS AND HOW IT FINDS IT
----------------------------------
1. The unified RA8D2 cross-configure with every ``RA8_USE_*`` middleware option
   forced ON, so apps that ``USES threadx`` / ``usbx`` / ``mbedtls`` are
   configured instead of skipped. This alone accounts for the overwhelming
   majority of firmware TUs.
2. Whatever the unified configure still leaves out, discovered by DIFFING the
   result against ``git ls-files`` -- never from a hardcoded app list. For each
   app directory still missing, the shared ``scripts/dev/ra8_apps.py`` registry
   supplies the source directory and toolchain used by ``just apps::build``.
   The app's own CMake ``ra8_add_app(USES ...)`` declaration supplies its
   middleware switches in a standalone configure. That is how the RA8P1 tier
   and ``ra8_cache_store_demo`` are picked up without duplicating their build
   choices here.
3. A last-resort derivation for TUs that NO configure compiles, described
   below.

Step 2 is the load-bearing one. A hardcoded residual list is the exact defect
that #296 / #332 / #358 / #359 / #360 were each an instance of: a scan list
that silently stops matching the tree. Deriving the residual means a new app,
at any depth, in any tier, needing any middleware, is picked up the day it
lands -- or fails this script loudly if it cannot be configured at all.

TUs THAT NOTHING BUILDS
-----------------------
One first-party TU is compiled by no configure at all, for a reason that is a
fact about the tree rather than a defect in the enumeration above:

  * ``examples/.../dfu_copy_to_run/src/payload.c`` -- a freestanding image linked at
    a fixed SRAM base by its own
    ``examples/ek_ra8d2/hw_validated/hil/dfu_copy_to_run/scripts/build_payload.sh``,
    never by CMake.

Being unbuilt is not a licence to go unlinted: it is first-party firmware and
CLAUDE.md holds it to the same bar. So a command is DERIVED from the nearest
already-covered sibling TU -- same directory subtree, therefore the same
middleware, the same board layer and the same include set -- and then VERIFIED
by actually running the cross-compiler over the file with it.

The verification is what separates this from guessing. A derived command that
does not compile the file is a hard error here, so the fallback can never
quietly hand clang-tidy a wrong command and let a bogus parse read as a clean
lint. Nothing is exempted and no path is allowlisted; a file either gets a
command that provably works or this script fails.

THE COVERAGE ASSERTION IS THE POINT
-----------------------------------
Any first-party cross-compiled ``.c`` absent from the merged database fails the
run. This script must not quietly emit a smaller database over which clang-tidy
would report a clean result. ``--check`` remains as a compatibility spelling
for CI, but completeness is mandatory for every caller.

USAGE
-----
    build_cross_compile_db.py --selftest      # assert the merge + gap logic fires
    build_cross_compile_db.py -o DIR          # write a complete compile_commands.json
    build_cross_compile_db.py -o DIR --check  # compatibility spelling; same strict verdict
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from functools import cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "dev"))
from git_environment import (  # noqa: E402 -- repository path added above
    isolated_git_environment,
    trusted_git_executable,
)
from ra8_apps import get_apps  # noqa: E402 -- path is repository-derived above


@cache
def firmware_apps() -> tuple[dict, ...]:
    """Return the Just app registry once for this generator invocation."""
    return tuple(get_apps())


# Middleware switches the unified configure forces ON so that apps declaring
# `USES <m>` are configured rather than skipped. Derived from the option()
# declarations in cmake/, checked below -- a new cmake/<m>.cmake that this list
# does not know about is a hard error, not a silently smaller database.
#
# RA8_USE_LEVELX_STANDALONE is deliberately absent: cmake/levelx_standalone.cmake
# FATAL_ERRORs when RA8_USE_LEVELX is also set, because both compile the same
# lx_nor_*.c. The app that wants it is picked up by the per-app residual pass.
UNIFIED_MIDDLEWARE_OFF = ("RA8_USE_LEVELX_STANDALONE",)

# Roots whose C is cross-compiled firmware, i.e. exactly the code the host
# compile database cannot describe.
FIRMWARE_ROOTS = ("examples/", "port/")

# ...minus the ports that are HOSTED, not cross-compiled. `port/posix/` binds
# `fw_if_fs` and `ra8_io_stream` to the host kernel's open/read/getdents ABI;
# it declares itself `[Ring 4 / Host Port] {World: Host}` and is compiled ONLY
# by tests/cmake/unit_tests.cmake, so it is already in the HOST compile
# database and is analysed by clang-tidy's host pass. No app cross-compiles it,
# so demanding a cross command for it can only ever be satisfied by a donor
# probe borrowing some unrelated app's flags -- which is how one of its three
# TUs "passed" while the other three failed on an unreachable `fw_if_fs.h`.
# Keep the two lists in step with route_bucket() in scripts/checks/tidy/collect.sh.
HOST_PORT_ROOTS = ("port/posix/",)

# Tests nested below a firmware example are host test translation units. The
# host CMake database owns them; treating their conventional tests/src path as
# firmware asks an Arm donor command to resolve host-only support such as
# unity_minimal.h and fails without analysing either domain correctly.
HOST_TEST_PATH_PART = "/tests/"

# Vendored SOUP under a firmware root -- CLAUDE.md exempts it from first-party
# standards, so it is not part of what this database must cover. None exists
# today; kept as the extension point if a firmware root ever vendors SOUP.
FIRMWARE_EXEMPT: tuple[str, ...] = ()

# A firmware tree this size cannot legitimately collapse to a handful of TUs.
# Fewer than this means a configure silently failed and the "merged" database
# describes almost nothing -- over which clang-tidy would report a clean run.
TU_FLOOR = 250

# How many uncovered paths to print before truncating.
MAX_SHOWN = 40

# The absorb() selftest fixture plants exactly this many distinct source paths.
EXPECTED_FIXTURE_ENTRIES = 2

# How many donor compile commands to try for a TU that no configure builds.
# Each probe is a real compilation, so this bounds the fallback's cost; the
# ordering puts the plausible donors first, so a correct one is found in the
# first few or the TU genuinely has no working command in the tree.
MAX_DONOR_PROBES = 60


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def run(argv: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    """Run `argv`, capturing output. Never raises; callers inspect returncode.

    A missing executable is reported as a failed run rather than an exception:
    donor probing walks compile commands from several toolchains (Arm and
    RISC-V), and only some of them are installed on any given machine. An
    absent compiler must disqualify that donor, not abort the whole build.
    """
    try:
        return subprocess.run(  # noqa: S603 -- argv built from repo-local paths
            argv,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            text=True,
            check=False,
        )
    except (FileNotFoundError, NotADirectoryError, PermissionError) as exc:
        return subprocess.CompletedProcess(argv, returncode=127, stdout="", stderr=str(exc))


def live_files(paths: list[str], root: Path) -> list[str]:
    """Keep only paths whose regular file exists below ``root``."""
    return [rel for rel in paths if (root / rel).is_file()]


def working_tree_files(root: Path = REPO_ROOT) -> list[str]:
    """Every cached or untracked, non-ignored live file, repo-relative."""
    out = run(
        [trusted_git_executable(), "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=root,
    )
    if out.returncode != 0:
        sys.stderr.write(f"git ls-files failed: {out.stderr.strip()}\n")
        sys.exit(1)
    return live_files(out.stdout.splitlines(), root)


def is_firmware_source(rel: str) -> bool:
    """Report whether `rel` is a first-party cross-compiled C source."""
    return (
        rel.endswith(".c")
        and rel.startswith(FIRMWARE_ROOTS)
        and not rel.startswith(FIRMWARE_EXEMPT)
        and not rel.startswith(HOST_PORT_ROOTS)
        and HOST_TEST_PATH_PART not in rel
    )


def firmware_sources() -> set[str]:
    """Repo-relative first-party ``.c`` under the cross-compiled roots."""
    return {rel for rel in working_tree_files() if is_firmware_source(rel)}


# ---------------------------------------------------------------------------
# Compile database entries
# ---------------------------------------------------------------------------
@dataclass
class Database:
    """Accumulates compile-command entries keyed by absolute source path."""

    entries: dict[str, dict] = field(default_factory=dict)

    def absorb(self, build_dir: Path) -> int:
        """Merge ``build_dir/compile_commands.json``. Returns entries added."""
        path = build_dir / "compile_commands.json"
        if not path.is_file():
            return 0
        added = 0
        for entry in json.loads(path.read_text(encoding="utf-8")):
            key = str(Path(entry["directory"], entry["file"]).resolve())
            if key in self.entries:
                continue
            self.entries[key] = entry
            added += 1
        return added

    def add(self, directory: Path, source: Path, argv: list[str]) -> None:
        """Add one hand-assembled compile-database entry (used by the derived-command pass)."""
        key = os.path.realpath(str(source))
        self.entries.setdefault(
            key,
            {
                "directory": str(directory),
                "file": str(source),
                "arguments": argv,
            },
        )

    def covered(self) -> set[str]:
        """Repo-relative paths of every covered source inside the repo."""
        root = str(REPO_ROOT)
        return {os.path.relpath(key, root) for key in self.entries if key.startswith(root + os.sep)}

    def write(self, out_dir: Path) -> Path:
        """Serialise the accumulated entries to compile_commands.json.

        Writes the values of the entry map, not a list built alongside it, so
        a translation unit compiled twice (two configurations of one source)
        contributes exactly one entry -- clangd and clang-tidy both take the
        first match and a duplicate would make which flags apply arbitrary.

        Returns the path written.
        """
        out_dir.mkdir(parents=True, exist_ok=True)
        path = out_dir / "compile_commands.json"
        path.write_text(
            json.dumps(list(self.entries.values()), indent=1) + "\n",
            encoding="utf-8",
        )
        return path


# ---------------------------------------------------------------------------
# Pass 1 -- the unified RA8D2 cross-configure
# ---------------------------------------------------------------------------
def known_middleware_options() -> list[str]:
    """Every ``RA8_USE_*`` option cmake/ declares, read from cmake/ itself.

    Read rather than listed so a new cmake/<middleware>.cmake is picked up
    automatically; a hardcoded list here would be the same silently-stale scan
    list this whole script exists to stop reintroducing.
    """
    names: set[str] = set()
    pattern = re.compile(r"option\(\s*(RA8_USE_[A-Z0-9_]+)")
    for cmake_file in sorted((REPO_ROOT / "cmake").glob("*.cmake")):
        names.update(pattern.findall(cmake_file.read_text(encoding="utf-8")))
    for match in pattern.findall((REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")):
        names.add(match)
    return sorted(names - set(UNIFIED_MIDDLEWARE_OFF))


def configure_unified(build_dir: Path, verbose: bool) -> None:
    """Configure the whole-tree RA8D2 cross build with all middleware ON."""
    argv = [
        "cmake",
        "-B",
        str(build_dir),
        "-S",
        str(REPO_ROOT),
        f"-DCMAKE_TOOLCHAIN_FILE={REPO_ROOT / 'cmake' / 'toolchain-ra8d2.cmake'}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-Wno-dev",
    ]
    argv += [f"-D{name}=ON" for name in known_middleware_options()]
    result = run(argv)
    if result.returncode != 0 or not (build_dir / "compile_commands.json").is_file():
        sys.stderr.write(result.stdout[-4000:])
        sys.stderr.write(result.stderr[-4000:])
        sys.stderr.write("ERROR: the unified RA8D2 cross-configure failed (see above).\n")
        sys.exit(1)
    if verbose:
        print(f"[db] unified RA8D2 configure -> {build_dir}")


# ---------------------------------------------------------------------------
# Pass 2 -- per-app configures for whatever the unified build left out
# ---------------------------------------------------------------------------
def app_dir_for(rel: str) -> Path | None:
    """Nearest ancestor directory of `rel` that is a discovered firmware app.

    Discovery is shared with ``just apps::build`` through ``ra8_apps.py`` so
    this pass cannot grow a second, subtly different definition of an app.
    """
    source = (REPO_ROOT / rel).resolve()
    candidates = [
        Path(app["dir"]).resolve()
        for app in firmware_apps()
        if Path(app["dir"]).resolve() in source.parents
    ]
    return max(candidates, key=lambda path: len(path.parts), default=None)


def configure_argv_for_app(app_dir: Path) -> list[str] | None:
    """Return the standalone configure consumed by ``just apps::build``."""
    app = next(
        (entry for entry in firmware_apps() if Path(entry["dir"]).resolve() == app_dir.resolve()),
        None,
    )
    if app is None:
        return None
    toolchain = REPO_ROOT / app["toolchain"]
    return [
        "cmake",
        "-S",
        str(Path(app["dir"]).resolve()),
        "-B",
        str(app_dir / "build"),
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]


def rewrite_build_dir(argv: list[str], build_dir: Path) -> list[str]:
    """Point a configure argv at `build_dir` and make it export its database."""
    out: list[str] = []
    skip_next = False
    for arg in argv:
        if skip_next:
            skip_next = False
            continue
        if arg == "-B":
            skip_next = True
            continue
        if arg.startswith("-B") and len(arg) > len("-B"):
            continue
        if arg.startswith("-DCMAKE_EXPORT_COMPILE_COMMANDS"):
            continue
        out.append(arg)
    out += ["-B", str(build_dir), "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
    return out


def configure_residual_apps(db: Database, scratch: Path, verbose: bool) -> list[str]:
    """Configure each app still missing from `db`. Returns paths still uncovered."""
    missing = sorted(firmware_sources() - db.covered())
    seen: set[Path] = set()
    for rel in missing:
        app_dir = app_dir_for(rel)
        if app_dir is None or app_dir in seen:
            continue
        seen.add(app_dir)
        argv = configure_argv_for_app(app_dir)
        if argv is None:
            continue
        build_dir = scratch / f"app-{app_dir.name}"
        result = run(rewrite_build_dir(argv, build_dir))
        if result.returncode != 0:
            if verbose:
                print(f"[db] per-app configure FAILED for {app_dir.relative_to(REPO_ROOT)}")
            continue
        added = db.absorb(build_dir)
        if verbose:
            print(f"[db] per-app {app_dir.relative_to(REPO_ROOT)} -> +{added}")
    return sorted(firmware_sources() - db.covered())


# ---------------------------------------------------------------------------
# Pass 3 -- derive a command for TUs no configure builds, then prove it works
# ---------------------------------------------------------------------------
def entry_argv(entry: dict) -> list[str]:
    """The compile command of a database entry, as an argv list."""
    argv = entry.get("arguments")
    if argv:
        return list(argv)
    return shlex.split(entry.get("command", ""))


def own_include_args(source: Path) -> list[str]:
    """``-I`` flags for the include directories `source`'s own library declares.

    Every library in this tree is laid out ``<lib>/inc`` + ``<lib>/src`` -- the
    convention check_header_file_placement.py enforces -- and a port library
    publishes ``<lib>/inc`` through target_include_directories(). A donor from
    another library supplies the middleware and HAL context but cannot supply
    that, so walk the TU's own ancestors and offer theirs.

    This is reading the tree's structure, not inventing flags: a directory is
    only added when it exists, and the result still has to compile the file.
    """
    args: list[str] = []
    current = source.parent
    while current == REPO_ROOT or REPO_ROOT in current.parents:
        candidate = current / "inc"
        if candidate.is_dir():
            args.append(f"-I{candidate}")
        if current == REPO_ROOT:
            break
        current = current.parent
    return args


def retarget(argv: list[str], source: Path, obj: Path) -> list[str]:
    """Rewrite a sibling's compile command to compile `source` into `obj`."""
    out: list[str] = []
    skip_next = False
    for arg in argv:
        if skip_next:
            skip_next = False
            continue
        if arg in ("-c", "-o"):
            skip_next = arg == "-o"
            continue
        if arg.endswith((".c", ".o", ".obj")):
            continue
        out.append(arg)
    return out + own_include_args(source) + ["-c", str(source), "-o", str(obj)]


def shared_prefix_len(a: str, b: str) -> int:
    """How many leading path components `a` and `b` have in common."""
    left, right = a.split("/"), b.split("/")
    count = 0
    for one, other in zip(left, right, strict=False):
        if one != other:
            break
        count += 1
    return count


def candidate_donors(db: Database, rel: str) -> list[dict]:
    """Covered entries ordered by how close they sit to `rel` in the tree.

    Closeness is shared path depth, so the same app or the same port library
    comes first, then the same tier, then the rest. Order is a PREFERENCE, not
    an answer -- the caller proves a donor correct by compiling with it.
    """
    root = str(REPO_ROOT)
    seen_dirs: set[Path] = set()
    scored: list[tuple[int, str, dict]] = []
    for key, entry in db.entries.items():
        if not key.startswith(root + os.sep) or not key.endswith(".c"):
            continue
        parent = Path(key).parent
        if parent in seen_dirs:
            continue
        seen_dirs.add(parent)
        candidate_rel = os.path.relpath(key, root)
        scored.append((-shared_prefix_len(rel, candidate_rel), candidate_rel, entry))
    scored.sort()
    return [entry for _, _, entry in scored[:MAX_DONOR_PROBES]]


def derive_unbuilt(db: Database, scratch: Path, verbose: bool) -> list[str]:
    """Give every still-uncovered TU a sibling-derived, compile-verified command.

    A donor is accepted only once the cross-compiler has compiled the file with
    its flags. Candidates are tried nearest-first; the first that compiles wins.
    Guessing is therefore not possible -- either a provably working command is
    found or the TU is reported uncovered and the run fails.
    """
    still: list[str] = []
    for rel in sorted(firmware_sources() - db.covered()):
        source = REPO_ROOT / rel
        obj = scratch / (rel.replace("/", "_") + ".o")
        accepted = False
        last_error = "no covered TU was available to derive a command from"
        for donor in candidate_donors(db, rel):
            argv = retarget(entry_argv(donor), source, obj)
            probe = run(argv, cwd=Path(donor["directory"]))
            if probe.returncode != 0:
                last_error = probe.stderr.strip()[-1200:]
                continue
            db.add(Path(donor["directory"]), source, argv)
            accepted = True
            if verbose:
                donor_rel = os.path.relpath(donor["file"], REPO_ROOT)
                print(f"[db] derived {rel} from {donor_rel} (compiles clean)")
            break
        if not accepted:
            print(
                f"ERROR: no compile command could be derived for {rel}.\n"
                f"  last attempt failed with:\n  {last_error}",
                file=sys.stderr,
            )
            still.append(rel)
    return still


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def report_uncovered(uncovered: list[str], total: int) -> int:
    """Print the coverage verdict. Returns the process exit code."""
    covered = total - len(uncovered)
    print(f"cross-compile database: {covered}/{total} first-party firmware TUs covered")
    if not uncovered:
        print("OK: every cross-compiled first-party .c has a compile command.")
        return 0
    print(file=sys.stderr)
    print(f"ERROR: {len(uncovered)} firmware TU(s) have no compile command:", file=sys.stderr)
    for rel in uncovered[:MAX_SHOWN]:
        print(f"  {rel}", file=sys.stderr)
    if len(uncovered) > MAX_SHOWN:
        print(f"  ... and {len(uncovered) - MAX_SHOWN} more", file=sys.stderr)
    print(file=sys.stderr)
    print(
        "Each needs a configure that reaches it. Firmware apps are discovered\n"
        "through scripts/dev/ra8_apps.py, and each app's CMakeLists.txt must\n"
        "declare its complete ra8_add_app() dependency surface.",
        file=sys.stderr,
    )
    return 1


# ---------------------------------------------------------------------------
# Selftest -- the gate must be shown to fire, in both directions
# ---------------------------------------------------------------------------
def _selftest_database() -> list[str]:
    """Assertions about Database merging and coverage accounting."""
    failures: list[str] = []

    # 1. absorb() merges, de-duplicates by real path, and reports what it added.
    with tempfile.TemporaryDirectory() as tmp:
        one = Path(tmp) / "one"
        one.mkdir()
        (one / "compile_commands.json").write_text(
            json.dumps(
                [
                    {"directory": str(REPO_ROOT), "file": "a.c", "arguments": ["cc", "a.c"]},
                    {"directory": str(REPO_ROOT), "file": "./a.c", "arguments": ["cc", "a.c"]},
                    {"directory": str(REPO_ROOT), "file": "b.c", "arguments": ["cc", "b.c"]},
                ]
            ),
            encoding="utf-8",
        )
        db = Database()
        added = db.absorb(one)
        if added != EXPECTED_FIXTURE_ENTRIES:
            failures.append(f"absorb() merged {added} entries, expected 2 after de-duplication")
        if db.covered() != {"a.c", "b.c"}:
            failures.append(f"covered() returned {sorted(db.covered())}, expected ['a.c', 'b.c']")

    return failures


def _selftest_reporting() -> list[str]:
    """Assertions about the uncovered-TU verdict."""
    failures: list[str] = []

    # 2. A database missing a TU must be reported as uncovered, not passed over.
    #    The verdict this prints is a DELIBERATE probe against a planted gap --
    #    seeing it fire here is the evidence that a real gap would fail too.
    print("selftest: probing the gap report with a planted uncovered TU ...")
    if report_uncovered(["examples/broken/main.c"], 10) == 0:
        failures.append("report_uncovered() returned success while a TU was uncovered")
    if report_uncovered([], 10) != 0:
        failures.append("report_uncovered() returned failure with nothing uncovered")

    return failures


def _selftest_command_building() -> list[str]:
    """Assertions about option discovery and compile-command construction."""
    failures: list[str] = []

    # 3. The middleware sweep must actually find the vendored options, and must
    #    exclude the mutually-exclusive one. An empty sweep would silently
    #    configure away every `USES` app.
    options = known_middleware_options()
    if "RA8_USE_THREADX" not in options or "RA8_USE_USBX" not in options:
        failures.append(f"known_middleware_options() missed a vendored switch: {options}")
    if "RA8_USE_LEVELX_STANDALONE" in options:
        failures.append("known_middleware_options() included the mutually-exclusive LevelX mode")

    # 4. The residual-app configure must use the same discovered toolchain as
    # `just apps::build`, rather than guessing from a path fragment here.
    apps = firmware_apps()
    if not apps:
        failures.append("ra8_apps.py discovered no firmware apps")
    for app in apps:
        app_dir = Path(app["dir"]).resolve()
        argv = configure_argv_for_app(app_dir)
        expected_toolchain = f"-DCMAKE_TOOLCHAIN_FILE={REPO_ROOT / app['toolchain']}"
        if argv is None or str(app_dir) not in argv or expected_toolchain not in argv:
            failures.append(f"configure argv drifted from ra8_apps.py for {app_dir}")
            break

    # 5. rewrite_build_dir must repoint -B in both spellings and force the export.
    rewritten = rewrite_build_dir(["cmake", "-S", ".", "-B", "/orig", "-DA=1"], Path("/new"))
    if "/orig" in rewritten or "-B" not in rewritten or "/new" not in rewritten:
        failures.append(f"rewrite_build_dir() left the original -B: {rewritten}")
    if "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" not in rewritten:
        failures.append("rewrite_build_dir() did not force the compile-command export")

    # 6. retarget() must strip the donor's own source/object and substitute ours,
    #    otherwise the derived command would compile the DONOR and report a
    #    clean parse for a file it never opened.
    donor_argv = ["arm-none-eabi-gcc", "-Iinc", "-c", "donor.c", "-o", "donor.o"]
    retargeted = retarget(donor_argv, Path("/w/mine.c"), Path("/w/mine.o"))
    if "donor.c" in retargeted or "donor.o" in retargeted:
        failures.append(f"retarget() kept the donor's source or object: {retargeted}")
    if retargeted[-4:] != ["-c", "/w/mine.c", "-o", "/w/mine.o"]:
        failures.append(f"retarget() did not compile the requested file: {retargeted}")
    if "-Iinc" not in retargeted:
        failures.append("retarget() dropped the donor's include flags")

    # 7. entry_argv() must read both database spellings; a database using
    #    "command" would otherwise derive an EMPTY compile line.
    if entry_argv({"arguments": ["cc", "x.c"]}) != ["cc", "x.c"]:
        failures.append("entry_argv() mishandled the 'arguments' form")
    if entry_argv({"command": "cc x.c"}) != ["cc", "x.c"]:
        failures.append("entry_argv() mishandled the 'command' form")

    return failures


def _selftest_source_classification() -> list[str]:
    """Assert app-local host tests and absent index paths stay out of the database."""
    failures: list[str] = []
    if is_firmware_source("examples/demo/tests/src/test_demo.c"):
        failures.append("is_firmware_source() claimed an app-local host test")
    if not is_firmware_source("examples/demo/src/main.c"):
        failures.append("is_firmware_source() dropped an example implementation")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "present.c").write_text("void present(void) {}\n", encoding="ascii")
        if live_files(["present.c", "missing.c"], root) != ["present.c"]:
            failures.append("live_files() did not keep the real file and drop the absent path")
        run([trusted_git_executable(), "init", "--quiet"], cwd=root)
        (root / ".gitignore").write_text("._*\n", encoding="ascii")
        (root / "ordinary.cc").write_text("void ordinary() {}\n", encoding="ascii")
        (root / "._artifact.c").write_bytes(b"\x00AppleDouble")
        enumerated = working_tree_files(root)
        if "present.c" not in enumerated or "ordinary.cc" not in enumerated:
            failures.append("working_tree_files() dropped an ordinary untracked C/C++ file")
        if "._artifact.c" in enumerated:
            failures.append("working_tree_files() included an ignored AppleDouble artifact")
    return failures


def _selftest_body() -> int:
    """Assert the merge and the gap detection both work, and that they fail."""
    failures = (
        _selftest_database()
        + _selftest_reporting()
        + _selftest_command_building()
        + _selftest_source_classification()
    )

    # 9. The floor must reject a database that collapsed to almost nothing.
    sources = firmware_sources()
    if len(sources) < TU_FLOOR:
        failures.append(
            f"firmware_sources() found {len(sources)} TUs, "
            f"below the floor of {TU_FLOOR} -- enumeration is broken"
        )

    # 10. The hosted-port carve-out, both directions. A carve-out that widened
    #    to swallow the cross-compiled ports would drop real firmware out of
    #    the database and read as a smaller, cleaner run; one that stopped
    #    matching would put the host port back in and demand a cross command
    #    that no configure can ever supply.
    if any(rel.startswith(HOST_PORT_ROOTS) for rel in sources):
        failures.append("firmware_sources() still claims a hosted port root")
    if not any(rel.startswith("port/") for rel in sources):
        failures.append("firmware_sources() claims no port/ TU at all -- carve-out too wide")

    if failures:
        print("SELFTEST FAILED:", file=sys.stderr)
        for problem in failures:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("selftest: cross-compile database merge + gap detection OK")
    return 0


def selftest() -> int:
    """Run compile-database fixtures without inheriting the caller's repo."""
    with isolated_git_environment():
        return _selftest_body()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> int:
    """Build a complete cross-compilation database.

    Completeness is mandatory for every caller. ``--check`` remains accepted
    for the CI call site, but cannot weaken or strengthen the verdict.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--out", help="directory to write compile_commands.json into")
    parser.add_argument(
        "--check",
        action="store_true",
        help="compatibility flag; completeness is always enforced",
    )
    parser.add_argument("--selftest", action="store_true", help="assert this script still fires")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.out:
        parser.error("--out is required unless --selftest is given")

    if shutil.which("cmake") is None:
        print(
            "ERROR: cmake not found; the cross-compile database cannot be built.", file=sys.stderr
        )
        return 1

    total = len(firmware_sources())
    if total < TU_FLOOR:
        print(
            f"ERROR: only {total} firmware TUs enumerated (floor {TU_FLOOR}). "
            "The enumeration is broken; refusing to emit a database that would "
            "let clang-tidy report a clean run over almost nothing.",
            file=sys.stderr,
        )
        return 1

    # The CMake build trees live INSIDE the output directory and are kept, not
    # thrown away: every entry's "directory" field names the tree it came from
    # and clang-tidy chdir()s there before parsing. Pointing those at a
    # temporary directory produced a hard LLVM abort once the run cleaned up.
    # Keeping them also makes a re-run an incremental CMake reconfigure.
    # Absolute: donor probes run with cwd set to the DONOR's build directory,
    # so a relative object path would be written relative to that instead --
    # which -fstack-usage turns into "cannot open ....su for writing".
    out_dir = Path(args.out).resolve()
    scratch = out_dir / ".compile_commands_scratch"
    scratch.mkdir(parents=True, exist_ok=True)

    db = Database()
    unified = scratch / "unified"
    configure_unified(unified, args.verbose)
    db.absorb(unified)
    if args.verbose:
        print(f"[db] unified -> {len(db.entries)} entries")
    configure_residual_apps(db, scratch, args.verbose)
    uncovered = derive_unbuilt(db, scratch, args.verbose)
    code = report_uncovered(uncovered, total)
    if code != 0:
        return code
    out_path = db.write(out_dir)
    print(f"wrote {out_path} ({len(db.entries)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
