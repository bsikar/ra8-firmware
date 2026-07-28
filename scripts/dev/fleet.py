#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Drive the CI fleet from its declaration.

``infra/fleet.yml`` says what machines exist and how much of each one CI may
use. This is the tool that acts on it -- listing, validating, generating the
inventory, converging a host, and changing a host's live capacity without
killing the jobs it is running.

Every command names a HOST from the declaration, never a class or a hostname,
so adding a machine really is adding a block:

    fleet.py list                    what is declared, and how it is sized
    fleet.py show <host>             one host in full, with its derived vars
    fleet.py validate                the fleet-declaration gate's check
    fleet.py inventory               write infra/ansible/inventory/hosts.ini
    fleet.py check <host> [play]     dry run -- report, change nothing
    fleet.py apply <host> [play]     converge, draining first where needed
    fleet.py remove <host>           tear down (classes that implement it)
    fleet.py status [host]           what each host is running, right now
    fleet.py scale <host> <n>        live capacity change; shrinking DRAINS

``list``, ``show``, ``validate``, ``inventory`` and ``status`` are read-only
and safe at any time. ``apply``, ``remove`` and ``scale`` are not.

Why capacity is a first-class verb here rather than only an Ansible run: a
converge on a container host recreates containers, which cancels whatever they
are running. ``apply`` therefore drains the host first through the same
mechanism ``scale`` uses, so re-provisioning is not a way to lose a job.
"""

from __future__ import annotations

import argparse
import json
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fleet_model as fm

CAPACITY_SCRIPT = fm.REPO_ROOT / "scripts" / "ci" / "fleet_capacity.sh"


def _fail(message: str) -> int:
    """Print an error to stderr and give the caller a shell exit status.

    Args:
        message: What went wrong, in one line.

    Returns:
        Always 2, the usage/precondition status this tool exits with.
    """
    print(f"fleet: error: {message}", file=sys.stderr)
    return 2


def _host(data: dict[str, Any], name: str) -> dict[str, Any]:
    """Look one host up in the declaration.

    Args:
        data: The parsed declaration.
        name: Fleet host name.

    Returns:
        That host's block.

    Raises:
        FleetError: No host of that name is declared.
    """
    if name not in data["hosts"]:
        msg = f"no host '{name}' in infra/fleet.yml. Declared: {', '.join(data['hosts'])}"
        raise fm.FleetError(msg)
    return data["hosts"][name]


def _run(argv: list[str], stdin: str | bytes | None = None, cwd: Path | None = None) -> int:
    """Run a command, streaming its output, and return its status.

    Args:
        argv: Fully resolved argument vector.
        stdin: Text (or bytes, for a tar stream) to feed the command. The
            ``bash -s`` transport carries whole scripts this way, which keeps
            them clear of both the Windows shell's quoting and the remote
            shell's.
        cwd: Directory to run in. Ansible needs ``infra/ansible``: both the
            playbook paths and ``ansible.cfg`` are resolved relative to it.

    Returns:
        The command's exit status.
    """
    proc = subprocess.run(  # noqa: S603 -- argv is built from the declaration, never a shell string
        argv,
        input=stdin,
        text=not isinstance(stdin, bytes),
        cwd=cwd,
        check=False,
    )
    return proc.returncode


# Where the play is unpacked inside a WSL distro. Under the distro's own ext4
# root, never /mnt/c: that is drvfs, a 9P translation to NTFS, and roughly an
# order of magnitude slower for the many-small-files work an unpack is.
WSL_STAGE = "/opt/ra8-infra"


def _push_to_wsl(host: dict[str, Any]) -> int:
    """Copy the playbooks and the capacity script into a WSL host's distro.

    WSL has no SSH daemon of its own, and giving a personal daily-driver
    machine one -- plus the inbound firewall hole to reach it -- is not a
    reasonable ask, so the play is shipped in and run with
    ``--connection=local``. This is the codified form of what was previously a
    hand-run pipeline in the playbook's header comment.

    Args:
        host: The ``docker_wsl`` host's declaration.

    Returns:
        0 on success, the first failing step's status otherwise.
    """
    ssh = fm.ssh_target(host)
    shell = fm.remote_shell(host)
    prepare = f"set -eu\nrm -rf {WSL_STAGE}\nmkdir -p {WSL_STAGE}\n"
    rc = _run([*ssh, shell], stdin=prepare)
    if rc:
        return rc
    tar_tool = shutil.which("tar") or "tar"
    tar = subprocess.run(  # noqa: S603 -- fixed argv, paths from the checkout
        [
            tar_tool,
            # macOS bsdtar attaches com.apple.provenance to every member, and
            # GNU tar in the distro then warns once per file -- 37 lines of
            # noise around the one line that matters. The archive is identical
            # without them.
            "--no-xattrs",
            "-czf",
            "-",
            "-C",
            str(fm.REPO_ROOT),
            "infra/ansible",
            "scripts/ci/fleet_capacity.sh",
        ],
        capture_output=True,
        check=False,
    )
    if tar.returncode:
        sys.stderr.write(tar.stderr.decode("utf-8", "replace"))
        return tar.returncode
    distro = host["connect"]["distro"]
    unpack = f"wsl -d {distro} -u root -e tar -xzf - -C {WSL_STAGE}"
    return _run([*ssh, unpack], stdin=tar.stdout)


def _capacity(data: dict[str, Any], name: str, args: list[str]) -> int:
    """Run ``fleet_capacity.sh`` on a host, over that host's transport.

    The script is piped from the checkout on every call rather than invoked
    from a copy on the host, so an operator command always runs the version in
    the tree. The copy the ``fleet_capacity`` role installs exists for the
    unattended quiet-hours timer, which has no checkout to read from.

    Args:
        data: The parsed declaration.
        name: Fleet host name.
        args: Arguments after the fixed configuration flags.

    Returns:
        The script's exit status.
    """
    host = _host(data, name)
    cls = fm.CLASSES[host["class"]]
    if cls.capacity_kind == "none":
        return _fail(f"{name} is a {host['class']} host and carries no runners to scale")
    flags = ["--kind", cls.capacity_kind]
    if cls.capacity_kind == "docker":
        if fm.docker_command(host) != "docker":
            flags.append("--sudo")
        for container in fm.container_names(host):
            flags += ["--container", container]
    else:
        flags += ["--scale-set", host["runners"]["labels"][0]]
    # Never a quoted argument: for the WSL host this line is parsed by Windows'
    # shell before `wsl -e` sees it, and quoting does not survive that. The
    # capacity script's flags are shaped so none is ever needed.
    remote = f"{fm.remote_shell(host)} -- {' '.join(flags)} {' '.join(args)}"
    return _run([*fm.ssh_target(host), remote], stdin=CAPACITY_SCRIPT.read_text(encoding="utf-8"))


def cmd_list(data: dict[str, Any], _args: argparse.Namespace) -> int:
    """Print every declared host with its class, capacity and schedule.

    Args:
        data: The parsed declaration.
        _args: Unused; the command takes no arguments.

    Returns:
        0.
    """
    print(
        f"{'HOST':<10} {'CLASS':<13} {'INSTANCES':<10} "
        f"{'PER INSTANCE':<16} {'QUIET HOURS':<22} PLAYS"
    )
    for name, host in data["hosts"].items():
        run = host.get("runners") or {}
        quiet = host.get("quiet_hours") or {}
        count = str(run.get("instances", "-"))
        per = f"{run['cpus']} cpu / {run['memory_gb']} GB" if run else "-"
        window = f"{quiet['window']} {quiet['days']} -> {quiet['instances']}" if quiet else "-"
        print(
            f"{name:<10} {host['class']:<13} {count:<10} {per:<16} {window:<22} "
            f"{','.join(host['provisions'])}"
        )
    print()
    print("make infra-check HOST=<host>       dry run (changes nothing)")
    print("make infra-apply HOST=<host>       converge to the declaration")
    print("make infra-scale HOST=<host> N=<n> live capacity change; shrinking DRAINS")
    print("docs/CI_FLEET.md                   add a host, retune one, quiet hours")
    return 0


def cmd_show(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Print one host's declaration and everything derived from it.

    Args:
        data: The parsed declaration.
        args: Parsed command line; uses ``args.host``.

    Returns:
        0.
    """
    host = _host(data, args.host)
    cls = fm.CLASSES[host["class"]]
    print(f"{args.host}: {host.get('summary', '')}")
    print(f"  class          {host['class']}  ({cls.summary})")
    print(f"  transport      {cls.transport}")
    print(f"  reachable as   {' '.join(fm.ssh_target(host)[5:])}")
    print(f"  provisions     {', '.join(host['provisions'])}")
    if cls.runner:
        want = fm.recommended_instances(data["sizing"], host["budget"])
        print(f"  instances      {host['runners']['instances']} (formula gives {want})")
        if fm.container_names(host):
            print(f"  registrations  {', '.join(fm.instance_names(args.host, host))}")
            print(f"  containers     {', '.join(fm.container_names(host))}")
    print("  derived ansible variables:")
    for key, value in sorted(fm.role_vars(args.host, host).items()):
        print(f"    {key}: {value}")
    return 0


