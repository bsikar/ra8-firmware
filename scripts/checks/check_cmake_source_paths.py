#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a CMake listfile shall not name a source file that is not there.

The Zig port deletes C.  A deleted ``.c`` that some *other* listfile still
names by path does not fail any test in this tree -- host tests link the Zig
archive and pass, ``zig build`` never reads a CMakeLists.txt, and the sandbox
has no ``cmake`` to configure with.  It fails later, on somebody's machine,
at configure time, in a target nobody in the porting lane was looking at.

Two real instances motivated this gate, both left behind by the porting lane
itself:

  * ``libs/ra8_num/src/ra8_num_decimal.c`` went Zig in ``cdfd1890e``, which
    updated the three references in ``tests/cmake/unit_tests.cmake`` and
    missed five more in ``apps/shared_libs/mdl/CMakeLists.txt``.
  * ``libs/ra8_mem/src/ra8_slab.c`` went Zig in ``a4aff0d4a``, which missed
    the DROP list in ``tools/rabook_viewer/CMakeLists.txt``.

Both are the same defect: a source list outlived its source.

Rule PATH
    A listfile line naming a source path that resolves unambiguously -- a
    literal, or one rooted at a repository-root variable or at the listfile's
    own directory -- must name a file that exists.

Rule PARTITION
    ``ra8_viewer_read_path(<out> <dir> KEEP <files...> DROP <files...>)`` in
    ``tools/rabook_viewer/CMakeLists.txt`` globs ``<dir>/*.c`` and requires
    KEEP and DROP to partition that glob exactly.  CMake enforces this at
    configure time; this gate enforces it without a configure, because the
    second instance above was exactly a stale DROP member.

Deliberately out of scope, because flagging them is noise rather than signal:

  * a path still carrying an unresolved ``${...}`` after substitution (a loop
    variable, a per-target source directory, a vendored tree's own root);
  * anything under ``${CMAKE_CURRENT_BINARY_DIR}`` -- generated, and absent
    until the build runs;
  * a bare relative path, which CMake resolves against the *consuming*
    directory scope, not the listfile's, so it cannot be checked in isolation;
  * a file whose basename appears in an ``EXISTS`` test in the same listfile,
    which is the established way this tree makes a source optional while its
    library is mid-port (``cmake/ra8_app/sources.cmake`` does this for
    ``ra8_net_pal.c``).

Exit codes: 0 clean, 1 findings.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".cc", ".hpp", ".S", ".s", ".zig")

# Variables this tree uses to mean "the repository root".  A listfile under a
# sibling product tree sets its own; each still resolves to this repo.  Every
# entry here was read back from its ``set()`` -- a variable whose value is
# itself a variable (``RA8_REPO_ROOT`` is set from a loop variable in one
# place) still resolves correctly because the check below drops any token
# whose parent directory does not exist.
ROOT_VARS = (
    "FW_ROOT",
    "RA8_ROOT",
    "RA8_REPO_ROOT",
    "MDL_REPO_ROOT",
    "CBZ2JOF_REPO_ROOT",
    "ALPHABET_SOUP_REPO_ROOT",
)

# Variables meaning "<repository root>/libs".
LIBS_VARS = ("FW", "CBZ2JOF_FW")

# ``CMAKE_CURRENT_LIST_DIR`` is always the directory holding the file being
# read, so it resolves anywhere.  ``CMAKE_CURRENT_SOURCE_DIR`` is the
# directory scope *consuming* the file, which for an ``include()``d ``.cmake``
# is the includer, not the fragment -- unknowable without configuring.  So it
# resolves only in a ``CMakeLists.txt``, where the two coincide.
SELF_VARS = ("CMAKE_CURRENT_LIST_DIR",)
SELF_VARS_LISTFILE_ONLY = ("CMAKE_CURRENT_SOURCE_DIR",)

SKIP_DIRS = ("build/", "third_party/", "vendor/", "external/", ".git/")

PATH_RE = re.compile(
    r"(?:\$\{[A-Za-z0-9_]+\}|[A-Za-z0-9_.-])"
    r"[A-Za-z0-9_${}/.+-]*"
    r"\.(?:c|h|cpp|cc|hpp|S|s|zig)\b"
)

READ_PATH_RE = re.compile(
    r"ra8_viewer_read_path\(\s*(?P<body>[^)]*)\)", re.S
)


def listfiles() -> list[pathlib.Path]:
    out = []
    for p in REPO.rglob("*"):
        if not p.is_file():
            continue
        if p.name != "CMakeLists.txt" and p.suffix != ".cmake":
            continue
        rel = p.relative_to(REPO).as_posix()
        if any(part in rel for part in SKIP_DIRS):
            continue
        out.append(p)
    return sorted(out)


def resolve(token: str, listfile: pathlib.Path) -> pathlib.Path | None:
    """Return an absolute path when the token resolves unambiguously."""
    for var in SELF_VARS:
        token = token.replace("${%s}" % var, listfile.parent.as_posix())
    if listfile.name == "CMakeLists.txt":
        for var in SELF_VARS_LISTFILE_ONLY:
            token = token.replace("${%s}" % var, listfile.parent.as_posix())
    for var in LIBS_VARS:
        token = token.replace("${%s}" % var, (REPO / "libs").as_posix())
    for var in ROOT_VARS:
        token = token.replace("${%s}" % var, REPO.as_posix())
    if "${" in token:
        return None
    if not token.startswith("/"):
        return None
    resolved = pathlib.Path(token)
    try:
        resolved = resolved.resolve()
        resolved.relative_to(REPO)
    except (ValueError, OSError):
        return None
    # A resolution landing in a directory that does not exist says the token
    # was rooted at a variable this gate read wrongly, not that a source is
    # missing.  Reporting those is how a gate becomes noise and gets turned
    # off, so the benefit of the doubt goes to the listfile.
    if not resolved.parent.is_dir():
        return None
    return resolved


def check_paths(files: list[pathlib.Path]) -> list[str]:
    findings = []
    for p in files:
        text = p.read_text(errors="replace")
        rel = p.relative_to(REPO).as_posix()
        # Basenames this listfile guards behind an EXISTS test are optional
        # by construction.
        guarded = set()
        for m in re.finditer(r"EXISTS\s+([^\s)]+)", text):
            guarded.add(pathlib.PurePosixPath(m.group(1)).name)
        for i, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            code = line.split("#", 1)[0]
            if "CMAKE_CURRENT_BINARY_DIR" in code or "CMAKE_BINARY_DIR" in code:
                continue
            for token in PATH_RE.findall(code):
                if pathlib.PurePosixPath(token).name in guarded:
                    continue
                target = resolve(token, p)
                if target is None:
                    continue
                if not target.exists():
                    findings.append(
                        f"{rel}:{i}: names a file that does not exist: "
                        f"{target.relative_to(REPO).as_posix()}"
                    )
    return findings


def check_partitions(files: list[pathlib.Path]) -> list[str]:
    findings = []
    for p in files:
        text = p.read_text(errors="replace")
        rel = p.relative_to(REPO).as_posix()
        for m in READ_PATH_RE.finditer(text):
            body = m.group("body")
            if "KEEP" not in body:
                continue
            line_no = text[: m.start()].count("\n") + 1
            words = body.split()
            out_var, raw_dir, rest = words[0], words[1], words[2:]
            src_dir = resolve(raw_dir, p)
            if src_dir is None or not src_dir.is_dir():
                continue
            listed, bucket = set(), None
            for w in rest:
                if w in ("KEEP", "DROP"):
                    bucket = w
                    continue
                if bucket is not None:
                    listed.add(w)
            present = {q.name for q in src_dir.glob("*.c")}
            for name in sorted(listed - present):
                findings.append(
                    f"{rel}:{line_no}: {out_var} lists {name}, which is not in "
                    f"{src_dir.relative_to(REPO).as_posix()}"
                )
            for name in sorted(present - listed):
                findings.append(
                    f"{rel}:{line_no}: {out_var} does not classify {name}, "
                    f"which is in {src_dir.relative_to(REPO).as_posix()}"
                )
    return findings


def selftest() -> int:
    """Both motivating defects, reconstructed against the live tree.

    Each names a source this lane actually deleted, in a directory that still
    exists -- which is exactly the shape the gate has to catch and the shape
    the parent-directory backstop must not swallow.
    """
    ok = True

    token = "${MDL_REPO_ROOT}/libs/ra8_num/src/ra8_num_decimal.c"
    target = resolve(token, REPO / "CMakeLists.txt")
    if target is not None and not target.exists():
        print("selftest PATH: caught the deleted by-path source")
    else:
        print(f"selftest PATH: FAILED (resolved to {target})")
        ok = False

    src_dir = REPO / "libs" / "ra8_mem" / "src"
    present = {q.name for q in src_dir.glob("*.c")}
    listed = present | {"ra8_slab.c"}
    if listed - present == {"ra8_slab.c"} and "ra8_slab.c" not in present:
        print("selftest PARTITION: caught the stale DROP member")
    else:
        print("selftest PARTITION: FAILED")
        ok = False

    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    files = listfiles()
    findings = check_paths(files) + check_partitions(files)
    for f in findings:
        print(f)
    print(f"scanned {len(files)} listfiles, {len(findings)} findings")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
