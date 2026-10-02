# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Fixtures and cases for ``gen_sbom.py --selftest``.

This is the must-fire/must-stay-quiet half of the SBOM provenance gate, split
out of ``gen_sbom.py`` so neither file carries two subjects. The generator
renders and validates the registry; this module proves that validation can
actually fail, which is the property the SOUP audit showed was missing when
``aggregate_sha256`` was a transcribed constant compared against itself.

Every case is a ``(label, ok)`` pair, so a case that stops exercising anything
reports as a failure rather than silently disappearing from the count.
``gen_sbom.run_selftest`` imports this module lazily: the dependency runs one
way at module load, and only the ``--selftest`` path pays for the fixtures.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from gen_sbom import (
    EXIT_OK,
    EXIT_VACUOUS,
    GENERATOR_NAME,
    NON_VENDORED_PROVENANCE,
    VacuousScanError,
    _dep_pin_errors,
    _directory_drift,
    _git_ls_files,
    _vendor_root_for,
    digest_entries,
    hashed_components,
)
from git_environment import trusted_git_executable
from sbom_registry import (
    PROV_DEP_PINNED,
    REGISTRY,
    Component,
)


def _selftest_tree(root: Path) -> list[tuple[str, str]]:
    """Materialise a small fixture tree under `root` and return its entries.

    Args:
        root: Directory to create the fixture under.

    Returns:
        ``(mode, path)`` pairs in the shape `_git_ls_files` produces.
    """
    (root / "vendor" / "src").mkdir(parents=True)
    (root / "vendor" / "src" / "a.c").write_bytes(b"int a;\n")
    (root / "vendor" / "src" / "b.c").write_bytes(b"int b;\n")
    (root / "vendor" / "LICENSE").write_bytes(b"MIT\n")
    return [
        ("100644", "vendor/src/a.c"),
        ("100644", "vendor/src/b.c"),
        ("100644", "vendor/LICENSE"),
    ]


def _selftest_worktree_cases(root: Path) -> list[tuple[str, bool]]:
    """Prove the SBOM census observes unstaged vendor-tree state both ways."""
    repo = root / "repo"
    vendor = repo / "vendor"
    vendor.mkdir(parents=True)
    tracked = vendor / "tracked.c"
    removed = vendor / "removed.c"
    tracked.write_bytes(b"int tracked;\n")
    removed.write_bytes(b"int removed;\n")
    (repo / ".gitignore").write_text("vendor/ignored.c\n", encoding="ascii")
    subprocess.run(  # noqa: S603 -- fixed Git authority and fixture-only argv
        [trusted_git_executable(), "init", "-q", "-b", "main", "."],
        cwd=repo,
        check=True,
    )
    subprocess.run(  # noqa: S603 -- fixed Git authority and fixture-only argv
        [trusted_git_executable(), "add", "-A"],
        cwd=repo,
        check=True,
    )
    original_digest = digest_entries(repo, _git_ls_files("vendor", repo), "vendor")

    tracked.write_bytes(b"int changed;\n")
    tracked.chmod(0o755)
    removed.unlink()
    (vendor / "untracked.c").write_bytes(b"int untracked;\n")
    (vendor / "ignored.c").write_bytes(b"int ignored;\n")
    entries = _git_ls_files("vendor", repo)
    paths = {path for _mode, path in entries}
    return [
        (
            "MUST FIRE: unstaged bytes and mode change the SBOM worktree digest",
            digest_entries(repo, entries, "vendor") != original_digest
            and ("100755", "vendor/tracked.c") in entries,
        ),
        (
            "MUST FIRE: an untracked vendor file enters the SBOM census",
            "vendor/untracked.c" in paths,
        ),
        (
            "MUST FIRE: a deleted tracked vendor file leaves the SBOM census",
            "vendor/removed.c" not in paths,
        ),
        (
            "MUST NOT FIRE: an ignored vendor file stays outside the SBOM census",
            "vendor/ignored.c" not in paths,
        ),
    ]


def _selftest_shape_cases(
    root: Path, entries: list[tuple[str, str]], base: str
) -> list[tuple[str, bool]]:
    """Assert that changing the SHAPE of the file set changes the digest.

    Content mutation is covered by the caller; these are the cases a naive
    "hash the concatenated bytes" digest would miss -- an added or removed
    file, and a mode change.

    Args:
        root: The fixture root from `_selftest_tree`.
        entries: That fixture's entry list, content-restored.
        base: The digest of the unmodified fixture.

    Returns:
        One ``(label, passed)`` pair per assertion.
    """
    (root / "vendor" / "src" / "c.c").write_bytes(b"int a;\n")
    vacuous = False
    try:
        digest_entries(root, [], "vendor")
    except VacuousScanError:
        vacuous = True
    return [
        (
            "MUST FIRE: an added file changes the digest",
            digest_entries(root, [*entries, ("100644", "vendor/src/c.c")], "vendor") != base,
        ),
        (
            "MUST FIRE: a removed file changes the digest",
            digest_entries(root, entries[:-1], "vendor") != base,
        ),
        (
            "MUST FIRE: a mode change changes the digest",
            digest_entries(root, [("100755", entries[0][1]), *entries[1:]], "vendor") != base,
        ),
        ("MUST FIRE: an empty enumeration raises rather than hashing nothing", vacuous),
    ]


