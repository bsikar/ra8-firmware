#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# rot_keystore.py -- versioned, tagged store for root-of-trust signing keys.
#
# Keeps a HISTORY of every RoT signing key so you can create new credentials
# whenever you want and still recover any prior key -- with or without the team
# OpenBao vault. Two backends, auto-selected (override with --backend):
#
#   openbao  KV v2 at BAO_KV_MOUNT / BAO_ROT_SECRET_PATH (default
#            secret / ra8d2/rot-signing-key). Native versioning = the history.
#            Reachability + identity come from openbao_client.py.
#   local    a 0700 directory (RA8_ROT_STORE_DIR, default ~/.config/ra8/rot)
#            holding one PEM per version plus history.json. This is the
#            "no vault" path: cloning the repo and doing RoT work needs nothing
#            but this script and openssl.
#
# Every stored version is tagged with: fingerprint (SHA-256 of the public SPKI,
# matching rot_provision.sh), algorithm, created_at (UTC), git_commit, note.
#
# Commands:
#   store   [--key PEM] [--note TEXT] [--backend B]   persist a key as a new version
#   get     [--version N] [--out PEM] [--backend B]   recover a version's private key
#   history [--backend B]                             list every version + its tags
#   status  [--backend B]                             active backend + latest version
#   rekey   [--patch] [--note TEXT] [--out PEM] [--backend B]
#           generate a NEW key, back up the current one, store + tag the new
#           version, optionally provision it into ra8_rot.c, install as working key

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from openbao_client import OpenBaoClient, OpenBaoError

