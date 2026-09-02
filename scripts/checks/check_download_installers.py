#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Require download-to-file and SHA-256 verification for bootstrap installers."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from download_installers_macos import check_macos_cleanup, macos_cleanup_mutations

ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- Git from PATH is intended
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
)

DOCKERFILE = ".devcontainer/Dockerfile"
PROVISION = "scripts/dev/provision_dev_box_toolchain.sh"
MAC_SETUP = "scripts/emu/setup_macos.sh"
SHA_ARGS = (
    "SHELLCHECK_SHA256_X86_64",
    "SHELLCHECK_SHA256_AARCH64",
    "SHFMT_SHA256_AMD64",
    "SHFMT_SHA256_ARM64",
    "ACTIONLINT_SHA256_AMD64",
    "ACTIONLINT_SHA256_ARM64",
    "JUST_SHA256_X86_64",
    "JUST_SHA256_AARCH64",
    "HADOLINT_SHA256_X86_64",
    "HADOLINT_SHA256_ARM64",
)
ARM_SHA_ARGS = (
    "ARM_GCC_SHA256_X86_64",
    "ARM_GCC_SHA256_AARCH64",
    "ARM_GCC_SHA256_DARWIN_X86_64",
    "ARM_GCC_SHA256_DARWIN_ARM64",
)
ARM_RELEASE_RE = re.compile(r"^[0-9]+\.[0-9]+\.rel[1-9][0-9]*$")
MAC_DOCKER_ARG_READER = r"""{
  local name="$1" value
  if ! value="$(awk -v name="${name}" '
    function inspect_instruction(    body, line, lower, target) {
      line = logical
      sub(/^[[:blank:]]*/, "", line)
      lower = tolower(line)
      if (substr(lower, 1, 3) != "arg" ||
          substr(line, 4, 1) !~ /[[:blank:]]/) {
        return
      }
      body = substr(line, 5)
      sub(/^[[:blank:]]+/, "", body)
      target = "(^|[[:blank:]])" name "([=[:blank:]]|$)"
      if (body !~ target) {
        return
      }
      count++
      if (continued || index(body, name "=") != 1) {
        invalid = 1
        return
      }
      value = substr(body, length(name) + 2)
    }
    {
      physical = $0
      if (physical ~ /^[[:blank:]]*(#.*)?$/) {
        next
      }
      has_continuation = physical ~ /\\[[:blank:]]*$/
      if (has_continuation) {
        sub(/\\[[:blank:]]*$/, "", physical)
        logical = logical physical
        continued = 1
        next
      }
      logical = logical physical
      inspect_instruction()
      logical = ""
      continued = 0
    }
    END {
      if (logical != "") {
        invalid = 1
        inspect_instruction()
      }
      if (count != 1 || invalid) exit 2
      printf "%s", value
    }
  ' "${dockerfile}")"; then
    echo "ERROR: expected one canonical ARG ${name}=... in ${dockerfile}." >&2
    return 1
  fi
  printf '%s' "${value}"
}"""
TLS_FLAGS = "--proto '=https' --proto-redir '=https' --tlsv1.2"
DOCKER_DOWNLOAD_MARKER = {
    "shellcheck": f"curl {TLS_FLAGS} -fsSL -o /tmp/shellcheck.tar.xz",
    "shfmt": f"curl {TLS_FLAGS} -fsSL -o /tmp/shfmt",
    "actionlint": f"curl {TLS_FLAGS} -fsSL -o /tmp/actionlint.tar.gz",
    "just": f"curl {TLS_FLAGS} -fsSL -o /tmp/just.tar.gz",
    "hadolint": f"curl {TLS_FLAGS} -fsSL -o /tmp/hadolint",
}
TOOLS = ("shellcheck", "shfmt", "actionlint", "just", "hadolint")
ARCHIVE_TOOLS = ("shellcheck", "actionlint", "just")
DOCKER_VERIFY_MARKER = {
    "shellcheck": '"${scsha}" /tmp/shellcheck.tar.xz | sha256sum -c -',
    "shfmt": '"${shsha}" /tmp/shfmt | sha256sum -c -',
    "actionlint": '"${alsha}" /tmp/actionlint.tar.gz | sha256sum -c -',
    "just": '"${justsha}" /tmp/just.tar.gz | sha256sum -c -',
    "hadolint": '"${hsha}" /tmp/hadolint | sha256sum -c -',
}
DOCKER_ARCHIVE_MEMBER = {
    "shellcheck": '"shellcheck-v${SHELLCHECK_VERSION}/shellcheck"',
    "actionlint": "-C /tmp/actionlint actionlint",
    "just": "-C /tmp/just just",
}
PROVISION_ARCHIVE_MEMBER = {
    "shellcheck": '"shellcheck-v${version}/shellcheck"',
    "actionlint": '-C "${tmp}/extract" actionlint',
    "just": '-C "${tmp}/extract" just',
}
DOCKER_END = {
    "shellcheck": "# cmake-format",
    "shfmt": "# cmake-format",
    "actionlint": "# just",
    "just": "# hadolint",
    "hadolint": "# Create a non-root user",
}
PROVISION_END = {
    "shellcheck": "install_shfmt()",
    "shfmt": "install_actionlint()",
    "actionlint": "install_hadolint()",
    "hadolint": "install_just()",
    "just": "install_doxygen()",
}


