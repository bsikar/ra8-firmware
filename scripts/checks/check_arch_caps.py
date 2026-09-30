#!/usr/bin/env python3
"""A capability flag must be backed by something, and a cleared one must say why.

`arch/arch.h` gates whole declaration blocks on the capability flags a core
answers in `arch/core/<core>/caps.h`, and it tells the reader outright that
"silence is not a third option: the port-completeness gate (epic invariant #4)
fails a capability flag that is set with no backend translation unit behind it,
and fails a cleared flag with no documented decline". Both `caps.h` files repeat
the promise. Nothing enforced it, so this is that gate (#694).

Four questions, asked mechanically, derived from the contract rather than from a
list kept here:

  - Is every flag the contract GATES a block on answered by every core? A flag
    the contract branches on and a core never mentions compiles as absent, which
    is a capability declined by accident.
  - Does a set flag have an implementation? Either a backend translation unit
    under `arch/<isa>/` defining one of the functions that flag's own block
    declares, or, until the migration lands, a MIGRATION note naming the path
    that implements it today. The named path has to be tracked, so when the
    implementation moves the note cannot keep pointing at nothing.
  - Does a cleared flag carry DECLINED and a reason in its own comment block?
  - Are the companion constants the contract references inside a gated block
    answered when that flag is set, and absent when it is cleared? Declaring
    ARCH_CACHE_LINE_BYTES for a core with no cache states a granularity for
    maintenance calls that are not declared for it.

The MIGRATION notes retire themselves: once a backend defines the symbols, the
note is stale and reported, so the pre-migration map cannot outlive the move.

Usage:
  scripts/checks/check_arch_caps.py [--selftest]

Exit codes: 0 clean, 1 findings, 2 usage/internal error.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# A read that finds fewer than this collapsed; it did not find a tidier tree.
CORE_FLOOR = 2
GATING_FLAG_FLOOR = 3

CONTRACT_REL = "arch/arch.h"
CORE_DIR_REL = "arch/core"
# Directories under arch/ that are not ISA backends.
NON_BACKEND_DIRS = frozenset({"core", "hosted"})

DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(ARCH_[A-Z0-9_]+)[ \t]+(.+?)[ \t]*$")
GATE_OPEN_RE = re.compile(r"^[ \t]*#[ \t]*if[ \t]+(ARCH_HAS_[A-Z0-9_]+)[ \t]*$")
ANY_IF_RE = re.compile(r"^[ \t]*#[ \t]*if")
ANY_ENDIF_RE = re.compile(r"^[ \t]*#[ \t]*endif")
DOC_REF_RE = re.compile(r"::(ARCH_[A-Z0-9_]+)")
FUNCTION_RE = re.compile(r"^[A-Za-z_][\w \t*]*?\b(arch_[a-z0-9_]+)[ \t]*\(", re.MULTILINE)
DEFINITION_RE = re.compile(r"^[A-Za-z_][\w \t*]*?\b(arch_[a-z0-9_]+)[ \t]*\([^;]*$", re.MULTILINE)
MIGRATION_RE = re.compile(r"MIGRATION:.*?`([^`]+)`", re.DOTALL)
DECLINE_RE = re.compile(r"\bDECLINED\b")
FLAG_VALUE_RE = re.compile(r"^\(?([01])U?\)?$")
COMMENT_LINE_RE = re.compile(r"^[ \t]*(/\*|\*|\*/)")


class CheckError(RuntimeError):
    """A read that cannot be trusted, as distinct from a finding."""


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode != 0:
        raise CheckError("not inside a git work tree")
    return Path(out.stdout.strip())


def tracked_paths(root: Path) -> set[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files"],
        capture_output=True,
        text=True,
        check=False,
    )
    if out.returncode != 0:
        raise CheckError("git ls-files failed")
    return {line for line in out.stdout.splitlines() if line}


def gated_blocks(text: str) -> dict[str, str]:
    """Return each ``#if ARCH_HAS_X`` block body, keyed by the flag it gates."""
    lines = text.splitlines()
    blocks: dict[str, list[str]] = {}
    open_gates: list[tuple[str | None, int]] = []
    for line in lines:
        match = GATE_OPEN_RE.match(line)
        if match:
            open_gates.append((match.group(1), 0))
            blocks.setdefault(match.group(1), [])
            continue
        if ANY_IF_RE.match(line):
            open_gates.append((None, 0))
            continue
        if ANY_ENDIF_RE.match(line):
            if open_gates:
                open_gates.pop()
            continue
        for flag, _ in open_gates:
            if flag is not None:
                blocks[flag].append(line)
    return {flag: "\n".join(body) for flag, body in blocks.items()}


