#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Selftests for ``check_linker_scripts.py``: every rule fired and held quiet.

This module is the checker's PROOF, not its enforcement. Each ``_selftest_*``
function drives one rule family against a fixture whose verdict is known by
construction -- a malformed script that must draw LD001-LD005, a tricky but
legal one that must stay silent, a synthetic option-setting script with one
word removed, a MEMORY region walked past the end of the SRAM window -- and
returns 0 only when the checker said exactly what the fixture was built to
make it say. A rule that matches nothing is a failure here, not a pass.

WHY IT LIVES BESIDE THE CHECKER RATHER THAN INSIDE IT
=====================================================
``check_linker_scripts.py`` carries ten rules and the device-header parse that
LD009/LD010 are measured against, and the proof of those rules is about as long
again as the rules themselves. Keeping both in one file put it past the
1000-line ceiling ``check_file_size.py`` enforces, and made a fixture edit
unreviewable next to a rule edit. The split follows the one already made for
``linker_script_fixtures.py`` (static fixture text) and for
``lint_coverage_rules.py``: data, proof, and enforcement each in their own file.

The import runs ONE WAY. This module imports the checker; the checker imports
this one only inside ``selftest()``, deferred, so there is no cycle and the
plain scan never pays for the fixtures. The builder-style fixtures that need
the checker's own tables (``_synth_option_script``, ``_sram_fixture``,
``_device_fixture``) moved here with the assertions that use them; the static
whole-file fixtures stay in ``linker_script_fixtures.py``.

Run it through the checker's own CLI: ``check_linker_scripts.py --selftest``.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "dev"))

from check_linker_scripts import (
    DEVICE_MEM,
    OPTION_SETTING_ADDR,
    SRAM_WINDOW_BASE,
    SRAM_WINDOW_SIZE,
    check_file,
    closure_problems,
    defined_symbols,
    eval_size,
    option_section,
    parse_device_memory_map,
    referenced_symbols,
    repo_files,
)
from git_environment import trusted_git_executable
from linker_script_fixtures import MALFORMED, OFS_BAD, OFS_GOOD, TRICKY


def _selftest_option_setting() -> int:
    """LD007 fires on the phantom region and a wrong OFS0 address, quiet on the twin."""
    rc = 0
    with tempfile.TemporaryDirectory() as td:
        bad = pathlib.Path(td) / "ofs_bad.ld"
        bad.write_bytes(OFS_BAD.encode())
        codes = {f.code for f in check_file(bad, bad.read_bytes())}
        if "LD007" not in codes:
            print("SELFTEST FAIL: ofs_bad.ld did not report LD007")
            rc = 1
        else:
            print("selftest: ofs_bad.ld -> LD007 (phantom + wrong OFS0) OK")

        good = pathlib.Path(td) / "ofs_good.ld"
        good.write_bytes(OFS_GOOD.encode())
        ld007 = [f for f in check_file(good, good.read_bytes()) if f.code == "LD007"]
        if ld007:
            print("SELFTEST FAIL: ofs_good.ld should have no LD007 but reported:")
            for f in ld007:
                print(f"    {f}")
            rc = 1
        else:
            print("selftest: ofs_good.ld -> no LD007 OK")
    return rc


def _synth_option_script(omit: tuple[str, ...] = (), stray: str = "") -> str:
    """Build a syntactically real .ld declaring the option family minus `omit`.

    Generated from OPTION_SETTING_ADDR so the "complete" case cannot rot as the
    HUM table grows -- the point of that case is that LD008 stays SILENT on a
    complete script, which is only meaningful if "complete" tracks the table.
    """
    names = [n for n in OPTION_SETTING_ADDR if n not in omit]
    provides = "\n".join(f"PROVIDE({n} = 0x{OPTION_SETTING_ADDR[n]:08X});" for n in names)
    sections = "\n".join(
        f"    {option_section(n)} {n} : {{ KEEP(*({option_section(n)})) }} > OFS_CFG" for n in names
    )
    if stray:
        sections += f"\n    {stray} 0x02C9F800 : {{ KEEP(*({stray})) }} > OFS_CFG"
    return (
        "/*\n * Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n */\n\n"
        "ENTRY(Reset_Handler)\n\n"
        "MEMORY\n{\n"
        "    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K\n"
        "    OFS_CFG (r) : ORIGIN = 0x02C9F000, LENGTH = 2K\n"
        "    OFS_OTP (r) : ORIGIN = 0x02E07000, LENGTH = 68K\n}\n\n"
        f"{provides}\n\n"
        "SECTIONS\n{\n"
        "    .text : { *(.text) } > MRAM\n"
        f"{sections}\n}}\n"
    )


