#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Drive the CI fleet from ``infra/fleet.yml``.

Commands name a declared host, derive its reachability, and expose read-only
inspection, inventory generation, guarded convergence, registration, removal,
and capacity control. Mutating convergence drains runner capacity and binds
bench-affecting work to the repository's authenticated whole-bench hold.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path
from threading import Event, Thread
from typing import Any, Protocol

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fleet_bench as fb
import fleet_capacity_client as fcc
import fleet_model as fm
import fleet_mutation_lock as fml
import fleet_reach as fr
import fleet_runner_maintenance as frm
import fleet_ssh_config as fsc
import fleet_typed_vars as ftv
import fleet_wsl as fw

IDLE_STOP_HELPER = fm.REPO_ROOT / "infra/ansible/roles/dev_box/files/ra8-hil-runner-idle-stop.py"
MUTATING_COMMANDS = frozenset(
    {
        "register-runner",
        "register-hil",
        "apply",
        "reconcile-parked-apply",
        "reconcile-parked-check",
        "reconcile-activate",
        "remove",
        "capacity-quarantine",
        "capacity-restore",
        "scale",
    }
)


class _SubparserGroup(Protocol):
    """Expose the parser-factory operation used to build fleet subcommands."""

    def add_parser(self, name: str, **kwargs: object) -> argparse.ArgumentParser:
        """Create and return one named subparser."""
        ...


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


def _run(
    argv: list[str],
    stdin: str | bytes | None = None,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    subprocess_kwargs: Mapping[str, object] | None = None,
) -> int:
    """Run a command, streaming its output, and return its status.

    Args:
        argv: Fully resolved argument vector.
        stdin: Text (or bytes, for a tar stream) to feed the command. The
            ``bash -s`` transport carries whole scripts this way, which keeps
            them clear of both the Windows shell's quoting and the remote
            shell's.
        cwd: Directory to run in. Ansible needs ``infra/ansible``: both the
            playbook paths and ``ansible.cfg`` are resolved relative to it.
        env: Exact child environment, or the caller's environment when absent.
        subprocess_kwargs: Narrow trusted FD inheritance for bench re-entry only.

    Returns:
        The command's exit status.
    """
    options: dict[str, object] = {
        "input": stdin,
        "text": not isinstance(stdin, bytes),
        "cwd": cwd,
        "env": env,
    }
    if subprocess_kwargs is not None:
        options.update(subprocess_kwargs)
    proc = subprocess.run(  # noqa: S603 -- argv is built from the declaration, never a shell string
        argv, check=False, **options
    )
    return proc.returncode


def cmd_list(data: dict[str, Any], _args: argparse.Namespace) -> int:
    """Print every declared host with its class, capacity and schedule.

    Args:
        data: The parsed declaration.
        _args: Unused; the command takes no arguments.

    Returns:
        0.
    """
    print(
        f"{'HOST':<10} {'CLASS':<14} {'INSTANCES':<10} "
        f"{'PER INSTANCE':<18} {'QUIET HOURS':<32} PLAYS"
    )
    for name, host in data["hosts"].items():
        run = host.get("runners") or {}
        quiet = host.get("quiet_hours") or {}
        count = str(run.get("instances", "-"))
        per = f"{run['cpus']} cpu / {run['memory_gb']} GB" if run else "-"
        window = f"{quiet['window']} {quiet['days']} -> {quiet['instances']}" if quiet else "-"
        print(
            f"{name:<10} {host['class']:<14} {count:<10} {per:<18} {window:<32} "
            f"{','.join(host['provisions'])}"
        )
    print()
    print("just infra::check <host>           dry run (changes nothing)")
    print("just infra::apply <host>           converge to the declaration")
    print("just infra::scale <host> <count>   live capacity change; shrinking DRAINS")
    print("just infra::ssh_config             name these machines in your ~/.ssh/config")
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
    print(f"  reachable as   {fr.ssh_destination(host)}")
    hops = fr.jump_chain(data, args.host)
    if hops:
        print(f"  via            {' -> '.join(hops)}")
    print(f"  provisions     {', '.join(host['provisions'])}")
    if cls.capacity_runner:
        want = fm.recommended_instances(data["sizing"], host["budget"])
        print(f"  instances      {host['runners']['instances']} (formula gives {want})")
        if fm.container_names(host):
            print(f"  registrations  {', '.join(fm.instance_names(args.host, host))}")
            print(f"  containers     {', '.join(fm.container_names(host))}")
    hil = host.get("hil_runner")
    if hil:
        print(f"  HIL listener   {hil['name']} ({','.join(hil['labels'])})")
        print(f"  HIL workflow   {hil['workflow']}")
        print(f"  HIL bench      {hil['bench']['host']}")
    lent = host.get("dev_slice")
    if lent:
        print(
            f"  dev slice      {fm.DEV_SLICE_UNIT}: CPUWeight {lent['cpu_weight']} "
            f"(vs {fm.SYSTEMD_DEFAULT_CPU_WEIGHT} for CI), MemoryMax "
            f"{lent['memory_gb']}G, swap {lent.get('swap_gb', 0)}G, "
            f"-j{lent['max_jobs']}"
        )
    print("  derived ansible variables:")
    for key, value in sorted(fm.role_vars(data, args.host, host).items()):
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
    native_hil = sum(1 for host in hosts.values() if host.get("hil_runner"))
    print(
        f"infra/fleet.yml OK: {len(hosts)} host(s), {runners} capacity-managed "
        f"runner instance(s), {native_hil} native HIL listener(s)"
    )
    return 0


