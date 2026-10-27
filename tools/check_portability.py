#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""RA8FW-618 slice 2: the portability gate.

Enforces the classification table published in libs/README.md (slice 1):

  1. Import-edge check. Every library marked `portable` in the README table
     may import only ra8_core, the other portable libraries, and the standard
     library. An include or import that names a non-portable library
     (ra8_hal, ra8_nsc, ra8_board_*, ra8_mpu, ra8_io, ...) or a vendor SDK
     header (FSP, BSP, CMSIS) is a violation.
  2. Syntax check. Every Zig source under a portable library is checked with
     `zig ast-check`.
  3. Cross-target compile check. C sources under portable libraries are
     compiled to throwaway objects for the host, riscv32 (ESP32-C) and
     thumbv7em (TI / Cortex-M4).

The fast gate compiles throwaway objects, so it can only catch what a
compile sees: ARM-only asm that fails a real freestanding image build passes
it silently (ra8_core's riscv32 image, RA8FW-1068). The companion
`--image-builds` phase runs each portable library's real cross builds.

Usage:
  python3 tools/check_portability.py            # gate; exit 0 clean, 1 findings
  python3 tools/check_portability.py --image-builds          # + real builds
  python3 tools/check_portability.py --image-builds ra8_core # subset
  python3 tools/check_portability.py --self-test
"""

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# Zig toolchain resolution: RA8_ZIG names a pinned or out-of-PATH install,
# the same convention just/zig.just and the CMake zig helpers use.
ZIG = os.environ.get("RA8_ZIG", "zig")
README = REPO / "libs" / "README.md"
VENDORED_DIRS = {"netxduo", "threadx", "third_party", "vendored", "external", "freertos"}
SKIP_DIRS = VENDORED_DIRS | {".git", ".zig-cache", "zig-out", "zig-pkg"}

INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^\">\n]+)[\">]", re.M)
IMPORT_RE = re.compile(r"@import\(\s*\"([^\"]+)\"\s*\)")
VENDOR_RE = re.compile(r"(?i)(fsp_|bsp_|cmsis|core_cm[0-9]|hal_data|r_ioport)")

# Freestanding targets get the gate's minimal libc-header shim
# (tools/portability_gate_libc): a real port supplies its own libc, so the
# gate checks ISA/semantics portability, not libc availability. The host
# target keeps the real headers.
TARGETS = {
    "host": {},
    "riscv32": {"target": "riscv32-freestanding-eabi", "stubs": True},
    "thumbv7em": {"target": "thumb-freestanding-eabi", "mcpu": "cortex_m4", "stubs": True},
}
STUB_DIR = REPO / "tools" / "portability_gate_libc"


def parse_table(readme_text):
    """Parse the ## Portability table: {lib_name: class}."""
    lines = readme_text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.strip() == "## Portability":
            start = i
            break
    if start is None:
        raise SystemExit("gate: no '## Portability' heading in libs/README.md")
    table = {}
    row_re = re.compile(r"^\|\s*`([^`]+)`\s*\|\s*(portable|port-through-interface|RA8-only)\s*\|")
    for line in lines[start + 1:]:
        if line.startswith("#"):
            break
        m = row_re.match(line)
        if m:
            table[m.group(1)] = m.group(2)
    if not table:
        raise SystemExit("gate: no classification rows parsed from libs/README.md")
    return table


def forbidden_header(name, header, nonportable):
    """True if a C include names a non-portable library or a vendor header."""
    first = header.split("/")[0]
    stem = first[:-2] if first.endswith(".h") else first
    for lib in nonportable:
        if header.startswith(lib + "/") or first == lib:
            return f"non-portable library {lib}"
        if stem == lib or stem.startswith(lib + "_"):
            return f"non-portable library {lib}"
    m = VENDOR_RE.search(header)
    if m:
        return f"vendor SDK header ({m.group(1)})"
    return None


def scan_file(path, lib, nonportable):
    findings = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"{path}: unreadable ({exc})"]
    if path.suffix in (".c", ".h"):
        for m in INCLUDE_RE.finditer(text):
            why = forbidden_header(lib, m.group(1).strip(), nonportable)
            if why:
                line = text.count("\n", 0, m.start()) + 1
                findings.append(f"{path}:{line}: includes {m.group(1).strip()} ({why})")
    elif path.suffix == ".zig":
        for m in IMPORT_RE.finditer(text):
            imp = m.group(1)
            if imp.startswith("./") or imp.startswith("../"):
                continue
            for lib2 in nonportable:
                if imp == lib2 or imp.startswith(lib2 + "/"):
                    line = text.count("\n", 0, m.start()) + 1
                    findings.append(f"{path}:{line}: imports {imp} (non-portable library {lib2})")
                    break
            else:
                vm = VENDOR_RE.search(imp)
                if vm:
                    line = text.count("\n", 0, m.start()) + 1
                    findings.append(f"{path}:{line}: imports {imp} (vendor SDK module {vm.group(1)})")
    return findings


