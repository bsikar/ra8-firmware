#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Root boundary for the small, fixed set of RA8 bench mutations.

This executable is installed root-owned by Ansible.  The unprivileged HIL
account may invoke it through one exact sudoers executable entry, but it never
gets a shell, an arbitrary executable, an arbitrary sysfs path, or a generic
``ip``/``uhubctl`` argument surface.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import ipaddress
import json
import os
import re
import secrets
import signal
import stat
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn, Protocol

POLICY_PATH = Path("/etc/ra8-hil-privileged-policy.json")
POLICY_MODE = 0o644
ROOT_STATE_MODE = 0o600
_DEFERRED_TERMINATION = (
    signal.SIGHUP,
    signal.SIGINT,
    signal.SIGQUIT,
    signal.SIGTERM,
)


class PolicyError(RuntimeError):
    """A request failed the privileged boundary's validation policy."""


@dataclass(frozen=True)
class _CommandResult:
    """Captured result of one fixed child process."""

    returncode: int
    stdout: str
    stderr: str


@dataclass(frozen=True)
class _LiveIfaceFacts:
    """Observed facts matched against one fleet policy."""

    exists: bool
    uplinks: set[str]
    ipv4_addresses: set[str]
    mac: str
    sysfs_device: str
    phc_index: int


def _fail(message: str) -> NoReturn:
    """Reject a request without reflecting attacker-controlled text."""
    raise PolicyError(message)


def _canonical_policy(value: dict[str, object]) -> bytes:
    """Return the fleet policy bytes used by checkout/install identity."""
    selected = {
        "board_iface": value["board_iface"],
        "mac": value["mac"],
        "phc_index": value["phc_index"],
        "sysfs_device": value["sysfs_device"],
        "version": value["version"],
    }
    return (json.dumps(selected, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _strict_policy(value: object) -> dict[str, object]:
    """Validate the exact fleet-derived physical-interface policy."""
    keys = {
        "board_iface",
        "declaration_sha256",
        "mac",
        "phc_index",
        "sysfs_device",
        "version",
    }
    if not isinstance(value, dict) or set(value) != keys:
        _fail("installed helper policy has an invalid schema")
    iface = value["board_iface"]
    mac = value["mac"]
    device = value["sysfs_device"]
    digest = value["declaration_sha256"]
    if not isinstance(iface, str) or re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,14}", iface) is None:
        _fail("installed helper policy has an invalid interface")
    if not isinstance(mac, str) or re.fullmatch(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}", mac) is None:
        _fail("installed helper policy has an invalid permanent MAC")
    if (
        not isinstance(device, str)
        or re.fullmatch(r"/sys/devices/[A-Za-z0-9_.:/-]+", device) is None
    ):
        _fail("installed helper policy has an invalid sysfs identity")
    if type(value["phc_index"]) is not int or value["phc_index"] < 0:
        _fail("installed helper policy has an invalid PHC identity")
    if type(value["version"]) is not int or value["version"] != 1:
        _fail("installed helper policy has an invalid version")
    expected = hashlib.sha256(_canonical_policy(value)).hexdigest()
    if not isinstance(digest, str) or not secrets.compare_digest(digest, expected):
        _fail("installed helper policy does not match its fleet declaration digest")
    return value


def _load_policy(path: Path = POLICY_PATH) -> dict[str, object]:
    """Open the fixed root-owned policy without following a final symlink."""
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except OSError:
        _fail("installed helper policy is missing or inaccessible")
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != 0
            or stat.S_IMODE(info.st_mode) != POLICY_MODE
        ):
            _fail("installed helper policy ownership or mode is invalid")
        maximum_policy_bytes = 2048
        raw = os.read(descriptor, maximum_policy_bytes + 1)
    finally:
        os.close(descriptor)
    if len(raw) > maximum_policy_bytes:
        _fail("installed helper policy is implausibly large")
    try:
        return _strict_policy(json.loads(raw.decode("utf-8", "strict")))
    except (UnicodeError, json.JSONDecodeError):
        _fail("installed helper policy is malformed")