_REPO_ROOT = Path(__file__).resolve().parents[2]
_ROT_C = _REPO_ROOT / "libs" / "ra8_dfu" / "src" / "ra8_rot.c"
_ROT_SIGN = _REPO_ROOT / "tools" / "rot_sign.py"
_ROT_PATCH = _REPO_ROOT / "tools" / "rot_patch_pubkey.py"
_WORKING_KEY = Path.home() / "ra8d2-rot-signing-key.pem"
_ALGORITHM = "ecdsa-p256"


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _git_commit() -> str:
    try:
        out = subprocess.run(  # noqa: S603
            ["git", "-C", str(_REPO_ROOT), "rev-parse", "--short", "HEAD"],  # noqa: S607
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"
    return out.stdout.strip() or "unknown"


def fingerprint(pem_path: Path) -> str:
    """SHA-256 of the public SPKI, matching rot_provision.sh's fingerprint."""
    spki = subprocess.run(  # noqa: S603
        ["openssl", "ec", "-in", str(pem_path), "-pubout"],  # noqa: S607
        capture_output=True,
        check=True,
    ).stdout
    dgst = subprocess.run(
        ["openssl", "dgst", "-sha256"],  # noqa: S607
        input=spki,
        capture_output=True,
        check=True,
    ).stdout.decode()
    return dgst.split()[-1]


def _tags(pem_path: Path, note: str) -> dict[str, str]:
    return {
        "fingerprint": fingerprint(pem_path),
        "algorithm": _ALGORITHM,
        "created_at": _now_iso(),
        "git_commit": _git_commit(),
        "note": note,
    }


class LocalBackend:
    """File-backed versioned store for operators without the OpenBao vault."""

    def __init__(self) -> None:
        override = os.environ.get("RA8_ROT_STORE_DIR", "").strip()
        self.dir = Path(override) if override else (Path.home() / ".config" / "ra8" / "rot")
        self.dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.index = self.dir / "history.json"

    @property
    def name(self) -> str:
        return f"local ({self.dir})"

    def _load(self) -> list[dict]:
        if not self.index.exists():
            return []
        return json.loads(self.index.read_text(encoding="utf-8"))

    def _save(self, entries: list[dict]) -> None:
        self.index.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
        self.index.chmod(0o600)

    def store(self, pem: str, tags: dict[str, str]) -> int:
        entries = self._load()
        if entries and entries[-1]["fingerprint"] == tags["fingerprint"]:
            return int(entries[-1]["version"])
        version = len(entries) + 1
        fname = f"rot-v{version:03d}-{tags['fingerprint'][:12]}.pem"
        key_file = self.dir / fname
        key_file.write_text(pem, encoding="ascii")
        key_file.chmod(0o600)
        entries.append({"version": version, "file": fname, **tags})
        self._save(entries)
        return version

    def get(self, version: int | None) -> str:
        entries = self._load()
        if not entries:
            msg = "local store is empty"
            raise OpenBaoError(msg)
        entry = (
            entries[-1]
            if version is None
            else next((e for e in entries if int(e["version"]) == version), None)
        )
        if entry is None:
            msg = f"version {version} not in local store"
            raise OpenBaoError(msg)
        return (self.dir / entry["file"]).read_text(encoding="ascii")

    def history(self) -> list[dict]:
        return self._load()


class OpenBaoBackend:
    """KV v2-backed versioned store; the vault's own versioning is the history."""

    def __init__(self, client: OpenBaoClient) -> None:
        self.client = client
        self.path = os.environ.get("BAO_ROT_SECRET_PATH", "ra8d2/rot-signing-key")

    @property
    def name(self) -> str:
        return f"openbao ({self.client.addr}, {self.client.mount}/{self.path})"

    def store(self, pem: str, tags: dict[str, str]) -> int:
        latest = self.history()
        if latest and latest[-1]["fingerprint"] == tags["fingerprint"]:
            return int(latest[-1]["version"])
        version = self.client.kv_put(self.path, {"pem": pem, **tags})
        self.client.kv_put_metadata(
            self.path,
            {"purpose": "RA8 root-of-trust signing key", "algorithm": _ALGORITHM},
        )
        return version

    def get(self, version: int | None) -> str:
        data = self.client.kv_get(self.path, version=version)
        if "pem" not in data:
            msg = f"no RoT key at version {version or 'latest'}"
            raise OpenBaoError(msg)
        return data["pem"]

    def history(self) -> list[dict]:
        meta = self.client.kv_metadata(self.path)
        versions = meta.get("versions", {})
        out: list[dict] = []
        for vnum in sorted(versions, key=int):
            if versions[vnum].get("destroyed") or versions[vnum].get("deletion_time"):
                continue
            data = self.client.kv_get(self.path, version=int(vnum))
            out.append(
                {
                    "version": int(vnum),
                    "fingerprint": data.get("fingerprint", "?"),
                    "algorithm": data.get("algorithm", "?"),
                    "created_at": data.get("created_at", versions[vnum].get("created_time", "?")),
                    "git_commit": data.get("git_commit", "?"),
                    "note": data.get("note", ""),
                }
            )
        return out


def select_backend(prefer: str) -> LocalBackend | OpenBaoBackend:
    """Pick a backend: 'local', 'openbao', or 'auto' (vault if reachable)."""
    if prefer == "local":
        return LocalBackend()
    client = OpenBaoClient()
    if prefer == "openbao":
        if not client.configured:
            sys.exit("openbao backend requested but no BAO_ADDR / AppRole creds found")
        return OpenBaoBackend(client)
    # auto: prefer the vault when it is configured AND reachable, else local.
    if client.configured:
        try:
            client.login()
        except OpenBaoError as exc:
            sys.stderr.write(f"rot_keystore: OpenBao unavailable ({exc}); using local store\n")
        else:
            return OpenBaoBackend(client)
    return LocalBackend()


def _print_history(rows: list[dict]) -> None:
    if not rows:
        print("(no versions stored yet)")
        return
    for r in rows:
        print(
            f"v{r['version']:<3} {r['fingerprint']}  {r['created_at']}  "
            f"{r['git_commit']}  {r.get('note') or '-'}"
        )


def _keygen(key_path: Path, pubkey_c: Path) -> None:
    subprocess.run(  # noqa: S603
        [
            sys.executable,
            str(_ROT_SIGN),
            "keygen",
            "--key",
            str(key_path),
            "--pubkey-c",
            str(pubkey_c),
        ],
        check=True,
    )
    key_path.chmod(0o600)


def cmd_store(args: argparse.Namespace) -> int:
    backend = select_backend(args.backend)
    key = Path(args.key)
    if not key.exists():
        sys.exit(f"key not found: {key}")
    version = backend.store(key.read_text(encoding="ascii"), _tags(key, args.note))
    print(f"stored {key} as version {version} in {backend.name}")
    return 0


def cmd_get(args: argparse.Namespace) -> int:
    backend = select_backend(args.backend)
    pem = backend.get(args.version)
    if args.out:
        out = Path(args.out)
        out.write_text(pem, encoding="ascii")
        out.chmod(0o600)
        print(f"wrote {args.version or 'latest'} to {out} (mode 0600)")
    else:
        sys.stdout.write(pem)
    return 0


def cmd_history(args: argparse.Namespace) -> int:
    backend = select_backend(args.backend)
    _print_history(backend.history())
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    backend = select_backend(args.backend)
    rows = backend.history()
    print(f"backend: {backend.name}")
    if rows:
        latest = rows[-1]
        print(f"latest:  v{latest['version']}  {latest['fingerprint']}  {latest['created_at']}")
    else:
        print("latest:  (none stored)")
    return 0


def cmd_rekey(args: argparse.Namespace) -> int:
    backend = select_backend(args.backend)
    working = Path(args.out)
    # 1. Preserve the outgoing working key first, so no key is ever lost.
    if working.exists():
        v = backend.store(working.read_text(encoding="ascii"), _tags(working, "superseded key"))
        print(f"backed up current working key as version {v} in {backend.name}")
    # 2. Generate the new keypair into a temp dir.
    with tempfile.TemporaryDirectory() as td:
        new_key = Path(td) / "rot-new.pem"
        pubkey_c = Path(td) / "rot-new-pubkey.h"
        _keygen(new_key, pubkey_c)
        tags = _tags(new_key, args.note or "rekey")
        version = backend.store(new_key.read_text(encoding="ascii"), tags)
        print(f"stored NEW key as version {version} ({tags['fingerprint']}) in {backend.name}")
        # 3. Optionally provision the new public key into ra8_rot.c.
        if args.patch:
            subprocess.run(  # noqa: S603
                [sys.executable, str(_ROT_PATCH), str(_ROT_C), str(pubkey_c)],
                check=True,
            )
        # 4. Install as the working key rot_sign.py uses.
        working.write_text(new_key.read_text(encoding="ascii"), encoding="ascii")
        working.chmod(0o600)
    print(f"installed new working key at {working} (mode 0600)")
    print("re-flash the device: the new public key is the only one it will trust.")
    return 0


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Versioned, tagged root-of-trust key store.")
    p.add_argument(
        "--backend",
        choices=("auto", "openbao", "local"),
        default="auto",
        help="key store backend (default: auto -- vault if reachable, else local)",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("store", help="store a key file as a new version")
    s.add_argument("--key", default=str(_WORKING_KEY), help="private-key PEM to store")
    s.add_argument("--note", default="manual store", help="tag note for this version")
    s.set_defaults(func=cmd_store)

    g = sub.add_parser("get", help="recover a version's private key")
    g.add_argument("--version", type=int, default=None, help="version number (default: latest)")
    g.add_argument("--out", default=None, help="write PEM here 0600 (default: stdout)")
    g.set_defaults(func=cmd_get)

    h = sub.add_parser("history", help="list every stored version and its tags")
    h.set_defaults(func=cmd_history)

    st = sub.add_parser("status", help="show the active backend and latest version")
    st.set_defaults(func=cmd_status)

    rk = sub.add_parser("rekey", help="create a new key, store + tag it, install as working key")
    rk.add_argument("--patch", action="store_true", help="also provision the pubkey into ra8_rot.c")
    rk.add_argument("--note", default=None, help="tag note for the new version")
    rk.add_argument("--out", default=str(_WORKING_KEY), help="working-key path to install to")
    rk.set_defaults(func=cmd_rekey)
    return p


def main(argv: list[str]) -> int:
    """CLI entry: dispatch to the selected subcommand."""
    args = _build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except (OpenBaoError, subprocess.CalledProcessError) as exc:
        sys.stderr.write(f"rot_keystore: {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