def _active(text: str) -> str:
    """Discard comment-only lines and join shell continuations."""
    code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    return re.sub(r"\\\n\s*", " ", code)


def _normalise_shell(text: str) -> str:
    """Collapse insignificant shell whitespace for exact statement checks."""
    return " ".join(_active(text).split())


def _exact_hex_assignment(text: str, name: str, length: int) -> str | None:
    """Return one indented literal hex assignment and reject every duplicate."""
    pattern = rf'(?m)^[ \t]*{re.escape(name)}="([0-9a-f]{{{length}}})"[ \t]*$'
    matches = re.findall(pattern, text)
    assignments = re.findall(rf"(?m)^[ \t]*{re.escape(name)}[+?]?=", text)
    return matches[0] if len(matches) == 1 and len(assignments) == 1 else None


def _unsafe_pipeline(text: str) -> bool:
    """Return whether downloaded bytes flow directly to a parser or shell."""
    active = _active(text)
    return bool(
        re.search(r"\bcurl\b[^\n;]*\|\s*(?:bash|sh|tar)\b", active)
        or re.search(r"\$\(\s*curl\b", active)
    )


def _section(text: str, start: str, end: str) -> str:
    """Return a required text section, or an empty string when anchors drift."""
    before, marker, rest = text.partition(start)
    del before
    if not marker:
        return ""
    body, marker, _after = rest.partition(end)
    return body if marker else ""


def _docker_instructions(text: str) -> tuple[tuple[str, bool], ...]:
    """Return Docker logical instructions and whether they used continuation."""
    instructions: list[tuple[str, bool]] = []
    logical = ""
    continued = False
    for physical in text.splitlines():
        if not physical.strip() or physical.lstrip().startswith("#"):
            continue
        match = re.search(r"\\[ \t]*$", physical)
        if match is not None:
            logical += physical[: match.start()]
            continued = True
            continue
        logical += physical
        instructions.append((logical, continued))
        logical = ""
        continued = False
    if logical:
        instructions.append((logical, True))
    return tuple(instructions)


def _docker_args(text: str, name: str) -> tuple[str | None, ...]:
    """Return every Docker ARG occurrence, marking non-canonical forms invalid."""
    values: list[str | None] = []
    target = re.compile(rf"(?:^|[ \t]){re.escape(name)}(?=$|[= \t])")
    instruction = re.compile(r"^[ \t]*(?i:ARG)[ \t]+(.*)$")
    for logical, continued in _docker_instructions(text):
        match = instruction.fullmatch(logical)
        if match is None:
            continue
        body = match.group(1).lstrip()
        if target.search(body) is None:
            continue
        prefix = f"{name}="
        if continued or not body.startswith(prefix):
            values.append(None)
            continue
        values.append(body[len(prefix) :])
    return tuple(values)


def _docker_arg(text: str, name: str) -> str | None:
    """Return a canonical Docker ARG only when it occurs exactly once."""
    values = _docker_args(text, name)
    return values[0] if len(values) == 1 and values[0] is not None else None


