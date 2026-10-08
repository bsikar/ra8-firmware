#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""No first-party Zig build bundles compiler_rt into a cortex-m link.

Zig's compiler_rt carries its own libm (floorf, sqrtf, sinf, ...), built
soft-float: the argument arrives in r0. The images are hard-float and link
`-nostdlib <zig archives> -lm -lgcc`, so a bundled copy's weak definitions
win over newlib's and every C float call reads the wrong register. That is
how reflow_content drew blank pages (RA8FW-941, fixed by RA8FW-943).

A build may still bundle compiler_rt for a host target. The accepted form
derives the switch from the target:

    lib.bundle_compiler_rt = lib.root_module.resolved_target.?.result.os.tag != .freestanding;

A bare `bundle_compiler_rt = true` fails unless its file is in ALLOWED with
the reason it is host-only or tracked.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

_BUNDLE_ON = re.compile(r"\bbundle_compiler_rt\s*=\s*true\b")

# Each entry is a repo-relative path and why its bare `true` is fine.
ALLOWED: dict[str, str] = {
    "apps/host/firmware_pipeline/zig/build.zig": "host adapter, linked by cmake and cargo on the host",
    "tests/abi_chain_fixture/build.zig": "host ABI fixture",
    "tests/zig_abi_fixture/build.zig": "host ABI fixture",
    "tests/zig_build_graph/cpu1_image.zig": "M33 RPC entry object, tracked by RA8FW-944",
}


def _repo_root() -> pathlib.Path:
    """Return the repository root."""
    return pathlib.Path(__file__).resolve().parents[2]


def _comment_free(line: str) -> str:
    """Drop a Zig line comment so prose about the field never matches."""
    return line.split("//", 1)[0]


def violations(path: str, text: str) -> list[str]:
    """Every bare bundle-on line in one file, as path:line findings."""
    if path in ALLOWED:
        return []
    found = []
    for number, line in enumerate(text.splitlines(), start=1):
        if _BUNDLE_ON.search(_comment_free(line)):
            found.append(f"{path}:{number}: {line.strip()}")
    return found


def _tracked_zig(root: pathlib.Path) -> list[str]:
    """First-party Zig sources: tracked, outside vendored trees."""
    out = subprocess.run(
        ["git", "ls-files", "*.zig"], cwd=root, check=True, capture_output=True, text=True
    ).stdout
    return [p for p in out.splitlines() if "third_party/" not in p]


def selftest() -> int:
    """Prove the rule fires, stays quiet on the accepted forms, and the allowlist exists."""
    bad = "    lib.bundle_compiler_rt = true;\n"
    good = (
        "    lib.bundle_compiler_rt = lib.root_module.resolved_target.?.result.os.tag != .freestanding;\n"
        "    lib.bundle_compiler_rt = false;\n"
        "    // bundle_compiler_rt = true would be wrong here\n"
    )
    checks = [
        (len(violations("libs/x/build.zig", bad)) == 1, "bare true is flagged"),
        (violations("libs/x/build.zig", good) == [], "accepted forms pass"),
        (violations(next(iter(ALLOWED)), bad) == [], "allowlisted file passes"),
    ]
    root = _repo_root()
    checks.extend((root.joinpath(p).is_file(), f"allowlisted {p} exists") for p in ALLOWED)
    failed = [name for ok, name in checks if not ok]
    for name in failed:
        print(f"check_compiler_rt_bundle selftest FAILED: {name}", file=sys.stderr)
    return 1 if failed else 0


def main() -> int:
    """Scan the tree, or run the selftest."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true")
    if parser.parse_args().selftest:
        return selftest()
    root = _repo_root()
    files = _tracked_zig(root)
    found = []
    for path in files:
        found.extend(violations(path, root.joinpath(path).read_text(encoding="utf-8")))
    for line in found:
        print(f"bundle_compiler_rt on a cortex-m link: {line}", file=sys.stderr)
    if found:
        print("derive it from the target (see this script's docstring)", file=sys.stderr)
        return 1
    print(f"check_compiler_rt_bundle: {len(files)} Zig files, no bare bundle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
