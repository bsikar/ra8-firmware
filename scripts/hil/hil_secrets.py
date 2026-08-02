# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Resolve the HIL Tapo secrets from OpenBao, falling back to a local .env.

Prefers the self-hosted OpenBao vault (talked to over HTTP by
openbao_client.py -- the existing k3s pod at BAO_ADDR, nothing spun up
locally) and falls back to the local .env so the smart plugs stay
controllable even when OpenBao or the k3s cluster is down.

Resolution order (first that yields ALL required keys wins):
  1. OpenBao -- AppRole login, then read a KV v2 secret. Attempted only when
                the consumer credentials file exists and the server answers
                within a short timeout.
  2. .env    -- python-dotenv into the process environment (the unchanged
                legacy behaviour); also the fallback for ANY OpenBao failure.

Note "ALL required keys": a partial OpenBao read is treated as a miss and
falls through, rather than leaving some keys vault-sourced and others stale
from the environment.

The OpenBao consumer credentials live OUTSIDE the repo in a 0600 file at
~/.config/hil/openbao.env (override the path with HIL_OPENBAO_ENV). It holds
how to reach the vault and the AppRole identity -- never the Tapo secrets
themselves. See openbao_client.py for the shared keys; hil-specific:
  BAO_SECRET_PATH   secret path under the mount (default "ra8d2/tapo")

populate_env() never raises on an OpenBao error: it always returns a source
string ("openbao", "dotenv", or "none") and leaves the .env-derived
environment intact for fallback.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from dotenv import load_dotenv

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "secrets"))
from openbao_client import OpenBaoClient, OpenBaoError, load_config

# Map OpenBao KV keys -> the TAPO_* environment variables the consumer reads.
#
# TAPO_RELAY_* is intentionally NOT in this map: the relay plug's IP/MAC are
# non-secret and resolved from .env only. Adding it here would enlarge the
# all-keys-or-miss set checked in populate_env(), so an existing vault holding
# only the board/pi keys would read as an incomplete miss and break board/pi
# OpenBao resolution. Keep relay out of OpenBao resolution.
_KEY_TO_ENV = {
    "user": "TAPO_USER",
    "pass": "TAPO_PASS",
    "board_ip": "TAPO_BOARD_IP",
    "board_mac": "TAPO_BOARD_MAC",
    "pi_ip": "TAPO_PI_IP",
    "pi_mac": "TAPO_PI_MAC",
}


def _fetch_from_openbao() -> dict | None:
    """Read the Tapo secret from OpenBao, or None when it is not configured."""
    cfg = load_config()
    client = OpenBaoClient(cfg)
    if not client.configured:
        return None
    secret_path = cfg.get("BAO_SECRET_PATH", "ra8d2/tapo")
    data = client.kv_get(secret_path)
    return {env: data[key] for key, env in _KEY_TO_ENV.items() if key in data}


def populate_env(env_file: Path, fallback_env: Path) -> str:
    """Populate os.environ with the TAPO_* secrets and return the source used.

    The .env baseline is loaded first so it is always present as a fallback,
    then OpenBao values are overlaid on top when -- and only when -- the vault
    yields a complete set. Any OpenBao error is swallowed.
    """
    source = "none"
    if env_file.exists():
        load_dotenv(env_file)
        source = "dotenv"
    elif fallback_env.exists():
        load_dotenv(fallback_env)
        source = "dotenv"

    try:
        secrets = _fetch_from_openbao()
    except (OpenBaoError, OSError):
        secrets = None

    if secrets and all(env in secrets for env in _KEY_TO_ENV.values()):
        for env, value in secrets.items():
            os.environ[env] = value
        source = "openbao"
    return source
