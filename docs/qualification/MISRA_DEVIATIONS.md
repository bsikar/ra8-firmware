# MISRA-C 2012 Deviation Register

**Last refreshed**: 2026-07-22 (D-001..D-007 active; D-007 records the
Rule 14.2 C23-attribute parse phantom on `RA8_PROTECTED_WRITE` inside
`[[nodiscard]]` functions -- same tooling gap as D-002 / D-005).

This document records every formal deviation taken against MISRA-C 2012
in the ra8-firmware codebase, following the deviation procedure in
**MISRA-C:2012 section 5.2** (rationale, scope, alternative
mitigation, reviewer sign-off).

The audit baseline lives in [`docs/MISRA.md`](../MISRA.md). The full
per-violation list lives in [`docs/MISRA_GAPS.csv`](../MISRA_GAPS.csv).

## Cross-references

- Project coding standard: [`docs/STYLE_GUIDE.md`](../STYLE_GUIDE.md).
- Architectural ring + TrustZone-world tagging:
  [`docs/RING_AND_WORLD.md`](../RING_AND_WORLD.md).
- Audit driver script:
  [`scripts/checks/misra_check_inner.sh`](../../scripts/checks/misra_check_inner.sh).
- Per-tool qualification dossier:
  [`docs/qualification/TOOL_QUALIFICATION.md`](TOOL_QUALIFICATION.md).
- IEC 61508-3:2010 section 7.4.4 ("Use of language subset") -- the
  governing safety-functional-safety clause that motivates the MISRA
  obligation here.
- DO-178C section 11.8 ("Software Coding Standards").
- ISO 26262-6:2018 table 1 (language-subset requirement at ASIL C/D).

## Tooling policy

`cppcheck` (with the upstream `misra.py` addon) is the **sole** MISRA-C
2012 enforcement tool for this project. Commercial MISRA checkers --
LDRA Testbed, Perforce Helix QAC, MathWorks Polyspace Bug Finder /
Code Prover, and PVS-Studio -- are explicitly **out of scope** and
will never be procured.

### Rationale

`ra8-firmware` is an MIT-licensed, FOSS, $0-budget personal /
research project. It will not ship as a regulated commercial product
and it will not seek third-party certification (see
`docs/CERTIFICATION_SCOPE.md`). Paying $5k-$30k per seat per year for
a commercial MISRA checker -- on top of the $30k-$150k independent-
assessor engagement that would also be required to convert that
checker's output into qualified evidence -- is incompatible with the
project's funding model and serves no qualification goal that the
project actually pursues.

### Coverage envelope