def _self_identity(policy: dict[str, object]) -> str:
    """Bind reviewed helper bytes and fleet policy in one reported identity."""
    helper_digest = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    return f"{helper_digest}:{policy['declaration_sha256']}"


def _validate_action(value: str) -> str:
    """Accept only the two power states supported by the bench."""
    if value not in {"off", "on"}:
        _fail("action must be off or on")
    return value


def _validate_port(value: str) -> str:
    """Accept only the three wired EK-RA8D2 ports on the fixed VIA hub."""
    if value not in {"1", "2", "4"}:
        _fail("port is not part of the declared bench topology")
    return value


def _validate_authorized(value: str) -> str:
    """Accept one exact sysfs authorization byte."""
    if value not in {"0", "1"}:
        _fail("USB authorization state must be 0 or 1")
    return value


def _validate_board_ip(value: str) -> str:
    """Accept a usable board host in the isolated 192.168.1.0/24 segment."""
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError:
        _fail("board address is not canonical IPv4")
    network = ipaddress.IPv4Network("192.168.1.0/24")
    if address not in network or address in {
        network.network_address,
        network.broadcast_address,
    }:
        _fail("board address is outside the isolated bench subnet")
    if address == ipaddress.IPv4Address("192.168.1.1"):
        _fail("board address collides with the bench host address")
    if str(address) != value:
        _fail("board address must use canonical dotted-decimal spelling")
    return value


def _validate_iface_facts(
    policy: dict[str, object],
    facts: _LiveIfaceFacts,
    allowed_ipv4: set[str] | frozenset[str] = frozenset(),
) -> str:
    """Match live facts to the one permanent fleet interface identity."""
    value = str(policy["board_iface"])
    if not facts.exists:
        _fail("interface does not exist")
    if value in facts.uplinks:
        _fail("fleet board interface carries a default route")
    if facts.ipv4_addresses - allowed_ipv4:
        _fail("fleet board interface carries a foreign IPv4 address")
    if facts.mac != policy["mac"] or facts.sysfs_device != policy["sysfs_device"]:
        _fail("live interface differs from the permanent fleet identity")
    if facts.phc_index != policy["phc_index"]:
        _fail("live interface PHC differs from the permanent fleet identity")
    return value


def _wait_child(pid: int) -> int:
    """Wait at most eight seconds for a child, then kill and reap it."""
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        waited, status_value = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            return os.waitstatus_to_exitcode(status_value)
        time.sleep(0.01)
    os.kill(pid, signal.SIGKILL)
    _, status_value = os.waitpid(pid, 0)
    _fail("fixed privileged command exceeded its time bound")


def _run(argv: list[str], *, check: bool = True) -> _CommandResult:
    """Run one fixed absolute executable with bounded, captured I/O."""
    allowed = {"/usr/sbin/ip", "/usr/sbin/uhubctl"}
    if (
        not argv
        or argv[0] not in allowed
        or any(not isinstance(arg, str) or "\0" in arg for arg in argv)
    ):
        _fail("child command is outside the fixed executable boundary")
    environment = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL": "C"}
    try:
        with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
            null_fd = os.open("/dev/null", os.O_RDONLY | os.O_CLOEXEC)
            actions = (
                (os.POSIX_SPAWN_DUP2, null_fd, 0),
                (os.POSIX_SPAWN_DUP2, output.fileno(), 1),
                (os.POSIX_SPAWN_DUP2, errors.fileno(), 2),
            )
            try:
                pid = os.posix_spawn(argv[0], argv, environment, file_actions=actions)
            finally:
                os.close(null_fd)
            returncode = _wait_child(pid)
            output.seek(0)
            errors.seek(0)
            stdout = output.read().decode("utf-8", "strict")
            stderr = errors.read().decode("utf-8", "strict")
    except (OSError, UnicodeError):
        _fail("fixed privileged command could not be executed")
    result = _CommandResult(returncode, stdout, stderr)
    if check and result.returncode != 0:
        executable = Path(argv[0]).name
        _fail(f"{executable} rejected a bounded bench operation")
    return result


