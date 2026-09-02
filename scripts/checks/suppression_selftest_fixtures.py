# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Immutable data fixtures for the suppression scanner selftest."""

from __future__ import annotations

import hashlib

from suppression_tool_selftest import TOOL_CONTROL_FIXTURES

EXPECTED_FIXTURE_SHA256 = "00145bcb1482f63d3811f64bd4d95fd0afdacd684e6127515632d4ebd23c2f54"
EXPECTED_FIXTURE_INVENTORY_SHA256 = (
    "d3fea00610180e9fff0dd44cb8d3a8b6f6bcc1e0e97c374ca4cf0e8576c2f26a"
)

CORE_FIXTURES = {
    "compiler_controls.c": r"""const char *pragma_text =
  "#pragma clang diagnostic " "ignored \\"-Wunused-variable\\"";
const char *attribute_text = "[[maybe_unused]] __attribute__((unused))";
const char *raw_text = R"tag(RA8_NASA_RULE_3_OK("not code"))tag";
#pragma clang diagnostic push
/* Suppression rationale: fixture variable exists only to trigger the warning contract. */
#pragma clang diagnostic ignored "-Wunused-variable"
static void clang_region(void) { int fixture_unused; }
#pragma clang diagnostic pop
#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wunused-variable"
#pragma GCC diagnostic pop
_Pragma("clang diagnostic push")
/* Suppression rationale: fixture operator variable exists only for warning coverage. */
_Pragma("clang diagnostic ignored \"-Wunused-variable\"")
static void clang_operator_region(void) { int fixture_unused; }
_Pragma("clang diagnostic pop")
_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic warning \"-Wunused-variable\"")
_Pragma("GCC diagnostic pop")
[[deprecated("fixture"), maybe_unused]] static int maybe_unused_value;
[[gnu::unused]] static int scoped_gnu_unused_value;
__attribute((unused)) static int short_gnu_unused_value;
__attribute__((unused)) static int gnu_unused_value;
__attribute__((no_sanitize("undefined"))) int unsanitized_identity(int value) { return value; }
__attribute__((__no_sanitize__("undefined")))
int underscored_unsanitized(int value) { return value; }
__attribute__((__no_sanitize_address__)) int address_unsanitized(int value) { return value; }
[[clang::no_sanitize("undefined")]] int scoped_unsanitized(int value) { return value; }
RA8_NASA_RULE_3_OK("fixture host allocator contract")
static void waived_allocator(void) {}
void *allowed_alloc(void) { return malloc(4); /* alloc-allow: fixture bounded allocator */ }
""",
    "malformed_controls.c": r"""#pragma clang diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic sideways "-Wunused-variable"
#pragma GCC diagnostic push
#pragma clang diagnostic pop
_Pragma("clang diagnostic ignored -Wunused-variable")
[[maybe_unused("reason")]] static int malformed_maybe_args;
[[maybe_unused bogus]] static int malformed_maybe_tail;
[[clang::no_sanitize(foo)]] int malformed_scoped_sanitizer(int value) { return value; }
__attribute__((unused extra)) static int malformed_gnu;
__attribute__((unused("reason"))) static int malformed_unused;
__attribute__((no_sanitize)) int malformed_empty_sanitizer(int value) { return value; }
__attribute__((no_sanitize(foo))) int malformed_sanitizer(int value) { return value; }
RA8_NASA_RULE_3_OK
static void bare_nasa(void) {}
static int orphan_control; /* alloc-allow: no allocator on this line */
static int bare_control; /* alloc-allow */
""",
    "alloc_consumer.c": r"""void *malloc(unsigned long size);
void *reasoned(void) { return malloc(4); /* alloc-allow: fixture bounded allocation */ }
void *string_only(void) { const char *text = "alloc-allow: not a comment"; return malloc(4); }
void orphan_control(void) { /* alloc-allow: no allocation is present */ }
""",
    "alloc_consumer.cpp": r"""void active_new(void) {
  int *value = new int; /* alloc-allow: fixture C++ object allocation */
  delete value; /* alloc-allow: fixture C++ object release */
}
void orphan_cpp_control(void) { /* alloc-allow: no allocation is present */ }
""",
    ".clang-tidy": """---
# Disabled checks:
#  -readability-fixture
#      Synthetic check proves source-located global exclusions.
Checks: >
  readability-*,
  -readability-fixture
WarningsAsErrors: '*'
""",
    "controls.py": """import pytest
import unittest
import pytest as pt
from unittest import expectedFailure, skipIf, skipUnless

@pytest.mark.skip(reason="fixture platform is absent")
@pytest.mark.skipif(True, reason="fixture condition")
@pytest.mark.xfail(reason="fixture defect", strict=True)
def test_controlled():
    pytest.skip("fixture runtime precondition")
    pytest.xfail("fixture runtime limitation")

@unittest.skip("fixture feature unavailable")
def test_unittest_controlled():
    raise unittest.SkipTest("fixture setup unavailable")

pytestmark = pt.mark.skip(reason="fixture module platform is absent")

@skipIf(True, "fixture skipIf condition")
def test_skip_if():
    pass

@skipUnless(False, "fixture skipUnless condition")
def test_skip_unless():
    pass

@expectedFailure
def test_expected_failure():
    pass

class FixtureCase(unittest.TestCase):
    def test_method_skip(self):
        self.skipTest("fixture method setup unavailable")

@pt.mark.xfail(reason="fixture permissive expected failure")
def test_permissive_xfail():
    pass

text = "pytest.mark.skip(reason='string only')"
""",
    "sample.py": """value = "# noqa: F401 -- string only"
controls = "# pragma: no cover; # pylint: disable=unused-import; # fmt: off"
import os  # noqa: F401 -- imported for fixture coverage
result = value  # type: ignore[assignment] -- fixture type mismatch
print(result)  # noqa: S603,S607 -- fixture normalized multi-rule waiver
# pragma: no cover -- fixture impossible branch
# pylint: disable=unused-import -- fixture analyzer region
# pylint: enable=unused-import
# mypy: ignore-errors -- fixture generated API surface
other = result  # pyright: ignore[reportAssignmentType] -- fixture mismatch
print(other)  # nosec B101 -- fixture security false positive
print(other)  # bandit: skip=B602 -- fixture security false positive
# ruff: noqa: E402 -- fixture file-level import ordering
# Suppression rationale: hand-aligned fixture table
# fmt: off
table = [1,  2]
# fmt: on
""",
    "bare_python_controls.py": """# fmt: off
value = 1
# pyright: ignore
# pylint: skip-file
""",
    "sample.c": """const char *text = "NOLINT(readability-magic-numbers)";
const char *raw = R"tag(// NOLINT(readability-magic-numbers))tag";
const char *spliced = "text \\
// NOLINT(readability-magic-numbers)";
int NOLINT(raw_token);
int prefixNOLINT = 0;
// NOLINTNEXTLINE(readability-magic-numbers) -- fixture literal
int value = 7;
/* GCOVR_EXCL_LINE -- fixture hardware path */
/* cppcheck-suppress unreadVariable -- fixture assembly output */
/* NOLINTBEGIN(readability-identifier-naming) -- fixture generated names */
int Fixture_Name;
/* NOLINTEND(readability-identifier-naming) */
// NOLINT(whitespace/line_length) -- fixture vendored cpplint syntax
// spliced line comment \\
NOLINT(readability-redundant-string-init) -- fixture spliced directive
// spliced line comment \\
int NOLINT(raw_spliced_token);
/* GCOVR_EXCL_START -- fixture unreachable region */
int unreachable_value;
/* GCOVR_EXCL_STOP */
/* cppcheck-suppress-begin unusedStructMember -- fixture generated declarations */
int unused_value;
/* cppcheck-suppress-end unusedStructMember */
unsigned long first_set_bit;  // NOLINT (runtime/int)
int legacy_size;  // NOLINT[readability/fn_size]
/* GCOVR_EXCL_BR_START -- fixture branch-only region */
if (unreachable_value) {
  unreachable_value = 2;
}
/* GCOVR_EXCL_BR_STOP */
/* LCOV_EXCL_START -- fixture LCOV region */
unreachable_value = 3;
/* LCOV_EXCL_STOP */
/* mcdc-deactivated: fixture operands are coupled by the fixture contract. */
int fixture_decision = fixture_left && fixture_right;
""",
    "malformed_mcdc.c": """/**
 * mcdc-deactivated: documentation prose is not an active marker.
 */
/* mcdc-deactivated because the colon is missing */
/* mcdc-deactivated: first marker for one decision */
/* mcdc-deactivated: duplicate marker for the same decision */
int duplicate_decision = fixture_left || fixture_right;
RA8_MCDC_DEACTIVATED("fixture function invariant")
RA8_MCDC_OK("unknown alias")
RA8_MCDC_DEACTIVATED(reason)
RA8_MCDC_DEACTIVATED(not_literal) RA8_MCDC_DEACTIVATED("valid after invalid")
RA8_MCDC_DEACTIVATED("valid before invalid") RA8_MCDC_DEACTIVATED(still_not_literal)
RA8_MCDC_DEACTIVATED("first distinct reason") RA8_MCDC_DEACTIVATED("second distinct reason")
/* mcdc-deactivated: no following compound decision */
int unrelated_value;
""",
    "native_test.cpp": """void skip_gtest(void) {
  GTEST_SKIP() << "fixture platform is absent";
}
void skip_unity_message(void) {
  TEST_IGNORE_MESSAGE("fixture peripheral is absent");
}
void skip_unity_bare(void) {
  TEST_IGNORE();
}
const char* skip_text = "GTEST_SKIP()";
void malformed_skip(void) { GTEST_SKIP(7); }
void malformed_unity(void) { TEST_IGNORE_MESSAGE(reason); }
""",
    "sample.sh": """#!/usr/bin/env bash
echo '# shellcheck disable=SC2086'
# shellcheck disable=SC2086  # fixture intentionally expands words
true;# shellcheck disable=SC2034  # fixture operator starts a comment word
value=${value#prefix}; cc -Wno-shadow -c sample.c
word#literal cc -Wno-conversion -c sample.c
cc -Wno-unused-parameter -c sample.c
cmake -S . -B build \\
  -Wno-dev
"$CMAKE" -S . -B variable-build \\
  -Wno-dev
FLAGS=(-DUNIT_TEST -w)
test -w sample.c
fold -w 80 sample.c
echo "-Wno-error is documentation"
cmake -E echo -Wno-dev
/usr/bin/cmake -S . -B absolute-build -Wno-dev
/usr/bin/clang++ -Wno-sign-conversion -c sample.cpp
echo "/usr/bin/clang++ -Wno-error is quoted documentation"
"/usr/bin/cmake" -S . -B quoted-build -Wno-dev
'/usr/bin/clang++' -Wno-float-conversion -c sample.cpp
cmake -E env /usr/bin/clang++ -Wno-padded -c sample.cpp
cmake --build build --target -Wno-dev
"/opt/tool chain/bin/clang++" -Wno-error -c spaced.cpp
"/opt/cmake tools/bin/cmake" -S . -B spaced-build -Wno-dev
/opt/tool\\ chain/bin/clang++ -Wno-shadow -c escaped.cpp
"/opt/tool \\\"quoted\\\" path/bin/clang++" -Wno-padded -c quoted.cpp
"/opt/tool\\\\path/bin/clang++" -Wno-switch -c backslash.cpp
'/opt/single quoted/bin/clang' -Wno-cast-align -c single.c
echo '"/opt/tool chain/bin/clang++" -Wno-unused-variable'
printf '%s\\n' "/opt/cmake tools/bin/cmake" -Wno-dev
# "/opt/tool chain/bin/clang++" -Wno-comment
"/opt/cmake tools/bin/cmake" -S . -B deprecated -Wno-deprecated
"/opt/cmake tools/bin/cmake" -S . -B dev-errors -Wno-error=dev
"/opt/cmake tools/bin/cmake" -S . -B deprecated-errors -Wno-error=deprecated
"/opt/tool chain/bin/clang++" -Wno-deprecated -c compiler-owned.cpp
"/opt/cmake tools/bin/cmake" -S . -B flags -DCMAKE_C_FLAGS=-Wno-deprecated
echo '"/opt/cmake tools/bin/cmake" -Wno-deprecated'
printf '%s\\n' "/opt/cmake tools/bin/cmake" -Wno-error=dev
# "/opt/cmake tools/bin/cmake" -Wno-error=deprecated
cat <<'PAYLOAD'
# shellcheck disable=SC2016
cc -Wno-error -c hidden.c
PAYLOAD
printf '%s\n' '<<PAYLOAD'
read -r payload <<<$value
mask=$((1 << 31))
# shellcheck disable=SC2015  # active after quoted heredoc lookalike
cat <<\\ESCAPED
# shellcheck disable=SC2014
ESCAPED
cat <<'END-MARK'
# shellcheck disable=SC2013
END-MARK
single='multiline text
# shellcheck disable=SC2012
single end'
double="multiline text
# shellcheck disable=SC2011
double end"
# shellcheck disable=SC2010  # active after multiline quotes
# shellcheck enable=SC2010
# shellcheck source=/dev/null
# shellcheck source-path=SCRIPTDIR
# shellcheck shell=bash
# shellcheck external-sources=true
SHELLCHECK_OPTS='--exclude=SC2001,SC2002'  # fixture central exclusions need stable rows
printf '%s\n' 'SHELLCHECK_OPTS=--exclude=SC2999'
probe || true  # fixture cleanup status is intentionally ignored
captured="$(probe || true)"
probe ||
  true
probe || \\
  true
printf '%s\n' 'probe || true'
# probe || true
gcovr --exclude-unreachable-branches  # fixture strips compiler-generated dead edges
coverage_text='--exclude-unreachable-branches'
gcovr --gcov-ignore-parse-errors=suspicious_hits.warn
cat <<'STATUS-DATA'
probe || true
STATUS-DATA
cat <<MISSING
# shellcheck disable=SC2009
""",
    "config/gcovr.cfg": """gcov-ignore-parse-errors = negative_hits.warn # fixture diagnostic
exclude-unreachable-branches = yes # fixture removes compiler-only edges
exclude-throw-branches = false # fixture keeps source branches
gcov-ignore-parse-errors = all # duplicate authority must fail
""",
    "invalid/gcovr.cfg": """exclude-throw-branches = sideways # invalid boolean must fail
""",
    ".shellcheckrc": """# fixture editor parity
exclude=SC2003,SC2004  # fixture central exclusions need stable rows
""",
    "just/sample.just": """set shell := ["bash", "-euo", "pipefail", "-c"]
probe:
    probe || true
captured:
    value="$(probe || true)"
""",
    "CMakeLists.txt": """target_compile_options(sample PRIVATE -w -Wno-unused-variable)
target_compile_options(sample PRIVATE -Wno-dev)
execute_process(COMMAND ${CMAKE_COMMAND} -S . -B build -DOPTION=ON -Wno-dev)
message(STATUS "-Wno-dev is data")
set(PAYLOAD "-Wno-dev")
execute_process(COMMAND ${CMAKE_COMMAND} -S . -B deprecated -Wno-deprecated)
set(CMAKE_C_FLAGS "-Wno-deprecated")
message(STATUS "/usr/bin/cmake -S . -B hidden -Wno-deprecated")
set(PROJECT_WNO
    -Wno-conversion
)
# Fixture negative tests intentionally return non-zero.
set_tests_properties(fails PROPERTIES WILL_FAIL TRUE)
# Fixture is registered but cannot execute on this platform.
set_property(TEST platform PROPERTY DISABLED ON)
# Fixture maps one intentional application status to a skipped result.
set_tests_properties(status PROPERTIES SKIP_RETURN_CODE 77)
get_property(not_a_control TEST platform PROPERTY DISABLED)
message("set_tests_properties(fake PROPERTIES WILL_FAIL TRUE)")
set(payload [=[
# NOLINT(readability-magic-numbers)
-Wno-error
]=])
#[=[
# shellcheck disable=SC2016
-Wno-error
]=]
# -Wno-error in a comment is not active
""",
    "Makefile": """WARNING_CFLAGS ?= -Wno-format-nonliteral
ECHO_TEXT = -Wno-error
""",
    "sample.yml": """payload: |
  # noqa: command-instead-of-shell
  cc -Wno-error -c hidden.c
run: |
  cc -Wno-error -c visible.c
  cmake -S . -B build \\
    -Wno-dev

shell: |-
  clang++ -Wno-dev -c visible.cpp
steps:
  - run: >-
      /usr/bin/cmake -S . -B folded
      -Wno-dev
  - run: >
      /usr/bin/clang++
      -Wno-conversion -c folded.cpp
task: value  # noqa: command-instead-of-shell
content: |
  #!/bin/bash
  probe || true  # fixture generated wrapper tolerates an absent probe
run: |
  probe || true
  probe ||
    true
  probe ||
    # retained rationale
    true
ansible.builtin.shell: |
  probe || true
items:
  - |2-
      # shellcheck disable=SC2016
      -Wno-error
pkg.cflags: -Wno-dev -Wno-array-bounds -w
commented_steps:
  - run: >-
      cmake -S . -B commented
      # The folded shell comment owns the rest of this paragraph.
      -Wno-dev
""",
    ".github/workflows/control.yml": """---
name: control fixture
jobs:
  fixture:
    continue-on-error: false
    steps:
      - name: Optional diagnostic
        continue-on-error: true
        run: false
      - name: Optional artefact
        uses: actions/upload-artifact@v4
        with:
          if-no-files-found: warn
      - name: Expected-empty artefact
        uses: actions/upload-artifact@v4
        with:
          if-no-files-found: ignore
""",
    "infra/ansible/roles/fixture/tasks/main.yml": """---
- name: Read-only fixture probe
  ansible.builtin.command: {cmd: probe}
  changed_when: false
  failed_when: false
- name: Protect fixture credential output
  ansible.builtin.command: {cmd: credential}
  no_log: true
- name: Continue fixture cleanup after an absent resource
  ansible.builtin.command: {cmd: cleanup}
  ignore_errors: true
- name: Keep directive-looking block payload inert
  ansible.builtin.debug:
    msg: |
      failed_when: false
""",
    "windows.yml": """steps:
  - shell: pwsh
    run: |
      if (($value -eq 1) -and ($other -eq 2)) {
        Write-Output "PowerShell is not parsed as malformed Bash"
      }
""",
    "policy.md": """A prose NOLINT mention is not a directive.
<!-- AI-OK: fixture policy quotation -->
<!-- MAGIC-OK -->
<!--
AI-OK: multiline fixture policy quotation
-->
<!-- FUTURE-OK: uncataloged policy marker -->
```html
<!-- AI-OK: fenced example only -->
```
`<!-- AI-OK: inline example only -->`
<!-- unterminated fixture marker
""",
    "malformed.py": """value = 1  # noqa: F401 E402
# pragma: no cover because fixture
# pylint: disable
# mypy: disable-error-code
# pyright: nonsense
# bandit: skip=
# fmt: sideways
# ruff: noqa: E402 F401
""",
    "unmatched.c": """// NOLINTEND(readability-magic-numbers)
// NOLINT(foo,,bar)
/* GCOVR_EXCL_BR_START -- mismatched branch region */
/* GCOVR_EXCL_STOP */
/* LCOV_EXCL_START -- mismatched tool region */
/* GCOVR_EXCL_STOP */
// clang-format off sideways
""",
    "unterminated.c": "// trailing line splice \\\\\n",
    "malformed.sh": """#!/usr/bin/env bash
cat <<
# shellcheck disable=SC2008  # remains visible after malformed operator
""",
    "unterminated.sh": """#!/usr/bin/env bash
value='unterminated
""",
    "port/threadx/vendor.c": "// NOLINT(readability-magic-numbers) -- vendor C\n",
    "port/threadx/CMakeLists.txt": "# NOLINT(readability-magic-numbers) -- owned glue\n",
    "libs/ra8_c6link/src/ra8_media_download.pb-c.c": (
        "// NOLINT(readability-magic-numbers) -- generated fixture\n"
    ),
    ".cppcheck-suppressions": """# ----------------------------------------
# Fixture grouped rules.
# Justification: paired divider rationale.
# ----------------------------------------
unusedFunction:sample.c
unusedFunction:sample.c
# Justification: adjacent local rationale A.
unreadVariable:sample.c
# Justification: adjacent local rationale B.
unknownMacro:sample.c
bogusRule:sample.c
""",
    "pyproject.toml": """[tool.ruff]
extend-exclude = [
  "generated", # generated fixture output
]

[tool.ruff.lint]
ignore = [
  "COM812", # formatter owns trailing commas
]

[tool.ruff.lint.per-file-ignores]
"fixture.py" = ["SLF001"] # third-party fixture internals lack a public seam
""",
    "apps/fixture/src/format_controls.c": r"""
const char *text = "clang-format off; @cond hidden; IWYU pragma: keep";
// Suppression rationale: fixture columns encode a protocol table reviewed by row.
// clang-format off
int columns[] = {1,  2};
// clang-format on
/** Suppression rationale: the fixture hides a private implementation-only contract. */
/** @cond INTERNAL */
int internal_contract;
/** @endcond */
// Suppression rationale: the fixture include provides a required macro side effect.
#include "fixture_keep.h" // IWYU pragma: keep
// Suppression rationale: the fixture include intentionally re-exports its API.
#include "fixture_export_one.h" // IWYU pragma: export
// Suppression rationale: these fixture includes form one exported facade.
// IWYU pragma: begin_exports
#include "fixture_export.h"
// IWYU pragma: end_exports
// Suppression rationale: these includes are required by generated registration.
// IWYU pragma: begin_keep
#include "fixture_registration.h"
// IWYU pragma: end_keep
// Suppression rationale: this fixture is an implementation-only header.
// IWYU pragma: private
// Suppression rationale: consumers must include the public fixture facade.
// IWYU pragma: private, include "fixture_public.h"
// Suppression rationale: the generated fixture peer is an intentional friend.
// IWYU pragma: friend "fixture_peer\\.h"
// Suppression rationale: the platform header is provided transitively.
// IWYU pragma: no_include <platform_fixture.h>
// Suppression rationale: the opaque fixture type must stay complete.
// IWYU pragma: no_forward_declare "struct fixture_type"
// Suppression rationale: the nonstandard fixture name has this associated header.
#include "fixture_associated.h" // IWYU pragma: associated
// Suppression rationale: consumers always require this registration header.
// IWYU pragma: always_keep
""",
    "apps/fixture/src/wrong_controls.c": r"""// Clang-format off
// clang-format off -- unsupported suffix
// iwyu pragma: keep
// IWYU pragma: no_include platform_fixture.h
// prettier-ignore
// markdownlint-disable MD013
""",
    "apps/fixture/src/clang_colon_control.c": r"""
// clang-format off: the fixture probes clang-format's supported colon rationale.
int columns[] = {1,  2};
// clang-format on
""",
    "apps/fixture/src/clang_empty_colon_control.c": r"""// clang-format off:
int preserved[] = {1,  2};
// clang-format on:
int reformatted[] = {1,  2};
""",
    "apps/fixture/src/unmatched_controls.c": r"""// clang-format on
// IWYU pragma: end_keep
/** @endcond */
""",
    "format_controls.yml": """---
payload: |
  # yamllint disable-line rule:line-length
# Suppression rationale: the fixture block intentionally demonstrates two noisy rules.
# yamllint disable rule:line-length rule:comments
url: https://example.invalid/an-intentionally-long-fixture-resource-locator
# yamllint enable rule:comments rule:line-length
# Suppression rationale: the signed URL cannot be wrapped or carry trailing whitespace edits.
# yamllint disable-line rule:line-length rule:trailing-spaces
signed_url: https://example.invalid/an-intentionally-long-fixture-resource-locator
""",
    "disable_file.yml": """# yamllint disable-file
fixture: deliberately broad
""",
    "wrong_format_controls.yml": """---
# YAMLLINT disable-line rule:line-length
# yamllint disable-line rule:line-length,rule:comments
# yamllint disable-file
fixture: malformed controls
""",
    "unmatched_format_controls.yml": """---
# yamllint enable rule:line-length
fixture: unmatched enable
""",
    "Dockerfile": """# Fixture prose mentions hadolint without controlling it.
FROM scratch
# hadolint ignore = DL3003 , SC2164  # The fixture combines Docker and shell findings.
RUN cd /tmp
""",
    "Dockerfile.hadolint-active": """FROM scratch
RUN cd /tmp
""",
    "Dockerfile.global": """# Suppression rationale: this generated fixture has no package index.
# hadolint global ignore = DL3008 , SC1091
FROM scratch
""",
    "Dockerfile.bad": """# Hadolint ignore=DL3008
# hadolint ignore=DL3008 -- unsupported suffix
# hadolint ignore=dl3008
# hadolint ignore=DL3008 ,
FROM scratch
""",
    "format_controls.cmake": """set(text "cmake-lint: disable=R0912")
# Suppression rationale: the fixture function is a declarative feature matrix.
# cmake-lint: disable=R0912,R0915 disable=C0103
# cmake-format: off -- the fixture table is compared byte-for-byte in source form.
set(matrix one    two)
# cmake-format: on
# Suppression rationale: the short spelling protects a second literal table.
# cmf: off
set(second_matrix three    four)
# cmf: on
""",
    "cmake_lint_space.cmake": """# Suppression rationale: whitespace exposes pragma behavior.
# cmake-lint: \tdisable=R0912\tdisable=C0103
function(BadName)
endfunction()
""",
    "cmake_lint_tab.cmake": """# Suppression rationale: tabs do not match the prefix.
# cmake-lint:	disable=C0103
function(BadName)
endfunction()
""",
    "wrong_format_controls.cmake": """# CMake-format: off
# cmake-format off
# cmake-lint: enable=R0912
set(value one)
""",
    "unmatched_format_controls.cmake": """
# Suppression rationale: unmatched fixture proves region validation.
# cmake-format: off
set(value one)
""",
    "docs/format_controls.md": """`<!-- prettier-ignore -->`
```html
<!-- markdownlint-disable MD013 -->
```
<!-- Suppression rationale: the fixture table is compared byte-for-byte. -->
<!-- prettier-ignore -->
<table><tr><td>fixture</td></tr></table>
<!-- Suppression rationale: the fixture URL is intentionally indivisible. -->
<!-- markdownlint-disable MD013 -->
https://example.invalid/an-intentionally-long-fixture-resource-locator
<!-- Suppression rationale: this one line contains a generated indivisible token. -->
<!-- markdownlint-disable-line MD013 -->
<!-- Suppression rationale: the next generated line contains an indivisible token. -->
<!-- markdownlint-disable-next-line MD013 -->
next generated line
<!-- markdownlint-capture -->
<!-- Suppression rationale: the captured fixture contains generated HTML. -->
<!-- markdownlint-disable MD033 -->
<!-- Suppression rationale: the nested generated URL intentionally remains bare. -->
<!-- markdownlint-disable MD034 -->
<span>generated section</span>
https://example.invalid/generated
<!-- markdownlint-restore -->
<!-- markdownlint-enable MD013 -->
<!-- Suppression rationale: generated front matter controls the whole fixture file. -->
<!-- markdownlint-disable-file MD041 -->
<!-- markdownlint-enable-file MD041 -->
<!-- @cond HTML_COMMENT_INACTIVE -->
<!-- @endcond -->
<!--
@cond MULTILINE_HTML_COMMENT_INACTIVE
@endcond
-->
<!-- Suppression rationale: the docs fixture hides an internal-only section. -->
@cond MARKDOWN_INTERNAL
hidden Markdown
@endcond
""",
    "docs/format_controls.dox": r"""
/** Suppression rationale: the dox fixture hides an internal-only section. */
\cond DOX_INTERNAL
hidden dox
\endcond
""",
    "libs/third_party/vendor/include/vendor_docs.h": """/** @cond VENDOR_INTERNAL */
int vendor_internal;
/** @endcond */
""",
    "docs/wrong_format_controls.md": """<!-- Prettier-ignore -->
<!-- prettier-ignore -- unsupported suffix -->
<!-- Markdownlint-disable MD013 -->
<!-- markdownlint-disable MD013,MD033 -->
<!-- markdownlint-capture MD013 -->
<!-- markdownlint-restore MD013 -->
<!-- markdownlint-configure-file { "MD013": FALSE } -->
<!-- markdownlint-configure-file { "MD013": 100 } -->
@COND WRONG_CASE
""",
    "docs/unmatched_format_controls.dox": "@endcond\n",
    "scripts/checks/suppression_selftest.py": """
# Suppression rationale: Python docs hide an internal helper.
## @cond PYTHON_INTERNAL
internal_helper = 1
## @endcond
""",
    "scripts/checks/suppression_scan.py": """# @cond single_hash_is_not_doxygen
## @COND WRONG_CASE
""",
    "generated/ignored.sh": """#!/bin/sh
if true; then
    echo ignored
fi
""",
    "generated/mixed-case.sh": """#!/bin/sh
if true; then
    echo ignored
fi
""",
    "generated/false.sh": """#!/bin/sh
if true; then
    echo formatted
fi
""",
    "generated/unset.sh": """#!/bin/sh
if true; then
    echo formatted
fi
""",
    "bad/generated/uppercase-value.sh": """#!/bin/sh
if true; then
    echo formatted
fi
""",
    ".prettierrc.json": "{}\n",
    ".markdownlint.json": "{}\n",
    ".editorconfig": """root = true

[generated/**]
# Generated shell fixtures are byte-for-byte protocol inputs.
ignore = true

[generated/mixed-case.sh]
# This mixed-case property proves EditorConfig names are case-insensitive.
IgNoRe = true

[generated/false.sh]
IGNORE = false

[generated/unset.sh]
iGnOrE = unset
""",
    "bad/.editorconfig": """root = true

[generated/**]
Ignore = TRUE

[invalid/**]
Ignore = maybe
""",
    ".yamllint.yaml": """---
extends: default
rules:
  # Fixture keys reproduce GitHub Actions' YAML 1.1 `on` requirement.
  truthy:
    check-keys: false
  # The fixture prose checker deliberately owns comment indentation.
  comments-indentation: false
""",
    ".hadolint.yaml": """---
failure-threshold: style
ignored:
  # Fixture packages resolve from a rotating archive without stable versions.
  - DL3008
""",
    ".github/tidy-baseline.txt": """# Fixture burn-down baseline.
sample.c\treadability-function-size\t1
sample.c\treadability-function-size\t1
malformed-row
""",
    ".github/cite-baseline.txt": """# Total at this baseline: 1 uncited access
missing.c\t1
""",
    ".github/mcdc-baseline.txt": """0
""",
    ".github/rogue-baseline.txt": """unexpected
""",
}

FIXTURES = CORE_FIXTURES | TOOL_CONTROL_FIXTURES


def fixture_digest(fixtures: dict[str, str]) -> str:
    """Return the length-framed digest used to authenticate fixture transport."""
    digest = hashlib.sha256()
    for rel, body in sorted(fixtures.items()):
        for data in (rel.encode(), body.encode()):
            digest.update(len(data).to_bytes(8, "big"))
            digest.update(data)
    return digest.hexdigest()
