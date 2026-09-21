#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Emit one runtime Wi-Fi provisioning packet to a non-terminal stdout.

The packet is consumed by the bench-only firmware provisioner after flashing;
credentials never enter CMake, compiler arguments, build metadata, or firmware
artifacts. Values come from explicitly exported variables, the gitignored
0600 ``coprocessor/esp32c6/wifi.env`` file, or the existing OpenBao service.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import signal
import stat
import string
import sys
import tempfile
from collections.abc import Callable, Iterator
from pathlib import Path
from unittest import mock

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
DEFAULT_ENV = REPO_ROOT / "coprocessor" / "esp32c6" / "wifi.env"
WIFI_KEYS = frozenset(("RA8_C6_WIFI_SSID", "RA8_C6_WIFI_PSK"))
URL_KEY = "RA8_MEDIA_DOWNLOAD_URL"
MIN_PSK_BYTES = 8
MAX_PSK_BYTES = 63
HEX_PSK_CHARS = 64
MAX_SSID_BYTES = 32
MAX_URL_BYTES = 511
MIN_QUOTED_VALUE_LENGTH = 2
ASCII_CONTROL_MAX = 0x20
ASCII_DELETE = 0x7F
ERROR_EXIT = 2
DEFAULT_EMIT_TIMEOUT_S = 30
MAX_EMIT_TIMEOUT_S = 600

sys.path.insert(0, str(SCRIPT_DIR))

from openbao_client import (  # noqa: E402 -- sibling path added above
    OpenBaoClient,
    OpenBaoError,
    creds_path,
)


class ProvisionError(ValueError):
    """A credential source or effective runtime value is invalid."""


def _timeout_seconds(raw: str) -> int:
    """Parse one bounded whole-second total emit deadline for argparse."""
    try:
        timeout_s = int(raw, 10)
    except ValueError as exc:
        message = "timeout must be a whole number of seconds"
        raise argparse.ArgumentTypeError(message) from exc
    if not 1 <= timeout_s <= MAX_EMIT_TIMEOUT_S:
        message = f"timeout must be in 1..{MAX_EMIT_TIMEOUT_S} seconds"
        raise argparse.ArgumentTypeError(message)
    return timeout_s


@contextlib.contextmanager
def _emit_deadline(timeout_s: int) -> Iterator[None]:
    """Bound the complete credential resolution, encoding, and stdout write."""

    def timed_out(_signum: int, _frame: object) -> None:
        message = f"credential emission exceeded its {timeout_s}-second deadline"
        raise ProvisionError(message)

    previous_handler = signal.signal(signal.SIGALRM, timed_out)
    previous_timer = signal.setitimer(signal.ITIMER_REAL, float(timeout_s))
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0.0)
        signal.signal(signal.SIGALRM, previous_handler)
        if previous_timer[0] > 0.0:
            signal.setitimer(signal.ITIMER_REAL, previous_timer[0], previous_timer[1])


def _read_private_file(path: Path) -> str:
    """Open a regular, non-symlink credential file once and return its text."""
    try:
        before = path.lstat()
    except FileNotFoundError:
        raise
    except OSError as exc:
        message = f"cannot inspect {path}: {exc.strerror}"
        raise ProvisionError(message) from exc
    if stat.S_ISLNK(before.st_mode):
        message = f"credential file must not be a symlink: {path}"
        raise ProvisionError(message)

    try:
        flags = os.O_RDONLY
        flags |= getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(path, flags)
    except FileNotFoundError:
        raise
    except OSError as exc:
        message = f"cannot open {path}: {exc.strerror}"
        raise ProvisionError(message) from exc

    try:
        opened = os.fstat(descriptor)
        if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            message = f"credential file changed while opening: {path}"
            raise ProvisionError(message)
        if not stat.S_ISREG(opened.st_mode):
            message = f"credential file must be a regular file: {path}"
            raise ProvisionError(message)
        mode = stat.S_IMODE(opened.st_mode)
        if mode & 0o077:
            message = f"credential file must grant no group/other permissions: {path}"
            raise ProvisionError(message)
        with os.fdopen(descriptor, encoding="utf-8") as stream:
            descriptor = -1
            return stream.read()
    except UnicodeDecodeError as exc:
        message = f"credential file is not valid UTF-8: {path}"
        raise ProvisionError(message) from exc
    except OSError as exc:
        message = f"cannot read {path}: {exc.strerror}"
        raise ProvisionError(message) from exc
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def _openbao_config(path: Path) -> dict[str, str]:
    """Read the operator's OpenBao configuration through the private-file gate."""
    config: dict[str, str] = {}
    for raw_line in _read_private_file(path).splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        config[key.strip()] = value.strip()
    return config


