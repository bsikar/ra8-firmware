#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate the offline-checkable ESP32-C6 staging and link contract.

The pinned ESP-hosted patch can apply cleanly while the eventual image is still
wrong. A renamed first-party header may make ``build.sh`` stage a nonexistent
path, the patched explicit component set may omit the directory we stage, or a
component ABI marker may become private while the post-link assertion retains
its old name. Those failures otherwise appear only in a full ESP-IDF build.

This gate checks the committed recipe without ESP-IDF, a network, or hardware:
every staged source exists, copies retain their basename, component identities
agree, and the patch/header/source/post-link symbol contracts form one chain.
A full pinned ESP-IDF build remains the end-to-end proof.

Run::

    check_c6_integration.py
    check_c6_integration.py --selftest
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
C6_DIR = REPO_ROOT / "coprocessor" / "esp32c6"
BUILD_SCRIPT = C6_DIR / "build.sh"
PATCH_FILE = C6_DIR / "patches" / "0001-custom-rpc-sync-response-hook.patch"
SERVICE_HEADER = REPO_ROOT / "port" / "esp32_c6" / "inc" / "ra8_mdl_service.h"
SERVICE_SOURCE = REPO_ROOT / "port" / "esp32_c6" / "src" / "mdl_service.c"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

COMPONENT_ABI = "ra8_mdl_service_component_abi"
CUSTOM_RPC_HOOK = "esp_hosted_custom_rpc_sync_handler"

_STAGED_COPY_RE = re.compile(
    r'cp\s+"\$\{SCRIPT_DIR\}/\.\./\.\./(?P<src>[^"]+)"\s*'
    r'(?:\\\s*)?"\$\{COMPONENT_DIR\}/(?P<dest>[^"]+)"',
    re.MULTILINE,
)
_COMPONENT_DIR_RE = re.compile(
    r'^[ \t]*COMPONENT_DIR="\$\{PERIPHERAL_DIR\}/components/(?P<name>[^"/]+)"$',
    re.MULTILINE,
)
_PATCH_COMPONENTS_RE = re.compile(r"^\+set\(COMPONENTS (?P<names>[^)]+)\)$", re.MULTILINE)

# These critical copies must not silently disappear from the recipe. Existence
# checks alone only prove the copies that remain, so they cannot catch removal.
REQUIRED_STAGED_SOURCES: frozenset[str] = frozenset(
    {
        "port/esp32_c6/CMakeLists.txt",
        "port/esp32_c6/src/mdl_service.c",
        "port/esp32_c6/inc/ra8_mdl_service.h",
        "libs/ra8_c6link/inc/ra8_mdl_protocol.h",
        "libs/ra8_c6link/inc/ra8_mdl_http.h",
    }
)


def parse_staged_copies(build_text: str) -> list[tuple[str, str]]:
    """Return repository source and component destination for each staged copy."""
    return [match.group("src", "dest") for match in _STAGED_COPY_RE.finditer(build_text)]


def _check_staged_copies(build_text: str, available_sources: frozenset[str] | None) -> list[str]:
    """Return findings for missing, renamed, or nonexistent staged files."""
    findings: list[str] = []
    copies = parse_staged_copies(build_text)
    if not copies:
        findings.append("staging: build.sh contains no first-party component copies")

    staged_sources = {source for source, _destination in copies}
    findings.extend(
        f"staging: build.sh no longer stages required source {required}"
        for required in sorted(REQUIRED_STAGED_SOURCES - staged_sources)
    )
    for source, destination in copies:
        exists = (
            source in available_sources
            if available_sources is not None
            else (REPO_ROOT / source).is_file()
        )
        if not exists:
            findings.append(f"staging: source does not exist: {source}")
        if Path(source).name != Path(destination).name:
            findings.append(
                f"staging: {source} is renamed to {destination}; copied component "
                "files must retain their source basename"
            )
    return findings


def _check_component(build_text: str, patch_text: str) -> list[str]:
    """Return findings when staged and explicitly enabled component names differ."""
    findings: list[str] = []
    component_match = _COMPONENT_DIR_RE.search(build_text)
    patch_matches = list(_PATCH_COMPONENTS_RE.finditer(patch_text))
    if component_match is None:
        findings.append("component: build.sh does not declare a literal staged component name")
    if len(patch_matches) != 1:
        findings.append(
            "component: patch must add exactly one explicit set(COMPONENTS ...) declaration"
        )
    if component_match is not None and len(patch_matches) == 1:
        component = component_match.group("name")
        if component not in patch_matches[0].group("names").split():
            findings.append(
                f"component: build.sh stages {component!r}, but the patch's explicit "
                "COMPONENTS set does not include it"
            )
    return findings


