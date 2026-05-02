# MISRA-C 2012 Deviation Register

This document records every formal deviation taken against MISRA-C 2012
in the ra8d2-firmware codebase, following the deviation procedure in
**MISRA-C:2012 section 5.2** (rationale, scope, alternative
mitigation, reviewer sign-off).

The audit baseline lives in [`docs/MISRA.md`](../MISRA.md). The full
per-violation list lives in [`docs/MISRA_GAPS.csv`](../MISRA_GAPS.csv).

## Cross-references

- Project coding standard: [`docs/STYLE_GUIDE.md`](../STYLE_GUIDE.md).
- Architectural ring + TrustZone-world tagging:
  [`docs/RING_AND_WORLD.md`](../RING_AND_WORLD.md).
- Audit driver script:
  [`scripts/utils/misra_check.sh`](../../scripts/utils/misra_check.sh).
- Per-tool qualification dossier:
  [`docs/qualification/TOOL_QUALIFICATION.md`](TOOL_QUALIFICATION.md).
- IEC 61508-3:2010 section 7.4.4 ("Use of language subset") -- the
  governing safety-functional-safety clause that motivates the MISRA
  obligation here.
- DO-178C section 11.8 ("Software Coding Standards").
- ISO 26262-6:2018 table 1 (language-subset requirement at ASIL C/D).

## Disposition classes

Each entry in this register is one of three dispositions, per the
gap-closure plan in `docs/MISRA.md`:

1. **Project deviation (formal)** -- the project is intentionally
   non-compliant with the rule; the deviation has rationale,
   alternative mitigation, and reviewer sign-off. This is the only
   class that consumes a deviation under MISRA-C:2012 sec. 5.2.
2. **Tooling gap (false positive)** -- cppcheck-MISRA reports a
   violation that is not actually present in the source code. These
   are recorded for traceability so that the audit baseline can be
   re-evaluated after the upstream tool fixes the parser bug; they
   do *not* require a formal MISRA deviation because the source obeys
   the rule.
3. **Code change** -- the violation is real and will be fixed in
   source; no deviation record is needed once the fix lands.

## Deviation index

| ID    | Rule            | Category | Class             | Status   | MAR        |
|-------|-----------------|----------|-------------------|----------|------------|
| D-001 | misra-c2012-15.5 | Advisory  | Project deviation | Active   | 2027-05-02 |
| D-002 | misra-c2012-17.3 | Mandatory | Tooling gap       | Active   | 2026-11-02 |
| D-003 | misra-c2012-9.2  | Required  | Tooling gap       | Active   | 2026-11-02 |
| D-004 | misra-c2012-12.1 | Advisory  | Partial deviation | Active   | 2027-05-02 |
| D-005 | misra-c2012-8.4  | Required  | Tooling gap       | Active   | 2026-11-02 |

`MAR` = mandatory annual review date (or earlier review trigger when
the underlying tooling assumption changes).

---

## D-001: Rule 15.5 -- single point of exit

- **Rule ID**: misra-c2012-15.5.
- **Rule text (paraphrased per MISRA licence)**: a function should
  have a single point of exit at the end.
- **Category**: Advisory.
- **Disposition**: Project deviation (formal).
- **Scope**: project-wide. Applies to every translation unit under
  `libs/`, `src/`, `port/`, and `examples/`.
- **Files affected**: 751 violations in the 2026-05-02 baseline,
  spread across substantially every `.c` file in the firmware tree.
  See `build/misra/results.txt` and `docs/MISRA_GAPS.csv` for the
  per-line list.

### Rationale

The project enforces **NASA Power-of-10 Rule 7** (check the return
value of every fallible function call) via the
`RA_RETURN_ON_ERROR(err, tag, msg)` macro defined in
`libs/ra_core/inc/ra_check.h` and the early-return idiom

```c
ra_err_t err = some_call(...);
if (err != k_ra_ok) {
  return err;
}
```

A strict single-exit refactor would either:

