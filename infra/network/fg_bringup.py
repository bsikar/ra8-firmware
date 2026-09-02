#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""FortiGate 81E-POE login + bench bring-up over the serial console.

Uses the login mechanics proven on this exact unit: send the line terminated by
a bare CR (a trailing LF submits an empty password and desyncs the login), then
read until a SUBSTRING match (case-insensitive, because FortiOS capitalises
"New Password:"), with chunked reads into a per-step buffer that is reset each
step so a stale echo cannot self-match.

Offline modes lint or render the tracked declaration. Explicit live modes log
in, bootstrap/configure/verify the firewall, or inspect/configure the AP.

Every credential is read from OpenBao and masked in the transcript.
"""

from __future__ import annotations

import contextlib
import importlib
import importlib.util
import os
import shlex
import sys
import time
from collections.abc import Callable
from pathlib import Path
from types import ModuleType
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import serial


def _load_exact_module(module_name: str, module_path: Path) -> ModuleType:
    """Load one repository-owned module without adding a directory to sys.path.

    The resolved file must remain in the requested directory, so a sibling
    symlink cannot redirect execution outside the reviewed tree. Registering
    the private module key before execution keeps dataclass type resolution
    correct while preventing an ambient module with the public name from
    satisfying the import.
    """
    expected_parent = module_path.parent.resolve(strict=True)
    resolved_path = module_path.resolve(strict=True)
    if resolved_path.parent != expected_parent or not resolved_path.is_file():
        msg = f"refusing non-local Python module {module_path}"
        raise ImportError(msg)
    private_name = f"_ra8_exact_{module_name}"
    spec = importlib.util.spec_from_file_location(private_name, resolved_path)
    if spec is None or spec.loader is None:
        msg = f"cannot load repository Python module {resolved_path}"
        raise ImportError(msg)
    module = importlib.util.module_from_spec(spec)
    sys.modules[private_name] = module
    loaded = False
    try:
        spec.loader.exec_module(module)
        loaded = True
    finally:
        if not loaded:
            sys.modules.pop(private_name, None)
    return module


_network_dir = Path(__file__).resolve().parent
_fg_ap_safety = _load_exact_module("fg_ap_safety", _network_dir / "fg_ap_safety.py")
_fg_selftest = _load_exact_module("fg_bringup_selftest", _network_dir / "fg_bringup_selftest.py")
_fortigate_config = _load_exact_module("fortigate_config", _network_dir / "fortigate_config.py")

AP_SETUP_COMMANDS = _fg_ap_safety.AP_SETUP_COMMANDS
_ap_checked_command = _fg_ap_safety.checked_command
_mask_secrets = _fg_ap_safety.mask_secrets
_ap_status_succeeded = _fg_ap_safety.status_succeeded
_uci_assignment = _fg_ap_safety.uci_assignment
_validate_wpa2_psk = _fg_ap_safety.validate_wpa2_psk
entrypoint_safety_checks = _fg_selftest.entrypoint_safety_checks
isolated_import_checks = _fg_selftest.isolated_import_checks
psk_selftest_checks = _fg_selftest.psk_selftest_checks
cli_argument_selftest_checks = _fg_selftest.cli_argument_selftest_checks
declaration_selftest_checks = _fg_selftest.declaration_selftest_checks
just_recipe_selftest_checks = _fg_selftest.just_recipe_selftest_checks
live_runtime_selftest_checks = _fg_selftest.live_runtime_selftest_checks
DEFAULT_CONF = _fortigate_config.DEFAULT_CONF
config_lint_errors = _fortigate_config.config_lint_errors
load_config_lines = _fortigate_config.load_config_lines
read_valid_config = _fortigate_config.read_valid_config
render_config_lines = _fortigate_config.render_config_lines
require_valid_config = _fortigate_config.require_valid_config

# The console cable is addressed by DEVICE CLASS, not by its serial number, for
# the same two reasons scripts/hil/lib/tty_resolve.sh gives: /dev/ttyUSB<n> is
# assignment order rather than identity, and baking a maintainer-specific serial
# into the tree puts bench hardware identifiers in a public repo. Pin a specific
# cable with FG_CONSOLE_TTY (or OpenBao `console_tty`) when two FTDI adapters
# are attached.
TTY_DIR = Path("/dev/serial/by-id")
TTY_PATTERN = "usb-FTDI_FT232R_USB_UART_*-if00-port0"
TTY_GLOB = f"{TTY_DIR}/{TTY_PATTERN}"
TTY_ENV = "FG_CONSOLE_TTY"
BAUD = 9600
BENCH = Path.home() / "ra8-bench"
LOG = BENCH / "fortigate_console.log"
STATUS_FILE = BENCH / "fg_bringup.status"
ARGC_IMPLICIT_LOGIN = 1
ARGC_MODE_ONLY = 2
ARGC_WITH_PATH = 3
ARGC_WITH_PATH_AND_LAN = 4

# read_until() indices for the post-wipe blank-password probe. The needle list
# is ["forced", "new password", "#", "$", "incorrect"].
PWP_FORCED = 0
PWP_NEWPW = 1
PWP_HASH = 2
PWP_DOLLAR = 3
PWP_INCORRECT = 4


def resolve_tty() -> str:
    """Return the FortiGate console device, resolved by identity.

    Order: the FG_CONSOLE_TTY override, then the single by-id path matching
    TTY_GLOB. Zero or several matches is a hard error naming the override --
    never a fallback to a guessed /dev/ttyUSB0, because reads from the wrong
    device look exactly like a console that answered nothing.
    """
    override = os.environ.get(TTY_ENV, "").strip()
    if override:
        return override
    hits = sorted(str(p) for p in TTY_DIR.glob(TTY_PATTERN))
    if len(hits) == 1:
        return hits[0]
    if not hits:
        msg = (
            f"fg_bringup: no console matches {TTY_GLOB} -- is the FTDI cable "
            f"plugged into this host? Set {TTY_ENV} to pin one explicitly."
        )
        raise SystemExit(msg)
    msg = (
        f"fg_bringup: {len(hits)} consoles match {TTY_GLOB} ({', '.join(hits)}) -- "
        f"set {TTY_ENV} to pin the FortiGate cable."
    )
    raise SystemExit(msg)


_SECRETS: list[str] = []


def redact(value: str) -> None:
    """Register a secret value so it is masked everywhere in the transcript."""
    if value and value not in _SECRETS:
        _SECRETS.append(value)


def mask(text: str) -> str:
    """Return text with every registered secret replaced by <REDACTED>."""
    return _mask_secrets(text, _SECRETS)


def log(msg: str) -> None:
    """Write one masked, timestamped line to stdout and the transcript file."""
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {mask(msg)}"
    print(line, flush=True)
    with LOG.open("a", encoding="utf-8", errors="replace") as fh:
        fh.write(line + "\n")


def status(msg: str) -> None:
    """Log a status line and mirror it (masked) to the one-line status file."""
    log(f"BRINGUP_STATUS: {msg}")
    STATUS_FILE.write_text(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {mask(msg)}\n", encoding="utf-8")


def creds() -> dict[str, str]:
    """Return the bench-network secret from OpenBao (read-only AppRole)."""
    repo_client = Path(__file__).resolve().parents[2] / "scripts/secrets/openbao_client.py"
    deployed_client = Path.home() / "ra8-firmware/scripts/secrets/openbao_client.py"
    client_path = repo_client if repo_client.is_file() else deployed_client
    openbao = _load_exact_module("openbao_client", client_path)
    return openbao.OpenBaoClient(openbao.load_config()).kv_get("ra8d2/bench-network")


def send(ser: serial.Serial, text: str, *, secret: bool = False) -> None:
    """Send one line terminated by a bare CR; secret text is masked in the log.

    A trailing LF would be submitted as a SECOND line by the FortiOS login
    prompt (the empty password after the username), desyncing the whole
    sequence -- so only a carriage return terminates the line.
    """
    log(f">>> {'<REDACTED>' if secret else text}")
    ser.write((text + "\r").encode("ascii", "replace"))
    ser.flush()


def read_until(ser: serial.Serial, needles: list[str], timeout: float) -> tuple[int, str]:
    """Chunked read into a fresh buffer until a needle SUBSTRING appears.

    Matching is CASE-INSENSITIVE: FortiOS prints "New Password:" /
    "Confirm Password:" with capitals, and a case-sensitive needle silently
    desynced the forced-password-change dialog.
    """
    buf = ""
    low_needles = [n.lower() for n in needles]
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk.decode("ascii", "replace")
            low = buf.lower()
            for idx, needle in enumerate(low_needles):
                if needle in low:
                    log(f"<<< {buf.strip()}")
                    return idx, buf
        time.sleep(0.2)
    log(f"<<< (timeout {timeout}s) {buf.strip()}")
    return -1, buf


def clean_prompt(ser: serial.Serial) -> int:
    """Nudge to a clean state. Returns 0=login prompt, 1=shell prompt, -1=unknown."""
    ser.reset_input_buffer()
    ser.write(b"\r")
    ser.flush()
    time.sleep(1.0)
    idx, _ = read_until(ser, ["login:", "#", "$"], 6)
    if idx == 0:
        return 0
    if idx in (1, 2):
        return 1
    # try once more
    ser.write(b"\r")
    ser.flush()
    time.sleep(1.0)
    idx, _ = read_until(ser, ["login:", "#", "$"], 6)
    if idx == 0:
        return 0
    if idx in (1, 2):
        return 1
    return -1


def login(ser: serial.Serial, user: str, password: str) -> bool:
    """Robust login. Returns True at a shell prompt."""
    redact(password)
    state = clean_prompt(ser)
    if state == 1:
        log("already at a shell prompt")
        return True
    if state != 0:
        status("no login/shell prompt -- console not responding cleanly")
        return False
    send(ser, user)
    if read_until(ser, ["assword"], 8)[0] != 0:
        status("never saw a password prompt after sending the username")
        return False
    time.sleep(0.4)
    send(ser, password, secret=True)
    idx, _ = read_until(ser, ["ncorrect", "#", "$", "accept", "(y/n)"], 10)
    if idx in (1, 2):
        log("login OK (shell prompt)")
        return True
    if idx in (3, 4):  # a post-login disclaimer wants acceptance
        send(ser, "a")
        if read_until(ser, ["#", "$"], 8)[0] in (0, 1):
            log("login OK after accepting the disclaimer")
            return True
    status("login rejected (Login incorrect) -- password did not authenticate")
    return False


def run_lines(ser: serial.Serial, lines: list[str], timeout: float = 60.0) -> None:
    """Send each CLI line and wait for the next FortiOS prompt after it."""
    for line in lines:
        send(ser, line)
        read_until(ser, ["#", "$"], timeout)


def capture(ser: serial.Serial) -> str:
    """Read-only: disable paging, dump status + interfaces. Returns the text."""
    run_lines(ser, ["config system console", "set output standard", "end"], 20)
    out = ""
    for cmd in (
        "get system status",
        "show system interface",
        "show system dhcp server",
    ):
        send(ser, cmd)
        _, buf = read_until(ser, ["#", "$"], 60)
        out += f"\n===== {cmd} =====\n{buf}\n"
    return out


def primary_lan_name(interface_text: str) -> str:
    """Return the PRIMARY LAN hard-switch name from a `show system interface` dump.

    The bench splits the one physical switch (sw0) into two hard switches: the
    primary (factory-named "lan" or "internal", depending on the model) carries
    the odd ports, and a SECOND switch named "lan-even" carries the even ports.
    Only the primary name is model-dependent, so only the primary is what gets
    resolved here and what configure_after_wipe() rewrites the "internal" token
    in fortigate-bench.conf to. "lan-even" is a literal we choose, and it is
    deliberately NOT confused with the primary: the needle `edit "lan"` cannot
    match inside `edit "lan-even"` (the closing quote differs), so the presence
    of the second switch cannot make this misreport the primary. See --selftest.
    """
    for name in ("lan", "internal"):
        if f'edit "{name}"' in interface_text:
            return name
    return "internal"


def run_config_lint(path: Path) -> int:
    """Validate a FortiOS declaration without touching credentials or hardware."""
    try:
        lines = read_valid_config(path)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"CONFIG LINT: FAIL: {exc}", file=sys.stderr)
        return 1
    print(f"CONFIG LINT: PASS ({len(lines)} commands from {path})")
    return 0


def run_replay_dry_run(path: Path, lan: str) -> int:
    """Emit the validated command stream without opening the console."""
    try:
        lines = read_valid_config(path, lan)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"REPLAY DRY RUN: FAIL: {exc}", file=sys.stderr)
        return 1
    for line in lines:
        print(line)
    print(
        f"REPLAY DRY RUN: PASS ({len(lines)} commands from {path}, LAN {lan!r})",
        file=sys.stderr,
    )
    return 0


def detect_lan(ser: serial.Serial) -> str:
    """Return the LAN interface name ('lan' or 'internal') from the live config."""
    send(ser, "show system interface")
    _, buf = read_until(ser, ["#", "$"], 60)
    return primary_lan_name(buf)


def do_login_mode(ser: serial.Serial, c: dict[str, str]) -> int:
    """Log in, capture the running config for the record, then log out."""
    if not login(ser, c["fortigate_admin_user"], c["fortigate_admin_pass"]):
        return 2
    status("LOGIN OK -- capturing current (junk) config for the record")
    text = capture(ser)
    (BENCH / "fortigate_prewipe_config.txt").write_text(mask(text), encoding="utf-8")
    serial_ok = c.get("fortigate_serial", "") in text
    status(f"captured pre-wipe config; serial-match={serial_ok}")
    send(ser, "exit")
    time.sleep(1)
    return 0


def _complete_forced_change(ser: serial.Serial, pw: str) -> str:
    """Answer the forced first-login password change, setting the password to pw.

    Returns 'pw' on success, 'fail' otherwise.
    """
    send(ser, pw, secret=True)
    if read_until(ser, ["confirm"], 15)[0] != 0:
        return "fail"
    time.sleep(0.4)
    send(ser, pw, secret=True)
    done, _ = read_until(ser, ["#", "$", "incorrect", "do not match", "sorry"], 25)
    return "pw" if done in (0, 1) else "fail"


def post_wipe_login(ser: serial.Serial, user: str, pw: str) -> str:
    """Log in on a freshly factory-reset unit and report the password state.

    Returns 'pw' when the admin password is now the OpenBao value, 'blank' when
    the account logged in with an empty password and still needs one set, or
    'fail'. v6.4.6 forces a password change on the first login, so the common
    path sets the password to pw while logging in and returns 'pw'.
    """
    state = clean_prompt(ser)
    if state != 0:
        return "pw" if state == 1 else "fail"
    send(ser, user)
    if read_until(ser, ["assword"], 12)[0] != 0:
        return "fail"
    time.sleep(0.4)
    send(ser, "", secret=True)  # factory default: empty password
    idx, _ = read_until(ser, ["forced", "new password", "#", "$", "incorrect"], 20)
    if idx in (PWP_FORCED, PWP_NEWPW):
        return _complete_forced_change(ser, pw)
    if idx in (PWP_HASH, PWP_DOLLAR):
        return "blank"
    if idx == PWP_INCORRECT:  # blank rejected -> a prior run already set pw
        return "pw" if login(ser, user, pw) else "fail"
    return "fail"


def configure_after_wipe(
    ser: serial.Serial,
    c: dict[str, str],
    declaration_lines: list[str],
    declaration_path: Path,
) -> int:
    """Shared post-wipe path: log in, ensure admin password = pw, replay conf."""
    user, pw = c["fortigate_admin_user"], c["fortigate_admin_pass"]
    result = post_wipe_login(ser, user, pw)
    if result == "fail":
        status("post-wipe login failed -- stopping for a human")
        return 2
    status(f"post-wipe login OK (password state: {result})")
    # Only set the password when it is still blank -- setting your OWN password
    # logs the session out, so we must NOT do it when the forced change already
    # made it pw. When we do set it, re-login afterwards.
    if result == "blank":
        run_lines(ser, ["config system admin", f"edit {user}"], 20)
        send(ser, f"set password {pw}", secret=True)
        read_until(ser, ["#", "$"], 20)
        run_lines(ser, ["next", "end"], 20)
        read_until(ser, ["login:", "#", "$"], 20)
        if not login(ser, user, pw):
            status("re-login after setting password failed")
            return 2
    status("admin password confirmed = OpenBao value")
    run_lines(ser, ["config system console", "set output standard", "end"], 20)

    lan = detect_lan(ser)
    status(f"replaying bench config on LAN interface '{lan}'")
    conf_lines = render_config_lines(declaration_lines, lan)
    require_valid_config(conf_lines, declaration_path)
    run_lines(ser, conf_lines, 60)
    run_lines(ser, ["get system status", f"show system interface {lan}", "get system poe"], 40)
    status("DONE: FortiGate wiped and bench-configured (10.0.40.1/24) -- ready for AP")
    return 0


def do_configure_mode(
    ser: serial.Serial,
    c: dict[str, str],
    declaration_lines: list[str],
    declaration_path: Path,
) -> int:
    """Configure an already-wiped unit (skips factoryreset)."""
    return configure_after_wipe(ser, c, declaration_lines, declaration_path)


def drain(ser: serial.Serial, seconds: float) -> str:
    """Read and log everything for a fixed window, returning the text.

    Used for the nested AP shell, whose '#' prompt is indistinguishable from
    the FortiOS one, so a prompt-based read would be ambiguous.
    """
    buf = ""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk.decode("ascii", "replace")
        time.sleep(0.15)
    if buf.strip():
        log(f"<<< {buf.strip()}")
    return buf


def ap_cmd(ser: serial.Serial, cmd: str, seconds: float = 4.0, *, secret: bool = False) -> str:
    """Send a command to the nested AP shell and drain its output for a window."""
    send(ser, cmd, secret=secret)
    return drain(ser, seconds)


def ap_cmd_checked(
    ser: serial.Serial,
    cmd: str,
    seconds: float = 4.0,
    *,
    secret: bool = False,
    transport: tuple[Callable[..., None], Callable[..., str]] = (send, drain),
) -> str:
    """Run one AP command and require its fixed exit-status marker."""
    sender, drainer = transport
    wrapped = _ap_checked_command(cmd)
    sender(ser, wrapped, secret=secret)
    output = drainer(ser, seconds)
    if not _ap_status_succeeded(output):
        message = "AP command failed or returned an ambiguous status"
        raise RuntimeError(message)
    return output


def ap_ssh_open(ser: serial.Serial, ip: str, user: str, pw: str) -> bool:
    """Open a nested ssh from the FortiGate console into the OpenWrt AP."""
    send(ser, f"execute ssh {user}@{ip}")
    idx, _ = read_until(ser, ["yes/no", "assword"], 25)
    if idx == 0:  # first-time host-key acceptance
        send(ser, "yes")
        if read_until(ser, ["assword"], 15)[0] != 0:
            return False
    send(ser, pw, secret=True)
    time.sleep(1.5)
    # Confirm we are inside the OpenWrt shell: the marker appears only in the
    # command OUTPUT, never in the echoed command line (quotes break it up).
    out = ap_cmd(ser, 'echo AP""_READY_$(uci get system.@system[0].hostname 2>/dev/null)', 6)
    return "AP_READY" in out


def ap_ssh_close(ser: serial.Serial) -> None:
    """Leave the nested AP ssh session and return to the FortiOS prompt."""
    send(ser, "exit")
    drain(ser, 3)


def do_ap_inspect_mode(ser: serial.Serial, c: dict[str, str]) -> int:
    """Log into the FortiGate, jump to the AP, dump its wireless/network."""
    if not login(ser, c["fortigate_admin_user"], c["fortigate_admin_pass"]):
        status("ap-inspect: FortiGate login failed")
        return 2
    run_lines(ser, ["config system console", "set output standard", "end"], 20)
    status("ap-inspect: reaching the AP over execute ssh")
    if not ap_ssh_open(ser, c["ap_ip"], c["ap_ssh_user"], c["ap_ssh_pass"]):
        status("ap-inspect: could not open the nested ssh to the AP")
        return 2
    status("ap-inspect: inside the OpenWrt shell")
    for cmd in (
        "cat /etc/openwrt_release",
        "iwinfo | grep -E 'ESSID|Mode|Channel|Hardware'",
        "uci show wireless",
        "uci show network.lan",
        "ip -4 addr show br-lan",
    ):
        ap_cmd(ser, cmd, 5)
    ap_ssh_close(ser)
    status("ap-inspect complete")
    return 0


def do_ap_configure_mode(ser: serial.Serial, c: dict[str, str]) -> int:
    """Jump to the AP and stand up the ra8-bench 2.4 GHz SSID (WPA2-PSK).

    radio1 is the 2.4 GHz radio on the MR18. The orphaned iot/guest SSIDs
    (their VLAN networks were wiped from the FortiGate) are disabled; the
    working home-network on 'lan' is left alone.
    """
    try:
        _validate_wpa2_psk(c["bench_psk"])
    except ValueError:
        status("ap-configure: bench_psk must be a printable 8..63 byte WPA2 passphrase")
        return 2
    if not login(ser, c["fortigate_admin_user"], c["fortigate_admin_pass"]):
        status("ap-configure: FortiGate login failed")
        return 2
    run_lines(ser, ["config system console", "set output standard", "end"], 20)
    status("ap-configure: reaching the AP over execute ssh")
    if not ap_ssh_open(ser, c["ap_ip"], c["ap_ssh_user"], c["ap_ssh_pass"]):
        status("ap-configure: could not open the nested ssh to the AP")
        return 2
    status("ap-configure: inside OpenWrt -- applying uci")
    try:
        for cmd in AP_SETUP_COMMANDS:
            ap_cmd_checked(ser, cmd, 2)
        redact(shlex.quote(c["bench_psk"]))
        psk_command = _uci_assignment("wireless.bench.key", c["bench_psk"])
        ap_cmd_checked(ser, psk_command, 2, secret=True)
        ap_cmd_checked(ser, "uci commit wireless", 4)
        ap_cmd_checked(ser, "uci commit dhcp", 4)
        status("ap-configure: committed; reloading wifi + dhcp")
        ap_cmd_checked(ser, "wifi reload", 12)
        ap_cmd_checked(ser, "/etc/init.d/dnsmasq restart", 6)
        ap_cmd_checked(ser, "/etc/init.d/odhcpd restart", 6)
        verify = (
            'test "$(uci -q get wireless.bench.ssid)" = ra8-bench && '
            'test "$(uci -q get wireless.bench.encryption)" = psk2 && '
            'test "$(uci -q get dhcp.lan.ignore)" = 1 && '
            'iwinfo 2>/dev/null | grep -F "ESSID: \\"ra8-bench\\"" >/dev/null'
        )
        ap_cmd_checked(ser, verify, 12)
    except RuntimeError:
        status("ap-configure: AP command or final SSID/hostapd assertion failed")
        ap_ssh_close(ser)
        return 2
    ap_ssh_close(ser)
    status("ap-configure complete: ra8-bench up on radio1 (2.4 GHz), dumb-AP DHCP off")
    return 0


def do_ap_exec_mode(ser: serial.Serial, c: dict[str, str]) -> int:
    """Jump to the AP and run each command passed after the mode argument."""
    cmds = sys.argv[2:]
    if not login(ser, c["fortigate_admin_user"], c["fortigate_admin_pass"]):
        status("ap-exec: FortiGate login failed")
        return 2
    run_lines(ser, ["config system console", "set output standard", "end"], 20)
    if not ap_ssh_open(ser, c["ap_ip"], c["ap_ssh_user"], c["ap_ssh_pass"]):
        status("ap-exec: could not open the nested ssh to the AP")
        return 2
    for cmd in cmds:
        ap_cmd(ser, cmd, 6)
    ap_ssh_close(ser)
    status("ap-exec complete")
    return 0


def do_ap_status_mode(ser: serial.Serial, c: dict[str, str]) -> int:
    """Jump to the AP and confirm ra8-bench hostapd is actually beaconing."""
    if not login(ser, c["fortigate_admin_user"], c["fortigate_admin_pass"]):
        status("ap-status: FortiGate login failed")
        return 2
    run_lines(ser, ["config system console", "set output standard", "end"], 20)
    if not ap_ssh_open(ser, c["ap_ip"], c["ap_ssh_user"], c["ap_ssh_pass"]):
        status("ap-status: could not open the nested ssh to the AP")
        return 2
    for cmd in (
        "iwinfo phy2-ap1 info",
        "iw dev | grep -E 'Interface|ssid|type|txpower'",
        "logread 2>/dev/null | grep -iE 'phy2-ap1|hostapd' | tail -12",
        "ubus list | grep hostapd",
    ):
        ap_cmd(ser, cmd, 6)
    ap_ssh_close(ser)
    status("ap-status complete")
    return 0


def do_verify_mode(ser: serial.Serial, c: dict[str, str]) -> int:
    """Log in and run read-only checks, incl. reaching the AP at 10.0.40.10."""
    if not login(ser, c["fortigate_admin_user"], c["fortigate_admin_pass"]):
        status("verify login failed")
        return 2
    run_lines(ser, ["config system console", "set output standard", "end"], 20)
    for cmd in (
        "get system status",
        "show system dhcp server",
        "get router info routing-table all",
        "get system interface physical",
        "execute ping-options repeat-count 3",
        "execute ping 10.0.40.10",
        "diagnose ip arp list",
    ):
        send(ser, cmd)
        read_until(ser, ["#", "$"], 45)
    status("verify complete")
    return 0


def do_bootstrap_mode(
    ser: serial.Serial,
    c: dict[str, str],
    declaration_lines: list[str],
    declaration_path: Path,
) -> int:
    """Log in, factory-reset, then configure the wiped unit end to end."""
    user, pw = c["fortigate_admin_user"], c["fortigate_admin_pass"]
    if not login(ser, user, pw):
        return 2
    status("LOGIN OK -- issuing execute factoryreset")
    run_lines(ser, ["config system console", "set output standard", "end"], 20)
    send(ser, "execute factoryreset")
    if read_until(ser, ["(y/n)", "y/n"], 20)[0] < 0:
        status("factoryreset did not prompt for confirmation -- aborting")
        return 2
    send(ser, "y")
    status("factoryreset confirmed -- device rebooting wiped")
    time.sleep(20)

    # Post-wipe: wait passively for the login prompt (no CR during BIOS).
    status("waiting for the wiped boot to reach a login prompt")
    deadline = time.monotonic() + 360
    got_login = False
    while time.monotonic() < deadline:
        if read_until(ser, ["login:"], 10)[0] == 0:
            got_login = True
            break
    if not got_login:
        status("wiped device never reached a login prompt")
        return 2
    return configure_after_wipe(ser, c, declaration_lines, declaration_path)


def run_selftest(*, config_only: bool = False) -> int:
    """Run offline both-direction regression checks without secrets or hardware."""
    config_checks, tracked_errors = declaration_selftest_checks(
        primary_lan_name,
        render_config_lines,
        _fortigate_config,
    )
    recipe_checks = just_recipe_selftest_checks(
        Path(__file__).resolve().parents[2] / "just/infra.just"
    )
    checks = recipe_checks + config_checks
    if not config_only:
        checks = (
            isolated_import_checks(Path(__file__).resolve(), _load_exact_module)
            + cli_argument_selftest_checks(_live_arg_count_valid)
            + live_runtime_selftest_checks(
                main,
                _live_runtime_safe,
                Path(__file__).resolve().parents[2] / ".venv",
            )
            + recipe_checks
            + entrypoint_safety_checks(main)
            + psk_selftest_checks(
                _uci_assignment,
                redact,
                mask,
                _SECRETS,
                (_validate_wpa2_psk, ap_cmd_checked),
            )
            + config_checks
        )
    ok = True
    for label, good in checks:
        ok = ok and good
        print(f"  [{'PASS' if good else 'FAIL'}] {label}")
    for error in tracked_errors:
        print(f"    {error}")

    label = "CONFIG SELFTEST" if config_only else "SELFTEST"
    print(f"{label}:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def usage_error(message: str) -> int:
    """Print a command-line error without touching credentials or hardware."""
    print(f"fg_bringup: {message}", file=sys.stderr)
    print(_usage_text(), file=sys.stderr)
    return 2


def _usage_text() -> str:
    """Return the complete command-line usage without touching live inputs."""
    return (
        "usage: fg_bringup.py {--selftest [config]|config-lint [config]|"
        "replay-dry-run [config] [lan]|bootstrap [config]|configure [config]|"
        "login|verify|ap-inspect|ap-configure|ap-status|ap-exec [command ...]}"
    )


def usage_help() -> int:
    """Print help for the explicitly requested read-only help mode."""
    print(_usage_text())
    return 0


def _dispatch_selftest_cli(args: list[str]) -> int:
    """Validate and dispatch the offline selftest CLI."""
    if len(args) == 1:
        return run_selftest()
    if args == ["--selftest", "config"]:
        return run_selftest(config_only=True)
    return usage_error("--selftest accepts only the optional 'config' selector")


def _dispatch_config_lint_cli(args: list[str]) -> int:
    """Validate and dispatch the offline config-lint CLI."""
    maximum_args = ARGC_WITH_PATH - 1
    if len(args) > maximum_args:
        return usage_error("config-lint accepts at most one config path")
    path = Path(args[1]) if len(args) == maximum_args else DEFAULT_CONF
    return run_config_lint(path)


def _dispatch_replay_dry_run_cli(args: list[str]) -> int:
    """Validate and dispatch the offline replay-dry-run CLI."""
    maximum_args = ARGC_WITH_PATH_AND_LAN - 1
    path_argc = ARGC_WITH_PATH - 1
    if len(args) > maximum_args:
        return usage_error("replay-dry-run accepts a config path and LAN name")
    path = Path(args[1]) if len(args) >= path_argc else DEFAULT_CONF
    lan = args[2] if len(args) == maximum_args else "internal"
    return run_replay_dry_run(path, lan)


def _dispatch_offline(mode: str, args: list[str]) -> int | None:
    """Run an offline mode, or return None when live dispatch is required."""
    if mode == "--selftest":
        return _dispatch_selftest_cli(args)
    if mode == "config-lint":
        return _dispatch_config_lint_cli(args)
    if mode == "replay-dry-run":
        return _dispatch_replay_dry_run_cli(args)
    return None


def _live_arg_count_valid(mode: str, argument_count: int) -> bool:
    """Return whether a known live mode has a safe argument shape."""
    if mode in {"bootstrap", "configure"}:
        return ARGC_MODE_ONLY <= argument_count <= ARGC_WITH_PATH
    if mode == "ap-exec":
        return argument_count >= ARGC_MODE_ONLY
    return argument_count == ARGC_MODE_ONLY


def _live_runtime_safe(
    isolated: int,
    sanitized: str | None,
    prefix: Path,
    base_prefix: Path,
    expected_venv: Path,
) -> bool:
    """Return whether a live action has the reviewed Python startup boundary."""
    return (
        isolated == 1
        and sanitized == "v1"
        and prefix.absolute() == expected_venv.absolute()
        and base_prefix.absolute() != prefix.absolute()
    )


def _live_runtime_error() -> str | None:
    """Describe an unsafe live runtime, or return None for the Just boundary."""
    expected_venv = Path(__file__).resolve().parents[2] / ".venv"
    if _live_runtime_safe(
        sys.flags.isolated,
        os.environ.get("RA8_INFRA_SANITIZED"),
        Path(sys.prefix),
        Path(sys.base_prefix),
        expected_venv,
    ):
        return None
    return (
        "live modes require a sanitized FortiGate Just recipe "
        "using the repository .venv and Python isolated mode"
    )


def _live_argument_error(mode: str, argument_count: int) -> int | None:
    """Return a usage error for an invalid live-mode invocation."""
    live_modes = {
        "login",
        "bootstrap",
        "configure",
        "verify",
        "ap-inspect",
        "ap-configure",
        "ap-status",
        "ap-exec",
    }
    if mode not in live_modes:
        return usage_error(f"unknown mode {mode!r}")
    if _live_arg_count_valid(mode, argument_count):
        return None
    if mode in {"bootstrap", "configure"}:
        return usage_error(f"{mode} accepts at most one config path")
    return usage_error(f"{mode} accepts no arguments")


def _live_declaration(mode: str, args: list[str]) -> tuple[Path, list[str]]:
    """Load and validate the declaration before any live dependency is used."""
    declaration_path = DEFAULT_CONF
    if mode not in {"bootstrap", "configure"}:
        return declaration_path, []
    path_argc = ARGC_WITH_PATH - 1
    if len(args) == path_argc:
        declaration_path = Path(args[1])
    return declaration_path, read_valid_config(declaration_path)


def _live_preflight(mode: str, args: list[str]) -> tuple[Path, list[str]] | None:
    """Validate live argv, runtime, and declaration before any dependency."""
    if _live_argument_error(mode, len(args) + 1) is not None:
        return None
    runtime_error = _live_runtime_error()
    if runtime_error is not None:
        print(f"fg_bringup: refusing live mode: {runtime_error}", file=sys.stderr)
        return None
    try:
        return _live_declaration(mode, args)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"fg_bringup: refusing live replay: {exc}", file=sys.stderr)
        return None


def _redacted_creds() -> dict[str, str]:
    """Read live credentials and register every secret for log masking."""
    config = creds()
    secret_keys = (
        "fortigate_admin_pass",
        "fortigate_maintainer_pass",
        "ap_ssh_pass",
        "bench_psk",
        "legacy_psk_iot_network",
        "legacy_psk_home_network",
        "legacy_psk_guest_network",
    )
    for key in secret_keys:
        redact(config.get(key, ""))
    return config


def _dispatch_live_handler(
    mode: str,
    ser: serial.Serial,
    config: dict[str, str],
    declaration_lines: list[str],
    declaration_path: Path,
) -> int:
    """Dispatch after the serial console and credentials are ready."""
    if mode == "bootstrap":
        return do_bootstrap_mode(ser, config, declaration_lines, declaration_path)
    if mode == "configure":
        return do_configure_mode(ser, config, declaration_lines, declaration_path)
    handlers = {
        "login": do_login_mode,
        "verify": do_verify_mode,
        "ap-inspect": do_ap_inspect_mode,
        "ap-configure": do_ap_configure_mode,
        "ap-status": do_ap_status_mode,
        "ap-exec": do_ap_exec_mode,
    }
    return handlers[mode](ser, config)


def _run_live_mode(mode: str, path: Path, lines: list[str]) -> int:
    """Open the physical console and execute one already-validated live mode."""
    runtime_error = _live_runtime_error()
    if runtime_error is not None:
        print(f"fg_bringup: refusing live mode: {runtime_error}", file=sys.stderr)
        return 2
    try:
        serial = importlib.import_module("serial")
    except ModuleNotFoundError as exc:
        print(f"fg_bringup: live mode requires pyserial: {exc}", file=sys.stderr)
        return 2
    serial_path = Path(str(getattr(serial, "__file__", ""))).resolve()
    if not serial_path.is_relative_to(Path(sys.prefix).resolve()):
        print("fg_bringup: refusing pyserial outside the managed .venv", file=sys.stderr)
        return 2

    BENCH.mkdir(parents=True, exist_ok=True)
    tty = resolve_tty()
    log(f"=== fg_bringup {mode} open {tty} @{BAUD} ===")
    ser = serial.Serial(tty, BAUD, timeout=0.3, write_timeout=5, exclusive=True)
    try:
        return _dispatch_live_handler(mode, ser, _redacted_creds(), lines, path)
    finally:
        log("=== fg_bringup close ===")
        with contextlib.suppress(serial.SerialException):
            ser.close()


def main(
    argv: list[str] | None = None,
    *,
    live_runner: Callable[[str, Path, list[str]], int] | None = None,
) -> int:
    """Parse the mode and dispatch offline checks before any live dependency."""
    args = sys.argv[1:] if argv is None else argv
    if not args:
        return usage_error("an explicit mode is required")
    mode = args[0]
    if mode in {"-h", "--help"}:
        return usage_help()
    offline_result = _dispatch_offline(mode, args)
    if offline_result is not None:
        return offline_result
    prepared = _live_preflight(mode, args)
    if prepared is None:
        return 2
    declaration_path, declaration_lines = prepared
    runner = _run_live_mode if live_runner is None else live_runner
    return runner(mode, declaration_path, declaration_lines)


if __name__ == "__main__":
    sys.exit(main())
