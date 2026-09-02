#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce the repository's single uv dependency and environment authority."""

from __future__ import annotations

import argparse
import ast
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import yaml
from python_lock_policy_scan import (
    SECONDARY_AUTHORITY_NAMES,
    VENDOR_BOUNDARIES,
    adjacent_import_closure_selftest,
    cli_consumer_findings,
    first_party_import_closure,
    hil_preflight_findings,
    hil_preflight_selftest,
    load_hil_tasks,
    python_source_paths,
    read_authored_text,
    requirement_findings,
    scanner_selection_selftest,
    unsafe_install_findings,
)
from python_lock_policy_uv_cache import uv_cache_policy_findings, uv_cache_policy_selftest
from python_lock_policy_uv_execution import (
    uv_execution_policy_findings,
    uv_execution_policy_selftest,
)
from python_lock_policy_uv_runner import (
    AuthenticatedUv,
    execution_attack_selftest,
    export_findings,
    find_uv,
)

ROOT = Path(__file__).resolve().parents[2]
PYPROJECT = ROOT / "pyproject.toml"
MANIFEST = ROOT / "scripts" / "dev" / "uv_release.json"
BOOTSTRAP = ROOT / "scripts" / "dev" / "bootstrap_uv.py"
MIN_DIRECT_DEPENDENCIES = 15
MIN_FIRST_PARTY_PYTHON_FILES = 250
EXPECTED_ASSETS = {
    "Darwin|aarch64": "uv-aarch64-apple-darwin.tar.gz",
    "Darwin|x86_64": "uv-x86_64-apple-darwin.tar.gz",
    "Windows|aarch64": "uv-aarch64-pc-windows-msvc.zip",
    "Windows|x86_64": "uv-x86_64-pc-windows-msvc.zip",
    "Linux|aarch64|gnu": "uv-aarch64-unknown-linux-gnu.tar.gz",
    "Linux|aarch64|musl": "uv-aarch64-unknown-linux-musl.tar.gz",
    "Linux|x86_64|gnu": "uv-x86_64-unknown-linux-gnu.tar.gz",
    "Linux|x86_64|musl": "uv-x86_64-unknown-linux-musl.tar.gz",
}
EXPORTS = {
    "k3s": Path("infra/ansible/roles/k3s_node/files/requirements.lock"),
    "hil": Path("infra/ansible/roles/hil_bench/files/requirements.lock"),
}
GALAXY_MANIFEST = Path("infra/ansible/requirements.yml")
INTERPRETER_IMPORT_ROOTS: frozenset[str] = frozenset({"__main__"})


@dataclass(frozen=True)
class ConsumerProof:
    """Describe one direct dependency's owning group and repository consumer."""

    group: str
    relative: str
    needle: str


@dataclass(frozen=True)
class ConsumerCatalog:
    """Bind consumer proofs, import mappings, and a non-vacuity scan floor."""

    proofs: Mapping[str, ConsumerProof]
    external_imports: Mapping[str, str]
    minimum_python_files: int
    cli_census: bool = True


