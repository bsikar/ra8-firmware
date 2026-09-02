# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction selftests for the Markdown reference validator."""

from __future__ import annotations

import sys
import tempfile
import time
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

import markdown_references as core
from git_environment import isolated_git_environment
from markdown_reference_policy import (
    BARE_FILE_SUFFIXES,
    LIBWEBP_ABSENCE_CLAUSE,
    PARSER_RUNTIME_LIMIT_SECONDS,
)

AnchorRef = core.AnchorRef
LinkRef = core.LinkRef
PathRef = core.PathRef
REPO_ROOT = core.REPO_ROOT
_bare_declaration_findings = core.bare_declaration_findings
_context_sha256 = core.context_sha256
_declared_bare_code_file = core.declared_bare_code_file
_declared_work_fixture = core.declared_work_fixture
_fail = core.fail
_git = core.git
_is_vendor = core.is_vendor
_path_reason = core.path_reason
_tracked_basename_index = core.tracked_basename_index
check_tree = core.check_tree
parse_document = core.parse_document

_SCRIPT_ROOT = "scripts"
_CITES_MARKER = "CITES" + "-OK"


def _init_fixture(root: Path) -> None:
    """Create a small git-owned Markdown fixture tree."""
    if _git(root, "init", "-q").returncode != 0:
        _fail("selftest git init failed")
    (root / "docs").mkdir()
    (root / "docs" / "planned").mkdir()
    (root / "docs" / "qualification" / "release").mkdir(parents=True)
    (root / "infra" / "live").mkdir(parents=True)
    (root / "scripts").mkdir()
    (root / "tests" / "core" / "src").mkdir(parents=True)
    (root / "tools").mkdir()
    (root / "tools" / "sample" / "src").mkdir(parents=True)
    (root / "libs" / "third_party" / "sample").mkdir(parents=True)
    (root / "libs" / "third_party" / "sample" / "src").mkdir()
    (root / "libs" / "third_party" / "sample" / "src" / "live.c").write_text(
        "/* fixture */\n", encoding="ascii"
    )
    (root / "libs" / "board_alpha" / "src" / "boot").mkdir(parents=True)
    (root / "libs" / "board_alpha" / "ld").mkdir()
    (root / "scripts" / "live.sh").write_text("#!/bin/sh\n", encoding="ascii")
    (root / "other").mkdir()
    (root / "other" / "live.sh").write_text("#!/bin/sh\n", encoding="ascii")
    (root / "tests" / "core" / "src" / "test_live.c").write_text(
        "/* fixture */\n", encoding="ascii"
    )
    (root / "scripts" / "README.md").write_text("# Script authority\n", encoding="ascii")
    (root / "tools" / "README.md").write_text(
        "# Tool authority\n\nA future\n"
        "`tools/<tool>/third_party/<component>` subtree is reserved for a dependency "
        "used exclusively by that tool.\nNo current dependency qualifies.\n",
        encoding="ascii",
    )
    (root / "docs" / "live.md").write_text("# Live heading\n", encoding="ascii")
    (root / "current.md").write_text("# Current sibling\n", encoding="ascii")
    (root / "docs" / "UPPER.MD").write_text("# Uppercase extension\n", encoding="ascii")
    (root / "docs" / "planned" / "README.md").write_text(
        "# Planned release layout\n", encoding="ascii"
    )
    (root / "docs" / "qualification" / "release" / "README.md").write_text(
        "# Planned qualification releases\n", encoding="ascii"
    )
    (root / "CMakeLists.txt").write_text("# component\n", encoding="ascii")
    (root / ".gitignore").write_text(
        "infra/live/private/\ninfra/removed/private/\n", encoding="ascii"
    )


