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
   and ``dev_slice_*`` name the mapping emits must exist in that role's
   defaults. A mapping keyed on a spelling no role reads is the same defect as
   a checker rule keyed on a string no macro produces: it matches nothing and
   reports success forever.

4. **The names both halves must agree on do agree.** The model predicts the dev
   slice's unit name (``fleet.py`` passes it to the capacity script) and the
   role creates it. Two spellings would give the host a quiet-hours window that
   freezes a slice nothing ever made -- a schedule that stands nothing down.

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

# Roles whose variables the mapping derives, keyed by the prefix it emits them
# under. Every emitted name must exist in the role's defaults, or the role
# would never read it and whatever it configures would silently not happen.
DERIVED_ROLES = {"fleet_capacity_": "fleet_capacity", "dev_slice_": "dev_slice"}


def _role_defaults(role: str) -> dict[str, Any]:
    """Read one role's declared defaults.

    Args:
        role: Role directory name under ``infra/ansible/roles``.

    Returns:
        The parsed ``defaults/main.yml`` mapping.
    """
    path = fm.ANSIBLE_DIR / "roles" / role / "defaults" / "main.yml"
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def _check_derived_vars() -> list[str]:
    """Every variable the mapping emits exists in the role that consumes it.

    Returns:
        One message per name the role would never read.
    """
    data = fm.load()
    emitted: set[str] = set()
    for name, host in data["hosts"].items():
        emitted |= set(fm.role_vars(name, host))
    problems = []
    for prefix, role in DERIVED_ROLES.items():
        declared = set(_role_defaults(role))
        problems += [
            f"fleet.py emits '{key}', which is in no {role} default -- the role "
            "would never read it, so whatever it configures would silently not happen"
            for key in sorted({k for k in emitted if k.startswith(prefix)} - declared)
        ]
    return problems


def _check_shared_constants() -> list[str]:
    """The names the model and a role BOTH have to know are the same name.

    ``fleet_model`` predicts the dev slice's unit name so ``fleet.py`` can pass
    it to the capacity script, and the ``dev_slice`` role creates it. Two
    spellings would produce a quiet-hours timer that freezes a slice nothing
    ever made -- a window that silently stands nothing down.

    Returns:
        One message per disagreement.
    """
    role_unit = _role_defaults("dev_slice").get("dev_slice_unit")
    if role_unit == fm.DEV_SLICE_UNIT:
        return []
    return [
        f"fleet_model.DEV_SLICE_UNIT is '{fm.DEV_SLICE_UNIT}' but the dev_slice role "
        f"creates '{role_unit}'. fleet.py passes the first to the capacity script and "
        "the role creates the second, so quiet hours would freeze a slice that does "
        "not exist and dev work would keep the machine through the owner's window."
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
        # --- the lent dev slice ---------------------------------------------
        "dev slice not weighted below CI": lambda d: _lend(d, cpu_weight=100),
        "dev slice weight out of range": lambda d: _lend(d, cpu_weight=0),
        "dev slice memory over what the runners leave": lambda d: _lend(d, memory_gb=8),
        "dev slice swap the host does not have": lambda d: _lend(d, swap_gb=4),
        "dev slice with no parallel bound": lambda d: _lend(d, max_jobs=0),
        "dev slice missing a required key": lambda d: d["hosts"]["nas"].update(
            dev_slice={"cpu_weight": 10, "memory_gb": 4}
        ),
        "dev slice on a class that runs none": lambda d: d["hosts"].update(
            {
                "box": {
                    "class": "dev_box",
                    "connect": {"ssh": "box"},
                    "provisions": ["dev-box"],
                    "dev_slice": {"cpu_weight": 10, "memory_gb": 4, "max_jobs": 4},
                }
            }
        ),
    }


def _lend(data: dict[str, Any], **override: int) -> None:
    """Give the selftest's host a dev slice, with one field made wrong.

    The base block is legal on the fixture host -- 2 runners x 8 GB of a 16 GB
    budget leaves nothing, so the memory field is what has to give: the fixture
    lends 0 GB is not legal either, hence the budget bump. Each caller then
    breaks exactly one field, so a rule that stopped firing is attributable.

    Args:
        data: The declaration being damaged.
        override: The one field to set to an illegal value.
    """
    data["hosts"]["nas"]["budget"]["memory_gb"] = 20
    data["hosts"]["nas"]["budget"]["swap_gb"] = 2
    data["hosts"]["nas"]["sizing_note"] = "fixture: budget raised to leave room to lend"
    slice_: dict[str, int] = {"cpu_weight": 10, "memory_gb": 4, "swap_gb": 2, "max_jobs": 4}
    slice_.update(override)
    data["hosts"]["nas"]["dev_slice"] = slice_


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
        # A validator that rejected EVERY dev slice would pass every mutation
        # below while making the feature unusable, so the legal shape is
        # asserted too -- the same reason the legal declaration above is.
        legal_lend = _good_declaration()
        _lend(legal_lend)
        if fm.validate(legal_lend, host_vars_dir=empty):
            failures.append("  a legal dev_slice was rejected")
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
    problems = fm.validate(data) + _check_derived_vars() + _check_shared_constants()
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
