#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Lint and format-check the devcontainer: the Dockerfile and the zshrc.

WHY THESE TWO FILES, TOGETHER
=============================
`.devcontainer/Dockerfile` pins every tool version CI resolves -- clang-format
22, the ARM toolchain by sha256, ruff, shellcheck, shfmt, cmake-format,
yamllint, actionlint. A defect in it changes what every other gate in this
repository runs. It was, until #371, linted and formatted by nothing at all.
`.devcontainer/zshrc` is copied to ~/.zshrc at image build time and is the
other half of the same artefact. They are checked as one unit because they are
one deliverable: the container.

THE TOOLS, AND WHY EACH ONE
---------------------------
Dockerfile -> hadolint (pinned 2.14.0). The obvious and only serious candidate:
  a single static binary, the same provisioning shape actionlint and shfmt
  already use. It found a real defect on its first run -- DL4006, eight
  `curl -fsSL <url> | tar -x` pipelines running under `/bin/sh -c`, where the
  pipeline's status is that of `tar` alone. A failed download produced an empty
  tarball, tar succeeded on it, and the image was built with the pinned tool
  silently absent. That is precisely the class check_errexit_masking.py exists
  to catch in shell, and it was live in the file that provisions CI. Fixed by
  the `SHELL ["/bin/bash", "-o", "pipefail", "-c"]` directive. Rule
  configuration and the one deliberate ignore live in .hadolint.yaml.

zshrc -> `zsh -n` (zsh 5.9, already in the image; no new provisioning).
  There is no zsh linter and no zsh formatter, and this was checked rather than
  assumed:

    * ShellCheck refuses zsh outright ("ShellCheck only supports sh/bash/dash/
      ksh"). Running it as `--shell=bash` is worse than not running it: this
      very file uses `${(%):-%n}` and `plugins=(...)`, zsh parameter-expansion
      flags and array syntax that bash cannot parse, so bash-mode ShellCheck
      reports errors that are not errors. A linter that is wrong about the
      file is not partial coverage, it is noise that trains people to ignore
      the gate. That is why lint_coverage_rules.py classifies `.zsh` as its
      own class rather than folding it into `shell`.
    * shfmt likewise handles sh/bash/mksh only.
    * `zsh -n` parses the file with the real zsh grammar and reports syntax
      errors without executing anything. It is a genuine check, it is the only
      one that exists, and it is what runs here.

  Being honest about the limit: `zsh -n` catches syntax, not semantics. It
  would not notice a misspelled plugin name. That is the ceiling of what is
  available for the language, and it is still strictly more than the nothing
  this file had before.

FORMAT ROLE
-----------
Neither tool rewrites its input, so the formatter half is a canonical-form
checker -- the same arrangement lint_coverage_rules.py already documents for
justfiles and linker scripts, where nothing in the ecosystem rewrites the file
either. DC001 rejects the layout deviations that a formatter would otherwise
fix: non-ASCII bytes, CRLF, a missing final newline, tab indentation and
trailing whitespace.

Run with --selftest to prove both linters fire on deliberately broken input and
stay quiet on the real files.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_coverage_rules import EXT_CLASS, NAME_CLASS
from selftest_assert import expect, report

# Vendored SOUP is governed by its upstream boundary, never by this checker.
THIRD_PARTY_PREFIXES = ("libs/third_party/", "apps/shared_libs/third_party/")

REPO_ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- trusted: fixed git argv
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
)

# The classes this checker claims. Membership is decided by the SHARED tables in
# lint_coverage_rules.py, never by a private copy of them here: a second
# classification map is how the coverage question became unanswerable in the
# first place (#296, #332, #358, #359, #360).
OWNED_CLASSES = ("dockerfile", "zsh")

HADOLINT_VERSION = "2.14.0"

# Both files must be found. An empty scan means the enumeration broke, and a
# gate must never mistake that for a clean tree.
FILE_FLOOR = 2