def portable_sources(lib_dir):
    files = []
    for root, dirs, names in os.walk(lib_dir):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in names:
            if name.endswith((".c", ".h", ".zig")):
                files.append(Path(root) / name)
    return files


def collect_inc_dirs(portable_names):
    incs = []
    for lib in portable_names:
        d = REPO / "libs" / lib
        for sub in ("inc", "src"):
            p = d / sub
            if p.is_dir():
                incs.append(str(p))
    p = REPO / "libs" / "if" / "inc"
    if p.is_dir() and "if" not in portable_names:
        incs.append(str(p))
    return incs


def run(cmd, cwd=None, timeout=120, env=None):
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout, cwd=cwd, env=env)
        return proc.returncode, proc.stdout + proc.stderr
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 2, str(exc)


# RA8FW-1068 companion phase: real freestanding image builds. A portable
# library's build.zig cross-compiles its whole tree for the target, so
# arch-specific asm fails at the actual instruction here, where the
# throwaway-object gate above cannot see it. Targets mirror the proof-port
# recipe verified 2026-10-26: ra8_core and ra8_xml both build clean
# ReleaseSmall archives for riscv32-freestanding-none and
# thumb-freestanding-eabi -Dcpu=cortex_m4.
IMAGE_TARGETS = (
    ("riscv32", ("-Dtarget=riscv32-freestanding-none",)),
    ("thumbv7em", ("-Dtarget=thumb-freestanding-eabi", "-Dcpu=cortex_m4")),
)


def image_build_phase(libs, build_zig_libs):
    """Build the named portable libraries' real freestanding images."""
    unknown = [lib for lib in libs if lib not in build_zig_libs]
    if unknown:
        return ["gate: --image-builds names portable libraries without a "
                f"build.zig: {', '.join(unknown)}"]
    lcache = Path("/tmp") / "ra8-gate-cache"
    gcache = Path("/tmp") / "ra8-gate-global-cache"
    env = dict(os.environ)
    env["ZIG_GLOBAL_CACHE_DIR"] = str(gcache)
    findings = []
    for lib in libs:
        d = REPO / "libs" / lib
        for tname, extra in IMAGE_TARGETS:
            cmd = [ZIG, "build", "-Doptimize=ReleaseSmall", *extra,
                   "--cache-dir", str(lcache)]
            code, out = run(cmd, cwd=d, env=env, timeout=600)
            if code == 0:
                print(f"gate: image build {lib} {tname}: clean")
            else:
                print(f"gate: image build {lib} {tname}: FAILED (exit {code})")
                findings.append(f"libs/{lib}: {tname} freestanding image build "
                                f"failed (exit {code})")
                for line in out.strip().splitlines()[-12:]:
                    findings.append(f"    {line}")
    return findings


