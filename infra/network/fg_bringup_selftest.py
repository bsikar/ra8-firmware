#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline adversarial CLI and shell-word proofs for fg_bringup."""

from __future__ import annotations

import contextlib
import importlib.machinery
import io
import os
import shlex
import sys
from collections.abc import Callable
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Protocol

EXPECTED_MASKS = 3
WPA2_MAX_LENGTH = 63
REFUSAL_EXIT = 2
FAKE_LIVE_RESULT = 73


class _ExactLoader(Protocol):
    """Exact-path repository module loader."""

    def __call__(self, module_name: str, module_path: Path) -> object:
        """Load one reviewed file without searching ambient import paths."""


def _exact_load_rejected(loader: _ExactLoader, module_path: Path) -> bool:
    """Return whether an absent exact module fails closed."""
    try:
        loader("fg_ap_safety_missing", module_path)
    except (ImportError, OSError):
        return True
    return False


def isolated_import_checks(
    script_path: Path,
    exact_loader: _ExactLoader,
) -> list[tuple[str, bool]]:
    """Prove isolated startup works and rejects ambient module fallbacks."""
    with TemporaryDirectory(prefix="ra8-fg-import-") as temporary:
        root = Path(temporary)
        hostile_dir = root / "hostile"
        hostile_dir.mkdir()
        for name in ("fg_ap_safety.py", "sitecustomize.py"):
            (hostile_dir / name).write_text("HOSTILE = True\n", encoding="utf-8")
        safety_spec = importlib.machinery.PathFinder.find_spec("fg_ap_safety", [str(hostile_dir)])
        site_spec = importlib.machinery.PathFinder.find_spec("sitecustomize", [str(hostile_dir)])
        previous_pythonpath = os.environ.get("PYTHONPATH")
        os.environ["PYTHONPATH"] = str(hostile_dir)
        try:
            ambient_absent = str(hostile_dir) not in sys.path
            local_safety = exact_loader(
                "fg_ap_safety_selftest",
                script_path.parent / "fg_ap_safety.py",
            )
            exact_local = (
                Path(str(getattr(local_safety, "__file__", ""))).resolve()
                == (script_path.parent / "fg_ap_safety.py").resolve()
            )
            missing_rejected = _exact_load_rejected(
                exact_loader,
                root / "missing/fg_ap_safety.py",
            )
        finally:
            if previous_pythonpath is None:
                os.environ.pop("PYTHONPATH", None)
            else:
                os.environ["PYTHONPATH"] = previous_pythonpath
        return [
            (
                "hostile PYTHONPATH module is a non-vacuous import candidate",
                safety_spec is not None,
            ),
            ("hostile site hook is a non-vacuous import candidate", site_spec is not None),
            (
                "exact loader selects the reviewed local sibling",
                exact_local,
            ),
            (
                "isolated interpreter disables environment and user-site paths",
                sys.flags.isolated == 1
                and sys.flags.ignore_environment == 1
                and sys.flags.no_user_site == 1
                and sys.flags.safe_path,
            ),
            ("isolated sys.path excludes hostile PYTHONPATH and site hook", ambient_absent),
            (
                "missing exact sibling fails closed without ambient fallback",
                missing_rejected,
            ),
        ]


class _LiveRunner(Protocol):
    """Callable boundary that owns all live dependencies."""

    def __call__(self, mode: str, path: Path, lines: list[str]) -> int:
        """Run one explicit live mode."""


class _Main(Protocol):
    """Injectable fg_bringup command dispatcher."""

    def __call__(
        self,
        argv: list[str],
        *,
        live_runner: _LiveRunner,
    ) -> int:
        """Dispatch one synthetic argument vector."""


class _PrimaryLan(Protocol):
    """Primary-LAN detector."""

    def __call__(self, text: str) -> str:
        """Return the primary FortiGate LAN interface name."""


class _RenderConfig(Protocol):
    """Model-specific declaration renderer."""

    def __call__(self, lines: list[str], lan: str) -> list[str]:
        """Render declaration lines for one primary LAN name."""


