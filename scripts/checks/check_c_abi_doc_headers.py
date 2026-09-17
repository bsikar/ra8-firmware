#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# check_c_abi_doc_headers.py -- keep the C ABI Doxygen reference honest.
#
# ADR-0005 splits the documentation into four generated slots; the C ABI
# reference at /api/c/ is the one Doxygen stays authoritative for while the
# libraries themselves move to Zig behind unchanged C headers. Doxyfile.capi
# deliberately carries no INPUT of its own: scripts/builders/docs_capi.sh feeds
# it the roots emitted by this script from config/c_abi_doc_headers.json, so the
# manifest is the single declaration of what the reference covers.
#
# That only holds if the manifest cannot drift from the tree. This script is the
# gate. It fails when
#
#   * a library ships public headers under libs/<lib>/inc/ and appears in
#     neither the documented nor the excluded list (the "new library silently
#     missing from the API reference" case this exists to prevent),
#   * a documented entry no longer has any public header (stale entry),
#   * an excluded entry matches nothing in the tree (stale exclusion),
#   * an exclusion carries no reason, or the same library is in both lists,
#   * an excluded_headers entry is not a tracked header, or its library is not
#     documented (excluding a header of an already-excluded library is a no-op
#     that reads like coverage),
#   * Doxyfile.capi drifts from the contract: a non-empty INPUT, an HTML_OUTPUT
#     other than the manifest's output slot, or a markdown mainpage (which would
#     make the C ABI slot a second copy of the whole site),
#   * either Doxyfile drops its half of the @scoperef contract, or the narrow
#     expansion grows a \\ref of its own (see check_doxyfile),
#   * a documented header references a docs/ page by its mangled Doxygen id
#     (@ref md_docs_...). That page is in the wide build's input set and not in
#     this one, so the reference is unresolvable here by construction; write
#     @scoperef{<id>,<display text>} instead, which both builds understand.
#
# Usage:
#   python3 scripts/checks/check_c_abi_doc_headers.py --check
#   python3 scripts/checks/check_c_abi_doc_headers.py --emit-inputs
#   python3 scripts/checks/check_c_abi_doc_headers.py --emit-excludes
#   python3 scripts/checks/check_c_abi_doc_headers.py --emit-headers
#   python3 scripts/checks/check_c_abi_doc_headers.py --selftest
#
# Exit status: 0 clean, 1 findings, 2 usage or environment error.

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = REPO_ROOT / "config" / "c_abi_doc_headers.json"
DOXYFILE_PATH = REPO_ROOT / "Doxyfile.capi"
WIDE_DOXYFILE_PATH = REPO_ROOT / "Doxyfile"
LIB_PREFIX = "libs/"

# One authored form, two expansions. The wide build resolves the reference;
# the narrow build renders the display text, because the target is outside its
# input set by design. Both halves are asserted so neither can quietly rot.
SCOPEREF_ALIAS = "scoperef{2}"
SCOPEREF_WIDE_EXPANSION = '@ref \\1 \\"\\2\\"'
SCOPEREF_NARROW_EXPANSION = "<tt>\\2</tt>"
# A docs/ page carries this prefix once Doxygen mangles its path into a page id.
MARKDOWN_PAGE_REF = "md_docs_"


def tracked_files(root: Path) -> list[str]:
    """Every path git tracks under libs/, as forward-slash repo-relative paths."""
    result = subprocess.run(
        ["git", "ls-files", "-z", "libs/"],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"git ls-files failed: {result.stderr.strip()}")
    return [p for p in result.stdout.split("\0") if p]


def public_headers(paths: list[str]) -> dict[str, list[str]]:
    """Map libs/<lib> -> its public headers, for every library that has some.

    A public header is a .h file under libs/<lib>/inc/ at any depth. Nothing
    else in a library is part of the C ABI surface: src/ is implementation and
    tests/ is not shipped.
    """
    found: dict[str, list[str]] = {}
    for path in paths:
        parts = path.split("/")
        if len(parts) < 4 or parts[0] != "libs" or parts[2] != "inc":
            continue
        if not path.endswith(".h"):
            continue
        found.setdefault(f"libs/{parts[1]}", []).append(path)
    return {lib: sorted(headers) for lib, headers in sorted(found.items())}


def library_dirs(paths: list[str]) -> set[str]:
    """Every libs/<lib> directory git tracks anything in."""
    dirs = set()
    for path in paths:
        parts = path.split("/")
        if len(parts) >= 3 and parts[0] == "libs":
            dirs.add(f"libs/{parts[1]}")
    return dirs


