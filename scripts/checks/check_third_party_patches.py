#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline gate for reproducible third-party patch series.

Vendored dependencies stay ready to compile: reviewed patches are already
present in their checked-in bytes. This gate proves, without fetching the
network, that each declared patch series reverses those bytes to the upstream
blob recorded in docs/sbom/upstream and reapplies to the checked-in blob.

Fetched dependencies use the other supported delivery model: the build checks
out a pin and applies the numbered series. For those, this gate verifies the
pin, series, and application entry point are connected, AND that the recorded
upstream record still describes the CURRENT pin: `upstream_pin` must equal the
live pin value, and every file the series touches must be recorded with a blob
whose id the patch's own pre-image abbreviation prefixes.

Honest limits, offline. The gate has no git objects for a fetched dependency,
so it cannot recompute a blob: it compares the registry against the patch's
`index` line, and those are two first-party artifacts. What it DOES catch is a
pin bump that orphans the series, a patch edited away from its recorded
pre-image, a target with no record, and a record no patch touches. Proving the
blob ids against the real upstream remains the job of the networked
`soup-upstream-refresh` gate.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gen"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from git_environment import sanitized_git_environment, trusted_git_executable
from sbom_registry import PROV_NOT_VENDORED, REGISTRY
from soup_manifest import KIND_PATCH, ManifestError, manifest_path, parse_manifest

REPO_ROOT = Path(__file__).resolve().parents[2]
POLICY_PATH = Path("docs/sbom/patches/registry.toml")
NUMBERED_PATCH_RE = re.compile(r"\A[0-9]{4}-[a-z0-9][a-z0-9-]*\.patch\Z")
HEX40_RE = re.compile(r"\A[0-9a-f]{40}\Z")
DELIVERIES = frozenset(("vendored", "fetched"))
CLASSIFICATIONS = frozenset(("functional", "metadata"))
METADATA_NAMES = frozenset((".gitattributes", ".gitignore", ".gitmodules"))
NUMSTAT_FIELD_COUNT = 3


@dataclass(frozen=True)
class PatchItem:
    """One numbered transformation and its review classification."""

    file: str
    classification: str


@dataclass(frozen=True)
class PatchComponent:
    """One vendored or fetched component's ordered patch series."""

    key: str
    delivery: str
    series: Path
    patches: tuple[PatchItem, ...]
    apply_script: Path | None = None
    series_token: str | None = None
    pin_file: Path | None = None
    pin_key: str | None = None
    # (upstream path, 40-hex blob) at the pin, for every file the series touches,
    # plus the pin those blobs were read from.
    upstream_blobs: tuple[tuple[str, str], ...] = ()
    upstream_pin: str | None = None


def _safe_rel_path(value: object, where: str) -> Path:
    """Return a validated repository-relative POSIX path."""
    if not isinstance(value, str) or not value:
        msg = f"{where}: expected a non-empty path string"
        raise ValueError(msg)
    pure = PurePosixPath(value)
    if pure.is_absolute() or ".." in pure.parts or str(pure) != value:
        msg = f"{where}: path must be normalized and repository-relative: {value!r}"
        raise ValueError(msg)
    return Path(value)


def _parse_patch(raw: object, where: str) -> PatchItem:
    """Parse and validate one patch table."""
    if not isinstance(raw, dict):
        msg = f"{where}: patch entry must be a table"
        raise TypeError(msg)
    unknown = set(raw) - {"file", "classification"}
    if unknown:
        msg = f"{where}: unknown patch fields: {', '.join(sorted(unknown))}"
        raise ValueError(msg)
    filename = raw.get("file")
    classification = raw.get("classification")
    if not isinstance(filename, str) or not NUMBERED_PATCH_RE.fullmatch(filename):
        msg = f"{where}: patch file must match NNNN-lower-kebab.patch"
        raise ValueError(msg)
    if classification not in CLASSIFICATIONS:
        msg = f"{where}: classification must be functional or metadata"
        raise ValueError(msg)
    return PatchItem(filename, classification)


def _parse_upstream_blobs(raw: object, where: str) -> tuple[tuple[str, str], ...]:
    """Parse the recorded pin blob table for one fetched component."""
    if not isinstance(raw, dict) or not raw:
        msg = f"{where}.upstream_blobs: expected a non-empty table"
        raise ValueError(msg)
    for name, value in raw.items():
        if not isinstance(value, str) or not HEX40_RE.fullmatch(value):
            msg = f"{where}.upstream_blobs.{name}: expected one full 40-hex blob id"
            raise ValueError(msg)
    return tuple(sorted(raw.items()))


