#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Deterministic serial-console driver for the bench FortiGate 81E-POE.

This talks to the FortiGate over its DB9 console (9600 8N1) with a scripted
read/expect/write loop rather than an interactive ``screen`` session, so every
provisioning step is reproducible and fully logged. The transcript lands in
``~/ra8-bench/fortigate_console.log``.

Credentials are read from OpenBao (``secret/ra8d2/bench-network``); they are
never accepted on the command line and never written to the transcript -- the
logger masks any value it has been told to redact.

Run it from the bench Pi (``ssh star``), which owns the USB console cable at
``/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A9MJ2SSQ-if00-port0``.

Subcommands::

    fg_console.py probe                # report console state; no login
    fg_console.py show                 # login, dump the running config
    fg_console.py run FILE             # login, replay CLI lines from FILE
    fg_console.py factoryreset         # WIPE (device asks to confirm)
    fg_console.py bootstrap FILE       # post-wipe: default login, set the
                                       # admin password from OpenBao, replay FILE
"""

from __future__ import annotations

import argparse
import contextlib
import re
import sys
import time
from pathlib import Path

import serial

# openbao_client lives in the sibling scripts/secrets/ tree; make it importable
# the same way scripts/hil/hil_secrets.py does (path relative to this file).
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "secrets"))
from openbao_client import OpenBaoClient, load_config

TTY = "/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A9MJ2SSQ-if00-port0"
BAUD = 9600
LOG = Path.home() / "ra8-bench" / "fortigate_console.log"
BENCH_KV_PATH = "ra8d2/bench-network"

# A FortiOS prompt is always the final line: "name #", "name (sub) #", "name $".
PROMPT = re.compile(r"[\w\-.]+(?:\s+\([\w\-.]+\))?\s*[#$]\s*$")
LOGIN = re.compile(r"login:\s*$", re.IGNORECASE)
PASSWORD = re.compile(r"password:\s*$", re.IGNORECASE)
NEW_PASSWORD = re.compile(r"new password:\s*$", re.IGNORECASE)
CONFIRM_PASSWORD = re.compile(r"confirm password:\s*$", re.IGNORECASE)
CONFIRM_YN = re.compile(r"\(y/n\)", re.IGNORECASE)

PSK_MIN_BOOT_SECONDS = 300

# expect() result indices for the post-reset first-login sequence, where the
# candidate list is [NEW_PASSWORD, PROMPT, LOGIN].
FIRST_LOGIN_FORCED_CHANGE = 0
FIRST_LOGIN_REJECTED = 2


class ConsoleError(RuntimeError):
    """The console timed out or the device returned an unexpected response."""


class Console:
    """Expect-style wrapper around one exclusive pyserial session."""

    def __init__(self, tty: str = TTY, baud: int = BAUD) -> None:
        """Open the console and start a fresh transcript entry.

        Args:
            tty: Serial device path (a stable ``by-id`` symlink by default).
            baud: Console line rate; the FortiGate 81E uses 9600.
        """
        self._secrets: list[str] = []
        self.buf = ""
        LOG.parent.mkdir(parents=True, exist_ok=True)
        self.note(f"=== open {tty} @{baud} ===")
        self.ser = serial.Serial(tty, baud, timeout=0.4, write_timeout=5, exclusive=True)

    def redact(self, value: str) -> None:
        """Register a secret so it is masked everywhere in the transcript.

        Args:
            value: The literal string to replace with ``<REDACTED>``.
        """
        if value and value not in self._secrets:
            self._secrets.append(value)

    def _mask(self, text: str) -> str:
        """Return ``text`` with every registered secret replaced."""
        for secret in self._secrets:
            text = text.replace(secret, "<REDACTED>")
        return text

    def note(self, msg: str) -> None:
        """Write one masked, timestamped line to stdout and the transcript.

        Args:
            msg: Human-readable status line (secrets are masked first).
        """
        line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {self._mask(msg)}"
        print(line, flush=True)
        with LOG.open("a", encoding="utf-8", errors="replace") as handle:
            handle.write(line + "\n")

    def send(self, text: str, *, secret: bool = False) -> None:
        """Send one line plus CRLF to the console.

        Args:
            text: The characters to transmit (no trailing newline needed).
            secret: When true, log ``<REDACTED>`` instead of the text.
        """
        self.note(f">>> {'<REDACTED>' if secret else text}")
        self.ser.write((text + "\r\n").encode("ascii", "replace"))
        self.ser.flush()

    def expect(self, patterns: list[re.Pattern[str]], timeout: float = 20.0) -> int:
        """Read until one pattern matches the final line, or time out.

        Args:
            patterns: Ordered candidates; the index of the first match wins.
            timeout: Seconds to wait before raising.

        Returns:
            The index into ``patterns`` of the matched pattern.

        Raises:
            ConsoleError: No pattern matched before ``timeout`` elapsed.
        """
        deadline = time.monotonic() + timeout
        self.buf = ""
        while time.monotonic() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk.decode("ascii", "replace")
                if "--More--" in self.buf:
                    self.ser.write(b" ")
                    self.ser.flush()
                    self.buf = self.buf.replace("--More--", "")
            lines = [ln.strip() for ln in self.buf.splitlines() if ln.strip()]
            if lines:
                for idx, pat in enumerate(patterns):
                    if pat.search(lines[-1]):
                        self.note(f"<<< {self.buf.strip()}")
                        return idx
        self.note(f"<<< (TIMEOUT after {timeout}s) {self.buf.strip()}")
        msg = f"timeout waiting for {[p.pattern for p in patterns]}"
        raise ConsoleError(msg)

    def cmd(self, line: str, timeout: float = 25.0) -> None:
        """Send one CLI line and wait for the next prompt.

        Args:
            line: The FortiOS CLI statement to run.
            timeout: Seconds to wait for the follow-up prompt.
        """
        self.send(line)
        self.expect([PROMPT], timeout)

    def wake(self) -> str:
        """Nudge the console and classify the state it lands in.

        Returns:
            One of ``"prompt"``, ``"login"``, or ``"password"``.
        """
        self.ser.reset_input_buffer()
        self.send("")
        idx = self.expect([PROMPT, LOGIN, PASSWORD], timeout=15)
        return ("prompt", "login", "password")[idx]

    def login(self, user: str, password: str) -> None:
        """Authenticate, tolerating an already-open session.

        Args:
            user: Administrator account name.
            password: Administrator password (registered for redaction).

        Raises:
            ConsoleError: The device returned to the login prompt.
        """
        self.redact(password)
        state = self.wake()
        if state == "prompt":
            self.note("already at a prompt; no login needed")
            return
        if state == "password":
            self.send("")
            state = self.wake()
        if state == "login":
            self.send(user)
            self.expect([PASSWORD], timeout=15)
            self.send(password, secret=True)
        if self.expect([PROMPT, LOGIN, PASSWORD], timeout=25) != 0:
            msg = "login rejected (device returned to the login prompt)"
            raise ConsoleError(msg)
        self.note("login OK")

    def unpage(self) -> None:
        """Disable console paging so long output is not chunked by --More--."""
        self.cmd("config system console")
        self.cmd("set output standard")
        self.cmd("end")

    def replay(self, script: Path) -> None:
        """Send every non-comment, non-blank line of a CLI script.

        Args:
            script: Path to a file of FortiOS CLI lines; ``#`` lines and blank
                lines are skipped so the file may carry a licence header.
        """
        for raw in script.read_text(encoding="ascii").splitlines():
            line = raw.rstrip()
            if line and not line.lstrip().startswith("#"):
                self.cmd(line, timeout=120)

    def close(self) -> None:
        """Close the serial session and mark the transcript."""
        self.note("=== close ===")
        with contextlib.suppress(serial.SerialException):
            self.ser.close()


def load_creds() -> dict[str, str]:
    """Read the bench-network secret from OpenBao via the read-only AppRole.

    Returns:
        The KV v2 secret data as a flat string-to-string mapping.
    """
    return OpenBaoClient(load_config()).kv_get(BENCH_KV_PATH)


def set_admin_password(con: Console, user: str, password: str) -> None:
    """Force the admin password to ``password`` regardless of prior state.

    Args:
        con: An authenticated console.
        user: Administrator account name.
        password: Desired password (already registered for redaction).
    """
    con.cmd("config system admin")
    con.cmd(f"edit {user}")
    con.send(f"set password {password}", secret=True)
    con.expect([PROMPT], timeout=20)
    con.cmd("next")
    con.cmd("end")
    con.note("admin password set from OpenBao")


def do_bootstrap(con: Console, creds: dict[str, str], script: Path) -> int:
    """Bring a freshly factory-reset FortiGate up to the bench config.

    Logs in with the factory default (admin / empty password), satisfies the
    forced first-login password change, then replays ``script``.

    Args:
        con: An open (not yet authenticated) console.
        creds: The bench-network secret from OpenBao.
        script: The CLI replay file (see ``fortigate-bench.conf``).

    Returns:
        Process exit status (0 on success).

    Raises:
        ConsoleError: The device never reached a login prompt, or the default
            login was rejected.
    """
    user, password = creds["fortigate_admin_user"], creds["fortigate_admin_pass"]
    con.redact(password)
    con.note("waiting for the device to finish booting")
    deadline = time.monotonic() + PSK_MIN_BOOT_SECONDS
    state = "unknown"
    while time.monotonic() < deadline:
        try:
            state = con.wake()
        except ConsoleError:
            continue
        if state in ("login", "prompt"):
            break
    else:
        msg = "device never reached a login prompt after reset"
        raise ConsoleError(msg)

    if state == "login":
        con.send(user)
        con.expect([PASSWORD], timeout=15)
        con.send("", secret=True)
        idx = con.expect([NEW_PASSWORD, PROMPT, LOGIN], timeout=30)
        if idx == FIRST_LOGIN_FORCED_CHANGE:
            con.send(password, secret=True)
            con.expect([CONFIRM_PASSWORD], timeout=15)
            con.send(password, secret=True)
            con.expect([PROMPT], timeout=30)
        elif idx == FIRST_LOGIN_REJECTED:
            msg = "default admin/<blank> login rejected after factory reset"
            raise ConsoleError(msg)

    con.unpage()
    set_admin_password(con, user, password)
    con.replay(script)
    return 0


def do_factoryreset(con: Console, creds: dict[str, str]) -> int:
    """Log in and issue ``execute factoryreset`` (the device reboots wiped).

    Args:
        con: An open console.
        creds: The bench-network secret from OpenBao.

    Returns:
        Process exit status (0 on success).
    """
    con.login(creds["fortigate_admin_user"], creds["fortigate_admin_pass"])
    con.send("execute factoryreset")
    con.expect([CONFIRM_YN], timeout=20)
    con.send("y")
    time.sleep(2)
    con.note("factoryreset issued; device is rebooting")
    return 0


def do_show(con: Console, creds: dict[str, str]) -> int:
    """Log in and dump status plus the full running configuration.

    Args:
        con: An open console.
        creds: The bench-network secret from OpenBao.

    Returns:
        Process exit status (0 on success).
    """
    con.login(creds["fortigate_admin_user"], creds["fortigate_admin_pass"])
    con.unpage()
    con.cmd("get system status", timeout=40)
    con.cmd("show full-configuration | grep .", timeout=180)
    return 0


def do_run(con: Console, creds: dict[str, str], script: Path) -> int:
    """Log in and replay a CLI script (idempotent bench config).

    Args:
        con: An open console.
        creds: The bench-network secret from OpenBao.
        script: The CLI replay file.

    Returns:
        Process exit status (0 on success).
    """
    con.login(creds["fortigate_admin_user"], creds["fortigate_admin_pass"])
    con.unpage()
    con.replay(script)
    return 0


def main() -> int:
    """Parse arguments and dispatch to the requested subcommand.

    Returns:
        Process exit status.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["probe", "show", "run", "factoryreset", "bootstrap"])
    parser.add_argument("file", nargs="?")
    args = parser.parse_args()

    con = Console()
    try:
        if args.mode == "probe":
            con.note(f"state: {con.wake()}")
            return 0
        creds = load_creds()
        con.redact(creds["fortigate_admin_pass"])
        if args.mode == "bootstrap":
            return do_bootstrap(con, creds, Path(args.file))
        if args.mode == "factoryreset":
            return do_factoryreset(con, creds)
        if args.mode == "show":
            return do_show(con, creds)
        return do_run(con, creds, Path(args.file))
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