class _ConfigModule(Protocol):
    """FortiGate declaration parser surface used by offline selftests."""

    CAM_RESERVED_IP: str
    DEFAULT_CONF: Path
    PROHIBITED_TRANSIENT_WIN_IPS: frozenset[str]
    PROHIBITED_TRANSIENT_WIN_MAC: str
    STAR_RESERVED_IP: str

    def load_config_lines(self, path: Path) -> list[str]:
        """Load one declaration."""

    def config_lint_errors(self, lines: list[str]) -> list[str]:
        """Return declaration findings."""


class _ArgValidator(Protocol):
    """Live-mode argument-count validator."""

    def __call__(self, mode: str, argument_count: int) -> bool:
        """Return whether one live argv shape is accepted."""


class _RuntimeSafe(Protocol):
    """Pure live-runtime boundary predicate."""

    def __call__(
        self,
        isolated: int,
        sanitized: str | None,
        prefix: Path,
        base_prefix: Path,
        expected_venv: Path,
    ) -> bool:
        """Return whether one synthetic startup state is safe."""


def _lan_detection_checks(primary_lan: _PrimaryLan) -> list[tuple[str, bool]]:
    """Return both-direction checks for primary LAN detection."""
    two_switch = (
        "config system interface\n"
        '    edit "wan1"\n        set mode dhcp\n    next\n'
        '    edit "lan"\n        set ip 10.0.40.1 255.255.255.0\n'
        "        set type hard-switch\n    next\n"
        '    edit "lan-even"\n        set ip 10.0.41.1 255.255.255.0\n'
        "        set type hard-switch\n    next\nend\n"
    )
    internal_box = (
        "config system interface\n"
        '    edit "internal"\n        set ip 10.0.40.1 255.255.255.0\n    next\n'
        '    edit "lan-even"\n        set ip 10.0.41.1 255.255.255.0\n    next\nend\n'
    )
    cases = (
        ("primary read past lan-even", two_switch, "lan"),
        ("internal primary is retained", internal_box, "internal"),
        ("empty dump defaults to internal", "", "internal"),
        ("lan-even alone never reads as lan", 'edit "lan-even"\n', "internal"),
    )
    return [
        (f"{label}: got {primary_lan(text)!r}, want {want!r}", primary_lan(text) == want)
        for label, text, want in cases
    ]


def _rewrite_checks(render: _RenderConfig) -> list[tuple[str, bool]]:
    """Return checks for model-specific LAN token rendering."""
    declaration = (
        'edit "internal"\n set srcintf "internal"\n'
        ' set dstintf "lan-even"\n set srcintf "lan-even"\n'
    )
    rewritten = "\n".join(render(declaration.splitlines(), "lan"))
    return [
        ("token rewrite resolves primary", '"internal"' not in rewritten and '"lan"' in rewritten),
        (
            "token rewrite keeps lan-even",
            rewritten.count('"lan-even"') == declaration.count('"lan-even"'),
        ),
        ("offline path does not import pyserial", "serial" not in sys.modules),
        ("offline path does not import OpenBao", "openbao_client" not in sys.modules),
    ]


def _move_reservations_to_wrong_server(lines: list[str]) -> list[str]:
    """Return a fixture whose odd reservations sit under the wrong server edit."""
    fixture = list(lines)
    dhcp_start = fixture.index("config system dhcp server")
    server_edit = fixture.index("edit 1", dhcp_start + 1)
    fixture[server_edit] = "edit 9"
    return fixture


def _mutate_reservation_line(
    lines: list[str],
    reserved_ip: str,
    old_line: str,
    new_line: str,
) -> list[str]:
    """Replace one line inside the reservation identified by its address."""
    fixture = list(lines)
    ip_line = fixture.index(f"set ip {reserved_ip}")
    search_start = max(0, ip_line - 1)
    search_end = min(len(fixture), ip_line + 4)
    line_index = fixture.index(old_line, search_start, search_end)
    fixture[line_index] = new_line
    return fixture


