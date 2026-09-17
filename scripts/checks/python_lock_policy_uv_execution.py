# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Semantic policy for immutable uv execution and lock-policy consumers."""

from __future__ import annotations

import ast
import hashlib
from pathlib import Path

DEPLOYMENT_CLOSURE_PATHS = (
    Path(".devcontainer/Dockerfile"),
    Path(".dockerignore"),
    Path("infra/ansible/roles/ci_runner/tasks/main.yml"),
    Path("infra/ansible/roles/dev_box/tasks/transaction.yml"),
    Path("infra/ansible/roles/hil_bench/tasks/transaction.yml"),
    Path("scripts/ci/devcontainer_image.sh"),
    Path("scripts/dev/fleet_wsl.py"),
    Path("scripts/dev/fleet_wsl_stage.py"),
)
EXEC_MODULE_SHA256 = "bd54ee9be90ca047c535349b2ab3855b4afcefd53c077a56215f5440d76e2ae4"
RUNNER_MODULE_SHA256 = "4242262cf1649cf5935dd8e12f42b737bbc2592b3d633615acf6593179502f40"
EXEC_EXACT_BODIES = {
    "_open_parent_fd": """
def _open_parent_fd(path, *, create=False):
    nofollow = getattr(os, "O_NOFOLLOW", None)
    cloexec = getattr(os, "O_CLOEXEC", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or cloexec is None or directory is None:
        fail("POSIX uv cache access requires O_NOFOLLOW, O_CLOEXEC, and O_DIRECTORY")
    if not path.is_absolute() or path.name in ("", ".", ".."):
        fail(f"uv cache artifact path is not an absolute file path: {path}")
    components = path.parent.parts[1:]
    if len(components) > MAX_CACHE_PATH_COMPONENTS:
        fail(f"uv cache artifact path has too many components: {path}")
    flags = os.O_RDONLY | nofollow | cloexec | directory
    descriptor = -1
    try:
        descriptor = os.open(path.anchor, flags)
        root_descriptor = descriptor
        descriptor = -1
        descriptor = open_parent_components(
            root_descriptor,
            components,
            flags,
            create=create,
            platform_name=sys.platform,
        )
    except (OSError, NotImplementedError, TypeError) as exc:
        if descriptor >= 0:
            os.close(descriptor)
        fail(f"cannot open cached uv parent {path.parent}: {exc}")
    return descriptor
""",
    "open_parent_components": """
def open_parent_components(descriptor, components, flags, *, create, platform_name):
    try:
        for index, component in enumerate(components):
            try:
                next_descriptor = -1
                alias_key = index, platform_name, component
                if alias_key in DARWIN_ROOT_ALIAS_POSITIONS:
                    _require_trusted_system_root(descriptor)
                    next_descriptor = _open_verified_darwin_alias(descriptor, component, flags)
                if next_descriptor < 0:
                    next_descriptor = os.open(component, flags, dir_fd=descriptor)
            except FileNotFoundError:
                if not create:
                    raise
                with suppress(FileExistsError):
                    os.mkdir(component, CACHE_DIRECTORY_MODE, dir_fd=descriptor)
                next_descriptor = os.open(component, flags, dir_fd=descriptor)
            previous = descriptor
            descriptor = next_descriptor
            os.close(previous)
    except Exception:
        os.close(descriptor)
        raise
    return descriptor
""",
    "_open_verified_darwin_alias": """
def _open_verified_darwin_alias(root_descriptor, component, flags):
    target = DARWIN_ROOT_ALIASES.get(component)
    if target is None:
        fail(f"unsupported Darwin uv cache root alias: {component}")
    expected = "/".join(target)
    before = os.stat(component, dir_fd=root_descriptor, follow_symlinks=False)
    before_target = os.readlink(component, dir_fd=root_descriptor)
    if not stat.S_ISLNK(before.st_mode):
        fail(f"Darwin uv cache root alias is not a symlink: /{component}")
    if before.st_uid != 0:
        fail(f"Darwin uv cache root alias is not root-owned: /{component}")
    if before_target != expected:
        fail(f"untrusted Darwin uv cache root alias: /{component}")
    alias_descriptor = -1
    physical_descriptor = -1
    succeeded = False
    try:
        alias_descriptor = os.open(
            component,
            flags & ~os.O_NOFOLLOW,
            dir_fd=root_descriptor,
        )
        physical_descriptor = _open_physical_alias_target(root_descriptor, target, flags)
        alias_state = os.fstat(alias_descriptor)
        physical_state = os.fstat(physical_descriptor)
        after = os.stat(component, dir_fd=root_descriptor, follow_symlinks=False)
        after_target = os.readlink(component, dir_fd=root_descriptor)
        if not stat.S_ISDIR(alias_state.st_mode):
            fail(f"Darwin uv cache alias target is not a directory: /{component}")
        if not stat.S_ISDIR(physical_state.st_mode):
            fail(f"Darwin uv cache physical target is not a directory: /{component}")
        if _stat_identity(alias_state) != _stat_identity(physical_state):
            fail(f"Darwin uv cache root alias target mismatched: /{component}")
        if _link_fingerprint(before) != _link_fingerprint(after):
            fail(f"Darwin uv cache root alias changed identity: /{component}")
        if after_target != expected:
            fail(f"Darwin uv cache root alias changed target: /{component}")
        succeeded = True
    finally:
        if alias_descriptor >= 0:
            os.close(alias_descriptor)
        if not succeeded and physical_descriptor >= 0:
            os.close(physical_descriptor)
    return physical_descriptor
""",
    "_new_temporary_fd": """
def _new_temporary_fd(parent, mode=PRIVATE_TEMPORARY_FILE_MODE):
    cloexec = getattr(os, "O_CLOEXEC", None)
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if cloexec is None or nofollow is None:
        fail("POSIX uv cache writes require O_CLOEXEC and O_NOFOLLOW")
    if mode not in (PROBE_EXECUTABLE_MODE, PRIVATE_TEMPORARY_FILE_MODE):
        fail("POSIX uv cache temporary mode is outside policy")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | cloexec | nofollow
    for _attempt in range(MAX_TEMPORARY_NAME_ATTEMPTS):
        name = f".ra8-uv-{secrets.token_hex(16)}.tmp"
        try:
            return os.open(name, flags, mode, dir_fd=parent), name
        except FileExistsError:
            continue
    fail("cannot allocate a private uv cache temporary")
""",
    "write_atomic_nofollow": """
def write_atomic_nofollow(path, payload, mode):
    if not payload or mode not in (0o600, 0o700):
        fail("uv cache write requires nonempty bytes and one private mode")
    parent = _open_parent_fd(path, create=True)
    descriptor = -1
    temporary = ""
    try:
        descriptor, temporary = _new_temporary_fd(parent)
        write_exact_fd(descriptor, payload)
        os.fsync(descriptor)
        os.fchmod(descriptor, mode)
        os.replace(temporary, path.name, src_dir_fd=parent, dst_dir_fd=parent)
    except (OSError, NotImplementedError, TypeError) as exc:
        fail(f"cannot write cached uv artifact {path}: {exc}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary:
            with suppress(FileNotFoundError):
                os.unlink(temporary, dir_fd=parent)
        os.close(parent)
""",
    "open_regular_nofollow": """
def open_regular_nofollow(path):
    nonblock = getattr(os, "O_NONBLOCK", None)
    cloexec = getattr(os, "O_CLOEXEC", None)
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nonblock is None or cloexec is None or nofollow is None:
        fail("POSIX uv cache access requires O_NONBLOCK, O_CLOEXEC, and O_NOFOLLOW")
    parent = _open_parent_fd(path)
    descriptor = -1
    try:
        descriptor = os.open(
            path.name,
            os.O_RDONLY | nonblock | cloexec | nofollow,
            dir_fd=parent,
        )
        state = os.fstat(descriptor)
    except OSError as exc:
        if descriptor >= 0:
            os.close(descriptor)
        fail(f"cannot open cached uv artifact {path}: {exc}")
    finally:
        os.close(parent)
    if not stat.S_ISREG(state.st_mode) or state.st_nlink != 1:
        os.close(descriptor)
        fail(f"cached uv artifact is not one single-link regular file: {path}")
    return descriptor
""",
    "portable_named_exec_snapshot": """
def portable_named_exec_snapshot(binary):
    if not binary:
        fail("authenticated uv executable bytes are empty")
    reader = -1
    writer = -1
    parent = -1
    temporary = ""
    identity = (-1, -1)
    try:
        with tempfile.TemporaryDirectory(prefix="ra8-uv-probe-") as raw:
            probe_path = Path(raw) / "probe"
            parent = _open_parent_fd(probe_path)
            _require_private_temporary_parent(parent)
            writer, temporary = _new_temporary_fd(parent, PROBE_EXECUTABLE_MODE)
            write_exact_fd(writer, binary)
            os.fsync(writer)
            os.fchmod(writer, PROBE_EXECUTABLE_MODE)
            writer_state = os.fstat(writer)
            identity = _stat_identity(writer_state)
            flags = os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW
            reader = os.open(temporary, flags, dir_fd=parent)
            if _stat_identity(os.fstat(reader)) != identity:
                fail("authenticated uv reader did not reopen the staged inode")
            os.close(writer)
            writer = -1
            _verify_portable_exec_fd(reader, binary, identity, linked=True)
            _verify_portable_exec_name(parent, temporary, binary, identity)
            try:
                yield reader, str(Path(raw) / temporary)
            finally:
                _verify_portable_exec_fd(reader, binary, identity, linked=True)
                _verify_portable_exec_name(parent, temporary, binary, identity)
                _unlink_matching_temporary(parent, temporary, identity, required=True)
                os.fsync(parent)
                temporary = ""
                _verify_portable_exec_fd(reader, binary, identity, linked=False)
    except (OSError, NotImplementedError, TypeError) as exc:
        fail(f"cannot stage portable authenticated uv executable: {exc}")
    finally:
        if writer >= 0:
            os.close(writer)
        if temporary and parent >= 0:
            with suppress(OSError):
                _unlink_matching_temporary(
                    parent,
                    temporary,
                    identity,
                    required=False,
                )
        if parent >= 0:
            os.close(parent)
        if reader >= 0:
            os.close(reader)
""",
}
EXEC_EXACT_FUNCTION_DIGESTS = {
    "_stat_identity": "a81b93626d87198239f1124ed99fab850e5eb4295fb82106e634033b64ebf516",
    "_link_fingerprint": "04c45b309f6f2012d9279e0394eff98ee65ef3881a9250a8f878cf7514ad0912",
    "_require_trusted_system_root": (
        "6cbfe9fd647e6deffbc970ebda15e88657ee2ca7648ec8fc5c40fa3befc1aed8"
    ),
    "_open_physical_alias_target": (
        "b7fc16104dc3b324b7c313966e1cad9af64f67a0b98f5b4bd8f58cd7dbdb9b62"
    ),
    "_verify_portable_exec_fd": (
        "41a9b81da3e1f72f62c9b89a5a94b059e92bab07da4994b8c837dad7ee564c82"
    ),
    "_unlink_matching_temporary": (
        "d4c1fbdc7a416521f30b9ca59f2d538587360a90f4bd50829342fdf279a88d52"
    ),
    "_require_private_temporary_parent": (
        "4c7252aa77a5879330b6609383b8375f594537ce87069410160bfc5b9847ddc1"
    ),
    "_verify_portable_exec_name": (
        "442b996daeaa03955af307ce0eca050adeee4ec0ef8c1adf9eb69842c487a6be"
    ),
    "linux_sealed_exec_fd": ("9a989bab6c3b2da394f1efa83b57ba96d5331b68f8df4de3252f898da56a4ef6"),
    "authenticated_executable_fd": (
        "7b2881c38c36e46545428ab2471f03f5e4bde59f71477eb898ea622f54dd7087"
    ),
    "run_uv_snapshot": "94c875cfa5a0c0d4571e999b296b6bd5adf71ae9344b6ed5dfe6e10ad250d07e",
}
EXEC_EXACT_ASSIGNMENTS = {
    "PROBE_EXECUTABLE_MODE": "320",
    "PRIVATE_TEMPORARY_DIRECTORY_MODE": "448",
    "PRIVATE_TEMPORARY_FILE_MODE": "384",
    "DARWIN_ROOT_ALIASES": ("{'tmp': ('private', 'tmp'), 'var': ('private', 'var')}"),
    "DARWIN_ROOT_ALIAS_POSITIONS": (
        "frozenset((0, 'darwin', component) for component in DARWIN_ROOT_ALIASES)"
    ),
}
EXEC_CALL_CONTRACTS = {
    "linux_sealed_exec_fd": (
        ("os", "memfd_create", 1),
        ("", "write_exact_fd", 1),
        ("os", "fchmod", 1),
        ("seals", "fcntl", 2),
    ),
    "portable_named_exec_snapshot": (
        ("tempfile", "TemporaryDirectory", 1),
        ("", "_open_parent_fd", 1),
        ("", "_require_private_temporary_parent", 1),
        ("", "_new_temporary_fd", 1),
        ("", "write_exact_fd", 1),
        ("os", "fsync", 2),
        ("os", "fchmod", 1),
        ("os", "open", 1),
        ("", "_verify_portable_exec_fd", 3),
        ("", "_verify_portable_exec_name", 2),
        ("", "_unlink_matching_temporary", 2),
    ),
    "authenticated_executable_fd": (
        ("", "linux_sealed_exec_fd", 1),
        ("", "portable_named_exec_snapshot", 1),
        ("", "executable_fd_path", 1),
        ("os", "close", 1),
    ),
    "run_uv_snapshot": (
        ("", "authenticated_executable_fd", 1),
        ("subprocess", "run", 1),
    ),
}
EXEC_MUTATIONS = (
    (
        '        fail(f"authenticated uv execution failed: {exc}")\n',
        '        fail(f"authenticated uv execution failed: {exc}")\n'
        "_uv_alias_escape = DARWIN_ROOT_ALIASES\n"
        '_uv_alias_escape["etc"] = ("private", "etc")\n'
        'globals()["DARWIN_ROOT_ALIAS_POSITIONS"] = frozenset(\n'
        '    (0, "darwin", component) for component in _uv_alias_escape\n'
        ")\n",
    ),
    (
        '        fail(f"authenticated uv execution failed: {exc}")\n',
        '        fail(f"authenticated uv execution failed: {exc}")\n'
        "UV_EXEC_UNREVIEWED_SURFACE = True\n",
    ),
    (
        "def _require_trusted_system_root(descriptor: int) -> None:\n",
        "@staticmethod\ndef _require_trusted_system_root(descriptor: int) -> None:\n",
    ),
    ("    held = os.fstat(descriptor)\n", "    return\n"),
    (
        "def _require_private_temporary_parent(descriptor: int) -> None:\n"
        '    """Require the held portable-snapshot directory to be caller-private."""\n'
        "    state = os.fstat(descriptor)\n",
        "def _require_private_temporary_parent(descriptor: int) -> None:\n"
        '    """Require the held portable-snapshot directory to be caller-private."""\n'
        "    return\n",
    ),
    (
        '    "tmp": ("private", "tmp"),\n',
        '    "etc": ("private", "etc"),\n    "tmp": ("private", "tmp"),\n',
    ),
    ("    if held.st_uid != 0:\n", "    if False:\n"),
    ("    if state.st_uid != os.geteuid():\n", "    if False:\n"),
    (
        "                [executable, *arguments],\n",
        "                [str(Path('/tmp/uv')), *arguments],\n",
    ),
    (
        "                if next_descriptor < 0:\n"
        "                    next_descriptor = os.open(component, flags, dir_fd=descriptor)\n"
        "            except FileNotFoundError:\n",
        "                if next_descriptor < 0:\n"
        "                    next_descriptor = os.open(component, flags)\n"
        "            except FileNotFoundError:\n",
    ),
    (
        "            reader = os.open(temporary, flags, dir_fd=parent)\n",
        "            reader = os.open(temporary, flags)\n",
    ),
    (
        "flags = os.O_RDONLY | nofollow | cloexec | directory",
        "flags = os.O_RDONLY | nofollow | cloexec",
    ),
    (
        "parent = _open_parent_fd(path, create=True)",
        "parent = _open_parent_fd(path)",
    ),
    (
        "os.replace(temporary, path.name, src_dir_fd=parent, dst_dir_fd=parent)",
        "os.replace(temporary, path.name)",
    ),
    ("seals.fcntl(descriptor, seals.F_ADD_SEALS, mask)", "pass"),
    ("            pass_fds=(descriptor,),\n", ""),
    (
        "            _require_private_temporary_parent(parent)\n",
        "            pass\n",
    ),
    (
        "            _verify_portable_exec_name(parent, temporary, binary, identity)\n"
        "            try:\n",
        "            try:\n",
    ),
    (
        "                _unlink_matching_temporary(parent, temporary, identity, required=True)\n",
        "                os.unlink(temporary, dir_fd=parent)\n",
    ),
)
RUNNER_MUTATION_ANCHOR = (
    "                failures.append("
    'f"post-auth {mode} replacement executed through lock/export")\n'
    "    return failures\n"
)
RUNNER_MUTATIONS = (
    (
        RUNNER_MUTATION_ANCHOR,
        RUNNER_MUTATION_ANCHOR + "AuthenticatedUv.run = lambda self, arguments, **kwargs: None\n",
    ),
    (
        RUNNER_MUTATION_ANCHOR,
        RUNNER_MUTATION_ANCHOR + "find_uv = lambda *_args, **_kwargs: None\n",
    ),
    (
        RUNNER_MUTATION_ANCHOR,
        RUNNER_MUTATION_ANCHOR + "UV_RUNNER_UNREVIEWED_SURFACE = True\n",
    ),
    ('                "--run",\n', '                "--verify-cache",\n'),
    ('probe = candidate.run(["--version"], timeout=10)', "probe = None"),
    ("lock_check = uv.run(\n", "lock_check = subprocess.run(\n"),
    (
        '            \'mv "$RA8_UV_ATTACK_CACHE" "$RA8_UV_ATTACK_CACHE.displaced"\\n\'\n',
        "",
    ),
)