def _parse_upstream_record(raw: dict, delivery: object, where: str) -> dict[str, object]:
    """Parse the optional recorded-upstream fields of a fetched component."""
    if "upstream_blobs" not in raw and "upstream_pin" not in raw:
        return {}
    if delivery != "fetched":
        msg = f"{where}: upstream_blobs/upstream_pin apply only to a fetched component"
        raise ValueError(msg)
    upstream_pin = raw.get("upstream_pin")
    if not isinstance(upstream_pin, str) or not HEX40_RE.fullmatch(upstream_pin):
        msg = f"{where}.upstream_pin: expected one full 40-hex commit id"
        raise ValueError(msg)
    return {
        "upstream_blobs": _parse_upstream_blobs(raw.get("upstream_blobs"), where),
        "upstream_pin": upstream_pin,
    }


def _parse_component(raw: object, index: int) -> PatchComponent:
    """Parse and validate one component table."""
    where = f"component[{index}]"
    if not isinstance(raw, dict):
        msg = f"{where}: entry must be a table"
        raise TypeError(msg)
    allowed = {
        "key",
        "delivery",
        "series",
        "patches",
        "apply_script",
        "series_token",
        "pin_file",
        "pin_key",
        "upstream_blobs",
        "upstream_pin",
    }
    unknown = set(raw) - allowed
    if unknown:
        msg = f"{where}: unknown fields: {', '.join(sorted(unknown))}"
        raise ValueError(msg)
    key = raw.get("key")
    delivery = raw.get("delivery")
    if not isinstance(key, str) or not key:
        msg = f"{where}: key must be a non-empty string"
        raise ValueError(msg)
    if delivery not in DELIVERIES:
        msg = f"{where}: delivery must be vendored or fetched"
        raise ValueError(msg)
    patches_raw = raw.get("patches")
    if not isinstance(patches_raw, list) or not patches_raw:
        msg = f"{where}: at least one [[component.patches]] entry is required"
        raise ValueError(msg)
    patches = tuple(
        _parse_patch(item, f"{where}.patches[{i}]") for i, item in enumerate(patches_raw)
    )
    filenames = tuple(item.file for item in patches)
    if len(set(filenames)) != len(filenames):
        msg = f"{where}: duplicate patch filename"
        raise ValueError(msg)
    kwargs: dict[str, object] = {}
    for field in ("apply_script", "pin_file"):
        if field in raw:
            kwargs[field] = _safe_rel_path(raw[field], f"{where}.{field}")
    for field in ("series_token", "pin_key"):
        if field in raw:
            value = raw[field]
            if not isinstance(value, str) or not value:
                msg = f"{where}.{field}: expected a non-empty string"
                raise ValueError(msg)
            kwargs[field] = value
    kwargs.update(_parse_upstream_record(raw, delivery, where))
    return PatchComponent(
        key,
        delivery,
        _safe_rel_path(raw.get("series"), f"{where}.series"),
        patches,
        **kwargs,
    )


def load_policy(root: Path) -> tuple[PatchComponent, ...]:
    """Load the strict machine-readable patch registry."""
    path = root / POLICY_PATH
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        msg = f"{POLICY_PATH}: {exc}"
        raise ValueError(msg) from exc
    if set(raw) != {"schema_version", "component"} or raw.get("schema_version") != 1:
        msg = f"{POLICY_PATH}: expected only schema_version=1 and component tables"
        raise ValueError(msg)
    rows = raw.get("component")
    if not isinstance(rows, list) or not rows:
        msg = f"{POLICY_PATH}: no component tables"
        raise ValueError(msg)
    components = tuple(_parse_component(row, index) for index, row in enumerate(rows))
    keys = tuple(component.key for component in components)
    if len(set(keys)) != len(keys):
        msg = f"{POLICY_PATH}: duplicate component key"
        raise ValueError(msg)
    return components


