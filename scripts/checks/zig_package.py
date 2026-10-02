# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Locate a pinned build.zig.zon package in the Zig global cache.

The python twin of cmake/zig_package.cmake: read the package's .url and
.hash out of build.zig.zon, look for <global_cache_dir>/p/<hash>, and run
`zig fetch` once when it is not there yet. A fetch that yields a different
hash is an error, never a silent switch to other contents.
"""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def pin(name: str, root: Path = REPO_ROOT) -> tuple[str, str]:
    """The (url, hash) build.zig.zon pins for `name`."""
    zon = (root / "build.zig.zon").read_text(encoding="utf-8")
    entry = re.search(rf"\.{re.escape(name)} = \.\{{([^}}]*)\}}", zon)
    if entry is None:
        raise SystemExit(f"{name}: no entry for it in build.zig.zon")
    url = re.search(r'\.url = "([^"]+)"', entry.group(1))
    digest = re.search(r'\.hash = "([^"]+)"', entry.group(1))
    if url is None or digest is None:
        raise SystemExit(f"{name}: build.zig.zon entry has no .url/.hash")
    return url.group(1), digest.group(1)


def package_dir(name: str, root: Path = REPO_ROOT) -> Path:
    """The fetched package root for `name`, fetching it first if needed."""
    url, digest = pin(name, root)
    env = subprocess.run(["zig", "env"], capture_output=True, text=True, check=True)
    path = Path(json.loads(env.stdout)["global_cache_dir"]) / "p" / digest
    if not path.is_dir():
        got = subprocess.run(
            ["zig", "fetch", url], capture_output=True, text=True, check=True, cwd=root
        ).stdout.strip()
        if got != digest:
            raise SystemExit(f"{name}: zig fetch gave '{got}', build.zig.zon pins '{digest}'")
    return path
