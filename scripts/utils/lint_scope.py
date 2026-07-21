#!/usr/bin/env python3
"""Single definition of the file scope for the cmake and yaml lint gates.

Both scopes used to be inline ``git ls-files | grep -Ev ... | grep -E ...``
pipelines in ``scripts/ci.sh``. That is a coverage map written down in a place
nothing can interrogate, which is precisely what ``check_lint_coverage.py``
must not do a second time: it would have had to restate the same two greps to
know what those gates cover, and the two copies would disagree the first time
either changed.

So the pipelines moved here. ``gate_lint_cmake`` and ``gate_lint_yaml`` consume
this to build their file lists, and the coverage gate consumes the same call to
learn what they claim. One definition, two readers, no way to drift.

Usage:
    lint_scope.py cmake     # every first-party CMakeLists.txt and *.cmake
    lint_scope.py yaml      # every first-party *.yml / *.yaml
    lint_scope.py --list    # the scope names this module knows

Output is one repo-relative path per line, sorted, suitable for `mapfile -t`.
Exits non-zero -- printing nothing -- when a scope resolves to zero files, so a
gate can never mistake a broken enumeration for a clean tree.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- trusted: fixed git argv
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
)

# Vendored SOUP and generated tables, matching the CLAUDE.md exemption list and
# the EXEMPT_PREFIXES table in lint_coverage_rules.py.
EXCLUDED_PREFIXES = ("libs/third_party/", "libs/fonts/", "tools/vela/generated/")

SCOPES: dict[str, tuple[str, ...]] = {
    "cmake": ("CMakeLists.txt", ".cmake"),
    "yaml": (".yml", ".yaml"),
}


def tracked() -> list[str]:
    proc = subprocess.run(
        [  # noqa: S607 -- trusted: fixed git argv
            "git",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write("lint_scope.py: FATAL -- `git ls-files` failed\n")
        sys.exit(2)
    return [p for p in proc.stdout.split("\0") if p]


def files_for(scope: str) -> list[str]:
    suffixes = SCOPES[scope]
    out = [
        rel for rel in tracked() if not rel.startswith(EXCLUDED_PREFIXES) and rel.endswith(suffixes)
    ]
    return sorted(out)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="print the file scope of a lint gate")
    ap.add_argument("scope", nargs="?", choices=sorted(SCOPES), help="which scope to print")
    ap.add_argument("--list", action="store_true", help="print known scope names")
    args = ap.parse_args(argv[1:])

    if args.list:
        for name in sorted(SCOPES):
            print(name)
        return 0
    if not args.scope:
        ap.error("a scope name is required (or --list)")

    found = files_for(args.scope)
    if not found:
        sys.stderr.write(
            f"lint_scope.py: FATAL -- scope {args.scope!r} matched no files.\n"
            "  An empty scope makes its gate report success over nothing.\n"
        )
        return 2
    print("\n".join(found))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