CONSUMER_PROOF_ROWS = {
    "pillow": ("hil", "scripts/hil/camera_livestream.sh", "from PIL import Image"),
    "pyserial": (
        "hil",
        "infra/network/fg_bringup.py",
        'importlib.import_module("serial")',
    ),
    "pyusb": ("hil", "scripts/hil/usb/libusb_bench.py", "import usb.core"),
    "python-dotenv": (
        "hil",
        "scripts/hil/hil_secrets.py",
        "from dotenv import load_dotenv",
    ),
    "python-kasa": (
        "hil",
        "scripts/hil/tapo_control.py",
        "from kasa import Credentials",
    ),
    "pyyaml": ("runtime", "scripts/ci/check_ci_parity.py", "import yaml"),
    "cmakelang": ("dev", "scripts/ci/gates/lint.sh", "cmake-format"),
    "gcovr": ("dev", "scripts/ci/gates/tests.sh", "require_cmd gcovr"),
    "libclang": (
        "dev",
        "scripts/ci/gates/checks.sh",
        "require_python_mod clang.cindex",
    ),
    "ruff": ("dev", "scripts/ci/gates/lint.sh", "require_tool_versions ruff"),
    "yamllint": ("dev", "scripts/ci/gates/lint.sh", "require_cmd yamllint"),
    "ansible-core": ("infra", "scripts/dev/infra.sh", "ansible-playbook"),
    "hvac": (
        "infra",
        "infra/ansible/group_vars/all.example.yml",
        "community.hashi_vault",
    ),
    "kubernetes": (
        "k3s",
        "infra/ansible/roles/k3s_node/tasks/main.yml",
        "import kubernetes",
    ),
    "ethos-u-vela": ("vela", "tools/vela/src/vela_gen.py", 'shutil.which("vela")'),
}
CONSUMER_PROOFS = {
    package: ConsumerProof(*values) for package, values in CONSUMER_PROOF_ROWS.items()
}
EXTERNAL_IMPORTS = {
    "PIL": "pillow",
    "clang": "libclang",
    "dotenv": "python-dotenv",
    "kasa": "python-kasa",
    "serial": "pyserial",
    "usb": "pyusb",
    "yaml": "pyyaml",
}
CONSUMER_CATALOG = ConsumerCatalog(CONSUMER_PROOFS, EXTERNAL_IMPORTS, MIN_FIRST_PARTY_PYTHON_FILES)


def canonical(name: str) -> str:
    """Canonicalize one Python distribution name."""
    return re.sub(r"[-_.]+", "-", name).lower()


def parse_dependency_entry(entry: object) -> tuple[str | None, str]:
    """Return a canonical pin/version or an included group marker."""
    if isinstance(entry, dict):
        if set(entry) != {"include-group"}:
            message = f"unsupported dependency-group record: {entry}"
            raise ValueError(message)
        included = entry["include-group"]
        if not isinstance(included, str):
            message = f"included dependency group must be a string: {entry}"
            raise TypeError(message)
        return None, included
    if not isinstance(entry, str):
        message = f"dependency entry must be a string: {entry!r}"
        raise TypeError(message)
    match = re.fullmatch(r"([A-Za-z0-9][A-Za-z0-9._-]*)==([0-9][A-Za-z0-9.!+_-]*)", entry)
    if match is None:
        message = f"direct dependency is not exactly pinned: {entry!r}"
        raise ValueError(message)
    name, version = match.groups()
    return canonical(name), version


def direct_declarations(path: Path) -> tuple[dict[str, str], dict[str, str]]:
    """Return exact pins and owning groups, rejecting duplicate or loose entries."""
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    groups = document.get("dependency-groups")
    if not isinstance(groups, dict):
        message = "missing dependency-groups table"
        raise TypeError(message)
    pins: dict[str, str] = {}
    owners: dict[str, str] = {}
    includes: set[str] = set()
    for group, entries in groups.items():
        if not isinstance(group, str) or not isinstance(entries, list):
            message = "dependency group must have a string name and list value"
            raise TypeError(message)
        for entry in entries:
            package, value = parse_dependency_entry(entry)
            if package is None:
                includes.add(value)
            elif package in pins:
                message = f"duplicate direct dependency: {package}"
                raise ValueError(message)
            else:
                pins[package] = value
                owners[package] = group
    missing_groups = includes - set(groups)
    if missing_groups:
        message = f"included dependency groups do not exist: {sorted(missing_groups)}"
        raise ValueError(message)
    if len(pins) < MIN_DIRECT_DEPENDENCIES:
        message = f"only {len(pins)} direct dependencies; policy floor is stale"
        raise ValueError(message)
    return pins, owners


def direct_pins(path: Path) -> dict[str, str]:
    """Return direct exact pins for callers that do not need owning groups."""
    return direct_declarations(path)[0]