def _run_fixture_cases(
    root: Path, cases: tuple[tuple[str, int, str], ...], failures: list[str]
) -> list[dict[str, object]]:
    """Run end-to-end document cases and return the last inventory."""
    inventory: list[dict[str, object]] = []
    for body, expected, label in cases:
        (root / "README.md").write_text(body, encoding="ascii")
        (root / "libs" / "third_party" / "sample" / "README.md").write_text(
            "[upstream dead](missing.md)\n", encoding="ascii"
        )
        if _git(root, "add", ".").returncode != 0:
            _fail("selftest git add failed")
        findings, _, inventory = check_tree(root, enforce_census=False, include_untracked=True)
        if len(findings) != expected:
            rendered = [finding.render() for finding in findings]
            failures.append(f"{label}: expected {expected}, got {rendered}")
    return inventory


def _check_vendor_scope_cases(root: Path, failures: list[str]) -> None:
    """Prove authored vendor indexes are checked while upstream prose is not."""
    authored_index = root / "libs" / "third_party" / "README.md"
    upstream_readme = root / "libs" / "third_party" / "sample" / "README.md"
    (root / "docs" / "live.md").write_text("# Live heading\n", encoding="ascii")
    authored_index.write_text("See docs/ABSENT.md.\n", encoding="ascii")
    upstream_readme.write_text("[upstream dead](missing.md)\n", encoding="ascii")
    if _git(root, "add", ".").returncode != 0:
        _fail("selftest vendor-scope git add failed")
    findings, _, _ = check_tree(root, enforce_census=False, include_untracked=True)
    if {finding.path for finding in findings} != {"libs/third_party/README.md"}:
        failures.append(f"vendor scope classification regressed: {findings}")

    authored_index.write_text(f"See {_SCRIPT_ROOT}/live.sh.\n", encoding="ascii")
    findings, _, _ = check_tree(root, enforce_census=False, include_untracked=True)
    if findings:
        failures.append(f"nested upstream Markdown was unexpectedly parsed: {findings}")


def _check_component_cases(root: Path, failures: list[str]) -> None:
    """Prove nearest-component and non-component relative paths fail closed."""
    (root / "README.md").write_text("# Root\n", encoding="ascii")
    (root / "component").mkdir()
    (root / "component" / "CMakeLists.txt").write_text("# component\n", encoding="ascii")
    (root / "component" / "README.md").write_text(
        "See ../dead.md and src/dead.c.\n", encoding="ascii"
    )
    (root / "component" / "docs").mkdir()
    (root / "component" / "docs" / "README.md").write_text(
        "Nested docs still bind src/dead.c to the component.\n", encoding="ascii"
    )
    if _git(root, "add", ".").returncode != 0:
        _fail("selftest component git add failed")
    findings, _, _ = check_tree(root, enforce_census=False, include_untracked=True)
    values = {finding.value for finding in findings}
    if values != {"../dead.md", "src/dead.c"}:
        failures.append(f"missing component-relative paths escaped: {findings}")

    (root / "component" / "README.md").write_text("# Component\n", encoding="ascii")
    (root / "component" / "docs" / "README.md").write_text("# Nested docs\n", encoding="ascii")
    (root / "docs" / "live.md").write_text(
        "A non-component src/ABSENT.c must not disappear.\n", encoding="ascii"
    )
    findings, _, _ = check_tree(root, enforce_census=False, include_untracked=True)
    if not any(finding.value == "src/ABSENT.c" for finding in findings):
        failures.append("non-component relative source path escaped")


def _check_tests_path_authority_cases(root: Path) -> None:
    """Build the root/component tests fixture the three case sets share."""
    component = root / "nested_component"
    (component / "docs").mkdir(parents=True)
    (component / "tests" / "local" / "src").mkdir(parents=True)
    (component / "CMakeLists.txt").write_text("# component\n", encoding="ascii")
    (component / "docs" / "README.md").write_text("# Nested\n", encoding="ascii")
    (component / "tests" / "local" / "src" / "test_local.c").write_text(
        "/* local */\n", encoding="ascii"
    )
    (root / "tests" / "shared" / "src").mkdir(parents=True)
    (root / "tests" / "shared" / "src" / "test_shared.c").write_text(
        "/* root */\n", encoding="ascii"
    )
    (root / "tests" / "root-owner").mkdir()


