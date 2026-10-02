#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: ``infra/fleet.yml`` describes a fleet that could actually be built.

The declaration is the single registry of what machines this project runs on,
so an error in it is an error in the estate. This checks three things a green Ansible run would not:

1. **The declaration is internally sound.** Every rule in
   :func:`fleet_model.validate` -- classes and plays that exist, an address any
   machine could reach the host at, a sound HIL listener and bench, and no
   leftover runner capacity (the fleet has no runner pool). A host addressed
   by an ssh alias is reachable only from whichever laptop defines it.

2. **Nothing tunes a host twice.** A committed ``host_vars`` file may not
   re-declare a variable the declaration owns. Extra-vars beat ``host_vars``,
   so a duplicate would not change behaviour -- it would leave a number in the
   tree that looks authoritative, that somebody will edit, and that will have
   no effect.

3. **The derived variables land somewhere real.** Every ``dev_box_hil_runner_*``
   and ``hil_bench_*`` name the mapping emits must exist in that role's defaults. A mapping keyed
   on a spelling no role reads is the same defect as a checker rule keyed on a
   string no macro produces: it matches nothing and reports success forever.

4. **No command the tooling builds needs an ssh alias.** Rule 1 checks the
   INPUT; this checks the derivation, by walking the real ssh argv and the real
   inventory line for every host and failing on any destination or ProxyJump
   hop that is a bare label. A future ``-J <fleet name>`` would pass every
   input rule and still only work on a machine that happened to define that
   name -- which is the whole SSH-alias bug, one layer down.

6. **The cache-only HIL repair stays cache-only.** Its standalone playbook,
   private inventory driver and isolated Justfile must match one exact
   execution document. The path and identity are literals, and inventory
   variables may not override the corresponding full-role safety defaults.

``--selftest`` runs first in the gate and asserts each rule fires on a
deliberately broken declaration and stays quiet on a legal one. Without it,
"0 problems" is indistinguishable from "checked nothing".
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from copy import deepcopy
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "scripts" / "dev"))

import fleet_model as fm  # noqa: E402 -- sibling tool, path set immediately above
import fleet_reach as fr  # noqa: E402 -- ditto; the reachability half of the model
import hil_cache_repair_rules as hctr  # noqa: E402 -- checker helper beside this script

# Roles whose variables the mapping derives, keyed by the prefix it emits them
# under. Every emitted name must exist in the role's defaults, or the role
# would never read it and whatever it configures would silently not happen.
DERIVED_ROLES = {
    "dev_box_hil_runner_": "dev_box",
    "hil_bench_": "hil_bench",
}

HIL_SERVICE_TEMPLATE = "infra/ansible/roles/dev_box/templates/ra8-hil-runner.service.j2"
HIL_SERVICE_REQUIRED = frozenset(
    {
        "[Service]",
        "User={{ dev_box_hil_runner_user }}",
        "Group={{ dev_box_hil_runner_group }}",
        "WorkingDirectory={{ dev_box_hil_runner_root }}",
        "EnvironmentFile={{ dev_box_hil_runner_env_file }}",
        'Environment="HOME={{ dev_box_hil_runner_home }}"',
        "ExecStart={{ dev_box_hil_runner_root }}/runsvc.sh",
        "NoNewPrivileges=true",
        "PrivateDevices=true",
        "PrivateTmp=true",
        "ProtectHome=true",
        "ProtectSystem=full",
        "ProtectControlGroups=true",
        "ProtectKernelModules=true",
        "ProtectKernelTunables=true",
        "RestrictSUIDSGID=true",
    }
)
HIL_SERVICE_SINGLETONS = (
    "User=",
    "Group=",
    "WorkingDirectory=",
    "EnvironmentFile=",
    "ExecStart=",
)
JINJA_VARIABLE = re.compile(r"{{\s*([A-Za-z_][A-Za-z0-9_]*)\s*}}")
LINT_PROVIDER_INPUTS = (HIL_SERVICE_TEMPLATE,)


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
        emitted |= set(fm.role_vars(data, name, host))
    problems = []
    for prefix, role in DERIVED_ROLES.items():
        declared = set(_role_defaults(role))
        problems += [
            f"fleet.py emits '{key}', which is in no {role} default -- the role "
            "would never read it, so whatever it configures would silently not happen"
            for key in sorted({k for k in emitted if k.startswith(prefix)} - declared)
        ]
    return problems


