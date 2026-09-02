# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Install or render the declaration-derived fleet SSH configuration."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import fleet_reach as fr


def _ensure_include(config: Path) -> str:
    """Put the generated fragment's Include line first in one SSH config."""
    header = (
        "# ra8-firmware: the CI fleet's machines, GENERATED from infra/fleet.yml.\n"
        "# Refresh with `just infra::ssh_config`.\n"
        f"{fr.SSH_INCLUDE_LINE}\n"
    )
    if not config.exists():
        config.write_text(header, encoding="utf-8")
        config.chmod(0o600)
        return f"created {config} with the include line"
    existing = config.read_text(encoding="utf-8")
    if any(line.strip() == fr.SSH_INCLUDE_LINE for line in existing.splitlines()):
        return f"{config} already includes it"
    config.write_text(f"{header}\n{existing}", encoding="utf-8")
    return f"prepended the include line to {config}"


def command(data: dict[str, Any], *, install: bool) -> int:
    """Print or install the fleet's generated SSH config fragment."""
    body = fr.render_ssh_config(data)
    if not install:
        print(body, end="")
        return 0
    ssh_dir = Path.home() / ".ssh"
    ssh_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    fragment = ssh_dir / fr.SSH_FRAGMENT_NAME
    fragment.write_text(body, encoding="utf-8")
    fragment.chmod(0o600)
    print(f"wrote {fragment} ({len(data['hosts'])} host(s))")
    print(_ensure_include(ssh_dir / "config"))
    return 0