def cmd_validate(data: dict[str, Any], _args: argparse.Namespace) -> int:
    """Report every rule the declaration breaks.

    Args:
        data: The parsed declaration.
        _args: Unused; the command takes no arguments.

    Returns:
        0 when the fleet is well declared, 1 otherwise.
    """
    problems = fm.validate(data)
    if problems:
        print(f"infra/fleet.yml: {len(problems)} problem(s):", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    hosts = data["hosts"]
    runners = sum(int((h.get("runners") or {}).get("instances", 0)) for h in hosts.values())
    print(f"infra/fleet.yml OK: {len(hosts)} host(s), {runners} declared runner instance(s)")
    return 0


def cmd_inventory(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Generate the Ansible inventory from the declaration.

    Args:
        data: The parsed declaration.
        args: Parsed command line; uses ``args.stdout``.

    Returns:
        0.
    """
    body = fm.render_inventory(data)
    if args.stdout:
        print(body, end="")
        return 0
    fm.INVENTORY.parent.mkdir(parents=True, exist_ok=True)
    fm.INVENTORY.write_text(body, encoding="utf-8")
    print(f"wrote {fm.INVENTORY.relative_to(fm.REPO_ROOT)} ({len(data['hosts'])} host(s))")
    return 0


def _playbook_argv(name: str, host: dict[str, Any], play: str, extra: list[str]) -> list[str]:
    """Build the ``ansible-playbook`` command for one host and play.

    Args:
        name: Fleet host name.
        host: That host's declaration.
        play: Play key from :data:`fleet_model.PLAYS`.
        extra: Extra flags, e.g. ``--check --diff``.

    Returns:
        The argument vector to run from ``infra/ansible``.
    """
    variables = fm.role_vars(name, host)
    argv = [
        "ansible-playbook",
        "-i",
        str(fm.INVENTORY),
        f"playbooks/{fm.PLAYS[play].playbook}",
        "--limit",
        name,
        "-e",
        json.dumps(variables),
    ]
    if fm.CLASSES[host["class"]].transport == "wsl":
        argv += ["-e", f"wsl_ci_host_id={name}"]
    return argv + extra


def _converge_wsl(name: str, host: dict[str, Any], plays: list[str], extra: list[str]) -> int:
    """Run a WSL host's plays inside its distro.

    Args:
        name: Fleet host name.
        host: That host's declaration.
        plays: Plays to run, in order.
        extra: Extra ansible flags (dry-run, teardown state).

    Returns:
        0 on success, the first failing play's status otherwise.
    """
    rc = _push_to_wsl(host)
    if rc:
        return rc
    variables = shlex.quote(json.dumps(fm.role_vars(name, host)))
    # The stage keeps the checkout's own layout, so the play's relative paths
    # (roles/, and fleet_capacity_src reaching back into scripts/) resolve
    # exactly as they do on the control node.
    lines = [
        "set -eu",
        f"cd {WSL_STAGE}/infra/ansible",
        *(
            "ansible-playbook --connection=local -i localhost, "
            f"playbooks/{fm.PLAYS[play].playbook} -e {variables} "
            f"-e wsl_ci_host_id={shlex.quote(name)} "
            f"-e fleet_capacity_src={WSL_STAGE}/scripts/ci/fleet_capacity.sh "
            f"{' '.join(extra)}"
            for play in plays
        ),
    ]
    return _run([*fm.ssh_target(host), fm.remote_shell(host)], stdin="\n".join(lines) + "\n")


def _plays_for(host: dict[str, Any], only: str | None) -> list[str]:
    """Which plays an apply or check should run.

    Args:
        host: One host's declaration.
        only: A single play the caller asked for, or None for all of them.

    Returns:
        The plays in declared order, or empty when ``only`` is not one of them.
    """
    if only is None:
        return list(host["provisions"])
    return [only] if only in host["provisions"] else []


def _converge_refusal(args: argparse.Namespace, host: dict[str, Any], plays: list[str]) -> str:
    """Why this converge must not run, if it must not.

    Args:
        args: Parsed command line.
        host: The target host's declaration.
        plays: Plays the caller selected.

    Returns:
        The refusal, or an empty string when the converge may proceed.
    """
    if not plays:
        return f"{args.host} does not provision '{args.play}' ({', '.join(host['provisions'])})"
    if args.mode == "remove" and not all(fm.PLAYS[p].removable for p in plays):
        return (
            f"{args.host} runs {', '.join(plays)}, and not every one of those roles owns "
            "both halves of its lifecycle. Removing it would mean undoing the rest by "
            "hand, which is the drift the roles exist to prevent -- add a teardown path "
            "to the role instead of tearing it down manually."
        )
    return ""


def cmd_converge(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Run a host's plays, either as a dry run or for real.

    On a container host a converge recreates containers, which cancels running
    jobs, so a real apply drains the host to zero instances first and restores
    the declared count afterwards. ``--no-drain`` skips that when the caller
    knows the host is idle; a dry run never drains, because it changes nothing.

    Args:
        data: The parsed declaration.
        args: Parsed command line.

    Returns:
        0 on success, non-zero from the first step that fails.
    """
    host = _host(data, args.host)
    plays = _plays_for(host, args.play)
    refusal = _converge_refusal(args, host, plays)
    if refusal:
        return _fail(refusal)
    rc = cmd_inventory(data, argparse.Namespace(stdout=False))
    if rc:
        return rc
    # `--tags capacity` refreshes only the drain script and the quiet-hours
    # timer. Nothing in that path stops, starts or recreates a container, so
    # draining the host first would cost it every running job's worth of runner
    # time to protect against a change that cannot touch them.
    capacity_only = args.tags == "capacity"
    drain = (
        args.mode == "apply"
        and not args.no_drain
        and not capacity_only
        and fm.container_names(host)
    )
    if drain:
        print(f"==> draining {args.host} before converging (a converge recreates containers)")
        # drain-all, not `scale 0`: a converge that changes the instance count
        # across the 1 <-> N boundary also renames the containers, so the drain
        # has to walk what is really on the host rather than what the
        # declaration predicts.
        rc = _capacity(data, args.host, ["drain-all"])
        if rc:
            return _fail("could not drain the host; refusing to converge over running jobs")
    extra = (["--check", "--diff"] if args.mode == "check" else []) + _remove_flags(host, args)
    # SHORT-LIVED credentials only, and preferably by file reference.
    #
    # Anything given as KEY=VALUE lands in this process's argv and in
    # ansible-playbook's, where `ps` on the control node can read it. That is
    # tolerable for a registration or removal token -- one hour, one use,
    # minted on demand -- and it is NOT tolerable for a PAT. Ansible reads
    # `-e @file` as well, so a long-lived credential goes in a mode-0600 file
    # outside the checkout and is passed as `-e @/path/to/secrets.yml`.
    for pair in args.extra_var:
        extra += ["-e", pair]
    if args.tags:
        extra += ["--tags", args.tags]
    if fm.CLASSES[host["class"]].transport == "wsl":
        rc = _converge_wsl(args.host, host, plays, extra)
    else:
        rc = _converge_ssh(args.host, host, plays, extra)
    if rc or args.mode == "remove":
        return rc
    if drain:
        return _capacity(data, args.host, ["scale", str(host["runners"]["instances"])])
    return 0


def _converge_ssh(name: str, host: dict[str, Any], plays: list[str], extra: list[str]) -> int:
    """Run a host's plays with Ansible over ssh, from the control node.

    Args:
        name: Fleet host name.
        host: That host's declaration.
        plays: Plays to run, in order.
        extra: Extra ansible flags (dry-run, teardown state).

    Returns:
        0 on success, the first failing play's status otherwise.
    """
    for play in plays:
        argv = _playbook_argv(name, host, play, extra)
        print(f"==> ansible-playbook {fm.PLAYS[play].playbook} --limit {name}")
        rc = _run(argv, cwd=fm.ANSIBLE_DIR)
        if rc:
            return rc
    return 0


def _remove_flags(host: dict[str, Any], args: argparse.Namespace) -> list[str]:
    """The extra-vars that turn a converge into a teardown.

    Args:
        host: One host's declaration.
        args: Parsed command line.

    Returns:
        The ``state=absent`` flags for a removal, empty otherwise.
    """
    if args.mode != "remove":
        return []
    flags = ["-e", "ci_runner_docker_state=absent", "-e", "fleet_capacity_enabled=false"]
    if fm.CLASSES[host["class"]].transport == "wsl":
        flags += ["-e", "wsl_ci_host_state=absent"]
    return flags


def cmd_status(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Report what each runner host is actually running.

    Read-only, and deliberately free of GitHub API calls: every probe is a
    local one over ssh, so any number of agents can run it without touching the
    shared REST quota.

    Args:
        data: The parsed declaration.
        args: Parsed command line; uses ``args.host``.

    Returns:
        0 even when a host is unreachable -- an unreachable machine is
        information, not a failure of the question.
    """
    names = [args.host] if args.host else list(data["hosts"])
    for name in names:
        host = _host(data, name)
        if fm.CLASSES[host["class"]].capacity_kind == "none":
            continue
        print(f"{name} ({host['class']}, declared {host['runners']['instances']} instance(s)):")
        # Flushed before handing the terminal to ssh, or Python's buffer holds
        # the heading until after the rows it introduces have already printed.
        sys.stdout.flush()
        _capacity(data, name, ["status"])
    return 0


def cmd_reach(data: dict[str, Any], _args: argparse.Namespace) -> int:
    """Probe every declared machine over its own transport.

    The transport is the point: ``win-ci`` is not an ssh alias but a jump
    through the bench Pi into a Windows box and then into a WSL distro, and a
    reachability check that did not know that would report the fleet broken.
    Because the list comes from the declaration, a machine added there is
    probed from the next run with nothing else edited.

    Args:
        data: The parsed declaration.
        _args: Unused; the command takes no arguments.

    Returns:
        0 when every machine answered, 1 otherwise.
    """
    rc = 0
    for name, host in data["hosts"].items():
        probe = _run([*fm.ssh_target(host), fm.remote_shell(host)], stdin="echo ok\n")
        if probe:
            print(f"  MISS  {name:<10} not reachable over its declared transport")
            rc = 1
        else:
            print(f"  ok    {name:<10} reachable")
    return rc


def cmd_scale(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Change how many instances a host is running, right now.

    Growing starts parked instances. Shrinking DRAINS: an instance is stopped
    only once it is idle, never signalled while it holds a job, because the
    runner cancels its in-flight job on SIGTERM.

    Args:
        data: The parsed declaration.
        args: Parsed command line; uses ``args.host`` and ``args.count``.

    Returns:
        The capacity script's status: non-zero when it could not converge
        inside its deadline, which means instances were left running on
        purpose.
    """
    return _capacity(data, args.host, ["scale", str(args.count)])


def _parser() -> argparse.ArgumentParser:
    """Build the command-line parser.

    Returns:
        A parser whose subcommands mirror the module docstring.
    """
    parser = argparse.ArgumentParser(prog="fleet.py", description=__doc__.splitlines()[0])
    subs = parser.add_subparsers(dest="command", required=True)
    subs.add_parser("list", help="what is declared, and how it is sized")
    subs.add_parser("show", help="one host in full").add_argument("host")
    subs.add_parser("validate", help="the fleet-declaration gate's check")
    subs.add_parser("reach", help="probe every machine over its declared transport")
    inv = subs.add_parser("inventory", help="write the Ansible inventory")
    inv.add_argument("--stdout", action="store_true", help="print instead of writing")
    for mode in ("check", "apply", "remove"):
        sub = subs.add_parser(mode, help=f"{mode} a host against the declaration")
        sub.add_argument("host")
        sub.add_argument("play", nargs="?", help="one play instead of all of them")
        sub.add_argument("--no-drain", action="store_true", help="do not drain before converging")
        sub.add_argument(
            "-e",
            "--extra-var",
            action="append",
            default=[],
            metavar="KEY=VALUE",
            help=(
                "pass through to ansible. KEY=VALUE is visible in ps, so use it only "
                "for a short-lived registration or removal token; pass anything "
                "long-lived as @/path/to/secrets.yml (mode 0600, outside the checkout)"
            ),
        )
        sub.add_argument(
            "--tags",
            default="",
            help="ansible tags; 'capacity' refreshes only the drain script and "
            "the quiet-hours timer, and needs no drain",
        )
    status = subs.add_parser("status", help="what each host is running, right now")
    status.add_argument("host", nargs="?")
    scale = subs.add_parser("scale", help="live capacity change; shrinking drains")
    scale.add_argument("host")
    scale.add_argument("count", type=int)
    return parser


def main(argv: list[str] | None = None) -> int:
    """Entry point.

    Args:
        argv: Command line, defaulting to ``sys.argv[1:]``.

    Returns:
        The chosen subcommand's exit status.
    """
    args = _parser().parse_args(argv)
    handlers = {
        "list": cmd_list,
        "show": cmd_show,
        "validate": cmd_validate,
        "reach": cmd_reach,
        "inventory": cmd_inventory,
        "check": cmd_converge,
        "apply": cmd_converge,
        "remove": cmd_converge,
        "status": cmd_status,
        "scale": cmd_scale,
    }
    args.mode = args.command
    try:
        data = fm.load()
        return handlers[args.command](data, args)
    except fm.FleetError as exc:
        return _fail(str(exc))


if __name__ == "__main__":
    sys.exit(main())