def _json_command(argv: list[str]) -> object:
    """Run a read-only ip query and parse its bounded JSON response."""
    result = _run(argv)
    maximum_query_bytes = 131_072
    if len(result.stdout) > maximum_query_bytes:
        _fail("ip query returned an implausibly large response")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        _fail("ip query returned malformed JSON")


def _json_rows(value: object) -> list[dict[str, object]]:
    """Validate the top level of an iproute2 JSON response."""
    if not isinstance(value, list) or not all(isinstance(row, dict) for row in value):
        _fail("ip query returned an unexpected object")
    return value


def _iface_facts(iface: str) -> tuple[set[str], set[str], bool]:
    """Read interface existence, address ownership, uplinks, and link state."""
    link_rows = _json_rows(_json_command(["/usr/sbin/ip", "-j", "link", "show", "dev", iface]))
    if len(link_rows) != 1:
        _fail("interface identity is ambiguous")
    flags = link_rows[0].get("flags", [])
    if not isinstance(flags, list) or not all(isinstance(item, str) for item in flags):
        _fail("interface flags are malformed")
    uplinks: set[str] = set()
    for family in ("-4", "-6"):
        route_rows = _json_rows(
            _json_command(
                [
                    "/usr/sbin/ip",
                    "-j",
                    family,
                    "route",
                    "show",
                    "table",
                    "all",
                    "default",
                ]
            )
        )
        uplinks.update(row["dev"] for row in route_rows if isinstance(row.get("dev"), str))
    addr_rows = _json_rows(
        _json_command(["/usr/sbin/ip", "-j", "-4", "addr", "show", "dev", iface])
    )
    addresses: set[str] = set()
    for row in addr_rows:
        info = row.get("addr_info", [])
        if not isinstance(info, list):
            _fail("interface address data is malformed")
        for address in info:
            if isinstance(address, dict) and address.get("family") == "inet":
                local = address.get("local")
                if not isinstance(local, str):
                    _fail("interface IPv4 data is malformed")
                addresses.add(local)
    return uplinks, addresses, "UP" in flags


def _live_physical_identity(policy: dict[str, object]) -> tuple[str, str, int]:
    """Resolve the interface's MAC, physical sysfs device, and PHC."""
    iface = str(policy["board_iface"])
    sysfs_entry = Path("/sys/class/net") / iface
    try:
        device = (sysfs_entry / "device").resolve(strict=True)
        mac = (sysfs_entry / "address").read_text(encoding="ascii").strip()
    except (OSError, UnicodeError):
        _fail("fleet board interface physical identity is inaccessible")
    phc_index = int(policy["phc_index"])
    phc = device / "ptp" / f"ptp{phc_index}"
    if not phc.exists():
        _fail("fleet board interface does not expose its declared PHC")
    return mac, str(device), phc_index


def _authenticate_live_iface(
    policy: dict[str, object], allowed_ipv4: set[str] | frozenset[str]
) -> tuple[str, bool]:
    """Authenticate one physical interface and its allowed address state."""
    iface = str(policy["board_iface"])
    exists = (Path("/sys/class/net") / iface).exists()
    mac, device, phc_index = _live_physical_identity(policy)
    uplinks, addresses, is_up = _iface_facts(iface)
    return (
        _validate_iface_facts(
            policy,
            _LiveIfaceFacts(exists, uplinks, addresses, mac, device, phc_index),
            allowed_ipv4,
        ),
        is_up,
    )


def _validate_live_iface(policy: dict[str, object]) -> tuple[str, bool]:
    """Authenticate a clean board interface before a new transaction."""
    return _authenticate_live_iface(policy, frozenset())


