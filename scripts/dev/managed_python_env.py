#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Create and verify the root-owned managed Python environment receipt."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
import re
import shutil
import stat
import sys
import tempfile
import time
import unicodedata
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Any, NoReturn, Protocol


class ManagedEnvironmentError(RuntimeError):
    """Report a failed managed-environment authentication check."""


@dataclass(frozen=True)
class TrustPolicy:
    """Describe the owners accepted while checking filesystem objects."""

    environment_uid: int
    route_uids: frozenset[int]

    @classmethod
    def production(cls) -> TrustPolicy:
        """Return the root-only production trust policy."""
        return cls(environment_uid=0, route_uids=frozenset({0}))

    @classmethod
    def selftest(cls) -> TrustPolicy:
        """Permit the current account only inside the private selftest tree."""
        uid = os.getuid()
        return cls(environment_uid=uid, route_uids=frozenset({0, uid}))


@dataclass(frozen=True)
class ObjectIdentity:
    """Capture fields whose change means a checked object was replaced."""

    device: int
    inode: int
    mode: int
    uid: int
    size: int
    mtime_ns: int
    ctime_ns: int

    @classmethod
    def from_stat(cls, value: os.stat_result) -> ObjectIdentity:
        """Build an identity from one stat result."""
        return cls(
            device=value.st_dev,
            inode=value.st_ino,
            mode=value.st_mode,
            uid=value.st_uid,
            size=value.st_size,
            mtime_ns=value.st_mtime_ns,
            ctime_ns=value.st_ctime_ns,
        )

    def cache_fields(self) -> tuple[int, ...]:
        """Return every identity field used to invalidate a warm authentication."""
        return (
            self.device,
            self.inode,
            self.mode,
            self.uid,
            self.size,
            self.mtime_ns,
            self.ctime_ns,
        )


@dataclass(frozen=True)
class InterpreterProbe:
    """Hold identity facts reported by the authenticated interpreter."""

    implementation: str
    version: str
    installed_sha256: str


@dataclass(frozen=True)
class EnvironmentTreeIdentity:
    """Bind every trusted object and regular-file byte under the environment."""

    sha256: str
    entries: int
    regular_file_bytes: int


@dataclass(frozen=True)
class TreeTrustContext:
    """Hold the trust boundary used while authenticating nested tree objects."""

    environment: Path
    interpreter_target: Path
    policy: TrustPolicy


class DigestWriter(Protocol):
    """Describe the only hash-object operation used by record framing."""

    def update(self, data: bytes) -> None:
        """Append bytes to the digest state."""


@dataclass(frozen=True)
class CommandResult:
    """Hold captured output and the normalized process status."""

    returncode: int
    stdout: str
    stderr: str


@dataclass(frozen=True)
class ReceiptSources:
    """Name the environment and locked inputs bound into one receipt."""

    environment: Path
    pyproject: Path
    lockfile: Path
    group: str


def _run_command(
    arguments: list[str], environment: dict[str, str], timeout_seconds: int
) -> CommandResult:
    """Run one absolute executable without a shell and capture both streams."""
    executable = Path(arguments[0])
    if not executable.is_absolute():
        _fail(f"refusing to execute a non-absolute command: {executable}")
    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        actions = (
            (os.POSIX_SPAWN_DUP2, stdout_file.fileno(), 1),
            (os.POSIX_SPAWN_DUP2, stderr_file.fileno(), 2),
        )
        try:
            process = os.posix_spawn(str(executable), arguments, environment, file_actions=actions)
        except OSError as error:
            _fail(f"cannot execute {executable}: {error}")
        deadline = time.monotonic() + timeout_seconds
        status = 0
        while True:
            waited, status = os.waitpid(process, os.WNOHANG)
            if waited == process:
                break
            if time.monotonic() >= deadline:
                os.kill(process, 9)
                os.waitpid(process, 0)
                _fail(f"command timed out after {timeout_seconds}s: {executable}")
            time.sleep(0.01)
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read().decode("utf-8", errors="replace")
        stderr = stderr_file.read().decode("utf-8", errors="replace")
    returncode = os.waitstatus_to_exitcode(status)
    return CommandResult(returncode, stdout, stderr)