def _literal_value(raw: str, path: Path, line_no: int) -> str:
    """Parse one dotenv value as data without evaluating shell syntax."""
    value = raw.strip()
    if not value:
        return ""
    if value[0] in ("'", '"'):
        quote = value[0]
        if len(value) < MIN_QUOTED_VALUE_LENGTH or value[-1] != quote:
            message = f"{path}:{line_no}: unmatched credential quote"
            raise ProvisionError(message)
        value = value[1:-1]
        if quote in value:
            message = f"{path}:{line_no}: embedded credential quote is unsupported"
            raise ProvisionError(message)
    elif any(char.isspace() for char in value):
        message = f"{path}:{line_no}: quote values containing whitespace"
        raise ProvisionError(message)
    return value


def parse_env_file(path: Path) -> dict[str, str]:
    """Parse the optional private Wi-Fi dotenv file without shell evaluation."""
    try:
        lines = _read_private_file(path).splitlines()
    except FileNotFoundError:
        return {}

    parsed: dict[str, str] = {}
    for line_no, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, raw_value = line.partition("=")
        if not separator or key.strip() != key or key not in WIFI_KEYS:
            message = f"{path}:{line_no}: expected a supported KEY=value assignment"
            raise ProvisionError(message)
        if key in parsed:
            message = f"{path}:{line_no}: duplicate assignment for {key}"
            raise ProvisionError(message)
        parsed[key] = _literal_value(raw_value, path, line_no)
    return parsed


def validate_values(ssid: str, psk: str, url: str) -> None:
    """Validate protocol and storage bounds before emitting any bytes."""
    ssid_bytes = ssid.encode("utf-8")
    psk_bytes = psk.encode("utf-8")
    url_bytes = url.encode("utf-8")
    if not ssid or len(ssid_bytes) > MAX_SSID_BYTES:
        message = "Wi-Fi SSID must contain 1-32 UTF-8 bytes"
        raise ProvisionError(message)
    psk_is_passphrase = MIN_PSK_BYTES <= len(psk_bytes) <= MAX_PSK_BYTES
    psk_is_hex = len(psk) == HEX_PSK_CHARS and all(char in string.hexdigits for char in psk)
    if not (psk_is_passphrase or psk_is_hex):
        message = "Wi-Fi PSK must contain 8-63 UTF-8 bytes or 64 hexadecimal digits"
        raise ProvisionError(message)
    if len(url_bytes) > MAX_URL_BYTES:
        message = "media URL exceeds the 511-byte runtime protocol bound"
        raise ProvisionError(message)
    if any(ord(char) < ASCII_CONTROL_MAX or ord(char) == ASCII_DELETE for char in ssid + psk + url):
        message = "runtime provisioning values may not contain control characters"
        raise ProvisionError(message)


def resolve_values(
    env_path: Path = DEFAULT_ENV,
    environment: dict[str, str] | None = None,
    vault_data: dict[str, str] | None = None,
) -> tuple[str, str, str]:
    """Resolve explicit/file values first, then the bench network in OpenBao."""
    effective_env = os.environ if environment is None else environment
    local = parse_env_file(env_path)
    ssid = effective_env.get("RA8_C6_WIFI_SSID", local.get("RA8_C6_WIFI_SSID", ""))
    psk = effective_env.get("RA8_C6_WIFI_PSK", local.get("RA8_C6_WIFI_PSK", ""))
    url = effective_env.get(URL_KEY, "")
    if not psk:
        if vault_data is None:
            openbao_env = creds_path()
            try:
                client = OpenBaoClient(_openbao_config(openbao_env))
            except FileNotFoundError:
                client = OpenBaoClient()
            if not client.configured:
                message = "Wi-Fi credentials are absent and OpenBao is not configured"
                raise ProvisionError(message)
            try:
                vault_data = client.kv_get("ra8d2/bench-network")
            except OpenBaoError as exc:
                message = "could not fetch the bench network from OpenBao"
                raise ProvisionError(message) from exc
        psk = vault_data.get("bench_psk", "")
        ssid = ssid or vault_data.get("bench_ssid", "") or "ra8-bench"
    validate_values(ssid, psk, url)
    return ssid, psk, url


