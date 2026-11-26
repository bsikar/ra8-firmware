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

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import io
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

INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^\">\n]+)[\">]", re.MULTILINE)
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
_FINDINGS_PREVIEW_LIMIT = 80


def parse_table(readme_text: str) -> dict[str, str]:
    """Parse the ## Portability table: {lib_name: class}."""
    lines = readme_text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.strip() == "## Portability":
            start = i
            break
    if start is None:
        msg = "gate: no '## Portability' heading in libs/README.md"
        raise SystemExit(msg)
    table: dict[str, str] = {}
    row_re = re.compile(r"^\|\s*`([^`]+)`\s*\|\s*(portable|port-through-interface|RA8-only)\s*\|")
    for line in lines[start + 1 :]:
        if line.startswith("#"):
            break
        m = row_re.match(line)
        if m:
            table[m.group(1)] = m.group(2)
    if not table:
        msg = "gate: no classification rows parsed from libs/README.md"
        raise SystemExit(msg)
    return table


def forbidden_header(_name: str, header: str, nonportable: list[str]) -> str | None:
    """Return a violation reason if a C include names a non-portable or vendor header."""
    first = header.split("/", maxsplit=1)[0]
    stem = first.removesuffix(".h")
    for lib in nonportable:
        if header.startswith(lib + "/") or first == lib:
            return f"non-portable library {lib}"
        if stem == lib or stem.startswith(lib + "_"):
            return f"non-portable library {lib}"
    m = VENDOR_RE.search(header)
    if m:
        return f"vendor SDK header ({m.group(1)})"
    return None


def scan_file(path: Path, lib: str, nonportable: list[str]) -> list[str]:
    """Scan one C, header, or Zig source file for non-portable or vendor edges."""
    findings: list[str] = []
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
            if imp.startswith(("./", "../")):
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
                    findings.append(
                        f"{path}:{line}: imports {imp} (vendor SDK module {vm.group(1)})"
                    )
    return findings


def portable_sources(lib_dir: Path) -> list[Path]:
    """Return every C, header, and Zig file under `lib_dir`, skipping vendored trees."""
    files: list[Path] = []
    for root, dirs, names in os.walk(lib_dir):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        files.extend(Path(root) / name for name in names if name.endswith((".c", ".h", ".zig")))
    return files


def collect_inc_dirs(portable_names: list[str], repo: Path = REPO) -> list[str]:
    """Collect public and internal include directories for portable libraries."""
    incs: list[str] = []
    for lib in portable_names:
        d = repo / "libs" / lib
        for sub in ("inc", "src"):
            p = d / sub
            if p.is_dir():
                incs.append(str(p))
    p = repo / "libs" / "if" / "inc"
    if p.is_dir() and "if" not in portable_names:
        incs.append(str(p))
    return incs


def run(
    cmd: list[str],
    cwd: Path | None = None,
    timeout: int = 120,
    env: dict[str, str] | None = None,
) -> tuple[int, str]:
    """Run a fixed toolchain command and return ``(returncode, combined_output)``."""
    try:
        proc = subprocess.run(  # noqa: S603 -- fixed zig/cc argv assembled from repo paths
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=cwd,
            env=env,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 2, str(exc)
    else:
        return proc.returncode, proc.stdout + proc.stderr


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


def image_build_phase(libs: list[str], build_zig_libs: list[str]) -> list[str]:
    """Build the named portable libraries' real freestanding images."""
    unknown = [lib for lib in libs if lib not in build_zig_libs]
    if unknown:
        bad = ", ".join(unknown)
        return [f"gate: --image-builds names portable libraries without a build.zig: {bad}"]
    tmp_root = Path(tempfile.gettempdir())
    lcache = tmp_root / "ra8-gate-cache"
    gcache = tmp_root / "ra8-gate-global-cache"
    env = dict(os.environ)
    env["ZIG_GLOBAL_CACHE_DIR"] = str(gcache)
    findings: list[str] = []
    for lib in libs:
        d = REPO / "libs" / lib
        for tname, extra in IMAGE_TARGETS:
            cmd = [ZIG, "build", "-Doptimize=ReleaseSmall", *extra, "--cache-dir", str(lcache)]
            code, out = run(cmd, cwd=d, env=env, timeout=600)
            if code == 0:
                print(f"gate: image build {lib} {tname}: clean")
            else:
                print(f"gate: image build {lib} {tname}: FAILED (exit {code})")
                findings.append(
                    f"libs/{lib}: {tname} freestanding image build failed (exit {code})"
                )
                findings.extend(f"    {line}" for line in out.strip().splitlines()[-12:])
    return findings


def _check_zig_ast(zig_files: list[Path]) -> list[str]:
    """Run ``zig ast-check`` across all portable Zig files."""
    if not zig_files:
        return []
    findings: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(lambda f: (f, run([ZIG, "ast-check", str(f)])[0]), zig_files))
    for f, code in results:
        if code != 0:
            findings.append(f"{f}: zig ast-check failed (exit {code})")
    print(f"gate: zig ast-check {len(zig_files)} files")
    return findings


def _compile_c_file(args: tuple[Path, list[str], Path, str]) -> tuple[Path, int]:
    """Compile one C source to a throwaway object under a fixed target."""
    src, base_cmd, objdir, target_name = args
    obj = objdir / f"{src.stem}-{target_name}.o"
    return src, run([*base_cmd, "-o", str(obj), str(src)])[0]


