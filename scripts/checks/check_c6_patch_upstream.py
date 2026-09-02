#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Apply-check the C6 patch series against its exact fetched upstream pin.

The per-push patch gate is deliberately offline: it proves the pin, numbered
series, and build entry point remain connected. This networked companion runs
in the controlled weekly SOUP refresh job. It fetches only the exact immutable
commit into a disposable repository, verifies the checkout identity, then
applies the committed series without building, flashing, or contacting a rig.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "dev"))

from git_environment import sanitized_git_environment  # noqa: E402 -- repository path added above

C6_DIR = REPO_ROOT / "coprocessor" / "esp32c6"
PINS_FILE = C6_DIR / "pins.env"
SERIES_FILE = C6_DIR / "patches" / "series"
EXPECTED_UPSTREAM = "https://github.com/espressif/esp-hosted-mcu"
PATCH_NAME_RE = re.compile(r"^[0-9]{4}-[a-z0-9][a-z0-9-]*\.patch$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


def _git_environment() -> dict[str, str]:
    """Return a noninteractive Git environment with no host-level filters."""
    environment = sanitized_git_environment()
    environment.update(
        {
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_LFS_SKIP_SMUDGE": "1",
            "GIT_TERMINAL_PROMPT": "0",
        }
    )
    return environment


def _git(cwd: Path, *args: str, timeout: int = 300) -> subprocess.CompletedProcess[str]:
    """Run one bounded Git command in the disposable repository."""
    return subprocess.run(  # noqa: S603 -- fixed Git and controlled argv
        [  # noqa: S607 -- Git is a required repository tool
            "git",
            "-c",
            "core.hooksPath=/dev/null",
            "-c",
            "http.followRedirects=false",
            "-c",
            "http.sslVerify=true",
            "-c",
            "protocol.file.allow=never",
            "-c",
            "protocol.ext.allow=never",
            "-C",
            str(cwd),
            *args,
        ],
        env=_git_environment(),
        text=True,
        capture_output=True,
        check=False,
        timeout=timeout,
    )


def _pin(text: str, name: str) -> str:
    """Return one unquoted strict KEY=value row."""
    prefix = name + "="
    values = [line[len(prefix) :] for line in text.splitlines() if line.startswith(prefix)]
    if len(values) != 1 or not values[0] or any(char.isspace() for char in values[0]):
        msg = f"{PINS_FILE.relative_to(REPO_ROOT)}: expected one strict {name}=value row"
        raise ValueError(msg)
    return values[0]


def load_inputs() -> tuple[str, str, tuple[Path, ...]]:
    """Load and validate the allowlisted upstream, full commit, and series."""
    pins = PINS_FILE.read_text(encoding="utf-8")
    upstream = _pin(pins, "ESP_HOSTED_MCU_URL")
    commit = _pin(pins, "ESP_HOSTED_MCU_COMMIT")
    if upstream != EXPECTED_UPSTREAM:
        msg = f"refusing unallowlisted C6 upstream: {upstream!r}"
        raise ValueError(msg)
    if not COMMIT_RE.fullmatch(commit):
        msg = "ESP_HOSTED_MCU_COMMIT must be one full lowercase 40-hex commit"
        raise ValueError(msg)
    names = tuple(
        line.strip()
        for line in SERIES_FILE.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )
    if not names or len(names) != len(set(names)):
        msg = "C6 patch series must be nonempty and contain no duplicates"
        raise ValueError(msg)
    if any(PATCH_NAME_RE.fullmatch(name) is None for name in names):
        msg = "C6 patch series contains a non-numbered patch name"
        raise ValueError(msg)
    patches = tuple(SERIES_FILE.parent / name for name in names)
    missing = [path.name for path in patches if not path.is_file()]
    if missing:
        msg = f"C6 patch series is missing: {', '.join(missing)}"
        raise ValueError(msg)
    return upstream, commit, patches


def check_checkout(checkout: Path, commit: str, patches: tuple[Path, ...]) -> list[str]:
    """Verify identity, non-vacuous apply, and exact reverse replay."""
    findings: list[str] = []
    identity = _git(checkout, "rev-parse", "HEAD")
    actual = identity.stdout.strip() if identity.returncode == 0 else ""
    if actual != commit:
        findings.append(f"checkout is {actual or 'unreadable'}, expected exact pin {commit}")
        return findings
    if not patches:
        return ["patch series is empty; applicability proof would be vacuous"]
    for patch in patches:
        check = _git(checkout, "apply", "--unidiff-zero", "--check", str(patch.resolve()))
        if check.returncode != 0:
            findings.append(f"{patch.name}: apply-check failed: {check.stderr.strip()}")
            break
        apply = _git(checkout, "apply", "--unidiff-zero", str(patch.resolve()))
        if apply.returncode != 0:
            findings.append(f"{patch.name}: checked but could not apply: {apply.stderr.strip()}")
            break
    if findings:
        return findings
    changed = _git(checkout, "diff", "--quiet", "--exit-code")
    if changed.returncode == 0:
        return ["applied series leaves no changed bytes; applicability proof is vacuous"]
    if changed.returncode != 1:
        return [f"could not inspect applied patch bytes: {changed.stderr.strip()}"]
    for patch in reversed(patches):
        reverse_check = _git(
            checkout,
            "apply",
            "--unidiff-zero",
            "--reverse",
            "--check",
            str(patch.resolve()),
        )
        if reverse_check.returncode != 0:
            findings.append(
                f"{patch.name}: reverse apply-check failed: {reverse_check.stderr.strip()}"
            )
            break
        reverse = _git(
            checkout,
            "apply",
            "--unidiff-zero",
            "--reverse",
            str(patch.resolve()),
        )
        if reverse.returncode != 0:
            findings.append(f"{patch.name}: reverse apply failed: {reverse.stderr.strip()}")
            break
    restored = _git(checkout, "status", "--porcelain=v1", "--untracked-files=all")
    if not findings and (restored.returncode != 0 or restored.stdout):
        findings.append("reverse replay did not restore the exact clean pinned checkout")
    return findings


def verify_upstream() -> list[str]:
    """Fetch only the exact pin and apply-check the series in a temp tree."""
    upstream, commit, patches = load_inputs()
    with tempfile.TemporaryDirectory(prefix="ra8-c6-patch-upstream-") as raw_tmp:
        checkout = Path(raw_tmp) / "esp-hosted-mcu"
        checkout.mkdir()
        init = _git(checkout, "init", "--quiet")
        if init.returncode != 0:
            return [f"git init failed: {init.stderr.strip()}"]
        fetch = _git(
            checkout,
            "fetch",
            "--quiet",
            "--no-tags",
            "--depth=1",
            upstream,
            commit,
        )
        if fetch.returncode != 0:
            return [f"exact-pin fetch failed: {fetch.stderr.strip()}"]
        checkout_pin = _git(checkout, "checkout", "--quiet", "--detach", "FETCH_HEAD")
        if checkout_pin.returncode != 0:
            return [f"exact-pin checkout failed: {checkout_pin.stderr.strip()}"]
        return check_checkout(checkout, commit, patches)


def _fixture(root: Path, content: str) -> str:
    """Create one committed source tree and return its exact commit."""
    root.mkdir()
    if _git(root, "init", "--quiet").returncode != 0:
        msg = "selftest git init failed"
        raise RuntimeError(msg)
    (root / "source.txt").write_text(content, encoding="ascii")
    for args in (
        ("config", "user.email", "selftest@invalid"),
        ("config", "user.name", "selftest"),
        ("add", "source.txt"),
        ("commit", "--quiet", "-m", "fixture"),
    ):
        result = _git(root, *args)
        if result.returncode != 0:
            msg = f"selftest git {' '.join(args)} failed: {result.stderr}"
            raise RuntimeError(msg)
    return _git(root, "rev-parse", "HEAD").stdout.strip()


def selftest() -> int:
    """Prove apply/reverse succeeds and context, identity, or scope drift fires."""
    failures: list[str] = []
    patch_text = """diff --git a/source.txt b/source.txt
--- a/source.txt
+++ b/source.txt
@@ -1 +1 @@
-before
+after
"""
    with tempfile.TemporaryDirectory(prefix="ra8-c6-patch-selftest-") as raw_tmp:
        root = Path(raw_tmp)
        patch = root / "0001-fixture.patch"
        patch.write_text(patch_text, encoding="ascii")
        good = root / "good"
        good_pin = _fixture(good, "before\n")
        if check_checkout(good, good_pin, (patch,)):
            failures.append("exact-pin applicable patch did not stay quiet")
        drift = root / "drift"
        drift_pin = _fixture(drift, "different\n")
        if not check_checkout(drift, drift_pin, (patch,)):
            failures.append("patch context drift did not fire")
        wrong_pin = root / "wrong-pin"
        actual_pin = _fixture(wrong_pin, "before\n")
        if not check_checkout(wrong_pin, "0" * len(actual_pin), (patch,)):
            failures.append("checkout identity drift did not fire")
        empty = root / "empty"
        empty_pin = _fixture(empty, "before\n")
        if not check_checkout(empty, empty_pin, ()):
            failures.append("empty patch series did not fire")
    if failures:
        print("check_c6_patch_upstream.py --selftest: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("check_c6_patch_upstream.py --selftest: PASS (4 cases, both directions)")
    return 0


def main() -> int:
    """Run isolated selftests or the controlled exact-pin network check."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--selftest", action="store_true")
    group.add_argument("--verify-upstream", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    try:
        findings = verify_upstream()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        findings = [str(exc)]
    if findings:
        print("check_c6_patch_upstream.py: FAIL", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    _upstream, commit, patches = load_inputs()
    print(
        f"check_c6_patch_upstream.py: {len(patches)} non-vacuous patch(es) "
        f"apply and reverse cleanly at exact pin {commit}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