def _authenticate_cleanup_iface(
    policy: dict[str, object], state: dict[str, object]
) -> tuple[str, bool]:
    """Authenticate stale state while allowing only its owned host address."""
    allowed = {"192.168.1.1"} if state["address_phase"] == "applied" else frozenset()
    return _authenticate_live_iface(policy, allowed)


def _state_location() -> tuple[Path, Path]:
    """Return fixed root-only state directory and file locations."""
    directory = Path("/run/ra8-hil-privileged")
    return directory, directory / "network-state.json"


def _strict_state(value: object, policy: dict[str, object]) -> dict[str, object]:
    """Validate every key and type in a root-owned cleanup record."""
    keys = {
        "version",
        "iface",
        "board_ip",
        "route_phase",
        "address_phase",
        "link_phase",
    }
    if not isinstance(value, dict) or set(value) != keys:
        _fail("network cleanup state has an invalid schema")
    if type(value["version"]) is not int or value["version"] != 1:
        _fail("network cleanup state has an invalid version")
    if value["iface"] != policy["board_iface"]:
        _fail("network cleanup state is not bound to the fleet interface")
    if not isinstance(value["board_ip"], str):
        _fail("network cleanup state has an invalid address")
    _validate_board_ip(value["board_ip"])
    phases = {"absent", "pending", "applied"}
    for key in ("route_phase", "address_phase", "link_phase"):
        if value[key] not in phases:
            _fail("network cleanup state has an invalid ownership phase")
    return value


def _ensure_state_dir(directory: Path) -> None:
    """Create and prove the fixed root-only state directory."""
    directory_mode = 0o700
    directory.mkdir(mode=directory_mode, parents=False, exist_ok=True)
    info = directory.lstat()
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != 0
        or stat.S_IMODE(info.st_mode) != directory_mode
    ):
        _fail("network state directory is not root-owned mode 0700")


def _save_state(path: Path, state: dict[str, object], policy: dict[str, object]) -> None:
    """Atomically persist cleanup ownership before each mutation."""
    _strict_state(state, policy)
    payload = (json.dumps(state, sort_keys=True, separators=(",", ":")) + "\n").encode()
    temporary = path.parent / f".network-state.{os.getpid()}.{secrets.token_hex(8)}"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW
    descriptor = os.open(temporary, flags, 0o600)
    try:
        os.write(descriptor, payload)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    temporary.replace(path)
    directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def _load_state(path: Path, policy: dict[str, object]) -> dict[str, object] | None:
    """Load cleanup state only when its inode is a small root-owned file."""
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except FileNotFoundError:
        return None
    try:
        info = os.fstat(descriptor)
        state_mode = 0o600
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != 0
            or stat.S_IMODE(info.st_mode) != state_mode
        ):
            _fail("network cleanup state ownership or mode is invalid")
        maximum_state_bytes = 4096
        if info.st_size > maximum_state_bytes:
            _fail("network cleanup state is implausibly large")
        raw = os.read(descriptor, maximum_state_bytes + 1)
    except OSError:
        _fail("network cleanup state is malformed")
    finally:
        os.close(descriptor)
    if len(raw) > maximum_state_bytes:
        _fail("network cleanup state is implausibly large")
    try:
        return _strict_state(json.loads(raw.decode("utf-8", "strict")), policy)
    except (UnicodeError, json.JSONDecodeError):
        _fail("network cleanup state is malformed")


def _route_present(board_ip: str, iface: str) -> bool:
    """Return whether the exact helper-owned host route exists."""
    rows = _json_rows(
        _json_command(["/usr/sbin/ip", "-j", "route", "show", "exact", f"{board_ip}/32"])
    )
    expected = f"{board_ip}/32"
    return any(
        row.get("dst") == expected
        and row.get("dev") == iface
        and row.get("prefsrc") == "192.168.1.1"
        for row in rows
    )