def _tests_path_reason(root: Path, failures: list[str]) -> Callable[[str], str | None]:
    """Return a bounded reason() closure over one component tests fixture."""
    source = "nested_component/docs/README.md"

    def reason(token: str) -> str | None:
        parsed = parse_document(f"`{token}`\n")
        if len(parsed.paths) != 1:
            failures.append(f"tests path fixture did not parse exactly: {token}")
            return "parse failure"
        return _path_reason(root, source, parsed.paths[0])

    return reason


def _check_tests_path_live_cases(root: Path, failures: list[str]) -> None:
    """Prove every live root/component tests path resolves to one authority."""
    reason = _tests_path_reason(root, failures)
    quiet = (
        "tests/core/src/test_live.c",
        "tests/shared/src/test_*.c",
        "tests/<category>/src/test_live.c",
        "tests/root-owner/build-cov/summary.json",
        "tests/local/src/test_local.c",
    )
    failures.extend(
        f"live root/component tests path was rejected: {token}"
        for token in quiet
        if reason(token) is not None
    )

    if reason("tests/local/src/absent.c") is None:
        failures.append("absent component-local tests path escaped")


def _check_tests_path_ambiguous_cases(root: Path, failures: list[str]) -> None:
    """Prove a path claimable by two authorities fails closed, not silently."""
    reason = _tests_path_reason(root, failures)
    component = root / "nested_component"
    (component / "tests" / "shared" / "src").mkdir(parents=True)
    (component / "tests" / "shared" / "src" / "test_shared.c").write_text(
        "/* local duplicate */\n", encoding="ascii"
    )
    (component / "tests" / "root-owner").mkdir()
    ambiguous = (
        "tests/shared/src/test_shared.c",
        "tests/shared/src/test_*.c",
        "tests/<category>/src/test_*.c",
        "tests/root-owner/build-cov/summary.json",
    )
    for token in ambiguous:
        detail = reason(token)
        if detail is None or "ambiguous" not in detail:
            failures.append(f"ambiguous tests path did not fail closed: {token}: {detail}")


def _check_tests_path_hostile_cases(root: Path, failures: list[str]) -> None:
    """Prove traversal and placeholder escapes out of a component are refused."""
    reason = _tests_path_reason(root, failures)
    (root / "outside.md").write_text("# Outside component\n", encoding="ascii")
    hostile = (
        "tests/local/../../../outside.md",
        "tests/${NAME}/../../outside.md",
        "tests/${lower}/src/test.c",
    )
    failures.extend(
        f"hostile tests path escaped: {token}" for token in hostile if reason(token) is None
    )


def _check_parser_cases(failures: list[str]) -> None:
    """Prove anchor and reference parsers against structural edge cases."""
    parsed = parse_document("# Same\n# Same\n[second](#same-1)\n")
    if (
        "same-1" not in parsed.anchors
        or parsed.anchor_collisions
        or parsed.links != (LinkRef(3, "#same-1"),)
    ):
        failures.append("duplicate heading anchors or inline-link parsing regressed")
    duplicate = parse_document('<a id="same"></a>\n# Same\n')
    if duplicate.anchor_collisions != (AnchorRef(2, "same"),):
        failures.append("explicit/generated duplicate anchors were not rejected")
    footnote = parse_document("[^note]: prose, not a link definition\n")
    if footnote.links:
        failures.append("a footnote was misclassified as a link definition")
    image = parse_document("![diagram][missing-image]\n")
    if image.missing_references != (LinkRef(1, "missing-image"),):
        failures.append("reference-style image without a definition escaped")
    shortcut = parse_document("See [docs/missing.md] for the plan.\n")
    if shortcut.missing_references != (LinkRef(1, "docs/missing.md"),):
        failures.append("path-looking shortcut reference without a definition escaped")
    fixture = PathRef(1, 0, "../escape", "A key that looks like a path")
    if not _declared_work_fixture("tools/work/tests/fixtures/bad_key.md", fixture):
        failures.append("exact workflow fixture declaration was rejected")
    wrong_fixture = PathRef(1, 0, "../other", "A key that looks like a path")
    if _declared_work_fixture("tools/work/tests/fixtures/bad_key.md", wrong_fixture):
        failures.append("a different workflow fixture escape was accepted")
    declared_document = parse_document(
        (REPO_ROOT / "docs/HIL_SUITE.md").read_text(encoding="utf-8")
    )
    declared_name = "dwf.h"
    declared_bare = next(ref for ref in declared_document.paths if ref.token == declared_name)
    if _declared_bare_code_file("docs/HIL_SUITE.md", declared_bare) is None:
        failures.append("exact declared external bare filename was rejected")
    if _declared_bare_code_file("docs/OTHER.md", declared_bare) is not None:
        failures.append("declared bare filename escaped from the wrong source")
    wrong_bare = PathRef(1, 0, "missing.h", "The package installs `missing.h`.")
    if _declared_bare_code_file("docs/HIL_SUITE.md", wrong_bare) is not None:
        failures.append("a different bare filename inherited an exact declaration")
    altered_bare = PathRef(1, 0, "dwf.h", "Use `dwf.h` from this repository now.")
    if _declared_bare_code_file("docs/HIL_SUITE.md", altered_bare) is not None:
        failures.append("declared bare filename escaped through unrelated current-use prose")


