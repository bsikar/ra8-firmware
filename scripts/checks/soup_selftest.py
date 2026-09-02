# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-directions selftest for the vendored-SOUP upstream provenance gate.

It lives beside ``check_soup_upstream.py`` rather than inside it only because
the checker crossed the project file-size cap; ``--selftest`` on that script is
still the entry point, and the ``soup-upstream`` gate runs it before the scan.

Two properties are asserted, because this claim -- "byte-identical to upstream"
-- was stated in three places and checked by nothing, so every tree passed it
and a drifted one would have too:

  * **it fires.** A mutated blob, a changed file mode, a lost file, a ghost
    manifest row, an edit on top of a reviewed patch, an undeclared deviation,
    a stale declaration, a pin that disagrees with the SBOM's, and every
    vacuity floor -- each is provoked one at a time against a REAL scratch git
    repository and driven through ``run_check()``, the function CI calls.
  * **it stays quiet.** The untouched fixture verifies clean, every record kind
    round-trips through the manifest format, and the shipped floors are real
    numbers rather than zero-with-a-comment.
"""

from __future__ import annotations

import contextlib
import io
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gen"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from check_soup_upstream import (
    EXIT_FAIL,
    EXIT_OK,
    EXIT_VACUOUS,
    MIN_COMPONENTS,
    MIN_ENTRIES,
    MIN_UPSTREAM_VERIFIED,
    VacuousScanError,
    _resolve_entry,
    run_check,
)
from git_environment import isolated_git_environment, trusted_git_executable
from sbom_registry import Component
from soup_manifest import (
    KIND_LOCAL,
    KIND_MOVED,
    KIND_OK,
    KIND_PATCH,
    Entry,
    ManifestError,
    format_manifest,
    git_ls_files,
    manifest_path,
    parse_manifest,
)

# --------------------------------------------------------------------------- #
# Selftest -- a real scratch repository, driven through run_check().            #
# --------------------------------------------------------------------------- #

FIXTURE_PATH = "libs/third_party/fixture"
FIXTURE_FLOORS = (1, 1, 1)
# The fixture manifest has two `ok`/`moved` rows among its four records; the
# other two are the declared patch and the declared local file.
FIXTURE_VERIFIED_ROWS = 2
# The shipped floors must be real numbers, not 0-with-a-comment. A floor of
# zero passes for a scan that covered nothing, which is the failure this whole
# family of constants exists to prevent.
FLOOR_SANITY_MIN = 1000
_FIXTURE_FILES = {
    "src/a.c": b"int a;\n",
    "src/b.c": b"int b;\n",
    "LICENSE": b"MIT\n",
    "patched.c": b"int patched;  /* local */\n",
    "generated.h": b"/* generated here, not upstream */\n",
}


def _quiet(func: Callable[..., int], *args: object) -> int:
    """Call `func` with its diagnostics captured, returning only its status.

    The must-fire cases below deliberately provoke real failures; letting their
    error text through would bury the pass/fail report the selftest exists to
    print.  The status is what is asserted, so only the status is kept.
    """
    sink = io.StringIO()
    with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
        return func(*args)


def _fixture_component(
    *,
    key: str = "fixture",
    upstream_commit: str = "0" * 40,
    modified: bool = True,
    patched_files: tuple[tuple[str, str], ...] = (("patched.c", "selftest patch"),),
    local_files: tuple[tuple[str, str], ...] = (("generated.h", "selftest local"),),
) -> Component:
    """Build the selftest's synthetic registry entry."""
    return Component(
        key=key,
        name="fixture",
        version="0",
        ctype="library",
        url="https://example.invalid/fixture",
        path=FIXTURE_PATH,
        provenance="commit-pinned-sha256",
        description="selftest fixture",
        upstream_commit=upstream_commit,
        modified=modified,
        patched_files=patched_files,
        local_files=local_files,
    )


