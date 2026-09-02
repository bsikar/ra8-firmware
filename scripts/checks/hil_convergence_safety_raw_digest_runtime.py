# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Adversarial runtime cases for the privileged raw-digest file reader."""

from __future__ import annotations

import errno
import hashlib
import os
import shutil
import tempfile
import time
from collections.abc import Callable, Mapping
from pathlib import Path, PurePosixPath

import hil_convergence_safety_image_lock_digest as digest
import hil_convergence_safety_raw_digest_fixtures as raw_digest_fixtures

Case = tuple[str, bool]

_LABEL_BY_PATH = {path: label for path, _pin, label, _mode in digest.target_specs()}
_MAIN_LABEL = _LABEL_BY_PATH[digest.DEVCONTAINER_IMAGE_PATH]
_RAW_FILE_LABEL = f"raw digest authority file: {digest.DEVCONTAINER_IMAGE_PATH}"
_FAILURE_ATTEMPTS = 16


def _path_name(pin_name: str) -> str:
    """Return the direct path authority corresponding to one digest pin."""
    return pin_name.removesuffix("_RAW_SHA256") + "_PATH"


def _payloads(inputs: Mapping[str, str]) -> dict[str, bytes]:
    """Return exact fixture bytes for every bound surface."""
    return {
        path: inputs[key].encode("utf-8") for path, key in raw_digest_fixtures.INPUT_BY_PATH.items()
    }


def _values(
    payloads: Mapping[str, bytes], path_overrides: Mapping[str, str] | None = None
) -> dict[str, object]:
    """Build exact test authorities without executing candidate source."""
    values = digest.authority_values()
    for path, pin_name, _label, _mode in digest.target_specs():
        effective = path if path_overrides is None else path_overrides.get(path, path)
        values[_path_name(pin_name)] = effective
        values[pin_name] = hashlib.sha256(payloads[path]).hexdigest()
    return values


def _write_fixture(root: Path, payloads: Mapping[str, bytes], values: Mapping[str, object]) -> None:
    """Write a root-owned-by-caller fixture with exact portable modes."""
    root.mkdir(mode=0o755)
    root.chmod(0o755)
    by_pin = {pin: original for original, pin, _label, _mode in digest.target_specs()}
    for effective, pin_name, _label, mode in digest.target_specs(values):
        original = by_pin[pin_name]
        pure = PurePosixPath(effective)
        if pure.is_absolute() or ".." in pure.parts:
            continue
        target = root / effective
        target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        ancestor = target.parent
        while ancestor != root:
            ancestor.chmod(0o755)
            ancestor = ancestor.parent
        target.write_bytes(payloads[original])
        target.chmod(mode)


def _with_fixture(
    inputs: Mapping[str, str],
    operation: Callable[[Path, dict[str, bytes], dict[str, object]], bool],
    *,
    path_overrides: Mapping[str, str] | None = None,
) -> bool:
    """Run one isolated case and recover renamed roots before cleanup."""
    container = Path(tempfile.mkdtemp(prefix="ra8-raw-digest-"))
    root = container / "repo"
    payloads = _payloads(inputs)
    values = _values(payloads, path_overrides)
    try:
        _write_fixture(root, payloads, values)
        return operation(root, payloads, values)
    finally:
        saved = container / "repo.saved"
        if saved.exists():
            if root.exists() or root.is_symlink():
                if root.is_dir() and not root.is_symlink():
                    shutil.rmtree(root)
                else:
                    root.unlink()
            saved.rename(root)
        shutil.rmtree(container)


def _exact_error(errors: list[str], text: str) -> bool:
    """Require one byte-exact stable diagnostic."""
    return errors == [text]


def _source_tuple(inputs: Mapping[str, str]) -> tuple[str, ...]:
    """Return all bound sources in production target order."""
    ordered = (
        inputs[raw_digest_fixtures.INPUT_BY_PATH[path]]
        for path, _pin, _label, _mode in digest.target_specs()
    )
    return tuple(ordered)


