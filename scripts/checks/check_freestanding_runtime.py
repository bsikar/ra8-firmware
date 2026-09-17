#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Freestanding target runtime binary dependency and linker script ratchet.

Enforces that target firmware images contain no implicit libc/newlib dependencies,
no general-purpose heap, no unapproved dynamic allocation, and that linker
scripts do not define heap anchors ('end', '_end') or '.heap' sections.

Differentiates between:
  - live symbols vs discarded symbols
  - live archive members vs discarded sections vs LOAD-only archives
  - project sbrk trap vs libnosys bump allocator
  - allowed libgcc compiler helpers vs forbidden libc symbols
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
from typing import Any

import freestanding_runtime_selftest as fr_selftest

_nm_min_fields = 2
_map_input_section_fields = 4
_map_symbol_fields = 2


def _repo_root() -> pathlib.Path:
    """Return repository root path."""
    return pathlib.Path(__file__).resolve().parents[2]


def _forbidden_symbols() -> set[str]:
    """Return the set of forbidden libc/allocator/stdio runtime symbols."""
    return {
        "malloc",
        "calloc",
        "realloc",
        "free",
        "aligned_alloc",
        "valloc",
        "pvalloc",
        "posix_memalign",
        "reallocarray",
        "strdup",
        "strndup",
        "asprintf",
        "vasprintf",
        "_malloc_r",
        "_calloc_r",
        "_realloc_r",
        "_free_r",
        "_sbrk_r",
        "_malloc_usable_size_r",
        "__assert_func",
        "printf",
        "fprintf",
        "fiprintf",
        "vfprintf",
        "_vfprintf_r",
        "_vfiprintf_r",
        "_printf_i",
        "_printf_common",
        "puts",
        "fputs",
    }


def _forbidden_archives() -> set[str]:
    """Return the set of forbidden standard library archives."""
    return {"libnosys.a", "libg_nano.a", "libc_nano.a", "libc.a"}


def _allowed_compiler_archives() -> set[str]:
    """Return compiler support archives that are explicitly allowed."""
    return {"libgcc.a", "libm.a"}


def _allowed_project_archives() -> set[str]:
    """First-party build-product archives allowed to contribute live members."""
    return {
        "libthreadx.a",
        "libthreadx_ns.a",
        "libra8_shared_ek_ra8d2.a",
        "libtfpsa_arm.a",
        "libtfpsa_dfu.a",
        "libtfpsa_rot.a",
        "libtfpsa_sb.a",
        "libtfpsa_sbns.a",
    }


def _allowed_libm_members() -> set[str]:
    """Return explicitly approved libm.a members (transcendental functions without malloc/stdio)."""
    return {
        "libm_a-wf_acos.o",
        "libm_a-wf_atan2.o",
        "libm_a-wf_fmod.o",
        "libm_a-wf_pow.o",
        "libm_a-wf_sqrt.o",
        "libm_a-sf_finite.o",
        "libm_a-sf_cos.o",
        "libm_a-sf_fabs.o",
        "libm_a-sf_nan.o",
        "libm_a-sf_sin.o",
        "libm_a-sf_tan.o",
        "libm_a-kf_cos.o",
        "libm_a-kf_sin.o",
        "libm_a-kf_tan.o",
        "libm_a-ef_acos.o",
        "libm_a-ef_atan2.o",
        "libm_a-ef_sqrt.o",
        "libm_a-ef_fmod.o",
        "libm_a-ef_pow.o",
        "libm_a-sf_ceil.o",
        "libm_a-sf_floor.o",
        "libm_a-ef_rem_pio2.o",
        "libm_a-math_errf.o",
        "libm_a-sf_scalbn.o",
        "libm_a-sf_atan.o",
        "libm_a-kf_rem_pio2.o",
    }


def _reviewed_runtime_primitives() -> set[str]:
    """Explicit, reviewed allowlist for project-owned freestanding ABI primitives."""
    return {
        "memset",
        "memcpy",
        "memmove",
        "memcmp",
        "memchr",
        "strlen",
        "strnlen",
        "strcmp",
        "strncmp",
        "strchr",
        "strrchr",
        "strstr",
        "strcpy",
        "strncpy",
        "abs",
    }


