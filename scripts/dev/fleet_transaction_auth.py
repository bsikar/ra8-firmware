# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Authenticate a mutating role payload against fleet and wrapper authority."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench_lock_capability as blc
import fleet_model as fm

ROLE_PREFIXES = {
    "dev_box": "dev_box_hil_runner_",
    "hil_bench": "hil_bench_",
}


class TransactionError(ValueError):
    """A role payload or controller capability was not fleet-authenticated."""


def expected_payload(host_name: str, role: str) -> dict[str, object]:
    """Return the exact fleet-derived policy values owned by one role."""
    data = fm.load()
    problems = fm.validate(data)
    if problems:
        raise TransactionError("fleet declaration is invalid: " + "; ".join(problems))
    host = data.get("hosts", {}).get(host_name)
    if not isinstance(host, dict) or host.get("class") != role:
        msg = f"{host_name!r} is not the declared {role} host"
        raise TransactionError(msg)
    prefix = ROLE_PREFIXES[role]
    payload = {
        key: value
        for key, value in fm.role_vars(data, host_name, host).items()
        if key.startswith(prefix)
    }
    if not payload:
        msg = f"fleet declaration produced no {role} policy payload"
        raise TransactionError(msg)
    return payload


def authenticate(
    host_name: str,
    role: str,
    raw_payload: str,
    capability: blc.Capability,
) -> None:
    """Require exact fleet policy plus the live controller wrapper process."""
    try:
        actual = json.loads(raw_payload)
    except json.JSONDecodeError as exc:
        msg = "role payload is not valid JSON"
        raise TransactionError(msg) from exc
    expected = expected_payload(host_name, role)
    if type(actual) is not dict or actual != expected:
        msg = "role payload differs from the immutable fleet declaration"
        raise TransactionError(msg)
    try:
        blc.authenticate(capability, fm.REPO_ROOT)
    except blc.CapabilityError as exc:
        msg = f"controller bench capability failed: {exc}"
        raise TransactionError(msg) from exc


def run_selftest() -> list[str]:
    """Prove each role authority class and the controller capability fail closed."""
    expected = {
        "dev_box_hil_runner_service": "ra8-hil-runner.service",
        "dev_box_hil_runner_root": "/opt/ra8-hil-runner",
        "dev_box_hil_runner_url": "https://example.invalid/runner.tgz",
        "dev_box_hil_runner_sha256": "a" * 64,
        "dev_box_hil_runner_bench_alias": "star",
    }
    capability = blc.Capability("0123456789abcdef", 41, 1000, "star.local")
    failures: list[str] = []
    module = sys.modules[__name__]
    with (
        mock.patch.object(module, "expected_payload", return_value=expected),
        mock.patch.object(blc, "authenticate", return_value=None),
    ):
        authenticate("dev", "dev_box", json.dumps(expected), capability)
        for key in expected:
            changed = dict(expected)
            changed[key] = "attacker-controlled"
            try:
                authenticate("dev", "dev_box", json.dumps(changed), capability)
            except TransactionError:
                pass
            else:
                failures.append(f"role payload override escaped: {key}")
    with (
        mock.patch.object(module, "expected_payload", return_value=expected),
        mock.patch.object(blc, "authenticate", side_effect=blc.CapabilityError("forged")),
    ):
        try:
            authenticate("dev", "dev_box", json.dumps(expected), capability)
        except TransactionError:
            pass
        else:
            failures.append("direct role payload passed without controller capability")
    return failures


def main() -> int:
    """Parse one Ansible-local authentication request."""
    if sys.argv[1:] == ["--selftest"]:
        failures = run_selftest()
        for failure in failures:
            print(f"fleet-transaction-auth selftest: {failure}", file=sys.stderr)
        return 1 if failures else 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("role", choices=tuple(ROLE_PREFIXES))
    parser.add_argument("payload_json")
    parser.add_argument("lock_id")
    parser.add_argument("holder_pid", type=int)
    parser.add_argument("holder_start_ticks", type=int)
    parser.add_argument("holder_target")
    args = parser.parse_args()
    capability = blc.Capability(
        args.lock_id,
        args.holder_pid,
        args.holder_start_ticks,
        args.holder_target,
    )
    try:
        authenticate(args.host, args.role, args.payload_json, capability)
    except (OSError, TransactionError) as exc:
        print(f"fleet-transaction-auth: {exc}", file=sys.stderr)
        return 3
    print("fleet-transaction-auth: authenticated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