def _function(tree: ast.AST, name: str) -> ast.FunctionDef | None:
    """Return one unambiguous function anywhere in a module."""
    matches = [
        node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef) and node.name == name
    ]
    return matches[0] if len(matches) == 1 else None


def _body_dump(function: ast.FunctionDef) -> str:
    """Return one function body without its documentation literal."""
    body = list(function.body)
    if (
        body
        and isinstance(body[0], ast.Expr)
        and isinstance(body[0].value, ast.Constant)
        and isinstance(body[0].value.value, str)
    ):
        body = body[1:]
    return ast.dump(ast.Module(body=body, type_ignores=[]), include_attributes=False)


def _exact_function_digest_finding(
    source: str, tree: ast.Module, name: str, expected: str
) -> list[str]:
    """Bind one complete security function, including signature and decorators."""
    function = _function(tree, name)
    if function is None:
        return [f"uv execution {name} complete semantic contract drifted"]
    lines = source.splitlines(keepends=True)
    starts = [function.lineno, *(item.lineno for item in function.decorator_list)]
    segment = "".join(lines[min(starts) - 1 : function.end_lineno]).rstrip("\r\n")
    actual = hashlib.sha256(segment.encode()).hexdigest()
    if actual != expected:
        return [f"uv execution {name} complete semantic contract drifted"]
    return []