# The exact trio #223 deleted from the four RA8P1 app scripts. LD008 exists to
# make that deletion impossible to land again, so the selftest reproduces it
# rather than an invented omission.
OFS3_FAMILY = ("OFS3_ADDR", "OFS3_SEC_ADDR", "OFS3_SEL_ADDR")

# Below this many words, OPTION_SETTING_ADDR has plainly been gutted and every
# LD008 case built from it would pass without asserting anything.
MIN_OPTION_WORDS = 20


def _selftest_option_completeness() -> int:
    """LD008 fires on a partial family and on a stray section, silent when complete."""
    rc = 0
    # Anchor: an emptied or OFS3-less table would make every case below vacuous.
    missing = [n for n in OFS3_FAMILY if n not in OPTION_SETTING_ADDR]
    n_words = len(OPTION_SETTING_ADDR)
    if missing or n_words < MIN_OPTION_WORDS:
        print(f"SELFTEST FAIL: OPTION_SETTING_ADDR lost {missing or 'entries'}; LD008 vacuous")
        return 1
    print(f"selftest: OPTION_SETTING_ADDR has {n_words} words incl. the OFS3 family OK")

    with tempfile.TemporaryDirectory() as td:
        cases = [
            ("partial.ld", _synth_option_script(omit=OFS3_FAMILY), True, "OFS3 family cut (#223)"),
            ("complete.ld", _synth_option_script(), False, "complete family"),
            ("stray.ld", _synth_option_script(stray=".option_setting_ofs4"), True, "phantom ofs4"),
        ]
        for fname, text, want_fire, label in cases:
            p = pathlib.Path(td) / fname
            p.write_bytes(text.encode())
            ld008 = [f for f in check_file(p, p.read_bytes()) if f.code == "LD008"]
            if want_fire and not ld008:
                print(f"SELFTEST FAIL: {fname} ({label}) did not report LD008")
                rc = 1
            elif not want_fire and ld008:
                print(f"SELFTEST FAIL: {fname} ({label}) should have no LD008 but reported:")
                for f in ld008:
                    print(f"    {f}")
                rc = 1
            else:
                verdict = "LD008" if want_fire else "no LD008"
                print(f"selftest: {fname} -> {verdict} ({label}) OK")

        # A script owning no option bytes at all must stay silent -- that is the
        # CPU1 / non-secure-image shape, 64 files in this tree.
        none = pathlib.Path(td) / "none.ld"
        none.write_bytes(TRICKY.encode())
        if [f for f in check_file(none, none.read_bytes()) if f.code == "LD008"]:
            print("SELFTEST FAIL: a script with no option-setting block reported LD008")
            rc = 1
        else:
            print("selftest: none.ld -> no LD008 (owns no option bytes) OK")
    return rc


def _selftest_fixtures() -> int:
    """The two whole-file fixtures: every code must fire, nothing may over-fire."""
    rc = 0
    with tempfile.TemporaryDirectory() as td:
        bad = pathlib.Path(td) / "malformed.ld"
        bad.write_bytes(MALFORMED.encode())
        got = check_file(bad, bad.read_bytes())
        codes = {f.code for f in got}
        expected = {"LD001", "LD002", "LD003", "LD004", "LD005"}
        missing = expected - codes
        if missing:
            print(f"SELFTEST FAIL: malformed.ld did not report {sorted(missing)}")
            for f in got:
                print(f"    got: {f}")
            rc = 1
        else:
            print(f"selftest: malformed.ld -> {len(got)} findings {sorted(codes)} OK")

        good = pathlib.Path(td) / "tricky.ld"
        good.write_bytes(TRICKY.encode())
        got = check_file(good, good.read_bytes())
        if got:
            print("SELFTEST FAIL: tricky.ld should be clean but reported:")
            for f in got:
                print(f"    {f}")
            rc = 1
        else:
            print("selftest: tricky.ld -> 0 findings OK")
    return rc