def _series_files(component: PatchComponent, root: Path) -> tuple[Path, ...]:
    """Validate a series file and return its patch paths in order."""
    series_path = root / component.series
    try:
        lines = series_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        msg = f"{component.series}: {exc}"
        raise ValueError(msg) from exc
    names = tuple(
        line.strip() for line in lines if line.strip() and not line.lstrip().startswith("#")
    )
    expected = tuple(item.file for item in component.patches)
    if names != expected:
        msg = f"{component.series}: series order {names!r} != registry {expected!r}"
        raise ValueError(msg)
    paths = tuple(series_path.parent / name for name in names)
    missing = tuple(path.relative_to(root).as_posix() for path in paths if not path.is_file())
    if missing:
        msg = f"{component.key}: missing patch files: {', '.join(missing)}"
        raise ValueError(msg)
    return paths


def _patch_targets(patch: Path, root: Path) -> tuple[str, ...]:
    """Return normalized target paths reported by Git's patch parser."""
    proc = subprocess.run(  # noqa: S603 -- fixed Git executable and validated patch path
        (trusted_git_executable(), "apply", "--numstat", str(patch)),
        cwd=root,
        env=sanitized_git_environment(),
        text=True,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip()
        msg = f"{patch.relative_to(root)}: git apply --numstat failed: {detail}"
        raise ValueError(msg)
    targets: list[str] = []
    for line in proc.stdout.splitlines():
        fields = line.split("\t", 2)
        if len(fields) != NUMSTAT_FIELD_COUNT or not fields[2]:
            msg = f"{patch.relative_to(root)}: malformed numstat row: {line!r}"
            raise ValueError(msg)
        target = fields[2]
        if " => " in target or target.startswith("{"):
            msg = f"{patch.relative_to(root)}: rename patches are unsupported: {target}"
            raise ValueError(msg)
        _safe_rel_path(target, str(patch.relative_to(root)))
        targets.append(target)
    if not targets:
        msg = f"{patch.relative_to(root)}: patch changes no files"
        raise ValueError(msg)
    return tuple(dict.fromkeys(targets))


def _blob_id(path: Path) -> str:
    """Return the raw Git blob SHA-1 for one file, without attributes."""
    data = path.read_bytes()
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()  # noqa: S324 -- Git object IDs require SHA-1


def _apply(patch: Path, work: Path, reverse: bool) -> str | None:
    """Apply one patch in a disposable component tree."""
    argv = [trusted_git_executable(), "apply", "--whitespace=nowarn"]
    if reverse:
        argv.append("--reverse")
    argv.append(str(patch))
    proc = subprocess.run(  # noqa: S603 -- fixed Git executable and validated patch path
        argv,
        cwd=work,
        env=sanitized_git_environment(),
        text=True,
        capture_output=True,
        check=False,
    )
    if proc.returncode == 0:
        return None
    direction = "reverse" if reverse else "forward"
    return f"{patch}: {direction} apply failed: {proc.stderr.strip()}"


def _copy_targets(source: Path, work: Path, targets: set[str]) -> list[str]:
    """Copy only files touched by patches into a disposable tree."""
    errors: list[str] = []
    for rel in sorted(targets):
        src = source / rel
        if not src.is_file():
            errors.append(f"{source}/{rel}: declared patched file is missing")
            continue
        dst = work / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    return errors


def _validate_metadata(
    component: PatchComponent, targets_by_patch: tuple[tuple[str, ...], ...]
) -> list[str]:
    """Reject a metadata classification on any code or payload file."""
    errors: list[str] = []
    for item, targets in zip(component.patches, targets_by_patch, strict=True):
        if item.classification != "metadata":
            continue
        invalid = tuple(path for path in targets if PurePosixPath(path).name not in METADATA_NAMES)
        if invalid:
            errors.append(
                f"{component.key}: {item.file} is classified metadata but changes "
                + ", ".join(invalid)
            )
    return errors


def _validate_vendored(
    component: PatchComponent,
    registry_component: object,
    manifest: object,
    patch_paths: tuple[Path, ...],
    root: Path,
) -> list[str]:
    """Prove a vendored series reverses to upstream and reapplies exactly."""
    errors: list[str] = []
    targets_by_patch = tuple(_patch_targets(path, root) for path in patch_paths)
    errors.extend(_validate_metadata(component, targets_by_patch))
    targets = {target for group in targets_by_patch for target in group}
    declared = set(dict(registry_component.patched_files))
    recorded = {entry.rel_path for entry in manifest.entries if entry.kind == KIND_PATCH}
    if targets != declared:
        errors.append(
            f"{component.key}: patch targets {sorted(targets)!r} != "
            f"patched_files {sorted(declared)!r}"
        )
    if targets != recorded:
        errors.append(
            f"{component.key}: patch targets {sorted(targets)!r} != "
            f"manifest patch rows {sorted(recorded)!r}"
        )
    if errors:
        return errors
    entries = manifest.by_path()
    source = root / registry_component.path
    with tempfile.TemporaryDirectory(prefix="ra8-patch-check-") as raw_tmp:
        work = Path(raw_tmp) / "component"
        work.mkdir()
        errors.extend(_copy_targets(source, work, targets))
        if errors:
            return errors
        for patch in reversed(patch_paths):
            failure = _apply(patch.resolve(), work, reverse=True)
            if failure:
                errors.append(f"{component.key}: {failure}")
                return errors
        errors.extend(
            f"{component.key}: reverse series does not reproduce upstream blob for {rel}"
            for rel in sorted(targets)
            if _blob_id(work / rel) != entries[rel].upstream_blob
        )
        for patch in patch_paths:
            failure = _apply(patch.resolve(), work, reverse=False)
            if failure:
                errors.append(f"{component.key}: {failure}")
                return errors
        for rel in sorted(targets):
            local_blob = entries[rel].local_blob
            same_bytes = (work / rel).read_bytes() == (source / rel).read_bytes()
            if _blob_id(work / rel) != local_blob or not same_bytes:
                errors.append(
                    f"{component.key}: forward series does not reproduce vendored blob for {rel}"
                )
    return errors


def _pin_value(path: Path, key: str) -> str | None:
    """Read one strict KEY=value pin from a shell-compatible pin file."""
    prefix = key + "="
    rows = [
        line[len(prefix) :].strip().strip("\"'")
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.startswith(prefix)
    ]
    return rows[0] if len(rows) == 1 else None


MIN_ABBREV = 7


def _patch_preimages(patch: Path) -> tuple[tuple[str, str], ...]:
    """Return (path, abbreviated pre-image blob) for every file a patch touches."""
    pairs: list[tuple[str, str]] = []
    current: str | None = None
    for line in patch.read_text(encoding="utf-8").splitlines():
        if line.startswith("diff --git a/"):
            current = line.removeprefix("diff --git a/").split(" b/", 1)[0]
        elif line.startswith("index ") and current is not None:
            pre = line.removeprefix("index ").split("..", 1)[0].strip()
            pairs.append((current, pre))
            current = None
    return tuple(pairs)


def _validate_upstream_binding(
    component: PatchComponent,
    patch_paths: tuple[Path, ...],
    targets_by_patch: tuple[tuple[str, ...], ...],
    pin: str | None,
) -> list[str]:
    """Require every touched file to name the exact blob recorded for the pin.

    Without this the fetched model has no offline proof at all: the series is
    applied with --unidiff-zero, so `git apply --check` still succeeds against
    an upstream whose target files have drifted anywhere outside the hunks.
    """
    recorded = dict(component.upstream_blobs)
    if not recorded:
        return [f"{component.key}: fetched entry records no upstream_blobs for its pin"]
    errors: list[str] = []
    if component.upstream_pin is None:
        errors.append(f"{component.key}: fetched entry records no upstream_pin")
    elif pin is not None and component.upstream_pin != pin:
        errors.append(
            f"{component.key}: upstream_blobs were recorded against "
            f"{component.upstream_pin[:12]}, but the live pin is {pin[:12]}"
        )
    seen: set[str] = set()
    for patch, targets in zip(patch_paths, targets_by_patch, strict=True):
        pairs = _patch_preimages(patch)
        if not pairs:
            errors.append(f"{component.key}: {patch.name} declares no pre-image blob")
        indexed = {rel for rel, _pre in pairs}
        # git's own target list is authoritative: a file with no `index` line
        # would otherwise never enter the comparison at all.
        errors.extend(
            f"{component.key}: {patch.name} touches {rel} with no pre-image blob line"
            for rel in sorted(set(targets) - indexed)
        )
        seen.update(targets)
        for rel, pre in pairs:
            seen.add(rel)
            expected = recorded.get(rel)
            if len(pre) < MIN_ABBREV or not all(c in "0123456789abcdef" for c in pre):
                errors.append(
                    f"{component.key}: {patch.name} pre-image for {rel} is not a usable "
                    f"blob abbreviation: {pre!r}"
                )
            elif expected is None:
                errors.append(
                    f"{component.key}: {patch.name} touches {rel}, which has no recorded "
                    "upstream blob for the pin"
                )
            elif not expected.startswith(pre):
                errors.append(
                    f"{component.key}: {patch.name} expects {rel} at {pre}, but the pin "
                    f"records {expected[: len(pre)]}"
                )
    errors.extend(
        f"{component.key}: upstream_blobs records {rel}, which no patch in the series touches"
        for rel in sorted(set(recorded) - seen)
    )
    return errors


def _validate_fetched(
    component: PatchComponent, patch_paths: tuple[Path, ...], root: Path
) -> list[str]:
    """Verify a fetched dependency connects its pin, series, and build script."""
    apply_script = component.apply_script
    series_token = component.series_token
    pin_file = component.pin_file
    pin_key = component.pin_key
    if apply_script is None or series_token is None or pin_file is None or pin_key is None:
        return [
            f"{component.key}: fetched entry requires apply_script, series_token, "
            "pin_file, and pin_key"
        ]
    targets_by_patch = tuple(_patch_targets(path, root) for path in patch_paths)
    errors = _validate_metadata(component, targets_by_patch)
    script_path = root / apply_script
    pin_path = root / pin_file
    if not script_path.is_file() or not pin_path.is_file():
        return [*errors, f"{component.key}: apply script or pin file is missing"]
    script = script_path.read_text(encoding="utf-8")
    pin = _pin_value(pin_path, pin_key)
    if pin is None or not HEX40_RE.fullmatch(pin):
        errors.append(f"{component.key}: {component.pin_file}:{pin_key} is not one full 40-hex pin")
        pin = None
    errors.extend(_validate_upstream_binding(component, patch_paths, targets_by_patch, pin))
    positions = (
        script.find(pin_key),
        script.find(series_token),
        script.find("git -C"),
        script.find(" apply "),
    )
    if min(positions) < 0 or positions[0] >= positions[1]:
        errors.append(
            f"{component.key}: build script does not connect pin-before-series and git apply"
        )
    direct = re.findall(r"patches/[0-9]{4}-[a-z0-9-]+\.patch", script)
    if direct:
        detail = (
            f"{component.key}: build script hard-codes patches "
            f"instead of consuming series: {direct}"
        )
        errors.append(detail)
    return errors


def check(root: Path = REPO_ROOT) -> list[str]:
    """Return all patch-policy violations in root."""
    try:
        policy = load_policy(root)
    except (TypeError, ValueError) as exc:
        return [str(exc)]
    registry = {
        component.key: component
        for component in REGISTRY
        if component.provenance != PROV_NOT_VENDORED
    }
    declared_patched = {key for key, component in registry.items() if component.patched_files}
    policy_vendored = {component.key for component in policy if component.delivery == "vendored"}
    errors: list[str] = []
    if declared_patched != policy_vendored:
        errors.append(
            f"vendored patch policy keys {sorted(policy_vendored)!r} != "
            f"registry patched keys {sorted(declared_patched)!r}"
        )
    registered_patch_files: set[Path] = set()
    for component in policy:
        try:
            patch_paths = _series_files(component, root)
            registered_patch_files.update(path.relative_to(root) for path in patch_paths)
            if component.delivery == "fetched":
                errors.extend(_validate_fetched(component, patch_paths, root))
                continue
            registry_component = registry.get(component.key)
            if registry_component is None:
                errors.append(
                    f"{component.key}: vendored patch policy has no SBOM registry component"
                )
                continue
            manifest_file = root / manifest_path(component.key)
            manifest = parse_manifest(
                component.key,
                manifest_file.read_text(encoding="utf-8"),
                manifest_path(component.key),
            )
            errors.extend(
                _validate_vendored(component, registry_component, manifest, patch_paths, root)
            )
        except (ManifestError, OSError, ValueError) as exc:
            errors.append(str(exc))
    patch_roots = (root / "docs/sbom/patches", root / "coprocessor")
    discovered = {
        path.relative_to(root)
        for scan_root in patch_roots
        if scan_root.exists()
        for path in scan_root.rglob("*.patch")
        if "build" not in path.parts and "upstream" not in path.parts
    }
    unregistered = discovered - registered_patch_files
    if unregistered:
        rendered = ", ".join(sorted(path.as_posix() for path in unregistered))
        errors.append(f"unregistered first-party patch files: {rendered}")
    return errors


def _selftest_patch() -> str:
    """Return a minimal patch used by the both-directions fixture."""
    return """diff --git a/src/value.c b/src/value.c
index 788b307..e58e70c 100644
--- a/src/value.c
+++ b/src/value.c
@@ -1 +1 @@
-int value = 1;
+int value = 2;
"""


def _selftest_vendored_replay(
    root: Path,
) -> tuple[list[str], list[str], list[str], Path, str, str]:
    """Return the vendored-replay directions plus the shared fetched fixture inputs."""
    patch_dir = root / "patches"
    source = root / "vendor"
    patch_dir = root / "patches"
    (source / "src").mkdir(parents=True)
    patch_dir.mkdir()
    current = source / "src/value.c"
    current.write_text("int value = 2;\n", encoding="utf-8")
    patch = patch_dir / "0001-change-value.patch"
    patch.write_text(_selftest_patch(), encoding="utf-8")
    upstream_blob = hashlib.sha1(  # noqa: S324 -- Git object IDs require SHA-1
        b"blob 15\0int value = 1;\n"
    ).hexdigest()
    local_blob = _blob_id(current)
    entry = SimpleNamespace(
        kind=KIND_PATCH,
        rel_path="src/value.c",
        upstream_blob=upstream_blob,
        local_blob=local_blob,
    )
    manifest = SimpleNamespace(entries=(entry,), by_path=lambda: {entry.rel_path: entry})
    registry_component = SimpleNamespace(
        path="vendor", patched_files=((entry.rel_path, "fixture"),)
    )
    component = PatchComponent(
        "fixture",
        "vendored",
        Path("patches/series"),
        (PatchItem(patch.name, "functional"),),
    )
    clean = _validate_vendored(component, registry_component, manifest, (patch,), root)
    current.write_text("int value = 3;\n", encoding="utf-8")
    drift = _validate_vendored(component, registry_component, manifest, (patch,), root)
    metadata = PatchComponent(
        "fixture",
        "vendored",
        component.series,
        (PatchItem(patch.name, "metadata"),),
    )
    current.write_text("int value = 2;\n", encoding="utf-8")
    mislabeled = _validate_vendored(metadata, registry_component, manifest, (patch,), root)
    return clean, drift, mislabeled, patch_dir, upstream_blob, local_blob


def _fetched_fixture_root(
    base: Path, blobs: dict[str, str], pin: str, recorded_pin: str, patch_text: str
) -> Path:
    """Materialize a complete fetched-component tree for one selftest direction."""
    root = base
    (root / "docs/sbom/patches").mkdir(parents=True, exist_ok=True)
    (root / "coprocessor/fix/patches").mkdir(parents=True, exist_ok=True)
    (root / "coprocessor/fix/patches/0001-fixture.patch").write_text(patch_text, encoding="utf-8")
    (root / "coprocessor/fix/patches/series").write_text("0001-fixture.patch\n", encoding="utf-8")
    (root / "coprocessor/fix/pins.env").write_text(f"FIXTURE_COMMIT={pin}\n", encoding="utf-8")
    (root / "coprocessor/fix/build.sh").write_text(
        '#!/bin/sh\n. ./pins.env\n: "$FIXTURE_COMMIT"\n'
        'while read -r p; do git -C "$c" apply "patches/$p"; done < patches/series\n',
        encoding="utf-8",
    )
    rows = "\n".join(f'"{rel}" = "{blob}"' for rel, blob in sorted(blobs.items()))
    (root / "docs/sbom/patches/registry.toml").write_text(
        "schema_version = 1\n\n[[component]]\n"
        'key = "fixture"\ndelivery = "fetched"\n'
        'series = "coprocessor/fix/patches/series"\n'
        'apply_script = "coprocessor/fix/build.sh"\n'
        'series_token = "patches/series"\n'
        'pin_file = "coprocessor/fix/pins.env"\n'
        'pin_key = "FIXTURE_COMMIT"\n'
        + (f'upstream_pin = "{recorded_pin}"\n' if recorded_pin else "")
        + (f"\n[component.upstream_blobs]\n{rows}\n" if rows else "")
        + '\n[[component.patches]]\nfile = "0001-fixture.patch"\n'
        'classification = "functional"\n',
        encoding="utf-8",
    )
    return root


@dataclass(frozen=True)
class _FetchedFixture:
    """One fetched-component selftest direction, as data."""

    name: str
    blobs: dict[str, str]
    pin: str
    recorded_pin: str
    patch_text: str


def _fetched_binding_errors(base: Path, case: _FetchedFixture) -> list[str]:
    """Run the REAL entry point CI runs and return only fetched-binding errors.

    Driving check() rather than the private helper is the point: deleting the
    production call site must make these cases fail, which calling the helper
    directly could never detect.
    """
    root = _fetched_fixture_root(
        base / case.name, case.blobs, case.pin, case.recorded_pin, case.patch_text
    )
    return [error for error in check(root) if not error.startswith("vendored patch policy keys")]


def _selftest_fetched_binding(
    base: Path, upstream_blob: str, local_blob: str
) -> dict[str, list[str]]:
    """Return every direction of the fetched pin binding, keyed by case name."""
    pin = "9" * 40
    other = ("f" if upstream_blob[0] != "f" else "0") + upstream_blob[1:]
    good = (
        "diff --git a/src/value.c b/src/value.c\n"
        f"index {upstream_blob[:7]}..{local_blob[:7]} 100644\n"
        "--- a/src/value.c\n+++ b/src/value.c\n"
        "@@ -1 +1 @@\n-int value = 1;\n+int value = 2;\n"
    )
    blank = good.replace(f"index {upstream_blob[:7]}..", "index ..", 1)
    extra = good + (
        "diff --git a/src/other.c b/src/other.c\n"
        "--- a/src/other.c\n+++ b/src/other.c\n"
        "@@ -1 +1 @@\n-int other = 1;\n+int other = 2;\n"
    )
    one = {"src/value.c": upstream_blob}
    return {
        "clean": _fetched_binding_errors(base, _FetchedFixture("clean", one, pin, pin, good)),
        "drift": _fetched_binding_errors(
            base, _FetchedFixture("drift", {"src/value.c": other}, pin, pin, good)
        ),
        "unrecorded": _fetched_binding_errors(
            base, _FetchedFixture("unrec", {"src/other.c": other}, pin, pin, good)
        ),
        "empty": _fetched_binding_errors(base, _FetchedFixture("empty", {}, pin, pin, good)),
        "pin-moved": _fetched_binding_errors(
            base, _FetchedFixture("pinmv", one, "1" * 40, pin, good)
        ),
        "blank-preimage": _fetched_binding_errors(
            base, _FetchedFixture("blank", one, pin, pin, blank)
        ),
        "no-index-line": _fetched_binding_errors(
            base, _FetchedFixture("noidx", one, pin, pin, extra)
        ),
    }


def selftest() -> int:
    """Prove exact replay and representative failure directions."""
    with tempfile.TemporaryDirectory(prefix="ra8-patch-selftest-") as raw_tmp:
        root = Path(raw_tmp)
        clean, drift, mislabeled, _patch_dir, upstream_blob, local_blob = _selftest_vendored_replay(
            root
        )

        fetched = _selftest_fetched_binding(root / "fetched", upstream_blob, local_blob)
    failures = []
    if fetched["clean"]:
        failures.append(f"recorded pin blob was rejected: {fetched['clean']}")
    failures.extend(
        f"fetched pin binding did not fail: {case}"
        for case in ("drift", "unrecorded", "empty", "pin-moved", "blank-preimage", "no-index-line")
        if not fetched[case]
    )
    if clean:
        failures.append(f"clean fixture failed: {clean}")
    if not drift:
        failures.append("vendored-byte drift did not fail")
    if not any("classified metadata" in error for error in mislabeled):
        failures.append("metadata classification on source did not fail")
    if failures:
        print("check_third_party_patches: selftest FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(
        "check_third_party_patches: selftest passed "
        "(vendored replay + fetched pin binding, both directions)."
    )
    return 0


def main() -> int:
    """CLI entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--selftest", action="store_true", help="run isolated both-directions tests"
    )
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    errors = check()
    if errors:
        print(
            f"check_third_party_patches: FAIL ({len(errors)} finding(s))",
            file=sys.stderr,
        )
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("check_third_party_patches: PASS -- every reviewed series reproduces its declared bytes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
