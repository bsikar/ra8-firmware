#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Versioned, tagged store for root-of-trust signing keys.

Keeps a HISTORY of every RoT signing key so you can create new credentials
whenever you want and still recover any prior key. The store is a 0700
directory (RA8_ROT_STORE_DIR, default ~/.config/ra8/rot) holding one PEM per
version plus history.json: cloning the repo and doing RoT work needs nothing
but this script and openssl.

Every stored version is tagged with: fingerprint (SHA-256 of the public SPKI,
matching rot_provision.sh), algorithm, created_at (UTC), git_commit, note.

Commands:
  store   [--key PEM] [--note TEXT] [--backend B]   persist a key as a new version
  get     [--version N] [--out PEM] [--backend B]   recover a version's private key
  history [--backend B]                             list every version + its tags
  status  [--backend B]                             active backend + latest version
  rekey   [--patch] [--note TEXT] [--out PEM] [--backend B]
          generate a NEW key, back up the current one, store + tag the new
          version, optionally provision it into ra8_rot.c, install as working key
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from datetime import UTC, datetime
from pathlib import Path


_REPO_ROOT = Path(__file__).resolve().parents[2]
_ROT_C = _REPO_ROOT / "libs" / "ra8_dfu" / "src" / "ra8_rot.c"
_ROT_SIGN = _REPO_ROOT / "tools" / "rot" / "src" / "rot_sign.py"
_ROT_PATCH = _REPO_ROOT / "tools" / "rot" / "src" / "rot_patch_pubkey.py"
_WORKING_KEY = Path.home() / "ra8d2-rot-signing-key.pem"
_ALGORITHM = "ecdsa-p256"


class KeyStoreError(Exception):
    """A key-store operation failed (empty store, unknown version, bad request)."""


def _now_iso() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds")