RECEIPT_NAME = ".ra8-managed-python-v1.json"
RECEIPT_MODE = 0o444


def _fail(message: str) -> NoReturn:
    """Raise one consistently typed authentication failure."""
    raise ManagedEnvironmentError(message)


def _load_checks() -> ModuleType:
    """Load the adjacent QA helper explicitly even under isolated Python mode."""
    name = "_ra8_managed_python_env_checks"
    loaded = sys.modules.get(name)
    if isinstance(loaded, ModuleType):
        return loaded
    path = Path(__file__).with_name("managed_python_env_checks.py")
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        _fail(f"cannot load managed-environment QA helper: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _identity(path: Path, *, follow_symlinks: bool = False) -> ObjectIdentity:
    """Read an object's stable identity without following links by default."""
    try:
        value = path.stat() if follow_symlinks else path.lstat()
    except OSError as error:
        _fail(f"cannot stat {path}: {error}")
    return ObjectIdentity.from_stat(value)


def _require_owner_mode(
    path: Path,
    identity: ObjectIdentity,
    owners: frozenset[int],
    *,
    allow_symlink: bool = False,
) -> None:
    """Reject an unsafe owner, writable route, or unexpected object kind."""
    if identity.uid not in owners:
        _fail(f"untrusted owner for {path}: uid {identity.uid}")
    if stat.S_ISLNK(identity.mode):
        if not allow_symlink:
            _fail(f"symlink is forbidden in managed environment route: {path}")
        return
    if identity.mode & 0o022:
        _fail(f"group/other-writable managed environment route: {path}")


def _path_components(path: Path) -> list[Path]:
    """Return every absolute route component from root through path."""
    components = [Path(path.anchor)]
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        components.append(current)
    return components


def _validate_route(path: Path, owners: frozenset[int]) -> dict[Path, ObjectIdentity]:
    """Validate every route component and return its pre-use identity."""
    snapshots: dict[Path, ObjectIdentity] = {}
    for component in _path_components(path):
        identity = _identity(component)
        if not stat.S_ISDIR(identity.mode):
            _fail(f"managed environment route component is not a directory: {component}")
        _require_owner_mode(component, identity, owners)
        snapshots[component] = identity
    return snapshots


def _canonical_environment(raw: str) -> Path:
    """Require an existing absolute path already in canonical spelling."""
    if not raw or not Path(raw).is_absolute() or raw == os.sep:
        _fail(f"managed environment must be a non-root absolute path: {raw!r}")
    if any(unicodedata.category(character) == "Cc" for character in raw):
        _fail("managed environment path contains a control character")
    normalized = os.path.normpath(raw)
    if raw != normalized:
        _fail(f"managed environment path is not canonical: {raw}")
    try:
        resolved = str(Path(raw).resolve(strict=True))
    except OSError as error:
        _fail(f"managed environment does not resolve: {raw}: {error}")
    if resolved != raw:
        _fail(f"managed environment route contains a symlink: {raw} -> {resolved}")
    return Path(raw)


def _safe_file_bytes(path: Path) -> tuple[bytes, ObjectIdentity]:
    """Read one regular file through a no-follow descriptor and recheck it."""
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        _fail(f"cannot open trusted file {path}: {error}")
    try:
        before = ObjectIdentity.from_stat(os.fstat(descriptor))
        if not stat.S_ISREG(before.mode):
            _fail(f"trusted file is not regular: {path}")
        chunks: list[bytes] = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        after = ObjectIdentity.from_stat(os.fstat(descriptor))
        if before != after:
            _fail(f"trusted file changed while it was read: {path}")
        return b"".join(chunks), before
    finally:
        os.close(descriptor)


def _sha256_file(path: Path) -> str:
    """Hash one no-follow regular file."""
    data, _identity_value = _safe_file_bytes(path)
    return hashlib.sha256(data).hexdigest()


def _tree_digest_record(
    digest: DigestWriter,
    kind: bytes,
    relative: Path,
    identity: ObjectIdentity,
    payload: bytes,
) -> None:
    """Append one length-delimited filesystem record to the tree digest."""
    fields = (
        kind,
        os.fsencode(str(relative)),
        stat.S_IMODE(identity.mode).to_bytes(4, "big"),
        identity.uid.to_bytes(8, "big"),
        (identity.size if kind != b"d" else 0).to_bytes(8, "big"),
        payload,
    )
    for field in fields:
        digest.update(len(field).to_bytes(8, "big"))
        digest.update(field)


def _trusted_regular_file_digest(path: Path, policy: TrustPolicy) -> tuple[bytes, ObjectIdentity]:
    """Hash one immutable environment file while holding a no-follow descriptor."""
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        _fail(f"cannot open managed environment file {path}: {error}")
    try:
        before = ObjectIdentity.from_stat(os.fstat(descriptor))
        if not stat.S_ISREG(before.mode):
            _fail(f"managed environment file is not regular: {path}")
        _require_owner_mode(path, before, frozenset({policy.environment_uid}))
        digest = hashlib.sha256()
        remaining = before.size
        while remaining:
            chunk = os.read(descriptor, min(1024 * 1024, remaining))
            if not chunk:
                _fail(f"managed environment file was truncated while hashing: {path}")
            digest.update(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            _fail(f"managed environment file grew while hashing: {path}")
        after = ObjectIdentity.from_stat(os.fstat(descriptor))
        if before != after:
            _fail(f"managed environment file changed while hashing: {path}")
        return digest.digest(), before
    finally:
        os.close(descriptor)


def _trusted_tree_symlink(
    path: Path,
    relative: Path,
    identity: ObjectIdentity,
    context: TreeTrustContext,
) -> bytes:
    """Validate one link target and return its exact spelling for the digest."""
    _require_owner_mode(
        path,
        identity,
        frozenset({context.policy.environment_uid}),
        allow_symlink=True,
    )
    try:
        spelling = os.fsencode(path.readlink())
        resolved = path.resolve(strict=True)
    except OSError as error:
        _fail(f"cannot resolve managed environment symlink {path}: {error}")
    try:
        resolved.relative_to(context.environment)
    except ValueError:
        python_link = relative.parent == Path("bin") and re.fullmatch(
            r"python(?:3(?:\.\d+)?)?", relative.name
        )
        if not python_link or resolved != context.interpreter_target:
            _fail(f"managed environment symlink escapes its root: {path} -> {resolved}")
    return spelling


def _environment_tree_identity(
    environment: Path, interpreter_target: Path, policy: TrustPolicy
) -> EnvironmentTreeIdentity:
    """Authenticate and hash the bounded environment tree without following links."""
    maximum_entries = 100_000
    maximum_bytes = 2 * 1024 * 1024 * 1024
    digest = hashlib.sha256(b"ra8-managed-python-tree-v1\0")
    context = TreeTrustContext(environment, interpreter_target, policy)
    pending = [environment]
    entries = 0
    regular_file_bytes = 0
    while pending:
        directory = pending.pop()
        directory_identity = _identity(directory)
        if not stat.S_ISDIR(directory_identity.mode):
            _fail(f"managed environment tree entry is not a directory: {directory}")
        _require_owner_mode(directory, directory_identity, frozenset({policy.environment_uid}))
        relative_directory = directory.relative_to(environment)
        _tree_digest_record(digest, b"d", relative_directory, directory_identity, b"")
        entries += 1
        if entries > maximum_entries:
            _fail("managed environment exceeds the authenticated tree bounds")
        try:
            children = sorted(directory.iterdir(), key=lambda item: os.fsencode(item.name))
        except OSError as error:
            _fail(f"cannot enumerate managed environment directory {directory}: {error}")
        for child in reversed(children):
            relative = child.relative_to(environment)
            if relative == Path(RECEIPT_NAME):
                continue
            identity = _identity(child)
            if stat.S_ISDIR(identity.mode):
                pending.append(child)
                continue
            if stat.S_ISREG(identity.mode):
                content_digest, stable_identity = _trusted_regular_file_digest(child, policy)
                _tree_digest_record(digest, b"f", relative, stable_identity, content_digest)
                regular_file_bytes += stable_identity.size
            elif stat.S_ISLNK(identity.mode):
                spelling = _trusted_tree_symlink(child, relative, identity, context)
                _tree_digest_record(digest, b"l", relative, identity, spelling)
            else:
                _fail(f"unsupported object in managed environment tree: {child}")
            entries += 1
            if entries > maximum_entries or regular_file_bytes > maximum_bytes:
                _fail("managed environment exceeds the authenticated tree bounds")
    return EnvironmentTreeIdentity(digest.hexdigest(), entries, regular_file_bytes)


def _interpreter_chain(
    interpreter: Path, policy: TrustPolicy
) -> tuple[Path, dict[Path, ObjectIdentity]]:
    """Resolve and validate every symlink object in the interpreter chain."""
    current = interpreter
    snapshots: dict[Path, ObjectIdentity] = {}
    seen: set[Path] = set()
    for _hop in range(32):
        if current in seen:
            _fail(f"interpreter symlink loop: {current}")
        seen.add(current)
        snapshots.update(_validate_route(current.parent, policy.route_uids))
        identity = _identity(current)
        snapshots[current] = identity
        _require_owner_mode(current, identity, policy.route_uids, allow_symlink=True)
        if stat.S_ISLNK(identity.mode):
            target = current.readlink()
            current = target if target.is_absolute() else current.parent / target
            current = Path(os.path.normpath(current))
            continue
        if not stat.S_ISREG(identity.mode) or not identity.mode & 0o111:
            _fail(f"managed interpreter target is not executable and regular: {current}")
        return current, snapshots
    _fail(f"managed interpreter symlink chain is too deep: {interpreter}")


def _check_snapshots(snapshots: dict[Path, ObjectIdentity]) -> None:
    """Reject any checked object whose identity changed after validation."""
    for path, before in snapshots.items():
        after = _identity(path)
        if stat.S_ISDIR(before.mode):
            stable = (before.device, before.inode, before.mode, before.uid)
            current = (after.device, after.inode, after.mode, after.uid)
        else:
            stable = before
            current = after
        if current != stable:
            _fail(f"managed environment object changed during verification: {path}")


def _probe_interpreter(interpreter: Path, environment: Path) -> InterpreterProbe:
    """Query only standard-library facts after interpreter bytes are trusted."""
    probe = """
import hashlib
import importlib.metadata
import json
import re
import sys

def canonical(name):
    return re.sub(r"[-_.]+", "-", name).lower()

packages = sorted(
    f"{canonical(dist.metadata['Name'])}=={dist.version}"
    for dist in importlib.metadata.distributions()
)
payload = {
    "base_prefix": sys.base_prefix,
    "executable": sys.executable,
    "implementation": sys.implementation.name,
    "packages_sha256": hashlib.sha256(
        json.dumps(packages, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    ).hexdigest(),
    "prefix": sys.prefix,
    "version": ".".join(str(value) for value in sys.version_info[:3]),
}
print(json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True))
"""
    clean_env = {
        "HOME": "/",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "/usr/bin:/bin",
        "PYTHONNOUSERSITE": "1",
    }
    result = _run_command([str(interpreter), "-I", "-c", probe], clean_env, 30)
    if result.returncode != 0:
        _fail(f"managed interpreter probe failed with exit {result.returncode}")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        _fail(f"managed interpreter returned malformed identity: {error}")
    if not isinstance(payload, dict):
        _fail("managed interpreter returned a non-object identity")
    prefix = str(Path(str(payload.get("prefix", ""))).resolve())
    executable = str(Path(str(payload.get("executable", ""))).resolve())
    if prefix != str(environment) or payload.get("base_prefix") == payload.get("prefix"):
        _fail("managed interpreter is not bound to the declared virtual environment")
    if executable != str(interpreter.resolve()):
        _fail("managed interpreter reported a different executable target")
    implementation = payload.get("implementation")
    version = payload.get("version")
    packages_sha256 = payload.get("packages_sha256")
    if implementation != "cpython" or not re.fullmatch(r"3\.(11|12|13|14)\.\d+", str(version)):
        _fail(f"unsupported managed Python identity: {implementation} {version}")
    if not re.fullmatch(r"[0-9a-f]{64}", str(packages_sha256)):
        _fail("managed interpreter returned an invalid installed-set digest")
    return InterpreterProbe(str(implementation), str(version), str(packages_sha256))


def _receipt_payload(
    sources: ReceiptSources,
    interpreter_target: Path,
    probe: InterpreterProbe,
    tree: EnvironmentTreeIdentity,
) -> dict[str, Any]:
    """Build the complete deterministic receipt payload."""
    return {
        "authority_sha256": _sha256_file(Path(__file__)),
        "checks_sha256": _sha256_file(Path(__file__).with_name("managed_python_env_checks.py")),
        "dependency_group": sources.group,
        "environment_path": str(sources.environment),
        "environment_tree_entries": tree.entries,
        "environment_tree_regular_file_bytes": tree.regular_file_bytes,
        "environment_tree_sha256": tree.sha256,
        "installed_distributions_sha256": probe.installed_sha256,
        "interpreter_target": str(interpreter_target),
        "pyproject_sha256": _sha256_file(sources.pyproject),
        "python_implementation": probe.implementation,
        "python_version": probe.version,
        "receipt_version": 1,
        "uv_lock_sha256": _sha256_file(sources.lockfile),
    }


def _collect_payload(
    environment_raw: str,
    pyproject: Path,
    lockfile: Path,
    group: str,
    policy: TrustPolicy,
) -> tuple[Path, dict[str, Any]]:
    """Authenticate the environment route and collect its current identity."""
    if not re.fullmatch(r"[a-z][a-z0-9-]{0,31}", group):
        _fail(f"invalid dependency group identity: {group!r}")
    environment = _canonical_environment(environment_raw)
    snapshots = _validate_route(environment, policy.route_uids)
    environment_identity = snapshots[environment]
    if environment_identity.uid != policy.environment_uid:
        _fail(f"managed environment is not owned by uid {policy.environment_uid}: {environment}")
    bin_dir = environment / "bin"
    bin_identity = _identity(bin_dir)
    if not stat.S_ISDIR(bin_identity.mode) or bin_identity.uid != policy.environment_uid:
        _fail(f"managed environment bin directory has an untrusted owner: {bin_dir}")
    _require_owner_mode(bin_dir, bin_identity, frozenset({policy.environment_uid}))
    snapshots[bin_dir] = bin_identity
    interpreter = bin_dir / "python3"
    target, interpreter_snapshots = _interpreter_chain(interpreter, policy)
    snapshots.update(interpreter_snapshots)
    tree_before = _environment_tree_identity(environment, target, policy)
    probe = _probe_interpreter(interpreter, environment)
    tree_after = _environment_tree_identity(environment, target, policy)
    if tree_before != tree_after:
        _fail("managed environment tree changed while its interpreter was probed")
    sources = ReceiptSources(environment, pyproject, lockfile, group)
    payload = _receipt_payload(sources, target, probe, tree_after)
    _check_snapshots(snapshots)
    return environment, payload


def _read_receipt(
    environment: Path, policy: TrustPolicy
) -> tuple[dict[str, Any], Path, ObjectIdentity]:
    """Read and validate the immutable receipt through a no-follow descriptor."""
    receipt = environment / RECEIPT_NAME
    data, identity = _safe_file_bytes(receipt)
    if identity.uid != policy.environment_uid:
        _fail(f"managed environment receipt has an untrusted owner: {receipt}")
    if stat.S_IMODE(identity.mode) != RECEIPT_MODE:
        _fail(f"managed environment receipt must have mode 0444: {receipt}")
    try:
        payload = json.loads(data.decode("ascii"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        _fail(f"managed environment receipt is malformed: {error}")
    if not isinstance(payload, dict):
        _fail("managed environment receipt must contain one JSON object")
    return payload, receipt, identity


def _verify(
    environment_raw: str,
    pyproject: Path,
    lockfile: Path,
    group: str,
    policy: TrustPolicy,
) -> Path:
    """Verify the receipt and return the authenticated bin directory."""
    environment = _canonical_environment(environment_raw)
    route = _validate_route(environment, policy.route_uids)
    if route[environment].uid != policy.environment_uid:
        _fail(f"managed environment is not owned by uid {policy.environment_uid}: {environment}")
    receipt, receipt_path, receipt_identity = _read_receipt(environment, policy)
    checked_environment, current = _collect_payload(
        environment_raw, pyproject, lockfile, group, policy
    )
    if set(receipt) != set(current):
        _fail("managed environment receipt has an unknown or incomplete schema")
    for key in sorted(current):
        if receipt.get(key) != current[key]:
            _fail(f"managed environment receipt mismatch for {key}")
    if _identity(receipt_path) != receipt_identity:
        _fail("managed environment receipt changed during verification")
    if _sha256_file(pyproject) != current["pyproject_sha256"]:
        _fail("pyproject.toml changed during managed environment verification")
    if _sha256_file(lockfile) != current["uv_lock_sha256"]:
        _fail("uv.lock changed during managed environment verification")
    if _sha256_file(Path(__file__)) != current["authority_sha256"]:
        _fail("managed environment authority changed during verification")
    if (
        _sha256_file(Path(__file__).with_name("managed_python_env_checks.py"))
        != current["checks_sha256"]
    ):
        _fail("managed environment checks helper changed during verification")
    return checked_environment / "bin"


def _authentication_cache_key(
    environment_raw: str,
    pyproject: Path,
    lockfile: Path,
    group: str,
    policy: TrustPolicy,
) -> str:
    """Key a process-local post-verification cache to every mutable input."""
    if not re.fullmatch(r"[a-z][a-z0-9-]{0,31}", group):
        _fail(f"invalid dependency group identity: {group!r}")
    environment = _canonical_environment(environment_raw)
    route = _validate_route(environment, policy.route_uids)
    environment_identity = route[environment]
    if environment_identity.uid != policy.environment_uid:
        _fail(f"managed environment is not owned by uid {policy.environment_uid}: {environment}")
    _payload, receipt, receipt_identity = _read_receipt(environment, policy)
    material = {
        "authority_sha256": _sha256_file(Path(__file__)),
        "checks_sha256": _sha256_file(Path(__file__).with_name("managed_python_env_checks.py")),
        "dependency_group": group,
        "environment_identity": environment_identity.cache_fields(),
        "environment_path": str(environment),
        "pyproject_sha256": _sha256_file(pyproject),
        "receipt_identity": receipt_identity.cache_fields(),
        "receipt_sha256": _sha256_file(receipt),
        "uv_lock_sha256": _sha256_file(lockfile),
    }
    encoded = json.dumps(material, sort_keys=True, separators=(",", ":")).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def _atomic_write(environment: Path, payload: dict[str, Any], policy: TrustPolicy) -> None:
    """Publish a read-only receipt atomically through the environment dirfd."""
    flags = os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    directory_fd = os.open(environment, flags)
    temporary = f".{RECEIPT_NAME}.tmp-{os.getpid()}"
    receipt_bytes = (
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
    ).encode("ascii")
    try:
        file_flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
        file_flags |= getattr(os, "O_NOFOLLOW", 0)
        descriptor = os.open(temporary, file_flags, 0o400, dir_fd=directory_fd)
        try:
            remaining = memoryview(receipt_bytes)
            while remaining:
                written = os.write(descriptor, remaining)
                if written <= 0:
                    _fail("could not finish the managed-environment receipt write")
                remaining = remaining[written:]
            os.fsync(descriptor)
            os.fchmod(descriptor, RECEIPT_MODE)
            identity = ObjectIdentity.from_stat(os.fstat(descriptor))
            if identity.uid != policy.environment_uid:
                _fail("temporary managed-environment receipt has an untrusted owner")
        finally:
            os.close(descriptor)
        os.replace(
            temporary,
            RECEIPT_NAME,
            src_dir_fd=directory_fd,
            dst_dir_fd=directory_fd,
        )
        os.fsync(directory_fd)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary, dir_fd=directory_fd)
        os.close(directory_fd)


def _write(
    environment_raw: str,
    pyproject: Path,
    lockfile: Path,
    group: str,
    policy: TrustPolicy,
) -> Path:
    """Collect, atomically publish, then re-verify one receipt."""
    environment, payload = _collect_payload(environment_raw, pyproject, lockfile, group, policy)
    _atomic_write(environment, payload, policy)
    return _verify(environment_raw, pyproject, lockfile, group, policy)


def _make_test_environment(root: Path, name: str) -> Path:
    """Create a private stdlib-only venv for authentication selftests."""
    environment = root / name
    result = _run_command(
        [str(Path(sys.executable).resolve()), "-I", "-m", "venv", str(environment)],
        os.environ.copy(),
        60,
    )
    if result.returncode != 0:
        _fail(f"selftest could not create a virtual environment: {result.stderr}")
    return environment


def _expect_rejection(label: str, operation: Callable[[], object]) -> None:
    """Require one hostile selftest operation to fail closed."""
    try:
        operation()
    except ManagedEnvironmentError:
        return
    _fail(f"selftest accepted {label}")


def _writable_route_selftest(
    root: Path,
    environment: Path,
    pyproject: Path,
    lockfile: Path,
    policy: TrustPolicy,
) -> None:
    """Exercise writable receipt, bin, environment, and parent rejection."""
    checks = _load_checks()

    receipt = environment / RECEIPT_NAME
    receipt.chmod(0o644)
    _expect_rejection(
        "a writable receipt",
        lambda: _verify(str(environment), pyproject, lockfile, "ci", policy),
    )
    receipt.chmod(RECEIPT_MODE)
    (environment / "bin").chmod(0o775)
    _expect_rejection(
        "a group-writable bin directory",
        lambda: _verify(str(environment), pyproject, lockfile, "ci", policy),
    )
    (environment / "bin").chmod(0o755)
    environment.chmod(0o777)
    _expect_rejection(
        "a writable environment root",
        lambda: _verify(str(environment), pyproject, lockfile, "ci", policy),
    )
    environment.chmod(0o755)

    interpreter = environment / "bin/python3"
    original_target = interpreter.readlink()
    interpreter.unlink()
    shutil.copy2(Path(sys.executable).resolve(), interpreter)
    interpreter.chmod(0o775)
    _expect_rejection(
        "a group-writable interpreter target",
        lambda: _verify(str(environment), pyproject, lockfile, "ci", policy),
    )
    interpreter.unlink()
    interpreter.symlink_to(original_target)

    checks.rewrite_receipt(environment, {"python_version": "3.99.0"}, RECEIPT_NAME, RECEIPT_MODE)
    _expect_rejection(
        "the wrong Python version",
        lambda: _verify(str(environment), pyproject, lockfile, "ci", policy),
    )
    checks.rewrite_receipt(environment, {"checks_sha256": "0" * 64}, RECEIPT_NAME, RECEIPT_MODE)
    _expect_rejection(
        "the wrong checks-helper digest",
        lambda: _verify(str(environment), pyproject, lockfile, "ci", policy),
    )
    _write(str(environment), pyproject, lockfile, "ci", policy)
    root.chmod(0o777)
    _expect_rejection(
        "a writable parent route",
        lambda: _verify(str(environment), pyproject, lockfile, "ci", policy),
    )
    root.chmod(0o700)


def _filesystem_selftest(root: Path, policy: TrustPolicy) -> None:
    """Exercise valid, stale, copied, writable, and symlinked environments."""
    checks = _load_checks()

    pyproject = root / "pyproject.toml"
    lockfile = root / "uv.lock"
    pyproject.write_text("[dependency-groups]\nci=[]\n", encoding="ascii")
    lockfile.write_text("version = 1\n", encoding="ascii")
    environment = _make_test_environment(root, "managed")
    expected_bin = _write(str(environment), pyproject, lockfile, "ci", policy)
    if expected_bin != environment / "bin":
        _fail("selftest did not return the authenticated bin directory")
    _verify(str(environment), pyproject, lockfile, "ci", policy)
    harness = checks.FilesystemHarness(
        refresh=lambda: _write(str(environment), pyproject, lockfile, "ci", policy),
        reject_environment=lambda label, candidate, group: _expect_rejection(
            label,
            lambda: _verify(str(candidate), pyproject, lockfile, group, policy),
        ),
        make_environment=lambda name: _make_test_environment(root, name),
        cache_key=lambda group: _authentication_cache_key(
            str(environment), pyproject, lockfile, group, policy
        ),
        fail=_fail,
        receipt_name=RECEIPT_NAME,
        receipt_mode=RECEIPT_MODE,
    )
    checks.cache_key_selftest(environment, pyproject, lockfile, harness)
    checks.stale_receipt_selftest(root, environment, pyproject, lockfile, harness)
    _writable_route_selftest(root, environment, pyproject, lockfile, policy)
    checks.nested_tree_selftest(environment, harness)


def _selftest() -> int:
    """Run non-vacuous positive and hostile receipt/consumer tests."""
    checks = _load_checks()

    home = Path.home().resolve()
    if _identity(home).mode & 0o022:
        _fail(f"selftest home route is group/other writable: {home}")
    root = Path(tempfile.mkdtemp(prefix=".ra8-managed-env-", dir=home))
    root.chmod(0o700)
    try:
        _filesystem_selftest(root, TrustPolicy.selftest())
    finally:
        root.chmod(0o700)
        shutil.rmtree(root)
    failures = checks.consumer_contract_selftest()
    if failures:
        _fail("; ".join(failures))
    print("managed_python_env.py --selftest: PASS")
    return 0


def _parser() -> argparse.ArgumentParser:
    """Build the command-line parser without exposing trust-policy overrides."""
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("write", "verify", "cache-key"):
        child = subparsers.add_parser(command)
        child.add_argument("--env", required=True)
        child.add_argument("--pyproject", required=True, type=Path)
        child.add_argument("--lock", required=True, type=Path)
        child.add_argument("--group", required=True)
        if command == "verify":
            child.add_argument("--print-bin", action="store_true")
    check = subparsers.add_parser("check-consumers")
    check.add_argument("--root", type=Path, default=Path.cwd())
    return parser


def main(argv: list[str] | None = None) -> int:
    """Dispatch receipt creation, authentication, and contract checks."""
    if argv is None:
        argv = sys.argv[1:]
    if argv == ["--selftest"]:
        return _selftest()
    arguments = _parser().parse_args(argv)
    if arguments.command == "check-consumers":
        checks = _load_checks()
        root = arguments.root.resolve()
        findings = checks.consumer_findings(root) + checks.consumer_runtime_findings(
            root, _run_command
        )
        for finding in findings:
            print(f"managed-python-env: {finding}", file=sys.stderr)
        if findings:
            return 1
        print("managed Python environment consumers share one authenticated authority")
        return 0
    policy = TrustPolicy.production()
    if arguments.command == "write":
        if os.geteuid() != 0:
            _fail("only root may create a production managed-environment receipt")
        bin_dir = _write(
            arguments.env, arguments.pyproject, arguments.lock, arguments.group, policy
        )
        print(f"wrote authenticated managed Python environment receipt for {bin_dir.parent}")
        return 0
    if arguments.command == "cache-key":
        print(
            _authentication_cache_key(
                arguments.env,
                arguments.pyproject,
                arguments.lock,
                arguments.group,
                policy,
            )
        )
        return 0
    bin_dir = _verify(arguments.env, arguments.pyproject, arguments.lock, arguments.group, policy)
    message = (
        str(bin_dir)
        if arguments.print_bin
        else f"authenticated managed Python environment: {bin_dir.parent}"
    )
    print(message)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ManagedEnvironmentError as error:
        print(f"managed_python_env.py: FATAL: {error}", file=sys.stderr)
        raise SystemExit(1) from None