def _selftest_symbol_scan() -> tuple[int, set[str], set[str]]:
    """LD006 halves: a symbol named only in a comment is neither defined nor used."""
    rc = 0
    ld_text = (
        "/* mentions g_ra8_ls_in_comment_only, which is NOT a definition */\n"
        "g_ra8_ls_alpha = .;\n"
        "PROVIDE(g_ra8_ls_beta = 0x20000000);\n"
    )
    c_text = (
        "/* prose naming g_ra8_ls_prose_only must not count as a use */\n"
        "// nor g_ra8_ls_slash_comment\n"
        "extern uint32_t g_ra8_ls_alpha;\n"
        "extern uint32_t g_ra8_ls_missing;\n"
    )
    got_def = defined_symbols(ld_text)
    if got_def != {"g_ra8_ls_alpha", "g_ra8_ls_beta"}:
        print(f"SELFTEST FAIL: defined_symbols -> {sorted(got_def)}")
        rc = 1
    else:
        print("selftest: defined_symbols ignores comment mentions OK")

    got_ref = referenced_symbols(c_text)
    if got_ref != {"g_ra8_ls_alpha", "g_ra8_ls_missing"}:
        print(f"SELFTEST FAIL: referenced_symbols -> {sorted(got_ref)}")
        rc = 1
    else:
        print("selftest: referenced_symbols ignores comment mentions OK")
    return rc, got_def, got_ref


def _selftest_closure(got_def: set[str], got_ref: set[str]) -> int:
    """LD006 closure, both directions: fires on a gap, silent when resolved."""
    rc = 0
    defined = {s: ["fake.ld"] for s in got_def}
    referenced = {s: ["fake.c"] for s in got_ref}
    problems = closure_problems(defined, referenced)
    if len(problems) != 1 or "g_ra8_ls_missing" not in problems[0]:
        print(f"SELFTEST FAIL: closure_problems -> {problems}")
        rc = 1
    else:
        print("selftest: closure fires on an undefined symbol OK")

    if closure_problems({"g_ra8_ls_a": ["x.ld"]}, {"g_ra8_ls_a": ["x.c"]}):
        print("SELFTEST FAIL: closure fired on a fully-resolved symbol")
        rc = 1
    else:
        print("selftest: closure quiet when every symbol resolves OK")
    return rc


def _selftest_worktree_inventory() -> int:
    """Candidate scope includes an unstaged move target and drops its source."""
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        subprocess.run(  # noqa: S603 -- fixed Git argv and private fixture path
            [trusted_git_executable(), "init", "-q", str(root)],
            check=True,
        )
        old = root / "old.c"
        old.write_text("int old_symbol;\n", encoding="utf-8")
        subprocess.run(  # noqa: S603 -- fixed Git argv and private fixture path
            [trusted_git_executable(), "-C", str(root), "add", "old.c"],
            check=True,
        )
        old.unlink()
        new = root / "new.c"
        new.write_text("int new_symbol;\n", encoding="utf-8")

        got = [path.relative_to(root).as_posix() for path in repo_files(root, "*.c")]
        if got != ["new.c"]:
            print(f"SELFTEST FAIL: worktree move inventory -> {got}")
            return 1
        print("selftest: worktree move drops deleted source and includes destination OK")
    return 0