def _check_symbols(
    build_text: str, patch_text: str, header_text: str, source_text: str
) -> list[str]:
    """Return findings for public, weak, strong, and post-link symbols."""
    findings: list[str] = []
    abi_decl = re.compile(rf"^\s*uint32_t\s+{COMPONENT_ABI}\s*\(\s*void\s*\)\s*;", re.MULTILINE)
    abi_definition = re.compile(
        rf"^\s*(?:\[\[[^\n]+\]\]\s*)?uint32_t\s+{COMPONENT_ABI}\s*\(\s*void\s*\)",
        re.MULTILINE,
    )
    if abi_decl.search(header_text) is None:
        findings.append(f"ABI: public header does not declare {COMPONENT_ABI}(void)")
    if abi_definition.search(source_text) is None:
        findings.append(f"ABI: source does not define externally visible {COMPONENT_ABI}(void)")
    if f"T[[:space:]]+{COMPONENT_ABI}$" not in build_text:
        findings.append(f"ABI: build.sh does not require strong text symbol {COMPONENT_ABI}")

    weak_hook = re.compile(
        rf"^\+__attribute__\(\(weak\)\)\s+esp_err_t\s+{CUSTOM_RPC_HOOK}\s*\(",
        re.MULTILINE,
    )
    strong_hook = re.compile(rf"^\s*esp_err_t\s+{CUSTOM_RPC_HOOK}\s*\(", re.MULTILINE)
    if weak_hook.search(patch_text) is None:
        findings.append(f"hook: patch does not provide weak extension point {CUSTOM_RPC_HOOK}")
    if strong_hook.search(source_text) is None:
        findings.append(f"hook: component source does not define strong {CUSTOM_RPC_HOOK}")
    if f"T[[:space:]]+{CUSTOM_RPC_HOOK}$" not in build_text:
        findings.append(f"hook: build.sh does not require strong text symbol {CUSTOM_RPC_HOOK}")
    return findings


def check_contract(
    build_text: str,
    patch_text: str,
    header_text: str,
    source_text: str,
    available_sources: frozenset[str] | None = None,
) -> list[str]:
    """Return all offline-checkable C6 integration-contract findings."""
    findings = _check_staged_copies(build_text, available_sources)
    findings.extend(_check_component(build_text, patch_text))
    findings.extend(_check_symbols(build_text, patch_text, header_text, source_text))
    return findings


_GOOD_BUILD = r"""
  COMPONENT_DIR="${PERIPHERAL_DIR}/components/mdl_service"
cp "${SCRIPT_DIR}/../../port/esp32_c6/CMakeLists.txt" "${COMPONENT_DIR}/CMakeLists.txt"
cp "${SCRIPT_DIR}/../../port/esp32_c6/src/mdl_service.c" \
  "${COMPONENT_DIR}/src/mdl_service.c"
cp "${SCRIPT_DIR}/../../port/esp32_c6/inc/ra8_mdl_service.h" \
  "${COMPONENT_DIR}/include/ra8_mdl_service.h"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_protocol.h" \
  "${COMPONENT_DIR}/include/ra8_mdl_protocol.h"
cp "${SCRIPT_DIR}/../../libs/ra8_c6link/inc/ra8_mdl_http.h" \
  "${COMPONENT_DIR}/include/ra8_mdl_http.h"
grep -Eq 'T[[:space:]]+ra8_mdl_service_component_abi$'
grep -Eq 'T[[:space:]]+esp_hosted_custom_rpc_sync_handler$'
"""
_GOOD_PATCH = r"""
+set(COMPONENTS esp_timer main mdl_service)
+__attribute__((weak)) esp_err_t esp_hosted_custom_rpc_sync_handler(
"""
_GOOD_HEADER = "uint32_t ra8_mdl_service_component_abi(void);\n"
_GOOD_SOURCE = """
[[gnu::noinline]] uint32_t ra8_mdl_service_component_abi(void) { return 1U; }
esp_err_t esp_hosted_custom_rpc_sync_handler(uint32_t id) { return ESP_OK; }
"""

SelftestCase = tuple[str, str, str, str, str, bool]