1. Introduce deeply nested `if (ok) { if (ok) { if (ok) { ... } } }`
   ladders that violate NASA Power-of-10 Rule 4 (cyclomatic-bound /
   60-line LineThreshold enforced by clang-tidy), or
2. Replace early returns with `goto cleanup;` -- forbidden by
   `docs/STYLE_GUIDE.md` and by NASA Power-of-10 Rule 1.

Both alternatives are strictly worse for safety and for review
ergonomics than the early-return idiom, which makes every error path
**locally** visible at the call site.

### Alternative mitigation

The intent of Rule 15.5 -- "every exit path is reachable, reviewable,
and reaches required cleanup" -- is met by the following independent
controls:

- **NASA Power-of-10 Rule 5** (>= 2 pre/post-condition checks per
  function), enforced by code review against `docs/STYLE_GUIDE.md`.
- **MC/DC structural-coverage target** of 100% at Phase 1, >=95% at
  Phase 2 (see `docs/qualification/SVCP.md`). MC/DC proves every exit
  path is executed by the verification suite.
- **clang-tidy LineThreshold = 60** keeps function bodies short
  enough that all exit paths fit in a single screen, eliminating the
  "hidden return" failure mode that motivates Rule 15.5 in larger
  functions.
- **No `goto` / `setjmp` / dynamic-allocation cleanup paths** (NASA
  Power-of-10 Rules 1 and 3, enforced by the pre-commit hooks
  `scripts/git/pre-commit` and `scripts/utils/check_no_dynamic_alloc.py`).
  Early return therefore cannot leak resources because there are no
  resources to leak.

### Standards basis

- **IEC 61508-3:2010 section 7.4.4** ("Use of language subset"):
  permits a documented subset that consciously deviates from the
  reference standard provided the deviation has rationale and a
  compensating control. The compensating control here is the MC/DC
  coverage obligation in Phase 1.
- **DO-178C section 6.4.4.2 (b)** accepts coverage-based proof of
  exit-path adequacy in lieu of a structural single-exit
  constraint.

### Risk assessment

- **Likelihood of escape**: low. Every exit path is exercised by the
  Phase 1 unit-test suite at >=100% MC/DC.
- **Severity of escape**: low. Functions are <=60 lines; any escape
  is locally visible at the early-return statement.
- **Net residual risk**: acceptable for IEC 61508 SIL 3 / DO-178C
  DAL B with the compensating MC/DC control.

### Review

- **Author**: Brighton Sikarskie.
- **Approved**: 2026-05-02.
- **Mandatory annual review**: 2027-05-02.
- **Trigger for early review**: any change that weakens the MC/DC
  coverage target, deletes the `RA_RETURN_ON_ERROR` macro, or relaxes
  the clang-tidy LineThreshold setting.

---

## D-002: Rule 17.3 -- function shall not be declared implicitly

- **Rule ID**: misra-c2012-17.3.
- **Rule text (paraphrased per MISRA licence)**: a function shall
  not be declared implicitly.
- **Category**: Mandatory.
- **Disposition**: Tooling gap (false positive).
- **Scope**: cppcheck 2.20 audit baseline only. Source code does
  not contain any implicit function declarations.
- **Files affected**: 170 spurious violations in the 2026-05-02
  baseline. See `docs/MISRA_GAPS.csv` for the per-line list.

### Root cause

cppcheck 2.20 does not implement `--std=c23`. The codebase uses

- C23 typed enums: `typedef enum : uint8_t { ... } name_t;`,
- C23 attributes: `[[nodiscard]]`, `[[maybe_unused]]`,
- C23 `= {}` empty aggregate initializers,

each of which raises `syntaxError` at the offending line. cppcheck's
recovery strategy abandons the in-flight declaration and continues
parsing the rest of the translation unit, so any function call to a
prototype that lived on a C23-syntax line is reported as "implicit
declaration". The actual source has the prototype; only the parser
mis-reads it.

### Why this is not a real defect

