#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: ``infra/fleet.yml`` describes a fleet that could actually be built.

The declaration is the single registry of what machines this project runs on
and how much of each one CI may use, so an error in it is an error in the
estate. This checks three things a green Ansible run would not:

1. **The declaration is internally sound.** Every rule in
   :func:`fleet_model.validate` -- classes and plays that exist, capacity that
   fits the declared budget, per-instance floors, a parseable quiet-hours
   window, and an instance count that is either the sizing formula's or comes
   with a written reason. A number nobody can re-derive is folklore.

2. **Nothing tunes a host twice.** A committed ``host_vars`` file may not
   re-declare a variable the declaration owns. Extra-vars beat ``host_vars``,
   so a duplicate would not change behaviour -- it would leave a number in the
   tree that looks authoritative, that somebody will edit, and that will have
   no effect.

3. **The derived variables land somewhere real.** Every ``fleet_capacity_*``
   name the mapping emits must exist in that role's defaults. A mapping keyed
   on a spelling no role reads is the same defect as a checker rule keyed on a
   string no macro produces: it matches nothing and reports success forever.

``--selftest`` runs first in the gate and asserts each rule fires on a
deliberately broken declaration and stays quiet on a legal one. Without it,
"0 problems" is indistinguishable from "checked nothing".
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from copy import deepcopy
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "dev"))

import fleet_model as fm  # noqa: E402 -- sibling tool, path set immediately above

CAPACITY_DEFAULTS = fm.ANSIBLE_DIR / "roles" / "fleet_capacity" / "defaults" / "main.yml"


def _check_capacity_vars() -> list[str]:
    """Every ``fleet_capacity_*`` variable the mapping emits exists in the role.

    Returns:
        One message per name the role would never read.
    """
    declared = yaml.safe_load(CAPACITY_DEFAULTS.read_text(encoding="utf-8")) or {}
    data = fm.load()
    emitted: set[str] = set()
    for name, host in data["hosts"].items():
        emitted |= {k for k in fm.role_vars(name, host) if k.startswith("fleet_capacity_")}
    return [
        f"fleet.py emits '{key}', which is in no fleet_capacity default -- the role "
        "would never read it, so whatever it configures would silently not happen"
        for key in sorted(emitted - set(declared))
    ]


def _good_declaration() -> dict[str, Any]:
    """A minimal legal declaration for the selftest to mutate.

    Returns:
        A one-host fleet that satisfies every rule.
    """
    return {
        "sizing": {"build_parallelism": 4, "memory_per_instance_gb": 8},
        "hosts": {
            "nas": {
                "class": "docker_linux",
                "connect": {"ssh": "nas"},
                "provisions": ["ci-runner-docker"],
                "runners": {
                    "instances": 2,
                    "cpus": 4,
                    "memory_gb": 8,
                    "labels": ["ra8-ci"],
                },
                "budget": {"mode": "reserved", "threads": 8, "memory_gb": 16},
            }
        },
    }


# name -> a mutation that must produce at least one problem. Each is a rule
# this gate claims to enforce; a rule with no row here is a rule nothing proves
# still fires.
def _mutations() -> dict[str, Any]:
    """The broken declarations the selftest asserts are rejected.

    Returns:
        Rule name to a function that damages a good declaration.
    """
    return {
        "unknown class": lambda d: d["hosts"]["nas"].update(class_="x") or _set(d, "class", "nope"),
        "no connect.ssh": lambda d: d["hosts"]["nas"]["connect"].clear(),
        "unknown play": lambda d: d["hosts"]["nas"].update(provisions=["not-a-play"]),
        "wrong budget mode": lambda d: d["hosts"]["nas"]["budget"].update(mode="burst"),
        "capacity over budget": lambda d: d["hosts"]["nas"]["runners"].update(instances=4),
        "instance under the CPU floor": lambda d: d["hosts"]["nas"]["runners"].update(cpus=2),
        "instance under the memory floor": lambda d: d["hosts"]["nas"]["runners"].update(
            memory_gb=4
        ),
        "unexplained instance count": lambda d: d["hosts"]["nas"]["runners"].update(instances=1),
        "no labels": lambda d: d["hosts"]["nas"]["runners"].update(labels=[]),
        "bad quiet window": lambda d: d["hosts"]["nas"].update(
            quiet_hours={"window": "evening", "days": "Fri", "instances": 0}
        ),
        "bad quiet day": lambda d: d["hosts"]["nas"].update(
            quiet_hours={"window": "18:00-23:00", "days": "Funday", "instances": 0}
        ),
        "quiet target is not a reduction": lambda d: d["hosts"]["nas"].update(
            quiet_hours={"window": "18:00-23:00", "days": "Fri", "instances": 2}
        ),
        "capacity on a non-runner class": lambda d: d["hosts"].update(
            {
                "box": {
                    "class": "dev_box",
                    "connect": {"ssh": "box"},
                    "provisions": ["dev-box"],
                    "runners": {"instances": 1},
                }
            }
        ),
        "bad sizing constant": lambda d: d["sizing"].update(build_parallelism=0),
    }


def _set(data: dict[str, Any], key: str, value: object) -> None:
    """Set a key on the selftest's single host.

    Args:
        data: The declaration being damaged.
        key: Key to set.
        value: Value to set it to. Deliberately ``object``: the point of a
            mutation is to write something the schema does not expect.
    """
    data["hosts"]["nas"][key] = value


def _selftest() -> int:
    """Assert every rule fires on a broken fleet and none fires on a legal one.

    Returns:
        0 when the checker demonstrably still has teeth, 1 otherwise.
    """
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        empty = Path(tmp)
        if fm.validate(_good_declaration(), host_vars_dir=empty):
            failures.append("  a legal declaration was rejected")
        for rule, damage in _mutations().items():
            broken = deepcopy(_good_declaration())
            damage(broken)
            if not fm.validate(broken, host_vars_dir=empty):
                failures.append(f"  rule not enforced: {rule}")
        good = _good_declaration()
        (empty / "nas.yml").write_text("ci_runner_docker_cpus: '9'\n", encoding="utf-8")
        if not fm.validate(good, host_vars_dir=empty):
            failures.append("  a host_vars file re-declaring a fleet-owned knob was accepted")
    if failures:
        print("check_fleet_declaration selftest FAILED:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"selftest OK: {len(_mutations())} rules fire, a legal declaration passes")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Entry point.

    Args:
        argv: Command line, defaulting to ``sys.argv[1:]``.

    Returns:
        0 when the declaration is sound, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="prove the rules still fire")
    args = parser.parse_args(argv)
    if args.selftest:
        return _selftest()
    try:
        data = fm.load()
    except fm.FleetError as exc:
        print(f"check_fleet_declaration: {exc}", file=sys.stderr)
        return 1
    problems = fm.validate(data) + _check_capacity_vars()
    if problems:
        print(f"infra/fleet.yml: {len(problems)} problem(s):", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    runners = sum(int((h.get("runners") or {}).get("instances", 0)) for h in data["hosts"].values())
    print(f"infra/fleet.yml OK: {len(data['hosts'])} host(s), {runners} runner instance(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