def make_packet(ssid: str, psk: str, url: str = "") -> bytes:
    """Encode one newline-terminated RA8NET1 packet without raw delimiters."""
    validate_values(ssid, psk, url)
    fields = (ssid.encode("utf-8").hex(), psk.encode("utf-8").hex(), url.encode("utf-8").hex())
    return f"RA8NET1:{fields[0]}:{fields[1]}:{fields[2]}\n".encode("ascii")


def _values_are_valid(values: tuple[str, str, str]) -> bool:
    """Return whether runtime values can be encoded."""
    try:
        make_packet(*values)
    except ProvisionError:
        return False
    return True


class _SelftestState:
    """Accumulate selftest checks and failures without nested closures."""

    def __init__(self) -> None:
        self.failures: list[str] = []
        self.checks = 0

    def expect(self, condition: bool, message: str) -> None:
        """Record one Boolean selftest expectation."""
        self.checks += 1
        if not condition:
            self.failures.append(message)

    def expect_rejected(self, operation: Callable[[], object], message: str) -> None:
        """Record that one operation raises the provisioning error."""
        self.checks += 1
        try:
            operation()
        except ProvisionError:
            return
        self.failures.append(message)


class _TerminalCapture(io.StringIO):
    """Text stream that reports an interactive terminal."""

    @staticmethod
    def isatty() -> bool:
        """Report that writes would expose bytes to a terminal."""
        return True


class _BinaryCapture(io.StringIO):
    """Redirected text stream with the binary buffer used by main."""

    def __init__(self) -> None:
        super().__init__()
        self.buffer = io.BytesIO()

    @staticmethod
    def isatty() -> bool:
        """Report that output is safely redirected."""
        return False


def _selftest_write_env(root: Path, name: str, text: str, mode: int = 0o600) -> Path:
    """Write one private temporary dotenv fixture."""
    path = root / name
    path.write_text(text, encoding="utf-8")
    path.chmod(mode)
    return path


def _selftest_source_precedence(state: _SelftestState, valid: Path) -> None:
    """Exercise environment, file, and vault source precedence."""
    got = resolve_values(valid, {}, {})
    state.expect(got == ("bench wifi", "correct-horse", ""), "private dotenv did not resolve")
    precedence = resolve_values(
        valid,
        {
            "RA8_C6_WIFI_SSID": "environment-network",
            "RA8_C6_WIFI_PSK": "environment-password",
            URL_KEY: "https://media.example.invalid/library",
        },
        {"bench_ssid": "vault-network", "bench_psk": "vault-password"},
    )
    state.expect(
        precedence
        == (
            "environment-network",
            "environment-password",
            "https://media.example.invalid/library",
        ),
        "environment did not take precedence over file and vault",
    )
    mixed_ssid = resolve_values(
        valid, {"RA8_C6_WIFI_SSID": "environment-network"}, {"bench_psk": "vault-password"}
    )
    state.expect(
        mixed_ssid == ("environment-network", "correct-horse", ""),
        "environment SSID did not combine with file PSK",
    )
    mixed_psk = resolve_values(
        valid,
        {"RA8_C6_WIFI_PSK": "environment-password"},
        {"bench_ssid": "vault-network", "bench_psk": "vault-password"},
    )
    state.expect(
        mixed_psk == ("bench wifi", "environment-password", ""),
        "environment PSK did not combine with file SSID",
    )


def _selftest_vault_fallback(state: _SelftestState, missing: Path) -> None:
    """Exercise vault defaults and preservation of an explicit SSID."""
    fallback = resolve_values(missing, {}, {"bench_psk": "vault-password"})
    state.expect(
        fallback == ("ra8-bench", "vault-password", ""),
        "OpenBao fallback did not supply the default SSID",
    )
    preserved_ssid = resolve_values(
        missing,
        {"RA8_C6_WIFI_SSID": "operator-network"},
        {"bench_ssid": "vault-network", "bench_psk": "vault-password"},
    )
    state.expect(
        preserved_ssid == ("operator-network", "vault-password", ""),
        "OpenBao fallback replaced an explicit SSID",
    )