def _add_unexpected_reservation(lines: list[str]) -> list[str]:
    """Add a complete but unreviewed fixture reservation to DHCP server 1."""
    fixture = list(lines)
    block_start = fixture.index("config reserved-address")
    block_end = fixture.index("end", block_start + 1)
    fixture[block_end:block_end] = [
        "edit 9",
        "set ip 10.0.40.198",
        "set mac 02:00:00:00:00:01",
        'set description "transient-fixture"',
        "next",
    ]
    return fixture


def _transient_reservation_checks(
    config: _ConfigModule,
    tracked: list[str],
) -> list[tuple[str, bool]]:
    """Return mutations proving transient Windows clients stay unreserved."""
    transient_ip = sorted(config.PROHIBITED_TRANSIENT_WIN_IPS)[0]
    transient_address = config.config_lint_errors(
        _mutate_reservation_line(
            tracked,
            config.STAR_RESERVED_IP,
            f"set ip {config.STAR_RESERVED_IP}",
            f"set ip {transient_ip}",
        )
    )
    transient_mac = config.config_lint_errors(
        _mutate_reservation_line(
            tracked,
            config.STAR_RESERVED_IP,
            "set mac 00:05:1b:db:75:d3",
            f"set mac {config.PROHIBITED_TRANSIENT_WIN_MAC}",
        )
    )
    return [
        (
            "transient Win11 address is rejected",
            any("must not be reserved" in item for item in transient_address),
        ),
        (
            "transient Win11 MAC is rejected",
            any("transient Win11 MAC" in item for item in transient_mac),
        ),
    ]


def _required_address_checks(
    config: _ConfigModule,
    tracked: list[str],
) -> list[tuple[str, bool]]:
    """Prove the required-reservation comparator binds IP and MAC itself."""
    wrong_ip = config.config_lint_errors(
        _mutate_reservation_line(
            tracked,
            config.STAR_RESERVED_IP,
            f"set ip {config.STAR_RESERVED_IP}",
            "set ip 10.0.40.197",
        )
    )
    wrong_mac = config.config_lint_errors(
        _mutate_reservation_line(
            tracked,
            config.CAM_RESERVED_IP,
            "set mac 88:a2:9e:9b:d0:ea",
            "set mac 02:00:00:00:00:fe",
        )
    )
    return [
        (
            "reservation IP is exact independently of transient-client policy",
            any("star reservation" in item for item in wrong_ip),
        ),
        (
            "reservation MAC is exact independently of transient-client policy",
            any("camera relay reservation" in item for item in wrong_mac),
        ),
    ]


def _exact_reservation_checks(
    config: _ConfigModule,
    tracked: list[str],
) -> list[tuple[str, bool]]:
    """Return mutations proving every declared reservation field is exact."""
    wrong_description = config.config_lint_errors(
        _mutate_reservation_line(
            tracked,
            config.STAR_RESERVED_IP,
            'set description "star-bench-wired"',
            'set description "wrong-description"',
        )
    )
    wrong_id = config.config_lint_errors(
        _mutate_reservation_line(
            tracked,
            config.CAM_RESERVED_IP,
            "edit 1",
            "edit 9",
        )
    )
    wrong_server = config.config_lint_errors(_move_reservations_to_wrong_server(tracked))
    unexpected = config.config_lint_errors(_add_unexpected_reservation(tracked))
    checks = [
        (
            "reservation description is exact",
            any("star reservation" in item for item in wrong_description),
        ),
        (
            "reservation ID is exact",
            any("camera relay reservation" in item for item in wrong_id),
        ),
        (
            "reservation DHCP server is exact",
            any("DHCP server 1" in item for item in wrong_server),
        ),
        (
            "unexpected reservation is rejected",
            any("expected exactly" in item for item in unexpected),
        ),
    ]
    return _required_address_checks(config, tracked) + checks


def _reservation_contract_checks(
    config: _ConfigModule,
    tracked: list[str],
) -> list[tuple[str, bool]]:
    """Return mutations proving the exact three-reservation contract."""
    return _transient_reservation_checks(config, tracked) + _exact_reservation_checks(
        config, tracked
    )