def evaluate(
    manifest: dict,
    paths: list[str],
    doxyfile_text: str | None,
    wide_doxyfile_text: str | None = None,
    header_texts: dict[str, str] | None = None,
) -> list[str]:
    """Return every finding as a human-readable line. Empty means clean."""
    findings: list[str] = []

    documented_entries = manifest.get("documented", [])
    excluded_entries = manifest.get("excluded", [])
    excluded_headers = manifest.get("excluded_headers", [])
    slot = manifest.get("output_slot", "api/c")

    documented = {}
    for entry in documented_entries:
        lib = entry.get("lib", "")
        if not lib:
            findings.append("documented entry with no 'lib' key")
            continue
        if lib in documented:
            findings.append(f"{lib}: listed twice under documented")
        documented[lib] = entry.get("include", f"{lib}/inc")

    excluded = {}
    for entry in excluded_entries:
        lib = entry.get("lib", "")
        if not lib:
            findings.append("excluded entry with no 'lib' key")
            continue
        if lib in excluded:
            findings.append(f"{lib}: listed twice under excluded")
        reason = (entry.get("reason") or "").strip()
        if not reason:
            findings.append(f"{lib}: excluded with no reason")
        excluded[lib] = reason

    for lib in sorted(set(documented) & set(excluded)):
        findings.append(f"{lib}: in both documented and excluded")

    headers = public_headers(paths)
    all_libs = library_dirs(paths)

    for lib in sorted(headers):
        if lib not in documented and lib not in excluded:
            count = len(headers[lib])
            findings.append(
                f"{lib}: {count} public header(s) under {lib}/inc/ but the library is in "
                "neither documented nor excluded in config/c_abi_doc_headers.json, so the "
                "C ABI reference would silently skip it"
            )

    for lib, include in sorted(documented.items()):
        if lib not in headers:
            findings.append(
                f"{lib}: documented but has no public header under {lib}/inc/ (stale entry)"
            )
        expected = f"{lib}/inc"
        if include != expected:
            findings.append(f"{lib}: documented include is '{include}', expected '{expected}'")

    for lib in sorted(excluded):
        if lib not in all_libs:
            findings.append(f"{lib}: excluded but no such directory is tracked (stale exclusion)")

    tracked_headers = {h for hs in headers.values() for h in hs}
    for entry in excluded_headers:
        path = entry.get("path", "")
        reason = (entry.get("reason") or "").strip()
        if not path:
            findings.append("excluded_headers entry with no 'path' key")
            continue
        if not reason:
            findings.append(f"{path}: excluded header with no reason")
        if path not in tracked_headers:
            findings.append(f"{path}: excluded header is not a tracked public header (stale)")
            continue
        lib = "/".join(path.split("/")[:2])
        if lib not in documented:
            findings.append(
                f"{path}: excluded header in {lib}, which is not documented -- the exclusion "
                "does nothing and reads like coverage"
            )

    if doxyfile_text is not None:
        findings.extend(check_doxyfile(doxyfile_text, slot))
    if wide_doxyfile_text is not None:
        findings.extend(check_wide_doxyfile(wide_doxyfile_text))
    if header_texts:
        findings.extend(check_header_references(header_texts))

    return findings