def _selftest_private_files(state: _SelftestState, root: Path, valid: Path) -> None:
    """Exercise private-file modes, symlinks, and dotenv syntax."""
    contents = "RA8_C6_WIFI_SSID=bench\nRA8_C6_WIFI_PSK=correct-horse\n"
    owner_read_only = _selftest_write_env(root, "owner-read-only.env", contents, 0o400)
    state.expect(
        parse_env_file(owner_read_only)["RA8_C6_WIFI_PSK"] == "correct-horse",
        "owner-only read permission was rejected",
    )
    public = _selftest_write_env(root, "public.env", contents, 0o640)
    state.expect_rejected(
        lambda: parse_env_file(public), "group-readable credential file was accepted"
    )
    symlink = root / "symlink.env"
    symlink.symlink_to(valid)
    state.expect_rejected(lambda: parse_env_file(symlink), "credential symlink was accepted")
    invalid_env_cases = (
        ("duplicate.env", "RA8_C6_WIFI_SSID=one\nRA8_C6_WIFI_SSID=two\n", "duplicate"),
        ("unknown.env", "UNSUPPORTED_KEY=value\n", "unknown key"),
        ("unmatched.env", 'RA8_C6_WIFI_SSID="bench\n', "unmatched quote"),
        ("embedded.env", 'RA8_C6_WIFI_SSID="ben"ch"\n', "embedded quote"),
    )
    for file_name, text, label in invalid_env_cases:
        path = _selftest_write_env(root, file_name, text)
        state.expect_rejected(
            lambda path=path: parse_env_file(path), f"dotenv {label} was accepted"
        )


def _selftest_values(state: _SelftestState) -> bytes:
    """Exercise protocol value bounds and exact packet framing."""
    valid_values = (
        ("bench", "a" * HEX_PSK_CHARS, ""),
        ("\u00e9" * 16, "\u00e9" * 4, ""),
        ("bench", "correct-horse", "https://media.example.invalid/"),
    )
    for values in valid_values:
        state.expect(_values_are_valid(values), f"valid runtime values were rejected: {values!r}")
    invalid_values = (
        ("", "correct-horse", ""),
        ("bench", "short", ""),
        ("bench", "g" * HEX_PSK_CHARS, ""),
        ("\u00e9" * 17, "correct-horse", ""),
        ("bench", "correct\nhorse", ""),
        ("bench", "correct-horse", "https://media.invalid/\x7f"),
        ("bench", "correct-horse", "x" * (MAX_URL_BYTES + 1)),
    )
    for values in invalid_values:
        state.expect(
            not _values_are_valid(values), f"invalid runtime values were accepted: {values!r}"
        )
    packet = make_packet("bench", "correct-horse")
    state.expect(
        packet == b"RA8NET1:62656e6368:636f72726563742d686f727365:\n",
        "packet framing changed",
    )
    return packet


def _selftest_openbao_errors(state: _SelftestState, missing: Path) -> None:
    """Exercise unavailable and failing OpenBao clients."""
    unconfigured = mock.Mock(configured=False)
    failing = mock.Mock(configured=True)
    failing.kv_get.side_effect = OpenBaoError("selftest transport failure")
    with mock.patch.object(sys.modules[__name__], "creds_path", return_value=missing):
        with mock.patch.object(sys.modules[__name__], "OpenBaoClient", return_value=unconfigured):
            state.expect_rejected(
                lambda: resolve_values(missing, {}, None), "unconfigured OpenBao was accepted"
            )
        with mock.patch.object(sys.modules[__name__], "OpenBaoClient", return_value=failing):
            state.expect_rejected(
                lambda: resolve_values(missing, {}, None),
                "OpenBao transport failure was accepted",
            )