def _check_c_compiles(c_files: list[Path], portable: list[str]) -> list[str]:
    """Compile every portable C file across host, riscv32, and thumbv7em targets."""
    if not c_files:
        return []
    incs = collect_inc_dirs(portable)
    objdir = Path(tempfile.gettempdir()) / "ra8-gate-objs"
    objdir.mkdir(parents=True, exist_ok=True)
    findings: list[str] = []
    for name, opts in TARGETS.items():
        # A throwaway object instead of -fsyntax-only: this zig's
        # -fsyntax-only still emits an object and then fails its cache
        # rename, so plain -c is the honest check.
        cmd = [ZIG, "cc", "-c", "-std=c2x", "-include", "stdbool.h"]
        target = opts.get("target")
        if target:
            cmd += ["-target", str(target)]
        if opts.get("mcpu"):
            cmd += ["-mcpu=" + str(opts["mcpu"])]
        cmd += [f"-I{d}" for d in incs]
        if opts.get("stubs"):
            cmd += [f"-I{STUB_DIR}"]
        bad = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            work = [(f, cmd, objdir, name) for f in c_files]
            results = list(pool.map(_compile_c_file, work))
        for f, code in results:
            if code != 0:
                findings.append(f"{f}: zig cc compile {name} failed (exit {code})")
                bad += 1
        print(f"gate: c compile {name}: {len(c_files) - bad}/{len(c_files)} clean")
    return findings


def _scan_portable_libs(
    portable: list[str], nonportable: list[str], repo: Path = REPO
) -> tuple[list[str], list[Path], list[Path], list[str]]:
    """Walk portable library directories and collect edge findings and sources."""
    findings: list[str] = []
    zig_files: list[Path] = []
    c_files: list[Path] = []
    build_zig_libs: list[str] = []
    for lib in portable:
        d = repo / "libs" / lib
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
    return findings, zig_files, c_files, build_zig_libs


def main() -> int:
    """Run the portability gate (or self-test) and return an exit status."""
    ap = argparse.ArgumentParser(description="RA8FW-618 slice 2 portability gate")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument(
        "--image-builds",
        nargs="*",
        metavar="LIB",
        default=None,
        help=(
            "also build the named portable libraries' real freestanding images "
            "(default: every portable library with a build.zig); slow"
        ),
    )
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    table = parse_table(README.read_text(encoding="utf-8"))
    portable = sorted(n for n, c in table.items() if c == "portable")
    nonportable = sorted(set(table) - set(portable))
    print(f"gate: {len(table)} rows; {len(portable)} portable, {len(nonportable)} non-portable")

    findings, zig_files, c_files, build_zig_libs = _scan_portable_libs(portable, nonportable)
    print(
        f"gate: edge scan {len(portable)} portable libs "
        f"({len(zig_files)} zig, {len(c_files)} c files)"
    )

    findings.extend(_check_zig_ast(zig_files))
    findings.extend(_check_c_compiles(c_files, portable))

    print(f"gate: {len(build_zig_libs)} portable libs carry a build.zig")
    if args.image_builds is not None:
        findings.extend(image_build_phase(args.image_builds or build_zig_libs, build_zig_libs))

    if findings:
        print(f"gate: FAIL, {len(findings)} finding(s):")
        for f in findings[:_FINDINGS_PREVIEW_LIMIT]:
            print(f"  {f}")
        if len(findings) > _FINDINGS_PREVIEW_LIMIT:
            print(f"  ... and {len(findings) - _FINDINGS_PREVIEW_LIMIT} more")
        return 1
    print("gate: PASS, portable rows hold their edges")
    return 0


def self_test() -> int:
    """Build a throwaway repo skeleton; catch a violation and pass a clean lib."""
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
        (tmp / "libs" / "README.md").write_text(table, encoding="utf-8")
        bad = tmp / "libs" / "ra8_fix" / "src"
        bad.mkdir(parents=True)
        (bad / "bad.c").write_text('#include "ra8_hal/x.h"\n#include "cmsis.h"\n', encoding="utf-8")
        (bad / "bad.zig").write_text('const h = @import("ra8_hal");\n', encoding="utf-8")
        clean = tmp / "libs" / "ra8_clean" / "src"
        clean.mkdir(parents=True)
        (clean / "ok.zig").write_text(
            'const l = @import("./ok.zig");\npub const x = l.x;\n', encoding="utf-8"
        )

        readme = tmp / "libs" / "README.md"
        code_bad = gate_silent(tmp, readme)
        (bad / "bad.c").unlink()
        (bad / "bad.zig").unlink()
        code_clean = gate_silent(tmp, readme)

        if code_bad != 1:
            print(f"self-test: FAIL, expected the fixture to fail (exit 1), got {code_bad}")
            return 2
        if code_clean != 0:
            print(f"self-test: FAIL, expected the clean lib to pass (exit 0), got {code_clean}")
            return 2
        print("self-test: PASS, gate catches the violation and passes the clean lib")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


def gate_silent(repo: Path = REPO, readme: Path = README) -> int:
    """Run main()'s checks with output discarded; used by the self-test."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        return real_gate(repo, readme)


def real_gate(repo: Path = REPO, readme: Path = README) -> int:
    """Scan a portability table and source tree for illegal edges."""
    table = parse_table(readme.read_text(encoding="utf-8"))
    portable = sorted(n for n, c in table.items() if c == "portable")
    nonportable = sorted(set(table) - set(portable))
    findings, _, _, _ = _scan_portable_libs(portable, nonportable, repo=repo)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