def contract_capabilities(text: str) -> tuple[dict[str, dict[str, set[str]]], set[str]]:
    """Derive the gated flags with their companions and functions, plus every
    flag the contract mentions at all."""
    blocks = gated_blocks(text)
    gated: dict[str, dict[str, set[str]]] = {}
    for flag, body in blocks.items():
        refs = set(DOC_REF_RE.findall(body))
        gated[flag] = {
            "companions": {r for r in refs if not r.startswith("ARCH_HAS_")},
            "functions": set(FUNCTION_RE.findall(body)),
        }
    mentioned = {r for r in DOC_REF_RE.findall(text) if r.startswith("ARCH_HAS_")}
    return gated, mentioned | set(gated)


def comment_block(lines: list[str], index: int) -> str:
    """The contiguous comment lines immediately above ``index``."""
    start = index
    while start > 0 and COMMENT_LINE_RE.match(lines[start - 1]):
        start -= 1
    return "\n".join(lines[start:index])


def parse_caps(text: str) -> dict[str, dict[str, str]]:
    """Every ``ARCH_*`` define with its raw value and its own comment block."""
    lines = text.splitlines()
    answers: dict[str, dict[str, str]] = {}
    for index, line in enumerate(lines):
        match = DEFINE_RE.match(line)
        if not match:
            continue
        name, raw = match.group(1), match.group(2).strip()
        answers[name] = {"value": raw, "doc": comment_block(lines, index)}
    return answers


def flag_state(raw: str) -> int | None:
    match = FLAG_VALUE_RE.match(raw)
    return int(match.group(1)) if match else None


def backend_definitions(files: dict[str, str]) -> dict[str, set[str]]:
    """Symbols each backend translation unit defines."""
    return {rel: set(DEFINITION_RE.findall(text)) for rel, text in files.items()}


def _check_set_flag(ctx: dict, flag: str, answer: dict) -> list[tuple[str, str, str]]:
    """A set flag owes a backend TU or a tracked migration home, not both."""
    rel, gated, backends, tracked = ctx["rel"], ctx["gated"], ctx["backends"], ctx["tracked"]
    wanted = gated[flag]["functions"]
    implementers = sorted(tu for tu, syms in backends.items() if syms & wanted)
    migration = MIGRATION_RE.search(answer["doc"])
    findings: list[tuple[str, str, str]] = []
    if implementers and migration:
        findings.append(
            (
                "stale-migration-note",
                rel,
                f"{flag} names a migration home but {implementers[0]} already "
                f"defines its symbols; drop the note",
            )
        )
        return findings
    if implementers:
        return findings
    if migration is None:
        wanted_text = ", ".join(sorted(wanted)) or "the block's symbols"
        findings.append(
            (
                "unbacked-capability",
                rel,
                f"{flag} is set and nothing implements it: no backend translation "
                f"unit under arch/ defines {wanted_text}, and the flag names no "
                f"MIGRATION home",
            )
        )
        return findings
    home = migration.group(1)
    if home not in tracked and not any(t.startswith(home) for t in tracked):
        findings.append(
            (
                "stale-migration-home",
                rel,
                f"{flag} names `{home}` as its implementation today; that path is "
                f"not tracked",
            )
        )
    return findings


