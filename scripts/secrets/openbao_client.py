# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shared HTTP client for the EXISTING OpenBao server.

This does NOT run, embed, or spin up a vault. It is a thin urllib client
(no `bao` CLI needed) that talks to the OpenBao server you already run --
the k3s pod reached at BAO_ADDR (e.g. http://100.64.0.1:32200). It performs
AppRole login plus KV v2 read / write / metadata, so the same operator
identity backs both the HIL Tapo secrets (hil_secrets.py) and the
root-of-trust key store (rot_keystore.py). No server = fall back to the
local path (see rot_keystore.py's `local` backend / the .env pattern).

Consumer config comes from an operator-controlled 0600 creds file (default
~/.config/hil/openbao.env, override with HIL_OPENBAO_ENV) or the matching
environment variables (e.g. CI injecting GitHub Actions secrets):

  BAO_ADDR          base URL, e.g. http://100.64.0.1:32200
  ROLE_ID           AppRole role id
  SECRET_ID         AppRole secret id
  BAO_APPROLE_PATH  auth mount        (default "approle")
  BAO_KV_MOUNT      KV v2 mount        (default "secret")

The file never holds the secrets themselves -- only how to reach the vault
and the AppRole identity.
"""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from pathlib import Path

_DEFAULT_CREDS = Path.home() / ".config" / "hil" / "openbao.env"
_HTTP_TIMEOUT_S = 6.0
_GET_ARG_COUNT = 4


class OpenBaoError(RuntimeError):
    """Any OpenBao transport, auth, or KV error (never leaks secret values)."""


def creds_path() -> Path:
    """Return the consumer creds file path, honouring HIL_OPENBAO_ENV."""
    override = os.environ.get("HIL_OPENBAO_ENV", "").strip()
    return Path(override) if override else _DEFAULT_CREDS


def _parse_env_file(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    # External, non-repo file: decode permissively rather than ASCII-strict.
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        out[key.strip()] = val.strip()
    return out


def load_config() -> dict[str, str]:
    """Load consumer config from the 0600 creds file, else the environment."""
    path = creds_path()
    if path.exists():
        return _parse_env_file(path)
    keys = ("BAO_ADDR", "ROLE_ID", "SECRET_ID", "BAO_APPROLE_PATH", "BAO_KV_MOUNT")
    return {k: os.environ[k] for k in keys if k in os.environ}


def _http_json(
    url: str,
    headers: dict[str, str],
    payload: dict | None = None,
    method: str | None = None,
) -> dict:
    if not url.startswith(("http://", "https://")):
        msg = "refusing non-http(s) OpenBao URL"
        raise OpenBaoError(msg)
    body = json.dumps(payload).encode("ascii") if payload is not None else None
    verb = method or ("POST" if payload is not None else "GET")
    hdrs = {"Content-Type": "application/json"} if body is not None else {}
    hdrs.update(headers)
    # Scheme guarded above; BAO_ADDR comes from a local 0600 operator-controlled
    # creds file, never untrusted input.
    req = urllib.request.Request(  # noqa: S310 -- HTTPS scheme validated above
        url, data=body, headers=hdrs, method=verb
    )
    try:
        with urllib.request.urlopen(  # noqa: S310 -- request URL validated above
            req, timeout=_HTTP_TIMEOUT_S
        ) as resp:
            raw = resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        msg = f"OpenBao HTTP {exc.code} for {verb} {url}"
        raise OpenBaoError(msg) from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        msg = f"OpenBao unreachable: {exc}"
        raise OpenBaoError(msg) from exc
    return json.loads(raw) if raw else {}


class OpenBaoClient:
    """AppRole-authenticated KV v2 client for a single OpenBao server."""

    def __init__(self, cfg: dict[str, str] | None = None) -> None:
        """Read connection settings from ``cfg``, or from the environment when None.

        Performs NO I/O and never raises on a missing setting -- every field
        defaults to empty. Reachability is a separate question answered by
        ``configured`` and ``login``, which is what lets a caller construct a
        client and then decide to fall back to a local store instead.
        """
        cfg = cfg if cfg is not None else load_config()
        self.addr = cfg.get("BAO_ADDR", "").rstrip("/")
        self.role_id = cfg.get("ROLE_ID", "")
        self.secret_id = cfg.get("SECRET_ID", "")
        self.approle = cfg.get("BAO_APPROLE_PATH", "approle")
        self.mount = cfg.get("BAO_KV_MOUNT", "secret")
        self._token: str | None = None

    @property
    def configured(self) -> bool:
        """True when address + AppRole identity are all present."""
        return bool(self.addr and self.role_id and self.secret_id)

    def login(self) -> str:
        """AppRole login; caches and returns the client token."""
        if self._token:
            return self._token
        if not self.configured:
            msg = "OpenBao not configured (missing BAO_ADDR / ROLE_ID / SECRET_ID)"
            raise OpenBaoError(msg)
        resp = _http_json(
            f"{self.addr}/v1/auth/{self.approle}/login",
            {},
            {"role_id": self.role_id, "secret_id": self.secret_id},
        )
        token = resp.get("auth", {}).get("client_token")
        if not token:
            msg = "OpenBao login returned no client_token"
            raise OpenBaoError(msg)
        self._token = token
        return token

    def _mount(self, mount: str | None) -> str:
        return mount or self.mount

    def kv_get(self, path: str, version: int | None = None, mount: str | None = None) -> dict:
        """Return one KV v2 version's secret data (latest unless version given)."""
        url = f"{self.addr}/v1/{self._mount(mount)}/data/{path}"
        if version is not None:
            url = f"{url}?version={int(version)}"
        resp = _http_json(url, {"X-Vault-Token": self.login()})
        return resp.get("data", {}).get("data", {})

    def kv_put(self, path: str, data: dict, mount: str | None = None) -> int:
        """Write a new KV v2 version; return the created version number."""
        resp = _http_json(
            f"{self.addr}/v1/{self._mount(mount)}/data/{path}",
            {"X-Vault-Token": self.login()},
            {"data": data},
        )
        return int(resp.get("data", {}).get("version", 0))

    def kv_metadata(self, path: str, mount: str | None = None) -> dict:
        """Return KV v2 metadata (per-version created_time, custom_metadata)."""
        url = f"{self.addr}/v1/{self._mount(mount)}/metadata/{path}"
        try:
            resp = _http_json(url, {"X-Vault-Token": self.login()})
        except OpenBaoError as exc:
            if "HTTP 404" in str(exc):
                return {}
            raise
        return resp.get("data", {})

    def kv_put_metadata(
        self,
        path: str,
        custom_metadata: dict[str, str],
        mount: str | None = None,
    ) -> None:
        """Set path-level custom_metadata (tags) on a KV v2 secret."""
        _http_json(
            f"{self.addr}/v1/{self._mount(mount)}/metadata/{path}",
            {"X-Vault-Token": self.login()},
            {"custom_metadata": custom_metadata},
        )


if __name__ == "__main__":
    import sys

    if len(sys.argv) >= _GET_ARG_COUNT and sys.argv[1] == "get":
        req_path = sys.argv[2].removeprefix("secret/")
        req_key = sys.argv[3]
        try:
            client = OpenBaoClient()
            if not client.configured:
                sys.exit(1)
            secret_data = client.kv_get(req_path)
            if req_key in secret_data:
                print(secret_data[req_key])
                sys.exit(0)
        except (OpenBaoError, OSError):
            sys.exit(1)
    sys.exit(1)