def _check_bare_parser_cases(failures: list[str]) -> None:
    """Prove the bounded bare-file parser is complete and linear-time."""
    bounded_names = tuple(f"sample.{suffix}" for suffix in BARE_FILE_SUFFIXES)
    bounded = parse_document(" ".join(f"`{name}`" for name in bounded_names) + "\n")
    if tuple(ref.token for ref in bounded.paths) != bounded_names:
        failures.append("bounded bare-file suffix census was not parsed exactly")
    if parse_document("`property.value`\n").paths:
        failures.append("arbitrary dotted tokens escaped the bounded suffix census")
    started = time.perf_counter()
    long_document = parse_document(f"`{'a' * 250_000}`\n")
    elapsed = time.perf_counter() - started
    if long_document.paths or elapsed > PARSER_RUNTIME_LIMIT_SECONDS:
        failures.append(f"bare-filename parser is non-linear ({elapsed:.3f}s)")


def _check_soup_cases(root: Path, failures: list[str]) -> None:
    """Prove upstream-relative paths bind to the declared local vendor root."""
    (root / "docs" / "SOUP").mkdir()
    source = "docs/SOUP/libwebp.md"
    (root / source).write_text(
        "# Sample\n\n- **Local path**: `libs/third_party/sample/`\n",
        encoding="ascii",
    )
    live = PathRef(3, 0, "src/live.c", "`src/live.c`")
    absent = PathRef(3, 0, "src/ABSENT.c", "`src/ABSENT.c`")
    declared_absent = PathRef(3, 0, "src/enc/*.c", LIBWEBP_ABSENCE_CLAUSE)
    malicious_absent = PathRef(
        3,
        0,
        "src/ABSENT.c",
        "`src/ABSENT.c` is required; `src/enc/*.c` are **not** vendored",
    )
    if _path_reason(root, source, live) is not None:
        failures.append("declared SOUP local source was rejected")
    if _path_reason(root, source, absent) is None:
        failures.append("absent SOUP local source escaped")
    if _path_reason(root, source, declared_absent) is not None:
        failures.append("exact declared SOUP absence was rejected")
    if _path_reason(root, source, malicious_absent) is None:
        failures.append("an unrelated SOUP path escaped through a negative claim")
    malicious_whitelisted = PathRef(
        3,
        0,
        "src/enc/*.c",
        "`src/enc/*.c` is required; documentation files are **not** vendored",
    )
    if _path_reason(root, source, malicious_whitelisted) is None:
        failures.append("a whitelisted SOUP token escaped through an unrelated negative clause")


