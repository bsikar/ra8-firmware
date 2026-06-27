# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# hil_secrets.py -- Resolve the HIL Tapo secrets, preferring a self-hosted
# OpenBao vault and falling back to the local .env so the smart plugs stay
# controllable even when OpenBao or the k3s cluster is down.
#
# Resolution order (first that yields ALL required keys wins):
#   1. OpenBao -- AppRole login, then read a KV v2 secret. Attempted only when
#                 the consumer credentials file exists and the server answers
#                 within a short timeout.
#   2. .env    -- python-dotenv into the process environment (the unchanged
#                 legacy behaviour); also the fallback for ANY OpenBao failure.
#
# The OpenBao consumer credentials live OUTSIDE the repo in a 0600 file at
# ~/.config/hil/openbao.env (override the path with HIL_OPENBAO_ENV). That file
# holds how to reach the vault and the AppRole identity -- never the Tapo
# secrets themselves:
#   BAO_ADDR          base URL, e.g. http://100.64.0.1:32200
#   ROLE_ID           AppRole role id
#   SECRET_ID         AppRole secret id
#   BAO_APPROLE_PATH  auth mount (default "approle")
#   BAO_KV_MOUNT      KV v2 mount (default "secret")
#   BAO_SECRET_PATH   secret path under the mount (default "hil/tapo")
#
# populate_env() never raises on an OpenBao error: it always returns a source
# string ("openbao", "dotenv", or "none") and leaves the .env-derived
# environment intact for fallback.

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from pathlib import Path

from dotenv import load_dotenv

# Map OpenBao KV keys -> the TAPO_* environment variables the consumer reads.
_KEY_TO_ENV = {
    "user": "TAPO_USER",
    "pass": "TAPO_PASS",
    "board_ip": "TAPO_BOARD_IP",
    "board_mac": "TAPO_BOARD_MAC",
    "pi_ip": "TAPO_PI_IP",
    "pi_mac": "TAPO_PI_MAC",
}

_DEFAULT_CREDS = Path.home() / ".config" / "hil" / "openbao.env"
_HTTP_TIMEOUT_S = 4.0


def _creds_path() -> Path:
    override = os.environ.get("HIL_OPENBAO_ENV", "").strip()
    return Path(override) if override else _DEFAULT_CREDS


def _parse_env_file(path: Path) -> dict:
    out = {}
    # External, non-repo file: decode permissively rather than ASCII-strict.
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        out[key.strip()] = val.strip()
    return out


def _http_json(url: str, headers: dict, payload: dict | None = None) -> dict:
    if not url.startswith(("http://", "https://")):
        msg = "refusing non-http(s) OpenBao URL"
        raise ValueError(msg)
    body = json.dumps(payload).encode("ascii") if payload is not None else None
    method = "POST" if payload is not None else "GET"
    hdrs = {"Content-Type": "application/json"} if payload is not None else {}
    hdrs.update(headers)
    # Scheme guarded above; BAO_ADDR comes from a local 0600 operator-controlled
    # creds file, never untrusted input.
    req = urllib.request.Request(url, data=body, headers=hdrs, method=method)  # noqa: S310
    with urllib.request.urlopen(req, timeout=_HTTP_TIMEOUT_S) as resp:  # noqa: S310
        return json.loads(resp.read().decode("utf-8"))


def _load_consumer_cfg() -> dict:
    # Prefer the operator-controlled 0600 creds file; otherwise read the same
    # keys from the environment (e.g. CI injecting GitHub Actions secrets).
    path = _creds_path()
    if path.exists():
        return _parse_env_file(path)
    keys = (
        "BAO_ADDR",
        "ROLE_ID",
        "SECRET_ID",
        "BAO_APPROLE_PATH",
        "BAO_KV_MOUNT",
        "BAO_SECRET_PATH",
    )
    return {k: os.environ[k] for k in keys if k in os.environ}


def _fetch_from_openbao() -> dict | None:
    cfg = _load_consumer_cfg()
    addr = cfg.get("BAO_ADDR", "").rstrip("/")
    role_id = cfg.get("ROLE_ID", "")
    secret_id = cfg.get("SECRET_ID", "")
    if not (addr and role_id and secret_id):
        return None
    approle = cfg.get("BAO_APPROLE_PATH", "approle")
    mount = cfg.get("BAO_KV_MOUNT", "secret")
    secret_path = cfg.get("BAO_SECRET_PATH", "ra8d2/tapo")

    login = _http_json(
        f"{addr}/v1/auth/{approle}/login",
        {},
        {"role_id": role_id, "secret_id": secret_id},
    )
    token = login["auth"]["client_token"]
    read = _http_json(
        f"{addr}/v1/{mount}/data/{secret_path}",
        {"X-Vault-Token": token},
    )
    data = read["data"]["data"]
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
    except (urllib.error.URLError, OSError, ValueError, KeyError, TimeoutError):
        secrets = None

    if secrets and all(env in secrets for env in _KEY_TO_ENV.values()):
        for env, value in secrets.items():
            os.environ[env] = value
        source = "openbao"
    return source