def _check_hil_service_template(
    repo_root: Path = REPO_ROOT, declared_vars: set[str] | None = None
) -> list[str]:
    """Validate the exact privileged systemd/Jinja input owned by this gate."""
    path = repo_root / HIL_SERVICE_TEMPLATE
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        return [f"{HIL_SERVICE_TEMPLATE}: cannot read template: {exc}"]
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    problems = [
        f"{HIL_SERVICE_TEMPLATE}: missing required service contract {line!r}"
        for line in sorted(HIL_SERVICE_REQUIRED - set(lines))
    ]
    problems.extend(
        f"{HIL_SERVICE_TEMPLATE}: {prefix} must occur exactly once"
        for prefix in HIL_SERVICE_SINGLETONS
        if sum(line.startswith(prefix) for line in lines) != 1
    )
    declared = declared_vars if declared_vars is not None else set(_role_defaults("dev_box"))
    unknown = sorted(set(JINJA_VARIABLE.findall(text)) - declared)
    if unknown:
        problems.append(f"{HIL_SERVICE_TEMPLATE}: undeclared Jinja variable(s): {unknown!r}")
    if "TAPO" in text.upper():
        problems.append(f"{HIL_SERVICE_TEMPLATE}: unrelated TAPO credentials must never be exposed")
    return problems


def _is_literal(destination: str) -> bool:
    """Whether an ssh destination is an address rather than a config alias.

    Args:
        destination: ``[user@]address`` as it would appear on an ssh command
            line.

    Returns:
        True when it carries a dot or a colon, i.e. an IPv4/IPv6 literal or a
        qualified name; False for a bare label, which only resolves through
        somebody's ``~/.ssh/config``.
    """
    address = destination.rpartition("@")[2]
    return "." in address or ":" in address


def _inventory_destinations(entry: str) -> list[str]:
    """Every host address one generated inventory line hands to Ansible.

    Args:
        entry: One line of the generated inventory.

    Returns:
        The ``ansible_host`` value plus every ``ProxyJump`` hop, empty for a
        ``connection=local`` host, which Ansible never dials.
    """
    out = []
    for field in ("ansible_host=", "-o ProxyJump="):
        _, found, tail = entry.partition(field)
        if not found:
            continue
        value = tail.split("'")[0].split()[0]
        out += value.split(",")
    return out


def _check_derived_reach(data: dict[str, Any]) -> list[str]:
    """No ssh command or inventory line the model builds names an alias.

    Args:
        data: The parsed declaration. The selftest hands it one whose
            derivation is broken, so a detector that stopped matching cannot
            report the real fleet clean forever.

    Returns:
        One message per derived destination that is a bare label.
    """
    problems = []
    for name in data["hosts"]:
        argv = fr.ssh_target(data, name)
        derived = {
            "the ssh command this tooling builds": [
                argv[-1],
                *fr.jump_chain(data, name),
            ],
            "the generated Ansible inventory": _inventory_destinations(
                fm.inventory_entry(data, name)
            ),
        }
        problems += [
            f"{name}: {what} dials '{token}', a bare label rather than an address. It "
            "would resolve only on a machine whose ~/.ssh/config happened to define it, "
            "which is exactly the SSH-alias fault removed -- one layer further down."
            for what, tokens in derived.items()
            for token in tokens
            if not _is_literal(token)
        ]
    return problems


def _good_hosts() -> dict[str, Any]:
    """Return the minimal legal host mapping used by the selftest."""
    return {
        "builder": {
            "class": "k8s_node",
            "connect": {"address": "10.0.0.3", "user": "builder"},
            "provisions": ["k3s-node"],
        },
        "dev": {
            "class": "dev_box",
            "connect": {"address": "10.0.0.4", "user": "developer"},
            "provisions": ["dev-box"],
            "hil_runner": {
                "name": "dev-hil",
                "repository": "https://github.com/example/firmware",
                "labels": ["hil", "ra8d2"],
                "bench": {"host": "bench", "aliases": ["bench.local"]},
            },
        },
        "bench": {
            "class": "hil_bench",
            "connect": {"address": "10.0.0.9", "user": "pi"},
            "provisions": ["hil-bench"],
            "board_interface": {
                "name": "eth0",
                "mac": "02:00:00:00:00:09",
                "sysfs_device": "/sys/devices/platform/bench-ethernet",
                "phc_index": 0,
            },
        },
    }