def _address_present(iface: str) -> bool:
    """Return whether the exact helper-owned host address exists."""
    _, addresses, _ = _iface_facts(iface)
    return "192.168.1.1" in addresses


def _link_is_up(iface: str) -> bool:
    """Return the current administrative UP state for one validated link."""
    _, _, is_up = _iface_facts(iface)
    return is_up


def _checkpoint_absent(
    path: Path,
    state: dict[str, object],
    policy: dict[str, object],
    key: str,
) -> None:
    """Persist that one formerly owned mutation is now absent."""
    state[key] = "absent"
    _save_state(path, state, policy)


def _cleanup_route(
    path: Path,
    state: dict[str, object],
    policy: dict[str, object],
) -> None:
    """Remove and checkpoint only an applied helper-owned route."""
    iface = str(state["iface"])
    board_ip = str(state["board_ip"])
    if state["route_phase"] == "applied" and _route_present(board_ip, iface):
        _run(
            [
                "/usr/sbin/ip",
                "route",
                "del",
                f"{board_ip}/32",
                "dev",
                iface,
                "src",
                "192.168.1.1",
            ],
        )
    if _route_present(board_ip, iface):
        _fail("owned route cleanup postcondition failed")
    if state["route_phase"] == "applied":
        _checkpoint_absent(path, state, policy, "route_phase")


def _cleanup_link(path: Path, state: dict[str, object], policy: dict[str, object]) -> None:
    """Restore and checkpoint an applied helper-owned link transition."""
    iface = str(state["iface"])
    if state["link_phase"] == "applied" and _link_is_up(iface):
        _run(["/usr/sbin/ip", "link", "set", "dev", iface, "down"])
    if state["link_phase"] == "applied" and _link_is_up(iface):
        _fail("owned link cleanup postcondition failed")
    if state["link_phase"] == "applied":
        _checkpoint_absent(path, state, policy, "link_phase")


def _cleanup_address(path: Path, state: dict[str, object], policy: dict[str, object]) -> None:
    """Remove and checkpoint only an applied helper-owned address."""
    iface = str(state["iface"])
    if state["address_phase"] == "applied" and _address_present(iface):
        _run(["/usr/sbin/ip", "addr", "del", "192.168.1.1/24", "dev", iface])
    if state["address_phase"] == "applied" and _address_present(iface):
        _fail("owned address cleanup postcondition failed")
    if state["address_phase"] == "applied":
        _checkpoint_absent(path, state, policy, "address_phase")


def _network_cleanup(path: Path, policy: dict[str, object]) -> None:
    """Recover the one serialized helper-owned network transaction."""
    state = _load_state(path, policy)
    if state is None:
        return
    phases = (state["route_phase"], state["link_phase"], state["address_phase"])
    if "pending" in phases:
        _fail("ambiguous pending network mutation requires trusted human recovery")
    _authenticate_cleanup_iface(policy, state)
    _cleanup_route(path, state, policy)
    _cleanup_link(path, state, policy)
    _cleanup_address(path, state, policy)
    path.unlink()


def _new_state(iface: str, board_ip: str) -> dict[str, object]:
    """Create a cleanup record that initially owns no mutations."""
    return {
        "version": 1,
        "iface": iface,
        "board_ip": board_ip,
        "route_phase": "absent",
        "address_phase": "absent",
        "link_phase": "absent",
    }