def find_tool(tool_name: str) -> str | None:
    """Locate tool binary in RA8_ARM_TOOLCHAIN_BIN, PATH, or standard toolchain paths."""
    env_bin = os.environ.get("RA8_ARM_TOOLCHAIN_BIN")
    if env_bin:
        p = pathlib.Path(env_bin) / tool_name
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)

    path_dirs = os.environ.get("PATH", "").split(os.pathsep)
    for d in path_dirs:
        if d:
            p = pathlib.Path(d) / tool_name
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)

    home_opt = str(pathlib.Path.home() / "opt" / "arm-gnu-toolchain-13.3" / "bin")
    known_paths = [
        home_opt,
        "/opt/arm-gnu-toolchain/bin",
        "/opt/toolchains/arm-gnu-toolchain-13.3/bin",
        "/usr/local/bin",
        "/usr/bin",
    ]
    for d in known_paths:
        p = pathlib.Path(d) / tool_name
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)

    return None


def parse_nm_symbols(nm_output: str) -> dict[str, str]:
    """Parse nm output into {symbol_name: symbol_type}."""
    symbols: dict[str, str] = {}
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) > _nm_min_fields:
            sym_type, sym_name = parts[1], parts[2]
            symbols[sym_name] = sym_type
        elif len(parts) == _nm_min_fields:
            sym_type, sym_name = parts[0], parts[1]
            symbols[sym_name] = sym_type
    return symbols


def _parse_map_archive_member(
    obj_path: str,
    arch_member_re: re.Pattern[str],
) -> tuple[str, str, bool] | None:
    """Extract archive name, member name, and whether it's a system archive."""
    m = arch_member_re.search(obj_path)
    if not m:
        return None
    arch_path, member = m.group(1), m.group(2)
    arch_path_fwd = arch_path.replace("\\", "/")
    arch_name = arch_path_fwd.split("/")[-1]
    # Heuristic for system toolchain archives: absolute paths containing 'arm-none-eabi' or 'gcc'
    is_system = "/" in arch_path_fwd and (
        "arm-none-eabi" in arch_path_fwd or "gcc" in arch_path_fwd
    )
    return arch_name, member, is_system


def _parse_map_discarded(
    discarded_text: str,
    arch_member_re: re.Pattern[str],
) -> dict[str, set[str]]:
    """Collect archive members that appear only under Discarded input sections."""
    discarded: dict[str, set[str]] = {}
    for line in discarded_text.splitlines():
        res = _parse_map_archive_member(line, arch_member_re)
        if res:
            arch_name, member, _ = res
            discarded.setdefault(arch_name, set()).add(member)
    return discarded


def _parse_map_memory_map(
    memory_map_text: str,
    arch_member_re: re.Pattern[str],
) -> tuple[dict[str, set[str]], dict[str, str], set[str]]:
    """Parse the memory map for live members, providers, and system archives."""
    live_members: dict[str, set[str]] = {}
    providers: dict[str, str] = {}
    system_archives: set[str] = set()
    lines = memory_map_text.splitlines()

    for i, line in enumerate(lines):
        parts = line.split()
        if (
            len(parts) >= _map_input_section_fields
            and parts[0].startswith(".")
            and parts[1].startswith("0x")
            and parts[2].startswith("0x")
        ):
            try:
                size = int(parts[2], 16)
            except ValueError:
                size = 0
            if size > 0:
                res = _parse_map_archive_member(parts[3], arch_member_re)
                if res:
                    arch_name, member, is_system = res
                    live_members.setdefault(arch_name, set()).add(member)
                    if is_system:
                        system_archives.add(arch_name)

        if (
            len(parts) == _map_symbol_fields
            and parts[0].startswith("0x")
            and not parts[1].startswith("0x")
        ):
            sym = parts[1]
            if i > 0:
                prev = lines[i - 1]
                res = _parse_map_archive_member(prev, arch_member_re)
                if res:
                    providers[sym] = f"{res[0]}({res[1]})"
                elif ".o" in prev:
                    obj_m = re.search(r"([^\s()]+\.o(?:bj)?)", prev)
                    if obj_m:
                        providers[sym] = obj_m.group(1).replace("\\", "/").split("/")[-1]

    return live_members, providers, system_archives