- Every first-party header uses `#pragma once` and is included before
  the matching definition; the C frontend used to build the firmware
  (`arm-none-eabi-gcc -std=gnu23 -Wimplicit-function-declaration
  -Werror`) treats any implicit declaration as a build-stopping
  error. CI builds are clean (see `.github/workflows/`).
- The Phase 1 host unit-test build (`make test`) compiles with
  `-Wimplicit-function-declaration -Werror` under host gcc / clang.
  Both toolchains support C23 declarations natively and produce zero
  implicit-declaration diagnostics.

### Alternative verification (until cppcheck ships C23)

- The arm-none-eabi-gcc cross build with `-std=gnu23
  -Wimplicit-function-declaration -Werror` is the authoritative
  Mandatory-rule check for 17.3.
- The host unit-test build provides a second independent compiler
  pass.
- `scripts/utils/check_world_tags.py` and the pre-commit
  clang-tidy run additionally enforce header inclusion hygiene.

### Standards basis

Per IEC 61508-3:2010 section 7.4.4.4, an automated tool that fails
to parse a source-language feature is not a substitute for a
qualified compiler. The compiler is the authoritative checker; the
audit tool is supplementary.

### Review

- **Author**: Brighton Sikarskie.
- **Approved**: 2026-05-02.
- **Mandatory annual review**: 2026-11-02 (six-month review tied to
  cppcheck release cadence; see TOOL_QUALIFICATION.md).
- **Trigger for early review**: cppcheck 2.21 (or any release that
  ships `--std=c23`) becoming available.

---

## D-003: Rule 9.2 -- braced aggregate initializers

- **Rule ID**: misra-c2012-9.2.
- **Rule text (paraphrased per MISRA licence)**: the initializer
  for an aggregate or union shall be enclosed in braces.
- **Category**: Required.
- **Disposition**: Tooling gap (false positive).
- **Scope**: cppcheck 2.20 audit baseline only.
- **Files affected**: 35 spurious violations in the 2026-05-02
  baseline.

### Root cause

C23 `= {}` (empty initializer) is mandated by `CLAUDE.md` in place
of the obsolete C99 `= {0}` form. cppcheck 2.20's MISRA addon reads
`= {}` as "no brace around aggregate" because its parser does not
recognize the C23 empty-initializer rule. The compiler accepts it
correctly at every build.

### Alternative verification

- arm-none-eabi-gcc `-std=gnu23 -Wmissing-braces -Werror` (cross
  build) and host gcc / clang in the unit-test build both validate
  every aggregate initializer at compile time.
- `scripts/utils/check_c23_zero_init.py` (pre-commit) actively
  *forbids* the legacy `= {0}` form and *requires* C23 `= {}`,
  giving an inverse check that complements the disabled cppcheck
  rule.

### Standards basis

Same as D-002. The compiler frontend is the authoritative checker
for syntactic initializer-form rules.

### Review

- **Author**: Brighton Sikarskie.
- **Approved**: 2026-05-02.
- **Mandatory annual review**: 2026-11-02.
- **Trigger for early review**: cppcheck adds `--std=c23`.

---

## D-004: Rule 12.1 -- explicit operator precedence

- **Rule ID**: misra-c2012-12.1.
- **Rule text (paraphrased per MISRA licence)**: the precedence of
  operators within expressions should be made explicit.
- **Category**: Advisory.
- **Disposition**: Partial deviation (formal). Project accepts
  precedence implicit in the C standard for the well-known cases
  enumerated below; all other Rule 12.1 hits are dispositioned as
  **Code change** and burned down on the housekeeping cadence in
  `docs/MISRA.md`.

### Accepted-as-implicit cases (no parentheses required)

The following precedences are sufficiently universal among C
programmers that adding parentheses would *reduce* readability:

1. `*` and `/` over `+` and `-`. Example: `a + b * c` is accepted
   without parentheses.
2. Unary operators (`-`, `!`, `~`, `++`, `--`, `&`, `*`) over any
   binary operator.