def _assignment_value(tree: ast.Module, name: str) -> ast.AST | None:
    """Return the sole top-level value assigned to one execution authority."""
    values = []
    for statement in tree.body:
        if not isinstance(statement, ast.Assign) or len(statement.targets) != 1:
            continue
        if isinstance(statement.targets[0], ast.Name) and statement.targets[0].id == name:
            values.append(statement.value)
    return values[0] if len(values) == 1 else None


def _authority_assignment_findings(tree: ast.Module) -> list[str]:
    """Bind Darwin aliases/modes and reject all later authority mutation."""
    findings = []
    for name, expression in EXEC_EXACT_ASSIGNMENTS.items():
        expected = ast.parse(expression, mode="eval").body
        actual = _assignment_value(tree, name)
        if actual is None or ast.dump(actual) != ast.dump(expected):
            findings.append(f"uv execution authority assignment drifted: {name}")
    protected = set(EXEC_EXACT_ASSIGNMENTS)
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id in protected
            and node.func.attr != "get"
        ):
            findings.append(f"uv execution authority mutation attempted: {node.func.value.id}")
        if isinstance(node, (ast.Subscript, ast.Attribute)) and isinstance(
            node.ctx, (ast.Store, ast.Del)
        ):
            root = node.value
            while isinstance(root, (ast.Subscript, ast.Attribute)):
                root = root.value
            if isinstance(root, ast.Name) and root.id in protected:
                findings.append(f"uv execution authority mutation attempted: {root.id}")
    return findings