def imported_roots(path: Path) -> tuple[set[str], list[str]]:
    """Return absolute import roots, including literal dynamic imports."""
    source, error = read_authored_text(path, "Python import policy input")
    if error is not None:
        return set(), [error]
    try:
        tree = ast.parse(source or "", filename=str(path))
    except SyntaxError as error:
        return set(), [f"{path}: cannot inspect Python imports: {error}"]
    roots: set[str] = set()
    importlib_modules = {"importlib"}
    import_functions = {"__import__"}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            roots.update(alias.name.partition(".")[0] for alias in node.names)
            importlib_modules.update(
                alias.asname or alias.name for alias in node.names if alias.name == "importlib"
            )
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            roots.add(node.module.partition(".")[0])
            if node.module == "importlib":
                import_functions.update(
                    alias.asname or alias.name
                    for alias in node.names
                    if alias.name == "import_module"
                )
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and node.args:
            function = node.func
            is_dynamic = (isinstance(function, ast.Name) and function.id in import_functions) or (
                isinstance(function, ast.Attribute)
                and isinstance(function.value, ast.Name)
                and function.value.id in importlib_modules
                and function.attr == "import_module"
            )
            argument = node.args[0]
            if (
                is_dynamic
                and isinstance(argument, ast.Constant)
                and isinstance(argument.value, str)
            ):
                roots.add(argument.value.partition(".")[0])
    return roots, []


def import_consumer_findings(
    root: Path,
    pins: Mapping[str, str],
    catalog: ConsumerCatalog,
) -> list[str]:
    """Find unclassified imports and imports without direct dependency proofs."""
    findings: list[str] = []
    sources, closure_errors = first_party_import_closure(
        root, python_source_paths(root), imported_roots
    )
    findings.extend(closure_errors)
    if len(sources) < catalog.minimum_python_files:
        findings.append(
            f"only {len(sources)} first-party Python files; import-consumer scan floor is stale"
        )
    local_roots = {path.stem for path in sources}
    local_roots.update(path.parent.name for path in sources if path.name == "__init__.py")
    local_roots.update(path.relative_to(root).parts[0] for path in sources)
    used_packages: set[str] = set()
    for path in sources:
        imported, errors = imported_roots(path)
        findings.extend(errors)
        for root_name in imported:
            package = catalog.external_imports.get(root_name)
            if package is not None:
                used_packages.add(package)
            elif (
                root_name not in INTERPRETER_IMPORT_ROOTS
                and root_name not in sys.stdlib_module_names
                and root_name not in local_roots
            ):
                findings.append(
                    f"{path.relative_to(root)}: unclassified third-party import root {root_name!r}"
                )
    for package in sorted(used_packages):
        if package not in pins:
            findings.append(f"imported dependency {package} is not directly pinned")
        if package not in catalog.proofs:
            findings.append(f"imported dependency {package} has no consumer/group proof")
    unused_mappings = set(catalog.external_imports.values()) - used_packages
    if unused_mappings:
        findings.append(f"external import map has no live imports: {sorted(unused_mappings)}")
    orphan_mappings = set(catalog.external_imports.values()) - set(catalog.proofs)
    if orphan_mappings:
        findings.append(f"external import map has no consumer proofs: {sorted(orphan_mappings)}")
    return findings


def check_consumers(
    root: Path,
    pins: dict[str, str],
    owners: dict[str, str],
    catalog: ConsumerCatalog = CONSUMER_CATALOG,
) -> list[str]:
    """Prove dependency pins, groups, consumers, and Python imports in both directions."""
    findings: list[str] = []
    if set(pins) != set(catalog.proofs):
        findings.append(
            f"direct dependency/consumer map differs: pins={sorted(pins)}, "
            f"consumers={sorted(catalog.proofs)}"
        )
    if set(pins) != set(owners):
        findings.append("direct dependency owner map differs from direct pins")
    for package, proof in catalog.proofs.items():
        if owners.get(package) != proof.group:
            findings.append(
                f"{package}: expected direct group {proof.group}, found {owners.get(package)!r}"
            )
        path = root / proof.relative
        source, error = read_authored_text(path, "dependency consumer proof")
        if error is not None:
            findings.append(error)
        elif proof.needle not in (source or ""):
            findings.append(f"{package}: consumer proof missing from {proof.relative}")
    findings.extend(import_consumer_findings(root, pins, catalog))
    if catalog.cli_census:
        findings.extend(cli_consumer_findings(root, pins))
    return findings


