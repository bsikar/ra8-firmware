# MISRA-C 2012 Compliance

This document captures the ra8d2-firmware MISRA-C 2012 audit baseline,
the open-source tooling gap, and the staged plan for closing the
identified gaps. The audit is intentionally **advisory** at this stage --
no MISRA finding currently blocks a commit.

For per-violation detail, see [`MISRA_GAPS.csv`](MISRA_GAPS.csv).

## Why MISRA-C 2012

This firmware targets the safety-critical assurance envelope used by
Renesas RA8 reference applications: **IEC 61508 SIL 3** (industrial
functional safety) and **DO-178C up to DAL B** (avionics software
considerations).

Both standards expect a documented coding standard with mechanical
enforcement of language-subset rules:

* **IEC 61508-3 Annex A.4** -- "Use of language subset" is a Highly
  Recommended technique at SIL 3. The clause names MISRA-C as the
  reference subset for C99/C11 codebases.
* **DO-178C section 11.8 / 6.3** -- the Software Coding Standard must
  define which language constructs are permitted; MISRA-C 2012 is the
  industry-standard answer for C and is explicitly cited by FAA AC
  20-115D, EASA AMC 20-115D, and TC AC 525-006.
* **ISO 26262-6 table 1** -- automotive ASIL C/D adopt the same
  language-subset requirement; MISRA-C 2012 is the named exemplar.

MISRA-C 2012 categorises every rule as **Mandatory**, **Required**, or
**Advisory** (see MISRA-C:2012 sec. 5.1). For SIL 3 / DAL B all
mandatory and required rules must be either obeyed or formally
**deviated** under MISRA-C:2012 sec. 5.2 (deviation procedure: rationale,
scope, alternative mitigation, review sign-off).

## The cppcheck-MISRA limitation

cppcheck is the only open-source MISRA-C 2012 checker available; the
commercial alternatives -- LDRA Testbed, PRQA QA-C / Helix QAC,
Polyspace Bug Finder, Coverity -- are out of scope for this project.

cppcheck's MISRA addon (`misra.py`, shipped under
`/opt/homebrew/share/Cppcheck/addons/`) implements a subset of the
MISRA-C 2012 rules. From the upstream addon's own checker table the
coverage is roughly:

* **Mandatory rules** (10 total): ~10 covered (most are syntactic
  bans on goto / setjmp / dynamic memory and are easy to detect).
* **Required rules** (~110): ~60 covered. The uncovered required
  rules are mostly the ones that need full whole-program type-flow
  analysis (e.g. essential-type model rules 10.1 -- 10.8 are only
  partially flagged) or interprocedural reasoning (rules 21.x on
  the standard library are checked syntactically only).
* **Advisory rules** (~50): ~25 covered.

So expect cppcheck to detect **roughly two thirds of the mandatory +
required rules** -- a useful coverage floor, but not a substitute for a
qualified commercial tool when the project enters formal SIL 3 / DAL B
certification.

A second important limitation: cppcheck 2.20 does not yet support
`--std=c23`. This codebase uses C23 typed enums (`enum : uint8_t`) and
`[[nodiscard]]` / `[[maybe_unused]]` attributes. cppcheck raises
`syntaxError` on the affected declarations and continues parsing the
rest of the translation unit, so we lose MISRA coverage on the
declaration line itself but retain coverage on the function bodies.
This will resolve when cppcheck ships C23 support; until then the
advisory baseline understates a small number of true violations.

A third limitation: cppcheck cannot redistribute the MISRA-C 2012 rule
texts (copyright MISRA Ltd). The audit therefore prints the rule ID
(e.g. `[misra-c2012-15.5]`) without the official rule text. A licensee
of MISRA-C 2012 can supply `--rule-texts=<file>` to upgrade the
diagnostics; that file is intentionally not committed to this repo.

## Audit results

The first audit was run on 2026-05-02 against `libs/`, `src/`, and
`port/` (excluding `libs/third_party/`). Run it yourself with:

```sh
make misra
# or directly
bash scripts/utils/misra_check.sh
```

* Source files scanned: 158 `.c` translation units (plus headers).
* Total unique violations: **1371**.
* Output: `build/misra/results.txt` (TSV) and
  `docs/MISRA_GAPS.csv` (capped at 1000 rows + tail summary).

### Top 5 violated rules

| Rank | Rule              | Count | Category | Topic |
|-----:|-------------------|------:|----------|-------|
| 1 | misra-c2012-15.5  | 751 | Advisory | A function should have a single point of exit at the end |
| 2 | misra-c2012-8.4   | 196 | Required | A compatible declaration shall be visible when an object/function with external linkage is defined |
| 3 | misra-c2012-17.3  | 170 | Mandatory | A function shall not be declared implicitly |
| 4 | misra-c2012-12.1  | 101 | Advisory | The precedence of operators within expressions should be made explicit |
| 5 | misra-c2012-9.2   |  35 | Required | The initializer for an aggregate or union shall be enclosed in braces |

(The rule-category column is from the MISRA-C 2012 published rule
tables; rule texts are paraphrased to stay within the licence.)

