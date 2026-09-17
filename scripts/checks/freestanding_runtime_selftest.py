#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction selftests for the freestanding runtime ratchet.

Exercises map/nm parsing, linker-script checks, baseline ratchet evaluation
for unknown, indebted, and zero-debt apps, archive allowlist policy, source
assert scanning, and RA8_FREESTANDING configuration. Imported lazily by
check_freestanding_runtime.selftest() so the detector module stays small.
"""

from __future__ import annotations

import json
import pathlib
import tempfile
from typing import Any

import check_freestanding_runtime as core


def _selftest_nm_and_map() -> list[str]:
    """Test nm parsing and map file parsing."""
    failures: list[str] = []
    mock_nm = """
0200109e T memset
0200e50c T malloc
0200e51c T free
0200e4d0 T __assert_func
02001234 T _sbrk
220801c8 B end
02003000 T __aeabi_uldivmod
"""
    syms = core.parse_nm_symbols(mock_nm)
    if "memset" not in syms or "malloc" not in syms or "__aeabi_uldivmod" not in syms:
        failures.append("nm symbol parser missed symbols")

    mock_map = """
Archive member included to satisfy reference by file (symbol)

.../libg_nano.a(libc_a-memcmp.o)
.../libg_nano.a(libc_a-memset.o)
.../libnosys.a(close.o)

Discarded input sections

 .text          0x00000000       0x60 .../libg_nano.a(libc_a-memcmp.o)
 .text._sbrk    0x00000000       0x14 CMakeFiles/blink.elf.dir/.../ra8_sbrk_trap.c.obj

Memory Map

LOAD .../libnosys.a

.text           0x02000000     0x1000
 .text.memset   0x0200109e       0x10 .../libg_nano.a(libc_a-memset.o)
                0x0200109e                memset
 .text._sbrk    0x02001234       0x14 CMakeFiles/app.elf.dir/.../ra8_sbrk_trap.c.obj
                0x02001234                _sbrk
"""
    map_res = core.parse_map_file(mock_map)
    if "libc_a-memcmp.o" in map_res["live_archive_members"].get("libg_nano.a", set()):
        failures.append("discarded archive member classified as live")
    if "libc_a-memset.o" not in map_res["live_archive_members"].get("libg_nano.a", set()):
        failures.append("live archive member not detected")
    if "close.o" in map_res["live_archive_members"].get("libnosys.a", set()):
        failures.append("LOAD-only archive member classified as live")
    if "ra8_sbrk_trap" not in map_res["symbol_providers"].get("_sbrk", ""):
        failures.append(f"_sbrk provider incorrect: {map_res['symbol_providers'].get('_sbrk')}")

    mock_bad_map = """
Memory Map
.text           0x02000000     0x1000
 .text._sbrk    0x02001234       0x20 .../libnosys.a(sbrk.o)
                0x02001234                _sbrk