def _baseline_case(inputs: Mapping[str, str]) -> bool:
    """Require the exact complete fixture to pass without diagnostics."""
    return _with_fixture(
        inputs,
        lambda root, _payloads_arg, values: not digest.audit_live_errors(root, values),
    )


def _surface_digest_case(inputs: Mapping[str, str], target_path: str) -> bool:
    """Require a byte change on each bound surface to fire once."""

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        target = root / target_path
        target.write_bytes(target.read_bytes() + b"\n")
        errors = digest.audit_live_errors(root, values)
        return _exact_error(
            errors,
            f"{_LABEL_BY_PATH[target_path]}: raw bytes differ from exact audited digest",
        )

    return _with_fixture(inputs, operation)


def _unsafe_relative_case(inputs: Mapping[str, str], unsafe: str) -> bool:
    """Require absolute and parent-traversing target authorities to fail."""
    original = digest.DEVCONTAINER_IMAGE_PATH

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        errors = digest.audit_live_errors(root, values)
        diagnostic = (
            f"{_MAIN_LABEL}: bound path must be a fixed normalized relative path"
            if unsafe.startswith("/") or ".." in PurePosixPath(unsafe).parts
            else f"{_MAIN_LABEL}: bound path differs from fixed repository authority"
        )
        return _exact_error(errors, diagnostic)

    return _with_fixture(inputs, operation, path_overrides={original: unsafe})


def _final_path_case(inputs: Mapping[str, str], kind: str) -> bool:
    """Require final symlink, hardlink, directory, FIFO, and missing refusal."""
    target_path = digest.DEVCONTAINER_IMAGE_PATH

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        target = root / target_path
        saved = target.with_name("saved-authority")
        target.rename(saved)
        if kind == "symlink":
            target.symlink_to(saved.name)
        elif kind == "hardlink":
            target.hardlink_to(saved)
        elif kind == "directory":
            target.mkdir()
        elif kind == "fifo":
            os.mkfifo(target, 0o755)
        elif kind != "missing":
            raise ValueError(kind)
        started = time.monotonic()
        errors = digest.audit_live_errors(root, values)
        elapsed = time.monotonic() - started
        diagnostic = (
            f"{_MAIN_LABEL}: cannot inspect or open bound bytes safely: {errno.ENOENT}"
            if kind == "missing"
            else f"{_MAIN_LABEL}: bound path is absent, linked, or non-regular"
        )
        return elapsed < 1.0 and _exact_error(errors, diagnostic)

    return _with_fixture(inputs, operation)


def _parent_symlink_case(inputs: Mapping[str, str]) -> bool:
    """Require a no-follow refusal when an intermediate directory is linked."""
    original = digest.DEVCONTAINER_IMAGE_PATH
    attacked = "scripts/raw-audit/ci/devcontainer_image.sh"  # PATHREF-OK: synthetic link attack

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        parent = (root / attacked).parent
        saved = parent.with_name("ci.saved")
        parent.rename(saved)
        parent.symlink_to(saved.name)
        hooks = digest.RawReadTestHooks(allow_alternate_fixed_paths=True)
        errors = digest.audit_live_errors(root, values, hooks=hooks)
        return _exact_error(
            errors,
            "raw digest authority directory: "
            f"{attacked}: bound path is absent, linked, or non-regular",
        )

    return _with_fixture(inputs, operation, path_overrides={original: attacked})


def _root_symlink_case(inputs: Mapping[str, str]) -> bool:
    """Require the repository root itself to be an unlinked directory."""

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        link = root.with_name("repo-link")
        link.symlink_to(root.name)
        errors = digest.audit_live_errors(link, values)
        return _exact_error(errors, "raw digest authority root: repository root is not a directory")

    return _with_fixture(inputs, operation)


def _root_replacement_case(inputs: Mapping[str, str]) -> bool:
    """Require a post-open root replacement to fail without reading it."""

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        saved = root.with_name("repo.saved")

        def replace(opened_root: Path, _fd: int) -> None:
            opened_root.rename(saved)
            opened_root.mkdir(mode=0o755)

        hooks = digest.RawReadTestHooks(after_root_open=replace)
        errors = digest.audit_live_errors(root, values, hooks=hooks)
        return _exact_error(
            errors,
            "raw digest authority root: path changed during retained-root audit",
        )

    return _with_fixture(inputs, operation)