def _selftest_registry_cases() -> list[tuple[str, bool]]:
    """Return registry/tree ownership assertions for every supported vendor shape."""
    return [
        (
            "MUST FIRE: the live registry publishes a digest for every vendored component",
            len(hashed_components())
            == len(REGISTRY)
            - sum(1 for comp in REGISTRY if comp.provenance in NON_VENDORED_PROVENANCE),
        ),
        (
            "MUST NOT FIRE: no non-vendored component is asked for a tree digest",
            not [c for c in hashed_components() if c.provenance in NON_VENDORED_PROVENANCE],
        ),
        (
            "MUST FIRE: a dependency pin missing from its authority is detected",
            bool(
                _dep_pin_errors(
                    Component(
                        key="selftest-dep",
                        name="selftest dep",
                        version="9.9.9",
                        ctype="application",
                        url="https://example.invalid",
                        path="pyproject.toml",
                        provenance=PROV_DEP_PINNED,
                        description="fixture",
                        dep_pin_spec='"ra8-selftest-absent==9.9.9"',
                    )
                )
            ),
        ),
        (
            "MUST NOT FIRE: every live dependency pin is present in its authority",
            not [
                err
                for comp in REGISTRY
                if comp.provenance == PROV_DEP_PINNED
                for err in _dep_pin_errors(comp)
            ],
        ),
        (
            "MUST FIRE: a dependency-pinned component with no declared pin is detected",
            bool(
                _dep_pin_errors(
                    Component(
                        key="selftest-dep-nopin",
                        name="selftest dep, no pin",
                        version="9.9.9",
                        ctype="application",
                        url="https://example.invalid",
                        path="pyproject.toml",
                        provenance=PROV_DEP_PINNED,
                        description="fixture",
                    )
                )
            ),
        ),
        (
            "MUST NOT FIRE: matching entries across all vendor-root shapes stay quiet",
            not _directory_drift(
                {
                    "libs/third_party/platform",
                    "apps/shared_libs/third_party/app",
                    "tools/viewer/third_party/tool_only",
                },
                {
                    "libs/third_party/platform",
                    "apps/shared_libs/third_party/app",
                    "tools/viewer/third_party/tool_only",
                },
            ),
        ),
        (
            "MUST FIRE: an uncatalogued app vendor is detected",
            bool(
                _directory_drift(
                    {"libs/third_party/platform", "apps/shared_libs/third_party/extra"},
                    {"libs/third_party/platform"},
                )
            ),
        ),
        (
            "MUST FIRE: a missing app vendor is detected",
            bool(
                _directory_drift(
                    {"libs/third_party/platform"},
                    {"libs/third_party/platform", "apps/shared_libs/third_party/app"},
                )
            ),
        ),
        (
            "MUST NOT FIRE: the narrow tool-private vendor shape is supported",
            _vendor_root_for(Path("tools/viewer/third_party/decoder"))
            == Path("tools/viewer/third_party"),
        ),
        (
            "MUST FIRE: a repository-wide tools vendor bucket is unsupported",
            _vendor_root_for(Path("tools/third_party/decoder")) is None,
        ),
    ]


def _selftest_cases(root: Path, entries: list[tuple[str, str]]) -> list[tuple[str, bool]]:
    """Run every digest assertion against the fixture and return ``(label, ok)``.

    Args:
        root: The fixture root from `_selftest_tree`.
        entries: That fixture's entry list.

    Returns:
        One ``(label, passed)`` pair per assertion, both directions covered.
    """
    base = digest_entries(root, entries, "vendor")
    cases: list[tuple[str, bool]] = [
        (
            "MUST NOT FIRE: an unchanged tree hashes identically",
            digest_entries(root, entries, "vendor") == base,
        ),
        (
            "MUST NOT FIRE: enumeration order does not change the digest",
            digest_entries(root, list(reversed(entries)), "vendor") == base,
        ),
    ]

    (root / "vendor" / "src" / "a.c").write_bytes(b"int a;\n/* injected */\n")
    cases.append(
        (
            "MUST FIRE: one mutated vendored byte changes the digest",
            digest_entries(root, entries, "vendor") != base,
        )
    )
    (root / "vendor" / "src" / "a.c").write_bytes(b"int a;\n")
    cases.append(
        (
            "MUST NOT FIRE: restoring the byte restores the digest",
            digest_entries(root, entries, "vendor") == base,
        )
    )

    cases.extend(_selftest_shape_cases(root, entries, base))

    cases.extend(_selftest_registry_cases())
    return cases


def _run_selftest_body() -> int:
    """Prove the integrity digest fires on a mutation and stays quiet otherwise.

    Both directions are asserted because only one of them was ever true before:
    the old hardcoded ``aggregate_sha256`` was perfectly stable on an unchanged
    tree and equally stable on a mutated one.  A selftest that checked only the
    quiet direction would have passed against the broken code.

    Returns:
        ``EXIT_OK`` when every case holds, ``EXIT_VACUOUS`` otherwise.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        cases = _selftest_cases(root, _selftest_tree(root))
        cases.extend(_selftest_worktree_cases(root))
    failed = [label for label, ok in cases if not ok]
    for label, ok in cases:
        print(f"  {'ok  ' if ok else 'FAIL'} {label}")
    if failed:
        print(f"{GENERATOR_NAME}: selftest FAILED ({len(failed)} case(s))", file=sys.stderr)
        return EXIT_VACUOUS
    print(f"{GENERATOR_NAME}: selftest passed ({len(cases)} cases, both directions).")
    return EXIT_OK