def _sram_fixture(ns_sram_len: str) -> str:
    """A board-shaped script whose NS_SRAM placeholder is sized `ns_sram_len`.

    The other rows are load-bearing: SRAM ``1024K - 256`` exercises subtraction,
    NOINIT sits at the top of SRAM (must stay silent), and NS_SRAM_RUN at 0x32..
    is the non-secure alias that must fall OUTSIDE the window LD009 judges.
    """
    return (
        "/*\n * Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n */\n\n"
        "ENTRY(Reset_Handler)\n\n"
        "MEMORY\n{\n"
        "    SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 1024K - 256\n"
        "    NOINIT (rw) : ORIGIN = 0x220FFF00, LENGTH = 256\n"
        f"    NS_SRAM (rwx) : ORIGIN = 0x22100000, LENGTH = {ns_sram_len}\n"
        "    NS_SRAM_RUN (rwx) : ORIGIN = 0x32100000, LENGTH = 512K\n}\n"
    )


def _selftest_sram_fit() -> int:
    """LD009 fires on a region past the SRAM end, silent when every region fits."""
    rc = 0
    # Anchor: a collapsed evaluator or a zeroed window constant would make every
    # case below vacuous. Compare only named constants and eval_size results,
    # never a bare literal, encoding the real bank arithmetic (1M + 640K = 1664K).
    if eval_size("1664K") != SRAM_WINDOW_SIZE or eval_size("1024K") != eval_size("1M"):
        print("SELFTEST FAIL: eval_size unit / SRAM-window anchor is wrong")
        return 1
    if eval_size("1M - 384K") != eval_size("640K") or eval_size("ORIGIN(SRAM)") is not None:
        print("SELFTEST FAIL: eval_size mis-handled subtraction or a symbolic expr")
        return 1

    with tempfile.TemporaryDirectory() as td:
        # 1024K overruns to 0x22200000 (the #544 defect); 640K lands exactly on
        # 0x221A0000, proving the bound is inclusive.
        for tag, ns_len, want in (("overrun.ld", "1024K", True), ("fits.ld", "640K", False)):
            p = pathlib.Path(td) / tag
            p.write_bytes(_sram_fixture(ns_len).encode())
            ld009 = [f for f in check_file(p, p.read_bytes()) if f.code == "LD009"]
            if want and (len(ld009) != 1 or "NS_SRAM" not in ld009[0].msg):
                print(f"SELFTEST FAIL: {tag} expected one LD009 on NS_SRAM, got {ld009}")
                rc = 1
            elif not want and ld009:
                print(f"SELFTEST FAIL: {tag} should have no LD009 but reported {ld009}")
                rc = 1
            else:
                print(f"selftest: {tag} -> {'LD009 on NS_SRAM' if want else 'no LD009'} OK")
    return rc


def _device_fixture(itcm: str, sdram_origin: str = "0x68000000") -> str:
    """A board-shaped script whose ITCM row and SDRAM origin are caller-chosen.

    Everything else is the real EK layout, so a finding can only come from the
    row under test rather than from a fixture that was never legal.
    """
    return (
        "/*\n * Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n */\n\n"
        "ENTRY(Reset_Handler)\n\n"
        "MEMORY\n{\n"
        "    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K\n"
        f"    ITCM (rwx) : {itcm}\n"
        "    DTCM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K\n"
        "    SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 1024K\n"
        f"    SDRAM (rwx) : ORIGIN = {sdram_origin}, LENGTH = 64M\n"
        "    NS_SRAM (rwx) : ORIGIN = 0x22100000, LENGTH = 640K\n}\n"
    )