def _network_prepare(path: Path, policy: dict[str, object], board_arg: str) -> None:
    """Recover stale owned state, then create one bounded host-only route."""
    _network_cleanup(path, policy)
    board_ip = _validate_board_ip(board_arg)
    iface, link_was_up = _validate_live_iface(policy)
    if _route_present(board_ip, iface):
        _fail("board host route already exists without helper ownership")
    state = _new_state(iface, board_ip)
    _save_state(path, state, policy)
    state["address_phase"] = "pending"
    _save_state(path, state, policy)
    _run(["/usr/sbin/ip", "addr", "add", "192.168.1.1/24", "dev", iface, "noprefixroute"])
    if not _address_present(iface):
        _fail("owned address add postcondition failed")
    state["address_phase"] = "applied"
    _save_state(path, state, policy)
    if not link_was_up:
        state["link_phase"] = "pending"
        _save_state(path, state, policy)
        _run(["/usr/sbin/ip", "link", "set", "dev", iface, "up"])
        if not _link_is_up(iface):
            _fail("owned link transition postcondition failed")
        state["link_phase"] = "applied"
        _save_state(path, state, policy)
    state["route_phase"] = "pending"
    _save_state(path, state, policy)
    _run(
        [
            "/usr/sbin/ip",
            "route",
            "add",
            f"{board_ip}/32",
            "dev",
            iface,
            "src",
            "192.168.1.1",
        ]
    )
    if not _route_present(board_ip, iface):
        _fail("owned route add postcondition failed")
    state["route_phase"] = "applied"
    _save_state(path, state, policy)


def _network_neigh_flush(path: Path, policy: dict[str, object]) -> None:
    """Flush neighbours only on the interface authenticated by live state."""
    state = _load_state(path, policy)
    if state is None or state["address_phase"] != "applied":
        _fail("neighbour flush requires an active helper-owned network state")
    _authenticate_cleanup_iface(policy, state)
    _run(["/usr/sbin/ip", "neigh", "flush", "dev", str(state["iface"])])


def _usb_power_command(kind: str, args: list[str]) -> list[str]:
    """Build one exact uhubctl argv after validating the fixed topology."""
    port_power_argc = 2
    if kind == "usb-port-power" and len(args) == port_power_argc:
        port = _validate_port(args[0])
        action = _validate_action(args[1])
        return ["/usr/sbin/uhubctl", "-S", "-l", "2-1.3", "-p", port, "-a", action]
    if kind == "usb-root-power" and len(args) == 1:
        action = _validate_action(args[0])
        return ["/usr/sbin/uhubctl", "-S", "-l", "2-1", "-a", action]
    _fail("USB power request has an invalid argument shape")


def _usb_authorize(args: list[str]) -> None:
    """Write one byte to one of three fixed sysfs authorization files."""
    authorize_argc = 2
    if len(args) != authorize_argc:
        _fail("USB authorization request has an invalid argument shape")
    port = _validate_port(args[0])
    value = _validate_authorized(args[1])
    device_id = f"2-1.3.{port}"
    device = _resolve_usb_device(
        Path("/sys/bus/usb/devices"),
        Path("/sys/devices"),
        device_id,
    )
    directory_fd = os.open(
        device,
        os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
    )
    try:
        descriptor = os.open(
            "authorized",
            os.O_WRONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=directory_fd,
        )
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != 0:
                _fail("USB authorization attribute has an invalid inode")
            os.write(descriptor, f"{value}\n".encode("ascii"))
        finally:
            os.close(descriptor)
    finally:
        os.close(directory_fd)


def _resolve_usb_device(bus_root: Path, sys_devices: Path, device_id: str) -> Path:
    """Resolve a kernel bus symlink only beneath the canonical device tree."""
    if re.fullmatch(r"2-1\.3\.[124]", device_id) is None:
        _fail("USB device identity is outside the declared topology")
    try:
        canonical_root = sys_devices.resolve(strict=True)
        resolved = (bus_root / device_id).resolve(strict=True)
    except OSError:
        _fail("USB device identity does not resolve")
    if resolved.name != device_id or canonical_root not in resolved.parents:
        _fail("USB bus link escapes the canonical device tree")
    if not resolved.is_dir():
        _fail("USB device identity is not a directory")
    return resolved


def _lock_descriptor() -> int:
    """Open and prove the fixed serialization lock without following links."""
    path = "/run/lock/ra8-hil-privileged.lock"
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    info = os.fstat(descriptor)
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0:
        os.close(descriptor)
        _fail("privileged serialization lock is not root-owned")
    return descriptor