"""
    bad_map_res = core.parse_map_file(mock_bad_map)
    if "sbrk.o" not in bad_map_res["symbol_providers"].get("_sbrk", ""):
        failures.append("libnosys sbrk provider was not flagged")

    return failures


def _selftest_linker_scripts() -> list[str]:
    """Test linker script verification."""
    failures: list[str] = []
    clean_ld = "SECTIONS { .text : { *(.text*) } > SRAM }"
    bad_ld_end = "SECTIONS { PROVIDE(end = .); }"
    bad_ld_heap = "SECTIONS { .heap : { *(._heap*) } > SRAM }"

    if core.check_linker_script(clean_ld, "clean.ld"):
        failures.append("clean linker script flagged")
    if not core.check_linker_script(bad_ld_end, "bad_end.ld"):
        failures.append("linker script defining 'end' not flagged")
    if not core.check_linker_script(bad_ld_heap, "bad_heap.ld"):
        failures.append("linker script defining '.heap' not flagged")
    return failures


def _selftest_new_app_policy(
    mock_baseline: dict[str, Any], clean_analysis: dict[str, Any]
) -> list[str]:
    """Test strict symbol and forbidden-archive policy for unknown apps."""
    failures: list[str] = []
    if core.evaluate_against_baseline("new_app", clean_analysis, mock_baseline):
        failures.append("clean app with libgcc helpers flagged")

    bad_sym = dict(clean_analysis, live_forbidden_symbols=["malloc"])
    if not core.evaluate_against_baseline("new_app", bad_sym, mock_baseline):
        failures.append("new forbidden malloc not flagged")

    bad_prim_src = dict(
        clean_analysis,
        runtime_primitive_providers={"memset": "libg_nano.a(libc_a-memset.o)"},
    )
    if not core.evaluate_against_baseline("new_app", bad_prim_src, mock_baseline):
        failures.append("forbidden runtime primitive provider not flagged")

    bad_nosys = dict(clean_analysis, live_archive_members={"libnosys.a": ["close.o"]})
    if not core.evaluate_against_baseline("new_app", bad_nosys, mock_baseline):
        failures.append("live libnosys member not flagged")

    bad_nano = dict(clean_analysis, live_archive_members={"libg_nano.a": ["libc_a-memset.o"]})
    if not core.evaluate_against_baseline("new_app", bad_nano, mock_baseline):
        failures.append("live newlib-nano member not flagged")

    bad_posix_sym = dict(clean_analysis, posix_symbols=["ra8_io_stream_posix_bind"])
    if not core.evaluate_against_baseline("new_app", bad_posix_sym, mock_baseline):
        failures.append("forbidden host-only POSIX symbol not flagged")

    bad_posix_obj = dict(clean_analysis, posix_providers={"io": "ra8_io_stream_posix.c.obj"})
    if not core.evaluate_against_baseline("new_app", bad_posix_obj, mock_baseline):
        failures.append("forbidden host-only POSIX provider not flagged")
    failures.extend(_selftest_new_app_archives(mock_baseline, clean_analysis))
    return failures


def _selftest_new_app_archives(
    mock_baseline: dict[str, Any], clean_analysis: dict[str, Any]
) -> list[str]:
    """Test allowlist policy for compiler and unknown archives."""
    failures: list[str] = []
    libm_ok = dict(
        clean_analysis,
        live_archive_members={"libgcc.a": ["_aeabi_uldivmod.o"], "libm.a": ["libm_a-wf_acos.o"]},
        system_archives=["libgcc.a", "libm.a"],
    )
    if core.evaluate_against_baseline("new_app", libm_ok, mock_baseline):
        failures.append("approved libm member unexpectedly flagged")

    libm_unapproved = dict(
        clean_analysis,
        live_archive_members={"libm.a": ["libm_a-unapproved.o"]},
        system_archives=["libm.a"],
    )
    if not core.evaluate_against_baseline("new_app", libm_unapproved, mock_baseline):
        failures.append("unapproved live libm member not flagged")

    arbitrary_arch = dict(
        clean_analysis, live_archive_members={"libfoo.a": ["foo.o"]}, system_archives=["libfoo.a"]
    )
    if not core.evaluate_against_baseline("new_app", arbitrary_arch, mock_baseline):
        failures.append("arbitrary unknown live archive not flagged")

    libm_discarded = dict(
        clean_analysis, discarded_archive_members={"libm.a": ["libm_a-unapproved.o"]}
    )
    if core.evaluate_against_baseline("new_app", libm_discarded, mock_baseline):
        failures.append("discarded libm member unexpectedly flagged")

    libm_forbidden_sym = dict(libm_ok, live_forbidden_symbols=["malloc"])
    if not core.evaluate_against_baseline("new_app", libm_forbidden_sym, mock_baseline):
        failures.append("approved libm member that introduces forbidden symbol not flagged")
    return failures


def _selftest_live_archive_fail_closed(
    mock_baseline: dict[str, Any], clean_analysis: dict[str, Any]
) -> list[str]:
    """Unknown live archives fail closed with or without system classification."""
    failures: list[str] = []
    unknown_live = dict(clean_analysis, live_archive_members={"libevil.a": ["evil.o"]})
    if not core.evaluate_against_baseline("new_app", unknown_live, mock_baseline):
        failures.append("unknown live archive without system tag not flagged")

    unknown_sys = dict(unknown_live, system_archives=["libevil.a"])
    if not core.evaluate_against_baseline("new_app", unknown_sys, mock_baseline):
        failures.append("unknown live archive with system tag not flagged")

    project_live = dict(clean_analysis, live_archive_members={"libthreadx.a": ["tx_x.o"]})
    if core.evaluate_against_baseline("new_app", project_live, mock_baseline):
        failures.append("approved project archive unexpectedly flagged")

    project_prim = dict(
        clean_analysis,
        runtime_primitive_providers={"memset": "libra8_shared_ek_ra8d2.a(m.o)"},
    )
    if core.evaluate_against_baseline("new_app", project_prim, mock_baseline):
        failures.append("primitive from approved project archive unexpectedly flagged")

    unknown_dead = dict(clean_analysis, discarded_archive_members={"libevil.a": ["evil.o"]})
    if core.evaluate_against_baseline("new_app", unknown_dead, mock_baseline):
        failures.append("discarded unknown archive unexpectedly flagged")

    heavy_unknown = dict(
        clean_analysis,
        live_archive_members={"libevil.a": ["evil.o"]},
        sbrk_present=True,
        sbrk_provider="ra8_sbrk_trap.c.obj",
    )
    if not core.evaluate_against_baseline("heavy_app", heavy_unknown, mock_baseline):
        failures.append("debt-baselined app with unknown live archive not flagged")
    return failures


def _selftest_debt_ratchet(mock_baseline: dict[str, Any]) -> list[str]:
    """Test that recorded debt ratchets without authorizing new archives."""
    failures: list[str] = []
    heavy_ok = {
        "live_forbidden_symbols": ["malloc", "free"],
        "live_archive_members": {"libnosys.a": ["close.o"]},
        "sbrk_present": True,
        "sbrk_provider": "ra8_sbrk_trap.c.obj",
        "end_symbol_present": False,
        "has_heap_section": False,
    }
    if core.evaluate_against_baseline("heavy_app", heavy_ok, mock_baseline):
        failures.append("heavy app within baseline flagged")

    heavy_realloc = dict(heavy_ok, live_forbidden_symbols=["malloc", "free", "realloc"])
    if not core.evaluate_against_baseline("heavy_app", heavy_realloc, mock_baseline):
        failures.append("debt expansion not flagged")

    # Recorded debt never authorizes an unapproved archive: the allowlist is
    # absolute on the ratchet path too.
    heavy_libm = dict(
        heavy_ok,
        live_archive_members={"libnosys.a": ["close.o"], "libm.a": ["libm_a-unapproved.o"]},
        system_archives=["libm.a"],
    )
    if not core.evaluate_against_baseline("heavy_app", heavy_libm, mock_baseline):
        failures.append("debt-baselined app with unapproved libm member not flagged")

    heavy_unknown = dict(
        heavy_ok,
        live_archive_members={"libfoo.a": ["foo.o"]},
        system_archives=["libfoo.a"],
    )
    if not core.evaluate_against_baseline("heavy_app", heavy_unknown, mock_baseline):
        failures.append("debt-baselined app with unknown live archive not flagged")
    return failures


def _selftest_zero_debt(clean_analysis: dict[str, Any]) -> list[str]:
    """Test that zero-debt baselines enforce strict invariants."""
    failures: list[str] = []
    zero_debt_baseline = {
        "apps": {
            "clean_app": {
                "forbidden_symbols": [],
                "forbidden_archives": {},
                "sbrk_provider": "none",
                "end_symbol": False,
            }
        }
    }

    if core.evaluate_against_baseline("clean_app", clean_analysis, zero_debt_baseline):
        failures.append("clean zero-debt baselined app unexpectedly flagged")

    bad_zero_prim = dict(
        clean_analysis,
        runtime_primitive_providers={"memset": "libg_nano.a(libc_a-memset.o)"},
    )
    bad_zero_sym = dict(clean_analysis, live_forbidden_symbols=["malloc"])
    if not core.evaluate_against_baseline("clean_app", bad_zero_sym, zero_debt_baseline):
        failures.append("zero-debt baselined app with forbidden symbol not flagged")

    bad_zero_arch = dict(clean_analysis, live_archive_members={"libnosys.a": ["close.o"]})
    if not core.evaluate_against_baseline("clean_app", bad_zero_arch, zero_debt_baseline):
        failures.append("zero-debt baselined app with forbidden archive member not flagged")

    bad_zero_end = dict(clean_analysis, end_symbol_present=True)
    if not core.evaluate_against_baseline("clean_app", bad_zero_end, zero_debt_baseline):
        failures.append("zero-debt baselined app with end symbol not flagged")

    bad_zero_heap = dict(clean_analysis, has_heap_section=True)
    if not core.evaluate_against_baseline("clean_app", bad_zero_heap, zero_debt_baseline):
        failures.append("zero-debt baselined app with .heap section not flagged")

    if not core.evaluate_against_baseline("clean_app", bad_zero_prim, zero_debt_baseline):
        failures.append(
            "zero-debt baselined app with runtime primitive from libg_nano.a not flagged"
        )

    bad_zero_sbrk = dict(
        clean_analysis,
        sbrk_present=True,
        sbrk_provider="ra8_sbrk_trap.c.obj",
    )
    if not core.evaluate_against_baseline("clean_app", bad_zero_sbrk, zero_debt_baseline):
        failures.append("zero-debt baselined app with newly-live _sbrk not flagged")

    bad_zero_posix = dict(clean_analysis, posix_symbols=["ra8_io_stream_posix_bind"])
    if not core.evaluate_against_baseline("clean_app", bad_zero_posix, zero_debt_baseline):
        failures.append("zero-debt baselined app with POSIX symbol not flagged")

    bad_zero_posix_obj = dict(clean_analysis, posix_providers={"io": "ra8_io_stream_posix.c.obj"})
    if not core.evaluate_against_baseline("clean_app", bad_zero_posix_obj, zero_debt_baseline):
        failures.append("zero-debt baselined app with POSIX provider not flagged")

    return failures


def _selftest_ratchet_eval() -> list[str]:
    """Test baseline ratchet evaluation."""
    mock_baseline = {
        "apps": {
            "heavy_app": {
                "forbidden_symbols": ["malloc", "free"],
                "forbidden_archives": {"libnosys.a": ["close.o"]},
                "sbrk_provider": "ra8_sbrk_trap.c.obj",
                "end_symbol": False,
            }
        }
    }

    clean_analysis = {
        "live_forbidden_symbols": [],
        "live_archive_members": {"libgcc.a": ["_aeabi_uldivmod.o"]},
        "system_archives": ["libgcc.a"],
        "runtime_primitive_providers": {"memset": "ra8_freestanding_mem.c.obj"},
        "sbrk_present": False,
        "sbrk_provider": "none",
        "end_symbol_present": False,
        "has_heap_section": False,
    }
    return (
        _selftest_new_app_policy(mock_baseline, clean_analysis)
        + _selftest_live_archive_fail_closed(mock_baseline, clean_analysis)
        + _selftest_debt_ratchet(mock_baseline)
        + _selftest_zero_debt(clean_analysis)
    )


def _selftest_scripts_and_ratchet() -> list[str]:
    """Test linker script verification and baseline ratchet evaluation."""
    return _selftest_linker_scripts() + _selftest_ratchet_eval()


def _selftest_source_asserts(repo_root: pathlib.Path) -> list[str]:
    """Test source assert scanner."""
    failures: list[str] = []
    clean_src = 'void f(void) { static_assert(1, "ok"); RA8_ASSERT(x > 0, "msg"); }'
    bad_inc = "#include <assert.h>\nvoid f(void) {}"
    bad_call = "void f(int x) { assert(x > 0); }"

    if core.check_source_asserts(clean_src, "clean.c"):
        failures.append("clean source with static_assert/RA8_ASSERT flagged")
    if not core.check_source_asserts(bad_inc, "bad_inc.c"):
        failures.append("<assert.h> include not flagged")
    if not core.check_source_asserts(bad_call, "bad_call.c"):
        failures.append("runtime assert() call not flagged")

    # Target esp-hosted assert must fail
    esp_hosted_bad = "void f(void) { assert(bus != NULL); }"
    if not core.check_source_asserts(esp_hosted_bad, "port/esp-hosted/src/ra8_esp_hosted_spi.c"):
        failures.append("target esp-hosted assert() not flagged")

    macro_redirect = (
        '#include "ra8_check.h"\n'
        "#ifndef assert\n"
        '#define assert(expr) RA8_ASSERT(expr, "msg")\n'
        "#endif\n"
    )
    if core.check_source_asserts(macro_redirect, "port/esp-hosted/inc/port_esp_hosted_host_os.h"):
        failures.append("macro redirect #define assert(...) incorrectly flagged")

    # POSIX fail-closed guard verification (repo-rooted so the check cannot
    # silently skip when run from another working directory).
    posix_header = repo_root / "port" / "posix" / "inc" / "ra8_io_stream_posix.h"
    if not posix_header.is_file():
        failures.append("port/posix/inc/ra8_io_stream_posix.h not found for guard check")
    else:
        text = posix_header.read_text(encoding="utf-8")
        if "#ifndef RA8_OFF_TARGET" not in text or "#error" not in text:
            failures.append("port/posix/inc/ra8_io_stream_posix.h missing fail-closed guard")

    return failures


def _selftest_provider_policy(
    mock_baseline: dict[str, Any], clean_analysis: dict[str, Any]
) -> list[str]:
    """Test primitive-provider and project-archive policy for unknown apps."""
    failures: list[str] = []
    project_arch = dict(
        clean_analysis,
        live_archive_members={"libthreadx.a": ["tx_thread_create.c.obj"]},
        system_archives=[],
    )
    if core.evaluate_against_baseline("new_app", project_arch, mock_baseline):
        failures.append("project-built archive unexpectedly flagged")

    prim_unknown = dict(
        clean_analysis,
        runtime_primitive_providers={"memcpy": "libfoo.a(foo.o)"},
    )
    if not core.evaluate_against_baseline("new_app", prim_unknown, mock_baseline):
        failures.append("primitive resolved to unknown archive not flagged")

    prim_libm_bad = dict(
        clean_analysis,
        live_archive_members={"libm.a": ["libm_a-unapproved.o"]},
        system_archives=["libm.a"],
        runtime_primitive_providers={"strlen": "libm.a(libm_a-unapproved.o)"},
    )
    if not core.evaluate_against_baseline("new_app", prim_libm_bad, mock_baseline):
        failures.append("primitive resolved to unapproved libm member not flagged")

    prim_libm_ok = dict(
        clean_analysis,
        live_archive_members={"libm.a": ["libm_a-wf_acos.o"]},
        system_archives=["libm.a"],
        runtime_primitive_providers={"strlen": "ra8_freestanding_str.c.obj"},
    )
    if core.evaluate_against_baseline("new_app", prim_libm_ok, mock_baseline):
        failures.append("project-resolved primitive with live libm unexpectedly flagged")

    prim_pathed = dict(
        clean_analysis,
        runtime_primitive_providers={"memset": ".../libgcc.a(_arm_muldf3.o)"},
    )
    if core.evaluate_against_baseline("new_app", prim_pathed, mock_baseline):
        failures.append("path-form allowed archive provider unexpectedly flagged")
    return failures


def _selftest_db_flags() -> list[str]:
    """Test RA8_FREESTANDING compile-database enforcement both directions."""
    failures: list[str] = []
    flagged = [{"file": "a.c", "command": "cc -DRA8_FREESTANDING -c a.c"}]
    if core.check_db_entries(flagged, must_have=True, exempt=None, label="Target"):
        failures.append("fully flagged target database unexpectedly flagged")
    if not core.check_target_db_flags(pathlib.Path("nonexistent-db.json")):
        failures.append("missing target database path not flagged")
    missing_flag = [*flagged, {"file": "b.c", "command": "cc -O2 -c b.c"}]
    if not core.check_db_entries(missing_flag, must_have=True, exempt=None, label="Target"):
        failures.append("target database entry missing the flag not flagged")

    stray_flag = [{"file": "h.c", "command": "cc -DRA8_FREESTANDING -c h.c"}]
    if not core.check_db_entries(
        stray_flag, must_have=False, exempt="-DRA8_TEST_FREESTANDING", label="Host"
    ):
        failures.append("host database entry setting the flag not flagged")

    exempt_flag = [
        {"file": "t.c", "command": "cc -DRA8_FREESTANDING -DRA8_TEST_FREESTANDING -c t.c"}
    ]
    if core.check_db_entries(
        exempt_flag, must_have=False, exempt="-DRA8_TEST_FREESTANDING", label="Host"
    ):
        failures.append("exempt freestanding-test host entry unexpectedly flagged")

    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        db_dir = root / "examples" / "demo" / "build"
        db_dir.mkdir(parents=True)
        db_path = db_dir / "compile_commands.json"
        db_path.write_text(json.dumps(flagged), encoding="utf-8")
        if core.discover_target_dbs(root) != [db_path]:
            failures.append("per-app target database not discovered")
        if core.check_discovered_target_dbs(root):
            failures.append("flagged per-app target database unexpectedly flagged")
        db_path.write_text(json.dumps(missing_flag), encoding="utf-8")
        if not core.check_discovered_target_dbs(root):
            failures.append("unflagged per-app target database not flagged")
        db_path.write_text("[]", encoding="utf-8")
        if not core.check_discovered_target_dbs(root):
            failures.append("empty per-app target database not flagged")
        if not core.check_target_db_flags(root / "examples" / "demo" / "none.json"):
            failures.append("missing target database not flagged")
    return failures


def run_selftests(repo_root: pathlib.Path) -> list[str]:
    """Run every freestanding-runtime selftest; return failure strings."""
    failures = _selftest_nm_and_map() + _selftest_scripts_and_ratchet()
    failures.extend(_selftest_source_asserts(repo_root))
    failures.extend(_selftest_db_flags())
    mock_baseline: dict[str, Any] = {"apps": {}}
    clean_analysis: dict[str, Any] = {
        "live_forbidden_symbols": [],
        "live_archive_members": {"libgcc.a": ["_aeabi_uldivmod.o"]},
        "system_archives": ["libgcc.a"],
        "runtime_primitive_providers": {"memset": "ra8_freestanding_mem.c.obj"},
        "sbrk_present": False,
        "sbrk_provider": "none",
        "end_symbol_present": False,
        "has_heap_section": False,
    }
    failures.extend(_selftest_provider_policy(mock_baseline, clean_analysis))
    return failures
