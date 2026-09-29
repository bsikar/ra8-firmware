#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the Zig tarball the macOS runner fetches is recorded per release.

The defect class
----------------
``.github/workflows/macos-host.yml`` provisions its own compiler, because the
hosted ``macos-14`` runner ships no Zig (#899).  It does that with two env
pins and one URL::

    ZIG_VERSION: 0.14.1
    ZIG_SHA256_AARCH64_MACOS: 39f3dc5e...
    https://ziglang.org/download/${ZIG_VERSION}/zig-aarch64-macos-${ZIG_VERSION}.tar.xz

``scripts/checks/check_workflow_toolchain_pins.py`` holds ``ZIG_VERSION`` to
``.devcontainer/Dockerfile``'s ``ARG ZIG_VERSION``, and deliberately treats a
workflow-only pin with no ``ARG`` owner as no finding: the digest is
workflow-only by design, so rule 1 must not invent an owner for it.  That
leaves the digest tied to nothing at all.

Bump the Dockerfile's pin and the agreement rule forces the workflow's
``ZIG_VERSION`` to move with it, so the runner fetches the *new* tarball and
checks it against the *old* digest.  Every Linux gate stays green, because no
Linux leg reads either value.  The Mac dies at provisioning with::

    zig.tar.xz: FAILED
    shasum: WARNING: 1 computed checksum did NOT match

which reads as a corrupted download or a tampered mirror, not as a digest
somebody forgot to move.  On a nightly only a Mac runs, that misdiagnosis is
expensive: the natural response is to re-run the job, and it fails identically.

The second half is the tarball *name*.  Zig renamed its release archives
between 0.14.0 and 0.14.1: ``zig-macos-aarch64-0.14.0.tar.xz`` became
``zig-aarch64-macos-0.14.1.tar.xz`` (target first from 0.14.1 on).  The
workflow spells the new shape out, so pinning any release at or before 0.14.0
404s at ``curl`` before the digest is ever consulted.

The fix, and why the table lives here
-------------------------------------
``RECORDED_DISTS`` records, per release and target, the archive name Zig
publishes and its sha256.  A row is added by whoever bumps the pin, from
ziglang.org's published download index, in the same commit and the same diff
the reviewer reads.  The value is not that this is a *source* of truth (it is a
transcription), it is that the transcription is **checkable from Linux**: a
bump that does not carry a row refuses in ``gate_toolchain_parity``, minutes
after it is written, instead of one night later on the only machine in the
suite that cannot be re-run locally.

Five rules
----------
1. **recorded** -- a workflow ``ZIG_SHA256_<TARGET>`` pin needs a row for that
   workflow's ``ZIG_VERSION`` and that target.  This is the rule the defect
   above needs.
2. **digest-agrees** -- the row's digest must equal the pin's value.
3. **owner-recorded** -- the Dockerfile's ``ARG ZIG_VERSION`` must appear in
   the table, so bumping the owner pin alone refuses even before a workflow is
   edited to match it.
4. **tarball-name** -- the download URL in the workflow, with ``${ZIG_VERSION}``
   resolved, must name exactly the archive recorded for that release, and each
   recorded name must match the convention for its own release (target first
   from 0.14.1, os first before it).  This is the ``curl`` 404 above.
5. **record-shape** -- every recorded release parses as a dotted version and
   every recorded digest is 64 lowercase hex characters, so a mistyped row
   cannot pass rules 1 to 4 by accident.

Scope
-----
Every ``.github/workflows/*.yml``.  Nothing here names ``macos-host.yml``: a
second workflow that starts provisioning Zig is covered the day it lands.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

from check_workflow_toolchain_pins import (
    dockerfile_pins,
    workflow_env_pins,
    workflow_files,
)

DOCKERFILE = Path(".devcontainer/Dockerfile")

# Release -> target -> (archive name, sha256), transcribed from
# https://ziglang.org/download/index.json when the pin is bumped.  Only
# releases this tree actually pins need a row; rule 3 refuses a pin with none.
RECORDED_DISTS: dict[str, dict[str, tuple[str, str]]] = {
    "0.14.1": {
        "aarch64-macos": (
            "zig-aarch64-macos-0.14.1.tar.xz",
            "39f3dc5e79c22088ce878edc821dedb4ca5a1cd9f5ef915e9b3cc3053e8faefa",
        ),
    },
}

# Zig put the target before the version from this release on; older archives
# spell the os first (zig-macos-aarch64-0.14.0.tar.xz).
TARGET_FIRST_SINCE = (0, 14, 1)

RULE_RECORDED = "recorded"
RULE_DIGEST_AGREES = "digest-agrees"
RULE_OWNER_RECORDED = "owner-recorded"
RULE_TARBALL_NAME = "tarball-name"
RULE_RECORD_SHAPE = "record-shape"

_SHA256_RE = re.compile(r"^ZIG_SHA256_(?P<target>[A-Z0-9_]+)$")
_HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
_VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)")
_ZIG_URL_RE = re.compile(
    r"https://ziglang\.org/download/(?P<version>[^/\s\"']+)/(?P<file>[^\s\"']+)"
)
_INTERP_RE = re.compile(r"\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?")


@dataclass(frozen=True)
class Finding:
    """One rule violation, named so the remedy is obvious from the text."""

    rule: str
    where: str
    detail: str

    def __str__(self) -> str:
        """Render the finding as one gate line."""
        return f"[{self.rule}] {self.where}: {self.detail}"


def env_target(name: str) -> str | None:
    """``ZIG_SHA256_AARCH64_MACOS`` -> ``aarch64-macos``; else ``None``."""
    match = _SHA256_RE.match(name)
    if not match:
        return None
    return match.group("target").lower().replace("_", "-")


def parse_version(version: str) -> tuple[int, int, int] | None:
    """The numeric release part of a version string, or ``None``."""
    match = _VERSION_RE.match(version)
    if not match:
        return None
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def expected_archive(version: str, target: str) -> str | None:
    """The archive name Zig publishes for a release and target.

    ``None`` when the version does not parse, which rule 5 reports on its own
    rather than guessing a shape for it.
    """
    numeric = parse_version(version)
    if numeric is None:
        return None
    try:
        arch, os_name = target.split("-", 1)
    except ValueError:
        return None
    target_first = numeric >= TARGET_FIRST_SINCE
    stem = f"{arch}-{os_name}" if target_first else f"{os_name}-{arch}"
    return f"zig-{stem}-{version}.tar.xz"


def recorded(version: str, target: str) -> tuple[str, str] | None:
    """The recorded ``(archive, digest)`` for a release and target."""
    return RECORDED_DISTS.get(version, {}).get(target)


def resolve_interpolations(text: str, env: dict[str, str]) -> str:
    """Substitute ``$NAME`` / ``${NAME}`` from ``env``, leaving unknowns."""
    return _INTERP_RE.sub(lambda m: env.get(m.group(1), m.group(0)), text)


def zig_download_urls(text: str, env: dict[str, str]) -> list[tuple[int, str, str]]:
    """Every ziglang.org download URL as ``(line, version, archive)``."""
    found: list[tuple[int, str, str]] = []
    for number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("#"):
            continue
        for match in _ZIG_URL_RE.finditer(line):
            version = resolve_interpolations(match.group("version"), env)
            archive = resolve_interpolations(match.group("file"), env)
            found.append((number, version, archive))
    return found


def audit_records() -> list[Finding]:
    """Rule 5, plus rule 4 against the table's own rows."""
    findings: list[Finding] = []
    for version, targets in sorted(RECORDED_DISTS.items()):
        numeric = parse_version(version)
        if numeric is None:
            findings.append(
                Finding(
                    RULE_RECORD_SHAPE,
                    "RECORDED_DISTS",
                    f"release {version!r} does not parse as a dotted version, so the "
                    "archive name convention cannot be decided for it.",
                )
            )
            continue
        for target, (archive, digest) in sorted(targets.items()):
            if not _HEX64_RE.match(digest):
                findings.append(
                    Finding(
                        RULE_RECORD_SHAPE,
                        "RECORDED_DISTS",
                        f"{version} {target}: digest {digest!r} is not 64 lowercase "
                        "hex characters, so it can only fail on the runner.",
                    )
                )
            expected = expected_archive(version, target)
            if expected is not None and archive != expected:
                findings.append(
                    Finding(
                        RULE_TARBALL_NAME,
                        "RECORDED_DISTS",
                        f"{version} {target}: recorded archive {archive!r} is not the "
                        f"name Zig publishes for that release ({expected!r}). Zig put "
                        "the target before the os from 0.14.1 on.",
                    )
                )
    return findings


def audit_owner(pins: dict[str, str]) -> list[Finding]:
    """Rule 3: the Dockerfile's release has a row."""
    version = pins.get("ZIG_VERSION")
    if not version:
        return [
            Finding(
                RULE_OWNER_RECORDED,
                str(DOCKERFILE),
                "ARG ZIG_VERSION is missing, so the release every Linux leg installs "
                "cannot be checked against the macOS runner's download.",
            )
        ]
    if version not in RECORDED_DISTS:
        return [
            Finding(
                RULE_OWNER_RECORDED,
                str(DOCKERFILE),
                f"ARG ZIG_VERSION={version!r} has no row in RECORDED_DISTS "
                f"(recorded: {sorted(RECORDED_DISTS) or '-'}). Add the archive name and "
                "sha256 from ziglang.org's download index in the same commit as the "
                "bump, or .github/workflows/macos-host.yml fetches a tarball nothing "
                "here has ever checked.",
            )
        ]
    return []


def audit_workflow(workflow: str, text: str) -> list[Finding]:
    """Rules 1, 2 and 4 for one workflow."""
    findings: list[Finding] = []
    env = workflow_env_pins(text)
    version = env.get("ZIG_VERSION")

    for name, value in sorted(env.items()):
        target = env_target(name)
        if target is None:
            continue
        if not version:
            findings.append(
                Finding(
                    RULE_RECORDED,
                    workflow,
                    f"env {name} has no ZIG_VERSION beside it, so no release can be "
                    "looked up for it.",
                )
            )
            continue
        row = recorded(version, target)
        if row is None:
            findings.append(
                Finding(
                    RULE_RECORDED,
                    workflow,
                    f"env {name} pins a digest for zig {version} {target}, which has no "
                    "row in scripts/checks/check_zig_dist_pins.py. A version bump that "
                    "leaves the digest behind fails only on the macos-14 runner, as "
                    "`shasum -c` reporting FAILED, which reads as a bad download.",
                )
            )
            continue
        archive, digest = row
        if value != digest:
            findings.append(
                Finding(
                    RULE_DIGEST_AGREES,
                    workflow,
                    f"env {name}={value!r} but zig {version} {target} is recorded as "
                    f"{digest!r} ({archive}). One of the two was left behind by a bump.",
                )
            )

    for number, url_version, archive in zig_download_urls(text, env):
        if "$" in url_version or "$" in archive:
            findings.append(
                Finding(
                    RULE_TARBALL_NAME,
                    workflow,
                    f"line {number}: the download URL still holds an unresolved "
                    f"interpolation after env substitution ({archive!r}), so what the "
                    "runner fetches cannot be checked from here.",
                )
            )
            continue
        targets = RECORDED_DISTS.get(url_version)
        if not targets:
            findings.append(
                Finding(
                    RULE_TARBALL_NAME,
                    workflow,
                    f"line {number}: the URL fetches zig {url_version}, which has no row "
                    "in RECORDED_DISTS, so neither its name nor its digest is checked.",
                )
            )
            continue
        names = {name for name, _ in targets.values()}
        if archive not in names:
            findings.append(
                Finding(
                    RULE_TARBALL_NAME,
                    workflow,
                    f"line {number}: the URL fetches {archive!r}, but zig {url_version} "
                    f"publishes {sorted(names)}. Zig renamed its archives at 0.14.1 "
                    "(target before os), so a pin either side of that boundary 404s at "
                    "curl before the digest is read.",
                )
            )
    return findings


def scan_tree(root: Path | None = None) -> list[Finding]:
    """Every rule, over the table, the Dockerfile and all workflows."""
    root = root or Path()
    findings = audit_records()
    dockerfile = root / DOCKERFILE
    if not dockerfile.is_file():
        findings.append(
            Finding(
                RULE_OWNER_RECORDED,
                str(DOCKERFILE),
                "the pin owner is missing, so the recorded releases cannot be checked "
                "against the one this tree installs.",
            )
        )
    else:
        findings.extend(audit_owner(dockerfile_pins(dockerfile.read_text(encoding="utf-8"))))
    for path in workflow_files(root):
        findings.extend(audit_workflow(path.as_posix(), path.read_text(encoding="utf-8")))
    return findings


_HEX_A = "a" * 64
_HEX_B = "b" * 64

_GOOD_WORKFLOW = """---
name: probe
on:
  workflow_dispatch:
env:
  ZIG_VERSION: RELEASE
  ZIG_SHA256_AARCH64_MACOS: DIGEST
jobs:
  probe:
    runs-on: macos-14
    steps:
      - run: |
          curl -fsSL -o zig.tar.xz \\
            "https://ziglang.org/download/${ZIG_VERSION}/zig-aarch64-macos-${ZIG_VERSION}.tar.xz"
          printf '%s  %s\\n' "${ZIG_SHA256_AARCH64_MACOS}" zig.tar.xz | shasum -a 256 -c -
"""


def _fixture(*replacements: tuple[str, str]) -> str:
    """The compliant workflow for the live pin, sabotaged one substring at a time."""
    version = sorted(RECORDED_DISTS)[-1]
    digest = RECORDED_DISTS[version]["aarch64-macos"][1]
    text = _GOOD_WORKFLOW.replace("RELEASE", version).replace("DIGEST", digest)
    for old, new in replacements:
        if old not in text:
            message = f"fixture substring not present: {old!r}"
            raise AssertionError(message)
        text = text.replace(old, new)
    return text


def _rules(findings: list[Finding]) -> set[str]:
    return {f.rule for f in findings}


def _case_table() -> list[tuple[str, str, set[str]]]:
    """Workflow cases: label, fixture text, the rules it must produce."""
    version = sorted(RECORDED_DISTS)[-1]
    digest = RECORDED_DISTS[version]["aarch64-macos"][1]
    return [
        ("the compliant workflow is quiet", _fixture(), set()),
        (
            "a digest left behind by a version bump fires digest-agrees",
            _fixture((digest, _HEX_A)),
            {RULE_DIGEST_AGREES},
        ),
        (
            "a release with no recorded row fires recorded and tarball-name",
            _fixture((f"ZIG_VERSION: {version}", "ZIG_VERSION: 9.9.9")),
            {RULE_RECORDED, RULE_TARBALL_NAME},
        ),
        (
            "a digest with no release beside it fires recorded",
            _fixture((f"  ZIG_VERSION: {version}\n", "")),
            {RULE_RECORDED, RULE_TARBALL_NAME},
        ),
        (
            "the pre-0.14.1 archive name fires tarball-name",
            _fixture(("zig-aarch64-macos-${ZIG_VERSION}", "zig-macos-aarch64-${ZIG_VERSION}")),
            {RULE_TARBALL_NAME},
        ),
        (
            "a URL naming an unpinned release fires tarball-name",
            _fixture(("download/${ZIG_VERSION}/", "download/0.13.0/")),
            {RULE_TARBALL_NAME},
        ),
        (
            "an unresolvable interpolation fires tarball-name",
            _fixture(("${ZIG_VERSION}.tar.xz", "${ZIG_RELEASE}.tar.xz")),
            {RULE_TARBALL_NAME},
        ),
        (
            "a commented-out URL is not scanned",
            _fixture(
                (
                    "      - run: |",
                    "      # https://ziglang.org/download/0.13.0/zig.tar.xz\n      - run: |",
                )
            ),
            set(),
        ),
    ]


def _selftest_workflows() -> list[str]:
    """Each workflow rule fires on its own fixture and on no other's."""
    failures: list[str] = []
    for label, text, expected in _case_table():
        got = _rules(audit_workflow("fixture.yml", text))
        ok = got == expected
        if not ok:
            failures.append(f"{label}: expected {sorted(expected)}, got {sorted(got)}")
        print(f"  [{'ok' if ok else 'FAIL'}] {label}")

    # A workflow that provisions no Zig at all must stay quiet: this rule is
    # about the pins a workflow carries, not about every workflow having them.
    quiet = audit_workflow("fixture.yml", "---\nname: probe\non:\n  push:\njobs: {}\n")
    if quiet:
        failures.append(f"a workflow with no zig pins was reported: {[str(f) for f in quiet]}")
    print(f"  [{'ok' if not quiet else 'FAIL'}] a workflow that pins no Zig is not a finding")
    return failures


def _selftest_naming() -> list[str]:
    """The archive-name convention matches what ziglang.org actually publishes."""
    failures: list[str] = []
    expectations = [
        ("0.14.1", "aarch64-macos", "zig-aarch64-macos-0.14.1.tar.xz"),
        ("0.15.1", "aarch64-macos", "zig-aarch64-macos-0.15.1.tar.xz"),
        ("0.14.0", "aarch64-macos", "zig-macos-aarch64-0.14.0.tar.xz"),
        ("0.13.0", "x86_64-linux", "zig-linux-x86_64-0.13.0.tar.xz"),
    ]
    for version, target, want in expectations:
        got = expected_archive(version, target)
        ok = got == want
        if not ok:
            failures.append(f"expected_archive({version}, {target}) = {got!r}, want {want!r}")
        print(f"  [{'ok' if ok else 'FAIL'}] zig {version} {target} -> {want}")

    if expected_archive("not-a-version", "aarch64-macos") is not None:
        failures.append("an unparseable version produced an archive name")
    print("  [ok] an unparseable release yields no archive name")

    if env_target("ZIG_SHA256_AARCH64_MACOS") != "aarch64-macos":
        failures.append("ZIG_SHA256_AARCH64_MACOS did not map to aarch64-macos")
    if env_target("ZIG_VERSION") is not None:
        failures.append("ZIG_VERSION was read as a digest pin")
    print("  [ok] the digest pin name maps to a target and the version pin does not")
    return failures


def _selftest_sources() -> list[str]:
    """The live tree really is the input, and the live pin really is recorded."""
    failures: list[str] = []
    if not DOCKERFILE.is_file():
        failures.append(f"{DOCKERFILE} is missing")
        return failures

    owned = dockerfile_pins(DOCKERFILE.read_text(encoding="utf-8")).get("ZIG_VERSION")
    if not owned:
        failures.append(f"{DOCKERFILE} ARG ZIG_VERSION was not read")
    elif owned not in RECORDED_DISTS:
        failures.append(f"the live pin {owned!r} has no recorded row")
    print(f"  [{'ok' if owned in RECORDED_DISTS else 'FAIL'}] the live pin {owned!r} is recorded")

    # Rule 3 has to refuse a release nobody recorded, or a bump sails through.
    bumped = audit_owner({"ZIG_VERSION": "9.9.9"})
    if _rules(bumped) != {RULE_OWNER_RECORDED}:
        failures.append(f"an unrecorded Dockerfile pin was not refused: {[str(f) for f in bumped]}")
    refused = _rules(bumped) == {RULE_OWNER_RECORDED}
    print(f"  [{'ok' if refused else 'FAIL'}] an unrecorded Dockerfile pin is refused")

    names = {p.name for p in workflow_files()}
    if "macos-host.yml" not in names:
        failures.append(f"the workflow scan did not find macos-host.yml (found {sorted(names)})")
    print(
        f"  [{'ok' if 'macos-host.yml' in names else 'FAIL'}] "
        f"the workflow scan finds macos-host.yml ({len(names)} workflow(s))"
    )

    # The rule is only worth having if the real workflow really does carry the
    # digest pin and the real download URL this check reads.
    macos = Path(".github/workflows/macos-host.yml")
    if macos.is_file():
        text = macos.read_text(encoding="utf-8")
        env = workflow_env_pins(text)
        if env_target(next((n for n in env if env_target(n)), "")) is None:
            failures.append("macos-host.yml carries no ZIG_SHA256_* pin")
        urls = zig_download_urls(text, env)
        if not urls:
            failures.append("no ziglang.org download URL was found in macos-host.yml")
        fetched = urls[0][2] if urls else "-"
        print(f"  [{'ok' if urls else 'FAIL'}] macos-host.yml really fetches {fetched}")
    return failures


def _selftest() -> int:
    """Prove every rule fires, the compliant form stays quiet, the tree is clean."""
    failures = _selftest_workflows()
    failures += _selftest_naming()
    table = audit_records()
    if table:
        failures.append(f"the recorded table is not clean: {[str(f) for f in table]}")
    print(f"  [{'ok' if not table else 'FAIL'}] the recorded table passes its own shape rules")
    failures += _selftest_sources()

    live = scan_tree()
    if live:
        failures.append(f"live: {len(live)} finding(s) on the current tree")
    print(f"  [{'ok' if not live else 'FAIL'}] the live tree is clean")

    for line in failures:
        sys.stderr.write(f"check_zig_dist_pins.py: SELFTEST FAIL -- {line}\n")
    if failures:
        return 1
    sys.stdout.write(
        "check_zig_dist_pins.py: selftest OK (5 rules fire, the compliant form stays "
        f"quiet, {len(RECORDED_DISTS)} release(s) recorded)\n"
    )
    return 0


def _roster() -> int:
    """Print what is recorded and what each workflow would fetch."""
    for version, targets in sorted(RECORDED_DISTS.items()):
        for target, (archive, digest) in sorted(targets.items()):
            sys.stdout.write(f"{version} {target}: {archive} sha256={digest}\n")
    for path in workflow_files():
        text = path.read_text(encoding="utf-8")
        env = workflow_env_pins(text)
        for number, version, archive in zig_download_urls(text, env):
            sys.stdout.write(f"{path}:{number} fetches zig {version} as {archive}\n")
    return 0


def main() -> int:
    """Entry point: ``--selftest`` proves non-vacuity, otherwise gate the tree."""
    args = sys.argv[1:]
    if "--selftest" in args:
        return _selftest()
    if "--roster" in args:
        return _roster()
    findings = scan_tree()
    if findings:
        sys.stderr.write("check_zig_dist_pins.py: Zig distribution pin problem(s):\n\n")
        for finding in findings:
            sys.stderr.write(f"  {finding}\n\n")
        sys.stderr.write(
            f"{len(findings)} finding(s). The macos-14 runner provisions its own Zig\n"
            "(#899), so the archive name and its sha256 are recorded per release in\n"
            "scripts/checks/check_zig_dist_pins.py. Add the row from ziglang.org's\n"
            "download index in the same commit as the bump; otherwise the first thing\n"
            "that notices is `shasum -c` on a nightly only a Mac can run.\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