def _good_declaration() -> dict[str, Any]:
    """A minimal legal declaration for the selftest to mutate.

    Returns:
        A one-host fleet that satisfies every rule.
    """
    return {"hosts": _good_hosts()}


# name -> a mutation that must produce at least one problem. Each is a rule
# this gate claims to enforce; a rule with no row here is a rule nothing proves
# still fires. Grouped into the three families the validator has, so a family
# that stopped firing is attributable at a glance.
def _mutations() -> dict[str, Any]:
    """The broken declarations the selftest asserts are rejected.

    Returns:
        Rule name to a function that damages a good declaration.
    """
    return {
        **_reach_mutations(),
        **_capacity_mutations(),
        **_hil_listener_mutations(),
        **_hil_interface_mutations(),
    }


def _hil_listener_mutations() -> dict[str, Any]:
    """Return mutations of the listener-to-bench relationship."""
    return {
        "HIL listener on wrong class": lambda d: d["hosts"]["dev"].update(**{"class": "hil_bench"}),
        "HIL listener with no name": lambda d: d["hosts"]["dev"]["hil_runner"].update(name=""),
        "HIL listener with no labels": lambda d: d["hosts"]["dev"]["hil_runner"].update(labels=[]),
        "HIL listener with a non-string label": lambda d: d["hosts"]["dev"]["hil_runner"].update(
            labels=["hil", 8]
        ),
        "HIL listener with implicit label repeated": lambda d: d["hosts"]["dev"][
            "hil_runner"
        ].update(labels=["self-hosted", "hil"]),
        "HIL listener with malformed repository": lambda d: d["hosts"]["dev"]["hil_runner"].update(
            repository="owner/repo"
        ),
        "HIL listener with unknown bench": lambda d: d["hosts"]["dev"]["hil_runner"][
            "bench"
        ].update(host="missing"),
        "HIL listener targeting non-bench host": lambda d: d["hosts"]["dev"]["hil_runner"][
            "bench"
        ].update(host="builder"),
        "HIL listener with malformed bench aliases": lambda d: d["hosts"]["dev"]["hil_runner"][
            "bench"
        ].update(aliases="bench.local"),
        "duplicate HIL registration name": lambda d: _duplicate_hil(
            d, repository="https://github.com/example/firmware-second"
        ),
    }


def _hil_interface_mutations() -> dict[str, Any]:
    """Return mutations of the permanent board-interface identity."""
    return {
        "HIL bench with missing board interface": lambda d: d["hosts"]["bench"].pop(
            "board_interface"
        ),
        "HIL bench with virtual board interface": lambda d: d["hosts"]["bench"].update(
            board_interface={
                "name": "eth0.42",
                "mac": "02:00:00:00:00:09",
                "sysfs_device": "/sys/devices/platform/bench-ethernet",
                "phc_index": 0,
            }
        ),
        "HIL bench with malformed permanent MAC": lambda d: d["hosts"]["bench"][
            "board_interface"
        ].update(mac="not-a-mac"),
        "HIL bench with unsafe sysfs identity": lambda d: d["hosts"]["bench"][
            "board_interface"
        ].update(sysfs_device="/sys/devices/../escape"),
        "HIL bench with invalid PHC identity": lambda d: d["hosts"]["bench"][
            "board_interface"
        ].update(phc_index=-1),
    }


def _duplicate_hil(data: dict[str, Any], **override: str) -> None:
    """Add a second legal dev-box shape sharing one listener identity field.

    Args:
        data: Declaration being damaged.
        override: Unique field used to leave exactly one duplicate behind.
    """
    duplicate = deepcopy(data["hosts"]["dev"])
    duplicate["connect"]["address"] = "10.0.0.5"
    duplicate["hil_runner"].update(override)
    data["hosts"]["dev-second"] = duplicate