@dataclass(frozen=True)
class _CycleAction:
    """One fixed power or authorization action inside a cycle."""

    kind: str
    args: tuple[str, ...]


class _CycleOps(Protocol):
    """Injectable cycle journal and device backend."""

    def save(self, path: Path, state: dict[str, object]) -> None:
        """Persist pending restoration."""

    def apply(self, action: _CycleAction) -> None:
        """Apply one exact device action."""

    def pause(self) -> None:
        """Wait the fixed cycle interval."""

    def clear(self, path: Path) -> None:
        """Clear a restored journal."""


def _strict_restore(value: object) -> dict[str, object]:
    """Validate one pending USB restoration journal."""
    if not isinstance(value, dict) or set(value) != {
        "kind",
        "port",
        "restore",
        "version",
    }:
        _fail("USB restore journal has an invalid schema")
    if value["version"] != 1 or value["kind"] not in {
        "port-power",
        "root-power",
        "authorize",
    }:
        _fail("USB restore journal has an invalid operation")
    port = value["port"]
    if value["kind"] == "root-power":
        if port != "" or value["restore"] != "on":
            _fail("USB restore journal has invalid root-power state")
    elif not isinstance(port, str) or _validate_port(port) != port:
        _fail("USB restore journal has an invalid port")
    elif value["restore"] != ("1" if value["kind"] == "authorize" else "on"):
        _fail("USB restore journal has an invalid restore action")
    return value


def _save_restore(path: Path, state: dict[str, object]) -> None:
    """Atomically record restoration before applying a transient off action."""
    payload = (
        json.dumps(_strict_restore(state), sort_keys=True, separators=(",", ":")) + "\n"
    ).encode()
    temporary = path.parent / f".restore-state.{os.getpid()}.{secrets.token_hex(8)}"
    descriptor = os.open(
        temporary,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o600,
    )
    try:
        os.write(descriptor, payload)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    temporary.replace(path)


def _load_restore(path: Path) -> dict[str, object] | None:
    """Load a small root-owned pending USB restoration journal."""
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    except FileNotFoundError:
        return None
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != 0
            or stat.S_IMODE(info.st_mode) != ROOT_STATE_MODE
        ):
            _fail("USB restore journal ownership or mode is invalid")
        maximum_restore_bytes = 512
        raw = os.read(descriptor, maximum_restore_bytes + 1)
    finally:
        os.close(descriptor)
    if len(raw) > maximum_restore_bytes:
        _fail("USB restore journal is implausibly large")
    try:
        return _strict_restore(json.loads(raw.decode("utf-8", "strict")))
    except (UnicodeError, json.JSONDecodeError):
        _fail("USB restore journal is malformed")


def _restore_action(state: dict[str, object]) -> _CycleAction:
    """Map a validated journal onto one exact restore action."""
    state = _strict_restore(state)
    if state["kind"] == "root-power":
        return _CycleAction("root-power", ("on",))
    if state["kind"] == "port-power":
        return _CycleAction("port-power", (str(state["port"]), "on"))
    return _CycleAction("authorize", (str(state["port"]), "1"))


class _LiveCycleOps:
    """Production cycle backend using only fixed helper operations."""

    def save(self, path: Path, state: dict[str, object]) -> None:
        """Persist one pending restore."""
        _save_restore(path, state)

    def apply(self, action: _CycleAction) -> None:
        """Apply an exact fixed-topology action."""
        if action.kind == "authorize":
            _usb_authorize(list(action.args))
        else:
            command = "usb-root-power" if action.kind == "root-power" else "usb-port-power"
            _run(_usb_power_command(command, list(action.args)))

    def pause(self) -> None:
        """Wait the fixed recovery interval."""
        time.sleep(1)

    def clear(self, path: Path) -> None:
        """Remove a successfully restored journal."""
        path.unlink()