def _check_arm_docker_pins(text: str) -> list[str]:
    """Require exact-one, well-formed Arm release and digest ARGs."""
    findings: list[str] = []
    release_values = _docker_args(text, "ARM_GCC_RELEASE")
    if len(release_values) != 1 or release_values[0] is None:
        findings.append(f"{DOCKERFILE}: expected one canonical ARG ARM_GCC_RELEASE")
    elif ARM_RELEASE_RE.fullmatch(release_values[0]) is None:
        findings.append(f"{DOCKERFILE}: ARM_GCC_RELEASE must match N.N.relN")
    for name in ARM_SHA_ARGS:
        values = _docker_args(text, name)
        if len(values) != 1 or values[0] is None:
            findings.append(f"{DOCKERFILE}: expected one canonical ARG {name}")
        elif re.fullmatch(r"[0-9a-f]{64}", values[0]) is None:
            findings.append(f"{DOCKERFILE}: ARG {name} is not a 64-hex sha256")
    return findings


def check_dockerfile(text: str) -> list[str]:
    """Check the canonical release pins and Docker install blocks."""
    findings: list[str] = []
    findings.extend(
        f"{DOCKERFILE}: missing 64-hex ARG {name}"
        for name in SHA_ARGS
        if not re.search(rf"^ARG {name}=[0-9a-f]{{64}}$", text, re.MULTILINE)
    )
    findings.extend(_check_arm_docker_pins(text))
    if _unsafe_pipeline(text):
        findings.append(f"{DOCKERFILE}: curl output is piped directly to a shell/archive parser")
    if "just.systems/install.sh" in text:
        findings.append(f"{DOCKERFILE}: mutable Just installer script is forbidden")
    for tool in TOOLS:
        start = "ARG SHELLCHECK_VERSION=" if tool == "shfmt" else f"ARG {tool.upper()}_VERSION="
        block = _section(text, start, DOCKER_END[tool])
        active = _active(block)
        if (
            not block
            or DOCKER_VERIFY_MARKER[tool] not in active
            or DOCKER_DOWNLOAD_MARKER[tool] not in active
        ):
            findings.append(f"{DOCKERFILE}: {tool} must download to /tmp and verify sha256")
        if tool in ARCHIVE_TOOLS and DOCKER_ARCHIVE_MEMBER[tool] not in active:
            findings.append(f"{DOCKERFILE}: {tool} archive extraction is not file-based/exact")
    if "hadolint-Linux-" in text:
        findings.append(f"{DOCKERFILE}: hadolint asset name has stale uppercase Linux spelling")
    return findings


def check_provision(text: str) -> list[str]:
    """Check native dev-box release installers use the verified helper."""
    findings: list[str] = []
    if _unsafe_pipeline(text):
        findings.append(f"{PROVISION}: curl output is piped directly to a shell/archive parser")
    helper = _section(text, "download_verified()", "install_shellcheck()")
    if (
        TLS_FLAGS not in _active(helper)
        or "-fsSL" not in helper
        or '-o "${output}"' not in helper
        or "sha256sum -c -" not in helper
    ):
        findings.append(f"{PROVISION}: download_verified must fetch to disk and check sha256")
    for tool in TOOLS:
        block = _section(text, f"install_{tool}()", PROVISION_END[tool])
        active = _active(block)
        if "download_verified" not in block:
            findings.append(f"{PROVISION}: install_{tool} bypasses download_verified")
        if tool in ARCHIVE_TOOLS and PROVISION_ARCHIVE_MEMBER[tool] not in active:
            findings.append(f"{PROVISION}: install_{tool} lacks file-based exact extraction")
    findings.extend(
        f"{PROVISION}: canonical Docker ARG {name} is not consumed"
        for name in SHA_ARGS
        if name not in text
    )
    if "hadolint-Linux-" in text or re.search(r"as_root\s+curl", text):
        findings.append(f"{PROVISION}: stale asset spelling or privileged direct download")
    return findings