def _check_companions(ctx: dict, flag: str, state: int) -> list[tuple[str, str, str]]:
    rel, gated, answers = ctx["rel"], ctx["gated"], ctx["answers"]
    findings: list[tuple[str, str, str]] = []
    for companion in sorted(gated[flag]["companions"]):
        present = companion in answers
        if state == 1 and not present:
            findings.append(
                (
                    "companion-missing",
                    rel,
                    f"{flag} is set and the contract reads {companion} inside its "
                    f"block, but this core does not answer it",
                )
            )
        if state == 0 and present:
            findings.append(
                (
                    "companion-orphaned",
                    rel,
                    f"{companion} is answered while {flag} is cleared, so it "
                    f"describes a capability this core declines",
                )
            )
    return findings


def check_core(rel: str, text: str, ctx: dict) -> list[tuple[str, str, str]]:
    """Every rule, for one core's caps.h."""
    answers = parse_caps(text)
    gated, known, backends = ctx["gated"], ctx["known"], ctx["backends"]
    scope = {
        "rel": rel,
        "gated": gated,
        "answers": answers,
        "backends": backends,
        "tracked": ctx["tracked"],
    }
    findings: list[tuple[str, str, str]] = []
    for name in sorted(answers):
        if name.startswith("ARCH_HAS_") and name not in known:
            findings.append(
                (
                    "unknown-capability",
                    rel,
                    f"{name} is answered here and {CONTRACT_REL} never mentions it",
                )
            )
    for flag in sorted(gated):
        if flag not in answers:
            findings.append(
                (
                    "unanswered-capability",
                    rel,
                    f"{CONTRACT_REL} gates a declaration block on {flag} and this "
                    f"core does not answer it, so the block compiles as absent",
                )
            )
            continue
        state = flag_state(answers[flag]["value"])
        if state is None:
            findings.append(
                (
                    "unreadable-capability",
                    rel,
                    f"{flag} is answered {answers[flag]['value']}, which is neither "
                    f"set nor cleared",
                )
            )
            continue
        if state == 1:
            findings.extend(_check_set_flag(scope, flag, answers[flag]))
        elif not DECLINE_RE.search(answers[flag]["doc"]):
            findings.append(
                (
                    "undocumented-decline",
                    rel,
                    f"{flag} is cleared with no DECLINED note saying why, so the "
                    f"absence reads as an oversight",
                )
            )
        findings.extend(_check_companions(scope, flag, state))
    return findings


def analyse(
    contract: str,
    caps: dict[str, str],
    backend_files: dict[str, str],
    tracked: set[str],
) -> tuple[dict[str, int], list[tuple[str, str, str]]]:
    """Pure core: the whole check over already-read text."""
    gated, known = contract_capabilities(contract)
    ctx = {
        "gated": gated,
        "known": known,
        "backends": backend_definitions(backend_files),
        "tracked": tracked,
    }
    findings: list[tuple[str, str, str]] = []
    for rel in sorted(caps):
        findings.extend(check_core(rel, caps[rel], ctx))
    counts = {"cores": len(caps), "gating_flags": len(gated), "backends": len(backend_files)}
    return counts, findings


def read_tree(root: Path) -> tuple[str, dict[str, str], dict[str, str]]:
    contract_path = root / CONTRACT_REL
    if not contract_path.is_file():
        raise CheckError(f"{CONTRACT_REL} is missing; the contract is the input")
    caps: dict[str, str] = {}
    core_root = root / CORE_DIR_REL
    for path in sorted(core_root.glob("*/caps.h")):
        caps[str(path.relative_to(root))] = path.read_text(encoding="utf-8")
    backends: dict[str, str] = {}
    arch_root = root / "arch"
    for path in sorted(arch_root.rglob("*")):
        if not path.is_file() or path.suffix not in {".c", ".S", ".s"}:
            continue
        top = path.relative_to(arch_root).parts[0]
        if top in NON_BACKEND_DIRS:
            continue
        backends[str(path.relative_to(root))] = path.read_text(encoding="utf-8")
    return contract_path.read_text(encoding="utf-8"), caps, backends