def check_manifest(path: Path) -> list[str]:
    """Validate the one official uv version/checksum platform manifest."""
    findings: list[str] = []
    document = json.loads(path.read_text(encoding="ascii"))
    if document.get("schema") != 1 or document.get("repository") != "astral-sh/uv":
        findings.append("uv release manifest identity/schema is invalid")
    version = document.get("version")
    if not isinstance(version, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        findings.append("uv release version is not exact semver")
    assets = document.get("assets")
    if not isinstance(assets, dict) or set(assets) != set(EXPECTED_ASSETS):
        return [*findings, "uv release platform matrix is not exact"]
    for key, expected_name in EXPECTED_ASSETS.items():
        record = assets.get(key)
        if not isinstance(record, dict) or set(record) != {"name", "sha256"}:
            findings.append(f"{key}: malformed uv asset record")
            continue
        if record["name"] != expected_name:
            findings.append(f"{key}: expected asset name {expected_name}")
        digest = record["sha256"]
        if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            findings.append(f"{key}: invalid uv asset SHA-256")
    return findings


def uv_cache_roots(
    root: Path,
    environment: Mapping[str, str] | None = None,
    executable: str | None = None,
) -> tuple[Path, ...]:
    """Return explicit authenticated-cache locations for source and snapshots."""
    env = os.environ if environment is None else environment
    candidates = [root / ".tools" / "uv"]
    configured = env.get("RA8_UV_CACHE_ROOT")
    if configured:
        candidates.append(Path(configured))
    history_root = env.get("RA8_CI_HISTORY_REPO")
    if history_root:
        candidates.append(Path(history_root) / ".tools" / "uv")
    if env.get("RA8_STAGED_HOOK_SNAPSHOT") == "1":
        source_paths = [
            Path(executable or "/usr/bin/python3"),
            *(Path(item) for item in env["PATH"].split(":")),
        ]
        for source_path in source_paths:
            candidate = source_path.absolute()
            bin_dir = candidate if candidate.name == "bin" else candidate.parent
            if bin_dir.name == "bin" and bin_dir.parent.name == ".venv":
                candidates.append(bin_dir.parents[1] / ".tools" / "uv")
    return tuple(dict.fromkeys(path.absolute() for path in candidates))


def cache_routing_selftest(root: Path) -> list[str]:
    """Prove snapshots and managed environments retain an explicit cache route."""
    failures: list[str] = []
    local_cache = root / ".tools" / "uv"
    source_root = root / "source"
    snapshot_root = root / "snapshot"
    staged = uv_cache_roots(
        snapshot_root,
        {
            "RA8_STAGED_HOOK_SNAPSHOT": "1",
            "PATH": f"{source_root / '.venv' / 'bin'}:/usr/bin",
        },
        "/usr/bin/python3",
    )
    if staged != (snapshot_root / ".tools" / "uv", source_root / ".tools" / "uv"):
        failures.append(f"staged cache routing failed: {staged}")
    configured = uv_cache_roots(
        root,
        {
            "RA8_UV_CACHE_ROOT": str(root / "configured"),
            "RA8_CI_HISTORY_REPO": str(root / "history"),
        },
    )
    if configured != (
        local_cache,
        root / "configured",
        root / "history" / ".tools" / "uv",
    ):
        failures.append(f"explicit cache routing failed: {configured}")
    return failures


def consumer_selftest(root: Path) -> list[str]:
    """Prove consumer and import coverage detects omissions and wrong groups."""
    failures: list[str] = []
    root.mkdir(parents=True, exist_ok=True)
    source = root / "consumer.py"
    source.write_text(
        "import __main__\n"
        "import importlib as loader\n"
        "import pathlib\n"
        "loader.import_module('yaml')\n",
        encoding="utf-8",
    )
    pins = {"pyyaml": "6.0.3"}
    owners = {"pyyaml": "runtime"}
    proofs = {"pyyaml": ConsumerProof("runtime", "consumer.py", "loader.import_module('yaml')")}
    catalog = ConsumerCatalog(proofs, {"yaml": "pyyaml"}, 1, cli_census=False)
    if check_consumers(root, pins, owners, catalog):
        failures.append("valid consumer/group/import fixture failed")
    if not check_consumers(root, pins, {"pyyaml": "hil"}, catalog):
        failures.append("wrong direct dependency group passed")
    omitted_catalog = ConsumerCatalog({}, {"yaml": "pyyaml"}, 1, cli_census=False)
    if not check_consumers(root, {}, {}, omitted_catalog):
        failures.append("dependency omitted from both pins and proofs passed")
    if not check_consumers(root, {}, {}, ConsumerCatalog({}, {}, 1, cli_census=False)):
        failures.append("unclassified imported dependency passed")
    source.write_text("import pathlib\n", encoding="utf-8")
    if not check_consumers(root, pins, owners, catalog):
        failures.append("consumer proof with no matching source passed")
    source.write_bytes(b"# invalid utf-8: \xa3\n")
    if not any("not valid UTF-8" in item for item in check_consumers(root, pins, owners, catalog)):
        failures.append("non-UTF-8 Python consumer did not fail clearly")
    failures.extend(adjacent_import_closure_selftest(root, source, imported_roots))
    return failures


def cli_consumer_selftest(root: Path) -> list[str]:
    """Prove every CLI-only package survives an omit-pin/owner/proof attack."""
    failures: list[str] = []
    root.mkdir(parents=True, exist_ok=True)
    (root / "consumers.sh").write_text(
        "ruff --version\n"
        "cmake-format --version\n"
        "gcovr --version\n"
        "yamllint --version\n"
        "ansible-playbook --version\n",
        encoding="utf-8",
    )
    python = root / "consumer.py"
    python.write_text(
        'import shutil\nfilesystem = shutil\nlocator = filesystem.which\ncommand = "vela"\n'
        "locator(command)\n",
        encoding="utf-8",
    )
    groups = {
        "ansible-core": "infra",
        "cmakelang": "dev",
        "ethos-u-vela": "vela",
        "gcovr": "dev",
        "ruff": "dev",
        "yamllint": "dev",
    }
    needles = {
        "ansible-core": "ansible-playbook --version",
        "cmakelang": "cmake-format --version",
        "ethos-u-vela": "locator(command)",
        "gcovr": "gcovr --version",
        "ruff": "ruff --version",
        "yamllint": "yamllint --version",
    }
    proofs = {
        package: ConsumerProof(
            group,
            "consumer.py" if package == "ethos-u-vela" else "consumers.sh",
            needles[package],
        )
        for package, group in groups.items()
    }
    pins = dict.fromkeys(groups, "1.0")
    catalog = ConsumerCatalog(proofs, {}, 1)
    if check_consumers(root, pins, groups, catalog):
        failures.append("valid independent CLI consumer census failed")
    for package in groups:
        reduced_pins = {name: pin for name, pin in pins.items() if name != package}
        reduced_owners = {name: group for name, group in groups.items() if name != package}
        reduced_proofs = {name: proof for name, proof in proofs.items() if name != package}
        findings = check_consumers(
            root,
            reduced_pins,
            reduced_owners,
            ConsumerCatalog(reduced_proofs, {}, 1),
        )
        expected = f"CLI dependency {package} is invoked but not directly pinned"
        if expected not in findings:
            failures.append(f"CLI omit-pin/owner/proof attack passed: {package}")
    return failures


def authority_selftest(root: Path) -> list[str]:
    """Prove allowed derived/vendor metadata is quiet and parallel authorities fail."""
    failures: list[str] = []
    root.mkdir(parents=True, exist_ok=True)
    (root / "pyproject.toml").write_text("[dependency-groups]\n", encoding="utf-8")
    (root / "uv.lock").write_text("version = 1\n", encoding="utf-8")
    (root / GALAXY_MANIFEST).parent.mkdir(parents=True, exist_ok=True)
    (root / GALAXY_MANIFEST).write_text("collections: []\n", encoding="utf-8")
    for relative in EXPORTS.values():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("# derived\n", encoding="utf-8")
    vendor = root / VENDOR_BOUNDARIES[0]
    vendor.mkdir(parents=True, exist_ok=True)
    vendor_names = [
        "requirements.txt",
        "pyproject.toml",
        "uv.lock",
        *sorted(SECONDARY_AUTHORITY_NAMES),
    ]
    for name in vendor_names:
        (vendor / name).write_text("# upstream\n", encoding="utf-8")
    prose = root / "docs" / "product-requirements" / "notes.txt"
    prose.parent.mkdir(parents=True, exist_ok=True)
    prose.write_text("Operational requirements are described in prose.\n", encoding="utf-8")
    constraints_prose = root / "docs" / "product-constraints" / "notes.txt"
    constraints_prose.parent.mkdir(parents=True, exist_ok=True)
    constraints_prose.write_text("Product constraints are described in prose.\n", encoding="utf-8")
    data = root / "data" / "requirements.json"
    data.parent.mkdir(parents=True, exist_ok=True)
    data.write_text('{"labels": ["alpha", "beta"]}\n', encoding="utf-8")
    constraints_data = root / "data" / "constraints.json"
    constraints_data.write_text('{"limits": ["alpha", "beta"]}\n', encoding="utf-8")
    if requirement_findings(root):
        failures.append("allowed derived, vendor, prose, or data metadata failed")

    stale_paths = [
        (root / "requirements.txt", "split==1\n"),
        (root / "constraints-dev.txt", "split==1\n"),
        *((root / name, "# split authority\n") for name in sorted(SECONDARY_AUTHORITY_NAMES)),
        (root / "tools" / "nested" / "pyproject.toml", "# split authority\n"),
        (root / "tools" / "nested" / "uv.lock", "# split authority\n"),
        (root / "config" / "requirements" / "dev.txt", "nested-package\n"),
        (root / "config" / "constraints" / "dev.txt", "nested-package\n"),
    ]
    for stale, content in stale_paths:
        stale.parent.mkdir(parents=True, exist_ok=True)
        stale.write_text(content, encoding="utf-8")
        if not any(str(stale.relative_to(root)) in item for item in requirement_findings(root)):
            failures.append(f"secondary dependency authority passed: {stale.relative_to(root)}")
        stale.unlink()
    return failures


def unsafe_installer_fixtures() -> dict[str, str]:
    """Return adversarial installer snippets that the policy must reject."""
    return {
        "argv-pip.py": (
            "import subprocess, sys\n"
            "subprocess.run([sys.executable, '-m', 'pip', 'install', 'rogue'], check=True)\n"
        ),
        "argv-uv.py": (
            "import subprocess\n"
            "subprocess.run(['/opt/bin/uv', 'pip', 'install', 'rogue'], check=True)\n"
        ),
        "alias-pip.py": (
            "import sys\nfrom subprocess import run as launch\n"
            "launch([sys.executable, '-m', 'pip', 'install', 'rogue'], check=True)\n"
        ),
        "alias-keyword.py": (
            "import subprocess as runner, sys as runtime\n"
            "runner.run("
            "args=[runtime.executable, '-m', 'pip', 'install', 'rogue'], check=True)\n"
        ),
        "from-alias-keyword.py": (
            "from subprocess import run as launch\n"
            "from sys import executable as interpreter\n"
            "launch(args=[interpreter, '-m', 'pip', 'install', 'rogue'], check=True)\n"
        ),
        "os-system.py": (
            "import os as host_os\nhost_os.system('python3.11 -m pip3.11 install rogue')\n"
        ),
        "assigned-argv.py": (
            "import subprocess, sys\n"
            "command = [sys.executable, '-m', 'pip', 'install', 'rogue']\n"
            "subprocess.run(command, check=True)\n"
        ),
        "assigned-launcher.py": (
            "import subprocess, sys\nprocess = subprocess\nlauncher = process.run\n"
            "interpreter = sys.executable\n"
            "launcher([interpreter, '-m', 'pip', 'install', 'rogue'], check=True)\n"
        ),
        "assigned-tuple.py": (
            "from subprocess import run\ncommand = ('uvx', 'rogue')\nrun(command, check=True)\n"
        ),
        "assigned-shell.py": (
            "import os\ncommand = 'python3 -m pip install rogue'\nos.system(command)\n"
        ),
        "concatenated-shell.py": ("import os\nos.system('python3 -m pip ' + 'install rogue')\n"),
        "formatted-shell.py": (
            "import subprocess\npackage = 'rogue'\n"
            "subprocess.run(f'python3 -m pip install {package}', shell=True)\n"
        ),
        "shell.sh": "python3 -m pip install rogue\n",
        "uvx.sh": "uvx rogue\n",
        "windows.cmd": "py.exe -m pip.exe install rogue\n",
    }


def unsafe_installer_selftest(root: Path) -> list[str]:
    """Prove shell and Python process installers fail while non-installs remain valid."""
    failures: list[str] = []
    root.mkdir(parents=True, exist_ok=True)
    safe = root / "safe.py"
    safe.write_text(
        "import subprocess as runner, sys as runtime\n"
        "command = [runtime.executable, '-m', 'pip', 'check']\n"
        "runner.run(args=command, check=True)\n",
        encoding="utf-8",
    )
    if unsafe_install_findings(root):
        failures.append("safe Python dependency graph check was rejected")
    for name, content in unsafe_installer_fixtures().items():
        fixture = root / name
        fixture.write_text(content, encoding="utf-8")
        if not any(name in item for item in unsafe_install_findings(root)):
            failures.append(f"unsafe installer passed: {name}")
        fixture.unlink()
    vendor = root / "libs" / "third_party" / "upstream" / "install.sh"
    vendor.parent.mkdir(parents=True, exist_ok=True)
    vendor.write_text("pip install upstream-build-helper\n", encoding="utf-8")
    if unsafe_install_findings(root):
        failures.append("vendored installer was treated as first-party policy")
    return failures


def export_freshness_selftest(uv: AuthenticatedUv) -> list[str]:
    """Prove current lock/exports pass and mutations fail with authenticated uv."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-uv-export-test-") as raw:
        fixture = Path(raw)
        shutil.copy2(PYPROJECT, fixture / "pyproject.toml")
        shutil.copy2(ROOT / "uv.lock", fixture / "uv.lock")
        for relative in EXPORTS.values():
            target = fixture / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
        if export_findings(fixture, EXPORTS, uv):
            failures.append("current lock/export freshness fixture failed")

        stale_relative = next(iter(EXPORTS.values()))
        stale_export = fixture / stale_relative
        stale_export.write_text(
            stale_export.read_text(encoding="utf-8") + "# stale\n", encoding="utf-8"
        )
        if not any(
            "is stale versus uv.lock" in item for item in export_findings(fixture, EXPORTS, uv)
        ):
            failures.append("mutated derived export passed freshness check")
        shutil.copy2(ROOT / stale_relative, stale_export)

        project = fixture / "pyproject.toml"
        content = project.read_text(encoding="utf-8")
        project.write_text(content.replace("PyYAML==6.0.3", "PyYAML==6.0.2"), encoding="utf-8")
        if not any("uv.lock is stale" in item for item in export_findings(fixture, EXPORTS, uv)):
            failures.append("mutated project passed lock freshness check")
    return failures


def selftest() -> int:
    """Prove every lock-policy boundary detects valid and invalid fixtures."""
    with tempfile.TemporaryDirectory(prefix="ra8-python-policy-test-") as raw:
        root = Path(raw)
        project = root / "pyproject.toml"
        failures: list[str] = []
        entries = [f"package-{index}==1.0.0" for index in range(MIN_DIRECT_DEPENDENCIES)]
        project.write_text(
            "[dependency-groups]\ndev = [" + ",".join(repr(item) for item in entries) + "]\n",
            encoding="utf-8",
        )
        pins, owners = direct_declarations(project)
        if len(pins) != MIN_DIRECT_DEPENDENCIES or set(owners.values()) != {"dev"}:
            failures.append("valid direct pins failed")
        for extra in (entries[0], "package-0==2.0.0", "loose>=1"):
            project.write_text(
                "[dependency-groups]\ndev = ["
                + ",".join(repr(item) for item in [*entries, extra])
                + "]\n",
                encoding="utf-8",
            )
            try:
                direct_pins(project)
            except ValueError:
                pass
            else:
                failures.append(f"invalid direct pin passed: {extra}")
        manifest = json.loads(MANIFEST.read_text(encoding="ascii"))
        fixture = root / "uv.json"
        fixture.write_text(json.dumps(manifest), encoding="ascii")
        if check_manifest(fixture):
            failures.append("valid uv manifest failed")
        manifest["assets"].pop(next(iter(manifest["assets"])))
        fixture.write_text(json.dumps(manifest), encoding="ascii")
        if not check_manifest(fixture):
            failures.append("incomplete uv matrix passed")
        failures.extend(consumer_selftest(root / "consumers"))
        failures.extend(cli_consumer_selftest(root / "cli-consumers"))
        failures.extend(authority_selftest(root / "authorities"))
        failures.extend(unsafe_installer_selftest(root / "installers"))
        failures.extend(scanner_selection_selftest(root / "selection"))
        failures.extend(cache_routing_selftest(root))
        failures.extend(uv_cache_policy_selftest(ROOT))
        failures.extend(uv_execution_policy_selftest(ROOT))
        failures.extend(hil_preflight_selftest(ROOT))
        live_manifest = json.loads(MANIFEST.read_text(encoding="ascii"))
        version = live_manifest.get("version")
        if not isinstance(version, str):
            failures.append("live uv manifest has no version")
        else:
            uv = find_uv(ROOT, version, MANIFEST, uv_cache_roots(ROOT))
            failures.extend(export_freshness_selftest(uv))
            failures.extend(execution_attack_selftest(BOOTSTRAP, EXPORTS))
        if failures:
            print("selftest: " + "; ".join(failures), file=sys.stderr)
            return 1
    print("check_python_lock_policy.py --selftest: PASS")
    return 0


def main() -> int:
    """Run the offline policy or its synthetic selftest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    try:
        pins, owners = direct_declarations(PYPROJECT)
        manifest = json.loads(MANIFEST.read_text(encoding="ascii"))
        version = manifest.get("version")
        if not isinstance(version, str):
            return fatal("uv manifest version is malformed")
        uv = find_uv(ROOT, version, MANIFEST, uv_cache_roots(ROOT))
        findings = [
            *check_consumers(ROOT, pins, owners),
            *check_manifest(MANIFEST),
            *requirement_findings(ROOT),
            *unsafe_install_findings(ROOT),
            *export_findings(ROOT, EXPORTS, uv),
            *hil_preflight_findings(load_hil_tasks(ROOT)),
            *uv_cache_policy_findings(ROOT),
            *uv_execution_policy_findings(ROOT),
        ]
    except (
        OSError,
        TypeError,
        ValueError,
        json.JSONDecodeError,
        yaml.YAMLError,
        subprocess.SubprocessError,
    ) as error:
        print(f"check_python_lock_policy.py: FATAL: {error}", file=sys.stderr)
        return 2
    if findings:
        print("\n".join(findings), file=sys.stderr)
        return 1
    print("Python dependencies, uv bootstrap, and managed exports match one lock")
    return 0


def fatal(message: str) -> int:
    """Report a fatal policy-input error."""
    print(f"check_python_lock_policy.py: FATAL: {message}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