def parse_map_file(map_content: str) -> dict[str, Any]:
    """Parse GNU ld map file to extract live archive members and symbol providers."""
    result: dict[str, Any] = {
        "live_archive_members": {},
        "discarded_archive_members": {},
        "symbol_providers": {},
        "system_archives": set(),
        "has_heap_section": False,
    }

    mm_pos = map_content.find("Linker script and memory map")
    if mm_pos == -1:
        mm_pos = map_content.find("Memory Map")

    memory_map_text = map_content[mm_pos:] if mm_pos != -1 else ""
    discarded_text = map_content[:mm_pos] if mm_pos != -1 else map_content

    heap_sec_re = re.compile(r"^\s*\.heap\s+0x[0-9a-fA-F]+\s+0x[1-9a-fA-F]", re.MULTILINE)
    if heap_sec_re.search(memory_map_text):
        result["has_heap_section"] = True

    arch_member_re = re.compile(r"([^\s()]+\.a)\(([^)]+\.o(?:bj)?)\)")
    result["discarded_archive_members"] = _parse_map_discarded(discarded_text, arch_member_re)

    live_members, providers, system_archives = _parse_map_memory_map(
        memory_map_text, arch_member_re
    )
    result["live_archive_members"] = live_members
    result["symbol_providers"] = providers
    result["system_archives"] = system_archives
    return result


def check_linker_script(content: str, path: str) -> list[str]:
    """Check a linker script for prohibited 'end'/'_end' definitions and '.heap' sections."""
    comment_re = re.compile(r"/\*.*?\*/", re.DOTALL)
    stripped = comment_re.sub(" ", content)

    violations = []
    end_sym_re = re.compile(r"\b(PROVIDE\s*\(\s*)?(_?end)\s*=", re.MULTILINE)
    violations.extend(
        f"{path}: defines forbidden heap anchor '{m.group(2)}'"
        for m in end_sym_re.finditer(stripped)
    )

    heap_sec_re = re.compile(r"(?<![a-zA-Z0-9_])\.heap\b")
    if heap_sec_re.search(stripped):
        violations.append(f"{path}: defines forbidden '.heap' output section")

    return violations


def check_all_linker_scripts(repo_root: pathlib.Path, baseline: dict[str, Any]) -> list[str]:
    """Check all target linker scripts in the repository."""
    violations: list[str] = []
    allowed_script_exceptions = set(baseline.get("linker_script_exceptions", []))

    roots = [
        repo_root / "libs",
        repo_root / "examples",
        repo_root / "apps",
    ]

    for root in roots:
        for ld_path in root.glob("**/*.ld"):
            rel_path = str(ld_path.relative_to(repo_root)).replace("\\", "/")
            if "third_party" in rel_path:
                continue

            content = ld_path.read_text(encoding="utf-8", errors="replace")
            script_violations = check_linker_script(content, rel_path)
            for v in script_violations:
                if rel_path in allowed_script_exceptions:
                    continue
                violations.append(v)

    return violations


def check_source_asserts(content: str, path: str) -> list[str]:
    """Check a source file for forbidden <assert.h> and runtime assert()."""
    comment_re = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
    stripped = comment_re.sub(" ", content)
    str_re = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    stripped = str_re.sub(" ", stripped)
    # Ignore preprocessor macro definitions redirecting assert, e.g. #define assert(...)
    macro_def_re = re.compile(r"#\s*define\s+assert\b[^\n]*")
    stripped = macro_def_re.sub(" ", stripped)

    violations = []
    if re.search(r"#\s*include\s+<assert\.h>", content):
        violations.append(f"{path}: includes forbidden libc <assert.h>")

    assert_call_re = re.compile(r"(?<![a-zA-Z0-9_])assert\s*\(")
    for m in assert_call_re.finditer(stripped):
        line_num = content[: m.start()].count("\n") + 1
        violations.append(
            f"{path}:{line_num}: uses forbidden standard assert() "
            "(use RA8_ASSERT for runtime invariants or static_assert for compile-time)"
        )
    return violations


def check_all_source_asserts(repo_root: pathlib.Path) -> list[str]:
    """Check all target-linkable first-party source files for forbidden asserts."""
    violations: list[str] = []
    roots = [
        repo_root / "libs",
        repo_root / "examples",
        repo_root / "apps",
        repo_root / "port",
    ]
    # port/posix is host-only and legitimately uses host facilities.
    # port/esp-hosted is target-linkable and audited (no blanket exemption).
    exempt_fragments = ("third_party", "tests", "apps/host", "port/posix", ".pb-c.")

    for root in roots:
        for p in root.glob("**/*"):
            if not p.is_file() or p.suffix not in (".c", ".h"):
                continue
            rel_path = str(p.relative_to(repo_root)).replace("\\", "/")
            if any(frag in rel_path for frag in exempt_fragments):
                continue
            content = p.read_text(encoding="utf-8", errors="replace")
            violations.extend(check_source_asserts(content, rel_path))

    return violations