def _selftest_staging_cases() -> list[SelftestCase]:
    """Return the quiet control and staged-file/component drift cases."""
    return [
        ("contract agrees", _GOOD_BUILD, _GOOD_PATCH, _GOOD_HEADER, _GOOD_SOURCE, False),
        (
            "staged source renamed away",
            _GOOD_BUILD.replace("inc/ra8_mdl_http.h", "inc/mdl_http.h", 1),
            _GOOD_PATCH,
            _GOOD_HEADER,
            _GOOD_SOURCE,
            True,
        ),
        (
            "destination kept stale basename",
            _GOOD_BUILD.replace("include/ra8_mdl_protocol.h", "include/mdl_protocol.h"),
            _GOOD_PATCH,
            _GOOD_HEADER,
            _GOOD_SOURCE,
            True,
        ),
        (
            "required copy removed",
            _GOOD_BUILD.replace(
                'cp "${SCRIPT_DIR}/../../port/esp32_c6/CMakeLists.txt" '
                '"${COMPONENT_DIR}/CMakeLists.txt"\n',
                "",
            ),
            _GOOD_PATCH,
            _GOOD_HEADER,
            _GOOD_SOURCE,
            True,
        ),
        (
            "component identity drifted",
            _GOOD_BUILD,
            _GOOD_PATCH.replace("main mdl_service", "main ra8_mdl_service"),
            _GOOD_HEADER,
            _GOOD_SOURCE,
            True,
        ),
        (
            "component assignment became nonliteral",
            _GOOD_BUILD.replace(
                '  COMPONENT_DIR="${PERIPHERAL_DIR}/components/mdl_service"',
                '  COMPONENT_DIR="${PERIPHERAL_DIR}/components/${COMPONENT_NAME}"',
            ),
            _GOOD_PATCH,
            _GOOD_HEADER,
            _GOOD_SOURCE,
            True,
        ),
    ]


def _selftest_symbol_cases() -> list[SelftestCase]:
    """Return public/private/post-link ABI and hook drift cases."""
    return [
        (
            "public ABI name drifted",
            _GOOD_BUILD,
            _GOOD_PATCH,
            _GOOD_HEADER.replace(COMPONENT_ABI, "mdl_service_component_abi"),
            _GOOD_SOURCE,
            True,
        ),
        (
            "ABI became private",
            _GOOD_BUILD,
            _GOOD_PATCH,
            _GOOD_HEADER,
            _GOOD_SOURCE.replace("[[gnu::noinline]] uint32_t", "static uint32_t"),
            True,
        ),
        (
            "post-link ABI name drifted",
            _GOOD_BUILD.replace(COMPONENT_ABI, "mdl_service_component_abi"),
            _GOOD_PATCH,
            _GOOD_HEADER,
            _GOOD_SOURCE,
            True,
        ),
        (
            "weak hook disappeared",
            _GOOD_BUILD,
            _GOOD_PATCH.replace(CUSTOM_RPC_HOOK, "custom_rpc_sync_handler"),
            _GOOD_HEADER,
            _GOOD_SOURCE,
            True,
        ),
    ]


def _selftest_cases() -> list[SelftestCase]:
    """Return one quiet control and one case for each protected seam."""
    return _selftest_staging_cases() + _selftest_symbol_cases()


def selftest() -> int:
    """Prove the detector fires on drift and stays quiet on agreement."""
    inventory = frozenset(source for source, _dest in parse_staged_copies(_GOOD_BUILD))
    failures: list[str] = []
    for label, build, patch, header, source, expect in _selftest_cases():
        findings = check_contract(build, patch, header, source, inventory)
        if bool(findings) != expect:
            verb = "reported nothing" if expect else f"reported {findings}"
            failures.append(f"  {label}: {verb}")
    if failures:
        sys.stderr.write("check_c6_integration.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return EXIT_FAIL
    cases = _selftest_cases()
    fires = sum(1 for case in cases if case[-1])
    print(
        f"check_c6_integration.py --selftest: OK "
        f"({len(cases)} cases: {fires} fire, {len(cases) - fires} stays quiet)."
    )
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Run the selftest or validate the real committed integration recipe."""
    if "--selftest" in argv[1:]:
        return selftest()
    required = (BUILD_SCRIPT, PATCH_FILE, SERVICE_HEADER, SERVICE_SOURCE)
    for path in required:
        if not path.is_file():
            sys.stderr.write(f"check_c6_integration.py: FATAL -- missing {path}\n")
            return EXIT_CONFIG
    findings = check_contract(*(path.read_text(encoding="utf-8") for path in required))
    if findings:
        sys.stderr.write(f"check_c6_integration.py: {len(findings)} C6 integration drift(s):\n")
        for finding in findings:
            sys.stderr.write(f"  {finding}\n")
        return EXIT_FAIL
    print("check_c6_integration.py: C6 staging/component/ABI contract agrees.")
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