def _check_macos_arm_mapping(text: str) -> list[str]:
    """Require immutable final host-to-Darwin asset selection."""
    findings: list[str] = []
    actual = _section(text, "arm_toolchain_asset()", "install_homebrew()")
    expected = r"""{
      case "$1" in
        arm64)
          printf '%s\t%s\n' "darwin-arm64" "ARM_GCC_SHA256_DARWIN_ARM64"
          ;;
        x86_64)
          printf '%s\t%s\n' "darwin-x86_64" "ARM_GCC_SHA256_DARWIN_X86_64"
          ;;
        *)
          echo "ERROR: unsupported macOS architecture $1 for Arm GNU Toolchain." >&2
          return 1
          ;;
      esac
    }"""
    if _normalise_shell(actual) != _normalise_shell(expected):
        findings.append(f"{MAC_SETUP}: Arm asset selector function is not exact")
    expected_counts = {
        "arm64)": 1,
        "x86_64)": 1,
        "darwin-arm64": 1,
        "darwin-x86_64": 1,
        "ARM_GCC_SHA256_X86_64": 1,
        "ARM_GCC_SHA256_AARCH64": 1,
        "ARM_GCC_SHA256_DARWIN_X86_64": 2,
        "ARM_GCC_SHA256_DARWIN_ARM64": 2,
    }
    if any(text.count(token) != count for token, count in expected_counts.items()):
        findings.append(f"{MAC_SETUP}: Arm case labels, assets, and hash tokens must be unique")
    assignment_counts = {
        name: len(re.findall(rf"(?m)^\s*{name}=", text))
        for name in ("arm_asset_arch", "arm_sha_arg", "arm_sha256")
    }
    if assignment_counts != {"arm_asset_arch": 0, "arm_sha_arg": 0, "arm_sha256": 1}:
        findings.append(f"{MAC_SETUP}: Arm asset/hash variables have an override assignment")
    order_tokens = (
        "read -r arm_asset_arch arm_sha_arg",
        'arm_sha256="$(dockerfile_arg "${arm_sha_arg}")"',
        "readonly arm_asset_arch arm_sha_arg arm_sha256",
        'arm_url="',
    )
    offsets = tuple(text.find(token) for token in order_tokens)
    if any(offset < 0 for offset in offsets) or offsets != tuple(sorted(offsets)):
        findings.append(
            f"{MAC_SETUP}: Arm asset selection must be final and readonly before URL use"
        )
    return findings


def _check_macos_arm_delete(text: str) -> list[str]:
    """Require a strict release and canonical readonly Arm prefix."""
    findings: list[str] = []
    required = (
        'arm_release="$(dockerfile_arg ARM_GCC_RELEASE)" || exit 1\nreadonly arm_release',
        r"^[0-9]+\.[0-9]+\.rel[1-9][0-9]*$",
        'readonly arm_version="${arm_release%.rel*}"',
        'home_root="$(cd "$HOME" && pwd -P)" || exit 1\nreadonly home_root',
        'readonly arm_root="${home_root}/opt"',
        'arm_prefix="$(canonical_cleanup_target '
        '"${arm_root}/arm-gnu-toolchain-${arm_version}" "$arm_root")" || exit 1\n'
        "readonly arm_prefix",
        'if [[ -z "$arm_prefix" ]]',
    )
    active = _normalise_shell(text)
    if any(_normalise_shell(token) not in active for token in required):
        findings.append(f"{MAC_SETUP}: Arm release/prefix derivation is not strict and readonly")
    return [*findings, *check_macos_cleanup(text)]


def _check_macos_arm(text: str, docker: str) -> list[str]:
    """Check Darwin Arm selection, pin ownership, and deletion safety."""
    findings = [*_check_macos_arm_mapping(text), *_check_macos_arm_delete(text)]
    reader = _section(text, "dockerfile_arg()", "require_arm_hash_pins()")
    if _normalise_shell(reader) != _normalise_shell(MAC_DOCKER_ARG_READER):
        findings.append(f"{MAC_SETUP}: Docker ARG reader active body is not exact")
    pin_validator = _section(text, "require_arm_hash_pins()", 'arm_release="')
    if (
        any(name not in pin_validator for name in ARM_SHA_ARGS)
        or len(re.findall(r"(?m)^\s*require_arm_hash_pins\s*$", text)) != 1
    ):
        findings.append(f"{MAC_SETUP}: bootstrap must validate all four Arm hash pins")
    expected_url = "arm-gnu-toolchain-${arm_release}-${arm_asset_arch}-arm-none-eabi.tar.xz"
    if expected_url not in text:
        findings.append(f"{MAC_SETUP}: Arm URL must include the Darwin asset selector")
    if TLS_FLAGS not in _active(_section(text, 'arm_url="', 'echo "[emu-setup] using')):
        findings.append(f"{MAC_SETUP}: Arm archive download must enforce HTTPS/TLS")
    for linux_name, darwin_name in (
        ("ARM_GCC_SHA256_AARCH64", "ARM_GCC_SHA256_DARWIN_ARM64"),
        ("ARM_GCC_SHA256_X86_64", "ARM_GCC_SHA256_DARWIN_X86_64"),
    ):
        linux_sha = _docker_arg(docker, linux_name)
        darwin_sha = _docker_arg(docker, darwin_name)
        if darwin_sha is not None and darwin_sha == linux_sha:
            findings.append(f"{DOCKERFILE}: {darwin_name} must not reuse the Linux archive hash")
    return findings