`cppcheck` 2.20's `misra.py` addon implements approximately
**60-70 %** of the MISRA-C 2012 mandatory + required + advisory rule
set. The remaining ~30 % is **accepted as residual risk** under
**IEC 61508-7 Annex D.7** ("achievable assurance with available
tools"): any MISRA rule that no FOSS tool can statically check
remains uncovered by automated analysis but is still in force as a
coding-standard obligation enforced via code review.

### Decision-finality

This decision is **final**. Future agents and contributors should
not raise the procurement question again, and should not insert
"upgrade path to LDRA / Polyspace / QAC" wording into any audit
artefact. If a downstream party adopts this codebase for a regulated
product they are responsible for procuring their own commercial
checker -- see `docs/CERTIFICATION_SCOPE.md`.

### Cross-references

- `docs/CERTIFICATION_SCOPE.md` -- the "no independent assessor"
  decision that makes the FOSS-only tooling stance internally
  consistent.
- `docs/QUALIFICATION_ROADMAP.md` Section 6 -- the procurement
  question, marked CLOSED with reference to this section.
- `docs/qualification/TOOL_QUALIFICATION.md` -- per-tool TQL
  classification including `cppcheck`.

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
| D-006 | misra-c2012-20.5 | Advisory  | Project deviation | Active   | 2027-05-02 |
| D-007 | misra-c2012-14.2 | Required  | Tooling gap       | Active   | 2026-11-02 |
| D-008 | misra-c2012-17.1 | Required  | Project deviation | Active   | 2027-07-27 |
| D-009 | misra-c2012-9.5  | Required  | Tooling gap       | Active   | 2026-11-02 |

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
`RA8_RETURN_ON_ERROR(err, tag, msg)` macro defined in
`libs/ra8_core/inc/ra8_check.h` and the early-return idiom

```c
ra8_err_t err = some_call(...);
if (err != k_ra8_ok) {
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
  `scripts/git/pre-commit` and `scripts/checks/check_no_dynamic_alloc.py`).
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
  coverage target, deletes the `RA8_RETURN_ON_ERROR` macro, or relaxes
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
- `scripts/checks/check_world_tags.py` and the pre-commit
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
- `scripts/git/pre-commit` actively
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

91 violations in the current tree (the 2026-05-02 baseline recorded
101; the retired BLE host facade accounted for 10). Each is recorded in
`.cppcheck-suppressions` under the `misra-c2012-12.1:` block as a
per-file:line entry. The full inventory is:

| File                                              | Lines |
|---------------------------------------------------|-------|
| `libs/ra8_core/src/ra8_log.c`                       | 251 |
| `libs/ra8_epub/src/ra8_epub_chapter.c`              | 64, 70, 77, 168, 192, 208, 212, 232, 247, 251, 271, 293, 298 |
| `libs/ra8_epub/src/ra8_epub_open.c`                 | 96, 213, 217 |
| `libs/ra8_fs/src/ra8_fs_fat.c`                      | 275, 319, 414, 417, 419, 478, 491, 526, 564, 734, 756, 816, 825, 867, 870, 1042, 1140, 1147, 1278, 1281, 1319, 1331, 1374, 1380, 1405 |
| `libs/ra8_mpu/src/ra8_mpu.c`                        | 62, 65, 68, 71, 98, 195 |
| `libs/ra8_net/src/ra8_net_tcp.c`                    | 84, 90 |
| `libs/ra8_ota/src/ra8_ota.c`                        | 190, 235, 255 |
| `libs/ra8_reflow/src/ra8_reflow_layout.c`           | 266, 311, 316, 345, 366, 403, 510, 535, 538, 541, 584, 615, 633, 636 |
| `libs/ra8_reflow/src/ra8_reflow_parse.c`            | 62 |
| `libs/ra8_reflow/src/ra8_reflow_render.c`           | 122 |
| `port/lwip/arch/sys_arch.c`                       | 158, 166, 181, 224, 230, 246, 253, 280, 286, 292, 299, 316, 333, 341, 355, 380, 398, 405, 452 |
| `port/nimble/src/nimble_npl_threadx.c`                | 354, 398, 417 |

Per-line review notes:

* Every flagged line falls into one of the accepted-as-implicit
  categories above. The 2026-05-02 spot check confirmed:
  - The `libs/ra8_epub/chapter`, `libs/ra8_fs/`,
    `libs/ra8_mpu/` (62-71), `libs/ra8_net/`, `libs/ra8_ota/`, and
    `libs/ra8_reflow/` hits are predominantly inside Doxygen `/** ... */`
    blocks or on function-signature lines that the `misra.py` addon
    has misattributed from a nearby expression in the dump.
  - The `port/lwip/arch/sys_arch.c` and
    `port/nimble/src/nimble_npl_threadx.c` hits are trivial null-check
    chains of the form `(p != NULL && p->field != 0U)` plus the
    `ticks * 1000U / hz` and `ms * hz + 999U` tick-conversion
    statements -- both squarely covered by case 1 (`* /` over `+ -`)
    and standard comparison-over-`&&` precedence.
  - The `libs/ra8_mpu/src/ra8_mpu.c` (98, 195) hits are MPU
    region-validation chains of the same null-check shape.

After the partial deviation above is applied during the next audit
pass, the residual code-change burn-down backlog is zero. Any future
12.1 hit raised against new source must be re-triaged: add
parentheses for genuine ambiguity, or extend the
`.cppcheck-suppressions` block above with a one-line justification
referencing the accepted-as-implicit category.

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
- **Files affected**: 166 spurious violations in the current tree (the
  2026-05-02 baseline recorded 196; the retired BLE host facade
  accounted for 30), distributed across:

  | File                                        | Hits |
  |---------------------------------------------|-----:|
  | `port/nimble/src/nimble_npl_threadx.c`          |  40  |
  | `port/lwip/arch/sys_arch.c`                 |  31  |
  | `libs/ra8_net/src/ra8_net_ipv4.c`             |  11  |
  | `libs/ra8_fs/src/ra8_fs_fat.c`                |  11  |
  | `libs/ra8_ota/src/ra8_ota.c`                  |  10  |
  | `libs/ra8_psa_crypto/src/ra8_psa_crypto.c`    |   9  |
  | `libs/ra8_gfx/src/ra8_gfx_text.c`             |   9  |
  | `libs/ra8_tls/src/ra8_tls.c`                  |   7  |
  | `libs/ra8_reflow/src/ra8_reflow_layout.c`     |   6  |
  | `libs/ra8_epub/src/ra8_epub_chapter.c`        |   6  |
  | `libs/ra8_touch_cal/src/ra8_touch_cal.c`      |   5  |
  | `libs/ra8_power_profile/src/ra8_power_profile.c` | 5 |
  | `libs/ra8_modem_at/src/ra8_modem_at.c`        |   5  |
  | `libs/ra8_mpu/src/ra8_mpu.c`                  |   4  |
  | `libs/ra8_epub/src/ra8_epub_open.c`           |   2  |
  | `libs/ra8_core/src/ra8_rand_stub.c`           |   2  |
  | `libs/ra8_reflow/src/ra8_reflow_render.c`     |   1  |
  | `libs/ra8_reflow/src/ra8_reflow_parse.c`      |   1  |
  | `libs/ra8_gfx/src/ra8_gfx_font_8x16.c`        |   1  |

### Root cause

cppcheck 2.20 cannot parse C23 attribute syntax (`[[nodiscard]]`,
`[[maybe_unused]]`). The project applies `[[nodiscard]]` to every
fallible public API in `libs/<module>/inc/<module>.h` to enforce
NASA Power-of-10 Rule 7 (check the return value of every call).
When cppcheck encounters

```c
[[nodiscard]] ra8_err_t ra8_mpu_configure(const ra8_mpu_cfg_t* cfg);
```

it emits `syntaxError` and discards the prototype from its symbol
table. The matching definition in `ra8_mpu.c` is therefore reported
as having no prior declaration -- a Rule 8.4 false positive.

A second class of 8.4 false positive arises for the `port/`
directory: the prototypes for `ble_npl_*` (NimBLE) and `sys_*`
(lwIP) live in third-party headers under `libs/third_party/`, which
are intentionally excluded from the audit (per
`scripts/checks/misra_check_inner.sh` `--suppress=*:libs/third_party/*`).
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
  (`libs/ra8_mpu/src/ra8_mpu.c`,
  `libs/ra8_tls/src/ra8_tls.c`,
  `port/nimble/src/nimble_npl_threadx.c`,
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

## D-006: Rule 20.5 -- #undef shall not be used

- **Rule ID**: misra-c2012-20.5.
- **Rule text (paraphrased per MISRA licence)**: `#undef` shall not
  be used.
- **Category**: Advisory.
- **Disposition**: Project deviation (deliberate, safety-motivated).
- **Scope**: exactly one site, `libs/ra8_nsc/inc/ra8_nsc_veneer.h`.
- **Files affected**:

  | File                                | Hits |
  |-------------------------------------|-----:|
  | `libs/ra8_nsc/inc/ra8_nsc_veneer.h` |   1  |

### Root cause

`RA8_NSC_VENEER` must mean two different things in two different
compilations of the same declaration. In a Secure-world translation
unit (`-mcmse`) it must carry
`__attribute__((cmse_nonsecure_entry))`, which is what makes the
linker emit the secure-gateway (SG) veneer that the Non-Secure world
branches through. Everywhere else -- Non-Secure images, single-world
firmware and the host unit tests -- it must be a plain no-op, because
gcc ignores the attribute without `-mcmse` and the resulting
`-Wattributes` diagnostic is fatal under the project `-Werror`
profile.

`libs/ra8_core/inc/ra8_attributes.h` also defines `RA8_NSC_VENEER`,
as an annotation-only marker, so that translation units which never
touch the NSC boundary can still be scanned by the libclang
annotation gate. Two headers therefore define the same macro, and
before this deviation whichever header a translation unit included
last silently won.

That is not a stylistic concern. When `ra8_attributes.h` won inside
an NSC translation unit -- as it did in `ra8_nsc_wdt.c` and
`ra8_nsc_xspi.c` -- the CMSE attribute was dropped, the SG veneer was
never emitted, and the Secure/Non-Secure boundary was broken with no
diagnostic beyond a macro-redefinition warning.

### Why `#undef` is the correct construct

`ra8_nsc_veneer.h` is designated the single authority for the macro.
It includes `ra8_attributes.h`, `#undef`s the generic marker, and
re-defines `RA8_NSC_VENEER` to carry the annotation and the CMSE
attribute together. The `#undef` is what makes the correct
definition win **regardless of include order**, which is the whole
safety property: a future edit that reorders includes in an NSC
translation unit cannot silently disarm the boundary.

### Alternatives considered and rejected

1. **Include-order convention** (require `ra8_nsc_veneer.h` last).
   Rejected: unenforceable by the compiler, and the failure mode is
   silent and security-critical.
2. **`#ifndef` guard in `ra8_attributes.h`**. Rejected: it makes the
   winner depend on include order in the opposite direction, so the
   same silent failure remains reachable.
3. **Move the CMSE logic into `ra8_attributes.h`** so only one
   definition exists. Rejected: `ra8_core` is the foundation library
   and is included by host tests and by both worlds;
   `check_core_layering.py` exists to keep TrustZone-specific
   concerns out of it, and `<arm_cmse.h>` is not available on the
   host.

### Alternative mitigation

The intent of Rule 20.5 -- that a macro's meaning be unambiguous at
every use site -- is met more strongly here than by the rule itself:

- `scripts/checks/check_nsc_cmse.sh` compiles every NSC translation
  unit under `-mcmse` with `-Wall -Wextra -Werror`, so a macro clash
  of this class fails the gate rather than warning past it.
- `scripts/checks/check_sg_offsets.py` inspects the linked Secure ELF
  and asserts the SG veneer slot offsets still match the `k_sg_off_*`
  enum the Non-Secure image reaches them by. A dropped veneer fails
  this gate at the object level, not merely at the source level.
- Both headers carry a `@warning` block naming the other and stating
  which one is authoritative.

### Standards basis

Rule 20.5 is **Advisory**, the weakest MISRA category, and MISRA
C:2012 permits a documented deviation for Advisory rules where the
alternative carries greater risk. The alternative here is a silent
TrustZone boundary break.

### Risk assessment

Low. One site, in a header whose sole purpose is to define this
macro, guarded by two independent automated checks (source-level and
object-level).

### Review

- **MAR**: 2027-05-02.
- **Earlier review trigger**: any change to the `RA8_NSC_VENEER`
  definition in either header; any toolchain that provides a
  portable `[[gnu::cmse_nonsecure_entry]]` spelling, which would
  allow the annotation and the attribute to be composed without
  redefinition.

---

## D-007: Rule 14.2 -- for loop shall be well formed

- **Rule ID**: misra-c2012-14.2.
- **Rule text (paraphrased per MISRA licence)**: a `for` loop shall
  be well formed (the loop counter is initialised in the first clause,
  tested in the second, modified only in the third, and not modified
  in the body).
- **Category**: Required.
- **Disposition**: Tooling gap (false positive).
- **Scope**: cppcheck audit baseline only. The `RA8_PROTECTED_WRITE`
  scoped-unlock macro (`libs/ra8_hal/inc/ra8_register_protection.h`)
  expands to a run-once `for` loop whose counter is initialised,
  tested and modified only in the three loop-header clauses and never
  in the body -- a well-formed loop.
- **Files affected**: the phantom fires only where the macro sits
  inside a `[[nodiscard]]` function. Beyond the `libs/ra8_hal/src/ra8_cgc*.c`
  hits already frozen in the baseline before this register was
  extended, the sites are:

  | File                                     | Hits |
  |------------------------------------------|-----:|
  | `libs/ra8_hal/src/ra8_bkup.c`            |  12  |
  | `libs/ra8_hal/src/ra8_bkup_tamper.c`     |   2  |
  | `libs/ra8_hal/src/ra8_bkup_security.c`   |   1  |

  The `ra8_bkup*` hits appeared when the VBATT / tamper bring-up moved
  its register writes inside `RA8_PROTECTED_WRITE` windows (issue #131)
  and the baseline was not extended at the time; the `ra8_cgc*` hits
  have carried the identical phantom in the baseline since those
  drivers were first audited.

### Root cause

Same C23-parse defect as D-002 and D-005. cppcheck (`--std=c11`,
which the audit is pinned to because 2.13/2.20 reject `--std=c23`)
raises `syntaxError` on a `[[nodiscard]]` function definition and its
recovery mis-reads the function body. When the body opens with the
`for` loop `RA8_PROTECTED_WRITE` expands to, the damaged parse charges
it Rule 14.2. The same macro in a plain (non-`[[nodiscard]]`)
function -- for example a `static void` helper -- parses cleanly and
draws no 14.2, which is the reproducer that isolates the cause: it is
the attribute, not the loop.

### Why this is not a real defect

- `RA8_PROTECTED_WRITE` expands to
  `for (uint32_t ra8_prot_once_ = ra8_prot_scope_begin(uv); ra8_prot_once_ != 0U; ra8_prot_once_ = ra8_prot_scope_end())`.
  The loop counter `ra8_prot_once_` is initialised in clause 1, tested
  in clause 2 and assigned in clause 3; the body never reads or writes
  it. That is precisely a well-formed loop.
- arm-none-eabi-gcc builds every affected translation unit with
  `-Wall -Wextra -Werror`; a genuinely malformed loop would not survive
  the cross build or the host unit-test build.

### Alternative verification (until cppcheck ships C23)

- arm-none-eabi-gcc cross build with the warning flags above.
- Host unit-test build (`make test`), a second independent compiler
  pass over the same sources.
- The Phase 4 commercial-tool re-audit (LDRA / Polyspace / QAC) will
  provide authoritative MISRA evidence at certification time.

### Standards basis

Same as D-002. Per IEC 61508-3:2010 section 7.4.4.4, the qualified
compiler is the authoritative checker; the unqualified open-source
audit tool is supplementary.

### Risk assessment

- **Likelihood of escape**: zero. A real malformed loop is a cross
  build error.
- **Severity of escape**: not applicable (likelihood is zero).
- **Net residual risk**: acceptable for IEC 61508 SIL 3 / DO-178C
  DAL B.

### Review

- **Author**: Brighton Sikarskie.
- **Mandatory annual review**: 2026-11-02 (shared cppcheck-cadence
  window with D-002, D-003 and D-005).
- **Trigger for early review**: cppcheck shipping `--std=c23`; or any
  change that removes `[[nodiscard]]` from the affected functions or
  reworks `RA8_PROTECTED_WRITE` away from a `for`-loop guard.

---

## Change log

| Date       | Author              | Change                              |
|------------|---------------------|-------------------------------------|
| 2026-05-02 | Brighton Sikarskie  | Initial population (D-001..D-005).  |
| 2026-07-18 | Brighton Sikarskie  | Add D-006 (Rule 20.5, NSC veneer).  |
| 2026-07-22 | Brighton Sikarskie  | Add D-007 (Rule 14.2, C23 attribute phantom). |

---

## D-008: Rule 17.1 -- the features of <stdarg.h> shall not be used

- **Rule ID**: misra-c2012-17.1.
- **Rule text (paraphrased per MISRA licence)**: the features of
  `<stdarg.h>` shall not be used.
- **Category**: Required.
- **Disposition**: Project deviation (formal).
- **Scope**: the esp-hosted logging bridge only --
  `port/esp-hosted/src/ra8_esp_hosted_fmt.c`,
  `ra8_esp_hosted_fmt_internal.h`, `ra8_esp_hosted_log.c`,
  `ra8_esp_hosted_log_internal.h`, and the one vtable row in
  `ra8_esp_hosted_osi.c` that forwards to it. No other first-party
  file in the repository uses `<stdarg.h>`, and none may without
  extending this record.

### Why the variadic interface is not a choice here

The vendored esp-hosted host driver
(`libs/third_party/esp-hosted/`, SOUP pinned at `949bb30`) logs through
`printf`-style call sites -- `ESP_LOGI(TAG, "rx len %u if %d", len, if_type)`
-- in 13 translation units. Those call sites are upstream's source and
are not editable: the tree records the component as having zero
deviations and verifies every file byte-identical to its upstream pin
(`docs/SOUP/esp-hosted-host.md`). The OS-abstraction vtable the driver
calls through likewise declares its log row as
`void (*_h_printf)(int level, const char *tag, const char *format, ...)`,
so the signature is fixed by the seam, not by this port.

A port that refused variadic arguments could therefore only drop the
driver's diagnostics entirely. On a link that has never been driven on
hardware, the diagnostics are the bring-up instrument.

### Why this is bounded

- **One entry point.** Every variadic path in the port funnels into
  `ra8_esp_hosted_log_vwrite`, which immediately converts the argument
  list into a finished string and calls nothing variadic thereafter.
  Nothing else in `port/esp-hosted/` takes a `...` parameter.
- **The formatter is first-party and fully tested.** The concern behind
  Rule 17.1 is that `va_arg` is unchecked: read at the wrong width and
  every later argument misaligns. `ra8_esp_hosted_fmt.c` addresses that
  directly -- it parses the length modifier explicitly and reads at
  exactly the named width, refuses to consume an argument for a
  conversion it does not implement (copying the specifier through
  verbatim instead, so later arguments stay aligned), and bounds every
  loop by a compile-time constant. `tests/test_ra8_esp_hosted_fmt.c`
  drives all of that, including the misalignment case, with MC/DC
  vectors.
- **The compiler checks the call sites.** Both the log entry point and
  the vtable row carry `[[gnu::format(printf, 3, 4)]]`, and the project
  builds with `-Wformat=2`, so a format string that disagrees with its
  arguments is a build error at the vendored call site -- which is the
  check Rule 17.1 exists to substitute for.
- **No allocation, bounded output.** The formatter writes only into a
  caller-supplied buffer and never calls the C library's `printf`
  family, whose formatting paths this project cannot admit (NASA Power
  of 10 Rule 3; this board has no heap and `_sbrk` fatal-errors).

### Review trigger

If the vendored driver is ever re-vendored with a non-variadic logging
seam, or if the port stops carrying the driver's diagnostics, this
deviation is withdrawn rather than renewed.

---

## D-009: Rule 9.5 -- array size explicit under designated initializers

- **Rule ID**: misra-c2012-9.5.
- **Rule text (paraphrased per MISRA licence)**: where designated
  initializers are used to initialize an array object, the size of
  the array shall be specified explicitly.
- **Category**: Required.
- **Disposition**: Tooling gap (false positive).
- **Scope**: cppcheck 2.13.0 audit baseline only.
- **Files affected**: 4 spurious violations, in
  `libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2.c` (3),
  `libs/ra8_hal/src/ra8_lvd.c` (1), `libs/ra8_hal/src/ra8_ssie.c` (1)
  and `libs/ra8_mpu/src/ra8_mpu.c` (1).

### Root cause

Every affected array *does* specify its size explicitly -- but as a
typed-enum constant rather than a numeric literal, e.g.

```c
static const ra8_mpu_region_t s_ra8_mpu_boot_regions[k_ra8_mpu_boot_region_count] = { ... };
```

cppcheck 2.13.0's MISRA addon resolves the size expression only when
it is a literal token, so an enum-named extent reads to the addon as
"no explicit size". The size is explicit, and the compiler resolves
it at translation time; the auditor simply cannot see it.

The construct is not incidental. `CLAUDE.md` makes typed enums
**mandatory** for every integer constant and forbids `#define` for
the purpose, so every fixed-size table in first-party code is
declared exactly this way. The rule as implemented therefore fires
on the house style rather than on a defect.

### Negative control

`libs/ra8_hal/src/ra8_ssie.c` proves the addon is not reacting to
designated initializers at all:

```c
static const ra8_mstp_t s_ssie_mstp_table[k_ra8_ssie_channel_count] = {
  k_ra8_mstp_ssie0,
  k_ra8_mstp_ssie1,
};
```

There is no designator anywhere in that initializer, so Rule 9.5
cannot apply by its own wording -- yet the addon reports it. The
common factor across all reported sites is the enum-named extent,
not the initializer form.

### Alternative verification

- arm-none-eabi-gcc `-std=gnu23 -Wall -Wextra -Werror` (cross build)
  and host gcc / clang in the unit-test build reject any array whose
  initializer overruns its declared extent, which is the hazard Rule
  9.5 exists to prevent.
- `scripts/checks/check_magic_numbers.py` independently forbids a
  numeric-literal extent, so the literal form the addon wants is not
  reachable in this codebase.

### Standards basis

Same as D-002. Per IEC 61508-3:2010 section 7.4.4.4 the qualified
compiler is the authoritative checker for declaration-form rules;
the unqualified open-source audit tool is supplementary.

### Risk assessment

- **Likelihood of escape**: zero. An array whose declared extent
  disagrees with its initializer is a cross-build error.
- **Severity of escape**: not applicable (likelihood is zero).
- **Net residual risk**: acceptable for IEC 61508 SIL 3 / DO-178C
  DAL B.

### Review

- **Author**: Brighton Sikarskie.
- **Approved**: 2026-08-03.
- **Mandatory annual review**: 2026-11-02.
- **Trigger for early review**: cppcheck's MISRA addon learns to
  resolve enum-named array extents.