def _check_planned_path_cases(root: Path, failures: list[str]) -> None:
    """Prove only named future namespaces can rely on policy authority."""
    release = PathRef(
        1,
        0,
        "docs/qualification/release/<tag>/conformance.md",
        "will be added under `docs/qualification/release/<tag>/conformance.md`",
    )
    if _path_reason(root, "docs/qualification/SQAP.md", release) is not None:
        failures.append("authorized qualification release namespace was rejected")
    if _path_reason(root, "README.md", release) is None:
        failures.append("qualification release namespace escaped from an unauthorized source")

    tool_private = PathRef(
        1,
        0,
        "tools/<tool>/third_party/<component>",
        "`tools/<tool>/third_party/<component>` subtree is reserved for a dependency "
        "used exclusively by that tool.",
    )
    if _path_reason(root, "tools/README.md", tool_private) is not None:
        failures.append("authorized future tool-private namespace was rejected")
    malicious_tool_private = PathRef(
        1,
        0,
        "tools/<tool>/third_party/<component>",
        "Use `tools/<tool>/third_party/<component>` now.",
    )
    if _path_reason(root, "tools/README.md", malicious_tool_private) is None:
        failures.append("tool-private namespace escaped without planned-language binding")
    misleading_future = PathRef(
        1,
        0,
        "tools/<tool>/third_party/<component>",
        "Future work is separate; use `tools/<tool>/third_party/<component>` now.",
    )
    if _path_reason(root, "tools/README.md", misleading_future) is None:
        failures.append("tool-private namespace escaped through unrelated future prose")
    misleading_current = PathRef(
        1,
        0,
        "tools/<tool>/third_party/<component>",
        "The current dependency may use `tools/<tool>/third_party/<component>` now.",
    )
    if _path_reason(root, "tools/README.md", misleading_current) is None:
        failures.append("tool-private namespace escaped through a current-dependency clause")
    if _path_reason(root, "README.md", tool_private) is None:
        failures.append("tool-private namespace escaped from a non-authority document")


def _check_bare_declaration_cases(root: Path, failures: list[str]) -> None:
    """Prove exact absent-file declarations cannot linger or broaden."""
    source = "docs/HIL_SUITE.md"
    (root / source).write_text("The package installs `dwf.h`.\n", encoding="ascii")
    if _git(root, "add", source).returncode != 0:
        _fail("selftest bare-declaration git add failed")
    parsed = {source: parse_document((root / source).read_text(encoding="ascii"))}
    key = (source, "dwf.h")
    exact = {key: "external package header"}
    exact_contexts = {key: (_context_sha256("The package installs `dwf.h`."),)}
    if _bare_declaration_findings(root, parsed, exact, exact_contexts):
        failures.append("live exact bare-file declaration was rejected")
    if not _bare_declaration_findings(
        root,
        parsed,
        {(source, "other.h"): "wrong token"},
        {(source, "other.h"): exact_contexts[key]},
    ):
        failures.append("wrong bare-file declaration token did not become stale")
    if not _bare_declaration_findings(
        root,
        parsed,
        {("docs/OTHER.md", "dwf.h"): "wrong"},
        {("docs/OTHER.md", "dwf.h"): exact_contexts[key]},
    ):
        failures.append("wrong bare-file declaration source did not become stale")
    if not _bare_declaration_findings(root, parsed, {key: ""}, exact_contexts):
        failures.append("empty bare-file declaration reason did not become stale")
    altered = {source: parse_document("Use `dwf.h` from this repository now.\n")}
    if not _bare_declaration_findings(root, altered, exact, exact_contexts):
        failures.append("changed semantic context left an absence declaration clean")

    (root / "external").mkdir()
    (root / "external" / "dwf.h").write_text("/* now tracked */\n", encoding="ascii")
    if _git(root, "add", "external/dwf.h").returncode != 0:
        _fail("selftest newly-present bare file git add failed")
    _tracked_basename_index.cache_clear()
    if not _bare_declaration_findings(root, parsed, exact, exact_contexts):
        failures.append("newly present bare filename left a stale declaration clean")


