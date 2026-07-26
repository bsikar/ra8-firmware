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
   app directory still missing, the app's OWN ``Makefile`` is asked for its
   configure line (``make -n``), which is then re-run into a scratch build
   directory. That is how the RA8P1 tier (a different chip, a different
   toolchain file) and ``ra8_cache_store_demo`` (LevelX standalone mode, which
   is mutually exclusive with the LevelX the unified build selects) are picked
   up without this file knowing either fact.
3. A last-resort derivation for TUs that NO configure compiles, described
   below.

Step 2 is the load-bearing one. A hardcoded residual list is the exact defect
that #296 / #332 / #358 / #359 / #360 were each an instance of: a scan list
that silently stops matching the tree. Deriving the residual means a new app,
at any depth, in any tier, needing any middleware, is picked up the day it
lands -- or fails this script loudly if it cannot be configured at all.

TUs THAT NOTHING BUILDS
-----------------------
Two first-party TUs are compiled by no configure at all, for reasons that are
facts about the tree rather than defects in the enumeration above:

  * ``port/filex/src/fx_media_driver_ra8_sdhi.c`` -- the FileX-over-SDHI bridge
    is spliced into an app only when that app declares ``USES filex``, and both
    FileX apps in the tree deliberately take the LevelX FileX port instead.
  * ``examples/.../dfu_copy_to_run/payload.c`` -- a freestanding image linked at
    a fixed SRAM base by its own ``build_payload.sh``, never by CMake.

Being unbuilt is not a licence to go unlinted: they are first-party firmware and
CLAUDE.md holds them to the same bar. So a command is DERIVED from the nearest
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
``--check`` fails when any first-party cross-compiled ``.c`` is absent from the
merged database. Without it this script would degrade exactly the way the thing
it replaces did: quietly emitting a smaller database, over which clang-tidy
would report a clean run. A database that covers less must be an error, not a
faster pass.

USAGE
-----
    build_cross_compile_db.py --selftest      # assert the merge + gap logic fires
    build_cross_compile_db.py -o DIR          # write DIR/compile_commands.json
    build_cross_compile_db.py -o DIR --check  # ... and fail on any uncovered TU
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
from pathlib import Path

REPO_ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- trusted: fixed git argv
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
)

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


def tracked_files() -> list[str]:
    """Every tracked path, repo-relative."""
    out = run(["git", "ls-files"], cwd=REPO_ROOT)
    if out.returncode != 0:
        sys.stderr.write(f"git ls-files failed: {out.stderr.strip()}\n")
        sys.exit(1)
    return out.stdout.splitlines()


def firmware_sources() -> set[str]:
    """Repo-relative first-party ``.c`` under the cross-compiled roots."""
    return {
        rel
        for rel in tracked_files()
        if rel.endswith(".c")
        and rel.startswith(FIRMWARE_ROOTS)
        and not rel.startswith(FIRMWARE_EXEMPT)
    }


def join_continuations(text: str) -> list[str]:
    """Fold backslash-newline continuations so one shell command is one string.

    ``make -n`` prints a recipe line exactly as written, continuations and all,
    so the cmake configure invocation arrives split across several output lines.
    """
    return [line.strip() for line in text.replace("\\\n", " ").splitlines() if line.strip()]


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
    """Nearest ancestor directory of `rel` that is a standalone app.

    An app directory is one holding BOTH a CMakeLists.txt and a Makefile: that
    pair is what makes it configurable on its own terms.
    """
    current = (REPO_ROOT / rel).parent
    while current != REPO_ROOT and REPO_ROOT in current.parents:
        if (current / "CMakeLists.txt").is_file() and (current / "Makefile").is_file():
            return current
        current = current.parent
    return None


def configure_line_from_makefile(app_dir: Path) -> list[str] | None:
    """The app's own cmake configure invocation, read out of its Makefile.

    Asking ``make -n`` means the toolchain file and every extra ``-D`` come
    from the app's Makefile rather than being restated here -- an app that
    switches chip or flips a middleware switch needs no edit to this script.
    """
    # -B (--always-make) is load-bearing. Without it `make -n build` prints
    # "Nothing to be done" whenever the app's ELF is already up to date -- which
    # it is in any tree where the apps have been built, e.g. straight after the
    # build-cross gate. The configure line then goes missing, the residual pass
    # silently configures nothing, and the database comes back smaller. The
    # coverage assertion catches that, but a checker whose answer depends on
    # whether someone happened to build first is wrong regardless of whether
    # something downstream notices.
    dry = run(["make", "-n", "-B", "build"], cwd=app_dir)
    if dry.returncode != 0:
        return None
    for line in join_continuations(dry.stdout):
        if not line.startswith("cmake ") or " -B" not in line:
            continue
        try:
            argv = shlex.split(line)
        except ValueError:
            return None
        if "--build" in argv:
            continue
        return argv
    return None


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
        argv = configure_line_from_makefile(app_dir)
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
    for one, other in zip(left, right):
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
        "Each needs a configure that reaches it. An app is picked up\n"
        "automatically when its directory holds BOTH a CMakeLists.txt and a\n"
        "Makefile whose `build` target configures it -- check that pair first.",
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

    # 4. Continuation folding: a `make -n` recipe split over lines is ONE command.
    #    Asserted through shlex.split rather than on the raw string, because the
    #    exact run of intervening whitespace is not what callers depend on.
    folded = join_continuations("cmake -S . \\\n  -B build \\\n  -DX=1\nnext\n")
    expected = [["cmake", "-S", ".", "-B", "build", "-DX=1"], ["next"]]
    if [shlex.split(line) for line in folded] != expected:
        failures.append(f"join_continuations() produced {folded!r}")

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


def selftest() -> int:
    """Assert the merge and the gap detection both work, and that they fail."""
    failures = _selftest_database() + _selftest_reporting() + _selftest_command_building()

    # 8. The floor must reject a database that collapsed to almost nothing.
    if len(firmware_sources()) < TU_FLOOR:
        failures.append(
            f"firmware_sources() found {len(firmware_sources())} TUs, "
            f"below the floor of {TU_FLOOR} -- enumeration is broken"
        )

    if failures:
        print("SELFTEST FAILED:", file=sys.stderr)
        for problem in failures:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("selftest: cross-compile database merge + gap detection OK")
    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> int:
    """Build a cross-compilation database, or verify the committed one is current.

    ``--check`` regenerates in memory and compares instead of writing, which
    is what CI runs: it fails when the checked-in database has drifted from
    the build system rather than silently rewriting it under the runner.

    Returns 0 on success or a matching ``--check``, non-zero when the database
    is stale.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--out", help="directory to write compile_commands.json into")
    parser.add_argument("--check", action="store_true", help="fail on any uncovered firmware TU")
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
            "make clang-tidy report a clean run over almost nothing.",
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
    scratch = out_dir / "cmake"
    scratch.mkdir(parents=True, exist_ok=True)

    db = Database()
    unified = scratch / "unified"
    configure_unified(unified, args.verbose)
    db.absorb(unified)
    if args.verbose:
        print(f"[db] unified -> {len(db.entries)} entries")
    configure_residual_apps(db, scratch, args.verbose)
    uncovered = derive_unbuilt(db, scratch, args.verbose)
    out_path = db.write(out_dir)

    print(f"wrote {out_path} ({len(db.entries)} entries)")
    code = report_uncovered(uncovered, total)
    return code if args.check else 0


if __name__ == "__main__":
    sys.exit(main())