def check_macos(text: str, docker: str) -> list[str]:
    """Check macOS bootstrap downloads and Darwin Arm archive selection."""
    findings: list[str] = []
    commit = _exact_hex_assignment(text, "homebrew_installer_commit", 40)
    digest = _exact_hex_assignment(text, "homebrew_installer_sha256", 64)
    if commit is None or digest is None:
        findings.append(f"{MAC_SETUP}: Homebrew installer commit/sha256 pin is missing")
    required = (
        "Homebrew/install/${homebrew_installer_commit}/install.sh",
        TLS_FLAGS,
        '-o "${installer}"',
        "shasum -a 256 -c -",
        '/bin/bash -p "${installer}"',
    )
    if any(token not in text for token in required):
        findings.append(f"{MAC_SETUP}: Homebrew installer is not download-verify-execute")
    if "/HEAD/install.sh" in text or _unsafe_pipeline(text):
        findings.append(f"{MAC_SETUP}: mutable or in-memory Homebrew execution is forbidden")
    return [*findings, *_check_macos_arm(text, docker)]


def live_findings() -> list[str]:
    """Return all findings across the three installer ownership surfaces."""
    return [
        *check_dockerfile((ROOT / DOCKERFILE).read_text(encoding="utf-8")),
        *check_provision((ROOT / PROVISION).read_text(encoding="utf-8")),
        *check_macos(
            (ROOT / MAC_SETUP).read_text(encoding="utf-8"),
            (ROOT / DOCKERFILE).read_text(encoding="utf-8"),
        ),
    ]


def _arm_pin_mutations(docker: str, name: str) -> tuple[tuple[str, str], ...]:
    """Return alternate Docker ARG spellings that can override an Arm pin."""
    alternate = "99.9.rel1" if name == "ARM_GCC_RELEASE" else "0" * 64
    return (
        (docker + f"\nARG {name}={alternate}\n", "duplicate"),
        (docker + f"\nARG {name}\\\n={alternate}\n", "continued"),
        (docker + f"\nARG UNUSED=ok {name}={alternate}\n", "multiple-name"),
        (docker + f"\nARG {name} {alternate}\n", "legacy"),
        (docker + f"\nARG {name}\\\n# ignored\n={alternate}\n", "comment continuation"),
        (
            docker + f"\nARG {name}\\\n  # indented ignored\n={alternate}\n",
            "indented-comment continuation",
        ),
        (docker + f"\nARG {name}\\\n\n={alternate}\n", "empty-line continuation"),
    )


def _arm_pin_noninstructions(docker: str, name: str) -> tuple[tuple[str, str], ...]:
    """Return continued non-ARG instructions that contain inert ARG-shaped text."""
    value = _docker_arg(docker, name)
    if value is None:
        return ()
    alternate = "99.9.rel1" if name == "ARM_GCC_RELEASE" else "0" * 64
    canonical = f"ARG {name}={value}\n"
    decoys = (
        (f"RUN printf decoy \\\n# ignored\nARG {name}={alternate}\n", "comment"),
        (
            f"RUN printf decoy \\\n  # indented ignored\nARG {name}={alternate}\n",
            "indented comment",
        ),
        (f"RUN printf decoy \\\n\nARG {name}={alternate}\n", "empty line"),
    )
    return tuple(
        (docker.replace(canonical, decoy + canonical, 1), label) for decoy, label in decoys
    )