def declaration_selftest_checks(
    primary_lan: _PrimaryLan,
    render: _RenderConfig,
    config: _ConfigModule,
) -> tuple[list[tuple[str, bool]], list[str]]:
    """Return declaration must-fire, must-stay-quiet, and non-vacuity checks."""
    try:
        tracked = config.load_config_lines(config.DEFAULT_CONF)
    except (OSError, UnicodeError) as exc:
        return [(f"tracked declaration load: {exc}", False)], []
    tracked_errors = config.config_lint_errors(tracked)
    checks = _lan_detection_checks(primary_lan) + _rewrite_checks(render)
    checks.extend(
        [
            ("tracked declaration passes lint", not tracked_errors),
            (
                "unclosed config block is rejected",
                any(
                    "unclosed config block" in item
                    for item in config.config_lint_errors(tracked[:-1])
                ),
            ),
            (
                "empty replay fails non-vacuity",
                any("non-vacuity" in item for item in config.config_lint_errors([])),
            ),
        ]
    )
    checks.extend(_reservation_contract_checks(config, tracked))
    return checks, tracked_errors


def _recipe_body(text: str, name: str) -> str:
    """Return one Just recipe body, excluding following recipes and aliases."""
    lines = text.splitlines()
    start = next(
        (
            index
            for index, line in enumerate(lines)
            if line == f"{name}:" or line.startswith(f"{name} ")
        ),
        -1,
    )
    if start < 0:
        return ""
    body: list[str] = []
    for line in lines[start + 1 :]:
        if line and not line.startswith((" ", "\t")):
            break
        body.append(line)
    return "\n".join(body)


def _fortigate_recipe_findings(text: str) -> list[str]:
    """Return unsafe or missing FortiGate Just recipe contracts."""
    findings: list[str] = []
    clean_line = next((line for line in text.splitlines() if line.startswith("clean_env :=")), "")
    clean_tokens = ("/usr/bin/env -i", "PYTHONNOUSERSITE=1", "RA8_INFRA_SANITIZED=v1")
    if not all(token in clean_line for token in clean_tokens) or "PYTHONPATH" in clean_line:
        findings.append("clean_env is not the reviewed empty isolated environment")
    modes = {
        "fortigate_config_selftest": ("--selftest config", "{{ quote(python) }} -B -I"),
        "fortigate_config_lint": ("config-lint", ".venv/bin/python3 -B -I"),
        "fortigate_replay_dry_run": ("replay-dry-run", ".venv/bin/python3 -B -I"),
        "fortigate_bootstrap": ("bootstrap", ".venv/bin/python3 -B -I"),
        "fortigate_ap_configure": ("ap-configure", ".venv/bin/python3 -B -I"),
        "fortigate_verify": ("verify", ".venv/bin/python3 -B -I"),
    }
    live = {"fortigate_bootstrap", "fortigate_ap_configure", "fortigate_verify"}
    for recipe, (mode, interpreter) in modes.items():
        body = _recipe_body(text, recipe)
        required = ("{{ clean_env }}", interpreter, "infra/network/fg_bringup.py", mode)
        if not body or not all(token in body for token in required):
            findings.append(f"{recipe} lacks its exact sanitized isolated argv")
        if recipe in live and 'env_args+=("FG_CONSOLE_TTY=$tty")' not in body:
            findings.append(f"{recipe} lacks shell-safe explicit TTY forwarding")
        if "HIL_OPENBAO_ENV" in body or "PYTHONPATH" in body:
            findings.append(f"{recipe} admits an ambient Python or credential path")
    signatures = {
        "fortigate_bootstrap": "Factory-reset and reconfigure the FortiGate?",
        "fortigate_ap_configure": "Reconfigure the bench AP over the FortiGate console?",
    }
    lines = text.splitlines()
    for recipe, prompt in signatures.items():
        recipe_index = next(
            (index for index, line in enumerate(lines) if line.startswith(f"{recipe} ")),
            -1,
        )
        previous = lines[recipe_index - 1] if recipe_index > 0 else ""
        if not previous.startswith(f'[confirm("{prompt}'):
            findings.append(f"{recipe} lacks a destructive confirmation")
    return findings