### Reading the top 5

* **Rule 15.5 (single-exit) -- 751 violations.** Advisory only.
  Triggered by every `if (err != k_ra_ok) return err;` early-return
  pattern, which is the project's default error-propagation idiom and
  is enforced by the `RA_RETURN_ON_ERROR` macro. Closing this rule
  outright would require restructuring nearly every function in the
  codebase. The plan (below) is a project-wide deviation under
  MISRA-C:2012 sec. 5.2.
* **Rule 8.4 (declaration before definition) -- 196 violations.**
  Required. Real bug-class -- catches non-static functions defined
  without a prior `extern` declaration in a header. Likely an actual
  hygiene gap; will be fixed in code, not deviated.
* **Rule 17.3 (implicit function declaration) -- 170 violations.**
  Mandatory. cppcheck's "implicit declaration" trips when a header
  fails to parse (e.g. on a C23 typed-enum line) and a downstream
  function call therefore looks undeclared. After cppcheck adopts
  C23, this count is expected to drop substantially. Treat as
  tooling gap until verified.
* **Rule 12.1 (operator precedence) -- 101 violations.** Advisory.
  Genuine readability fix; close in code by adding redundant
  parentheses.
* **Rule 9.2 (initializer braces) -- 35 violations.** Required. The
  C23 `= {}` empty-initializer (allowed by CLAUDE.md) is currently
  read as an under-braced aggregate initializer. Likely tooling
  gap; revisit after cppcheck-C23.

## Gap-closure plan

Per MISRA-C:2012 sec. 5.2 each finding has three possible dispositions:

1. **Code change** -- modify source to obey the rule.
2. **Project deviation** -- formal sign-off that the rule is not
   followed, with rationale, scope, and mitigation.
3. **cppcheck suppression** -- record that the finding is a tooling
   false positive (only legitimate when the violation is provably
   absent in the code, not when MISRA is genuinely violated).

The triage below tracks that decision per top-violated rule.

| Rule              | Count | Disposition       | Rationale |
|-------------------|------:|-------------------|-----------|
| misra-c2012-15.5  | 751 | Project deviation | Single-exit conflicts with NASA Power-of-10 Rule 7 (check every return value) and with the project's `RA_RETURN_ON_ERROR` macro. Mitigation: NASA Rule 5 enforces >= 2 pre/post-condition assertions per function, which provides equivalent assurance against missed cleanup paths. |
| misra-c2012-8.4   | 196 | Code change       | Add missing `extern` declarations / move definitions behind their existing public headers. |
| misra-c2012-17.3  | 170 | Tooling gap       | Re-audit after cppcheck adds `--std=c23`. Track residual count and reclassify whatever remains. |
| misra-c2012-12.1  | 101 | Code change       | Mechanical fix: add redundant parentheses; clang-format will not re-flatten them. |
| misra-c2012-9.2   |  35 | Tooling gap       | Caused by C23 `= {}` empty-aggregate initializers. Re-audit after cppcheck-C23. |
| misra-c2012-13.3, 8.9, 18.4, 10.8, 17.8, 17.7, 7.3, 21.x | 90 | Mixed | Per-finding triage in the next audit pass. |

### Workflow

1. **No pre-commit gate yet.** The existing pre-commit suite already
   runs ASCII / format / clang-tidy / cppcheck (without MISRA addon)
   / no-dynamic-alloc / world-tags / since-version / obsolete-
   standards. Adding a 1371-finding MISRA gate would block every
   commit. The MISRA addon is wired only via `make misra`.
2. **Quarterly audit cadence.** Re-run `make misra` quarterly,
   compare against the pinned baseline (`build/misra/results.txt`),
   and burn down violations during housekeeping sprints.
3. **Pre-commit gate flips on once the count is under 100.**
   `scripts/utils/misra_check.sh` already supports a baseline-pinned
   `--check` mode (the existing `scripts/misra_check.sh` follows the
   same pattern).
4. **Commercial-tool re-audit before any certification claim.**
   cppcheck-MISRA at ~two-thirds rule coverage is sufficient for
   internal hygiene but not for IEC 61508 / DO-178C credit. Final
   compliance evidence will require LDRA, Polyspace, or QAC.

## Tooling reference

| Asset                                | Purpose |
|--------------------------------------|---------|
| `scripts/utils/misra_check.sh`       | This audit. Generates `build/misra/results.txt` and prints per-rule tally. Invoked by `make misra`. |
| `scripts/misra_check.sh`             | Older baseline-gated MISRA-C 2023 wrapper -- complementary, not redundant. |
| `.cppcheck-suppressions`             | Project-wide suppressions; new MISRA deviations will be added here with justification comments per the existing convention. |
| `docs/MISRA_GAPS.csv`                | Capped per-violation list (1000 rows + tail summary). |
| `build/misra/results.txt`            | Full TSV per-violation list (regenerated each audit). |
| `build/misra/raw.txt`                | Raw cppcheck stderr. |
| `build/misra/misra-raw.txt`          | Raw misra.py stdout. |
