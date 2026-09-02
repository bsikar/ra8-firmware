#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every tool a gate declares must exist in the image the gates run in (#513).

Why this exists
---------------
``scripts/ci.sh`` and the fragments under ``scripts/ci/gates/`` declare their
external dependencies with ``require_cmd`` / ``require_python_mod``, which fail
loudly when a tool is absent. That is the right behaviour at run time and it is
far too late: the gate has already been scheduled, a runner has already been
taken, and the verdict is a provisioning error rather than an answer about the
tree. Twice in one day a gate landed asserting a tool the deployed runner image
does not carry -- ``runner-clock`` wanted ``gh``, ``ci-status-contract`` wanted
``jq`` -- and both were caught by somebody noticing, not by a check.

``toolchain-parity`` cannot close this. It reads the pinned ``ARG`` versions out
of ``.devcontainer/Dockerfile`` and compares them against tools that ARE on
PATH; a dependency that is simply missing has no pin to disagree with, so it is
structurally invisible there. This checker asks the other question: does every
declared dependency resolve at all, in the image that will run the gate.

Reaching the image, or failing
------------------------------
The subject is the *deployed image*, never the Dockerfile that is supposed to
describe it -- the two are free to disagree and have (#487, and the pinned
doxygen layer of #486 that was never rebuilt in). So the probe is either:

* **inside the image** -- the gate's normal home is a workflow step on
  ``runs-on: ra8-ci``, where the process already IS the runner container. That
  is proved rather than assumed: the image writes ``/etc/ra8-ci-runner``, and
  without that marker this checker will not claim to have probed it.
* **into the image** -- given a container runtime that holds the image, run the
  same probe inside a throwaway container.

With neither, the answer is EXIT 2 and a message naming both routes. It is not
a pass. A gate that shrugs when it cannot reach its subject re-creates exactly
the blind spot it was written to close.

Non-vacuity
-----------
An extractor that stops matching reports an empty dependency set and therefore
a clean run, forever. Two guards: the scan fails when it finds fewer than
``K_MIN_DEPENDENCIES`` declarations (the tree carries roughly twice that), and
``--selftest`` asserts the extractor on every shape the sources actually use --
plus the negative case, a declaration inside a comment, which must NOT be
collected. The verdict is asserted in both directions too: a present tool must
pass and an absent one must fail.

Run::

    check_runner_image_deps.py                  # auto: marker, else a runtime
    check_runner_image_deps.py --image REF      # probe REF via docker/podman
    check_runner_image_deps.py --local          # probe this PATH (needs marker)
    check_runner_image_deps.py --list           # print what would be probed
    check_runner_image_deps.py --selftest       # prove the checker both ways

Exit 0 when every declared dependency resolves, 1 when any does not, and 2 when
no image could be reached or the sources could not be parsed.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CI_SH = REPO_ROOT / "scripts" / "ci.sh"
GATES_DIR = REPO_ROOT / "scripts" / "ci" / "gates"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_UNREACHABLE = 2

# Written by infra/images/runner/Dockerfile. Its only job is to answer "is this
# process running inside the fleet's runner image" with evidence instead of a
# guess -- an Ubuntu that happens to have the tools is not the subject.
K_IMAGE_MARKER = Path("/etc/ra8-ci-runner")

# The ref the ARC scale set and both Docker hosts boot. Kept as a default so
# the developer path is one flag shorter; the CI path never uses it, because
# there the marker means the probe is already inside the image.
K_DEFAULT_IMAGE = "localhost/ra8-ci-runner:v2"

# Container runtimes that can run a throwaway probe container, in preference
# order. Nothing else in this tree needs one, so absence is ordinary.
K_RUNTIMES = ("docker", "podman")

K_PROBE_TIMEOUT_S = 300

# Floor for the extracted declaration count. The tree carries ~24; a scan that
# finds fewer than this has stopped seeing its subject, which reads as a clean
# run and is the failure this whole file is written against.
K_MIN_DEPENDENCIES = 10

K_KIND_CMD = "cmd"
K_KIND_MOD = "pymod"

# A declaration and its argument. The argument is captured loosely on purpose:
# a `require_cmd "$tool"` is not a dependency this checker can resolve, and it
# has to say so rather than skip the line and report a clean scan.
_DECL_RE = re.compile(r"^[ \t]*require_(cmd|python_mod)[ \t]+(\S+)")

# What a resolvable argument looks like: a literal command or module name.
_LITERAL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")


@dataclass(frozen=True)
class Dependency:
    """One ``require_cmd`` / ``require_python_mod`` declaration found in a gate.

    Attributes:
        kind: ``K_KIND_CMD`` for an executable, ``K_KIND_MOD`` for a Python
            module.
        name: The executable or module name as declared.
        where: ``<path>:<line>`` of the declaration, for failure messages.
    """

    kind: str
    name: str
    where: str


def _fail(message: str) -> None:
    """Print a fatal message and exit 2 -- scan impossible, never scan clean.

    Args:
        message: What could not be done, and what would make it possible.
    """
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(EXIT_UNREACHABLE)


def gate_sources() -> list[Path]:
    """Return the shell files that declare gate dependencies, in scan order.

    Returns:
        ``scripts/ci.sh`` followed by every ``scripts/ci/gates/*.sh`` fragment.
    """
    return [CI_SH, *sorted(GATES_DIR.glob("*.sh"))]


def extract_dependencies(paths: list[Path]) -> tuple[list[Dependency], list[str]]:
    """Collect every dependency declaration in `paths`.

    A declaration inside a comment is not collected: the regex anchors on the
    call at the start of the line, so the prose in these files that *mentions*
    ``require_cmd`` cannot inflate the set.

    Args:
        paths: Shell files to scan.

    Returns:
        A ``(dependencies, unresolvable)`` pair. ``unresolvable`` lists the
        declarations whose argument is not a literal name -- a variable, say --
        which this checker cannot probe and must not silently drop.
    """
    found: list[Dependency] = []
    unresolvable: list[str] = []
    for path in paths:
        if not path.is_file():
            continue
        rel = path.relative_to(REPO_ROOT)
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as error:
            # Every tracked file in this tree is 7-bit ASCII and the `ascii`
            # gate keeps it that way, so this is a foreign file in the scan
            # path rather than a source. Say which, instead of a traceback.
            _fail(f"{rel} is not UTF-8 text and cannot be scanned for dependencies: {error}")
            raise  # unreachable: _fail() exits
        for number, line in enumerate(text.splitlines(), start=1):
            match = _DECL_RE.match(line)
            if match is None:
                continue
            kind = K_KIND_CMD if match.group(1) == "cmd" else K_KIND_MOD
            name = match.group(2)
            where = f"{rel}:{number}"
            if _LITERAL_RE.match(name) is None:
                unresolvable.append(f"{where}: require_{match.group(1)} {name}")
                continue
            found.append(Dependency(kind, name, where))
    return found, unresolvable


def unique_names(deps: list[Dependency], kind: str) -> list[str]:
    """Return the distinct names of `kind` declared in `deps`, sorted.

    Args:
        deps: Extracted declarations.
        kind: ``K_KIND_CMD`` or ``K_KIND_MOD``.

    Returns:
        Sorted, de-duplicated names.
    """
    return sorted({dep.name for dep in deps if dep.kind == kind})


def _probe_script(commands: list[str], modules: list[str]) -> str:
    """Build the shell probe that reports which dependencies resolve.

    The same script runs locally and inside a container, so the two paths
    cannot answer the question differently.

    Args:
        commands: Executable names to look for on PATH.
        modules: Python module names to import.

    Returns:
        A POSIX shell script printing ``<kind> <name> <0|1>`` per dependency.
    """
    lines = ["set -u"]
    lines.extend(
        f'if command -v {name} >/dev/null 2>&1; then echo "cmd {name} 1"; '
        f'else echo "cmd {name} 0"; fi'
        for name in commands
    )
    lines.extend(
        f'if python3 -c "import {name}" >/dev/null 2>&1; then echo "pymod {name} 1"; '
        f'else echo "pymod {name} 0"; fi'
        for name in modules
    )
    return "\n".join(lines) + "\n"


def _parse_probe(output: str) -> dict[tuple[str, str], bool]:
    """Turn the probe script's output back into a lookup table.

    Args:
        output: The probe's stdout.

    Returns:
        Mapping of ``(kind, name)`` to whether it resolved.
    """
    table: dict[tuple[str, str], bool] = {}
    for line in output.splitlines():
        fields = line.split()
        expected_fields = 3
        if len(fields) != expected_fields:
            continue
        table[(fields[0], fields[1])] = fields[2] == "1"
    return table


def _run_probe(argv: list[str], script: str) -> str:
    """Run the probe with `argv`, feeding `script` on stdin, and return stdout.

    Args:
        argv: The command that executes a shell reading the script from stdin.
        script: The probe script.

    Returns:
        The probe's stdout.
    """
    try:
        proc = subprocess.run(  # noqa: S603 -- fixed argv, no shell, tools via shutil.which
            argv,
            input=script,
            capture_output=True,
            text=True,
            timeout=K_PROBE_TIMEOUT_S,
            check=False,
        )
    except subprocess.TimeoutExpired:
        _fail(f"the probe did not finish within {K_PROBE_TIMEOUT_S}s: {' '.join(argv)}")
    except OSError as error:
        _fail(f"could not run the probe ({' '.join(argv)}): {error}")
    if proc.returncode != 0 and not proc.stdout.strip():
        _fail(
            f"the probe failed (rc={proc.returncode}) and produced nothing: "
            f"{' '.join(argv)}\n{proc.stderr.strip()}"
        )
    return proc.stdout


def in_runner_image() -> bool:
    """Report whether this process is running inside the fleet's runner image.

    Returns:
        True when the image's own marker file is present.
    """
    return K_IMAGE_MARKER.is_file()


def find_runtime(preferred: str | None) -> str | None:
    """Locate a container runtime able to run a probe container.

    Args:
        preferred: A runtime named on the command line, or None to search.

    Returns:
        The resolved executable path, or None when there is none.
    """
    for name in [preferred] if preferred else list(K_RUNTIMES):
        found = shutil.which(name)
        if found is not None:
            return found
    return None


def probe_local(script: str) -> str:
    """Run the probe on this machine's PATH.

    Args:
        script: The probe script.

    Returns:
        The probe's stdout.
    """
    shell = shutil.which("bash") or shutil.which("sh")
    if shell is None:
        _fail("no bash or sh on PATH; the probe cannot run")
    return _run_probe([str(shell), "-s"], script)


def probe_image(runtime: str, image: str, script: str) -> str:
    """Run the probe inside a throwaway container from `image`.

    Args:
        runtime: Path to the container runtime executable.
        image: Image reference to probe.
        script: The probe script.

    Returns:
        The probe's stdout.
    """
    argv = [runtime, "run", "--rm", "--interactive", "--entrypoint", "bash", image, "-s"]
    return _run_probe(argv, script)


def _resolve_probe(args: argparse.Namespace, script: str) -> tuple[str, str]:
    """Choose how to reach the image, run the probe, and say what was probed.

    Args:
        args: Parsed command line.
        script: The probe script.

    Returns:
        A ``(subject, output)`` pair; `subject` names what was actually probed.

    Raises:
        SystemExit: Exit 2 when no image can be reached, which is the whole
            point of this function: it never falls back to "probed nothing".
    """
    if args.local and not in_runner_image():
        _fail(
            f"--local was asked for, but {K_IMAGE_MARKER} is absent, so this is not the "
            "fleet's runner image. Probing this PATH would answer a question nobody asked."
        )
    if args.local or (args.image is None and in_runner_image()):
        marker = K_IMAGE_MARKER.read_text(encoding="utf-8").strip()
        return f"this process, inside {marker}", probe_local(script)
    image = args.image or K_DEFAULT_IMAGE
    runtime = find_runtime(args.runtime)
    if runtime is None:
        _fail(
            f"cannot reach an image to probe. This process is not inside the runner image "
            f"({K_IMAGE_MARKER} is absent) and no container runtime "
            f"({', '.join(K_RUNTIMES)}) is on PATH to start one from {image}. Run this "
            "gate on `runs-on: ra8-ci`, where the step already executes in the image, or "
            "give it a runtime that holds the image. It will not report a clean scan it "
            "did not perform."
        )
    return f"{image} via {Path(runtime).name}", probe_image(runtime, image, script)


def report(deps: list[Dependency], table: dict[tuple[str, str], bool], subject: str) -> int:
    """Print the verdict for `deps` against a probe `table`.

    Args:
        deps: Extracted declarations.
        table: Probe results keyed by ``(kind, name)``.
        subject: What was probed, for the report header.

    Returns:
        0 when every dependency resolved, 1 otherwise.
    """
    missing = [dep for dep in deps if not table.get((dep.kind, dep.name), False)]
    commands = len(unique_names(deps, K_KIND_CMD))
    modules = len(unique_names(deps, K_KIND_MOD))
    print(f"runner image dependency scan: {commands} command(s), {modules} python module(s)")
    print(f"probed: {subject}")
    if not missing:
        print("every dependency a gate declares resolves in the image.")
        return EXIT_OK
    seen: set[tuple[str, str]] = set()
    for dep in missing:
        key = (dep.kind, dep.name)
        if key in seen:
            continue
        seen.add(key)
        kind = "command" if dep.kind == K_KIND_CMD else "python module"
        wheres = sorted({other.where for other in missing if (other.kind, other.name) == key})
        print(f"\nMISSING  {kind} '{dep.name}'")
        for where in wheres:
            print(f"         declared at {where}")
    print(
        f"\nFAIL: {len(seen)} declared dependency/dependencies do not exist in {subject}. "
        "Every gate that declares one fails there with a provisioning error instead of a "
        "verdict. Add the tool to .devcontainer/Dockerfile and rebuild the runner image "
        "(infra/images/README.md), or stop declaring it."
    )
    return EXIT_FAIL


def _selftest_extraction() -> None:
    """Assert the extractor collects every real shape and no commented one.

    Raises:
        AssertionError: When a shape the sources use stops being collected, or
            a commented mention starts being collected.
    """
    fragment = (
        "gate_example() (\n"
        "  require_cmd cmake\n"
        '  require_cmd clang-18 "the gate pins clang-18 to match CI"\n'
        "  require_cmd git || exit 1\n"
        "  require_cmd actionlint \\\n"
        '  require_python_mod yaml "run just setup-python"\n'
        "  require_python_mod clang.cindex \\\n"
        "  # require_cmd never_declared_only_mentioned\n"
        "  #   Use require_cmd / require_python_mod for every dependency.\n"
        ")\n"
    )
    # extract_dependencies reports each finding relative to the repo root, so
    # the fragment is staged under it rather than in a temporary directory.
    staged = REPO_ROOT / ".ra8-selftest-fragment.sh"
    staged.write_text(fragment, encoding="utf-8")
    try:
        deps, unresolvable = extract_dependencies([staged])
    finally:
        staged.unlink()
    commands = unique_names(deps, K_KIND_CMD)
    modules = unique_names(deps, K_KIND_MOD)
    expected_commands = ["actionlint", "clang-18", "cmake", "git"]
    expected_modules = ["clang.cindex", "yaml"]
    if commands != expected_commands:
        message = f"selftest: extractor returned commands {commands}, expected {expected_commands}"
        raise AssertionError(message)
    if modules != expected_modules:
        message = f"selftest: extractor returned modules {modules}, expected {expected_modules}"
        raise AssertionError(message)
    if unresolvable:
        message = f"selftest: extractor reported {unresolvable} as unresolvable"
        raise AssertionError(message)


def _selftest_unresolvable() -> None:
    """Assert a non-literal declaration is reported rather than skipped.

    Raises:
        AssertionError: When a ``require_cmd "$tool"`` is silently dropped.
    """
    staged = REPO_ROOT / ".ra8-selftest-unresolvable.sh"
    staged.write_text('  require_cmd "$tool"\n', encoding="utf-8")
    try:
        deps, unresolvable = extract_dependencies([staged])
    finally:
        staged.unlink()
    if deps or not unresolvable:
        message = (
            f"selftest: a non-literal require_cmd produced deps={deps} "
            f"unresolvable={unresolvable}; it must be reported, never dropped"
        )
        raise AssertionError(message)


def _selftest_verdict() -> None:
    """Assert the verdict fires on an absent tool and stays quiet on a present one.

    Raises:
        AssertionError: When either direction is wrong -- a checker that only
            ever passes is the defect this file exists to prevent.
    """
    present = Dependency(K_KIND_CMD, "sh", "selftest:1")
    absent = Dependency(K_KIND_CMD, "ra8-tool-that-cannot-exist", "selftest:2")
    script = _probe_script(["sh", "ra8-tool-that-cannot-exist"], [])
    table = _parse_probe(probe_local(script))
    if not table.get((K_KIND_CMD, "sh"), False):
        message = "selftest: the probe did not find 'sh', so it can no longer find anything"
        raise AssertionError(message)
    if table.get((K_KIND_CMD, "ra8-tool-that-cannot-exist"), True):
        message = "selftest: the probe claimed a tool that cannot exist is present"
        raise AssertionError(message)
    # The verdict is exercised with its output swallowed. Printing a real
    # "FAIL: 1 declared dependency..." block from a PASSING selftest would
    # teach a reader to skim past that exact line in a log, which is the line
    # this gate exists to make them read.
    quiet = io.StringIO()
    with contextlib.redirect_stdout(quiet):
        good = report([present], table, "selftest")
        bad = report([absent], table, "selftest")
    if good != EXIT_OK:
        message = "selftest: a present dependency was reported missing"
        raise AssertionError(message)
    if bad != EXIT_FAIL:
        message = "selftest: an absent dependency was reported as fine"
        raise AssertionError(message)
    if "MISSING" not in quiet.getvalue():
        message = "selftest: the failing verdict printed no MISSING line to act on"
        raise AssertionError(message)


def selftest() -> int:
    """Prove the extractor and the verdict, both directions, before any real scan.

    Returns:
        0 when every assertion holds; an AssertionError escapes otherwise.
    """
    _selftest_extraction()
    _selftest_unresolvable()
    _selftest_verdict()
    print("selftest: extraction, unresolvable-argument reporting and both verdict directions OK")
    return EXIT_OK


def _parser() -> argparse.ArgumentParser:
    """Build the command-line parser.

    Returns:
        The configured parser.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--image",
        default=None,
        help=f"image reference to probe with a container runtime (default {K_DEFAULT_IMAGE} "
        "when this process is not itself inside the runner image)",
    )
    parser.add_argument(
        "--runtime",
        default=None,
        help=f"container runtime to use ({' or '.join(K_RUNTIMES)}); auto-detected by default",
    )
    parser.add_argument(
        "--local",
        action="store_true",
        help="probe this process's own PATH; refuses unless the runner-image marker is present",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="print the dependencies that would be probed, and exit",
    )
    parser.add_argument("--selftest", action="store_true", help="prove the checker, then exit")
    return parser


def _collect() -> list[Dependency]:
    """Extract the dependency set and refuse a scan that has gone blind.

    Returns:
        Every resolvable declaration in the gate sources.

    Raises:
        SystemExit: Exit 2 when a declaration cannot be parsed, or when the
            extractor found implausibly few -- both mean the scan is not
            seeing its subject and a clean report would be a lie.
    """
    deps, unresolvable = extract_dependencies(gate_sources())
    if unresolvable:
        listing = "\n  ".join(unresolvable)
        _fail(
            "these dependency declarations do not name a literal tool, so this gate cannot "
            f"probe them:\n  {listing}\nDeclare the tool by name, or the gate that needs it "
            "goes unchecked."
        )
    if len(deps) < K_MIN_DEPENDENCIES:
        _fail(
            f"only {len(deps)} dependency declaration(s) found across {len(gate_sources())} "
            f"gate source file(s), below the floor of {K_MIN_DEPENDENCIES}. The extractor has "
            "stopped seeing require_cmd / require_python_mod, which would report a clean "
            "image forever."
        )
    return deps


def main(argv: list[str] | None = None) -> int:
    """Extract the declared dependencies and prove each resolves in the image.

    Args:
        argv: Command-line arguments, or None to read ``sys.argv``.

    Returns:
        0 when every dependency resolves, 1 when any does not, 2 when no image
        could be reached.
    """
    args = _parser().parse_args(argv)
    if args.selftest:
        return selftest()
    deps = _collect()
    commands = unique_names(deps, K_KIND_CMD)
    modules = unique_names(deps, K_KIND_MOD)
    if args.list:
        for name in commands:
            print(f"cmd    {name}")
        for name in modules:
            print(f"pymod  {name}")
        return EXIT_OK
    subject, output = _resolve_probe(args, _probe_script(commands, modules))
    return report(deps, _parse_probe(output), subject)


if __name__ == "__main__":
    sys.exit(main())