def check_doxyfile(text: str, slot: str) -> list[str]:
    """The narrow Doxyfile has to stay narrow and stay fed from the manifest."""
    findings: list[str] = []
    settings: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        settings[key.strip()] = value.strip()

    if settings.get("INPUT", "") != "":
        findings.append(
            "Doxyfile.capi: INPUT is set in the file; it must stay empty so "
            "scripts/builders/docs_capi.sh can supply it from the manifest"
        )
    html_output = settings.get("HTML_OUTPUT", "")
    if html_output != slot:
        findings.append(
            f"Doxyfile.capi: HTML_OUTPUT is '{html_output}', expected the manifest "
            f"output_slot '{slot}'"
        )
    if settings.get("USE_MDFILE_AS_MAINPAGE", ""):
        findings.append(
            "Doxyfile.capi: USE_MDFILE_AS_MAINPAGE is set; the C ABI slot documents "
            "headers, the Markdown hub owns the prose"
        )
    patterns = settings.get("FILE_PATTERNS", "").split()
    if sorted(patterns) != ["*.dox", "*.h"]:
        findings.append(
            f"Doxyfile.capi: FILE_PATTERNS is '{settings.get('FILE_PATTERNS', '')}', expected "
            "'*.h *.dox' (the .dox file carries the @defgroup tree the headers file under)"
        )

    expansion = alias_expansion(text, SCOPEREF_ALIAS)
    if expansion is None:
        findings.append(
            f"Doxyfile.capi: no ALIASES entry for {SCOPEREF_ALIAS}; a header that references "
            "a docs/ page or an internal header needs one authored form both builds accept"
        )
    elif "ref " in expansion or expansion.lstrip().startswith(("@ref", "\\ref")):
        findings.append(
            f"Doxyfile.capi: the {SCOPEREF_ALIAS} expansion is '{expansion}'; it must not "
            "resolve a reference here, because the target is outside this build's input set "
            "and Doxygen would warn on every use"
        )
    elif expansion != SCOPEREF_NARROW_EXPANSION:
        findings.append(
            f"Doxyfile.capi: the {SCOPEREF_ALIAS} expansion is '{expansion}', expected "
            f"'{SCOPEREF_NARROW_EXPANSION}'"
        )
    return findings


def alias_expansion(text: str, alias: str) -> str | None:
    """Return the expansion an `ALIASES` line gives `alias`, or None when absent."""
    for raw in text.splitlines():
        line = raw.strip()
        if not line.startswith("ALIASES"):
            continue
        _, _, value = line.partition("=")
        value = value.strip()
        if value.startswith('"') and value.endswith('"') and len(value) >= 2:
            value = value[1:-1]
        name, sep, expansion = value.partition("=")
        if sep and name.strip() == alias:
            return expansion
    return None


def check_wide_doxyfile(text: str) -> list[str]:
    """The wide build owns the other half: there the reference must resolve."""
    expansion = alias_expansion(text, SCOPEREF_ALIAS)
    if expansion is None:
        return [
            f"Doxyfile: no ALIASES entry for {SCOPEREF_ALIAS}; the wide build is where "
            "the reference resolves, so dropping it turns every use into plain text"
        ]
    if expansion != SCOPEREF_WIDE_EXPANSION:
        return [
            f"Doxyfile: the {SCOPEREF_ALIAS} expansion is '{expansion}', expected "
            f"'{SCOPEREF_WIDE_EXPANSION}'"
        ]
    return []


def check_header_references(header_texts: dict[str, str]) -> list[str]:
    """A documented header must not @ref a docs/ page this build cannot see."""
    findings: list[str] = []
    for path in sorted(header_texts):
        for number, line in enumerate(header_texts[path].splitlines(), start=1):
            for command in ("@ref ", "\\ref "):
                marker = command + MARKDOWN_PAGE_REF
                if marker in line:
                    findings.append(
                        f"{path}:{number}: references a docs/ page as "
                        f"'{command.strip()} {MARKDOWN_PAGE_REF}...'; that page is not in this "
                        f"build's input set, so use @scoperef{{<id>,<display text>}} instead"
                    )
    return findings


def load_manifest(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        raise RuntimeError(f"manifest not found: {path}")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"manifest is not valid JSON: {exc}")


NARROW_ALIAS_LINE = f'ALIASES += "{SCOPEREF_ALIAS}={SCOPEREF_NARROW_EXPANSION}"\n'
WIDE_ALIAS_LINE = f'ALIASES += "{SCOPEREF_ALIAS}={SCOPEREF_WIDE_EXPANSION}"\n'