def _selftest_emission(state: _SelftestState, packet: bytes) -> None:
    """Exercise terminal refusal and redirected binary emission."""
    terminal = _TerminalCapture()
    with contextlib.redirect_stdout(terminal), contextlib.redirect_stderr(io.StringIO()):
        terminal_rc = main(["emit"])
    state.expect(
        terminal_rc == ERROR_EXIT and terminal.getvalue() == "",
        "terminal emission was not refused",
    )
    redirected = _BinaryCapture()
    with (
        mock.patch.object(
            sys.modules[__name__],
            "resolve_values",
            return_value=("bench", "correct-horse", ""),
        ),
        contextlib.redirect_stdout(redirected),
        contextlib.redirect_stderr(io.StringIO()),
    ):
        emit_rc = main(["emit"])
    state.expect(
        emit_rc == 0 and redirected.buffer.getvalue() == packet,
        "redirected end-to-end emission failed",
    )


def _selftest_force_timeout(_path: Path) -> tuple[str, str, str]:
    """Raise the process alarm while credential resolution is active."""
    signal.raise_signal(signal.SIGALRM)
    return ("must-not", "reach-this-secret", "")


def _selftest_timeout(state: _SelftestState) -> None:
    """Exercise total emission deadlines and timeout argument bounds."""
    timeout_output = _BinaryCapture()
    with (
        mock.patch.object(
            sys.modules[__name__], "resolve_values", side_effect=_selftest_force_timeout
        ),
        contextlib.redirect_stdout(timeout_output),
        contextlib.redirect_stderr(io.StringIO()) as timeout_error,
    ):
        timeout_rc = main(["--timeout", "1", "emit"])
    state.expect(
        timeout_rc == ERROR_EXIT
        and timeout_output.buffer.getvalue() == b""
        and "deadline" in timeout_error.getvalue()
        and "secret" not in timeout_error.getvalue(),
        "total emit deadline failed or exposed credential material",
    )
    state.expect(
        _timeout_seconds("1") == 1
        and _timeout_seconds(str(MAX_EMIT_TIMEOUT_S)) == MAX_EMIT_TIMEOUT_S,
        "valid timeout boundaries were rejected",
    )
    for invalid_timeout in ("0", "601", "1.5", "invalid"):
        state.checks += 1
        try:
            _timeout_seconds(invalid_timeout)
        except argparse.ArgumentTypeError:
            continue
        state.failures.append(f"invalid timeout was accepted: {invalid_timeout}")


def run_selftest() -> int:
    """Exercise source precedence, secure file I/O, validation, and emission."""
    state = _SelftestState()
    with tempfile.TemporaryDirectory() as name:
        root = Path(name)
        valid = _selftest_write_env(
            root,
            "wifi.env",
            'RA8_C6_WIFI_SSID="bench wifi"\nRA8_C6_WIFI_PSK=correct-horse\n',
        )
        missing = root / "missing.env"
        _selftest_source_precedence(state, valid)
        _selftest_vault_fallback(state, missing)
        _selftest_private_files(state, root, valid)
        packet = _selftest_values(state)
        _selftest_openbao_errors(state, missing)
        _selftest_emission(state, packet)
        _selftest_timeout(state)
    if state.failures:
        for failure in state.failures:
            print(f"wifi_provision.py --selftest: FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"wifi_provision.py --selftest: PASS ({state.checks} checks)")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Parse the mode and emit only to a pipe or redirected file descriptor."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=DEFAULT_ENV)
    parser.add_argument("--timeout", type=_timeout_seconds, default=DEFAULT_EMIT_TIMEOUT_S)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("command", nargs="?", choices=("emit",))
    args = parser.parse_args(argv)
    if args.selftest:
        if args.command is not None:
            parser.error("--selftest accepts no command")
        return run_selftest()
    if args.command != "emit":
        parser.error("the emit command is required")
    if sys.stdout.isatty():
        print(
            "wifi_provision.py: refusing to print credential material to a terminal",
            file=sys.stderr,
        )
        return ERROR_EXIT
    try:
        with _emit_deadline(args.timeout):
            ssid, psk, url = resolve_values(args.env_file)
            packet = make_packet(ssid, psk, url)
            sys.stdout.buffer.write(packet)
            sys.stdout.buffer.flush()
    except (OSError, ProvisionError) as exc:
        print(f"wifi_provision.py: {exc}", file=sys.stderr)
        return ERROR_EXIT
    return 0


if __name__ == "__main__":
    sys.exit(main())