def _selftest_device_map() -> int:
    """The map is really READ from ra8_device.h, and rejects a broken header."""
    rc = 0
    # Anchor: prove DEVICE_MEM came from the header rather than from a default.
    # A stubbed-out parser would fail here before any fixture below runs.
    sram_size = DEVICE_MEM["k_ra8_mem_sram_size"]
    sram_base = DEVICE_MEM["k_ra8_mem_sram_base"]
    if sram_size != eval_size("1664K"):
        print(f"SELFTEST FAIL: k_ra8_mem_sram_size parsed as {sram_size}")
        return 1
    if sram_size != SRAM_WINDOW_SIZE or sram_base != SRAM_WINDOW_BASE:
        print("SELFTEST FAIL: the SRAM window constants are not the header's values")
        return 1
    print("selftest: SRAM window read from ra8_device.h OK")

    synthetic = (
        "typedef enum : uintptr_t {\n"
        "  k_ra8_mem_mram_base  = 0x0A000000U, /**< moved. */\n"
        "  k_ra8_mem_itcm_base  = 0x00000000U,\n"
        "  k_ra8_mem_dtcm_base  = 0x20000000U,\n"
        "  k_ra8_mem_sram_base  = 0x22000000U,\n"
        "  k_ra8_mem_sdram_base = 0x68000000U,\n"
        "} ra8_device_mem_base_t;\n"
        "typedef enum : uint32_t {\n"
        "  k_ra8_mem_mram_size = 0x00100000U,\n"
        "  k_ra8_mem_itcm_size = 0x00010000U,\n"
        "  k_ra8_mem_dtcm_size = 0x00010000U,\n"
        "  k_ra8_mem_sram_size = 0x001A0000U,\n"
        "} ra8_device_mem_size_t;\n"
    )
    parsed = parse_device_memory_map(synthetic)
    moved_mram_base = 0x0A000000
    if parsed["k_ra8_mem_mram_base"] != moved_mram_base:
        print(f"SELFTEST FAIL: synthetic header parsed as {parsed}")
        rc = 1
    else:
        print("selftest: synthetic header parses to its own values OK")

    try:
        parse_device_memory_map(synthetic.replace("k_ra8_mem_sram_size", "k_ra8_mem_sram_bytes"))
    except ValueError as exc:
        if "k_ra8_mem_sram_size" not in str(exc):
            print(f"SELFTEST FAIL: rename raised the wrong ValueError: {exc}")
            rc = 1
        else:
            print("selftest: a renamed memory-map enum is a loud failure OK")
    else:
        print("SELFTEST FAIL: a renamed memory-map enum was accepted")
        rc = 1
    return rc


def _selftest_device_region_fit() -> int:
    """LD010 fires on a mis-addressed / oversized named region, silent when legal."""
    rc = 0
    cases = (
        # tag, ITCM row, SDRAM origin, expected LD010 count, region named in msg
        ("legal.ld", "ORIGIN = 0x00000000, LENGTH = 64K", "0x68000000", 0, ""),
        ("itcm_addr.ld", "ORIGIN = 0x30000000, LENGTH = 64K", "0x68000000", 1, "ITCM"),
        ("itcm_size.ld", "ORIGIN = 0x00000000, LENGTH = 128K", "0x68000000", 1, "ITCM"),
        ("sdram_addr.ld", "ORIGIN = 0x00000000, LENGTH = 64K", "0x60000000", 1, "SDRAM"),
    )
    with tempfile.TemporaryDirectory() as td:
        for tag, itcm, sdram, want, region in cases:
            path = pathlib.Path(td) / tag
            path.write_bytes(_device_fixture(itcm, sdram).encode())
            got = [f for f in check_file(path, path.read_bytes()) if f.code == "LD010"]
            if len(got) != want or (region and region not in got[0].msg):
                named = region or "nothing"
                print(f"SELFTEST FAIL: {tag} expected {want} LD010 on {named}, got {got}")
                rc = 1
            else:
                print(f"selftest: {tag} -> {want} LD010 OK")
        # A 64K ITCM overrun is LD010's, not LD009's: the TCM window is nowhere
        # near the SRAM array, so exactly one rule must own each defect.
        path = pathlib.Path(td) / "itcm_size.ld"
        if [f for f in check_file(path, path.read_bytes()) if f.code == "LD009"]:
            print("SELFTEST FAIL: LD009 double-reported an ITCM overrun")
            rc = 1
        else:
            print("selftest: ITCM overrun reported once, by LD010 only OK")
    return rc


def run_selftests() -> int:
    """Assert every finding code fires, and that none of them over-fires."""
    rc = _selftest_fixtures()
    rc |= _selftest_option_setting()
    rc |= _selftest_option_completeness()
    rc |= _selftest_sram_fit()
    rc |= _selftest_device_map()
    rc |= _selftest_device_region_fit()
    scan_rc, got_def, got_ref = _selftest_symbol_scan()
    return rc | scan_rc | _selftest_closure(got_def, got_ref) | _selftest_worktree_inventory()
