#!/usr/bin/env python3
"""Check that a Zig library's overridable default lives in its own archive member.

A Zig static library compiles its whole module graph into a SINGLE object file,
so splitting a definition into another `.zig` file does not split it into
another archive member. Only an explicit `b.addObject()` in the library's
`build.zig` does that.

That matters for a symbol the firmware image (or a host test) defines strongly
to override the library's default. The override only works because the linker
never pulls the member holding the default: the caller's member leaves the
symbol undefined, so the call binds to the strong definition. Emit the default
beside its caller instead and the call resolves locally, the member is never
needed, and the override silently stops being reached. Nothing fails to build
and nothing warns.

`libs/ra8_ota/src/reset_hook.zig` is the worked example: `ra8_ota_commit_and_reboot`
calls `ra8_ota_system_reset_hook`, `tests/misc/src/test_ra8_ota.c` defines it
strongly, and `build.zig` splits the default into its own object.

This check flags any symbol a migrated library exports that first-party code
outside that library also defines, unless the library's `build.zig` splits the
exporting file into its own object.

Per-library Zig test fixtures under `libs/<lib>/tests/` are excluded: they link
their own test binary, not the firmware archive, so a duplicate definition
there is not an override.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

EXPORT_FN = re.compile(r"^[ \t]*(?:pub[ \t]+)?export[ \t]+fn[ \t]+(\w+)", re.MULTILINE)
C_DEF = re.compile(
    r"^[ \t]*(?:void|int|bool|uint8_t|uint16_t|uint32_t|int32_t|size_t)"
    r"[ \t]+(\w+)[ \t]*\([^;]*$",
    re.MULTILINE,
)
B_PATH = re.compile(r"""b\.path\(\s*["']([^"']+)["']\s*\)""")
SCAN_ROOTS = ("apps", "examples", "tools", "tests")
SKIP_PARTS = ("third_party", ".zig-cache", ".git")


def skipped(path: Path) -> bool:
    """Whether a path is vendored, generated, or otherwise not ours to check."""
    return any(part in str(path) for part in SKIP_PARTS)


def balanced_span(text: str, open_at: int) -> str:
    """Return the text of the parenthesised call starting at `open_at`."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_at : i + 1]
    return text[open_at:]


def split_objects(build_zig: Path) -> tuple[set[str], bool]:
    """Return files `build.zig` gives their own object, and whether all were read.

    An `addObject` whose module comes from a variable has no path literal to
    read, so the library is reported unresolved and reviewed by hand rather
    than assumed safe.
    """
    text = build_zig.read_text(errors="replace")
    paths: set[str] = set()
    resolved = True
    for match in re.finditer(r"\bb\.addObject\s*\(", text):
        call = balanced_span(text, match.end() - 1)
        found = B_PATH.findall(call)
        if found:
            paths.update(found)
        else:
            resolved = False
    return paths, resolved


def migrated_libraries(root: Path) -> dict[str, Path]:
    """Every library carrying a `build.zig`, keyed by library name."""
    return {p.parent.name: p.parent for p in sorted(root.glob("libs/*/build.zig"))}


def exported_symbols(lib_dir: Path) -> dict[str, Path]:
    """Every symbol the library exports, mapped to the file exporting it."""
    found: dict[str, Path] = {}
    src = lib_dir / "src"
    if not src.exists():
        return found
    for f in sorted(src.rglob("*.zig")):
        if skipped(f):
            continue
        for sym in EXPORT_FN.findall(f.read_text(errors="replace")):
            found.setdefault(sym, f)
    return found


def defined_symbols(path: Path) -> set[str]:
    """Symbols this file defines: Zig exports, or C function definitions."""
    text = path.read_text(errors="replace")
    if path.suffix == ".zig":
        return set(EXPORT_FN.findall(text))
    return set(C_DEF.findall(text))


def candidate_files(root: Path, libs: dict[str, Path]) -> list[Path]:
    """First-party sources that could define an override, excluding lib tests."""
    files: list[Path] = []
    for name in SCAN_ROOTS:
        base = root / name
        if base.exists():
            files.extend(p for p in base.rglob("*.c") if not skipped(p))
            files.extend(p for p in base.rglob("*.zig") if not skipped(p))
    for lib_dir in libs.values():
        src = lib_dir / "src"
        if src.exists():
            files.extend(p for p in src.rglob("*.zig") if not skipped(p))
    return sorted(set(files))