def _docker_arm_selftest(docker: str) -> list[str]:
    """Return failures from malformed/missing/duplicate Arm pin cases."""
    failures: list[str] = []
    release_line = "ARG ARM_GCC_RELEASE=13.3.rel1"
    mutations = (
        (
            docker.replace(release_line, "ARG ARM_GCC_RELEASE=../../tmp", 1),
            "hostile Arm release",
        ),
        (docker + "\n  ARG ARM_GCC_RELEASE=13.3.rel1\n", "indented duplicate release"),
        (docker.replace(f"{release_line}\n", "", 1), "missing Arm release"),
    )
    for mutated, label in mutations:
        if not check_dockerfile(mutated):
            failures.append(f"Dockerfile {label} must fire")
    for name in ("ARM_GCC_RELEASE", *ARM_SHA_ARGS):
        value = _docker_arg(docker, name)
        if value is None:
            failures.append(f"canonical {name} must exist for mutation")
            continue
        for mutated, label in _arm_pin_mutations(docker, name):
            if not check_dockerfile(mutated):
                failures.append(f"Dockerfile {label} {name} must fire")
        for mutated, label in _arm_pin_noninstructions(docker, name):
            if check_dockerfile(mutated):
                failures.append(f"Dockerfile continued RUN {label} {name} must stay quiet")
    first_hash = ARM_SHA_ARGS[0]
    first_value = _docker_arg(docker, first_hash)
    if first_value and not check_dockerfile(
        docker.replace(f"ARG {first_hash}={first_value}\n", "", 1)
    ):
        failures.append(f"Dockerfile missing {first_hash} must fire")
    return failures


def _macos_arm_selftest(macos: str, docker: str) -> list[str]:
    """Return failures from Darwin-vs-Linux archive mutation cases."""
    failures: list[str] = []
    mutations = (
        (macos.replace("darwin-arm64", "aarch64", 1), "arm64 Linux archive"),
        (
            macos.replace("ARM_GCC_SHA256_DARWIN_ARM64", "ARM_GCC_SHA256_AARCH64", 1),
            "arm64 Linux hash",
        ),
        (macos.replace("darwin-x86_64", "x86_64", 1), "x86_64 Linux archive"),
        (
            macos.replace("ARM_GCC_SHA256_DARWIN_X86_64", "ARM_GCC_SHA256_X86_64", 1),
            "x86_64 Linux hash",
        ),
    )
    for mutated, label in mutations:
        if not check_macos(mutated, docker):
            failures.append(f"macOS {label} selection must fire")
    pairs = (
        ("ARM_GCC_SHA256_AARCH64", "ARM_GCC_SHA256_DARWIN_ARM64", "arm64"),
        ("ARM_GCC_SHA256_X86_64", "ARM_GCC_SHA256_DARWIN_X86_64", "x86_64"),
    )
    for linux_name, darwin_name, label in pairs:
        linux_sha = _docker_arg(docker, linux_name)
        darwin_sha = _docker_arg(docker, darwin_name)
        if not linux_sha or not darwin_sha:
            failures.append(f"canonical {label} hashes must exist for mutation")
            continue
        if not check_macos(macos, docker.replace(darwin_sha, linux_sha, 1)):
            failures.append(f"Darwin {label} duplicate Linux hash pin must fire")
    branch = 'printf \'%s\\t%s\\n\' "darwin-arm64" "ARM_GCC_SHA256_DARWIN_ARM64"'
    branch_override = f'{branch}\n    arm_asset_arch="aarch64"'
    if not check_macos(macos.replace(branch, branch_override, 1), docker):
        failures.append("appended arm64 branch override must fire")
    readonly_line = "  readonly arm_asset_arch arm_sha_arg arm_sha256"
    final_override = f'{readonly_line}\n  arm_sha256="{_docker_arg(docker, ARM_SHA_ARGS[0])}"'
    if not check_macos(macos.replace(readonly_line, final_override, 1), docker):
        failures.append("appended final hash override must fire")
    dead_case = """  case "$1" in
    dead)
      printf '%s\\t%s\\n' "darwin-arm64" "ARM_GCC_SHA256_DARWIN_ARM64"
      ;;
"""
    if not check_macos(macos.replace('  case "$1" in\n', dead_case, 1), docker):
        failures.append("prepended dead asset case must fire")
    return failures