def _exact_body_finding(tree: ast.Module, name: str, expected: str) -> list[str]:
    """Bind one security-critical function to its reviewed semantic body."""
    function = _function(tree, name)
    fixture = _function(ast.parse(expected), name)
    if function is None or fixture is None or _body_dump(function) != _body_dump(fixture):
        return [f"uv execution {name} semantic contract drifted"]
    return []


def _call_count(function: ast.FunctionDef, owner: str, name: str) -> int:
    """Count exact direct-name or one-level qualified calls in a function."""
    count = 0
    for node in ast.walk(function):
        if not isinstance(node, ast.Call):
            continue
        if not owner and isinstance(node.func, ast.Name) and node.func.id == name:
            count += 1
        if (
            owner
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == owner
            and node.func.attr == name
        ):
            count += 1
    return count


def _required_calls(
    tree: ast.Module,
    function_name: str,
    required: tuple[tuple[str, str, int], ...],
) -> list[str]:
    """Report a missing function or any exact call-count drift."""
    function = _function(tree, function_name)
    if function is None:
        return [f"uv execution function is missing or ambiguous: {function_name}"]
    return [
        f"uv execution {function_name} {owner}.{name} call chain drifted"
        for owner, name, expected in required
        if _call_count(function, owner, name) != expected
    ]