def selftest() -> int:
    """Probe the rules against synthetic trees, so the gate itself is tested."""
    base_manifest = {
        "output_slot": "api/c",
        "documented": [{"lib": "libs/ra8_box", "include": "libs/ra8_box/inc"}],
        "excluded": [{"lib": "libs/third_party", "reason": "vendored SOUP"}],
        "excluded_headers": [],
    }
    base_paths = [
        "libs/ra8_box/inc/ra8_box.h",
        "libs/ra8_box/src/ra8_box.c",
        "libs/third_party/mbedtls/include/mbedtls/aes.h",
    ]
    good_doxyfile = (
        "INPUT =\nHTML_OUTPUT = api/c\nFILE_PATTERNS = *.h *.dox\n" + NARROW_ALIAS_LINE
    )
    good_wide_doxyfile = WIDE_ALIAS_LINE

    probes: list[tuple[str, dict, list[str], str, bool]] = [
        ("clean tree passes", base_manifest, base_paths, good_doxyfile, True),
        (
            "undeclared library with public headers fails",
            base_manifest,
            base_paths + ["libs/ra8_new/inc/ra8_new.h"],
            good_doxyfile,
            False,
        ),
        (
            "documented library with no public header fails",
            {**base_manifest,
             "documented": base_manifest["documented"] + [
                 {"lib": "libs/ra8_gone", "include": "libs/ra8_gone/inc"}]},
            base_paths,
            good_doxyfile,
            False,
        ),
        (
            "exclusion with no reason fails",
            {**base_manifest, "excluded": [{"lib": "libs/third_party", "reason": "  "}]},
            base_paths,
            good_doxyfile,
            False,
        ),
        (
            "stale exclusion fails",
            {**base_manifest,
             "excluded": base_manifest["excluded"] + [
                 {"lib": "libs/ra8_deleted", "reason": "went away"}]},
            base_paths,
            good_doxyfile,
            False,
        ),
        (
            "src/ and tests/ headers are not public headers",
            base_manifest,
            base_paths + ["libs/ra8_box/src/internal.h", "libs/ra8_box/tests/helper.h"],
            good_doxyfile,
            True,
        ),
        (
            "nested public header still counts",
            base_manifest,
            base_paths + ["libs/ra8_box/inc/sub/deep.h"],
            good_doxyfile,
            True,
        ),
        (
            "excluded header of an undocumented library fails",
            {**base_manifest,
             "excluded_headers": [
                 {"path": "libs/third_party/mbedtls/include/mbedtls/aes.h", "reason": "soup"}]},
            base_paths,
            good_doxyfile,
            False,
        ),
        (
            "Doxyfile with a hard-coded INPUT fails",
            base_manifest,
            base_paths,
            "INPUT = libs\nHTML_OUTPUT = api/c\nFILE_PATTERNS = *.h *.dox\n" + NARROW_ALIAS_LINE,
            False,
        ),
        (
            "Doxyfile writing outside the api/c slot fails",
            base_manifest,
            base_paths,
            "INPUT =\nHTML_OUTPUT = html\nFILE_PATTERNS = *.h *.dox\n" + NARROW_ALIAS_LINE,
            False,
        ),
        (
            "Doxyfile with a markdown mainpage fails",
            base_manifest,
            base_paths,
            "INPUT =\nHTML_OUTPUT = api/c\nFILE_PATTERNS = *.h *.dox\n"
            "USE_MDFILE_AS_MAINPAGE = README.md\n" + NARROW_ALIAS_LINE,
            False,
        ),
    ]

    header = "/** @file x.h */\n"
    # (name, narrow Doxyfile, wide Doxyfile, header bodies, expect_clean)
    ref_probes: list[tuple[str, str, str, dict[str, str], bool]] = [
        ("both alias halves present passes",
         good_doxyfile, good_wide_doxyfile, {"libs/ra8_box/inc/ra8_box.h": header}, True),
        ("narrow Doxyfile without the alias fails",
         "INPUT =\nHTML_OUTPUT = api/c\nFILE_PATTERNS = *.h *.dox\n",
         good_wide_doxyfile, {}, False),
        ("narrow alias that resolves a reference fails",
         "INPUT =\nHTML_OUTPUT = api/c\nFILE_PATTERNS = *.h *.dox\n"
         'ALIASES += "scoperef{2}=@ref \\1"\n',
         good_wide_doxyfile, {}, False),
        ("narrow alias with an unexpected expansion fails",
         "INPUT =\nHTML_OUTPUT = api/c\nFILE_PATTERNS = *.h *.dox\n"
         'ALIASES += "scoperef{2}=<b>\\2</b>"\n',
         good_wide_doxyfile, {}, False),
        ("wide Doxyfile without the alias fails",
         good_doxyfile, "PROJECT_NAME = x\n", {}, False),
        ("wide alias that does not resolve fails",
         good_doxyfile, 'ALIASES += "scoperef{2}=<tt>\\2</tt>"\n', {}, False),
        ("header referencing a docs/ page by mangled id fails",
         good_doxyfile, good_wide_doxyfile,
         {"libs/ra8_dfu/inc/ra8_rot.h": " * @see @ref md_docs_2formats_2ROT1 -- spec\n"}, False),
        ("the same reference written as @scoperef passes",
         good_doxyfile, good_wide_doxyfile,
         {"libs/ra8_dfu/inc/ra8_rot.h":
          " * @see @scoperef{md_docs_2formats_2ROT1,docs/formats/ROT1.md} -- spec\n"}, True),
        ("backslash \\ref form is caught too",
         good_doxyfile, good_wide_doxyfile,
         {"libs/ra8_dfu/inc/ra8_rot.h": " * \\ref md_docs_2formats_2NSR1\n"}, False),
        ("a reference to a header id is untouched by the page rule",
         good_doxyfile, good_wide_doxyfile,
         {"libs/ra8_box/inc/ra8_box.h": " * @ref ra8_box_open\n"}, True),
    ]

    failures = 0
    for name, manifest, paths, doxyfile, expect_clean in probes:
        findings = evaluate(manifest, paths, doxyfile, WIDE_ALIAS_LINE)
        clean = not findings
        if clean != expect_clean:
            failures += 1
            print(f"  FAIL {name}: expected {'clean' if expect_clean else 'findings'}, got {findings}")
        else:
            print(f"  ok   {name}")

    for name, narrow, wide, header_texts, expect_clean in ref_probes:
        findings = evaluate(base_manifest, base_paths, narrow, wide, header_texts)
        clean = not findings
        if clean != expect_clean:
            failures += 1
            print(f"  FAIL {name}: expected {'clean' if expect_clean else 'findings'}, got {findings}")
        else:
            print(f"  ok   {name}")

    total = len(probes) + len(ref_probes)
    print(f"selftest: {total - failures}/{total} probes passed")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="fail on manifest/tree drift")
    group.add_argument("--emit-inputs", action="store_true",
                       help="print the documented include roots, one per line")
    group.add_argument("--emit-excludes", action="store_true",
                       help="print the excluded header paths, one per line")
    group.add_argument("--emit-headers", action="store_true",
                       help="print the effective documented header set, one per line")
    group.add_argument("--selftest", action="store_true", help="probe the rules and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    try:
        manifest = load_manifest(MANIFEST_PATH)
        paths = tracked_files(REPO_ROOT)
    except RuntimeError as exc:
        print(f"check_c_abi_doc_headers: {exc}", file=sys.stderr)
        return 2

    if args.emit_inputs:
        for entry in manifest.get("documented", []):
            print(entry.get("include", f"{entry.get('lib', '')}/inc"))
        return 0

    if args.emit_excludes:
        for entry in manifest.get("excluded_headers", []):
            print(entry.get("path", ""))
        return 0

    if args.emit_headers:
        headers = public_headers(paths)
        skipped = {e.get("path", "") for e in manifest.get("excluded_headers", [])}
        documented = {e.get("lib", "") for e in manifest.get("documented", [])}
        for lib in sorted(documented):
            for header in headers.get(lib, []):
                if header not in skipped:
                    print(header)
        return 0

    doxyfile_text = DOXYFILE_PATH.read_text() if DOXYFILE_PATH.exists() else None
    if doxyfile_text is None:
        print(f"check_c_abi_doc_headers: {DOXYFILE_PATH} not found", file=sys.stderr)
        return 2
    wide_text = WIDE_DOXYFILE_PATH.read_text() if WIDE_DOXYFILE_PATH.exists() else None
    if wide_text is None:
        print(f"check_c_abi_doc_headers: {WIDE_DOXYFILE_PATH} not found", file=sys.stderr)
        return 2

    headers = public_headers(paths)
    skipped = {e.get("path", "") for e in manifest.get("excluded_headers", [])}
    documented_libs = {e.get("lib", "") for e in manifest.get("documented", [])}
    header_texts: dict[str, str] = {}
    for lib in sorted(documented_libs):
        for header in headers.get(lib, []):
            if header in skipped:
                continue
            try:
                header_texts[header] = (REPO_ROOT / header).read_text(errors="replace")
            except OSError:
                continue

    findings = evaluate(manifest, paths, doxyfile_text, wide_text, header_texts)
    if findings:
        print("check_c_abi_doc_headers: FINDINGS")
        for finding in findings:
            print(f"  - {finding}")
        return 1

    documented = [e.get("lib", "") for e in manifest.get("documented", [])]
    total = sum(len(headers.get(lib, [])) for lib in documented)
    print(
        f"check_c_abi_doc_headers: OK -- {len(documented)} documented librar(ies), "
        f"{total} public header(s), {len(manifest.get('excluded', []))} excluded."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