def main():
    ap = argparse.ArgumentParser(description="RA8FW-618 slice 2 portability gate")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--image-builds", nargs="*", metavar="LIB", default=None,
                    help="also build the named portable libraries' real "
                         "freestanding images (default: every portable "
                         "library with a build.zig); slow")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    table = parse_table(README.read_text(encoding="utf-8"))
    portable = sorted(n for n, c in table.items() if c == "portable")
    nonportable = sorted(set(table) - set(portable))
    print(f"gate: {len(table)} rows; {len(portable)} portable, {len(nonportable)} non-portable")

    findings = []
    zig_files = []
    c_files = []
    build_zig_libs = []
    for lib in portable:
        d = REPO / "libs" / lib
        if not d.is_dir():
            findings.append(f"libs/{lib}: table row has no directory")
            continue
        if (d / "build.zig").exists():
            build_zig_libs.append(lib)
        for f in portable_sources(d):
            findings.extend(scan_file(f, lib, nonportable))
            if f.suffix == ".zig":
                zig_files.append(f)
            elif f.suffix == ".c":
                c_files.append(f)

    print(f"gate: edge scan {len(portable)} portable libs "
          f"({len(zig_files)} zig, {len(c_files)} c files)")

    if zig_files:
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(
                lambda f: (f, run([ZIG, "ast-check", str(f)])[0]),
                zig_files))
        for f, code in results:
            if code != 0:
                findings.append(f"{f}: zig ast-check failed (exit {code})")
        print(f"gate: zig ast-check {len(zig_files)} files")

    if c_files:
        incs = collect_inc_dirs(portable)
        for name, opts in TARGETS.items():
            objdir = Path("/tmp") / "ra8-gate-objs"
            objdir.mkdir(parents=True, exist_ok=True)
            # A throwaway object instead of -fsyntax-only: this zig's
            # -fsyntax-only still emits an object and then fails its cache
            # rename, so plain -c is the honest check.
            cmd = [ZIG, "cc", "-c", "-std=c2x", "-include", "stdbool.h"]
            target = opts.get("target")
            if target:
                cmd += ["-target", target]
            if opts.get("mcpu"):
                cmd += ["-mcpu=" + opts["mcpu"]]
            cmd += [f"-I{d}" for d in incs]
            if opts.get("stubs"):
                cmd += [f"-I{STUB_DIR}"]
            bad = 0
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results = list(pool.map(
                    lambda f: (f, run(cmd + ["-o", str(objdir / (f.stem + "-" + name + ".o")), str(f)])[0]),
                    c_files))
            for f, code in results:
                if code != 0:
                    findings.append(f"{f}: zig cc compile {name} failed (exit {code})")
                    bad += 1
            print(f"gate: c compile {name}: {len(c_files) - bad}/{len(c_files)} clean")

    print(f"gate: {len(build_zig_libs)} portable libs carry a build.zig")
    if args.image_builds is not None:
        findings.extend(
            image_build_phase(args.image_builds or build_zig_libs,
                              build_zig_libs))

    if findings:
        print(f"gate: FAIL, {len(findings)} finding(s):")
        for f in findings[:80]:
            print(f"  {f}")
        if len(findings) > 80:
            print(f"  ... and {len(findings) - 80} more")
        return 1
    print("gate: PASS, portable rows hold their edges")
    return 0


def self_test():
    """Build a throwaway repo skeleton; the gate must catch a violation and
    pass a clean lib."""
    tmp = Path(tempfile.mkdtemp(prefix="ra8_gate_selftest_"))
    try:
        (tmp / "libs").mkdir(parents=True)
        table = (
            "## Portability\n\n"
            "| Library | Class | Decided by |\n|---|---|---|\n"
            "| `ra8_fix` | portable | self-test |\n"
            "| `ra8_clean` | portable | self-test |\n"
            "| `ra8_hal` | RA8-only | self-test |\n"
        )
        (tmp / "libs" / "README.md").write_text(table)
        bad = tmp / "libs" / "ra8_fix" / "src"
        bad.mkdir(parents=True)
        (bad / "bad.c").write_text('#include "ra8_hal/x.h"\n#include "cmsis.h"\n')
        (bad / "bad.zig").write_text('const h = @import("ra8_hal");\n')
        clean = tmp / "libs" / "ra8_clean" / "src"
        clean.mkdir(parents=True)
        (clean / "ok.zig").write_text('const l = @import("./ok.zig");\npub const x = l.x;\n')

        global REPO, README
        saved = (REPO, README)
        REPO = tmp
        README = tmp / "libs" / "README.md"
        try:
            code_bad = gate_silent()
            (bad / "bad.c").unlink()
            (bad / "bad.zig").unlink()
            code_clean = gate_silent()
        finally:
            REPO, README = saved

        if code_bad != 1:
            print(f"self-test: FAIL, expected the fixture to fail (exit 1), got {code_bad}")
            return 2
        if code_clean != 0:
            print(f"self-test: FAIL, expected the clean lib to pass (exit 0), got {code_clean}")
            return 2
        print("self-test: PASS, gate catches the violation and passes the clean lib")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def gate_silent():
    """Run main()'s checks with output discarded; used by the self-test."""
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = real_gate()
    return code


def real_gate():
    table = parse_table(README.read_text(encoding="utf-8"))
    portable = sorted(n for n, c in table.items() if c == "portable")
    nonportable = sorted(set(table) - set(portable))
    findings = []
    for lib in portable:
        d = REPO / "libs" / lib
        if not d.is_dir():
            findings.append(f"libs/{lib}: table row has no directory")
            continue
        for f in portable_sources(d):
            findings.extend(scan_file(f, lib, nonportable))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