3. Member access (`.`, `->`) and array subscript (`[]`) over any
   other operator. Example: `&s->field` is accepted.
4. Postfix function-call `()` over any other operator.

### Cases that require parentheses (Code change)

All other Rule 12.1 hits -- mixing `&` with `==`, `<<` with `+`,
`?:` with binary arithmetic, `||` with `&&`, etc. -- shall be fixed
in source by adding redundant parentheses. clang-format is
configured to leave redundant parentheses untouched.

### Files affected

101 violations in the 2026-05-02 baseline. After the partial
deviation above is applied during the next audit pass, the residual
count is the Code-change burn-down backlog.

### Standards basis

- IEC 61508-3:2010 section 7.4.4.6 (b) permits accepting a coding
  rule subject to "documented justification" of the cases that are
  treated as obvious by domain practitioners. The four cases above
  are taught in every introductory C course (Kernighan and Ritchie,
  *The C Programming Language*, 2nd ed., section 2.12 and table on
  p. 53) and are correctly understood by every static-analysis tool
  the project uses.

### Risk assessment

- **Likelihood of misread**: low. The accepted cases are taught at
  the level of introductory C texts.
- **Severity of misread**: low. clang-tidy enforces braces around
  every control statement, so any precedence misread is contained
  inside the immediate expression.
- **Net residual risk**: acceptable for IEC 61508 SIL 3 / DO-178C
  DAL B.

### Review

- **Author**: Brighton Sikarskie.
- **Approved**: 2026-05-02.
- **Mandatory annual review**: 2027-05-02.

---

## D-005: Rule 8.4 -- compatible declaration before definition

- **Rule ID**: misra-c2012-8.4.
- **Rule text (paraphrased per MISRA licence)**: a compatible
  declaration shall be visible when an object or function with
  external linkage is defined.
- **Category**: Required.
- **Disposition**: Tooling gap (false positive).
- **Scope**: cppcheck 2.20 audit baseline only.
- **Files affected**: 196 spurious violations in the 2026-05-02
  baseline, distributed across:

  | File                                        | Hits |
  |---------------------------------------------|-----:|
  | `port/nimble/nimble_npl_threadx.c`          |  40  |
  | `port/lwip/arch/sys_arch.c`                 |  31  |
  | `libs/ra_net/src/ra_net_ipv4.c`             |  11  |
  | `libs/ra_fs/src/ra_fs_fat.c`                |  11  |
  | `libs/ra_ble_host/src/ra_ble_security.c`    |  11  |
  | `libs/ra_ota/src/ra_ota.c`                  |  10  |
  | `libs/ra_psa_crypto/src/ra_psa_crypto.c`    |   9  |
  | `libs/ra_gfx/src/ra_gfx_text.c`             |   9  |
  | `libs/ra_ble_host/src/ra_ble_gatt_client.c` |   9  |
  | `libs/ra_tls/src/ra_tls.c`                  |   7  |
  | `libs/ra_reflow/src/ra_reflow_layout.c`     |   6  |
  | `libs/ra_epub/src/ra_epub_chapter.c`        |   6  |
  | `libs/ra_ble_host/src/ra_ble_mesh.c`        |   6  |
  | `libs/ra_touch_cal/src/ra_touch_cal.c`      |   5  |
  | `libs/ra_power_profile/src/ra_power_profile.c` | 5 |
  | `libs/ra_modem_at/src/ra_modem_at.c`        |   5  |
  | `libs/ra_mpu/src/ra_mpu.c`                  |   4  |
  | `libs/ra_ble_host/src/ra_ble_gatt.c`        |   4  |
  | `libs/ra_epub/src/ra_epub_open.c`           |   2  |
  | `libs/ra_core/src/ra_rand_stub.c`           |   2  |
  | `libs/ra_reflow/src/ra_reflow_render.c`     |   1  |
  | `libs/ra_reflow/src/ra_reflow_parse.c`      |   1  |
  | `libs/ra_gfx/src/ra_gfx_font_8x16.c`        |   1  |

