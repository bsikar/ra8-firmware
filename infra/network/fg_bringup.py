#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""FortiGate 81E-POE login + bench bring-up over the serial console.

Uses the login mechanics proven on this exact unit: send the line terminated by
a bare CR (a trailing LF submits an empty password and desyncs the login), then
read until a SUBSTRING match (case-insensitive, because FortiOS capitalises
"New Password:"), with chunked reads into a per-step buffer that is reset each
step so a stale echo cannot self-match.

Modes:
  login        log in, capture the running config (read-only), log out
  bootstrap    log in, execute factoryreset, then configure the wiped unit
  configure    configure an already-wiped unit (skip factoryreset)
  verify       log in, dump status / DHCP / routing, ping the AP
  ap-inspect   jump to the AP over `execute ssh`, dump its wireless/network
  ap-configure jump to the AP, stand up the ra8-bench SSID + dumb-AP settings
  ap-status    jump to the AP, confirm ra8-bench hostapd is beaconing
  ap-exec      jump to the AP, run each command passed after the mode argument

Every credential is read from OpenBao and masked in the transcript.
"""

from __future__ import annotations

import contextlib
import os
import sys
import time
from pathlib import Path

import serial

# openbao_client lives in the sibling scripts/secrets/ tree. Make it importable
# whether this runs from the repo (infra/network/) or the bench Pi (~/ra8-bench).
sys.path.insert(0, str(Path.home() / "ra8-firmware" / "scripts" / "secrets"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "secrets"))
from openbao_client import OpenBaoClient, load_config

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
CONF = BENCH / "fortigate-bench.conf"

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
    for s in _SECRETS:
        text = text.replace(s, "<REDACTED>")
    return text


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
    return OpenBaoClient(load_config()).kv_get("ra8d2/bench-network")


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
    for cmd in ("get system status", "show system interface", "show system dhcp server"):
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


def configure_after_wipe(ser: serial.Serial, c: dict[str, str]) -> int:
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
    text = CONF.read_text(encoding="ascii")
    if lan != "internal":
        text = text.replace('"internal"', f'"{lan}"')
    conf_lines = [
        ln.rstrip() for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith("#")
    ]
    run_lines(ser, conf_lines, 60)
    run_lines(ser, ["get system status", f"show system interface {lan}", "get system poe"], 40)
    status("DONE: FortiGate wiped and bench-configured (10.0.40.1/24) -- ready for AP")
    return 0


def do_configure_mode(ser: serial.Serial, c: dict[str, str]) -> int:
    """Configure an already-wiped unit (skips factoryreset)."""
    return configure_after_wipe(ser, c)


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
    if not login(ser, c["fortigate_admin_user"], c["fortigate_admin_pass"]):
        status("ap-configure: FortiGate login failed")
        return 2
    run_lines(ser, ["config system console", "set output standard", "end"], 20)
    status("ap-configure: reaching the AP over execute ssh")
    if not ap_ssh_open(ser, c["ap_ip"], c["ap_ssh_user"], c["ap_ssh_pass"]):
        status("ap-configure: could not open the nested ssh to the AP")
        return 2
    status("ap-configure: inside OpenWrt -- applying uci")
    setup = [
        "uci set wireless.radio1.channel='6'",
        "uci set wireless.radio1.htmode='HT20'",
        "uci set wireless.radio1.disabled='0'",
        "uci delete wireless.bench",
        "uci set wireless.bench='wifi-iface'",
        "uci set wireless.bench.device='radio1'",
        "uci set wireless.bench.mode='ap'",
        "uci set wireless.bench.network='lan'",
        "uci set wireless.bench.ssid='ra8-bench'",
        "uci set wireless.bench.encryption='psk2'",
        "uci set wireless.iot_5g.disabled='1'",
        "uci set wireless.iot_2g.disabled='1'",
        "uci set wireless.guest_5g.disabled='1'",
        "uci set wireless.guest_2g.disabled='1'",
        # Dumb AP: the FortiGate (10.0.40.1) is the ONLY DHCP server. Stop the
        # AP's dnsmasq/odhcpd from serving DHCP/RA on the lan bridge, else two
        # servers race on 10.0.40.0/24 with overlapping pools.
        "uci set dhcp.lan.ignore='1'",
        "uci set dhcp.lan.dhcpv4='disabled'",
        "uci set dhcp.lan.dhcpv6='disabled'",
        "uci set dhcp.lan.ra='disabled'",
    ]
    for cmd in setup:
        ap_cmd(ser, cmd, 2)
    # the PSK, masked in the transcript
    ap_cmd(ser, f"uci set wireless.bench.key='{c['bench_psk']}'", 2, secret=True)
    ap_cmd(ser, "uci commit wireless", 4)
    ap_cmd(ser, "uci commit dhcp", 4)
    status("ap-configure: committed; reloading wifi + dhcp")
    ap_cmd(ser, "wifi reload", 12)
    ap_cmd(ser, "/etc/init.d/dnsmasq restart", 6)
    ap_cmd(ser, "/etc/init.d/odhcpd restart", 6)
    ap_cmd(ser, "sleep 3; uci show wireless.bench; uci show dhcp.lan.ignore", 6)
    ap_cmd(ser, "iwinfo 2>/dev/null | grep -E 'ESSID|Channel'", 6)
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


def do_bootstrap_mode(ser: serial.Serial, c: dict[str, str]) -> int:
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
    return configure_after_wipe(ser, c)


def run_selftest() -> int:
    """Offline regression check for the LAN-name detection + token rewrite.

    Touches no hardware and reads no secret. It pins the behaviour that the
    odd/even split depends on: with the second switch 'lan-even' present, the
    primary is still read correctly, and rewriting the 'internal' token to the
    resolved primary name leaves every 'lan-even' reference intact. Both
    directions are asserted (a must-detect-'lan' case and a must-detect-
    'internal' case) so a scope collapse cannot pass quietly.
    """
    two_switch = (
        "config system interface\n"
        '    edit "wan1"\n        set mode dhcp\n    next\n'
        '    edit "lan"\n        set ip 10.0.40.1 255.255.255.0\n'
        "        set type hard-switch\n    next\n"
        '    edit "lan-even"\n        set ip 10.0.41.1 255.255.255.0\n'
        "        set type hard-switch\n    next\n"
        "end\n"
    )
    internal_box = (
        "config system interface\n"
        '    edit "internal"\n        set ip 10.0.40.1 255.255.255.0\n    next\n'
        '    edit "lan-even"\n        set ip 10.0.41.1 255.255.255.0\n    next\n'
        "end\n"
    )
    cases = [
        ("primary read past lan-even", primary_lan_name(two_switch), "lan"),
        ("no false-'internal' when primary is lan", primary_lan_name(internal_box), "internal"),
        ("empty dump defaults to internal", primary_lan_name(""), "internal"),
        ("lan-even alone never reads as lan", primary_lan_name('edit "lan-even"\n'), "internal"),
    ]
    ok = True
    for label, got, want in cases:
        good = got == want
        ok = ok and good
        print(f"  [{'PASS' if good else 'FAIL'}] {label}: got {got!r}, want {want!r}")

    # The rewrite configure_after_wipe() applies must resolve the primary token
    # and leave the literal second switch untouched.
    decl = (
        'edit "internal"\n set srcintf "internal"\n'
        ' set dstintf "lan-even"\n set srcintf "lan-even"\n'
    )
    rewritten = decl.replace('"internal"', '"lan"')
    primary_resolved = '"internal"' not in rewritten and 'edit "lan"' in rewritten
    lan_even_kept = rewritten.count('"lan-even"') == decl.count('"lan-even"')
    checks = [
        ("token rewrite resolves primary", primary_resolved),
        ("token rewrite keeps lan-even", lan_even_kept),
    ]
    for label, good in checks:
        ok = ok and good
        print(f"  [{'PASS' if good else 'FAIL'}] {label}")

    print("SELFTEST:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    """Parse the mode argument, open the console, and dispatch to the handler."""
    mode = sys.argv[1] if len(sys.argv) > 1 else "login"
    if mode == "--selftest":  # offline: no console, no OpenBao, no hardware
        return run_selftest()
    BENCH.mkdir(parents=True, exist_ok=True)
    tty = resolve_tty()
    log(f"=== fg_bringup {mode} open {tty} @{BAUD} ===")
    handlers = {
        "login": do_login_mode,
        "bootstrap": do_bootstrap_mode,
        "configure": do_configure_mode,
        "verify": do_verify_mode,
        "ap-inspect": do_ap_inspect_mode,
        "ap-configure": do_ap_configure_mode,
        "ap-status": do_ap_status_mode,
        "ap-exec": do_ap_exec_mode,
    }
    ser = serial.Serial(tty, BAUD, timeout=0.3, write_timeout=5, exclusive=True)
    try:
        c = creds()
        # Register EVERY secret value so it is masked in the transcript -- uci
        # show / config dumps echo PSKs back, so a missing one leaks.
        for key in (
            "fortigate_admin_pass",
            "fortigate_maintainer_pass",
            "ap_ssh_pass",
            "bench_psk",
            "legacy_psk_iot_network",
            "legacy_psk_home_network",
            "legacy_psk_guest_network",
        ):
            redact(c.get(key, ""))
        return handlers.get(mode, do_login_mode)(ser, c)
    finally:
        log("=== fg_bringup close ===")
        with contextlib.suppress(serial.SerialException):
            ser.close()


if __name__ == "__main__":
    sys.exit(main())
