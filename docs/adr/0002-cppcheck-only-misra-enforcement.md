# ADR-0002: cppcheck-only MISRA enforcement under a FOSS-only budget

## Status

Accepted -- 2026-02-10.

## Context

ADR-0001 commits the project to IEC 61508 SIL 3 / DO-178C Level B.
Both standards expect the source language to be constrained by a
documented coding standard; for C, the de-facto choice is **MISRA
C:2012** (with Amendments 1-4) or its safety-critical superset
**MISRA C:2023**.

Enforcing MISRA in CI requires a checker. The mainstream choices:

| Checker            | License           | Rule coverage              | Cost    |
|--------------------|-------------------|----------------------------|---------|
| LDRA Testbed       | Commercial        | Full MISRA C:2012/2023     | $$$$    |
| Parasoft C/C++test | Commercial        | Full MISRA C:2012/2023     | $$$$    |
| PRQA / Helix QAC   | Commercial        | Full MISRA C:2012/2023     | $$$$    |
| Coverity           | Commercial (free for OSS) | Partial MISRA       | $$$ / 0 |
| clang-tidy         | FOSS (Apache 2)   | ~30 MISRA rules via `misra-*` checks | 0 |
| cppcheck           | FOSS (GPLv3)      | ~120 MISRA rules via the bundled `addons/misra.py` add-on | 0 |
| PC-lint Plus       | Commercial        | Full MISRA C:2012          | $$$     |

Project constraints:

* **$0 software budget.** This is a personal research project;
  per-seat licensing for a commercial MISRA checker is not on the
  table.
* **FOSS-only policy.** Every tool that runs in CI or the
  pre-commit hook must be installable from a public package
  manager (apt, brew, pip) by anyone cloning the repo, with a
  license compatible with redistribution.
* **Zero-warning gate.** ADR-0001 requires CI to fail on any new
  diagnostic, so the chosen checker must produce a stable,
  baseline-able output rather than a flood of low-confidence
  findings.

`cppcheck` is the only FOSS option with broad MISRA coverage. Its
MISRA add-on covers ~120 of the 175 directive + rule items in
MISRA C:2012, which is sufficient to demonstrate the *spirit* of
MISRA conformance without the audit-grade traceability of a
commercial tool.

## Decision

* **MISRA enforcement is performed by `cppcheck` only**, via the
  pre-commit hook (`scripts/git/pre-commit`) and the CI workflow.
  The exact invocation is

      cppcheck --enable=warning,style,performance,portability \
               --error-exitcode=1 \
               --suppressions-list=.cppcheck-suppressions \
               --inline-suppr \
               --std=c11 \
               --quiet \
               <files>

  The standard is pinned to **`--std=c11`**, not `--std=c23`. The
  pinned cppcheck (2.13) predates C23 support: the codebase's C23
  typed enums (`enum : uint8_t`) and `[[...]]` attributes raise
  `syntaxError` under a C23 parse, and cppcheck has no `c23` value for
  `--std`. All three invocations use `--std=c11` accordingly:
  `scripts/checks/cppcheck.sh`, `scripts/checks/misra_check.sh`, and
  `scripts/checks/misra_check_inner.sh` (whose inline comment records
  the version limitation). The **consequence** is that any line using
  C23-only syntax raises `syntaxError`; cppcheck recovers and continues
  parsing the rest of the translation unit, so MISRA coverage is the
  *parseable subset* of the tree rather than every line. For the two
  MISRA rules this most affects -- 15.1 (`goto`) and 21.4 (`<setjmp.h>`)
  -- a parse-independent textual backstop
  (`scripts/checks/check_no_goto_setjmp.py`) closes the gap for
  `goto` / `setjmp` / `longjmp` across the whole tree.

* **No commercial MISRA checker is integrated.** The project does
  not maintain LDRA / Parasoft / Coverity result files. If a
  certification authority ever requires audit-grade evidence, a
  one-shot run of a commercial checker will be commissioned then.
* **Documented deviations** live in
  `.cppcheck-suppressions` (mechanical) and
  `docs/qualification/MISRA_DEVIATIONS.md` (formal rationale). The current
  per-file-per-rule population is `.github/misra-baseline.txt`; new deviations
  require a written rationale and machine-checked ownership.
* **Rules cppcheck cannot check** (e.g. MISRA-C:2012 Rule 11.1 on
  function pointers, 21.x on standard-library use) are documented
  as "out of automated scope" in `docs/MISRA.md` and reviewed by
  hand at qualification time.

## Consequences

### Positive

* Anyone cloning the repo can run the same MISRA gate with
  `apt install cppcheck` (or `brew install cppcheck`) -- no
  license server, no auth.
* The hook fails fast, so MISRA regressions are caught at commit
  time rather than at certification time.
* Aligns with the FOSS-only policy already established for the
  rest of the toolchain (clang, clang-format, clang-tidy, gcc,
  cmake, scan-build, libFuzzer).

### Negative

* MISRA coverage is incomplete (~120 of 175 items). A future
  certification effort would need to either (a) commission a
  commercial-tool sweep, or (b) document the un-checked rules as
  "manually reviewed" with reviewer sign-off per rule per file.
* cppcheck's MISRA add-on occasionally lags the official rule
  text after a MISRA amendment. Drift is mitigated by tracking
  the cppcheck release cadence in `docs/MISRA.md`.

### Neutral

* The project does not claim formal MISRA C:2012 *conformance*.
  It claims **MISRA-style enforcement at the FOSS-tool ceiling**
  and documents the gap honestly.

## References

* `CLAUDE.md` -- "Code Style" section.
* `docs/MISRA.md` -- per-rule status and the cppcheck-vs-MISRA gap.
* `.github/misra-baseline.txt` -- machine-readable current
  per-file-per-rule population.
* `docs/qualification/MISRA_DEVIATIONS.md` -- formal deviation register.
* `.cppcheck-suppressions` -- the mechanical suppression list.
* MISRA C:2012 (Guidelines for the use of the C language in
  critical systems, Third Edition, March 2013, including
  Amendments 1-4).
* cppcheck MISRA add-on:
  https://github.com/danmar/cppcheck/blob/main/addons/misra.py