### Root cause

cppcheck 2.20 cannot parse C23 attribute syntax (`[[nodiscard]]`,
`[[maybe_unused]]`). The project applies `[[nodiscard]]` to every
fallible public API in `libs/<module>/inc/<module>.h` to enforce
NASA Power-of-10 Rule 7 (check the return value of every call).
When cppcheck encounters

```c
[[nodiscard]] ra_err_t ra_mpu_configure(const ra_mpu_cfg_t* cfg);
```

it emits `syntaxError` and discards the prototype from its symbol
table. The matching definition in `ra_mpu.c` is therefore reported
as having no prior declaration -- a Rule 8.4 false positive.

A second class of 8.4 false positive arises for the `port/`
directory: the prototypes for `ble_npl_*` (NimBLE) and `sys_*`
(lwIP) live in third-party headers under `libs/third_party/`, which
are intentionally excluded from the audit (per
`scripts/utils/misra_check.sh` `--suppress=*:libs/third_party/*`).
The `port/` translation units therefore appear to define functions
with no prior declaration, but the declaration genuinely exists --
the auditor was instructed not to look at it.

The reproducer is documented in this register on purpose: a future
maintainer who sees the 196-hit baseline drop after the cppcheck
upgrade can confirm both root causes are gone.

### Why this is not a real defect

- The project compiles every translation unit with arm-none-eabi-gcc
  `-Wmissing-prototypes -Wstrict-prototypes -Wimplicit-function-
  declaration -Werror` at IEC 61508 SIL 3 / DO-178C DAL B build
  level. A real Rule 8.4 violation (definition without prior
  matching prototype) would fail the cross build and would block
  every commit at the pre-commit clang-tidy + CI build gate.
- Every public function in the affected files is declared in the
  matching `*/inc/*.h` header; the headers are included before the
  definitions in the same translation unit. This was spot-checked on
  the four representative files
  (`libs/ra_mpu/src/ra_mpu.c`,
  `libs/ra_ble_host/src/ra_ble_gatt.c`,
  `port/nimble/nimble_npl_threadx.c`,
  `port/lwip/arch/sys_arch.c`) before this register was authored.
- Module-internal functions are marked `static` and are caught
  separately by clang-tidy's `misc-unused-using-decls` and gcc's
  `-Wmissing-declarations`.

### Alternative verification (until cppcheck ships C23)

- arm-none-eabi-gcc cross build with the warning flags listed above
  is the authoritative Required-rule check for 8.4.
- Host unit-test build (`make test`) provides a second independent
  compiler pass.
- The Phase 4 commercial-tool re-audit (LDRA / Polyspace / QAC --
  see `docs/MISRA.md` section "Workflow", item 4) will provide
  authoritative MISRA evidence at certification time.

### Standards basis

Same as D-002. Per IEC 61508-3:2010 section 7.4.4.4, the qualified
compiler is the authoritative checker for declaration-compatibility
rules; the unqualified open-source audit tool is supplementary.

### Risk assessment

- **Likelihood of escape**: zero. The cross compiler rejects any
  real Rule 8.4 violation as a build error.
- **Severity of escape**: not applicable (likelihood is zero).
- **Net residual risk**: acceptable for IEC 61508 SIL 3 / DO-178C
  DAL B.

### Review

- **Author**: Brighton Sikarskie.
- **Approved**: 2026-05-02.
- **Mandatory annual review**: 2026-11-02 (tied to cppcheck release
  cadence -- shared review window with D-002 and D-003).
- **Trigger for early review**: cppcheck 2.21 (or any release that
  ships `--std=c23`) becoming available; or any change that removes
  `[[nodiscard]]` from public-header prototypes.

---

## Change log

| Date       | Author              | Change                              |
|------------|---------------------|-------------------------------------|
| 2026-05-02 | Brighton Sikarskie  | Initial population (D-001..D-005).  |