def _reach_mutations() -> dict[str, Any]:
    """Breakages in how a machine is declared and reached.

    Returns:
        Rule name to a function that damages a good declaration.
    """
    return {
        "unknown class": lambda d: (
            d["hosts"]["builder"].update(class_="x") or _set(d, "class", "nope")
        ),
        "unknown play": lambda d: d["hosts"]["builder"].update(provisions=["not-a-play"]),
        "no connect.address": lambda d: d["hosts"]["builder"]["connect"].clear(),
        # THE regression guard. A bare label is an ~/.ssh/config alias, and a
        # fleet addressed by aliases is drivable only from whichever machine
        # defines them -- which is how a runner host once sat at half capacity with
        # nothing able to converge it back.
        "address is an ssh alias": lambda d: d["hosts"]["builder"]["connect"].update(
            address="builder"
        ),
        "address carries the login user": lambda d: d["hosts"]["builder"]["connect"].update(
            address="builder@10.0.0.3"
        ),
        "address with whitespace in it": lambda d: d["hosts"]["builder"]["connect"].update(
            address="10.0.0.3 "
        ),
        "jump is not a declared host": lambda d: d["hosts"]["builder"]["connect"].update(
            jump="bastion"
        ),
        "jump chain revisits a host": lambda d: d["hosts"]["builder"]["connect"].update(
            jump="builder"
        ),
    }


def _capacity_mutations() -> dict[str, Any]:
    """Runner capacity on any host: the fleet has no runner pool left.

    Returns:
        Rule name to a function that damages a good declaration.
    """
    return {
        "runners on the k3s node": lambda d: d["hosts"]["builder"].update(runners={"instances": 1}),
        "budget on the dev box": lambda d: d["hosts"]["dev"].update(
            budget={"threads": 4, "memory_gb": 8}
        ),
        "quiet hours on the k3s node": lambda d: d["hosts"]["builder"].update(
            quiet_hours={"window": "18:00-23:00", "days": "Fri", "instances": 0}
        ),
    }


def _set(data: dict[str, Any], key: str, value: object) -> None:
    """Set a key on the selftest's single host.

    Args:
        data: The declaration being damaged.
        key: Key to set.
        value: Value to set it to. Deliberately ``object``: the point of a
            mutation is to write something the schema does not expect.
    """
    data["hosts"]["builder"][key] = value


def _jumped_declaration() -> dict[str, Any]:
    """A legal two-host fleet where one machine is reached through the other.

    Returns:
        The good declaration plus a bench the builder is reached through.
    """
    data = _good_declaration()
    data["hosts"]["builder"]["connect"]["jump"] = "bench"
    return data


def _check_jump_resolves(host_vars_dir: Path) -> list[str]:
    """A declared hop must reach the ssh command line as an ADDRESS.

    The mutation table proves a bad hop is rejected; this proves a good one is
    honoured, and honoured as a literal. ``-J bench`` would satisfy every input
    rule and still only work on a machine that defined that alias -- the same
    defect the addresses themselves had.

    Args:
        host_vars_dir: Empty fixture directory for the validator.

    Returns:
        One message per way the hop failed to reach the command line.
    """
    data = _jumped_declaration()
    problems = [f"  a legal ProxyJump was rejected: {p}" for p in fm.validate(data, host_vars_dir)]
    argv = fr.ssh_target(data, "builder")
    if "-J" not in argv:
        problems.append("  a declared connect.jump produced no -J on the ssh command line")
    elif argv[argv.index("-J") + 1] != "pi@10.0.0.9":
        hop = argv[argv.index("-J") + 1]
        problems.append(f"  the ProxyJump hop is '{hop}', not the hop host's address")
    if "ProxyJump=pi@10.0.0.9" not in fm.inventory_entry(data, "builder"):
        problems.append("  the generated inventory does not hand Ansible the ProxyJump hop")
    if "ProxyJump bench" not in fr.render_ssh_config(data):
        problems.append("  the generated ssh config does not carry the hop")
    return problems


