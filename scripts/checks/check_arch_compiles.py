#!/usr/bin/env python3
"""Compile the arch tier contract, for every core, in every capability state.

`arch/arch.h` is the contract every `arch/<isa>/` backend implements (#694). It
is a header with no backend behind it yet, and until this gate existed NOTHING
in the tree compiled it: not a library, not a test, not a tool. A header nobody
compiles rots exactly the way an unmeasured number rots, and it had:

  - `::K_ARCH_FAULT_RAW_MAX` referenced in the documentation of
    ::arch_fault_info_t and defined nowhere, with `raw[8]` written as a literal;
  - `bool` used in five declarations with no `<stdbool.h>`. That one is latent
    rather than live -- `bool` is a keyword in the C23 the tree pins -- but a
    contract header should not depend on the standard to supply its types by
    accident.

Neither is visible to a text checker. `scripts/checks/check_arch_caps.py` is the
port-completeness gate and reads the capability ANSWERS out of caps.h; this gate
is its compiler-side twin and checks what only a compiler can:

  1. the contract PARSES against each real core's `caps.h`, at the C standard
     the tree pins, with warnings promoted to errors;
  2. the `static_assert`s in the contract hold for that core's capability
     VALUES (a priority-bit width outside 1..8, say);
  3. every capability-gated block is valid in BOTH states. The two real cores
     between them never exercise `ARCH_HAS_MEM_PROTECT (0)`,
     `ARCH_HAS_RTOS_CONTEXT (0)` or `ARCH_HAS_TRUSTZONE_M (0)`, so a syntax
     error inside one of those `#if` blocks would sit undiscovered until the
     first backend that declines the capability. Two synthetic cores, all
     optional capabilities off and all on, close that hole;
  4. the contract stays FREESTANDING. A bare-metal target has no `<assert.h>`;
     the include list is checked against the C23 freestanding set rather than by
     cross-compiling, so the gate needs no target toolchain to enforce it.

Everything is DERIVED from `arch/arch.h` and the real caps files: the gated
capability names come from the `#if ARCH_HAS_*` lines in the contract, and the
companion values the synthetic cores need come from whichever real core defines
them. Adding a new optional capability to the contract therefore extends this
gate automatically, with no list here to keep in step.

Usage:
    check_arch_compiles.py [--root DIR] [--cc COMPILER]
    check_arch_compiles.py --selftest
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ARCH_REL = "arch/arch.h"
CORE_GLOB = "arch/core/*/caps.h"

# The tree pins CMAKE_C_STANDARD 23 and the contract's fixed-underlying-type
# enum requires it, so there is one standard to check, not a range.
C_STANDARD = "c23"

# A core count below this means discovery broke, which otherwise reads exactly
# like a tree in which every core passes.
CORE_FLOOR = 2

# C23 freestanding headers (N3220 4.6p3), plus the ones a freestanding
# implementation is additionally required to provide. A contract header that
# reaches outside this set cannot be included by a bare-metal backend.
FREESTANDING_HEADERS = frozenset(
    {
        "float.h",
        "iso646.h",
        "limits.h",
        "stdalign.h",
        "stdarg.h",
        "stdbit.h",
        "stdbool.h",
        "stbool.h",
        "stdckdint.h",
        "stddef.h",
        "stdint.h",
        "stdnoreturn.h",
    }
)

WARNING_FLAGS = (
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    # -Wundef is the one that matters most here: a capability flag is consumed
    # with `#if ARCH_HAS_X`, and a misspelled name silently evaluates to 0,
    # which reads as a clean decline rather than as the typo it is.
    "-Wundef",
    "-Wconversion",
    "-Werror",
)

SYSTEM_INCLUDE_RE = re.compile(r"^\s*#\s*include\s*<([^>]+)>", re.MULTILINE)
LOCAL_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
GATED_CAP_RE = re.compile(r"^\s*#\s*if\s+(ARCH_HAS_[A-Z0-9_]+)\s*$", re.MULTILINE)
DEFINE_RE = re.compile(r"^\s*#\s*define\s+(ARCH_[A-Z0-9_]+)\s+(.+?)\s*$", re.MULTILINE)


class CheckError(RuntimeError):
    """A problem with the check itself, not with the tree it is checking."""


@dataclass(frozen=True)
class Finding:
    where: str
    kind: str
    detail: str

    def render(self) -> str:
        return f"{self.where}: {self.kind}: {self.detail}"


def find_compiler(explicit: str | None) -> list[str]:
    """Resolve a C compiler, preferring an explicit one, then $CC, then PATH."""
    candidates: list[list[str]] = []
    if explicit:
        candidates.append(explicit.split())
    env_cc = os.environ.get("CC", "").strip()
    if env_cc:
        candidates.append(env_cc.split())
    for name in ("cc", "gcc", "clang"):
        candidates.append([name])
    # zig ships a complete C frontend; it is what makes this gate runnable in a
    # container that has no system compiler.
    candidates.append(["zig", "cc"])

    for candidate in candidates:
        if shutil.which(candidate[0]):
            return candidate
    raise CheckError(
        "no C compiler found; tried --cc, $CC, cc, gcc, clang and zig cc. "
        "This gate compiles the arch contract and cannot run without one."
    )


def discover_cores(root: Path) -> list[Path]:
    """Every real core directory that answers the capability contract."""
    return sorted(root.glob(CORE_GLOB))


def gated_capabilities(arch_text: str) -> list[str]:
    """The capability flags that gate a declaration block in the contract."""
    seen: list[str] = []
    for name in GATED_CAP_RE.findall(arch_text):
        if name not in seen:
            seen.append(name)
    return seen


def capability_values(core_files: list[Path]) -> dict[str, str]:
    """Union of every ARCH_* define across the real cores, with one real value.

    The synthetic cores need a plausible value for the companion constants a set
    capability brings with it (region counts, a cache line size, a flavour
    string). Copying them from a real core keeps this script free of a second
    list that could drift from `caps.h`.
    """
    values: dict[str, str] = {}
    for path in core_files:
        for name, value in DEFINE_RE.findall(path.read_text(encoding="utf-8")):
            values.setdefault(name, value)
    return values


def synthetic_caps(state: bool, gated: list[str], values: dict[str, str]) -> str:
    """A caps.h for a core that answers every optional capability `state`.

    Flags keep their name and get 0 or 1; the companion constants keep whatever
    a real core uses, because their value is irrelevant to whether the gated
    block parses and a wrong one would only produce noise.
    """
    answer = "1" if state else "0"
    lines = [
        "#pragma once",
        "/* Generated by scripts/checks/check_arch_compiles.py -- not a real core. */",
        f'#define ARCH_CORE_NAME "synthetic-{"on" if state else "off"}"',
        '#define ARCH_ISA_NAME "synthetic"',
        "#define ARCH_IRQ_PRIORITY_BITS (4U)",
    ]
    emitted = {"ARCH_CORE_NAME", "ARCH_ISA_NAME", "ARCH_IRQ_PRIORITY_BITS"}
    for name in sorted(set(values) | set(gated)):
        if name in emitted:
            continue
        if name.startswith("ARCH_HAS_"):
            lines.append(f"#define {name} ({answer})")
        else:
            lines.append(f"#define {name} {values[name]}")
        emitted.add(name)
    return "\n".join(lines) + "\n"


def compile_against(
    cc: list[str], root: Path, caps_dir: Path, workdir: Path, label: str
) -> Finding | None:
    """Compile a translation unit whose only content is the contract header."""
    unit = workdir / f"probe_{label}.c"
    unit.write_text('#include "arch.h"\n', encoding="utf-8")
    cmd = [
        *cc,
        f"-std={C_STANDARD}",
        *WARNING_FLAGS,
        "-c",
        str(unit),
        "-o",
        str(workdir / f"probe_{label}.o"),
        "-I",
        str(root / "arch"),
        "-I",
        str(caps_dir),
    ]
    done = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if done.returncode == 0:
        return None
    diagnostic = (done.stderr or done.stdout).strip()
    # The probe file's own path is a temporary directory; strip it so the
    # message names the header the reader can actually open.
    diagnostic = diagnostic.replace(str(workdir) + "/", "")
    first = "\n    ".join(diagnostic.splitlines()[:12])
    return Finding(
        where=f"{ARCH_REL} against {label}",
        kind="contract-does-not-compile",
        detail=f"{' '.join(cc)} -std={C_STANDARD} rejected it:\n    {first}",
    )


def check_freestanding(root: Path, arch_text: str) -> list[Finding]:
    """The contract may only reach for headers a bare-metal target ships."""
    findings: list[Finding] = []
    for header in sorted(set(SYSTEM_INCLUDE_RE.findall(arch_text))):
        if header not in FREESTANDING_HEADERS:
            findings.append(
                Finding(
                    where=ARCH_REL,
                    kind="nonfreestanding-include",
                    detail=(
                        f"<{header}> is not in the C23 freestanding set, so a bare-metal "
                        f"backend cannot include this contract. Use a freestanding "
                        f"equivalent ({', '.join(sorted(FREESTANDING_HEADERS)[:4])}, ...) "
                        f"or a language keyword."
                    ),
                )
            )
    for header in sorted(set(LOCAL_INCLUDE_RE.findall(arch_text))):
        if header != "caps.h":
            findings.append(
                Finding(
                    where=ARCH_REL,
                    kind="contract-reaches-upward",
                    detail=(
                        f'"{header}" is not part of the arch tier. The contract is the '
                        f"lowest tier and may include only the selected core's caps.h."
                    ),
                )
            )
    return findings


def run(root: Path, cc: list[str]) -> list[Finding]:
    arch_path = root / ARCH_REL
    if not arch_path.is_file():
        raise CheckError(f"{ARCH_REL} is missing; the arch tier contract is the thing this gate checks")
    arch_text = arch_path.read_text(encoding="utf-8")

    findings = check_freestanding(root, arch_text)

    cores = discover_cores(root)
    if len(cores) < CORE_FLOOR:
        raise CheckError(
            f"discovered {len(cores)} core(s) under {CORE_GLOB}, floor is {CORE_FLOOR}; "
            f"a discovery that stops finding cores reads exactly like a tree in which "
            f"every core compiles"
        )

    gated = gated_capabilities(arch_text)
    if not gated:
        raise CheckError(
            "no capability-gated block found in the contract; either the contract lost "
            "its optional surface or the '#if ARCH_HAS_*' grammar this gate reads has changed"
        )
    values = capability_values(cores)

    with tempfile.TemporaryDirectory() as tmp:
        workdir = Path(tmp)
        for caps in cores:
            label = caps.parent.name
            finding = compile_against(cc, root, caps.parent, workdir, label)
            if finding:
                findings.append(finding)

        # The states no real core covers.
        for state in (False, True):
            label = f"synthetic-{'on' if state else 'off'}"
            caps_dir = workdir / label
            caps_dir.mkdir(exist_ok=True)
            (caps_dir / "caps.h").write_text(
                synthetic_caps(state, gated, values), encoding="utf-8"
            )
            finding = compile_against(cc, root, caps_dir, workdir, label)
            if finding:
                findings.append(finding)

    return findings


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------

_GOOD_ARCH = """#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "caps.h"
static_assert(ARCH_IRQ_PRIORITY_BITS >= 1U && ARCH_IRQ_PRIORITY_BITS <= 8U, "range");
bool arch_irq_is_active(uint32_t irq);
#if ARCH_HAS_CACHE
void arch_cache_clean(uintptr_t base, uint32_t size);
#endif
#if ARCH_HAS_TRUSTZONE_M
bool arch_trustzone_region_set(uint8_t index, uintptr_t base);
#endif
"""

_CAPS_A = """#pragma once
#define ARCH_CORE_NAME "a"
#define ARCH_ISA_NAME "a"
#define ARCH_IRQ_PRIORITY_BITS (4U)
#define ARCH_HAS_CACHE (1)
#define ARCH_CACHE_LINE_BYTES (32U)
#define ARCH_HAS_TRUSTZONE_M (1)
"""

_CAPS_B = """#pragma once
#define ARCH_CORE_NAME "b"
#define ARCH_ISA_NAME "b"
#define ARCH_IRQ_PRIORITY_BITS (3U)
#define ARCH_HAS_CACHE (0)
#define ARCH_HAS_TRUSTZONE_M (1)
"""


def _plant(root: Path, arch: str = _GOOD_ARCH, caps_a: str = _CAPS_A, caps_b: str = _CAPS_B) -> None:
    (root / "arch" / "core" / "a").mkdir(parents=True, exist_ok=True)
    (root / "arch" / "core" / "b").mkdir(parents=True, exist_ok=True)
    (root / "arch" / "arch.h").write_text(arch, encoding="utf-8")
    (root / "arch" / "core" / "a" / "caps.h").write_text(caps_a, encoding="utf-8")
    (root / "arch" / "core" / "b" / "caps.h").write_text(caps_b, encoding="utf-8")


def selftest(cc: list[str]) -> int:
    cases: list[tuple[str, dict[str, str], str | None]] = [
        ("a clean contract passes on both cores", {}, None),
        (
            "a non-freestanding include is rejected",
            {"arch": _GOOD_ARCH.replace("#include <stdbool.h>", "#include <assert.h>\n#include <stdbool.h>")},
            "nonfreestanding-include",
        ),
        (
            "reaching up out of the arch tier is rejected",
            {"arch": _GOOD_ARCH.replace('#include "caps.h"', '#include "caps.h"\n#include "ra8_scb.h"')},
            "contract-reaches-upward",
        ),
        (
            # Note the asymmetry: dropping <stdbool.h> would NOT fail here,
            # because `bool` is a keyword in the C23 the tree pins. <stdint.h>
            # is the include the contract genuinely cannot do without.
            "a missing fixed-width-integer include fails to compile",
            {"arch": _GOOD_ARCH.replace("#include <stdint.h>\n", "")},
            "contract-does-not-compile",
        ),
        (
            "a caps value the contract asserts against fails the build",
            {"caps_a": _CAPS_A.replace("(4U)", "(9U)")},
            "contract-does-not-compile",
        ),
        (
            "a syntax error inside an enabled gated block is caught",
            {"arch": _GOOD_ARCH.replace("void arch_cache_clean(uintptr_t base, uint32_t size);", "void arch_cache_clean(")},
            "contract-does-not-compile",
        ),
        (
            "a syntax error inside a block NO real core enables is still caught",
            # Both planted cores set ARCH_HAS_TRUSTZONE_M, so only the
            # synthetic-off core can reach an #else-side mistake. Break the
            # block by leaving the #if unterminated for the disabled state.
            {
                "arch": _GOOD_ARCH.replace(
                    "#if ARCH_HAS_TRUSTZONE_M\nbool arch_trustzone_region_set(uint8_t index, uintptr_t base);\n#endif\n",
                    "#if ARCH_HAS_TRUSTZONE_M\nbool arch_trustzone_region_set(uint8_t index, uintptr_t base);\n#else\nvoid arch_trustzone_absent(\n#endif\n",
                )
            },
            "contract-does-not-compile",
        ),
        (
            "an unconditional narrowing conversion is caught by -Wconversion",
            {"arch": _GOOD_ARCH + "static inline uint8_t arch_narrow(uint32_t v) { uint8_t r = v; return r; }\n"},
            "contract-does-not-compile",
        ),
    ]

    failures = 0
    for name, overrides, expected in cases:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _plant(
                root,
                arch=overrides.get("arch", _GOOD_ARCH),
                caps_a=overrides.get("caps_a", _CAPS_A),
                caps_b=overrides.get("caps_b", _CAPS_B),
            )
            try:
                findings = run(root, cc)
            except CheckError as exc:
                print(f"  FAIL {name}: check aborted: {exc}")
                failures += 1
                continue
            kinds = {f.kind for f in findings}
            if expected is None and findings:
                print(f"  FAIL {name}: expected clean, got {sorted(kinds)}")
                failures += 1
            elif expected is not None and expected not in kinds:
                print(f"  FAIL {name}: expected {expected}, got {sorted(kinds) or 'clean'}")
                failures += 1

    # Discovery floor: one core must abort rather than read clean.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _plant(root)
        import shutil as _sh

        _sh.rmtree(root / "arch" / "core" / "b")
        try:
            run(root, cc)
        except CheckError:
            pass
        else:
            print("  FAIL a single discovered core must abort, not read clean")
            failures += 1

    total = len(cases) + 1
    if failures:
        print(f"selftest: {failures}/{total} case(s) FAILED")
        return 1
    print(f"selftest: check_arch_compiles.py OK ({total} cases)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--cc", default=None, help="C compiler to use (default: $CC, then cc/gcc/clang/zig cc)")
    parser.add_argument("--selftest", action="store_true", help="run the built-in cases and exit")
    args = parser.parse_args(argv)

    try:
        cc = find_compiler(args.cc)
        if args.selftest:
            return selftest(cc)
        root = Path(args.root).resolve()
        findings = run(root, cc)
    except CheckError as exc:
        print(f"arch-compiles: {exc}", file=sys.stderr)
        return 2

    if findings:
        for finding in findings:
            print(finding.render(), file=sys.stderr)
        print(
            f"arch-compiles: {len(findings)} problem(s) in the arch tier contract",
            file=sys.stderr,
        )
        return 1

    cores = [p.parent.name for p in discover_cores(Path(args.root).resolve())]
    print(
        f"arch-compiles: {ARCH_REL} compiles at -std={C_STANDARD} with warnings as errors "
        f"for {len(cores)} core(s) ({', '.join(cores)}) plus the all-off and all-on "
        f"synthetic cores"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