def _keyword_value(function: ast.FunctionDef, call_owner: str, keyword: str) -> ast.AST | None:
    """Return one keyword value from one qualified call, rejecting ambiguity."""
    values = []
    for node in ast.walk(function):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            continue
        if not isinstance(node.func.value, ast.Name) or node.func.value.id != call_owner:
            continue
        values.extend(item.value for item in node.keywords if item.arg == keyword)
    return values[0] if len(values) == 1 else None


def _exec_call_findings(tree: ast.Module) -> list[str]:
    """Bind immutable execution call chains and descriptor inheritance."""
    findings = []
    for name, required in EXEC_CALL_CONTRACTS.items():
        findings.extend(_required_calls(tree, name, required))
    runner = _function(tree, "run_uv_snapshot")
    pass_fds = None if runner is None else _keyword_value(runner, "subprocess", "pass_fds")
    if not (
        isinstance(pass_fds, ast.Tuple)
        and len(pass_fds.elts) == 1
        and isinstance(pass_fds.elts[0], ast.Name)
        and pass_fds.elts[0].id == "descriptor"
    ):
        findings.append("uv execution subprocess is not bound to the immutable descriptor")
    return findings


def exec_module_findings(source: str) -> list[str]:
    """Bind sealed/read-only descriptor creation and subprocess inheritance."""
    identity_findings = (
        []
        if hashlib.sha256(source.encode()).hexdigest() == EXEC_MODULE_SHA256
        else ["uv execution module byte identity drifted"]
    )
    try:
        tree = ast.parse(source)
    except SyntaxError as exc:
        return [f"uv execution module is invalid Python: {exc}"]
    findings = [
        finding
        for name, expected in EXEC_EXACT_BODIES.items()
        for finding in _exact_body_finding(tree, name, expected)
    ]
    findings.extend(
        finding
        for name, expected in EXEC_EXACT_FUNCTION_DIGESTS.items()
        for finding in _exact_function_digest_finding(source, tree, name, expected)
    )
    return [
        *identity_findings,
        *findings,
        *_authority_assignment_findings(tree),
        *_exec_call_findings(tree),
    ]