def _git(args: list[str], cwd: Path) -> None:
    """Run one git command in `cwd`, discarding its output."""
    subprocess.run(  # noqa: S603  # trusted: fixed git argv, no shell
        [trusted_git_executable(), *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        check=True,
    )


def _write_fixture_repo(root: Path) -> dict[str, tuple[str, str]]:
    """Materialise a REAL git repository holding the fixture vendored tree.

    A real repository, not a stub: Git supplies the same tracked/untracked
    worktree census the gate uses in CI, and the gate derives raw blob ids from
    those files.

    Args:
        root: Scratch directory to initialise as a repository.

    Returns:
        ``{rel path: (mode, blob)}`` exactly as `git_ls_files` will report it.
    """
    _git(["init", "-q", "-b", "main", "."], root)
    for rel, data in _FIXTURE_FILES.items():
        target = root / FIXTURE_PATH / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    _git(["add", "-A"], root)
    return git_ls_files(FIXTURE_PATH, (), root)


def _selftest_worktree_cases(
    root: Path, original: dict[str, tuple[str, str]]
) -> list[tuple[str, bool]]:
    """Prove unstaged bytes, modes, additions, and deletions affect the census."""
    source = root / FIXTURE_PATH / "src/a.c"
    source.write_bytes(b"int changed;\n")
    mutated = git_ls_files(FIXTURE_PATH, (), root)
    source.write_bytes(_FIXTURE_FILES["src/a.c"])

    added_path = root / FIXTURE_PATH / "untracked.c"
    added_path.write_bytes(b"int untracked;\n")
    added = git_ls_files(FIXTURE_PATH, (), root)
    added_path.unlink()

    deleted_path = root / FIXTURE_PATH / "src/b.c"
    deleted_path.unlink()
    deleted = git_ls_files(FIXTURE_PATH, (), root)
    deleted_path.write_bytes(_FIXTURE_FILES["src/b.c"])

    source.chmod(0o755)
    remoded = git_ls_files(FIXTURE_PATH, (), root)
    source.chmod(0o644)
    return [
        (
            "MUST FIRE: an unstaged vendored-byte mutation changes the worktree blob",
            mutated["src/a.c"][1] != original["src/a.c"][1],
        ),
        (
            "MUST FIRE: an untracked vendored file enters the worktree census",
            "untracked.c" in added,
        ),
        (
            "MUST FIRE: a deleted tracked vendor file leaves the worktree census",
            "src/b.c" not in deleted,
        ),
        (
            "MUST FIRE: an unstaged executable-bit change changes the worktree mode",
            remoded["src/a.c"][0] == "100755",
        ),
    ]


def _write_fixture_manifest(root: Path, ours: dict[str, tuple[str, str]], **mutate: str) -> None:
    """Write the fixture's manifest, optionally corrupting one field.

    Args:
        root: Scratch repository root.
        ours: The fixture's ``{rel path: (mode, blob)}``.
        mutate: ``kind``/``blob``/``mode``/``drop``/``extra`` knobs the
            must-fire cases use to break exactly one thing.
    """
    entries: list[Entry] = []
    for rel, (mode, blob) in sorted(ours.items()):
        if rel == mutate.get("drop"):
            continue
        if rel == "patched.c":
            entries.append(Entry(KIND_PATCH, mode, rel, upstream_blob="1" * 40, local_blob=blob))
        elif rel == "generated.h":
            entries.append(Entry(KIND_LOCAL, mode, rel, local_blob=blob))
        else:
            entries.append(Entry(KIND_OK, mode, rel, upstream_blob=blob))
    entries = [
        Entry(
            e.kind,
            mutate["mode"] if e.rel_path == mutate.get("mode_of") else e.mode,
            e.rel_path,
            upstream_blob=("2" * 40 if e.rel_path == mutate.get("blob_of") else e.upstream_blob),
            local_blob=("3" * 40 if e.rel_path == mutate.get("local_of") else e.local_blob),
            upstream_path=e.upstream_path,
        )
        for e in entries
    ]
    if mutate.get("extra"):
        entries.append(Entry(KIND_OK, "100644", mutate["extra"], upstream_blob="4" * 40))
    header = {"upstream-url": "https://example.invalid", "ref": "v0", "commit": "0" * 40}
    out = root / manifest_path("fixture")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(format_manifest("fixture", header, entries), encoding="utf-8")


def _selftest_manifest_cases(
    root: Path, ours: dict[str, tuple[str, str]]
) -> list[tuple[str, bool]]:
    """Break the MANIFEST one way at a time and assert `run_check`'s verdict.

    Args:
        root: The scratch repository from `_write_fixture_repo`.
        ours: That fixture's ``{rel path: (mode, blob)}``.

    Returns:
        One ``(label, passed)`` pair per assertion.
    """
    comps = (_fixture_component(),)

    def verdict(**mutate: str) -> int:
        _write_fixture_manifest(root, ours, **mutate)
        return _quiet(run_check, comps, root, FIXTURE_FLOORS)

    return [
        ("MUST NOT FIRE: an untouched fixture verifies clean", verdict() == EXIT_OK),
        (
            "MUST FIRE: a vendored file that is not the upstream blob",
            verdict(blob_of="src/a.c") == EXIT_FAIL,
        ),
        (
            "MUST FIRE: a mode that disagrees with upstream",
            verdict(mode_of="src/b.c", mode="100755") == EXIT_FAIL,
        ),
        (
            "MUST FIRE: an edit on top of a reviewed patch",
            verdict(local_of="patched.c") == EXIT_FAIL,
        ),
        (
            "MUST FIRE: a tracked file absent from the manifest",
            verdict(drop="src/b.c") == EXIT_FAIL,
        ),
        (
            "MUST FIRE: a manifest record with no file in the tree",
            verdict(extra="ghost.c") == EXIT_FAIL,
        ),
    ]


def _selftest_registry_cases(
    root: Path, ours: dict[str, tuple[str, str]]
) -> list[tuple[str, bool]]:
    """Hold the manifest correct and break the REGISTRY's declarations instead.

    Args:
        root: The scratch repository from `_write_fixture_repo`.
        ours: That fixture's ``{rel path: (mode, blob)}``.

    Returns:
        One ``(label, passed)`` pair per assertion.
    """
    _write_fixture_manifest(root, ours)
    comps = (_fixture_component(),)

    def verdict(
        *,
        key: str = "fixture",
        upstream_commit: str = "0" * 40,
        modified: bool = True,
        patched_files: tuple[tuple[str, str], ...] = (("patched.c", "selftest patch"),),
        local_files: tuple[tuple[str, str], ...] = (("generated.h", "selftest local"),),
    ) -> int:
        component = _fixture_component(
            key=key,
            upstream_commit=upstream_commit,
            modified=modified,
            patched_files=patched_files,
            local_files=local_files,
        )
        return _quiet(run_check, (component,), root, FIXTURE_FLOORS)

    return [
        (
            "MUST FIRE: a patch/local record the registry does not declare",
            verdict(patched_files=(), local_files=()) == EXIT_FAIL,
        ),
        (
            "MUST FIRE: a patched file on a component recording modified=False",
            verdict(modified=False) == EXIT_FAIL,
        ),
        (
            "MUST FIRE: a declaration for a file that is byte-identical to upstream (stale)",
            verdict(
                patched_files=(("patched.c", "why"), ("src/a.c", "a patch that no longer exists"))
            )
            == EXIT_FAIL,
        ),
        (
            "MUST FIRE: the registry pin and the verified pin are different revisions",
            verdict(upstream_commit="7" * 40) == EXIT_FAIL,
        ),
        (
            "MUST FIRE: a vendored component with no manifest at all",
            verdict(key="absent") == EXIT_VACUOUS,
        ),
        (
            "MUST FIRE: a manifest whose rows prove nothing against upstream",
            _quiet(run_check, comps, root, (1, 1, 99)) == EXIT_VACUOUS,
        ),
        (
            "MUST FIRE: a scan covering fewer components than the floor",
            _quiet(run_check, comps, root, (99, 1, 1)) == EXIT_VACUOUS,
        ),
    ]


# Each row: (label, vendored path, our (mode, blob), upstream listing).
# `_resolve_entry` must REFUSE every one of them rather than write a record --
# a refresh that quietly classified any of these would launder the defect.
_REFUSAL_CASES = (
    (
        "an undeclared file whose bytes differ from upstream",
        "src/a.c",
        {"src/a.c": ("100644", "f" * 40)},
    ),
    ("an undeclared file upstream does not have at all", "invented.c", {}),
    (
        "a 'local' file upstream turns out to publish",
        "generated.h",
        {"generated.h": ("100644", "f" * 40)},
    ),
    (
        "a 'local' file whose bytes exist elsewhere upstream",
        "generated.h",
        {"somewhere/else.h": ("100644", "9" * 40)},
    ),
    ("a 'patch' of a file upstream does not have", "patched.c", {}),
    (
        "a 'patch' declaration on a file identical to upstream (stale)",
        "patched.c",
        {"patched.c": ("100644", "9" * 40)},
    ),
)


def _selftest_resolve_cases() -> list[tuple[str, bool]]:
    """Assert `_resolve_entry`'s mappings, and its refusal to invent a deviation."""
    comp = _fixture_component()
    tree = {"src/a.c": ("100644", "a" * 40), "moved/here.c": ("100644", "c" * 40)}
    cases: list[tuple[str, bool]] = [
        (
            "MUST NOT FIRE: a byte-identical file resolves as 'ok'",
            _resolve_entry(comp, "src/a.c", ("100644", "a" * 40), tree).kind == KIND_OK,
        ),
        (
            "MUST NOT FIRE: a relocated but identical file resolves as 'moved'",
            _resolve_entry(comp, "flat.c", ("100644", "c" * 40), tree).upstream_path
            == "moved/here.c",
        ),
        (
            "MUST NOT FIRE: a declared patch keeps upstream's hash alongside ours",
            _resolve_entry(
                comp, "patched.c", ("100644", "d" * 40), {"patched.c": ("100644", "e" * 40)}
            ).upstream_blob
            == "e" * 40,
        ),
    ]
    for label, path, tree_arg in _REFUSAL_CASES:
        fired = False
        try:
            _resolve_entry(comp, path, ("100644", "9" * 40), tree_arg)
        except VacuousScanError:
            fired = True
        cases.append((f"MUST FIRE: --refresh refuses {label}", fired))
    return cases


def _selftest_format_cases() -> list[tuple[str, bool]]:
    """Assert the manifest format round-trips and rejects malformed records."""
    entries = [
        Entry(KIND_OK, "100644", "a.c", upstream_blob="a" * 40),
        Entry(KIND_MOVED, "120000", "b.c", upstream_blob="b" * 40, upstream_path="up/b.c"),
        Entry(KIND_PATCH, "100644", "c.c", upstream_blob="c" * 40, local_blob="d" * 40),
        Entry(KIND_LOCAL, "100755", "d.c", local_blob="e" * 40),
    ]
    text = format_manifest("fixture", {"commit": "0" * 40}, entries)
    parsed = parse_manifest("fixture", text, Path("fixture"))
    cases = [
        ("MUST NOT FIRE: every record kind round-trips", list(parsed.entries) == entries),
        (
            "MUST NOT FIRE: only ok/moved rows count as upstream-verified",
            parsed.verified_count() == FIXTURE_VERIFIED_ROWS,
        ),
    ]
    for label, bad in (
        ("an unknown record kind", "bogus 100644 " + "a" * 40 + " a.c"),
        ("a truncated blob id", "ok 100644 abc a.c"),
        ("a non-octal file mode", "ok 10x644 " + "a" * 40 + " a.c"),
        ("a duplicate path", "ok 100644 " + "a" * 40 + " a.c\nok 100644 " + "b" * 40 + " a.c"),
    ):
        fired = False
        try:
            parse_manifest("fixture", f"# component: fixture\n{bad}\n", Path("fixture"))
        except ManifestError:
            fired = True
        cases.append((f"MUST FIRE: the parser rejects {label}", fired))
    cases.append(
        (
            "MUST FIRE: the parser rejects a manifest naming another component",
            _raises_manifest_error("other", "# component: fixture\n"),
        )
    )
    return cases


def _raises_manifest_error(key: str, text: str) -> bool:
    """Return True when `parse_manifest` rejects `text` for `key`."""
    try:
        parse_manifest(key, text, Path("fixture"))
    except ManifestError:
        return True
    return False


def _run_selftest_body() -> int:
    """Prove the gate fires on every provenance defect and stays quiet otherwise.

    Both directions are asserted because only one of them has ever been true of
    this claim: "byte-identical to upstream" was stated in three places and
    checked nowhere, so every tree passed and a corrupted one would have too.

    Returns:
        ``EXIT_OK`` when every case holds, ``EXIT_VACUOUS`` otherwise.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        ours = _write_fixture_repo(root)
        cases = _selftest_worktree_cases(root, ours)
        cases.extend(_selftest_manifest_cases(root, ours))
        cases.extend(_selftest_registry_cases(root, ours))
    cases.extend(_selftest_resolve_cases())
    cases.extend(_selftest_format_cases())
    cases.append(
        (
            "MUST NOT FIRE: the shipped floors are below the live tree, not zero",
            MIN_COMPONENTS > 1
            and MIN_ENTRIES > FLOOR_SANITY_MIN
            and MIN_UPSTREAM_VERIFIED > FLOOR_SANITY_MIN,
        )
    )
    failed = [label for label, ok in cases if not ok]
    for label, ok in cases:
        print(f"  {'ok  ' if ok else 'FAIL'} {label}")
    if failed:
        print(f"check_soup_upstream: selftest FAILED ({len(failed)} case(s))", file=sys.stderr)
        return EXIT_VACUOUS
    print(f"check_soup_upstream: selftest passed ({len(cases)} cases, both directions).")
    return EXIT_OK


def run_selftest() -> int:
    """Run provenance fixtures without inheriting the caller's repository."""
    with isolated_git_environment():
        return _run_selftest_body()