def check_tree(root: Path) -> list[str]:
    """Scan the tree and return one line per unreachable overridable default."""
    libs = migrated_libraries(root)
    exports: dict[str, tuple[str, Path]] = {}
    for lib, lib_dir in libs.items():
        for sym, src_file in exported_symbols(lib_dir).items():
            exports.setdefault(sym, (lib, src_file))

    findings: list[str] = []
    unresolved = {
        lib for lib, lib_dir in libs.items() if not split_objects(lib_dir / "build.zig")[1]
    }

    for path in candidate_files(root, libs):
        rel = path.relative_to(root).as_posix()
        for sym in defined_symbols(path):
            owner = exports.get(sym)
            if owner is None:
                continue
            lib, src_file = owner
            src_rel = src_file.relative_to(root).as_posix()
            if rel == src_rel or rel.startswith(f"libs/{lib}/"):
                continue
            paths, _ = split_objects(lib_dir_of(libs, lib) / "build.zig")
            exporting = src_rel[len(f"libs/{lib}/") :]
            if exporting in paths:
                continue
            if lib in unresolved:
                findings.append(
                    f"{src_rel}: exports `{sym}`, which {rel} also defines, and "
                    f"libs/{lib}/build.zig builds objects from a module variable "
                    f"this check cannot read. Confirm by hand that `{exporting}` "
                    f"is its own archive member"
                )
                continue
            findings.append(
                f"{src_rel}: exports `{sym}`, which {rel} also defines. The "
                f"override cannot be reached: add an addObject() in "
                f"libs/{lib}/build.zig so `{exporting}` is its own archive member"
            )
    return findings


def lib_dir_of(libs: dict[str, Path], lib: str) -> Path:
    """Directory of a migrated library by name."""
    return libs[lib]


CASES: tuple[tuple[str, str, str, str, bool], ...] = (
    (
        "split default is reachable",
        "export fn hook() callconv(.c) void {}",
        'const o = b.addObject(.{ .root_source_file = b.path("src/hook.zig") });',
        "void hook(void)\n{\n}\n",
        False,
    ),
    (
        "unsplit default shadows the override",
        "export fn hook() callconv(.c) void {}",
        'const l = b.addStaticLibrary(.{ .root_source_file = b.path("src/hook.zig") });',
        "void hook(void)\n{\n}\n",
        True,
    ),
    (
        "no outside definition is not a finding",
        "export fn hook() callconv(.c) void {}",
        'const l = b.addStaticLibrary(.{ .root_source_file = b.path("src/hook.zig") });',
        "void unrelated(void)\n{\n}\n",
        False,
    ),
)


def run_selftest() -> int:
    """Run the built-in cases over synthetic trees; return a process exit code."""
    failures = 0
    for name, zig, build, consumer, expect in CASES:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            lib = root / "libs" / "demo"
            (lib / "src").mkdir(parents=True)
            (lib / "src" / "hook.zig").write_text(zig)
            (lib / "build.zig").write_text(build)
            app = root / "apps" / "demo"
            app.mkdir(parents=True)
            (app / "main.c").write_text(consumer)
            got = bool(check_tree(root))
            status = "ok" if got == expect else "FAIL"
            if got != expect:
                failures += 1
            print(f"  [{status}] {name}: findings={got} expected={expect}")
    print(f"selftest: {len(CASES) - failures}/{len(CASES)} passed")
    return 1 if failures else 0


def main() -> int:
    """Entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root to scan")
    parser.add_argument("--selftest", action="store_true", help="run built-in cases")
    args = parser.parse_args()

    if args.selftest:
        return run_selftest()

    findings = check_tree(Path(args.root).resolve())
    for line in findings:
        print(f"error: {line}")
    if findings:
        print(f"\n{len(findings)} unreachable-override finding(s)")
        return 1
    print("no unreachable overridable defaults")
    return 0


if __name__ == "__main__":
    sys.exit(main())
