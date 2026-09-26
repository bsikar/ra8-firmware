#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Hold the RTOS/middleware symbol inventory of ``libs/`` to a declared ledger.

Invariant (#695, workstream (c)): no first-party library names an RTOS or
middleware API symbol. A scheduler, a USB device stack or a flash-translation
layer is reached through a seam bound under ``port/``; a library that spells
``_tx_timer_interrupt`` or ``lx_nor_flash_open`` has wired one specific
middleware into a tier meant to outlive it.

The tree does not satisfy that invariant yet, and #695 is design-only until the
owner schedules it, so this gate does NOT assert the end state. It freezes the
inventory that exists today: every leak site is declared below with the symbols
it names, and the sweep fails when tree and ledger disagree either way.

* A NEW symbol, or a leak in a library that has none today, fails. The
  inventory is a ceiling, so "write your own RTOS" cannot get further away
  while the seam is still being designed.
* A declared symbol that has GONE also fails, asking for the entry to be
  dropped in the same change. A burn-down that leaves the ledger untouched
  leaves a gate guarding a leak nobody has any more, which is the stale-checker
  defect class this repository treats as a red.

Identifiers are read from code only: comments and string literals are stripped
first, so prose explaining a seam may name the middleware it seams.

Run::

    check_rtos_symbol_isolation.py                 # sweep every first-party library
    check_rtos_symbol_isolation.py libs/x/src/y.c  # scan listed files (no ledger check)
    check_rtos_symbol_isolation.py --print-ledger  # emit the observed inventory
    check_rtos_symbol_isolation.py --selftest      # prove both directions

Exit 0 on agreement, 1 when tree and ledger disagree, and 2 when the sweep or
the ledger collapses below its non-vacuity floor.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path
from selftest_assert import expect, report

REPO_ROOT = Path(__file__).resolve().parents[2]
LIBS_ROOT = REPO_ROOT / "libs"

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")
EXCLUDE_FRAGMENTS = ("/third_party/", "/ra8_fonts/")

PROGRAM = "check_rtos_symbol_isolation.py"
OPTION_ARG_COUNT = 2

# Floors. A sweep that has stopped matching must not report a clean tree.
FILE_FLOOR = 600
LEDGER_SITE_FLOOR = 6
LEDGER_SYMBOL_FLOOR = 40

# Vendor namespaces. The leading-underscore forms are the middleware's own
# internal entry points; the bare forms are its published API.
INTERNAL_PREFIXES: dict[str, str] = {
    "_tx_": "ThreadX",
    "_ux_": "USBX",
    "_nx_": "NetX Duo",
    "_fx_": "FileX",
    "_lx_": "LevelX",
}
PUBLIC_PREFIXES: dict[str, str] = {
    "ux_": "USBX",
    "nx_": "NetX Duo",
    "fx_": "FileX",
    "lx_": "LevelX",
}

# ThreadX has no namespace of its own: `tx_` collides with every transmit-side
# field in this tree (`tx_len`, `tx_pool`, `tx_queue_index`, `tx_threshold`), so
# the ThreadX rule needs an object family plus a service verb, never `tx_`
# alone. The false-positive control in --selftest pins exactly that.
THREADX_OBJECTS = (
    "thread",
    "mutex",
    "semaphore",
    "event_flags",
    "queue",
    "byte_pool",
    "block_pool",
    "timer",
)
THREADX_VERBS = (
    "abort",
    "activate",
    "allocate",
    "change",
    "cleanup",
    "create",
    "deactivate",
    "delete",
    "entry_exit_notify",
    "extend",
    "flush",
    "front_send",
    "get",
    "identify",
    "info_get",
    "performance_info_get",
    "performance_system_info_get",
    "preemption_change",
    "prioritize",
    "priority_change",
    "put",
    "receive",
    "relinquish",
    "release",
    "reset",
    "resume",
    "send",
    "set",
    "sleep",
    "stack_error_notify",
    "suspend",
    "terminate",
    "time_slice_change",
    "wait_abort",
)
THREADX_API_RE = re.compile(
    rf"^tx_(?:{'|'.join(THREADX_OBJECTS)})_(?:{'|'.join(THREADX_VERBS)})[a-z0-9_]*$"
)
THREADX_SINGLETONS = frozenset(
    {
        "tx_application_define",
        "tx_initialize_kernel_enter",
        "tx_interrupt_control",
        "tx_kernel_enter",
        "tx_time_get",
        "tx_trace_disable",
        "tx_trace_enable",
    }
)

# A first-party identifier carrying the middleware's NAME is a leak too:
# `g_ra8_threadx_systick_ready` is ra8_core's own symbol, and it is still why
# Ring-1 cannot be read without knowing which scheduler is linked.
NAME_TOKENS: dict[str, str] = {
    "azure_rtos": "Azure RTOS",
    "filex": "FileX",
    "levelx": "LevelX",
    "netxduo": "NetX Duo",
    "threadx": "ThreadX",
    "usbx": "USBX",
}

# The declared inventory: what libs/ names today, verified on dev @73a62d3.
# Shrink an entry in the same change that removes the symbol.
DECLARED_SITES: dict[str, frozenset[str]] = {
    # LevelX reached directly by the cache store; #616 owns the standalone-mode
    # question for this dependency.
    "libs/ra8_cache_store/src/ra8_cache_store.c": frozenset({"lx_nor_flash_close"}),
    "libs/ra8_cache_store/src/ra8_cache_store_mount.c": frozenset(
        {
            "internal_open_levelx",
            "lx_nor_flash_format",
            "lx_nor_flash_initialize",
            "lx_nor_flash_open",
            "lx_nor_flash_sector_read",
            "lx_nor_flash_sector_release",
            "lx_nor_flash_sector_write",
        }
    ),
    # The Ring-1 leak #695 names: the SysTick handler dispatches into ThreadX
    # and USBX through weak externs.
    "libs/ra8_core/src/ra8_time.c": frozenset(
        {
            "_tx_timer_interrupt",
            "g_ra8_threadx_systick_ready",
            "ux_dcd_ra8_usb_irq_reenable",
        }
    ),
    # USBX device stack in the DFU library, including one field of its PUBLIC
    # header, so a DFU consumer inherits the middleware name.
    "libs/ra8_dfu/inc/ra8_dfu_device.h": frozenset({"usbx_pool"}),
    "libs/ra8_dfu/src/ra8_dfu_device.c": frozenset(
        {
            "_ux_device_class_dfu_entry",
            "_ux_device_stack_class_register",
            "_ux_device_stack_initialize",
            "_ux_system_initialize",
            "usbx_pool",
            "ux_dcd_ra8_usb_initialize",
            "ux_slave_class_dfu_parameter_capabilities",
            "ux_slave_class_dfu_parameter_framework",
            "ux_slave_class_dfu_parameter_framework_length",
            "ux_slave_class_dfu_parameter_get_status",
            "ux_slave_class_dfu_parameter_instance_activate",
            "ux_slave_class_dfu_parameter_instance_deactivate",
            "ux_slave_class_dfu_parameter_notify",
            "ux_slave_class_dfu_parameter_read",
            "ux_slave_class_dfu_parameter_will_detach",
            "ux_slave_class_dfu_parameter_write",
        }
    ),
    # ThreadX mutex/thread/time services behind a library-local shim header.
    "libs/ra8_wdt_supervisor/src/ra8_wdt_sup_tx_shim_internal.h": frozenset(
        {
            "tx_mutex_create",
            "tx_mutex_delete",
            "tx_mutex_get",
            "tx_mutex_put",
            "tx_thread_create",
            "tx_thread_delete",
            "tx_thread_sleep",
            "tx_thread_terminate",
            "tx_time_get",
        }
    ),
    "libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c": frozenset(
        {
            "tx_mutex_create",
            "tx_mutex_delete",
            "tx_mutex_get",
            "tx_mutex_put",
            "tx_thread_create",
            "tx_thread_delete",
            "tx_thread_sleep",
            "tx_thread_terminate",
            "tx_time_get",
        }
    ),
}

BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
STRING_RE = re.compile(r'"(?:\\.|[^"\\\n])*"')
IDENTIFIER_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def strip_non_code(text: str) -> str:
    """Remove block comments, line comments and string literals from C text."""
    without_comments = LINE_COMMENT_RE.sub("", BLOCK_COMMENT_RE.sub(" ", text))
    return STRING_RE.sub('""', without_comments)


def classify(identifier: str) -> str | None:
    """Name the middleware an identifier belongs to, or None when it is ours."""
    for prefix, vendor in INTERNAL_PREFIXES.items():
        if identifier.startswith(prefix):
            return vendor
    for prefix, vendor in PUBLIC_PREFIXES.items():
        if identifier.startswith(prefix):
            return vendor
    if identifier in THREADX_SINGLETONS or THREADX_API_RE.match(identifier):
        return "ThreadX"
    lowered = identifier.lower()
    for token, vendor in NAME_TOKENS.items():
        if token in lowered:
            return vendor
    return None


def scan_text(text: str) -> dict[str, str]:
    """Return every middleware symbol named in code, mapped to its vendor."""
    code = strip_non_code(text)
    found: dict[str, str] = {}
    for identifier in set(IDENTIFIER_RE.findall(code)):
        vendor = classify(identifier)
        if vendor is not None:
            found[identifier] = vendor
    return found


def _in_scope(path: Path) -> bool:
    posix = path.as_posix()
    if any(fragment in posix for fragment in EXCLUDE_FRAGMENTS):
        return False
    return not is_build_output_path(posix)


def enumerate_targets(paths: list[str]) -> list[Path]:
    """List the library sources to scan: the caller's paths, else all of libs/."""
    if paths:
        return [Path(p) for p in paths if Path(p).suffix in SOURCE_SUFFIXES]
    return sorted(
        path
        for path in LIBS_ROOT.rglob("*")
        if path.suffix in SOURCE_SUFFIXES and path.is_file() and _in_scope(path)
    )


def observe(targets: list[Path]) -> dict[str, dict[str, str]]:
    """Map each scanned file's repository-relative path to its symbol findings."""
    observed: dict[str, dict[str, str]] = {}
    for path in targets:
        found = scan_text(path.read_text(encoding="utf-8", errors="replace"))
        if not found:
            continue
        try:
            rel = path.resolve().relative_to(REPO_ROOT).as_posix()
        except ValueError:
            rel = path.as_posix()
        observed[rel] = found
    return observed


def compare(observed: dict[str, dict[str, str]], full_sweep: bool) -> list[str]:
    """Diff the observed inventory against the ledger, in both directions."""
    findings: list[str] = []
    for rel, found in sorted(observed.items()):
        declared = DECLARED_SITES.get(rel, frozenset())
        findings.extend(
            f"NEW_LEAK {rel}: {symbol} ({found[symbol]}) is not declared"
            for symbol in sorted(set(found) - declared)
        )
    if not full_sweep:
        return findings
    for rel, declared in sorted(DECLARED_SITES.items()):
        if not (REPO_ROOT / rel).is_file():
            findings.append(f"STALE_LEDGER {rel}: declared file no longer exists")
            continue
        findings.extend(
            f"STALE_LEDGER {rel}: {symbol} is gone; drop it from the ledger"
            for symbol in sorted(declared - set(observed.get(rel, {})))
        )
    return findings


def _print_ledger(observed: dict[str, dict[str, str]]) -> int:
    """Emit the observed inventory in ledger form, for a burn-down change."""
    for rel, found in sorted(observed.items()):
        print(f'    "{rel}": frozenset(')
        print("        {")
        for symbol in sorted(found):
            print(f'            "{symbol}",')
        print("        }")
        print("    ),")
    return 0


def _selftest_detector(failures: list[str]) -> None:
    """Assert every leak shape fires and transmit-side fields stay quiet."""
    print(" detector fires")
    expect(
        scan_text("void f(void){ _tx_timer_interrupt(); }") == {"_tx_timer_interrupt": "ThreadX"},
        "ThreadX internal entry point detected",
        failures,
    )
    expect(classify("tx_mutex_get") == "ThreadX", "ThreadX object+verb API detected", failures)
    expect(classify("tx_mutex_put") == "ThreadX", "ThreadX release-side verb detected", failures)
    expect(classify("tx_time_get") == "ThreadX", "ThreadX singleton service detected", failures)
    expect(classify("lx_nor_flash_open") == "LevelX", "LevelX API detected", failures)
    expect(classify("_ux_device_stack_initialize") == "USBX", "USBX internal detected", failures)
    expect(
        classify("g_ra8_threadx_systick_ready") == "ThreadX",
        "first-party identifier carrying a middleware name detected",
        failures,
    )
    expect(classify("usbx_pool") == "USBX", "middleware-named struct field detected", failures)

    print(" detector stays quiet (false-positive control)")
    benign = (
        "tx_len",
        "tx_pool",
        "tx_queue_index",
        "tx_threshold",
        "tx_buf_type",
        "ra8_time_ms",
        "fw_os_mutex_t",
    )
    for identifier in benign:
        expect(classify(identifier) is None, f"{identifier} is not a middleware symbol", failures)
    expect(
        scan_text('/* ThreadX ticks via _tx_timer_interrupt */\nconst char* s = "ux_device";\n')
        == {},
        "comments and string literals are not code",
        failures,
    )


def _full_ledger_view() -> dict[str, dict[str, str]]:
    """Build the observation a tree exactly matching the ledger would produce."""
    return {rel: dict.fromkeys(symbols, "ThreadX") for rel, symbols in DECLARED_SITES.items()}


def _selftest_ledger(failures: list[str]) -> None:
    """Assert the comparison fails on a new leak and on a stale entry."""
    print(" ledger comparison, both directions")
    site = next(iter(DECLARED_SITES))
    expect(
        compare(_full_ledger_view(), full_sweep=True) == [],
        "a tree matching the ledger exactly is clean",
        failures,
    )
    with_new = _full_ledger_view()
    with_new[site] = with_new[site] | {"tx_thread_resume": "ThreadX"}
    expect(
        any(f.startswith("NEW_LEAK") for f in compare(with_new, full_sweep=True)),
        "an undeclared symbol at a declared site fails",
        failures,
    )
    fresh = _full_ledger_view()
    fresh["libs/ra8_gfx/src/ra8_gfx.c"] = {"nx_packet_allocate": "NetX Duo"}
    expect(
        any(f.startswith("NEW_LEAK") for f in compare(fresh, full_sweep=True)),
        "a leak in a library with no declared entry fails",
        failures,
    )
    burned = _full_ledger_view()
    burned.pop(site)
    expect(
        any(f.startswith("STALE_LEDGER") for f in compare(burned, full_sweep=True)),
        "a burned-down symbol still in the ledger fails",
        failures,
    )
    expect(
        compare({site: {}}, full_sweep=False) == [],
        "a partial path scan does not judge ledger staleness",
        failures,
    )

    print(" floors")
    expect(len(DECLARED_SITES) >= LEDGER_SITE_FLOOR, "ledger holds its site floor", failures)
    expect(
        sum(len(v) for v in DECLARED_SITES.values()) >= LEDGER_SYMBOL_FLOOR,
        "ledger holds its symbol floor",
        failures,
    )


def _selftest() -> int:
    """Prove the detector and the ledger comparison both fire and stay quiet."""
    failures: list[str] = []
    print(f"{PROGRAM} --selftest")
    _selftest_detector(failures)
    _selftest_ledger(failures)
    return report(failures)


def main(argv: list[str]) -> int:
    """Sweep libs/ against the declared inventory, or run the selftest."""
    options = [arg for arg in argv[1:] if arg.startswith("--")]
    if options and len(argv) != OPTION_ARG_COUNT:
        print(f"{PROGRAM}: {options[0]} accepts no paths", file=sys.stderr)
        return 2
    if options and options[0] not in ("--selftest", "--print-ledger"):
        print(f"{PROGRAM}: unknown option {options[0]}", file=sys.stderr)
        return 2
    if options == ["--selftest"]:
        return _selftest()

    paths = [] if options else argv[1:]
    targets = enumerate_targets(paths)
    full_sweep = not paths
    if full_sweep and len(targets) < FILE_FLOOR:
        print(
            f"{PROGRAM}: FATAL -- only {len(targets)} library file(s) in scope, "
            f"floor is {FILE_FLOOR}. A collapsed sweep is not clean.",
            file=sys.stderr,
        )
        return 2
    if not targets:
        print(f"{PROGRAM}: no library files to scan")
        return 0

    observed = observe(targets)
    if options == ["--print-ledger"]:
        return _print_ledger(observed)

    declared_symbols = sum(len(v) for v in DECLARED_SITES.values())
    if full_sweep and (
        len(DECLARED_SITES) < LEDGER_SITE_FLOOR or declared_symbols < LEDGER_SYMBOL_FLOOR
    ):
        print(
            f"{PROGRAM}: FATAL -- ledger holds {len(DECLARED_SITES)} site(s) and "
            f"{declared_symbols} symbol(s), floors are {LEDGER_SITE_FLOOR} and "
            f"{LEDGER_SYMBOL_FLOOR}. Shrink it by burning leaks down, not by editing it.",
            file=sys.stderr,
        )
        return 2

    findings = compare(observed, full_sweep=full_sweep)
    if findings:
        print(f"{PROGRAM}: the tree and the RTOS-symbol ledger disagree:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        print(
            "\nNEW_LEAK: reach the middleware through a seam bound under port/, or, while "
            "#695 is still design-only, declare the site in DECLARED_SITES with the reason "
            "it cannot wait.\nSTALE_LEDGER: the leak is gone -- drop its entry in the same "
            "change, so the ledger keeps measuring the real distance to the #695 invariant.",
            file=sys.stderr,
        )
        return 1

    leaks = sum(len(v) for v in observed.values())
    print(
        f"{PROGRAM}: {len(targets)} library file(s) scanned; {leaks} declared RTOS/middleware "
        f"symbol(s) across {len(observed)} file(s), no undeclared leak and no stale entry."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