def main(argv: list[str]) -> int:
    if "--selftest" in argv[1:]:
        return selftest()
    try:
        root = repo_root()
        contract, caps, backends = read_tree(root)
        counts, findings = analyse(contract, caps, backends, tracked_paths(root))
    except CheckError as failure:
        print(f"{Path(__file__).name}: ERROR: {failure}", file=sys.stderr)
        return 2

    if counts["cores"] < CORE_FLOOR or counts["gating_flags"] < GATING_FLAG_FLOOR:
        print(
            f"{Path(__file__).name}: ERROR: read collapsed -- {counts['cores']} core(s) "
            f"and {counts['gating_flags']} gated capability flag(s), floors are "
            f"{CORE_FLOOR} and {GATING_FLAG_FLOOR}. Either {CORE_DIR_REL}/*/caps.h "
            f"moved or the contract stopped gating on its flags; fix the read rather "
            f"than the floor, because a collapsed read is also perfectly quiet.",
            file=sys.stderr,
        )
        return 2

    if findings:
        print(
            f"{Path(__file__).name}: {len(findings)} capability declaration(s) are "
            f"not backed by anything:",
            file=sys.stderr,
        )
        for kind, rel, detail in findings:
            print(f"  {rel}: {kind}: {detail}", file=sys.stderr)
        print(
            "\nA set flag owes an implementation: a backend translation unit under "
            "arch/<isa>/ defining the functions its block declares, or a MIGRATION "
            "note naming the tracked path that implements it today. A cleared flag "
            "owes DECLINED and a reason. Silence is what this gate exists to refuse.",
            file=sys.stderr,
        )
        return 1

    print(
        f"{Path(__file__).name}: clean -- {counts['gating_flags']} gated capability "
        f"flag(s) answered by {counts['cores']} core(s), {counts['backends']} backend "
        f"translation unit(s)"
    )
    return 0


_CONTRACT = """
#if ARCH_HAS_CACHE
/** @brief Line size is ::ARCH_CACHE_LINE_BYTES. */
void arch_cache_clean(uintptr_t base, size_t size);
void arch_cache_invalidate(uintptr_t base, size_t size);
#endif /* ARCH_HAS_CACHE */

#if ARCH_HAS_MEM_PROTECT
/** @brief Slot below ::ARCH_MEM_PROTECT_REGIONS. */
bool arch_mem_protect_region_set(uint8_t index);
#endif /* ARCH_HAS_MEM_PROTECT */

#if ARCH_HAS_TRUSTZONE_M
bool arch_trustzone_region_set(uint8_t index);
#endif /* ARCH_HAS_TRUSTZONE_M */

/* ::ARCH_HAS_SIMD is described but gates nothing here. */
"""


def _caps(cache: str = "(1)", doc: str = "", extra: str = "") -> str:
    return f"""
/** @brief Cache line size. */
#define ARCH_CACHE_LINE_BYTES (32U)
{doc}
#define ARCH_HAS_CACHE {cache}
/** @brief MIGRATION: implemented today by `libs/mpu.c`. */
#define ARCH_HAS_MEM_PROTECT (1)
/** @brief Regions. */
#define ARCH_MEM_PROTECT_REGIONS (16U)
/** @brief MIGRATION: implemented today by `libs/sau.c`. */
#define ARCH_HAS_TRUSTZONE_M (1)
{extra}
"""


_TRACKED = {"libs/mpu.c", "libs/sau.c", "libs/cache.c"}
_MIGRATION_DOC = "/** @brief MIGRATION: implemented today by `libs/cache.c`. */"


def _kinds(caps_text: str, backends: dict[str, str] | None = None) -> list[str]:
    _, findings = analyse(_CONTRACT, {"caps.h": caps_text}, backends or {}, _TRACKED)
    return sorted(kind for kind, _, _ in findings)


