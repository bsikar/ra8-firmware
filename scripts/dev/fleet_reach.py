#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""How a control node REACHES the machines ``infra/fleet.yml`` declares.

One responsibility, and it is the one the fleet was missing. Every host used to
be addressed by a bare ssh alias -- ``ssh: truenas``, ``ssh: star`` -- which
resolves only through one machine's private ``~/.ssh/config``. The two things a
control node needs were then split so that neither half had both: the Mac had
the aliases and no ansible, the dev box had ansible and could resolve none of
them, so ``fleet.py status truenas`` from the dev box died on
*Could not resolve hostname truenas* while the machine answered fine on
``10.10.10.1``. A naming gap, not a routing one -- and it cost a NAS running at
half its declared capacity with nothing able to converge it back (#526).

So a host declares an ADDRESS (an IP or a name a resolver can answer), a login
``user``, and an optional ``jump`` naming another host in the same file. This
module turns those three into everything that has to dial a machine:

* the ``ssh`` argv every command in :mod:`fleet` uses -- literals only, so it
  works on a machine whose ``~/.ssh/config`` is empty,
* the ``ProxyJump`` chain, resolved through the declaration rather than through
  an alias the hop happens to have somewhere,
* the generated ``~/.ssh`` fragment, so the friendly names exist on any control
  node by GENERATION rather than by hand-copying,
* the rules that keep an alias from creeping back into the declaration.

It is imported by :mod:`fleet_model`, and through it by both front doors -- the
``fleet`` CLI and the ``fleet-declaration`` gate -- so there is still exactly
one definition of how a machine is reached.
"""

from __future__ import annotations

from typing import Any

# The generated SSH config fragment, and the one line that pulls it in.
#
# Hand-writing the aliases onto every control node would have moved the same
# per-machine prerequisite one level down and rotted the same way, so the
# fragment is GENERATED from the declaration and installed with one command.
#
# Relative, not absolute: ssh_config(5) resolves an Include path without a
# leading slash against ~/.ssh, so the same line works for every user on every
# machine.
SSH_FRAGMENT_NAME = "ra8-fleet.config"
SSH_INCLUDE_LINE = f"Include {SSH_FRAGMENT_NAME}"

# ssh options every fleet connection carries.
#
# BatchMode makes an unreachable host an error instead of a password prompt
# that hangs an unattended converge. accept-new is trust-on-first-use: it pins
# a host key the first time and still FAILS on a key that later changes, which
# is what lets a fresh control node work without a hand-seeded known_hosts --
# the last per-machine prerequisite after the aliases. It is strictly stronger
# than the fleet's Ansible transport, which sets host_key_checking = False.
SSH_OPTIONS = (
    "-o",
    "ConnectTimeout=15",
    "-o",
    "BatchMode=yes",
    "-o",
    "StrictHostKeyChecking=accept-new",
)


def ssh_destination(host: dict[str, Any]) -> str:
    """The ``[user@]address`` a host is reached at, from anywhere.

    The address is a literal -- an IP or a DNS name that resolves off the
    machine's own resolver -- never an ``~/.ssh/config`` alias, which is a fact
    about one laptop rather than about the fleet. :func:`check_connect`
    enforces that.

    Args:
        host: One host's declaration.

    Returns:
        ``user@address`` when a login user is declared, ``address`` otherwise.
    """
    connect = host["connect"]
    address = str(connect["address"])
    user = connect.get("user")
    return f"{user}@{address}" if user else address


def jump_chain(data: dict[str, Any], name: str) -> list[str]:
    """The ProxyJump hops that reach a host, outermost first.

    ``connect.jump`` names another host IN THIS FLEET rather than an alias, so
    a hop is resolved to a literal destination here exactly as the target is.
    Hops chain: a host behind a host behind a bastion yields both, in the order
    ``ssh -J`` connects to them.

    Args:
        data: The parsed declaration.
        name: Fleet host name.

    Returns:
        One ``[user@]address`` per hop, empty when the host is reached direct.
    """
    hosts = data["hosts"]
    chain: list[str] = []
    seen = {name}
    cursor = (hosts[name].get("connect") or {}).get("jump")
    # Bounded by the fleet size, and a repeat means a cycle the validator
    # reports: a loop here would hang every command that reaches a host.
    for _ in range(len(hosts)):
        if not cursor or cursor in seen or cursor not in hosts:
            break
        seen.add(str(cursor))
        chain.append(ssh_destination(hosts[cursor]))
        cursor = (hosts[cursor].get("connect") or {}).get("jump")
    chain.reverse()
    return chain


def ssh_target(data: dict[str, Any], name: str) -> list[str]:
    """The ``ssh`` argv prefix that reaches a host from ANY control node.

    Every element comes from the declaration, so this command works on a
    machine whose ``~/.ssh/config`` is empty. That is the whole point: the
    generated fragment (:func:`render_ssh_config`) is a convenience for people
    typing ``ssh truenas``, and nothing in this tooling depends on it.

    Args:
        data: The parsed declaration.
        name: Fleet host name.

    Returns:
        A complete ssh command up to but not including the remote command.
    """
    argv = ["ssh", *SSH_OPTIONS]
    hops = jump_chain(data, name)
    if hops:
        argv += ["-J", ",".join(hops)]
    argv.append(ssh_destination(data["hosts"][name]))
    return argv


def render_ssh_config(data: dict[str, Any]) -> str:
    """Generate an SSH config fragment naming every declared machine.

    This is what turns any machine into a control node with one command
    instead of a hand-copied ``~/.ssh/config``. It is deliberately NOT what
    this tooling reads: :func:`ssh_target` passes literal addresses, so the
    fragment is a convenience for a person typing ``ssh truenas`` -- and for
    the scripts and docs that already spell a host that way -- rather than a
    prerequisite anything can be broken by omitting.

    ``ProxyJump`` names the fleet host rather than its address, because the
    fragment defines that host too and ssh resolves the hop through it. One
    address, one place.

    Args:
        data: The parsed declaration.

    Returns:
        An ``ssh_config(5)`` body, one ``Host`` block per declared machine.
    """
    lines = [
        "# GENERATED by scripts/dev/fleet.py from infra/fleet.yml -- do not edit.",
        "#",
        "# Install or refresh with:",
        "#     python3 scripts/dev/fleet.py ssh-config --install",
        "#",
        "# Every address here comes from the declaration, so a machine added there",
        "# is reachable by name from the next run of that command with nothing",
        "# hand-edited. StrictHostKeyChecking accept-new pins a key on first use",
        "# and still refuses one that later CHANGES.",
        "",
    ]
    for name, host in data["hosts"].items():
        connect = host["connect"]
        lines.append(f"# {host.get('summary', name)}")
        lines.append(f"Host {name}")
        lines.append(f"    HostName {connect['address']}")
        if connect.get("user"):
            lines.append(f"    User {connect['user']}")
        if connect.get("jump"):
            lines.append(f"    ProxyJump {connect['jump']}")
        lines.append("    StrictHostKeyChecking accept-new")
        lines.append("")
    return "\n".join(lines)


def check_connect(name: str, host: dict[str, Any], hosts: dict[str, Any]) -> list[str]:
    """Rule: a host declares an address any machine could reach it at.

    This is the rule the fleet was missing, and it cost real work (#526). An
    address is required to be a LITERAL: an IP, or a name with a dot in it that
    a resolver can answer. A bare label is exactly the defect and is rejected
    by name, so it cannot come back the next time a machine is added. The login
    user is its own field rather than a ``user@`` prefix, and a jump names
    another host IN THIS FLEET, so a hop is declared once and resolved the same
    way its target is.

    Args:
        name: Fleet host name.
        host: That host's declaration.
        hosts: Every declared host, for resolving ``connect.jump``.

    Returns:
        One message per violation.
    """
    connect = host.get("connect") or {}
    address = str(connect.get("address") or "")
    if not address:
        return [
            f"{name}: connect.address is required -- an IP or a resolvable DNS name. "
            "Nothing can reach a machine that only one laptop's ~/.ssh/config knows about."
        ]
    bad = []
    if "@" in address:
        bad.append(
            f"{name}: connect.address '{address}' carries a login user. Put the user in "
            "connect.user; the address is the machine, not the account."
        )
    elif "." not in address and ":" not in address:
        bad.append(
            f"{name}: connect.address '{address}' is a bare label, which is an "
            "~/.ssh/config alias rather than an address -- it resolves on whichever "
            "machine happens to define it and nowhere else (#526). Declare the IP or a "
            "fully qualified name; `fleet.py ssh-config` generates the alias FROM it."
        )
    bad += [
        f"{name}: connect.{key} '{connect[key]}' contains whitespace"
        for key in ("address", "user", "jump")
        if connect.get(key) and str(connect[key]).split() != [str(connect[key])]
    ]
    return bad + _check_jump(name, connect.get("jump"), hosts)


def _check_jump(name: str, jump: object, hosts: dict[str, Any]) -> list[str]:
    """Rule: a ProxyJump hop is another declared host, and the chain terminates.

    Args:
        name: Fleet host name.
        jump: That host's ``connect.jump``, or None. Deliberately ``object``:
            a declaration may put anything here, and a checker that raised on
            a wrong type would teach nothing.
        hosts: Every declared host.

    Returns:
        One message per violation.
    """
    if not jump:
        return []
    if not isinstance(jump, str) or jump not in hosts:
        return [
            f"{name}: connect.jump '{jump}' is not a declared host. A hop is a fleet "
            f"host, not an ssh alias -- declared: {', '.join(hosts)}"
        ]
    seen = {name}
    cursor = jump
    # Bounded by the fleet size: one more hop than there are hosts can only
    # mean the chain revisits one, and an unbounded walk would hang every
    # command that reaches this host rather than reporting the cycle.
    for _ in range(len(hosts) + 1):
        if not cursor:
            return []
        if cursor in seen:
            return [
                f"{name}: connect.jump chain revisits '{cursor}', so reaching this host "
                "would need itself to already be reachable"
            ]
        if cursor not in hosts:
            # Reported against the host that names it, not against this one:
            # that host's own check_connect call covers it.
            return []
        seen.add(cursor)
        cursor = str((hosts[cursor].get("connect") or {}).get("jump") or "")
    return [f"{name}: connect.jump chain does not terminate"]
