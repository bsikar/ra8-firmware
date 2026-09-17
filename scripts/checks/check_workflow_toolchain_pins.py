#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a toolchain version pinned inside a workflow agrees with its owner.

The defect class
----------------
``.devcontainer/Dockerfile`` is this tree's single source of truth for native
toolchain pins.  ``scripts/ci/lib/lang_toolchains.sh`` reads ``ARG
ZIG_VERSION`` out of it rather than restating it, and
``scripts/checks/check_tool_versions.py`` compares the installed zig against
that same ``ARG``.  Everything on Linux therefore moves together when the pin
moves.

The macOS host-build workflow (#899) cannot: it runs on a GitHub-hosted
``macos-14`` runner that ships no Zig and is not an Ansible-managed image, so
it provisions its own compiler and carries ``env: ZIG_VERSION`` to do it.  That
is a second copy of a pin nothing compared.  Bump the Dockerfile and the Mac
keeps building with the old compiler: both legs stay green, and the two are no
longer measuring the same toolchain.

For #899 that is not a cosmetic skew.  The fix is to pin an explicit
``aarch64-macos`` query so Zig links **its own**
``libc/darwin/libSystem.tbd`` instead of the Command Line Tools stub, and
#1210 added a check that the bundled stub really declares ``arm64-macos``.
That stub ships *with the compiler*.  A version skew means the Linux checks
vet one stub while the only machine that exercises the native link path vets
another, which is exactly the evidence this lane exists to produce.

``extractions/setup-just``'s ``just-version`` is already held to the Dockerfile
pin by ``scripts/ci/check_ci_parity.py``.  This is the same rule for the pins a
workflow carries in ``env:``, which that check does not look at.

Four rules
----------
1. **agreement** -- a workflow ``env`` key that the Dockerfile also declares as
   ``ARG <NAME>=`` must carry the Dockerfile's value.  This is the rule the
   defect above needs; the other three keep it meaningful.
2. **interpolation** -- a pinned value must not be restated as a literal
   anywhere else in the same workflow.  A download URL that spells the version
   out instead of interpolating ``${ZIG_VERSION}`` can be left behind by a bump
   of the very pin above it, which puts rule 1 back to sleep.
3. **digest shape** -- a ``*SHA256*`` pin must be 64 lowercase hex characters.
   A truncated or upper-cased digest fails at ``shasum -c`` time on the runner,
   long after the change that broke it.
4. **digest companion** -- ``<TOOL>_SHA256[_<suffix>]`` requires
   ``<TOOL>_VERSION`` in the same workflow.  A digest with no version beside it
   cannot be read, bumped, or checked by anybody.

Scope, derived rather than listed
---------------------------------
Every ``.github/workflows/*.yml``, and within each one every ``env`` map at
workflow, job, and step level.  The pin *names* come from the Dockerfile, so a
workflow that starts pinning ``RUST_VERSION`` tomorrow is covered the day it
lands; nothing here has to be edited or remembered.

Non-vacuity
-----------
``--selftest`` builds fixtures that violate each rule and asserts each one
fires, builds the compliant form and asserts it stays quiet, asserts no rule is
reported as another, proves the Dockerfile really is being read (the live ``ARG
ZIG_VERSION`` is found, a fabricated ``ARG`` name is not) and that the workflow
scan really reaches ``macos-host.yml``, and finally asserts the live tree is
clean so a green run is not vacuous.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

DOCKERFILE = Path(".devcontainer/Dockerfile")
WORKFLOW_DIR = Path(".github/workflows")

RULE_AGREEMENT = "agreement"
RULE_INTERPOLATION = "interpolation"
RULE_DIGEST_SHAPE = "digest-shape"
RULE_DIGEST_COMPANION = "digest-companion"

_ARG_RE = re.compile(r"^ARG\s+([A-Za-z_][A-Za-z0-9_]*)=(\S*)", re.MULTILINE)
_SHA256_RE = re.compile(r"^(?P<tool>[A-Z0-9]+)_SHA256(?:_[A-Z0-9_]+)?$")
_HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
# A value worth insisting on interpolating: a dotted version or a long digest.
# A bare word ("true", "3") is not a pin anybody bumps in two places.
_PINNED_VALUE_RE = re.compile(r"^(?:\d+\.\d+(?:\.\d+)*|[0-9a-fA-F]{32,})$")


@dataclass(frozen=True)
class Finding:
    """One rule violation, named so the remedy is obvious from the text."""

    rule: str
    workflow: str
    detail: str

    def __str__(self) -> str:
        """Render the finding as one gate line."""
        return f"[{self.rule}] {self.workflow}: {self.detail}"


def dockerfile_pins(text: str) -> dict[str, str]:
    """Every ``ARG NAME=value`` the Dockerfile declares; the first wins."""
    pins: dict[str, str] = {}
    for name, value in _ARG_RE.findall(text):
        pins.setdefault(name, value)
    return pins


def _env_maps(node: object) -> list[dict[str, object]]:
    """Collect every ``env`` mapping at any depth of a parsed workflow."""
    found: list[dict[str, object]] = []
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "env" and isinstance(value, dict):
                found.append(value)
            else:
                found.extend(_env_maps(value))
    elif isinstance(node, list):
        for item in node:
            found.extend(_env_maps(item))
    return found


def workflow_env_pins(text: str) -> dict[str, str]:
    """Flatten a workflow's env maps to ``name -> value`` strings.

    Booleans and nulls are skipped: YAML has already turned those into Python
    objects and no toolchain pin is one.
    """
    document = yaml.safe_load(text)
    pins: dict[str, str] = {}
    for env in _env_maps(document):
        for name, value in env.items():
            if isinstance(value, bool) or value is None:
                continue
            pins.setdefault(str(name), str(value))
    return pins


def _declaration_lines(text: str, name: str, value: str) -> set[int]:
    """Line numbers where ``name: value`` is declared, 1-based."""
    pattern = re.compile(rf"^\s*{re.escape(name)}\s*:\s*[\"']?{re.escape(value)}[\"']?\s*$")
    return {n for n, line in enumerate(text.splitlines(), start=1) if pattern.match(line)}


def restated_literals(text: str, name: str, value: str) -> list[int]:
    """Lines restating ``value`` outside its own declaration and comments."""
    declared = _declaration_lines(text, name, value)
    hits: list[int] = []
    for number, line in enumerate(text.splitlines(), start=1):
        if number in declared:
            continue
        if line.lstrip().startswith("#"):
            continue
        code = line.split(" #", 1)[0]
        if value in code:
            hits.append(number)
    return hits


def audit_workflow(workflow: str, text: str, pins: dict[str, str]) -> list[Finding]:
    """Apply all four rules to one workflow's text."""
    findings: list[Finding] = []
    env = workflow_env_pins(text)

    for name, value in sorted(env.items()):
        owned = pins.get(name)
        if owned is not None and owned != value:
            findings.append(
                Finding(
                    RULE_AGREEMENT,
                    workflow,
                    f"env {name}={value!r} but {DOCKERFILE} declares "
                    f"ARG {name}={owned!r}. The Dockerfile owns this pin "
                    "(scripts/ci/lib/lang_toolchains.sh and "
                    "scripts/checks/check_tool_versions.py both read it); "
                    "bump both together.",
                )
            )

        if _PINNED_VALUE_RE.match(value):
            restated = restated_literals(text, name, value)
            if restated:
                where = ", ".join(f"line {n}" for n in restated)
                findings.append(
                    Finding(
                        RULE_INTERPOLATION,
                        workflow,
                        f"env {name}={value!r} is spelled out again at {where}. "
                        f"Interpolate ${{{name}}} instead, or a bump of the pin "
                        "leaves that copy behind.",
                    )
                )

        digest = _SHA256_RE.match(name)
        if digest:
            if not _HEX64_RE.match(value):
                findings.append(
                    Finding(
                        RULE_DIGEST_SHAPE,
                        workflow,
                        f"env {name}={value!r} is not 64 lowercase hex characters, "
                        "so `shasum -a 256 -c` on the runner is the first thing "
                        "that would notice.",
                    )
                )
            companion = f"{digest.group('tool')}_VERSION"
            if companion not in env:
                findings.append(
                    Finding(
                        RULE_DIGEST_COMPANION,
                        workflow,
                        f"env {name} has no {companion} beside it, so nothing says "
                        "which release this digest belongs to.",
                    )
                )

    return findings


def workflow_files(root: Path | None = None) -> list[Path]:
    """Every workflow this rule applies to, in a stable order."""
    directory = (root or Path()) / WORKFLOW_DIR
    if not directory.is_dir():
        return []
    return sorted(p for p in directory.iterdir() if p.suffix in (".yml", ".yaml"))


def scan_tree(root: Path | None = None) -> list[Finding]:
    """Audit every workflow against the Dockerfile's pins."""
    root = root or Path()
    dockerfile = root / DOCKERFILE
    if not dockerfile.is_file():
        return [
            Finding(
                RULE_AGREEMENT,
                str(DOCKERFILE),
                "the pin owner is missing, so no workflow pin can be verified",
            )
        ]
    pins = dockerfile_pins(dockerfile.read_text(encoding="utf-8"))
    findings: list[Finding] = []
    for path in workflow_files(root):
        text = path.read_text(encoding="utf-8")
        findings.extend(audit_workflow(path.as_posix(), text, pins))
    return findings


_HEX = "a" * 64
_GOOD_WORKFLOW = """---
name: probe
on:
  workflow_dispatch:
env:
  ZIG_VERSION: 1.2.3
  ZIG_SHA256_AARCH64_MACOS: HEXDIGEST
jobs:
  probe:
    runs-on: macos-14
    steps:
      - run: |
          curl -fsSL -o zig.tar.xz "https://example.invalid/zig-${ZIG_VERSION}.tar.xz"
          printf '%s  %s\\n' "${ZIG_SHA256_AARCH64_MACOS}" zig.tar.xz | shasum -a 256 -c -
"""


def _fixture(*replacements: tuple[str, str]) -> str:
    """The compliant workflow, optionally sabotaged one substring at a time."""
    text = _GOOD_WORKFLOW.replace("HEXDIGEST", _HEX)
    for old, new in replacements:
        if old not in text:
            message = f"fixture substring not present: {old!r}"
            raise AssertionError(message)
        text = text.replace(old, new)
    return text


def _rules(findings: list[Finding]) -> set[str]:
    return {f.rule for f in findings}


def _case_table() -> list[tuple[str, str, set[str]]]:
    """Every rule case: label, fixture text, the rules it must produce."""
    return [
        ("the compliant workflow is quiet", _fixture(), set()),
        (
            "a pin that disagrees with the Dockerfile fires agreement",
            _fixture(("ZIG_VERSION: 1.2.3", "ZIG_VERSION: 9.9.9")),
            {RULE_AGREEMENT},
        ),
        (
            "a version spelled out in a run step fires interpolation",
            _fixture(("zig-${ZIG_VERSION}.tar.xz", "zig-1.2.3.tar.xz")),
            {RULE_INTERPOLATION},
        ),
        (
            "a digest spelled out in a run step fires interpolation",
            _fixture(('"${ZIG_SHA256_AARCH64_MACOS}"', f'"{_HEX}"')),
            {RULE_INTERPOLATION},
        ),
        ("a short digest fires digest-shape", _fixture((_HEX, "a" * 40)), {RULE_DIGEST_SHAPE}),
        (
            "an upper-case digest fires digest-shape",
            _fixture((_HEX, "A" * 64)),
            {RULE_DIGEST_SHAPE},
        ),
        (
            "a digest with no version beside it fires digest-companion",
            _fixture(
                ("  ZIG_VERSION: 1.2.3\n", ""),
                ("zig-${ZIG_VERSION}.tar.xz", "zig.tar.xz"),
            ),
            {RULE_DIGEST_COMPANION},
        ),
        (
            "a comment naming the version is not a restatement",
            _fixture(("jobs:", "# the pinned release is 1.2.3, as in the Dockerfile\njobs:")),
            set(),
        ),
    ]


def _selftest_rules(pins: dict[str, str]) -> list[str]:
    """Each rule fires on its own fixture and on no other rule's."""
    failures: list[str] = []
    for label, text, expected in _case_table():
        got = _rules(audit_workflow("fixture.yml", text, pins))
        ok = got == expected
        if not ok:
            failures.append(f"{label}: expected {sorted(expected)}, got {sorted(got)}")
        print(f"  [{'ok' if ok else 'FAIL'}] {label}")

    # A pin the Dockerfile does not own is allowed to exist: the macOS digest is
    # workflow-only by design, and rule 1 must not invent an owner for it.
    unowned = audit_workflow("fixture.yml", _fixture(), {"JUST_VERSION": "1.40.0"})
    if unowned:
        failures.append(f"a workflow-only pin was reported: {[str(f) for f in unowned]}")
    verdict = "ok" if not unowned else "FAIL"
    print(f"  [{verdict}] a pin the Dockerfile does not own is not a finding")
    return failures


def _selftest_sources() -> list[str]:
    """The Dockerfile and the workflow directory really are the inputs."""
    failures: list[str] = []
    if not DOCKERFILE.is_file():
        failures.append(f"{DOCKERFILE} is missing")
        return failures

    live_pins = dockerfile_pins(DOCKERFILE.read_text(encoding="utf-8"))
    failures.extend(
        f"{DOCKERFILE} pin {name} was not read"
        for name in ("ZIG_VERSION", "JUST_VERSION")
        if not live_pins.get(name)
    )
    if live_pins.get("RA8_NOT_A_REAL_PIN"):
        failures.append("a fabricated ARG name resolved to a value")
    print(
        f"  [{'ok' if live_pins.get('ZIG_VERSION') else 'FAIL'}] "
        f"{DOCKERFILE} really is read (ZIG_VERSION={live_pins.get('ZIG_VERSION')!r})"
    )

    names = {p.name for p in workflow_files()}
    if "macos-host.yml" not in names:
        failures.append(f"the workflow scan did not find macos-host.yml (found {sorted(names)})")
    print(
        f"  [{'ok' if 'macos-host.yml' in names else 'FAIL'}] "
        f"the workflow scan finds macos-host.yml ({len(names)} workflow(s))"
    )
    return failures


def _selftest() -> int:
    """Prove every rule fires, the compliant form stays quiet, and the tree is clean."""
    failures = _selftest_rules({"ZIG_VERSION": "1.2.3", "JUST_VERSION": "1.40.0"})
    failures += _selftest_sources()

    live = scan_tree()
    if live:
        failures.append(f"live: {len(live)} finding(s) on the current tree")
    print(f"  [{'ok' if not live else 'FAIL'}] the live tree is clean")

    for line in failures:
        sys.stderr.write(f"check_workflow_toolchain_pins.py: SELFTEST FAIL -- {line}\n")
    if failures:
        return 1
    sys.stdout.write(
        "check_workflow_toolchain_pins.py: selftest OK (4 rules fire, the compliant "
        f"form stays quiet, {len(workflow_files())} workflow(s) in scope)\n"
    )
    return 0


def main() -> int:
    """Entry point: ``--selftest`` proves non-vacuity, otherwise gate the tree."""
    args = sys.argv[1:]
    if "--selftest" in args:
        return _selftest()
    if "--roster" in args:
        pins = dockerfile_pins(DOCKERFILE.read_text(encoding="utf-8"))
        for path in workflow_files():
            env = workflow_env_pins(path.read_text(encoding="utf-8"))
            owned = sorted(n for n in env if n in pins)
            sys.stdout.write(f"{path}: {len(env)} env pin(s), owned: {owned or '-'}\n")
        return 0
    findings = scan_tree()
    if findings:
        sys.stderr.write("check_workflow_toolchain_pins.py: workflow pin problem(s):\n\n")
        for finding in findings:
            sys.stderr.write(f"  {finding}\n\n")
        sys.stderr.write(
            f"{len(findings)} finding(s). {DOCKERFILE} owns the native toolchain pins;\n"
            "a workflow that carries its own copy has to agree with it, or the macOS\n"
            "host-build job (#899) measures a different compiler than every Linux gate.\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