def _parent_replacement_case(inputs: Mapping[str, str]) -> bool:
    """Require a post-open parent replacement to fail the identity rewalk."""
    original = digest.DEVCONTAINER_IMAGE_PATH
    attacked = "scripts/raw-audit/ci/devcontainer_image.sh"  # PATHREF-OK: synthetic replace attack

    def operation(root: Path, payloads: dict[str, bytes], values: dict[str, object]) -> bool:
        changed = False

        def replace(relative: str, _fd: int) -> None:
            nonlocal changed
            if relative != attacked or changed:
                return
            changed = True
            parent = (root / attacked).parent
            saved = parent.with_name("ci.saved")
            parent.rename(saved)
            parent.mkdir(mode=0o755)
            replacement = parent / Path(attacked).name
            replacement.write_bytes(payloads[original])
            replacement.chmod(0o755)

        hooks = digest.RawReadTestHooks(
            allow_alternate_fixed_paths=True,
            after_file_open=replace,
        )
        errors = digest.audit_live_errors(root, values, hooks=hooks)
        return changed and _exact_error(
            errors,
            f"raw digest authority file: {attacked}: parent changed during raw-byte read",
        )

    return _with_fixture(inputs, operation, path_overrides={original: attacked})


def _capability_case(inputs: Mapping[str, str], name: str) -> bool:
    """Require each unavailable platform primitive to fail before opening."""

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        hooks = digest.RawReadTestHooks(missing_capability=name)
        errors = digest.audit_live_errors(root, values, hooks=hooks)
        return _exact_error(
            errors,
            f"raw digest authority: required platform capability is unavailable: {name}",
        )

    return _with_fixture(inputs, operation)


def _authority_case(inputs: Mapping[str, str], kind: str) -> bool:
    """Require owner, group, and exact-mode mismatches to fail closed."""

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        root_stat = root.stat()
        authority = digest.RawReadAuthority(
            root_uid=root_stat.st_uid,
            root_gid=root_stat.st_gid,
            file_uid=root_stat.st_uid + (1 if kind == "owner" else 0),
            file_gid=root_stat.st_gid + (1 if kind == "group" else 0),
        )
        if kind == "mode":
            (root / digest.DEVCONTAINER_IMAGE_PATH).chmod(0o775)
        errors = digest.audit_live_errors(root, values, authority=authority)
        diagnostic = {
            "owner": "bound path owner differs from repository authority",
            "group": "bound path group differs from repository authority",
            "mode": "bound path mode differs from exact audited mode",
        }[kind]
        if kind in {"owner", "group"}:
            expected = [
                f"{label}: {diagnostic}" for _path, _pin, label, _mode in digest.target_specs()
            ]
            return errors == expected
        return _exact_error(errors, f"{_MAIN_LABEL}: {diagnostic}")

    return _with_fixture(inputs, operation)


def _oversize_case(inputs: Mapping[str, str]) -> bool:
    """Require the fixed maximum to stop an oversized regular file."""
    target_path = digest.DEVCONTAINER_IMAGE_PATH

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        oversized = b"x" * (digest.maximum_authority_bytes() + 1)
        (root / target_path).write_bytes(oversized)
        values["DEVCONTAINER_IMAGE_RAW_SHA256"] = hashlib.sha256(oversized).hexdigest()
        errors = digest.audit_live_errors(root, values)
        return _exact_error(errors, f"{_RAW_FILE_LABEL}: bound file exceeds maximum audited size")

    return _with_fixture(inputs, operation)