def _documentation_findings(repo_root: Path) -> list[str]:
    """Return raw live Python commands that bypass the documented Just boundary."""
    findings: list[str] = []
    required = {
        repo_root / "infra/network/README.md": (
            "just infra::fortigate_bootstrap",
            "just infra::fortigate_ap_configure",
            "just infra::fortigate_verify",
        ),
        repo_root / "infra/network/fortigate-bench.conf": ("just infra::fortigate_bootstrap",),
    }
    raw = ("python3 infra/network/fg_bringup.py", "fg_bringup.py verify")
    for path, required_commands in required.items():
        text = path.read_text(encoding="utf-8")
        if any(token in text for token in raw):
            findings.append(f"{path.name} documents a raw live Python entrypoint")
        if not all(command in text for command in required_commands):
            findings.append(f"{path.name} omits a required FortiGate Just entrypoint")
    return findings


def just_recipe_selftest_checks(just_path: Path) -> list[tuple[str, bool]]:
    """Prove documented FortiGate entrypoints retain their isolation contract."""
    text = just_path.read_text(encoding="utf-8")
    repo_root = just_path.parents[1]
    weakened_isolation = text.replace(".venv/bin/python3 -B -I", ".venv/bin/python3 -B", 1)
    weakened_bytecode = text.replace(
        "{{ quote(python) }} -B -I",
        "{{ quote(python) }} -I",
        1,
    )
    weakened_tty = text.replace('env_args+=("FG_CONSOLE_TTY=$tty")', 'env_args+=("$tty")', 1)
    weakened_confirmation = text.replace(
        '[confirm("Factory-reset and reconfigure the FortiGate? '
        'This disrupts the bench network.")]\n',
        "",
        1,
    )
    return [
        ("FortiGate Just recipes retain exact safe argv", not _fortigate_recipe_findings(text)),
        (
            "FortiGate docs contain no raw live Python command",
            not _documentation_findings(repo_root),
        ),
        (
            "recipe checker rejects missing isolated mode",
            bool(_fortigate_recipe_findings(weakened_isolation)),
        ),
        (
            "recipe checker rejects source-tree bytecode writes",
            bool(_fortigate_recipe_findings(weakened_bytecode)),
        ),
        (
            "recipe checker rejects unsafe TTY forwarding",
            bool(_fortigate_recipe_findings(weakened_tty)),
        ),
        (
            "recipe checker rejects missing destructive confirmation",
            bool(_fortigate_recipe_findings(weakened_confirmation)),
        ),
    ]


class _Assignment(Protocol):
    """Shell-safe UCI assignment renderer."""

    def __call__(self, option: str, value: str) -> str:
        """Render one assignment or reject it."""


class _Redact(Protocol):
    """Secret registration callable."""

    def __call__(self, value: str) -> None:
        """Register one value for masking."""


class _Mask(Protocol):
    """Transcript masking callable."""

    def __call__(self, value: str) -> str:
        """Mask registered values in text."""


class _ValidatePsk(Protocol):
    """WPA2 passphrase validator."""

    def __call__(self, value: str) -> str:
        """Return a valid passphrase or reject it."""


class _CheckedApCommand(Protocol):
    """Status-marker AP command boundary."""

    def __call__(
        self,
        ser: object,
        cmd: str,
        seconds: float = 4.0,
        *,
        secret: bool = False,
        transport: tuple[Callable[..., None], Callable[..., str]],
    ) -> str:
        """Execute through an injected fake serial transport."""


def _uci_value_rejected(assignment: _Assignment, value: str) -> bool:
    """Return whether a UCI value is refused before serial transport."""
    try:
        assignment("wireless.bench.key", value)
    except ValueError:
        return True
    return False


