#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Check installed Ansible collections against the exact Galaxy manifest."""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
import tempfile
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

import fleet_path_authority as fpa

ROOT = Path(__file__).resolve().parents[2]
REQUIREMENTS = ROOT / "infra" / "ansible" / "requirements.yml"
ANSIBLE_ROOT = ROOT / "infra" / "ansible"
ESSENTIAL_COLLECTIONS = frozenset({"ansible.posix", "community.hashi_vault", "kubernetes.core"})
BUILTIN_COLLECTIONS = frozenset({"ansible.builtin", "ansible.legacy"})
MODULE_RE = re.compile(r"^\s*([a-z][a-z0-9_]*\.[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*):\s*(?:#.*)?$")
LOOKUP_RE = re.compile(r"\b(?:lookup|query)\(\s*['\"]([a-z][a-z0-9_]*\.[a-z][a-z0-9_]*)\.")
CALLBACK_RE = re.compile(r"([a-z][a-z0-9_]*\.[a-z][a-z0-9_]*)\.[a-z][a-z0-9_]*")


def _callback_collections(root: Path) -> set[str]:
    """Discover collection callbacks selected by first-party Python transports."""
    scripts = root / "scripts" / "dev"
    search = scripts if scripts.is_dir() else root
    found: set[str] = set()
    for path in sorted(search.rglob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            if not isinstance(node, ast.Dict):
                continue
            for key, value in zip(node.keys, node.values, strict=True):
                if not (
                    isinstance(key, ast.Constant)
                    and key.value == "ANSIBLE_STDOUT_CALLBACK"
                    and isinstance(value, ast.Constant)
                    and isinstance(value.value, str)
                ):
                    continue
                match = CALLBACK_RE.fullmatch(value.value)
                if match is not None:
                    found.add(match.group(1))
    return found


def consumer_collections(root: Path = ROOT) -> set[str]:
    """Discover Galaxy names used by Ansible YAML and Python callbacks."""
    ansible_root = root / "infra" / "ansible"
    if not ansible_root.is_dir():
        ansible_root = root
    found: set[str] = set()
    for path in sorted((*ansible_root.rglob("*.yml"), *ansible_root.rglob("*.yaml"))):
        if path.name == "requirements.yml":
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.lstrip().startswith("#"):
                continue
            module_match = MODULE_RE.match(line)
            if module_match is not None:
                found.add(".".join(module_match.group(1).split(".")[:2]))
            found.update(LOOKUP_RE.findall(line))
    found.update(_callback_collections(root))
    return found - BUILTIN_COLLECTIONS


def expected_versions(path: Path = REQUIREMENTS, consumer_root: Path = ROOT) -> dict[str, str]:
    """Return exact collection versions from the repository manifest."""
    document = yaml.safe_load(path.read_text(encoding="ascii"))
    collections = document.get("collections") if isinstance(document, dict) else None
    if not isinstance(collections, list) or not collections:
        message = "Ansible collection manifest is empty or malformed"
        raise ValueError(message)
    expected: dict[str, str] = {}
    for record in collections:
        if not isinstance(record, dict):
            message = "Ansible collection record must be a mapping"
            raise TypeError(message)
        name = record.get("name")
        version = record.get("version")
        if not isinstance(name, str) or not isinstance(version, str):
            message = "Ansible collection name and version must be strings"
            raise TypeError(message)
        if (
            name in expected
            or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?", version) is None
        ):
            message = f"Ansible collection {name!r} is duplicated or not a semver pin"
            raise ValueError(message)
        expected[name] = version
    consumers = consumer_collections(consumer_root)
    missing_essential = ESSENTIAL_COLLECTIONS - consumers
    if missing_essential:
        message = f"essential Ansible consumers disappeared: {sorted(missing_essential)}"
        raise ValueError(message)
    if set(expected) != consumers:
        message = (
            "Galaxy manifest/consumer mismatch: "
            f"manifest-only={sorted(set(expected) - consumers)}, "
            f"consumer-only={sorted(consumers - set(expected))}"
        )
        raise ValueError(message)
    return expected


def manifest_selftest() -> None:
    """Prove manifest/consumer exactness and the essential intent floor."""
    with tempfile.TemporaryDirectory() as raw_root:
        root = Path(raw_root)
        tasks = root / "roles" / "sample" / "tasks"
        tasks.mkdir(parents=True)
        scripts = root / "scripts" / "dev"
        scripts.mkdir(parents=True)
        consumers = """---
- name: Kubernetes consumer
  kubernetes.core.k8s:
- name: Vault consumer
  ansible.builtin.debug:
    msg: "{{ lookup('community.hashi_vault.vault_kv2_get', 'secret') }}"
"""
        (tasks / "main.yml").write_text(consumers, encoding="utf-8")
        callback = """environment = {
    "ANSIBLE_STDOUT_CALLBACK": "ansible.posix.json",
}
"""
        (scripts / "callback.py").write_text(callback, encoding="utf-8")
        manifest = root / "requirements.yml"
        exact = """---
collections:
  - name: ansible.posix
    version: 2.2.0
  - name: community.hashi_vault
    version: 7.1.0
  - name: kubernetes.core
    version: 6.5.0
"""
        manifest.write_text(exact, encoding="utf-8")
        expected_versions(manifest, root)
        if consumer_collections(root) != ESSENTIAL_COLLECTIONS:
            message = "YAML/module/lookup/callback consumer discovery drifted"
            raise AssertionError(message)
        manifest.write_text(exact + "  - name: stale.extra\n    version: 1.0.0\n")
        try:
            expected_versions(manifest, root)
        except ValueError:
            pass
        else:
            message = "manifest-only collection passed"
            raise AssertionError(message)
        manifest.write_text(exact.replace("  - name: kubernetes.core\n    version: 6.5.0\n", ""))
        try:
            expected_versions(manifest, root)
        except ValueError:
            pass
        else:
            message = "consumer-only collection passed"
            raise AssertionError(message)


def installed_versions(document: object, root: Path) -> dict[str, list[tuple[str, str]]]:
    """Inventory every local physical collection location without collapsing it."""
    if not isinstance(document, dict):
        message = "ansible-galaxy collection list JSON must be an object"
        raise TypeError(message)
    root = root.absolute()
    installed: dict[str, list[tuple[str, str]]] = {}
    for inventory_path, collections in document.items():
        if not isinstance(inventory_path, str) or not isinstance(collections, dict):
            continue
        try:
            Path(inventory_path).absolute().relative_to(root)
        except ValueError:
            continue
        for name, record in collections.items():
            if isinstance(name, str) and isinstance(record, dict):
                version = record.get("version")
                if isinstance(version, str):
                    installed.setdefault(name, []).append((inventory_path, version))
    return installed


def check(document: object, root: Path, expected: dict[str, str] | None = None) -> list[str]:
    """Return missing, extra, duplicate-location, and wrong-version findings."""
    wanted = expected or expected_versions()
    present = installed_versions(document, root)
    findings = fpa.confined_link_errors(root)
    for name in sorted(wanted.keys() | present.keys()):
        records = present.get(name, [])
        required = wanted.get(name)
        if required is None:
            findings.append(f"{name}: unexpected local collection {records}")
        elif len(records) != 1 or records[0][1] != required:
            findings.append(f"{name}: expected one {required}, installed {records or ['absent']}")
    return findings


def selftest() -> int:
    """Prove scoped exact-set checking in every direction."""
    manifest_selftest()
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw) / "collections"
        local_root = root / "ansible_collections"
        local_root.mkdir(parents=True)
        expected = {"ansible.posix": "1.2.3", "kubernetes.core": "4.5.6"}
        local = str(local_root)
        good = {
            local: {name: {"version": version} for name, version in expected.items()},
            "/usr/share/ansible/collections": {"global.extra": {"version": "9.9.9"}},
        }
        if check(good, root, expected):
            print("selftest: matching scoped inventory failed", file=sys.stderr)
            return 1
        cases = (
            {local: {"ansible.posix": {"version": "1.2.3"}}},
            {local: {**good[local], "local.extra": {"version": "1.0.0"}}},
            {local: {name: {"version": "0.0.0"} for name in expected}},
            {
                local: good[local],
                str(root / "duplicate"): {"ansible.posix": {"version": "1.2.3"}},
            },
        )
        if any(not check(case, root, expected) for case in cases):
            print("selftest: missing/extra/wrong/duplicate case passed", file=sys.stderr)
            return 1
        outside = Path(raw) / "outside"
        outside.mkdir()
        link = local_root / "ansible"
        link.symlink_to(outside, target_is_directory=True)
        if not check(good, root, expected):
            print("selftest: escaping collection link passed", file=sys.stderr)
            return 1
    print("check_ansible_collections.py --selftest: PASS")
    return 0


def main() -> int:
    """Check stdin inventory or run the offline selftest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdin", action="store_true", help="read ansible-galaxy JSON from stdin")
    parser.add_argument("--root", type=Path, help="repository-local collections root")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if not args.stdin or args.root is None:
        parser.error("--stdin and --root are required outside selftest mode")
    try:
        document = json.load(sys.stdin)
        findings = check(document, args.root)
    except (OSError, TypeError, ValueError, json.JSONDecodeError, yaml.YAMLError) as error:
        print(f"check_ansible_collections.py: FATAL: {error}", file=sys.stderr)
        return 2
    if findings:
        print("\n".join(findings), file=sys.stderr)
        return 1
    print("Ansible Galaxy collections match infra/ansible/requirements.yml")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