class Finding:
    """One rule violation, reported as path:line: [CODE] message."""

    def __init__(self, rel: str, line: int, code: str, msg: str) -> None:
        """Record one finding; all four fields are required and none is derived."""
        self.rel, self.line, self.code, self.msg = rel, line, code, msg

    def __str__(self) -> str:
        """Render as ``rel:line: [CODE] message`` -- editor-jumpable."""
        return f"{self.rel}:{self.line}: [{self.code}] {self.msg}"


def classify(rel: str) -> str | None:
    """The shared class of `rel`, by exact name then extension."""
    name = rel.rsplit("/", 1)[-1]
    if name in NAME_CLASS:
        return NAME_CLASS[name]
    if "." in name[1:]:
        suffix = "." + name.rsplit(".", 1)[-1]
        return EXT_CLASS.get(suffix.lower())
    return None


def targets() -> list[str]:
    """Every first-party Dockerfile and zsh file, repo-relative and sorted."""
    proc = subprocess.run(
        ["git", "ls-files", "-z"],  # noqa: S607 -- git from PATH is intended
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    # The vendored powerlevel10k theme config is not a project script; the same
    # exemption lint_coverage_rules.py records for it.
    return sorted(
        rel
        for rel in proc.stdout.split("\0")
        if rel
        and not rel.startswith(THIRD_PARTY_PREFIXES)
        and rel != ".devcontainer/p10k.zsh"
        and classify(rel) in OWNED_CLASSES
    )


def check_format(rel: str, raw: bytes) -> list[Finding]:
    """DC001 -- the canonical-form rules, on raw bytes."""
    findings: list[Finding] = []

    def add(line: int, msg: str) -> None:
        """Append one DC001 finding, closing over this file's relative path."""
        findings.append(Finding(rel, line, "DC001", msg))

    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        add(raw[: exc.start].count(b"\n") + 1, f"non-ASCII byte 0x{raw[exc.start]:02x}")
        text = raw.decode("ascii", errors="replace")
    if b"\r\n" in raw:
        add(raw.split(b"\r\n")[0].count(b"\n") + 1, "CRLF line ending")
    if raw and not raw.endswith(b"\n"):
        add(text.count("\n") + 1, "no final newline")
    for num, line in enumerate(text.splitlines(), start=1):
        if line.startswith("\t"):
            add(num, "tab indentation (use spaces)")
        if line != line.rstrip():
            add(num, "trailing whitespace")
    return findings


def check_uv_execution(rel: str, text: str) -> list[Finding]:
    """DC004 -- execute uv only through the authenticated bootstrap runner."""
    boundary = "WORKDIR /opt/ra8-python-project\n"
    if boundary not in text:
        return []
    start = text.find(boundary) + len(boundary)
    end = text.find("\nENV RA8_TOOL_VENV", start)
    if text.count(boundary) != 1 or end < 0:
        return [Finding(rel, 1, "DC004", "uv provisioning block is not structurally bounded")]
    block = text[start:end]
    normalized = " ".join(block.replace("\\\n", " ").split())
    expected = " ".join(EXPECTED_UV_RUN_BLOCK.replace("\\\n", " ").split())
    raw_uv = re.compile(r"(?:^|[;&|]\s*)(?:[^\s;]+/)?uv(?:\s|$)")
    other_raw = any(
        raw_uv.search(" ".join(instruction.replace("\\\n", " ").split()))
        for instruction in re.findall(r"(?ms)^RUN .*?(?=\n(?:RUN|ENV|ARG|COPY|WORKDIR|#)|\Z)", text)
        if instruction != block
    )
    unsafe = normalized != expected or other_raw
    if not unsafe:
        return []
    line = text[:start].count("\n") + 1
    return [Finding(rel, line, "DC004", "uv execution bypasses authenticated bytes")]


def run_hadolint(rel: str, path: Path) -> list[Finding]:
    """DC002 -- hadolint over one Dockerfile. Config comes from .hadolint.yaml.

    hadolint prints ``<file>:<line> <CODE> <severity>: <message>``. That leading
    ``<file>:<line>`` is re-parsed rather than passed through, so a finding reads
    like every other checker here instead of carrying a second, absolute copy of
    the path inside the message.
    """
    proc = subprocess.run(  # noqa: S603 -- fixed argv
        ["hadolint", "--no-color", str(path)],  # noqa: S607 -- provisioned on PATH
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    findings: list[Finding] = []
    prefix = f"{path}:"
    for raw in (proc.stdout + proc.stderr).splitlines():
        line = raw.strip()
        if not line:
            continue
        num = 0
        if line.startswith(prefix):
            rest = line[len(prefix) :]
            head, _, tail = rest.partition(" ")
            if head.isdigit():
                num, line = int(head), tail
        findings.append(Finding(rel, num, "DC002", line))
    return findings


def run_zsh_syntax(rel: str, path: Path) -> list[Finding]:
    """DC003 -- `zsh -n` over one zsh file. Parses only; never executes."""
    proc = subprocess.run(  # noqa: S603 -- fixed argv
        ["zsh", "-n", str(path)],  # noqa: S607 -- zsh is in the image
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode == 0:
        return []
    detail = (proc.stderr + proc.stdout).strip() or f"zsh -n exited {proc.returncode}"
    # zsh reports `<file>:<line>: <message>`; drop its copy of the path for the
    # same reason as hadolint's above.
    num = 0
    prefix = f"{path}:"
    if detail.startswith(prefix):
        rest = detail[len(prefix) :]
        head, _, tail = rest.partition(":")
        if head.isdigit():
            num, detail = int(head), tail.strip()
    return [Finding(rel, num, "DC003", f"zsh syntax error: {detail}")]


def check_one(rel: str, root: Path = REPO_ROOT) -> list[Finding]:
    """Every finding for one devcontainer file."""
    path = root / rel
    findings = check_format(rel, path.read_bytes())
    cls = classify(rel)
    if cls == "dockerfile":
        findings.extend(check_uv_execution(rel, path.read_text(encoding="ascii")))
        findings.extend(run_hadolint(rel, path))
    elif cls == "zsh":
        findings.extend(run_zsh_syntax(rel, path))
    return findings


def require_tools() -> int:
    """Fail loudly when a linter is absent. A gate must never degrade to a no-op."""
    missing = []
    if shutil.which("hadolint") is None:
        missing.append(
            f"hadolint (pinned {HADOLINT_VERSION}) -- https://github.com/hadolint/hadolint/releases"
        )
    if shutil.which("zsh") is None:
        missing.append("zsh -- apt-get install zsh (already in the devcontainer image)")
    if missing:
        print("check_devcontainer.py: FATAL -- required tool(s) absent:", file=sys.stderr)
        for item in missing:
            print(f"  {item}", file=sys.stderr)
        print(
            "  This gate FAILS rather than skipping: a checker whose tool is\n"
            "  missing reports nothing, and nothing is indistinguishable from clean.",
            file=sys.stderr,
        )
        return 1
    return 0


GOOD_DOCKERFILE = b"""FROM ubuntu:24.04
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN echo hello
"""

# DL3020 (use COPY not ADD) and DL4006 (a pipe with no pipefail): two families,
# so a single over-broad ignore in .hadolint.yaml cannot silence the selftest.
BAD_DOCKERFILE = b"""FROM ubuntu:24.04
ADD ./x /x
RUN curl -fsSL http://example.com/a.tgz | tar -xz
"""

EXPECTED_UV_RUN_BLOCK = (
    "RUN set -eux; "
    'UV_PROJECT_ENVIRONMENT="${PYTHON_TOOL_VENV}" UV_PYTHON_DOWNLOADS=never '
    "/usr/bin/python3 -I -S /opt/ra8-uv-bootstrap/bootstrap_uv.py "
    "--cache-root /opt/ra8-uv-cache --ensure-and-run --no-config sync "
    "--locked --only-group ci --no-install-project --python /usr/bin/python3; "
    "UV_PYTHON_DOWNLOADS=never "
    "/usr/bin/python3 -I -S /opt/ra8-uv-bootstrap/bootstrap_uv.py "
    "--cache-root /opt/ra8-uv-cache --run --no-config lock --check; "
    "UV_PYTHON_DOWNLOADS=never "
    "/usr/bin/python3 -I -S /opt/ra8-uv-bootstrap/bootstrap_uv.py "
    "--cache-root /opt/ra8-uv-cache --run --no-config pip check "
    '--python "${PYTHON_TOOL_VENV}/bin/python3"; '
    '"${PYTHON_TOOL_VENV}/bin/python3" -c '
    '"import PIL, clang.cindex, dotenv, kasa, serial, usb.core, yaml"; '
    '"${PYTHON_TOOL_VENV}/bin/ruff" --version; '
    '"${PYTHON_TOOL_VENV}/bin/cmake-format" --version; '
    '"${PYTHON_TOOL_VENV}/bin/yamllint" --version; '
    '"${PYTHON_TOOL_VENV}/bin/gcovr" --version; '
    'rm -f -- "${PYTHON_TOOL_VENV}/.lock"'
)

GOOD_UV_BLOCK = (
    "WORKDIR /opt/ra8-python-project\n"
    f"{EXPECTED_UV_RUN_BLOCK}\n"
    'ENV RA8_TOOL_VENV="${PYTHON_TOOL_VENV}"\n'
)

GOOD_ZSHRC = b"""export PATH="$HOME/.local/bin:$PATH"
if [[ -r ~/.p10k.zsh ]]; then
  source ~/.p10k.zsh
fi
"""

BAD_ZSHRC = b"if [[ -r ~/.p10k.zsh ]; then\n  source ~/.p10k.zsh\n"


def _assert_dockerfile(root: Path, failures: list[str]) -> None:
    """Assert hadolint fires on a bad Dockerfile and is silent on a clean one."""
    (root / ".devcontainer/Dockerfile").write_bytes(GOOD_DOCKERFILE)
    got = check_one(".devcontainer/Dockerfile", root)
    expect(not got, f"a clean Dockerfile yields no findings (got {got})", failures)

    (root / ".devcontainer/Dockerfile").write_bytes(BAD_DOCKERFILE)
    got = check_one(".devcontainer/Dockerfile", root)
    blob = " ".join(f.msg for f in got)
    expect(any(f.code == "DC002" for f in got), "hadolint fires on a bad Dockerfile", failures)
    expect("DL3020" in blob, "  ... reporting DL3020 (ADD instead of COPY)", failures)
    expect("DL4006" in blob, "  ... reporting DL4006 (pipe without pipefail)", failures)


def _assert_uv_execution(failures: list[str]) -> None:
    """Assert authenticated uv execution passes and captured-path execution fails."""
    got = check_uv_execution(".devcontainer/Dockerfile", GOOD_UV_BLOCK)
    expect(not got, f"authenticated uv Docker execution is clean (got {got})", failures)
    mutations = (
        ("RUN set -eux;", "RUN set -eux; /tmp/uv --version;", "raw uv execution"),
        ("--run --no-config lock", "--verify-cache --no-config lock", "mode replacement"),
        ("--run --no-config pip", "--no-config --run pip", "argv reordering"),
        ("--no-config lock --check;", "--no-config lock --check || true;", "status masking"),
    )
    for old, new, label in mutations:
        bad = GOOD_UV_BLOCK.replace(old, new, 1)
        got = check_uv_execution(".devcontainer/Dockerfile", bad)
        expect(any(finding.code == "DC004" for finding in got), f"{label} fires DC004", failures)


def _assert_zshrc(root: Path, failures: list[str]) -> None:
    """Assert zsh -n fires on a broken zshrc and accepts zsh-only syntax.

    The negative case is the whole reason this file's class is ``zsh`` and not
    ``shell``: bash cannot parse either of those lines, so checking them with
    bash would report a syntax error in a correct file.
    """
    (root / ".devcontainer/zshrc").write_bytes(GOOD_ZSHRC)
    got = check_one(".devcontainer/zshrc", root)
    expect(not got, f"a clean zshrc yields no findings (got {got})", failures)

    (root / ".devcontainer/zshrc").write_bytes(BAD_ZSHRC)
    got = check_one(".devcontainer/zshrc", root)
    expect(
        any(f.code == "DC003" for f in got),
        "zsh -n fires on a zshrc with a syntax error",
        failures,
    )

    (root / ".devcontainer/zshrc").write_bytes(
        b'H=${(%):-%n}\nplugins=(git gh docker)\nprint -r -- "$H ${#plugins}"\n'
    )
    got = check_one(".devcontainer/zshrc", root)
    expect(
        not got,
        f"zsh-specific syntax is accepted, not flagged (got {got})",
        failures,
    )


def _assert_format_rules(failures: list[str]) -> None:
    """Assert each DC001 byte-level formatting rule fires on its own payload."""
    for payload, want, label in (
        (b"FROM x\n\tRUN y\n", "tab indentation", "tab indentation fires DC001"),
        (b"FROM x   \n", "trailing whitespace", "trailing whitespace fires DC001"),
        (b"FROM x", "no final newline", "a missing final newline fires DC001"),
        (b"FROM x\r\ny\n", "CRLF", "a CRLF ending fires DC001"),
        (b"FROM \xc2\xa9\n", "non-ASCII", "a non-ASCII byte fires DC001"),
    ):
        got = check_format("f", payload)
        expect(any(want in f.msg for f in got), label, failures)


def _assert_scope(failures: list[str]) -> None:
    """Assert the real scope resolves both files and excludes the vendored theme."""
    found = targets()
    expect(
        len(found) >= FILE_FLOOR,
        f"the real scope resolves both files (got {found})",
        failures,
    )
    expect(
        ".devcontainer/p10k.zsh" not in found,
        "the vendored p10k theme config stays out of scope",
        failures,
    )


def selftest() -> int:
    """Assert both linters fire on broken input and stay quiet on good input."""
    print("check_devcontainer.py --selftest")
    failures: list[str] = []

    if require_tools() != 0:
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / ".devcontainer").mkdir()
        _assert_dockerfile(root, failures)
        _assert_uv_execution(failures)
        _assert_zshrc(root, failures)
        _assert_format_rules(failures)

    _assert_scope(failures)
    return report(failures)


def main(argv: list[str]) -> int:
    """Lint and format-check the devcontainer definition.

    The devcontainer is what gives the Mac an Ubuntu userland to run the CI
    suite in, so a broken definition does not fail loudly -- it makes `just
    ci` unable to run at all, on the one platform that cannot verify natively.

    Returns 0 when clean, 1 on any finding or a failing selftest.
    """
    ap = argparse.ArgumentParser(description="Lint and format-check the devcontainer files")
    ap.add_argument("--selftest", action="store_true", help="assert both directions")
    ap.add_argument("--list-files", action="store_true", help="print the scanned file list")
    args = ap.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    files = targets()
    if args.list_files:
        print("\n".join(files))
        return 0

    if require_tools() != 0:
        return 2
    if len(files) < FILE_FLOOR:
        print(
            f"check_devcontainer.py: FATAL -- {len(files)} file(s) found, floor is "
            f"{FILE_FLOOR}. An empty scan reports success because it saw nothing.",
            file=sys.stderr,
        )
        return 2

    findings: list[Finding] = []
    for rel in files:
        findings.extend(check_one(rel))
    if findings:
        print(
            f"\n{len(findings)} finding(s) in {len(files)} devcontainer file(s):\n", file=sys.stderr
        )
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print(f"check_devcontainer.py: {len(files)} file(s), no findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