def _redaction_checks(
    redact: _Redact,
    mask: _Mask,
    secrets: list[str],
) -> list[tuple[str, bool]]:
    """Prove overlapping raw/rendered credentials are fully masked."""
    old_secrets = list(secrets)
    try:
        secrets.clear()
        short = "bench"
        long = "bench-network-psk"
        rendered = shlex.quote(long)
        for value in (short, long, rendered, long):
            redact(value)
        masked = mask(f"raw={long} short={short} rendered={rendered}")
    finally:
        secrets[:] = old_secrets
    return [
        ("longest overlapping credential is masked whole", "network-psk" not in masked),
        ("short overlapping credential is masked", short not in masked),
        (
            "rendered credential is masked before its raw substring",
            rendered not in masked,
        ),
        (
            "duplicate registration does not change masking",
            masked.count("<REDACTED>") == EXPECTED_MASKS,
        ),
    ]


def _psk_form_checks(validate_psk: _ValidatePsk) -> list[tuple[str, bool]]:
    """Prove the exact printable WPA2 passphrase contract."""
    return [
        ("8-byte WPA2 passphrase accepted", validate_psk("12345678") == "12345678"),
        (
            "63-byte WPA2 passphrase accepted",
            len(validate_psk("x" * WPA2_MAX_LENGTH)) == WPA2_MAX_LENGTH,
        ),
        ("7-byte WPA2 passphrase rejected", _value_rejected(validate_psk, "x" * 7)),
        ("64-byte WPA2 passphrase rejected", _value_rejected(validate_psk, "x" * 64)),
        (
            "non-printable WPA2 passphrase rejected",
            _value_rejected(validate_psk, "abcd\t1234"),
        ),
    ]


def _value_rejected(validate: _ValidatePsk, value: str) -> bool:
    """Return whether one WPA2 passphrase is rejected."""
    try:
        validate(value)
    except ValueError:
        return True
    return False


def _ap_status_case(checked: _CheckedApCommand, output: str) -> tuple[bool, list[str]]:
    """Run one AP command against a fake serial transport."""
    sent: list[str] = []

    def sender(_ser: object, command: str, *, secret: bool = False) -> None:
        _ = secret
        sent.append(command)

    def drainer(_ser: object, _seconds: float) -> str:
        return output

    try:
        checked(object(), "uci commit wireless", transport=(sender, drainer))
    except RuntimeError:
        return False, sent
    return True, sent


def _ap_status_checks(checked: _CheckedApCommand) -> list[tuple[str, bool]]:
    """Prove AP success cannot be claimed without one zero status marker."""
    success = _ap_status_case(checked, "ok\n__RA8_AP_RC__0\n")
    failed = _ap_status_case(checked, "bad\n__RA8_AP_RC__1\n")
    missing = _ap_status_case(checked, "no marker")
    duplicate = _ap_status_case(checked, "__RA8_AP_RC__0\n__RA8_AP_RC__0\n")
    return [
        ("fake serial accepts one zero AP status", success[0]),
        ("fake serial wrapper emits a fixed status marker", "printf" in success[1][0]),
        ("fake serial rejects nonzero AP status", not failed[0]),
        ("fake serial rejects missing AP status", not missing[0]),
        ("fake serial rejects ambiguous duplicate AP status", not duplicate[0]),
    ]


def psk_selftest_checks(
    assignment: _Assignment,
    redact: _Redact,
    mask: _Mask,
    secrets: list[str],
    safety: tuple[_ValidatePsk, _CheckedApCommand],
) -> list[tuple[str, bool]]:
    """Return shell-word, control-byte, and redaction checks for the AP PSK."""
    adversarial = "quote' ; reboot; $(id) #"
    validate_psk, checked = safety
    command = assignment("wireless.bench.key", adversarial)
    words = shlex.split(command)
    old_secrets = list(secrets)
    try:
        secrets.clear()
        redact(adversarial)
        redact(shlex.quote(adversarial))
        redacted = mask(command)
    finally:
        secrets[:] = old_secrets
    expected_word_count = 3
    checks = [
        (
            "quoted PSK remains one UCI assignment word",
            words == ["uci", "set", f"wireless.bench.key={adversarial}"],
        ),
        (
            "quoted PSK metacharacters cannot form a second command",
            len(words) == expected_word_count and words[2].endswith(adversarial),
        ),
        (
            "PSK is redacted from the rendered command",
            adversarial not in redacted and "<REDACTED>" in redacted,
        ),
        ("newline PSK is rejected", _uci_value_rejected(assignment, "line1\nline2")),
        (
            "carriage-return PSK is rejected",
            _uci_value_rejected(assignment, "left\rright"),
        ),
        ("NUL PSK is rejected", _uci_value_rejected(assignment, "left\0right")),
        ("tab PSK is rejected", _uci_value_rejected(assignment, "left\tright")),
    ]
    return (
        checks
        + _redaction_checks(redact, mask, secrets)
        + _psk_form_checks(validate_psk)
        + _ap_status_checks(checked)
    )