def _race_diagnostic_matches(kind: str, errors: list[str]) -> bool:
    """Require one exact legitimate diagnostic for a file-race attack."""
    expected = {
        "shrink": f"{_RAW_FILE_LABEL}: bound file shrank during bounded raw-byte read",
        "extra": f"{_RAW_FILE_LABEL}: bound file has bytes beyond its audited pre-read size",
        "growth": f"{_MAIN_LABEL}: bound file metadata changed during bounded raw-byte read",
        "timestamp": f"{_MAIN_LABEL}: bound file metadata changed during bounded raw-byte read",
    }
    if kind != "rebound":
        return _exact_error(errors, expected[kind])
    legitimate = {
        (f"{_RAW_FILE_LABEL}: path changed during raw-byte read",),
        (f"{_RAW_FILE_LABEL}: parent changed during raw-byte read",),
    }
    return tuple(errors) in legitimate


def _file_race_case(inputs: Mapping[str, str], kind: str) -> bool:
    """Require shrink, extra growth, timestamp, and path-rebound detection."""
    target_path = digest.DEVCONTAINER_IMAGE_PATH

    def operation(root: Path, payloads: dict[str, bytes], values: dict[str, object]) -> bool:
        changed = False
        target = root / target_path

        def after_open(relative: str, _fd: int) -> None:
            nonlocal changed
            if relative != target_path or changed or kind not in {"shrink", "extra"}:
                return
            changed = True
            if kind == "shrink":
                target.write_bytes(payloads[target_path][:1])
            else:
                with target.open("ab") as stream:
                    stream.write(b"x")

        def after_read(relative: str, _fd: int) -> None:
            nonlocal changed
            if relative != target_path or changed or kind not in {"growth", "timestamp"}:
                return
            changed = True
            if kind == "growth":
                with target.open("ab") as stream:
                    stream.write(b"x")
            else:
                current = target.stat()
                os.utime(target, ns=(current.st_atime_ns, current.st_mtime_ns + 1_000_000))

        def rebound(relative: str) -> None:
            nonlocal changed
            if relative != target_path or changed or kind != "rebound":
                return
            changed = True
            saved = target.with_name("original-authority")
            target.rename(saved)
            target.write_bytes(payloads[target_path])
            target.chmod(0o755)

        hooks = digest.RawReadTestHooks(
            after_file_open=after_open,
            after_read=after_read,
            before_post_rewalk=rebound,
        )
        errors = digest.audit_live_errors(root, values, hooks=hooks)
        return changed and _race_diagnostic_matches(kind, errors)

    return _with_fixture(inputs, operation)


def _fd_set() -> set[int] | None:
    """Return the Linux descriptor set when procfs is available."""
    proc = Path("/proc/self/fd")
    if not proc.is_dir():
        return None
    return {int(entry.name) for entry in proc.iterdir() if entry.name.isdigit()}


def _descriptor_case(inputs: Mapping[str, str], mode: str) -> bool:
    """Require exhaustive normal closes and a fail-closed close diagnostic."""

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        before = _fd_set()
        close_failure = mode == "close-failure"
        errors: list[str] = []
        raised_count = 0
        for _attempt in range(_FAILURE_ATTEMPTS):
            raised_this_audit = False

            def fail_after_close(_fd: int) -> None:
                nonlocal raised_count, raised_this_audit
                if not raised_this_audit:
                    raised_this_audit = True
                    raised_count += 1
                    raise RuntimeError(raw_digest_fixtures.PRE_CLOSE_FAILURE)

            hooks = (
                digest.RawReadTestHooks(after_close_fd=fail_after_close) if close_failure else None
            )
            errors = digest.audit_live_errors(root, values, hooks=hooks)
        after = _fd_set()
        residue_free = before is None or before == after
        if close_failure:
            return (
                raised_count == _FAILURE_ATTEMPTS
                and residue_free
                and _exact_error(
                    errors,
                    f"{_RAW_FILE_LABEL}: descriptor-close hook failed",
                )
            )
        return residue_free and not errors

    return _with_fixture(inputs, operation)