_FIXTURE_CASES = (
    ("[live](docs/live.md#live-heading)\n`tools/<name>/src`\n", 0, "live link"),
    ("[dead](docs/dead.md)\n", 1, "missing link"),
    ("[anchor](docs/live.md#dead-heading)\n", 1, "missing anchor"),
    ("[defined][target]\n[target]: docs/live.md\n", 0, "defined reference link"),
    ("[missing][target]\n", 1, "missing reference definition"),
    ("[encoded](docs/live.md#live%2Dheading)\n", 0, "percent-encoded anchor"),
    ("[site](/docs/live.md#live-heading)\n", 0, "live repository-root link"),
    ("[site](/docs/dead.md)\n", 1, "missing repository-root link"),
    ("`CMakeLists.txt`\n", 0, "live repository-root authority"),
    ("See current.md for the live sibling.\n", 0, "live lowercase sibling"),
    ("See missing.md for the absent sibling.\n", 1, "missing lowercase sibling"),
    ("`missing.md`\n", 1, "missing backticked lowercase sibling"),
    ("Use `live.sh`.\n", 0, "bare script basename exists and may be ambiguous"),
    ("Use `absent.py`.\n", 1, "missing bare Python basename"),
    ("Use `test_<name>.c`.\n", 0, "bare placeholder basename has a match"),
    ("Use `missing_<name>.c`.\n", 1, "bare placeholder basename has no match"),
    (
        "Ordinary prose mentions example.py without a code span.\n",
        0,
        "dotted prose is ignored",
    ),
    ("See FOO.md for the missing policy.\n", 1, "missing repository-root authority"),
    (f"`{_SCRIPT_ROOT}/live.sh`\n", 0, "live code path"),
    (f"`{_SCRIPT_ROOT}/dead.sh`\n", 1, "missing code path"),
    (
        f"`{_SCRIPT_ROOT}/live.sh:999999`\n",
        1,
        "rot-prone code-path line citation",
    ),
    (
        f"`{_SCRIPT_ROOT}/live.sh:999999` {_CITES_MARKER}: captured tool output\n",
        0,
        "reasoned line-citation transcript exception",
    ),
    (
        f"`{_SCRIPT_ROOT}/live.sh:999999` {_CITES_MARKER}:\n",
        1,
        "empty line-citation exception reason",
    ),
    (
        f"[source]({_SCRIPT_ROOT}/live.sh#L999999)\n",
        1,
        "beyond-EOF local source line link",
    ),
    (f"[source]({_SCRIPT_ROOT}/live.sh)\n", 0, "local source link without line anchor"),
    (
        f"[broken]({_SCRIPT_ROOT}/dead.sh\n",
        1,
        "unbalanced inline-link opener preserves a missing path",
    ),
    (
        f"[broken]({_SCRIPT_ROOT}/live.sh\n",
        0,
        "unbalanced inline-link opener preserves a live path",
    ),
    (
        f"[balanced]({_SCRIPT_ROOT}/live.sh) {_SCRIPT_ROOT}/dead.sh\n",
        1,
        "balanced inline-link masking preserves adjacent prose",
    ),
    (
        f"<code>{_SCRIPT_ROOT}/live.sh</code>\n",
        0,
        "live path in HTML code markup",
    ),
    (
        f"<code>{_SCRIPT_ROOT}/dead.sh</code>\n",
        1,
        "missing path in HTML code markup",
    ),
    (
        f"<code>{_SCRIPT_ROOT}/live.sh</code><code>{_SCRIPT_ROOT}/dead.sh</code>\n",
        1,
        "adjacent HTML wrappers preserve the second missing path",
    ),
    (
        f"<code>{_SCRIPT_ROOT}/live.sh</code><br><code>{_SCRIPT_ROOT}/dead.sh</code>\n",
        1,
        "HTML break preserves the second missing path",
    ),
    (
        f"<code>{_SCRIPT_ROOT}/live.sh</code><br><code>{_SCRIPT_ROOT}/live.sh</code>\n",
        0,
        "adjacent live HTML-wrapped paths stay quiet",
    ),
    (
        f"| executable | `{_SCRIPT_ROOT}/live.sh` |\n",
        0,
        "live path in a Markdown table",
    ),
    (
        f"| executable | `{_SCRIPT_ROOT}/dead.sh` |\n",
        1,
        "missing path in a Markdown table",
    ),
    (
        f'<a href="{_SCRIPT_ROOT}/live.sh">source</a>\n',
        0,
        "live HTML link target",
    ),
    (
        f'<a href="{_SCRIPT_ROOT}/dead.sh">source</a>\n',
        1,
        "missing HTML link target",
    ),
    (f"`{_SCRIPT_ROOT}/*.sh`\n", 0, "live glob has a current match"),
    (f"`{_SCRIPT_ROOT}/missing-*.sh`\n", 1, "empty glob fails closed"),
    (
        "`tests/<category>/src/test_*.c`\n",
        0,
        "mixed placeholder and glob has a current match",
    ),
    (
        "`tests/<category>/src/missing_*.c`\n",
        1,
        "empty mixed placeholder and glob fails closed",
    ),
    (
        "`libs/board_<board>/{src/boot,ld}/`\n",
        0,
        "placeholder plus multi-segment brace has a current match",
    ),
    (
        "`libs/board_alpha/{,src/boot}/`\n",
        0,
        "brace glob with an empty alternative has a current match",
    ),
    (
        "`libs/missing_<board>/{src/boot,ld}/`\n",
        1,
        "empty placeholder plus multi-segment brace fails closed",
    ),
    ("See docs/ABSENT.md.\n", 1, "uppercase path is not a placeholder"),
    ("```sh\nscripts/dead.sh\n```\n", 1, "missing fenced path"),
    ("`scripts/${NAME}.sh`\n", 0, "dynamic path with live owner"),
    ("`tools/missing/${NAME}.py`\n", 1, "dynamic path with missing nested owner"),
    (
        "`docs/qualification/release/<tag>/conformance.md`\n",
        1,
        "planned path needs an authorized source",
    ),
    ("`docs/<tag>/ABSENT.md`\n", 1, "placeholder without planned-root authority"),
    ("`libs/<component>/ABSENT.md`\n", 1, "populated root needs a real match"),
    ("`tools/<tool>/ABSENT.c`\n", 1, "tool template needs a real match"),
    ("docs/ABSENT.elf\n", 1, "global ignored extension is not authority"),
    ("libs/ABSENT.pyc\n", 1, "ignored bytecode extension is not authority"),
    (
        "tools/removed/build/output.elf\n",
        1,
        "removed build owner does not inherit a broad ignore",
    ),
    (
        "tests/build-fuzz/crashes/<target>/crash-<sha1>\n",
        0,
        "generated build namespace has a concrete owner",
    ),
    ("See docs/dead.md for details.\n", 1, "missing bare prose path"),
    ("State: infra/live/private/state.yml\n", 0, "owned ignored local state"),
    ("State: infra/removed/private/state.yml\n", 1, "stale ignored owner"),
)


def selftest() -> int:
    """Prove every detector in both directions, including end-to-end scope."""
    failures: list[str] = []
    with isolated_git_environment(), tempfile.TemporaryDirectory() as raw_tmp:
        root = Path(raw_tmp)
        _init_fixture(root)
        cases = _FIXTURE_CASES
        inventory = _run_fixture_cases(root, cases, failures)
        if not any(row["path"] == "docs/UPPER.MD" for row in inventory):
            failures.append("uppercase Markdown extension escaped the tracked inventory")
        _check_component_cases(root, failures)
        _check_tests_path_authority_cases(root)
        _check_tests_path_live_cases(root, failures)
        _check_tests_path_ambiguous_cases(root, failures)
        _check_tests_path_hostile_cases(root, failures)
        _check_soup_cases(root, failures)
        _check_planned_path_cases(root, failures)
        _check_vendor_scope_cases(root, failures)
        _check_bare_declaration_cases(root, failures)

    _check_parser_cases(failures)
    _check_bare_parser_cases(failures)

    if failures:
        for failure in failures:
            print(f"selftest: check_markdown_references.py FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"selftest: check_markdown_references.py OK ({len(cases) + 45} both-direction cases)")
    return 0