def _check_hil_service_selftest(root: Path) -> list[str]:
    """Prove the managed systemd template contract accepts and rejects."""
    failures: list[str] = []
    template = root / HIL_SERVICE_TEMPLATE
    template.parent.mkdir(parents=True, exist_ok=True)
    good = "\n".join(sorted(HIL_SERVICE_REQUIRED)) + "\n"
    template.write_text(good, encoding="utf-8")
    declared = set(JINJA_VARIABLE.findall(good))
    if _check_hil_service_template(root, declared):
        failures.append("  the hardened HIL systemd template was rejected")
    template.write_text(good.replace("NoNewPrivileges=true\n", ""), encoding="utf-8")
    if not _check_hil_service_template(root, declared):
        failures.append("  a HIL systemd template missing its sandbox was accepted")
    template.write_text(good + "Environment={{ undeclared_secret }}\n", encoding="utf-8")
    if not _check_hil_service_template(root, declared):
        failures.append("  an undeclared HIL service variable was accepted")
    return failures


def _selftest() -> int:
    """Assert every rule fires on a broken fleet and none fires on a legal one.

    Returns:
        0 when the checker demonstrably still has teeth, 1 otherwise.
    """
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        empty = Path(tmp)
        good_declaration = _good_declaration()
        if fm.validate(good_declaration, host_vars_dir=empty):
            failures.append("  a legal declaration was rejected")
        failures += _check_hil_service_selftest(empty)
        failures += hctr.selftest(REPO_ROOT)
        failures += _check_jump_resolves(empty)
        if _check_derived_reach(_jumped_declaration()):
            failures.append("  a fleet reachable only by address was reported unreachable")
        # Both directions for the derivation check itself: an alias in the
        # declaration must come out the far end as an alias on a command line,
        # or the check is decoration.
        aliased = _jumped_declaration()
        aliased["hosts"]["bench"]["connect"]["address"] = "bench"
        if not _check_derived_reach(aliased):
            failures.append("  an ssh alias survived into a derived ssh command unreported")
        for rule, damage in _mutations().items():
            broken = deepcopy(_good_declaration())
            damage(broken)
            if not fm.validate(broken, host_vars_dir=empty):
                failures.append(f"  rule not enforced: {rule}")
        good = _good_declaration()
        (empty / "dev.yml").write_text("hil_bench_eth_iface: eth9\n", encoding="utf-8")
        if not fm.validate(good, host_vars_dir=empty):
            failures.append("  a host_vars file re-declaring a fleet-owned knob was accepted")
    failures.extend(_selftest_authority_errors())
    if failures:
        print("check_fleet_declaration selftest FAILED:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"selftest OK: {len(_mutations())} rules fire, a legal declaration and "
        "the standalone cache-only HIL execution contract passes"
    )
    return 0


def _selftest_authority_errors() -> list[str]:
    """Return failures in lint-provider versus policy-ownership boundaries."""
    failures = []
    if LINT_PROVIDER_INPUTS != (HIL_SERVICE_TEMPLATE,):
        failures.append("  --list-files no longer reports only its semantic template input")
    if len(LINT_PROVIDER_INPUTS) != len(set(LINT_PROVIDER_INPUTS)):
        failures.append("  --list-files reports duplicate semantic template inputs")
    if len(hctr.policy_input_files(REPO_ROOT)) <= len(LINT_PROVIDER_INPUTS):
        failures.append("  authored-file ownership census collapsed into lint provider inputs")
    return failures


def main(argv: list[str] | None = None) -> int:
    """Entry point.

    Args:
        argv: Command line, defaulting to ``sys.argv[1:]``.

    Returns:
        0 when the declaration is sound, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="prove the rules still fire")
    parser.add_argument("--list-files", action="store_true", help="list exact template inputs")
    args = parser.parse_args(argv)
    if args.list_files:
        print(*LINT_PROVIDER_INPUTS, sep="\n")
        return 0
    if args.selftest:
        return _selftest()
    try:
        data = fm.load()
    except fm.FleetError as exc:
        print(f"check_fleet_declaration: {exc}", file=sys.stderr)
        return 1
    problems = (
        fm.validate(data)
        + _check_derived_vars()
        + _check_hil_service_template()
        + hctr.check(REPO_ROOT, data)
        + _check_derived_reach(data)
    )
    if problems:
        print(f"infra/fleet.yml: {len(problems)} problem(s):", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    native_hil = sum(1 for host in data["hosts"].values() if host.get("hil_runner"))
    print(f"infra/fleet.yml OK: {len(data['hosts'])} host(s), {native_hil} native HIL listener(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