def _ambiguous_close_attempt(
    root: Path,
    values: dict[str, object],
) -> bool:
    """Prove a post-close FD reuse is never closed by stale ledger authority."""
    before = _fd_set()
    replacement = -1
    reused = False

    def reuse_then_fail(released: int) -> None:
        nonlocal replacement, reused
        if replacement >= 0:
            return
        replacement = os.open(os.devnull, os.O_RDONLY | os.O_CLOEXEC)
        if replacement != released:
            message = "released descriptor number was not reused"
            raise RuntimeError(message)
        reused = True
        message = "injected post-close failure"
        raise OSError(errno.EIO, message)

    hooks = digest.RawReadTestHooks(after_close_fd=reuse_then_fail)
    try:
        errors = digest.audit_live_errors(root, values, hooks=hooks)
        replacement_live = replacement >= 0 and os.fstat(replacement) is not None
        expected = f"{_RAW_FILE_LABEL}: descriptor-close hook failed"
        return reused and replacement_live and _exact_error(errors, expected)
    finally:
        if replacement >= 0:
            os.close(replacement)
        after = _fd_set()
        if before is not None and after != before:
            message = "ambiguous close fixture leaked a descriptor"
            raise RuntimeError(message)


def _ambiguous_close_case(inputs: Mapping[str, str]) -> bool:
    """Repeat the reused-FD proof across independent fixture roots."""
    return all(
        _with_fixture(
            inputs,
            lambda root, _payloads_arg, values: _ambiguous_close_attempt(root, values),
        )
        for _attempt in range(_FAILURE_ATTEMPTS)
    )


def _root_hook_failure_case(inputs: Mapping[str, str]) -> bool:
    """Require root-open hook exceptions to close the retained descriptor."""

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        before = _fd_set()

        def fail_after_root_open(_root: Path, _fd: int) -> None:
            raise RuntimeError(raw_digest_fixtures.ROOT_OPEN_FAILURE)

        hooks = digest.RawReadTestHooks(after_root_open=fail_after_root_open)
        errors: list[str] = []
        for _attempt in range(_FAILURE_ATTEMPTS):
            errors = digest.audit_live_errors(root, values, hooks=hooks)
        after = _fd_set()
        return (before is None or before == after) and _exact_error(
            errors,
            "raw digest authority root: cannot inspect or open safely: hook",
        )

    return _with_fixture(inputs, operation)


def _directory_authority_case(inputs: Mapping[str, str], kind: str) -> bool:
    """Require safe root and exact intermediate-directory metadata."""

    def operation(root: Path, _payloads_arg: dict[str, bytes], values: dict[str, object]) -> bool:
        root_modes = {
            "root-mode": 0o775,
            "root-owner-permission": 0o400,
            "root-private-mode": 0o700,
            "root-group-private-mode": 0o750,
            "root-readonly-mode": 0o555,
        }
        if kind in root_modes:
            root.chmod(root_modes[kind])
            try:
                errors = digest.audit_live_errors(root, values)
            finally:
                root.chmod(0o755)
            if kind in {"root-mode", "root-owner-permission"}:
                return _exact_error(
                    errors,
                    "raw digest authority root: "
                    "repository root mode is not safe for the audited authority",
                )
            return errors == []
        (root / "scripts").chmod(0o750)
        expected = [
            f"raw digest authority directory: {path}: "
            "bound path mode differs from exact audited mode"
            for path, _pin, _label, _mode in digest.target_specs()
        ]
        return digest.audit_live_errors(root, values) == expected

    return _with_fixture(inputs, operation)


def _implementation_cases(inputs: Mapping[str, str]) -> list[Case]:
    """Prove each live reader control is load-bearing without pin changes."""
    authority_source = inputs["image_lock_digest"]
    controls = digest.implementation_controls()
    mutations = digest.implementation_mutations(authority_source)
    results = [
        (
            "raw digest implementation controls are uniquely present",
            not digest.implementation_errors(authority_source)
            and len(mutations) == len(controls)
            and len({control.label for control in controls}) == len(controls),
        )
    ]
    sources = _source_tuple(inputs)
    results.extend(
        (
            f"raw digest control mutation fires: {label}",
            digest.source_errors(sources, mutant) == [expected],
        )
        for label, mutant, expected in mutations
    )
    return results