def entrypoint_safety_checks(main: _Main) -> list[tuple[str, bool]]:
    """Prove non-live argv shapes cannot cross the live dependency boundary."""
    live_calls: list[str] = []

    def forbidden_live(
        mode: str,
        _path: Path,
        _lines: list[str],
    ) -> int:
        live_calls.append(mode)
        return 99

    cases = (
        ("zero arguments", [], 2),
        ("explicit help", ["--help"], 0),
        ("invalid mode", ["not-a-mode"], 2),
    )
    checks: list[tuple[str, bool]] = []
    for label, argv, expected in cases:
        before = len(live_calls)
        with (
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            result = main(argv, live_runner=forbidden_live)
        checks.append(
            (
                f"{label} cannot load credentials, serial, or hardware",
                result == expected and len(live_calls) == before,
            )
        )
    return checks


def cli_argument_selftest_checks(valid: _ArgValidator) -> list[tuple[str, bool]]:
    """Return pure checks for explicit live CLI argument shapes."""
    return [
        ("implicit no-argument login is rejected", not valid("login", 1)),
        ("explicit login remains valid", valid("login", 2)),
        ("login with an extra argument is rejected", not valid("login", 3)),
    ]


def _guard_dispatch_results(main: _Main) -> tuple[int, int, int, list[str]]:
    """Exercise raw and sanitized dispatch against a fake live boundary."""
    live_calls: list[str] = []

    def fake_live(mode: str, _path: Path, _lines: list[str]) -> int:
        live_calls.append(mode)
        return FAKE_LIVE_RESULT

    previous_sanitized = os.environ.pop("RA8_INFRA_SANITIZED", None)
    try:
        with (
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            raw_result = main(["verify"], live_runner=fake_live)
        raw_calls = len(live_calls)
        os.environ["RA8_INFRA_SANITIZED"] = "v1"
        with (
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            safe_result = main(["verify"], live_runner=fake_live)
    finally:
        if previous_sanitized is None:
            os.environ.pop("RA8_INFRA_SANITIZED", None)
        else:
            os.environ["RA8_INFRA_SANITIZED"] = previous_sanitized
    return raw_result, raw_calls, safe_result, live_calls


def live_runtime_selftest_checks(
    main: _Main,
    runtime_safe: _RuntimeSafe,
    expected_venv: Path,
) -> list[tuple[str, bool]]:
    """Prove unsafe live startup is rejected before injected dependencies."""
    raw_result, raw_calls, safe_result, live_calls = _guard_dispatch_results(main)
    prefix = Path(sys.prefix)
    base_prefix = Path(sys.base_prefix)
    return [
        (
            "raw live mode refuses before its dependency boundary",
            raw_result == REFUSAL_EXIT and raw_calls == 0,
        ),
        (
            "sanitized isolated managed-venv control reaches only the fake boundary",
            safe_result == FAKE_LIVE_RESULT and live_calls == ["verify"],
        ),
        (
            "pure guard rejects non-isolated startup",
            not runtime_safe(0, "v1", prefix, base_prefix, expected_venv),
        ),
        (
            "pure guard rejects an unsanitized environment",
            not runtime_safe(1, None, prefix, base_prefix, expected_venv),
        ),
        (
            "pure guard rejects a Python environment outside the repo",
            not runtime_safe(
                1,
                "v1",
                expected_venv.parent / "not-managed-venv",
                base_prefix,
                expected_venv,
            ),
        ),
        ("live guard does not import pyserial", "serial" not in sys.modules),
        ("live guard does not import OpenBao", "openbao_client" not in sys.modules),
    ]