def _publish_inventory(body: str) -> None:
    """Atomically publish one complete shared Ansible inventory generation."""
    fm.INVENTORY.parent.mkdir(parents=True, exist_ok=True)
    fd, raw = tempfile.mkstemp(prefix=f".{fm.INVENTORY.name}.", dir=fm.INVENTORY.parent)
    temporary = Path(raw)
    try:
        os.fchmod(fd, 0o644)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(body)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(fm.INVENTORY)
        directory = os.open(fm.INVENTORY.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def _inventory_publication_selftest() -> list[str]:
    """Prove concurrent readers observe only complete inventory generations."""
    failures: list[str] = []
    original = fm.INVENTORY
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-inventory-") as raw:
        fm.INVENTORY = Path(raw) / "hosts.ini"
        first = "[fleet]\n" + "a=1\n" * 8192
        second = "[fleet]\n" + "b=2\n" * 8192
        fm.INVENTORY.write_text(first, encoding="utf-8")
        stopped = Event()
        partial: list[str] = []

        def read_generations() -> None:
            while not stopped.is_set():
                observed = fm.INVENTORY.read_text(encoding="utf-8")
                if observed not in {first, second}:
                    partial.append(observed)
                    stopped.set()

        reader = Thread(target=read_generations)
        reader.start()
        try:
            for index in range(32):
                _publish_inventory(first if index % 2 else second)
        finally:
            stopped.set()
            reader.join()
            fm.INVENTORY = original
        if partial:
            failures.append("concurrent inventory reader observed a partial generation")
    return failures


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
    _publish_inventory(body)
    print(f"wrote {fm.inventory_label()} ({len(data['hosts'])} host(s))")
    return 0


def cmd_ssh_config(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Print or install the declaration-derived SSH config fragment."""
    return fsc.command(data, install=args.install)


def cmd_ssh_target(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Print the ssh command that reaches one host from this machine.

    The declaration is the only place that knows a host is behind a jump, or
    which account it is entered as, so the scripts that probe a machine ask
    here rather than spelling an alias of their own. Every token is
    whitespace-free by construction -- the fleet-declaration gate rejects an
    address, user or jump containing any -- so a caller may split the line on
    spaces and use it as an argv.

    Args:
        data: The parsed declaration.
        args: Parsed command line; uses ``args.host``.

    Returns:
        0.
    """
    _host(data, args.host)
    print(" ".join(fr.ssh_target(data, args.host)))
    return 0


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
    boundary = fb.control_flow_refusal(
        fb.FlowRequest(
            str(host["class"]),
            plays,
            args.mode,
            args.tags,
            args.extra_var,
            bool(getattr(args, "trusted_tags", False)),
        )
    )
    if boundary:
        return boundary
    return ""


def _restore_after_converge(data: dict[str, Any], args: argparse.Namespace, rc: int) -> int:
    """Restore declared capacity after a drained converge, preserving failure."""
    if rc:
        quarantine = fcc.run(data, args.host, ["quarantine"], _run)
        return rc or quarantine
    return fcc.run(data, args.host, ["restore"], _run)


def _is_parked_command(command: str) -> bool:
    """Return whether reconciliation requires zero admission through postcheck."""
    return command in {"reconcile-parked-apply", "reconcile-parked-check"}


def _converge_extra(host: dict[str, Any], args: argparse.Namespace) -> list[str]:
    """Build Ansible flags without weakening credential handling."""
    extra = (["--check", "--diff"] if args.mode == "check" else []) + _remove_flags(host, args)
    if _is_parked_command(args.command):
        extra += ["-e", "fleet_reconcile_parked=true"]
    if args.command in {"reconcile-activate", "reconcile-activation-check"}:
        extra += ["-e", "fleet_reconcile_activation_hold=true"]
    # SHORT-LIVED credentials only, and preferably by file reference.
    #
    # Anything given as KEY=VALUE lands in this process's argv and in
    # ansible-playbook's, where `ps` on the control node can read it. That is
    # tolerable only for non-secret compatibility variables. Credentials use
    # the typed commands below, which validate and snapshot mode-0600 files
    # before any inventory write, drain, staging, or remote command.
    for pair in args.extra_var:
        extra += ["-e", pair]
    if args.tags:
        extra += ["--tags", args.tags]
    return extra


def _registration_args(
    host: str,
    play: str | None,
    tags: str,
    typed_vars: ftv.TypedVars,
    original_argv: list[str],
) -> argparse.Namespace:
    """Build the internal apply request shared by typed registration commands."""
    return argparse.Namespace(
        command="apply",
        mode="apply",
        host=host,
        play=play,
        no_drain=False,
        extra_var=[],
        tags=tags,
        vars_file="",
        typed_vars=typed_vars,
        trusted_tags=True,
        original_argv=original_argv,
    )


def cmd_register_runner(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Register a declared Docker runner host from one schema-limited vars file."""
    host = _host(data, args.host)
    if host["class"] not in ftv.CONTAINER_RUNNER_CLASSES:
        return _fail(
            f"{args.host} is class {host['class']}; runner registration is limited to "
            f"{', '.join(sorted(ftv.CONTAINER_RUNNER_CLASSES))}"
        )
    provisions = list(host["provisions"])
    if len(provisions) != 1 or provisions[0] not in ftv.CONTAINER_RUNNER_PLAYS:
        return _fail(
            f"{args.host} does not have one typed container-runner play: {', '.join(provisions)}"
        )
    typed_vars = ftv.read_typed_vars_file(args.vars_file, ftv.RUNNER_REGISTRATION)
    request = _registration_args(args.host, provisions[0], "", typed_vars, args.original_argv)
    return cmd_converge(data, request)


def cmd_register_hil(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Register the one declared native HIL listener from a typed vars file."""
    candidates = [name for name, host in data["hosts"].items() if "hil_runner" in host]
    if len(candidates) != 1:
        return _fail(f"expected exactly one declared HIL listener, found {len(candidates)}")
    name = candidates[0]
    host = _host(data, name)
    if host["class"] != "dev_box" or "dev-box" not in host["provisions"]:
        return _fail(f"declared HIL listener {name} is not provisioned by the dev-box role")
    typed_vars = ftv.read_typed_vars_file(args.vars_file, ftv.HIL_REGISTRATION)
    request = _registration_args(name, "dev-box", "hil-runner", typed_vars, args.original_argv)
    return cmd_converge(data, request)


def _typed_vars_for_converge(
    args: argparse.Namespace, host: dict[str, Any]
) -> ftv.TypedVars | None:
    """Validate typed vars before inventory writes, draining, or remote work."""
    typed_vars = getattr(args, "typed_vars", None)
    vars_file = getattr(args, "vars_file", "")
    if vars_file:
        if args.mode != "remove":
            message = "--vars-file is accepted only by the typed remove operation"
            raise fm.FleetError(message)
        if host["class"] not in ftv.CONTAINER_RUNNER_CLASSES:
            message = "runner removal vars are accepted only for container-runner hosts"
            raise fm.FleetError(message)
        typed_vars = ftv.read_typed_vars_file(vars_file, ftv.RUNNER_REMOVAL)
    if any(value.startswith("@") for value in args.extra_var):
        message = (
            "raw -e @file is not accepted; use register-runner, register-hil, or "
            "remove --vars-file so the file is validated before side effects"
        )
        raise fm.FleetError(message)
    return typed_vars


@dataclass(frozen=True)
class _ConvergeTransport:
    """Carry one validated converge transaction across its transport boundary."""

    data: dict[str, Any]
    args: argparse.Namespace
    host: dict[str, Any]
    plays: list[str]
    extra: list[str]
    typed_vars: ftv.TypedVars | None
    no_drain_tags: bool


def _bench_guard_argv(
    host: dict[str, Any], plays: list[str], args: argparse.Namespace
) -> list[str]:
    """Build the outer whole-bench transaction before any side effect."""
    try:
        return fb.guarded_argv(
            fb.GuardRequest(
                fm.REPO_ROOT,
                Path(__file__).resolve(),
                args.original_argv,
                host["class"],
                plays,
                args.mode,
                os.environ,
            )
        )
    except ValueError as exc:
        raise fm.FleetError(str(exc)) from exc


def _bench_ansible_extra(
    host: dict[str, Any], plays: list[str], args: argparse.Namespace
) -> list[str]:
    """Carry the authenticated outer hold into the remote role."""
    try:
        return fb.ansible_extra(host["class"], plays, args.mode, os.environ)
    except ValueError as exc:
        raise fm.FleetError(str(exc)) from exc


def _runner_maintenance_request(
    data: dict[str, Any],
    host: dict[str, Any],
    args: argparse.Namespace,
    extra: list[str],
) -> frm.MaintenanceRequest:
    """Build the read-only preview and declared idle-stop transport."""
    preview = frm.playbook_argv(
        data,
        args.host,
        host,
        "dev-box",
        ["--check", "--diff", *extra],
    )
    remote = "/usr/bin/sudo -n /usr/bin/python3 - ra8-hil-runner.service"
    return frm.MaintenanceRequest(
        preview,
        fm.ANSIBLE_DIR,
        os.environ,
        args.host,
        [*fr.ssh_target(data, args.host), remote],
        IDLE_STOP_HELPER.read_text(encoding="utf-8"),
    )


def _prepare_native_runner(request: _ConvergeTransport) -> frm.MaintenanceDecision:
    """Preview exact drift and stop only an idle listener when needed."""
    if not frm.applies(request.host["class"], request.plays, request.args.mode):
        return frm.MaintenanceDecision(proceed=True, status=0)
    if request.typed_vars is None:
        maintenance = _runner_maintenance_request(
            request.data, request.host, request.args, request.extra
        )
        return frm.prepare(maintenance)
    with ftv.local_vars_snapshot(request.typed_vars) as snapshot:
        guarded_extra = [*request.extra, "-e", f"@{snapshot}"]
        maintenance = _runner_maintenance_request(
            request.data, request.host, request.args, guarded_extra
        )
        return frm.prepare(maintenance)


def _run_converge_transport(request: _ConvergeTransport) -> int:
    """Run one already-validated converge over its declared transport."""
    if fm.CLASSES[request.host["class"]].transport == "wsl":
        spec = fw.ConvergeSpec(
            request.data,
            request.args.host,
            request.plays,
            request.extra,
            request.typed_vars,
            request.args.mode,
        )
        return fw.converge(
            spec,
            sync_image=request.args.mode == "apply" and not request.no_drain_tags,
            run=_run,
        )
    if request.typed_vars is None:
        return _converge_ssh(request.data, request.args.host, request.plays, request.extra)
    with ftv.local_vars_snapshot(request.typed_vars) as snapshot:
        extra = [*request.extra, "-e", f"@{snapshot}"]
        return _converge_ssh(request.data, request.args.host, request.plays, extra)


def _bench_guard_subprocess_kwargs(
    command: str, capability: Callable[[], dict[str, object]]
) -> dict[str, object] | None:
    """Carry the live guardian only through a mutating bench re-entry."""
    return capability() if command in MUTATING_COMMANDS else None


def _bench_guard_inheritance_selftest() -> list[str]:
    """Prove only mutating bench re-entry inherits the live guardian FD."""
    failures: list[str] = []
    calls = 0
    expected = {"env": {"RA8_FLEET_MUTATION_GUARDIAN_FD": "9"}, "pass_fds": (9,)}

    def capability() -> dict[str, object]:
        nonlocal calls
        calls += 1
        return expected

    if _bench_guard_subprocess_kwargs("apply", capability) != expected or calls != 1:
        failures.append("mutating bench re-entry dropped its guardian capability")
    if _bench_guard_subprocess_kwargs("check", capability) is not None or calls != 1:
        failures.append("read-only bench re-entry inherited a mutation capability")
    return failures


def cmd_converge(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Run a dry check or a guarded real converge of one host's plays.

    Container-host applies drain first. A reconciler-only parked apply leaves
    capacity at zero for its caller's postcheck. Bench-host applies re-enter
    under the physical bench lock before inventory generation or remote work.
    """
    host = _host(data, args.host)
    parked = _is_parked_command(args.command)
    plays = _plays_for(host, args.play)
    refusal = _converge_refusal(args, host, plays)
    if parked and not host.get("runners"):
        refusal = "parked reconciliation is limited to capacity-managed runner hosts"
    if refusal:
        return _fail(refusal)
    guard = _bench_guard_argv(host, plays, args)
    if guard:
        guardian = _bench_guard_subprocess_kwargs(args.command, fml.guardian_subprocess_kwargs)
        return _run(guard, cwd=fm.REPO_ROOT, subprocess_kwargs=guardian)
    typed_vars = _typed_vars_for_converge(args, host)
    rc = cmd_inventory(data, argparse.Namespace(stdout=False))
    if rc:
        return rc
    # Some tag sets cannot stop, start or recreate a container -- `capacity`
    # refreshes the drain script and the quiet-hours timer, `dev-slice` tunes a
    # cgroup beside them. Draining the host for either would cost it every
    # running job's worth of runner time to protect against a change that
    # cannot touch them. The whitelist lives in fleet_model.NO_DRAIN_TAGS so
    # adding a tag is a deliberate act with the rule in front of you.
    no_drain_tags = args.tags in fm.NO_DRAIN_TAGS
    extra = _converge_extra(host, args)
    extra += _bench_ansible_extra(host, plays, args)
    request = _ConvergeTransport(data, args, host, plays, extra, typed_vars, no_drain_tags)
    maintenance = _prepare_native_runner(request)
    if not maintenance.proceed:
        return maintenance.status
    drain = (
        args.mode == "apply"
        and not args.no_drain
        and not no_drain_tags
        and (parked or fm.container_names(host))
    )
    if drain:
        print(f"==> parking {args.host} before converging (a converge changes admission)")
        # Persistent containers use drain-all because a 1 <-> N declaration
        # change renames them. ARC admission is a direct zero scale; its
        # ephemeral controller deletes only runners that hold no job.
        rc = fcc.run(data, args.host, ["maintenance-enter"], _run)
        if rc:
            return _fail("could not drain the host; refusing to converge over running jobs")
    rc = _run_converge_transport(request)
    if drain and not parked:
        # Interactive convergence restores the declared service after either
        # outcome. The reconciler uses the parked command instead and owns
        # postcheck, receipt publication, and the eventual capacity restore.
        rc = _restore_after_converge(data, args, rc)
    return rc


def _converge_ssh(data: dict[str, Any], name: str, plays: list[str], extra: list[str]) -> int:
    """Run a host's plays with Ansible over ssh, from the control node.

    Args:
        data: The parsed declaration.
        name: Fleet host name.
        plays: Plays to run, in order.
        extra: Extra ansible flags (dry-run, teardown state).

    Returns:
        0 on success, the first failing play's status otherwise.
    """
    host = data["hosts"][name]
    for play in plays:
        argv = frm.playbook_argv(data, name, host, play, extra)
        print(f"==> ansible-playbook {fm.PLAYS[play].playbook} --limit {name}")
        rc = _run(
            argv,
            cwd=fm.ANSIBLE_DIR,
            env=frm.ansible_environment(os.environ, fm.ANSIBLE_DIR),
        )
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
    flags = [
        "-e",
        "ci_runner_docker_state=absent",
        "-e",
        "fleet_capacity_enabled=false",
    ]
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
        fcc.run(data, name, ["status"], _run)
    return 0


def cmd_reach(data: dict[str, Any], _args: argparse.Namespace) -> int:
    """Probe every declared machine over its own transport.

    The transport is the point: ``win-ci`` is not an ssh alias but a jump
    through the bench Pi into a Windows box and then into a WSL distro, and a
    reachability check that did not know that would report the fleet broken.
    Because the list AND every address come from the declaration, a machine
    added there is probed from the next run with nothing else edited, on a
    control node with no ``~/.ssh/config`` at all.

    Args:
        data: The parsed declaration.
        _args: Unused; the command takes no arguments.

    Returns:
        0 when every machine answered, 1 otherwise.
    """
    rc = 0
    for name, host in data["hosts"].items():
        # Captured, not streamed: the probe's own "ok" belongs to this function,
        # not to the operator's terminal, and a failing host's ssh chatter would
        # otherwise bury the one line that says which host failed.
        probe = subprocess.run(  # noqa: S603 -- argv built from the declaration
            [*fr.ssh_target(data, name), fm.remote_shell(host)],
            input="echo ok\n",
            text=True,
            capture_output=True,
            check=False,
        ).returncode
        where = fr.ssh_destination(host)
        if probe:
            print(f"  MISS  {name:<10} not reachable at {where}")
            rc = 1
        else:
            print(f"  ok    {name:<10} reachable at {where}")
    return rc


def cmd_capacity_quarantine(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Retain durable maintenance and drive one host to zero admission."""
    return fcc.run(data, args.host, ["quarantine"], _run)


def cmd_capacity_restore(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Restore the current quiet-hours target and clear durable maintenance."""
    return fcc.run(data, args.host, ["restore"], _run)


def cmd_scale(data: dict[str, Any], args: argparse.Namespace) -> int:
    """Change live capacity, draining idle runners during shrink."""
    return fcc.run(data, args.host, ["scale", str(args.count)], _run)


def cmd_selftest(data: dict[str, Any], _args: argparse.Namespace) -> int:
    """Run transport and typed-operation tests without contacting any host."""
    failures = (
        ftv.run_selftest()
        + fw.run_selftest(data)
        + fb.run_selftest()
        + frm.run_selftest()
        + fml.run_selftest()
        + fcc.run_selftest(data)
        + _bench_guard_inheritance_selftest()
        + _inventory_publication_selftest()
        + fm.controller_inventory_selftest(data)
        + fb.parser_selftest(_parser)
    )
    if data["hosts"]["win-ci"]["class"] not in ftv.CONTAINER_RUNNER_CLASSES:
        failures.append("declared WSL runner class was refused")
    if data["hosts"]["dev"]["class"] in ftv.CONTAINER_RUNNER_CLASSES:
        failures.append("non-container dev host was accepted as a container runner")
    for failure in failures:
        print(f"fleet.py --selftest: FAIL: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("fleet.py --selftest: PASS (typed schema, ownership/mode, quoting, redaction, cleanup)")
    return 0


def _add_converge_parsers(subs: _SubparserGroup) -> None:
    """Add apply/check/remove parsers and their shared guarded arguments."""
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
                "pass a non-secret compatibility variable through to ansible; "
                "credentials require register-runner, register-hil, or remove --vars-file"
            ),
        )
        if mode == "remove":
            sub.add_argument(
                "--vars-file",
                default="",
                help="typed mode-0600 removal/dataset vars file",
            )
        sub.add_argument(
            "--tags",
            default="",
            help="ansible tags; "
            + "/".join(sorted(fm.NO_DRAIN_TAGS))
            + " touch no container and so need no drain",
        )


def _parser() -> argparse.ArgumentParser:
    """Build the command-line parser.

    Returns:
        A parser whose subcommands mirror the module docstring.
    """
    parser = argparse.ArgumentParser(prog="fleet.py", description=__doc__.splitlines()[0])
    subs = parser.add_subparsers(dest="command", required=True)
    subs.add_parser("selftest", help="exercise typed vars and WSL rendering offline")
    subs.add_parser("list", help="what is declared, and how it is sized")
    subs.add_parser("show", help="one host in full").add_argument("host")
    subs.add_parser("validate", help="the fleet-declaration gate's check")
    subs.add_parser("reach", help="probe every machine over its declared transport")
    inv = subs.add_parser("inventory", help="write the Ansible inventory")
    inv.add_argument("--stdout", action="store_true", help="print instead of writing")
    ssh_config = subs.add_parser(
        "ssh-config", help="the fleet's host aliases, generated from the declaration"
    )
    ssh_config.add_argument(
        "--install",
        action="store_true",
        help=(
            f"write ~/.ssh/{fr.SSH_FRAGMENT_NAME} and include it from ~/.ssh/config -- "
            "the one command that makes this machine a control node"
        ),
    )
    subs.add_parser(
        "ssh-target", help="the ssh command that reaches one host from here"
    ).add_argument("host")
    register_runner = subs.add_parser(
        "register-runner", help="first-register one declared Docker runner host"
    )
    register_runner.add_argument("host")
    register_runner.add_argument("vars_file")
    subs.add_parser(
        "register-hil", help="first-register the one declared native HIL listener"
    ).add_argument("vars_file")
    _add_converge_parsers(subs)
    parked_commands = {
        "reconcile-parked-apply": ("apply", False),
        "reconcile-parked-check": ("check", False),
        "reconcile-activate": ("apply", True),
        "reconcile-activation-check": ("check", True),
    }
    for command, (mode, no_drain) in parked_commands.items():
        internal = subs.add_parser(command, help=argparse.SUPPRESS)
        internal.add_argument("host")
        internal.set_defaults(
            mode=mode,
            play=None,
            no_drain=no_drain,
            extra_var=[],
            tags="",
            vars_file="",
            trusted_tags=False,
        )
    status = subs.add_parser("status", help="what each host is running, right now")
    status.add_argument("host", nargs="?")
    for command in ("capacity-quarantine", "capacity-restore"):
        internal = subs.add_parser(command, help=argparse.SUPPRESS)
        internal.add_argument("host")
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
    original_argv = list(argv if argv is not None else sys.argv[1:])
    args = _parser().parse_args(original_argv)
    args.original_argv = original_argv
    handlers = {
        "selftest": cmd_selftest,
        "list": cmd_list,
        "show": cmd_show,
        "validate": cmd_validate,
        "reach": cmd_reach,
        "inventory": cmd_inventory,
        "ssh-config": cmd_ssh_config,
        "ssh-target": cmd_ssh_target,
        "register-runner": cmd_register_runner,
        "register-hil": cmd_register_hil,
        "check": cmd_converge,
        "apply": cmd_converge,
        "reconcile-parked-apply": cmd_converge,
        "reconcile-parked-check": cmd_converge,
        "reconcile-activate": cmd_converge,
        "reconcile-activation-check": cmd_converge,
        "remove": cmd_converge,
        "status": cmd_status,
        "capacity-quarantine": cmd_capacity_quarantine,
        "capacity-restore": cmd_capacity_restore,
        "scale": cmd_scale,
    }
    if not hasattr(args, "mode"):
        args.mode = args.command
    try:
        data = fm.load()
        if args.command in MUTATING_COMMANDS:
            if "RA8_FLEET_MUTATION_GUARDIAN_FD" not in os.environ:
                return fml.run_locked(
                    data, [sys.executable, str(Path(__file__).resolve()), *original_argv]
                )
            fml.require_guardian_capability()
        return handlers[args.command](data, args)
    except (fml.MutationLockError, fm.FleetError) as exc:
        return _fail(str(exc))


if __name__ == "__main__":
    sys.exit(main())