def runner_module_findings(source: str) -> list[str]:
    """Bind lock/export work to bootstrap --run instead of a returned path."""
    identity_findings = (
        []
        if hashlib.sha256(source.encode()).hexdigest() == RUNNER_MODULE_SHA256
        else ["lock-policy uv runner module byte identity drifted"]
    )
    try:
        tree = ast.parse(source)
    except SyntaxError as exc:
        return [f"uv runner module is invalid Python: {exc}"]
    findings = [*identity_findings, *_required_calls(tree, "run", (("subprocess", "run", 1),))]
    findings.extend(_required_calls(tree, "find_uv", (("candidate", "run", 1),)))
    findings.extend(_required_calls(tree, "export_findings", (("uv", "run", 1),)))
    findings.extend(_required_calls(tree, "_one_export_findings", (("uv", "run", 1),)))
    run = _function(tree, "run")
    if run is None:
        return findings
    literals = {
        node.value
        for node in ast.walk(run)
        if isinstance(node, ast.Constant) and isinstance(node.value, str)
    }
    if not {"/usr/bin/python3", "-I", "-S", "--manifest", "--cache-root", "--run"}.issubset(
        literals
    ):
        findings.append("lock-policy uv runner no longer uses the exact bootstrap --run boundary")
    if "str(candidate)" in source or "str(uv)" in source:
        findings.append("lock-policy uv runner executes a returned mutable cache path")
    rename_old = 'mv "$RA8_UV_ATTACK_CACHE" "$RA8_UV_ATTACK_CACHE.displaced"'
    move_replacement = 'mv "$RA8_UV_ATTACK_REPLACEMENT" "$RA8_UV_ATTACK_CACHE"'
    if rename_old not in source or move_replacement not in source:
        findings.append("lock-policy path attack does not preserve the authenticated inode link")
    return findings