def _git_commit() -> str:
    try:
        out = subprocess.run(  # noqa: S603 -- fixed Git argv, no shell
            ["git", "-C", str(_REPO_ROOT), "rev-parse", "--short", "HEAD"],  # noqa: S607 -- Git is an operator prerequisite
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"
    return out.stdout.strip() or "unknown"


def fingerprint(pem_path: Path) -> str:
    """SHA-256 of the public SPKI, matching rot_provision.sh's fingerprint."""
    spki = subprocess.run(  # noqa: S603 -- explicit argv is never a shell
        ["openssl", "ec", "-in", str(pem_path), "-pubout"],  # noqa: S607 -- OpenSSL is an operator prerequisite
        capture_output=True,
        check=True,
    ).stdout
    dgst = subprocess.run(
        ["openssl", "dgst", "-sha256"],  # noqa: S607 -- OpenSSL is an operator prerequisite
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
    """File-backed versioned store: one 0700 directory with one PEM per version."""

    def __init__(self) -> None:
        """Resolve the store directory and create it 0700 if it does not exist.

        Constructing the backend is what creates the directory, so every other
        method may assume it is present. The 0700 mode is applied at creation
        rather than checked afterwards: these are private signing keys, and a
        store that was briefly world-readable has already leaked.
        """
        override = os.environ.get("RA8_ROT_STORE_DIR", "").strip()
        self.dir = Path(override) if override else (Path.home() / ".config" / "ra8" / "rot")
        self.dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.index = self.dir / "history.json"

    @property
    def name(self) -> str:
        """Human-readable backend identity, including the resolved directory.

        Printed by every command so the operator can see WHICH store was
        written -- the directory is environment-overridable, and a rekey into
        an unexpected store is the mistake worth making visible.
        """
        return f"local ({self.dir})"

    def _load(self) -> list[dict]:
        if not self.index.exists():
            return []
        return json.loads(self.index.read_text(encoding="utf-8"))

    def _save(self, entries: list[dict]) -> None:
        self.index.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
        self.index.chmod(0o600)

    def store(self, pem: str, tags: dict[str, str]) -> int:
        """Persist ``pem`` as a new version and return that version number.

        Idempotent against the newest entry: re-storing a key whose fingerprint
        already matches the latest version returns that version instead of
        appending a duplicate. ``rekey`` relies on this -- it backs up the
        outgoing working key on every run, and without the check a repeated
        rekey would inflate the history with copies of the same key.

        Note it compares only the LAST entry, so re-storing a key that was
        current several versions ago does create a new version. That is
        intended: returning to an older key is a real event worth recording.

        Both the PEM and the index are written 0600.
        """
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
        """Return the private-key PEM for ``version``, or the newest when None.

        Raises KeyStoreError for both an empty store and an unknown version.
        """
        entries = self._load()
        if not entries:
            msg = "local store is empty"
            raise KeyStoreError(msg)
        entry = (
            entries[-1]
            if version is None
            else next((e for e in entries if int(e["version"]) == version), None)
        )
        if entry is None:
            msg = f"version {version} not in local store"
            raise KeyStoreError(msg)
        return (self.dir / entry["file"]).read_text(encoding="ascii")

    def history(self) -> list[dict]:
        """Every stored version, oldest first, as tag dicts.

        Ordering is the contract the callers depend on: ``status`` and
        ``store`` both read ``[-1]`` as "the current key".
        """
        return self._load()


def select_backend() -> LocalBackend:
    """Return the one key-store backend (the local versioned directory)."""
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
    subprocess.run(  # noqa: S603 -- current interpreter runs the repository-owned signer
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
    """Persist an existing key file as a new version, tagging it as it goes.

    Tags are computed here rather than by the backend, so a key stored locally
    and the same key stored in the vault carry identical fingerprint, algorithm
    and git_commit metadata -- which is what lets the two histories be compared.

    Exits (rather than returning non-zero) when the key file is absent: there
    is nothing partial to report.
    """
    backend = select_backend()
    key = Path(args.key)
    if not key.exists():
        sys.exit(f"key not found: {key}")
    version = backend.store(key.read_text(encoding="ascii"), _tags(key, args.note))
    print(f"stored {key} as version {version} in {backend.name}")
    return 0


def cmd_get(args: argparse.Namespace) -> int:
    """Recover a stored private key to a file or to stdout.

    ``--out`` writes 0600; without it the PEM goes to stdout, which is the
    pipe-friendly path and deliberately leaves protecting the key to the
    caller (redirecting it into a world-readable file is the caller's choice
    to make, not something this can detect).
    """
    backend = select_backend()
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
    """List every version in the active backend with its tags, oldest first.

    Reports only the backend actually selected -- it does not merge the local
    and vault histories, which are independent stores whose version numbers
    are not comparable. Pass ``--backend`` twice over to see both.
    """
    backend = select_backend()
    _print_history(backend.history())
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    """Print which backend auto-selection resolved to, and its newest version.

    The quickest way to answer "would a rekey right now write to the vault or
    to my local store?" -- worth checking first, since that decision depends
    on live vault reachability and so can differ between two runs on one
    machine.
    """
    backend = select_backend()
    rows = backend.history()
    print(f"backend: {backend.name}")
    if rows:
        latest = rows[-1]
        print(f"latest:  v{latest['version']}  {latest['fingerprint']}  {latest['created_at']}")
    else:
        print("latest:  (none stored)")
    return 0


def cmd_rekey(args: argparse.Namespace) -> int:
    """Generate a new signing key, preserving the outgoing one, and install it.

    Ordering is the safety property, and it is why this is one command rather
    than a documented sequence: the CURRENT working key is stored first, before
    anything is generated or overwritten. Every later step can fail without
    having destroyed a key, whereas generating first and backing up afterwards
    has a window in which the only copy of the old key is the file about to be
    overwritten.

    The new keypair is generated inside a temporary directory that is removed
    on exit, so the private key exists in exactly two places afterwards: the
    backend, and the working-key path.

    ``--patch`` additionally rewrites the public key baked into ra8_rot.c. That
    is a source change, and after it the device must be re-flashed -- the new
    public key is then the only one it will trust, so a device left on the old
    image will reject everything signed with the new key.
    """
    backend = select_backend()
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
            subprocess.run(  # noqa: S603 -- current interpreter runs the repository-owned patcher
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
    except (KeyStoreError, subprocess.CalledProcessError) as exc:
        sys.stderr.write(f"rot_keystore: {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