def _selftest_cases() -> list[tuple[str, list[str]]]:
    """Each case: the kinds it must produce, in both directions."""
    backend = {"arch/armv8m/cache.c": "void arch_cache_clean(uintptr_t b, size_t s)\n{\n}\n"}
    unrelated = {"arch/armv8m/boot.c": "void arch_cpu_idle(void)\n{\n}\n"}
    return [
        ("clean with a tracked migration home", _kinds(_caps(doc=_MIGRATION_DOC))),
        ("set flag with no home and no backend", _kinds(_caps(doc="/** @brief On. */"))),
        (
            "migration home that is not tracked",
            _kinds(_caps(doc="/** @brief MIGRATION: `libs/gone.c`. */")),
        ),
        ("cleared flag with no decline note", _kinds(_caps(cache="(0)", doc="/** x */"))),
        (
            "cleared flag with a decline note",
            _kinds(
                _caps(cache="(0)", doc="/** DECLINED: no cache. */").replace(
                    "#define ARCH_CACHE_LINE_BYTES (32U)", ""
                )
            ),
        ),
        (
            "backend definition satisfies a set flag",
            _kinds(_caps(doc="/** @brief On. */"), backend),
        ),
        (
            "backend defining something else does not",
            _kinds(_caps(doc="/** @brief On. */"), unrelated),
        ),
        (
            "migration note left behind after the backend landed",
            _kinds(_caps(doc=_MIGRATION_DOC), backend),
        ),
        (
            "gated flag this core never answers",
            _kinds(
                _caps(doc=_MIGRATION_DOC).replace("#define ARCH_HAS_TRUSTZONE_M (1)", "")
            ),
        ),
        (
            "flag the contract never mentions",
            _kinds(_caps(doc=_MIGRATION_DOC, extra="#define ARCH_HAS_INVENTED (1)")),
        ),
        (
            "companion the contract reads, unanswered",
            _kinds(
                _caps(doc=_MIGRATION_DOC).replace(
                    "#define ARCH_MEM_PROTECT_REGIONS (16U)", ""
                )
            ),
        ),
        (
            "companion answered for a declined flag",
            _kinds(_caps(cache="(0)", doc="/** DECLINED: none. */")),
        ),
        (
            "flag answered with neither 1 nor 0",
            _kinds(_caps(cache="(TBD)", doc=_MIGRATION_DOC)),
        ),
    ]


_EXPECTED = [
    [],
    ["unbacked-capability"],
    ["stale-migration-home"],
    # A cleared cache flag leaves ARCH_CACHE_LINE_BYTES describing a capability
    # this core declines, so the honest read reports both.
    ["companion-orphaned", "undocumented-decline"],
    [],
    [],
    ["unbacked-capability"],
    ["stale-migration-note"],
    ["unanswered-capability"],
    ["unknown-capability"],
    ["companion-missing"],
    ["companion-orphaned"],
    ["unreadable-capability"],
]


def selftest() -> int:
    failures: list[str] = []
    cases = _selftest_cases()
    for (label, actual), expected in zip(cases, _EXPECTED):
        if actual != expected:
            failures.append(f"{label}: expected {expected}, got {actual}")

    counts, _ = analyse(_CONTRACT, {"caps.h": _caps(doc=_MIGRATION_DOC)}, {}, _TRACKED)
    if counts["gating_flags"] != 3:
        failures.append(f"contract read found {counts['gating_flags']} gated flags, want 3")
    gated, known = contract_capabilities(_CONTRACT)
    if "ARCH_HAS_SIMD" not in known or "ARCH_HAS_SIMD" in gated:
        failures.append("a described-but-ungated flag must be known and not gated")
    if gated["ARCH_HAS_CACHE"]["functions"] != {"arch_cache_clean", "arch_cache_invalidate"}:
        failures.append("the cache block's declared functions were misread")

    if failures:
        for failure in failures:
            print(f"selftest: {Path(__file__).name} FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"selftest: {Path(__file__).name} OK ({len(cases) + 3} both-direction cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