def uv_execution_policy_findings(root: Path) -> list[str]:
    """Return immutable-execution findings for both production modules."""
    exec_source = (root / "scripts/dev/bootstrap_uv_exec.py").read_text(encoding="utf-8")
    runner_source = (root / "scripts/checks/python_lock_policy_uv_runner.py").read_text(
        encoding="utf-8"
    )
    return [
        *exec_module_findings(exec_source),
        *runner_module_findings(runner_source),
        *deployment_closure_findings(root),
    ]


def deployment_closure_findings(root: Path) -> list[str]:
    """Require every staged bootstrap deployment to carry its exec helper."""
    findings: list[str] = []
    for relative in DEPLOYMENT_CLOSURE_PATHS:
        source = (root / relative).read_text(encoding="utf-8")
        has_bootstrap = "bootstrap_uv.py" in source
        has_helper = "bootstrap_uv_exec.py" in source
        if has_bootstrap != has_helper:
            findings.append(f"{relative}: uv bootstrap/exec-helper deployment closure drifted")
    return findings


def _mutate_once(source: str, old: str, new: str) -> str:
    """Return one exact mutation, rejecting a stale selftest anchor."""
    if source.count(old) != 1:
        message = f"uv execution mutation anchor count drifted: {old!r}"
        raise ValueError(message)
    return source.replace(old, new, 1)


def uv_execution_policy_selftest(root: Path) -> list[str]:
    """Prove each immutable-execution and consumer boundary fires."""
    exec_source = (root / "scripts/dev/bootstrap_uv_exec.py").read_text(encoding="utf-8")
    runner_source = (root / "scripts/checks/python_lock_policy_uv_runner.py").read_text(
        encoding="utf-8"
    )
    failures = ["live uv execution policy failed"] if uv_execution_policy_findings(root) else []
    failures.extend(
        f"uv execution mutation passed: {old}"
        for old, new in EXEC_MUTATIONS
        if not exec_module_findings(_mutate_once(exec_source, old, new))
    )
    failures.extend(
        f"uv runner mutation passed: {old}"
        for old, new in RUNNER_MUTATIONS
        if not runner_module_findings(_mutate_once(runner_source, old, new))
    )
    with_helper = "COPY bootstrap_uv.py bootstrap_uv_exec.py /trusted/"
    fixture = dict.fromkeys(DEPLOYMENT_CLOSURE_PATHS, with_helper)
    if _deployment_fixture_findings(fixture):
        failures.append("complete uv deployment fixture failed")
    first = DEPLOYMENT_CLOSURE_PATHS[0]
    fixture[first] = "COPY bootstrap_uv.py /trusted/"
    if not _deployment_fixture_findings(fixture):
        failures.append("bootstrap deployment without exec helper passed")
    fixture[first] = "COPY bootstrap_uv_exec.py /trusted/"
    if not _deployment_fixture_findings(fixture):
        failures.append("orphan uv exec-helper deployment passed")
    return failures


def _deployment_fixture_findings(documents: dict[Path, str]) -> list[str]:
    """Return bootstrap/helper closure findings for synthetic documents."""
    return [
        str(relative)
        for relative, source in documents.items()
        if ("bootstrap_uv.py" in source) != ("bootstrap_uv_exec.py" in source)
    ]