def _perform_cycle(
    path: Path,
    state: dict[str, object],
    off: _CycleAction,
    ops: _CycleOps,
) -> None:
    """Defer catchable termination; a journal covers uncatchable SIGKILL."""
    pending: list[int] = []

    def remember(signum: int, _frame: object) -> None:
        if not pending:
            pending.append(signum)

    previous: dict[int, object] = {}
    try:
        for signum in _DEFERRED_TERMINATION:
            previous[signum] = signal.signal(signum, remember)
        ops.save(path, state)
        try:
            ops.apply(off)
            ops.pause()
        finally:
            ops.apply(_restore_action(state))
            ops.clear(path)
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)
    if pending:
        os.kill(os.getpid(), pending[0])


def _recover_restore(path: Path, ops: _CycleOps) -> None:
    """Restore a prior interrupted cycle before accepting another mutation."""
    state = _load_restore(path)
    if state is None:
        return
    ops.apply(_restore_action(state))
    ops.clear(path)


def _cycle_request(command: str, args: list[str]) -> tuple[dict[str, object], _CycleAction]:
    """Parse one exact transactional USB cycle request."""
    if command == "usb-root-cycle" and not args:
        return {
            "kind": "root-power",
            "port": "",
            "restore": "on",
            "version": 1,
        }, _CycleAction("root-power", ("off",))
    if command in {"usb-port-cycle", "usb-authorize-cycle"} and len(args) == 1:
        port = _validate_port(args[0])
        if command == "usb-port-cycle":
            return {
                "kind": "port-power",
                "port": port,
                "restore": "on",
                "version": 1,
            }, _CycleAction("port-power", (port, "off"))
        return {
            "kind": "authorize",
            "port": port,
            "restore": "1",
            "version": 1,
        }, _CycleAction("authorize", (port, "0"))
    _fail("USB cycle request has an invalid argument shape")


def _mutate(command: str, args: list[str], policy: dict[str, object]) -> None:
    """Serialize and dispatch one validated privileged mutation."""
    if os.geteuid() != 0:
        _fail("mutating commands require root")
    directory, state_path = _state_location()
    _ensure_state_dir(directory)
    restore_path = directory / "restore-state.json"
    cycle_ops = _LiveCycleOps()
    descriptor = _lock_descriptor()
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        _recover_restore(restore_path, cycle_ops)
        if command in {"usb-port-power", "usb-root-power"}:
            _run(_usb_power_command(command, args))
        elif command == "usb-authorize":
            _usb_authorize(args)
        elif command in {"usb-port-cycle", "usb-root-cycle", "usb-authorize-cycle"}:
            state, off = _cycle_request(command, args)
            _perform_cycle(restore_path, state, off, cycle_ops)
        elif command == "net-prepare" and len(args) == 1:
            _network_prepare(state_path, policy, args[0])
        elif command == "net-neigh-flush" and not args:
            _network_neigh_flush(state_path, policy)
        elif command == "net-cleanup" and not args:
            _network_cleanup(state_path, policy)
        else:
            _fail("unknown command or invalid argument count")
    finally:
        os.close(descriptor)


def _parser() -> argparse.ArgumentParser:
    """Build the exact command-line interface."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?")
    parser.add_argument("args", nargs="*")
    parser.add_argument("--identity", action="store_true")
    parser.add_argument("--policy-interface", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    """Dispatch identity/selftest reads or one root-only mutation."""
    args = _parser().parse_args(argv)
    result = 2
    if args.identity or args.policy_interface:
        if args.command is None and not args.args:
            try:
                policy = _load_policy()
                value = _self_identity(policy) if args.identity else policy["board_iface"]
                print(value)
                result = 0
            except PolicyError as exc:
                print(f"ra8-hil-privileged: {exc}", file=sys.stderr)
    elif args.command is not None:
        try:
            _mutate(args.command, args.args, _load_policy())
            result = 0
        except PolicyError as exc:
            print(f"ra8-hil-privileged: {exc}", file=sys.stderr)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