def _run_readelf_heap_check(readelf_bin: str | None, elf_path: pathlib.Path) -> bool:
    """Return True if readelf detects a .heap section."""
    if not readelf_bin:
        return False
    r_proc = subprocess.run(  # noqa: S603  # trusted tool binary
        [readelf_bin, "-S", str(elf_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if r_proc.returncode != 0:
        return False
    return any(re.search(r"\[\s*\d+\]\s+\.heap\b", line) for line in r_proc.stdout.splitlines())


def _read_symbols_and_heap(
    elf_path: pathlib.Path, nm_bin: str, readelf_bin: str | None
) -> tuple[dict[str, str], bool]:
    """Run nm and readelf to extract defined symbols and check for .heap section."""
    proc = subprocess.run(  # noqa: S603  # trusted tool binary
        [nm_bin, "-n", str(elf_path)],
        capture_output=True,
        text=True,
        check=True,
    )
    symbols = parse_nm_symbols(proc.stdout)
    has_heap = _run_readelf_heap_check(readelf_bin, elf_path)
    return symbols, has_heap


def _load_map_info(map_path: pathlib.Path | None, has_heap: bool) -> tuple[dict[str, Any], bool]:
    """Parse map file if present, updating heap flag."""
    map_info: dict[str, Any] = {
        "live_archive_members": {},
        "discarded_archive_members": {},
        "symbol_providers": {},
        "has_heap_section": False,
    }
    if map_path and map_path.is_file():
        map_content = map_path.read_text(encoding="utf-8", errors="replace")
        map_info = parse_map_file(map_content)
        if map_info["has_heap_section"]:
            has_heap = True
    return map_info, has_heap


def analyze_image(
    elf_path: pathlib.Path,
    map_path: pathlib.Path | None = None,
    nm_binary: str | None = None,
    readelf_binary: str | None = None,
) -> dict[str, Any]:
    """Analyze a single target ELF and its map file."""
    if map_path is None:
        candidate_map = elf_path.with_suffix(".map")
        if candidate_map.is_file():
            map_path = candidate_map

    nm_bin = nm_binary or find_tool("arm-none-eabi-nm")
    readelf_bin = readelf_binary or find_tool("arm-none-eabi-readelf")
    if not nm_bin:
        msg = "arm-none-eabi-nm tool not found"
        raise RuntimeError(msg)

    symbols, has_heap_section = _read_symbols_and_heap(elf_path, nm_bin, readelf_bin)
    map_info, has_heap_section = _load_map_info(map_path, has_heap_section)
    all_forbidden = _forbidden_symbols()
    live_forbidden_syms = sorted(s for s in symbols if s in all_forbidden)

    sbrk_present = "_sbrk" in symbols
    sbrk_provider = map_info["symbol_providers"].get("_sbrk", "unknown" if sbrk_present else "none")
    end_sym_present = ("end" in symbols) or ("_end" in symbols)
    reviewed_prims = _reviewed_runtime_primitives()
    prim_providers = {
        s: map_info["symbol_providers"].get(s, "project") for s in symbols if s in reviewed_prims
    }
    posix_prefixes = ("ra8_io_stream_posix", "fw_fs_posix", "fw_if_fs_posix")
    posix_symbols = sorted(s for s in symbols if any(s.startswith(p) for p in posix_prefixes))
    posix_providers = {
        s: prov for s, prov in map_info["symbol_providers"].items() if "posix" in prov.lower()
    }

    return {
        "elf": str(elf_path),
        "map": str(map_path) if map_path else None,
        "live_forbidden_symbols": live_forbidden_syms,
        "live_archive_members": {k: sorted(v) for k, v in map_info["live_archive_members"].items()},
        "system_archives": sorted(map_info.get("system_archives", set())),
        "discarded_archive_members": {
            k: sorted(v) for k, v in map_info["discarded_archive_members"].items()
        },
        "runtime_primitive_providers": prim_providers,
        "sbrk_present": sbrk_present,
        "sbrk_provider": sbrk_provider,
        "end_symbol_present": end_sym_present,
        "has_heap_section": has_heap_section,
        "posix_symbols": posix_symbols,
        "posix_providers": posix_providers,
    }


def _check_live_archive_policy(app_name: str, analysis: dict[str, Any]) -> list[str]:
    """Fail closed on live archives outside the explicit allowlists."""
    violations: list[str] = []
    allowed = _allowed_compiler_archives()
    project = _allowed_project_archives()
    live = analysis.get("live_archive_members", {})
    for arch in sorted(live):
        if arch in allowed:
            if arch == "libm.a":
                unapproved = [m for m in live[arch] if m not in _allowed_libm_members()]
                if unapproved:
                    violations.append(
                        f"{app_name}: contains unapproved libm.a members: {sorted(unapproved)}"
                    )
        elif arch in project or arch in _forbidden_archives():
            continue
        else:
            violations.append(f"{app_name}: contains unapproved live archive '{arch}'")
    return violations


def _split_archive_provider(provider: str) -> tuple[str, str] | None:
    """Split an 'archive(member)' provider into its (archive, member) pair."""
    match = re.match(r"^(.+\.a)\(([^)]+)\)$", provider)
    if match is None:
        return None
    arch = match.group(1).replace("\\", "/").split("/")[-1]
    return arch, match.group(2)


def _check_primitive_providers(app_name: str, analysis: dict[str, Any]) -> list[str]:
    """Reject runtime primitives resolved to unapproved archives."""
    violations: list[str] = []
    allowed = _allowed_compiler_archives() | _allowed_project_archives()
    for sym, provider in analysis.get("runtime_primitive_providers", {}).items():
        if any(arch in provider for arch in _forbidden_archives()):
            violations.append(
                f"{app_name}: runtime primitive '{sym}' resolved to forbidden '{provider}'"
            )
            continue
        split = _split_archive_provider(provider)
        if split is None:
            continue
        arch, member = split
        if arch not in allowed or (arch == "libm.a" and member not in _allowed_libm_members()):
            violations.append(
                f"{app_name}: runtime primitive '{sym}' resolved to unapproved '{provider}'"
            )
    return violations


def _check_posix_block(app_name: str, analysis: dict[str, Any]) -> list[str]:
    """Reject host-only POSIX symbols and objects in target images."""
    violations: list[str] = []
    if analysis.get("posix_symbols"):
        violations.append(
            f"{app_name}: contains forbidden host-only POSIX symbol(s): {analysis['posix_symbols']}"
        )
    if analysis.get("posix_providers"):
        providers = analysis["posix_providers"]
        violations.append(f"{app_name}: links host-only POSIX object(s): {providers}")
    return violations


def _check_sbrk_ratchet(
    app_name: str, analysis: dict[str, Any], app_debt: dict[str, Any]
) -> list[str]:
    """Reject unexpected live _sbrk against the recorded provider."""
    violations: list[str] = []
    expected_provider = app_debt.get("sbrk_provider", "none")
    if expected_provider == "none" and analysis["sbrk_present"]:
        violations.append(
            f"{app_name}: unexpected live _sbrk symbol (provider: '{analysis['sbrk_provider']}')"
        )
    elif (
        analysis["sbrk_present"]
        and expected_provider != "none"
        and expected_provider != analysis["sbrk_provider"]
    ):
        violations.append(
            f"{app_name}: _sbrk provider changed from '{expected_provider}' "
            f"to '{analysis['sbrk_provider']}'"
        )
    return violations


def _eval_strict_freestanding(
    app_name: str,
    analysis: dict[str, Any],
) -> list[str]:
    """Enforce strict freestanding invariants for images with zero allowable debt."""
    violations: list[str] = []
    live_syms = set(analysis["live_forbidden_symbols"])
    if live_syms:
        violations.append(f"{app_name}: contains forbidden runtime symbols: {sorted(live_syms)}")

    violations.extend(_check_live_archive_policy(app_name, analysis))

    live_archives = analysis["live_archive_members"]
    for arch in _forbidden_archives():
        members = live_archives.get(arch, [])
        if members:
            violations.append(f"{app_name}: contains forbidden live members from {arch}: {members}")

    violations.extend(_check_primitive_providers(app_name, analysis))

    if analysis["sbrk_present"]:
        violations.append(
            f"{app_name}: unexpected live _sbrk symbol (provider: '{analysis['sbrk_provider']}')"
        )

    if analysis["end_symbol_present"]:
        violations.append(f"{app_name}: defines heap anchor symbol 'end'/'_end'")

    violations.extend(_check_posix_block(app_name, analysis))
    return violations


def _eval_ratchet_baseline(
    app_name: str,
    analysis: dict[str, Any],
    app_debt: dict[str, Any],
) -> list[str]:
    """Enforce that an image does not exceed its recorded debt."""
    violations: list[str] = []
    live_syms = set(analysis["live_forbidden_symbols"])
    baseline_syms = set(app_debt.get("forbidden_symbols", []))
    new_syms = live_syms - baseline_syms
    if new_syms:
        violations.append(
            f"{app_name}: introduces new forbidden symbols beyond baseline: {sorted(new_syms)}"
        )

    # The compiler-archive allowlist is absolute, not ratcheted: recorded debt
    # never authorizes an unapproved system archive or libm member.
    violations.extend(_check_live_archive_policy(app_name, analysis))

    live_archives = analysis["live_archive_members"]
    baseline_archives = app_debt.get("forbidden_archives", {})
    for arch in _forbidden_archives():
        current_members = set(live_archives.get(arch, []))
        base_members = set(baseline_archives.get(arch, []))
        new_members = current_members - base_members
        if new_members:
            violations.append(
                f"{app_name}: introduces new live members from {arch} "
                f"beyond baseline: {sorted(new_members)}"
            )

    # The archive allowlist is absolute, not ratcheted: recorded debt never
    # authorizes an unapproved archive, member, or primitive provider.
    violations.extend(_check_primitive_providers(app_name, analysis))

    violations.extend(_check_sbrk_ratchet(app_name, analysis, app_debt))

    expected_end = app_debt.get("end_symbol", False)
    if analysis["end_symbol_present"] and not expected_end:
        violations.append(f"{app_name}: defines unexpected heap anchor 'end'/'_end'")

    violations.extend(_check_posix_block(app_name, analysis))

    return violations


def _is_zero_debt(app_debt: dict[str, Any]) -> bool:
    """Return True if an app baseline entry specifies zero allowable debt."""
    forbidden_syms = app_debt.get("forbidden_symbols", [])
    forbidden_archs = app_debt.get("forbidden_archives", {})
    has_arch_members = any(bool(members) for members in forbidden_archs.values())
    sbrk_provider = app_debt.get("sbrk_provider", "none")
    end_symbol = app_debt.get("end_symbol", False)
    return (
        not forbidden_syms
        and not has_arch_members
        and sbrk_provider in (None, "none")
        and not end_symbol
    )


def evaluate_against_baseline(
    app_name: str,
    analysis: dict[str, Any],
    baseline: dict[str, Any],
) -> list[str]:
    """Check whether the analyzed image violates the freestanding ratchet."""
    violations: list[str] = []
    if analysis.get("has_heap_section"):
        violations.append(f"{app_name}: contains forbidden '.heap' section")

    app_debt = baseline.get("apps", {}).get(app_name)
    if app_debt is None:
        # Unknown app: strictly enforce zero debt
        violations.extend(_eval_strict_freestanding(app_name, analysis))
    elif _is_zero_debt(app_debt):
        # Baselined app with zero recorded debt: strictly enforce zero debt
        violations.extend(_eval_strict_freestanding(app_name, analysis))
    else:
        violations.extend(_eval_ratchet_baseline(app_name, analysis, app_debt))

    return violations


def _check_ra8_freestanding_build_files(repo_root: pathlib.Path) -> list[str]:
    """Verify build-system files discriminate target builds with RA8_FREESTANDING."""
    failures: list[str] = []
    # Build-system declarations always exist, so this half of the check can
    # never pass vacuously: the toolchain must discriminate target builds
    # with RA8_FREESTANDING even when no compile database was generated.
    toolchain = repo_root / "cmake" / "toolchain-ra8d2.cmake"
    if not toolchain.is_file():
        failures.append("cmake/toolchain-ra8d2.cmake not found")
    elif "RA8_FREESTANDING" not in toolchain.read_text(encoding="utf-8"):
        failures.append("cmake/toolchain-ra8d2.cmake does not define RA8_FREESTANDING")

    add_app = repo_root / "cmake" / "ra8_add_app.cmake"
    if not add_app.is_file():
        failures.append("cmake/ra8_add_app.cmake not found")
    elif "RA8_FREESTANDING" not in add_app.read_text(encoding="utf-8"):
        failures.append("cmake/ra8_add_app.cmake does not define RA8_FREESTANDING")
    return failures


def check_db_entries(
    entries: list[dict[str, Any]],
    *,
    must_have: bool,
    exempt: str | None,
    label: str,
) -> list[str]:
    """Check one compile database for the RA8_FREESTANDING discriminator."""
    failures: list[str] = []
    for entry in entries:
        cmd = entry.get("command", " ".join(entry.get("arguments", [])))
        if exempt is not None and exempt in cmd:
            continue
        has_flag = "-DRA8_FREESTANDING" in cmd
        if must_have and not has_flag:
            rel_file = entry.get("file", "unknown")
            failures.append(f"{label} missing -DRA8_FREESTANDING: {rel_file}")
            break
        if not must_have and has_flag:
            rel_file = entry.get("file", "unknown")
            failures.append(f"{label} sets -DRA8_FREESTANDING: {rel_file}")
            break
    return failures


def _load_db_entries(db_path: pathlib.Path) -> list[dict[str, Any]] | None:
    """Load compile database entries, or None when unreadable."""
    try:
        entries = json.loads(db_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return entries if isinstance(entries, list) else None


def discover_target_dbs(repo_root: pathlib.Path) -> list[pathlib.Path]:
    """Find per-app target compile databases under gitignored build dirs."""
    dbs: list[pathlib.Path] = []
    for top in ("examples", "apps"):
        base = repo_root / top
        if base.is_dir():
            dbs.extend(
                p for p in sorted(base.glob("**/build/compile_commands.json")) if p.is_file()
            )
    return dbs


def check_target_db_flags(db_path: pathlib.Path) -> list[str]:
    """Require every TU in one target database to define RA8_FREESTANDING."""
    entries = _load_db_entries(db_path) if db_path.is_file() else None
    if entries is None:
        return [f"Target compile database unreadable or missing: {db_path}"]
    if not entries:
        return [f"Target compile database empty: {db_path}"]
    return check_db_entries(entries, must_have=True, exempt=None, label="Target compile command")


def check_discovered_target_dbs(repo_root: pathlib.Path) -> list[str]:
    """Enforce the discriminator on every generated per-app target database."""
    failures: list[str] = []
    for db_path in discover_target_dbs(repo_root):
        failures.extend(check_target_db_flags(db_path))
    return failures


def _check_ra8_freestanding_config(repo_root: pathlib.Path) -> list[str]:
    """Verify RA8_FREESTANDING is defined for target builds and absent for host builds."""
    failures = _check_ra8_freestanding_build_files(repo_root)

    target_db_path = repo_root / "compile_commands.json"
    if target_db_path.is_file():
        entries = _load_db_entries(target_db_path)
        if entries is not None:
            failures.extend(
                check_db_entries(
                    entries, must_have=True, exempt=None, label="Target compile command"
                )
            )

    failures.extend(check_discovered_target_dbs(repo_root))

    host_db_path = None
    for p in (
        repo_root / "tests" / "build-darwin" / "compile_commands.json",
        repo_root / "tests" / "build-linux" / "compile_commands.json",
    ):
        if p.is_file():
            host_db_path = p
            break

    if host_db_path:
        # Host tests explicitly building freestanding primitives WILL have the
        # flag (e.g. test_ra8_freestanding). We only check general host code.
        entries = _load_db_entries(host_db_path)
        if entries is not None:
            failures.extend(
                check_db_entries(
                    entries,
                    must_have=False,
                    exempt="-DRA8_TEST_FREESTANDING",
                    label="Host compile command",
                )
            )

    return failures


def selftest() -> int:
    """Run all freestanding-runtime selftests."""
    failures = fr_selftest.run_selftests(_repo_root())
    failures.extend(_check_ra8_freestanding_config(_repo_root()))
    if failures:
        print(f"check_freestanding_runtime selftest: {len(failures)} failure(s):", file=sys.stderr)
        for f in failures:
            print(f"  FAIL: {f}", file=sys.stderr)
        return 1
    print("check_freestanding_runtime selftest: all cases pass (both directions).")
    return 0


def _load_baseline(baseline_path: pathlib.Path) -> dict[str, Any] | None:
    """Load baseline dictionary from JSON file."""
    if not baseline_path.is_file():
        return {}
    try:
        return json.loads(baseline_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        print(f"Error reading baseline file {baseline_path}: {e}", file=sys.stderr)
        return None


def _process_elf_list(
    elfs: list[tuple[str, pathlib.Path, pathlib.Path | None]],
    baseline: dict[str, Any],
    violations: list[str],
    *,
    update: bool,
) -> int:
    """Analyze ELFs and evaluate against baseline or update baseline."""
    for app_name, elf_path, map_file in elfs:
        try:
            analysis = analyze_image(elf_path, map_path=map_file)
        except (OSError, subprocess.SubprocessError, RuntimeError) as e:
            print(f"Error analyzing {elf_path}: {e}", file=sys.stderr)
            return 2

        if update:
            baseline.setdefault("apps", {})[app_name] = {
                "forbidden_symbols": analysis["live_forbidden_symbols"],
                "forbidden_archives": {
                    k: v
                    for k, v in analysis["live_archive_members"].items()
                    if k in _forbidden_archives()
                },
                "sbrk_provider": analysis["sbrk_provider"],
                "end_symbol": analysis["end_symbol_present"],
            }
        else:
            violations.extend(evaluate_against_baseline(app_name, analysis, baseline))
    return 0


def _collect_elfs(
    scan_dir: pathlib.Path | None,
    elf: pathlib.Path | None,
    map_path: pathlib.Path | None,
    app_name: str | None,
) -> list[tuple[str, pathlib.Path, pathlib.Path | None]]:
    """Gather ELF targets to inspect."""
    elfs: list[tuple[str, pathlib.Path, pathlib.Path | None]] = []
    if scan_dir:
        elfs.extend((app_name or p.stem, p, None) for p in scan_dir.glob("**/*.elf"))
    if elf:
        elfs.append((app_name or elf.stem, elf, map_path))
    return elfs


def _run_static_checks(
    args: argparse.Namespace, root: pathlib.Path, baseline: dict[str, Any]
) -> int:
    """Run static source-assert and linker-script checks."""
    if args.check_asserts:
        assert_violations = check_all_source_asserts(root)
        if assert_violations:
            print(
                f"FATAL: Source assert violations ({len(assert_violations)}):",
                file=sys.stderr,
            )
            for v in assert_violations:
                print(f"  {v}", file=sys.stderr)
            return 1
        print("check_freestanding_runtime: target source clean (no standard libc assert).")

    if args.check_scripts:
        script_violations = check_all_linker_scripts(root, baseline)
        if script_violations:
            print(
                f"FATAL: Linker script violations ({len(script_violations)}):",
                file=sys.stderr,
            )
            for v in script_violations:
                print(f"  {v}", file=sys.stderr)
            return 1
        print(
            "check_freestanding_runtime: linker scripts clean (no unauthorized 'end' or '.heap')."
        )
    return 0


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description="Freestanding runtime dependency ratchet.")
    parser.add_argument("--selftest", action="store_true", help="Run self-tests.")
    parser.add_argument("--check-scripts", action="store_true", help="Audit target linker scripts.")
    parser.add_argument("--check-asserts", action="store_true", help="Audit for libc assert.")
    parser.add_argument("--elf", type=pathlib.Path, help="Target ELF to inspect.")
    parser.add_argument("--map", type=pathlib.Path, help="Linker map file to inspect.")
    parser.add_argument("--app-name", type=str, help="Application name for baseline check.")
    parser.add_argument("--scan-dir", type=pathlib.Path, help="Scan a directory for .elf files.")
    parser.add_argument("--baseline", type=pathlib.Path, help="Path to baseline JSON file.")
    parser.add_argument("--update-baseline", action="store_true", help="Update baseline file.")

    args = parser.parse_args()
    if args.selftest:
        return selftest()

    root = _repo_root()
    baseline_path = args.baseline or (root / ".github" / "freestanding-runtime-baseline.json")
    baseline = _load_baseline(baseline_path)
    if baseline is None:
        return 2
    baseline.setdefault("linker_script_exceptions", [])

    violations: list[str] = []

    static_rc = _run_static_checks(args, root, baseline)
    elfs_to_check = _collect_elfs(args.scan_dir, args.elf, args.map, args.app_name)
    # Gate stages run in order and fail fast on the first nonzero status.
    rc = static_rc or _process_elf_list(
        elfs_to_check, baseline, violations, update=args.update_baseline
    )
    if rc != 0:
        return rc
    if args.scan_dir:
        db_violations = check_target_db_flags(args.scan_dir / "compile_commands.json")
        if db_violations:
            print(
                f"FATAL: Freestanding target misconfigured ({len(db_violations)}):",
                file=sys.stderr,
            )
            for v in db_violations:
                print(f"  {v}", file=sys.stderr)
            return 1

    if args.update_baseline:
        baseline_path.write_text(
            json.dumps(baseline, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"check_freestanding_runtime: updated baseline at {baseline_path}")
    elif violations:
        print(f"FATAL: Freestanding runtime violations ({len(violations)}):", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