def _path_cases(inputs: Mapping[str, str]) -> list[Case]:
    """Return fixed-path, type, parent, and root attack cases."""
    results = [
        (
            f"raw digest unsafe relative target refused: {unsafe}",
            _unsafe_relative_case(inputs, unsafe),
        )
        for unsafe in (
            "/outside/devcontainer_image.sh",
            "scripts/../devcontainer_image.sh",  # PATHREF-OK: traversal-refusal fixture
        )
    ]
    results.append(
        (
            "raw digest normalized but noncanonical target refused",
            _unsafe_relative_case(
                inputs,
                "scripts/ci/other_authority.sh",  # PATHREF-OK: synthetic wrong-authority fixture
            ),
        )
    )
    results.extend(
        (f"raw digest final {kind} refused without blocking", _final_path_case(inputs, kind))
        for kind in ("symlink", "hardlink", "directory", "fifo", "missing")
    )
    results.extend(
        (
            ("raw digest linked parent refused", _parent_symlink_case(inputs)),
            ("raw digest linked root refused", _root_symlink_case(inputs)),
            ("raw digest replaced parent detected", _parent_replacement_case(inputs)),
            ("raw digest replaced root detected", _root_replacement_case(inputs)),
        )
    )
    return results


def _capability_cases(inputs: Mapping[str, str]) -> list[Case]:
    """Return platform-capability and metadata-authority cases."""
    capabilities = (
        "O_DIRECTORY",
        "O_NOFOLLOW",
        "O_CLOEXEC",
        "O_NONBLOCK",
        "open_dir_fd",
        "stat_dir_fd",
        "stat_follow_symlinks",
        "pread",
    )
    results = [
        (f"raw digest missing capability refused: {name}", _capability_case(inputs, name))
        for name in capabilities
    ]
    results.extend(
        (f"raw digest {kind} authority mismatch refused", _authority_case(inputs, kind))
        for kind in ("owner", "group", "mode")
    )
    # Root-owner-permission is (T, F), root-mode is (F, T), and the three
    # accepted root modes are (F, F) for the two independent policy terms.
    # No mode can make both terms true without already being rejected by either.
    results.extend(
        (
            f"raw digest {kind} authority policy holds",
            _directory_authority_case(inputs, kind),
        )
        for kind in (
            "root-mode",
            "root-owner-permission",
            "root-private-mode",
            "root-group-private-mode",
            "root-readonly-mode",
            "intermediate-mode",
        )
    )
    results.append(("raw digest oversize authority refused", _oversize_case(inputs)))
    return results


def _race_and_close_cases(inputs: Mapping[str, str]) -> list[Case]:
    """Return bounded-read race and exhaustive-close cases."""
    results = [
        (f"raw digest {kind} race detected", _file_race_case(inputs, kind))
        for kind in ("shrink", "extra", "growth", "timestamp")
    ]
    results.extend(
        (
            (
                "raw digest rebound race repeatedly detects one exact identity change",
                all(_file_race_case(inputs, "rebound") for _attempt in range(_FAILURE_ATTEMPTS)),
            ),
            (
                "raw digest descriptor audit leaves no residue",
                _descriptor_case(inputs, "normal"),
            ),
            (
                "raw digest close failure is diagnosed without residue",
                _descriptor_case(inputs, "close-failure"),
            ),
            (
                "raw digest root hook failure is diagnosed without residue",
                _root_hook_failure_case(inputs),
            ),
            (
                "raw digest ambiguous close cannot consume reused descriptor",
                _ambiguous_close_case(inputs),
            ),
        )
    )
    return results


def cases(inputs: Mapping[str, str]) -> list[Case]:
    """Return complete two-sided cases for the live raw-digest reader."""
    results: list[Case] = [
        ("raw digest exact complete-surface baseline passes", _baseline_case(inputs))
    ]
    results.extend(
        (
            f"raw digest byte mutation fires: {path}",
            _surface_digest_case(inputs, path),
        )
        for path in raw_digest_fixtures.INPUT_BY_PATH
    )
    return (
        results
        + _path_cases(inputs)
        + _capability_cases(inputs)
        + _race_and_close_cases(inputs)
        + _implementation_cases(inputs)
    )