def _macos_parser_safety_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return hostile parser, hash validation, release, and prefix mutations."""
    return (
        (macos.replace("count != 1", "count < 1", 1), "duplicate-tolerant ARG parser"),
        (
            macos.replace(
                "ARM_GCC_SHA256_X86_64 ARM_GCC_SHA256_AARCH64", "ARM_GCC_SHA256_AARCH64", 1
            ),
            "incomplete hash-pin validation",
        ),
        (
            macos.replace(r"^[0-9]+\.[0-9]+\.rel[1-9][0-9]*$", r"^.*$", 1),
            "hostile release grammar",
        ),
        (
            macos.replace(
                'readonly arm_root="${home_root}/opt"',
                'readonly arm_root="${home_root}"',
                1,
            ),
            "prefix outside HOME/opt",
        ),
    )


def _macos_safety_selftest(macos: str, docker: str) -> list[str]:
    """Return failures from hostile parser, prefix, and deletion mutations."""
    mutations = (
        *_macos_parser_safety_mutations(macos),
        *macos_cleanup_mutations(macos),
    )
    failures: list[str] = []
    for mutated, label in mutations:
        if not check_macos(mutated, docker):
            failures.append(f"macOS {label} must fire")
    return failures


def _run_docker_reader(function: str, docker: str, name: str) -> tuple[bool, str]:
    """Execute only the extracted Docker ARG reader against an inert fixture."""
    with tempfile.TemporaryDirectory(prefix="ra8-arm-reader-") as raw:
        dockerfile = Path(raw) / "Dockerfile"
        dockerfile.write_text(docker, encoding="utf-8")
        script = f'set -u\ndockerfile="$1"\ndockerfile_arg(){function}\ndockerfile_arg "$2"\n'
        result = subprocess.run(  # noqa: S603 -- extracted audited reader, fixed arguments.
            [  # noqa: S607 -- fixed Bash from PATH runs an inert parser fixture.
                "bash",
                "-c",
                script,
                "reader-selftest",
                str(dockerfile),
                name,
            ],
            check=False,
            capture_output=True,
            text=True,
        )
    return result.returncode == 0, result.stdout


def _docker_reader_execution_selftest(macos: str, docker: str) -> list[str]:
    """Exercise canonical and hostile Docker forms through the macOS reader."""
    function = _section(macos, "dockerfile_arg()", "require_arm_hash_pins()")
    failures: list[str] = []
    for name in ("ARM_GCC_RELEASE", *ARM_SHA_ARGS):
        expected = _docker_arg(docker, name)
        if expected is None:
            failures.append(f"canonical {name} must exist for reader execution")
            continue
        accepted, value = _run_docker_reader(function, docker, name)
        if not accepted or value != expected:
            failures.append(f"macOS reader canonical {name} must stay quiet")
        for mutated, label in _arm_pin_mutations(docker, name):
            accepted, _value = _run_docker_reader(function, mutated, name)
            if accepted:
                failures.append(f"macOS reader {label} {name} must fire")
        for mutated, label in _arm_pin_noninstructions(docker, name):
            accepted, value = _run_docker_reader(function, mutated, name)
            if not accepted or value != expected:
                failures.append(f"macOS reader continued RUN {label} {name} must stay quiet")
    return failures


def _run_cleanup_validator(function: str, target: str, root: str, cwd: Path) -> tuple[bool, str]:
    """Run only the canonicalizer; never invoke the recursive removal helper."""
    script = f'set -u\ncanonical_cleanup_target(){function}\ncanonical_cleanup_target "$1" "$2"\n'
    result = subprocess.run(  # noqa: S603 -- extracted audited validator, fixed arguments.
        [  # noqa: S607 -- fixed Bash from PATH runs an inert path fixture.
            "bash",
            "-c",
            script,
            "cleanup-selftest",
            target,
            root,
        ],
        check=False,
        cwd=cwd,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0, result.stdout.strip()


def _cleanup_execution_selftest(macos: str) -> list[str]:
    """Exercise physical root/target boundaries without deleting anything."""
    function = _section(macos, "canonical_cleanup_target()", "safe_remove_tree()")
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-cleanup-guard-") as raw:
        base = Path(raw)
        safe_root = base / "safe-root"
        safe_root.mkdir()
        safe_target = safe_root / "safe-target"
        safe_target.mkdir()
        missing_target = safe_root / "missing-target"
        missing_root = base / "missing-root"
        link_root = base / "link-root"
        link_root.symlink_to(safe_root, target_is_directory=True)
        link_target = safe_root / "link-target"
        link_target.symlink_to(base, target_is_directory=True)
        file_target = safe_root / "file-target"
        file_target.write_text("fixture", encoding="ascii")
        cases = (
            (safe_target, safe_root, True, "existing directory"),
            (missing_target, safe_root, True, "missing direct child"),
            (missing_root / "child", missing_root, True, "missing root under physical parent"),
            (Path("/child"), Path("/"), False, "root allowed boundary"),
            (Path("relative"), safe_root, False, "relative target"),
            (safe_root, safe_root, False, "target equals allowed root"),
            (link_root / "child", link_root, False, "symlinked allowed root"),
            (link_target, safe_root, False, "symlinked target"),
            (file_target, safe_root, False, "non-directory target"),
        )
        for target, root, expected, label in cases:
            accepted, canonical = _run_cleanup_validator(function, str(target), str(root), base)
            if accepted != expected or (accepted and canonical != str(target)):
                failures.append(f"executable cleanup validator case failed: {label}")
    return failures


def _macos_privileged_exec_selftest(macos: str, docker: str) -> list[str]:
    """Require privileged Bash for the downloaded installer."""
    mutations = (
        (
            macos.replace('/bin/bash -p "${installer}"', '/bin/bash "${installer}"', 1),
            "non-privileged Homebrew installer shell",
        ),
    )
    return [label for mutated, label in mutations if not check_macos(mutated, docker)]


def _basic_selftest_failures(docker: str, provision: str, macos: str) -> list[str]:
    """Exercise canonical installer inputs and direct unsafe mutations."""
    duplicate_pin = (
        macos + '\nhomebrew_installer_commit="0000000000000000000000000000000000000000"\n'
    )
    cases = (
        (bool(check_dockerfile(docker)), "canonical Dockerfile must stay quiet"),
        (bool(check_provision(provision)), "canonical native provisioner must stay quiet"),
        (bool(check_macos(macos, docker)), "canonical macOS installer must stay quiet"),
        (
            not check_dockerfile(docker.replace(DOCKER_VERIFY_MARKER["just"], "sha256sum", 1)),
            "Dockerfile missing verification must fire",
        ),
        (
            not check_dockerfile(docker.replace("--proto-redir '=https'", "", 1)),
            "Dockerfile redirect downgrade must fire",
        ),
        (
            not check_dockerfile(docker.replace(DOCKER_ARCHIVE_MEMBER["actionlint"], ".", 1)),
            "Dockerfile unbounded archive extraction must fire",
        ),
        (
            not check_dockerfile(docker + "\nRUN curl https://example.invalid/x | bash\n"),
            "Dockerfile curl-to-shell must fire",
        ),
        (
            not check_provision(provision.replace("download_verified", "download_unchecked", 1)),
            "native provisioner helper bypass must fire",
        ),
        (
            not check_provision(provision.replace(PROVISION_ARCHIVE_MEMBER["just"], ".", 1)),
            "native provisioner unbounded archive extraction must fire",
        ),
        (
            not check_macos(macos.replace("${homebrew_installer_commit}", "HEAD", 1), docker),
            "mutable Homebrew installer ref must fire",
        ),
        (not check_macos(duplicate_pin, docker), "duplicate Homebrew installer commit must fire"),
    )
    return [label for failed, label in cases if failed]


def selftest() -> int:
    """Prove clean live inputs stay quiet and each unsafe direction fires."""
    docker = (ROOT / DOCKERFILE).read_text(encoding="utf-8")
    provision = (ROOT / PROVISION).read_text(encoding="utf-8")
    macos = (ROOT / MAC_SETUP).read_text(encoding="utf-8")
    failures = _basic_selftest_failures(docker, provision, macos)
    failures.extend(
        f"{label} must fire" for label in _macos_privileged_exec_selftest(macos, docker)
    )
    failures.extend(_docker_arm_selftest(docker))
    failures.extend(_macos_arm_selftest(macos, docker))
    failures.extend(_macos_safety_selftest(macos, docker))
    failures.extend(_docker_reader_execution_selftest(macos, docker))
    failures.extend(_cleanup_execution_selftest(macos))
    if failures:
        print("check_download_installers.py --selftest: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("check_download_installers.py --selftest: PASS (222 both-direction cases)")
    return 0


def main() -> int:
    """Run selftests or the live installer policy audit."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    findings = live_findings()
    if findings:
        print("\n".join(findings), file=sys.stderr)
        return 1
    print("check_download_installers.py: pinned download/verify/install flows are intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
