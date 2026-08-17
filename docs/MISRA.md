# MISRA-C 2012 Compliance

This document captures the ra8-firmware MISRA-C 2012 audit baseline,
the open-source tooling gap, and the staged plan for closing the
identified gaps. The audit is enforced in CI as a **ratchet** (issue
#240): any NEW finding relative to the committed baseline
(`.github/misra-baseline.txt`) fails the `misra` CI job, while the
existing debt burns down without blocking unrelated work.

The committed per-file-per-rule inventory is
`.github/misra-baseline.txt`; [`MISRA_GAPS.csv`](MISRA_GAPS.csv) is a
capped, hand-trimmed excerpt of the 2026-05-02 audit.

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

cppcheck's MISRA addon (`misra.py`, shipped in the install's addons
directory -- `misra_check_inner.sh` locates it per platform)
implements a subset of the MISRA-C 2012 rules. From the upstream addon's own checker table the
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
required rules**. The remaining one third is **accepted as residual
risk** under IEC 61508-7 Annex D.7 ("achievable assurance with
available tools"). Commercial MISRA checkers (LDRA / Helix QAC /
Polyspace / PVS-Studio) are **explicitly out of scope** for this
project -- see `docs/qualification/MISRA_DEVIATIONS.md` Section
"Tooling policy" and `docs/CERTIFICATION_SCOPE.md`. Any downstream
party who adopts this codebase for a regulated product is responsible
for procuring their own qualified checker.

A second important limitation: the pinned cppcheck (2.13.0, run at
`--std=c11`) does not parse C23 syntax. This codebase uses C23 typed
enums (`enum : uint8_t`) and `[[nodiscard]]` / `[[maybe_unused]]`
attributes. cppcheck raises `syntaxError` on the affected declarations
and continues parsing the rest of the translation unit, so we lose
MISRA coverage on the declaration line itself but retain coverage on
the function bodies, and the baseline understates a small number of
true violations while over-reporting the tooling-gap rules.

**Measured 2026-08-15: cppcheck 2.21.0 accepts `--std=c23` and parses
those attributes**, so the early-review trigger on the C23 tooling-gap
deviations has fired. Adopting it means moving the pin *and* a full
rebaseline -- finding sets are not comparable across versions or `--std`
modes -- so the audit deliberately stays on 2.13.0 until that review
lands. See `docs/qualification/MISRA_DEVIATIONS.md` D-002 and D-005.

A third limitation: cppcheck cannot redistribute the MISRA-C 2012 rule
texts (copyright MISRA Ltd). The audit therefore prints the rule ID
(e.g. `[misra-c2012-15.5]`) without the official rule text. A licensee
of MISRA-C 2012 can supply `--rule-texts=<file>` to upgrade the
diagnostics; that file is intentionally not committed to this repo.

## Audit results

The first audit was run on 2026-05-02 against `libs/`, `src/` (since
dissolved into `libs/ra8_secure_app/`), and
`port/` (excluding `libs/third_party/`). Run it yourself with:

```sh
make misra
# or directly
bash scripts/checks/misra_check_inner.sh
```

At that 2026-05-02 audit:

* Source files scanned: 158 `.c` translation units (plus headers).
* Total unique violations: **1271** (was 1371 at first scan; the 101
  misra-c2012-12.1 advisory hits were closed under D-004 by per-line
  suppression entries in `.cppcheck-suppressions`. Those line anchors
  have since decayed and the instances are ratchet-held again -- see
  `docs/qualification/MISRA_DEVIATIONS.md` D-004's reconciliation).
* Output: `build/misra/results.txt` (TSV, regenerated per run);
  `docs/MISRA_GAPS.csv` froze a capped excerpt (1000 rows + tail
  summary) of that audit and has only been hand-trimmed since.

The current enforced number lives in `.github/misra-baseline.txt` (see
its `# total findings:` header line) and is much larger than the
2026-05 figure: the audited tree has more than tripled in translation
units (`tools/` joined the scan roots 2026-08-13), and the CI-pinned
cppcheck 2.13 (vs the 2.20 used for the first audit) parses none of
the C23 syntax, inflating the tooling-gap rule counts. The two numbers
are not comparable; the ratchet only compares like-for-like on the
pinned toolchain, and `scripts/checks/check_misra_deviations.py` keeps
the deviation register's derived numbers matching the baseline.

### Top 5 violated rules (2026-05-02 audit)

| Rank | Rule              | Count | Category | Topic |
|-----:|-------------------|------:|----------|-------|
| 1 | misra-c2012-15.5  | 751 | Advisory | A function should have a single point of exit at the end |
| 2 | misra-c2012-8.4   | 196 | Required | A compatible declaration shall be visible when an object/function with external linkage is defined |
| 3 | misra-c2012-17.3  | 170 | Mandatory | A function shall not be declared implicitly |
| 4 | misra-c2012-12.1  | 101 | Advisory | The precedence of operators within expressions should be made explicit |
| 5 | misra-c2012-9.2   |  35 | Required | The initializer for an aggregate or union shall be enclosed in braces |

(The rule-category column is from the MISRA-C 2012 published rule
tables; rule texts are paraphrased to stay within the licence.)

### Reading the top 5 (2026-05-02 counts)

Current per-rule numbers live in the deviation register's
machine-checked index.

* **Rule 15.5 (single-exit) -- 751 violations.** Advisory only.
  Triggered by every `if (err != k_ra8_ok) return err;` early-return
  pattern, which is the project's default error-propagation idiom and
  is enforced by the `RA8_RETURN_ON_ERROR` macro. Closing this rule
  outright would require restructuring nearly every function in the
  codebase. The plan (below) is a project-wide deviation under
  MISRA-C:2012 sec. 5.2.
* **Rule 8.4 (declaration before definition) -- 196 violations.**
  Required. Initially triaged as a real bug-class. Per the
  investigation recorded in
  [`docs/qualification/MISRA_DEVIATIONS.md`](qualification/MISRA_DEVIATIONS.md)
  D-005, every hit is a cppcheck-2.20 false positive caused by the
  `[[nodiscard]]` C23 attribute on the matching public-header
  prototype (or, for the `port/` translation units, by the
  third-party header that owns the prototype being intentionally
  excluded from the audit). The cross compiler rejects any real
  Rule 8.4 violation as a build error, so the source obeys the
  rule. Reclassified as Tooling gap.
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
  gap; revisit if the pin moves to a C23-parsing cppcheck.

## Gap-closure plan

Per MISRA-C:2012 sec. 5.2 each finding has three possible dispositions:

1. **Code change** -- modify source to obey the rule.
2. **Project deviation** -- formal sign-off that the rule is not
   followed, with rationale, scope, and mitigation.
3. **cppcheck suppression** -- record that the finding is a tooling
   false positive (only legitimate when the violation is provably
   absent in the code, not when MISRA is genuinely violated).

The triage below tracks that decision per top-violated rule.

| Rule              | Count (2026-05-02) | Disposition       | Rationale |
|-------------------|------:|-------------------|-----------|
| misra-c2012-15.5  | 751 | Project deviation (D-001) | Single-exit conflicts with NASA Power-of-10 Rule 7 (check every return value) and with the project's `RA8_RETURN_ON_ERROR` macro. Mitigation: NASA Rule 5 enforces >= 2 pre/post-condition assertions per function plus 100% MC/DC at Phase 1, which provides equivalent assurance against missed cleanup paths. |
| misra-c2012-8.4   | 196 | Tooling gap (D-005)        | Caused by cppcheck-2.20 syntaxError on the C23 `[[nodiscard]]` attribute on public-header prototypes (and by intentional exclusion of `libs/third_party/` headers for the `port/` files). Authoritative check is arm-none-eabi-gcc `-Wmissing-prototypes -Werror`. Re-audit if the pin moves to a C23-parsing cppcheck (trigger FIRED 2026-08-15). |
| misra-c2012-17.3  | 170 | Tooling gap (D-002)        | Re-audit if the pin moves to a C23-parsing cppcheck (2.21.0 ships one; trigger FIRED 2026-08-15). Track residual count and reclassify whatever remains. |
| misra-c2012-12.1  | 101 | Partial deviation + Code change (D-004) | Accept implicit precedence for `* /` over `+ -`, unary over binary, member-access over any, postfix call over any. Add redundant parentheses everywhere else. clang-format will not re-flatten them. |
| misra-c2012-9.2   |  35 | Tooling gap (D-003)        | Caused by C23 `= {}` empty-aggregate initializers. Re-audit if the pin moves to a C23-parsing cppcheck (trigger FIRED 2026-08-15). |
| misra-c2012-13.3, 8.9, 18.4, 10.8, 17.8, 17.7, 7.3, 21.x | 90 | Mixed | Per-finding triage in the next audit pass. |

### Workflow

1. **CI ratchet gate (issue #240).** The `misra` job in
   `.github/workflows/firmware.yml` re-runs the audit on every push /
   PR and fails when any per-file-per-rule finding count rises above
   the committed baseline (`.github/misra-baseline.txt`, compared by
   `scripts/checks/misra_ratchet.py`). Counts are keyed on
   `(file, rule)` rather than raw `file:line` findings so ordinary
   line drift does not churn the baseline; the accepted coarseness is
   that a new violation offset by a simultaneous fix of the same rule
   in the same file nets zero. Findings can only shrink: when they
   do, the gate passes and prints a notice to regenerate + commit the
   smaller baseline (`make misra-baseline`, run on the CI-pinned
   cppcheck -- the baseline header records the generating version).
   There is still no pre-commit hook: a full cppcheck pass is too
   slow per-commit, so the gate rides CI.
2. **Burn-down.** Reduce violations during housekeeping sprints, then
   `make misra-baseline` + commit to lock each tranche in.
3. **No commercial-tool re-audit.** cppcheck-MISRA at ~two-thirds
   rule coverage is the project's permanent MISRA enforcement
   stance. The uncovered rules are accepted as residual risk per
   IEC 61508-7 Annex D.7. This project will not seek certification
   itself (see `docs/CERTIFICATION_SCOPE.md`); a downstream
   adopter who pursues certification is responsible for their own
   commercial-tool re-audit.

## Tooling reference

| Asset                                | Purpose |
|--------------------------------------|---------|
| `scripts/checks/misra_check_inner.sh`       | This audit. Generates `build/misra/results.txt` and prints per-rule tally. Invoked by `make misra`. |
| `scripts/checks/misra_ratchet.py`     | Ratchet comparator: fails on any (file, rule) count above `.github/misra-baseline.txt`; `--update` regenerates the baseline. Invoked by `make misra-check` / `make misra-baseline` and the `misra` CI job. |
| `.github/misra-baseline.txt`         | Committed per-file-per-rule finding counts + the generating cppcheck version. |
| `scripts/checks/misra_check.sh`             | Developer front end for the same pinned audit; `--check` also runs the committed ratchet. |
| `scripts/checks/check_misra_deviations.py` | Re-derives every machine-checked claim in `docs/qualification/MISRA_DEVIATIONS.md` from the baseline and the suppression list; runs in the `misra` gate. |
| `.cppcheck-suppressions`             | Project-wide suppressions with justification comments. Every `misra-c2012-*` rule family here must be owned by the deviation register's "Suppression ownership" list (gated). |
| `docs/MISRA_GAPS.csv`                | Capped per-violation excerpt of the 2026-05-02 audit (1000 rows + tail summary), hand-trimmed since. |
| `build/misra/results.txt`            | Full TSV per-violation list (regenerated each audit). |
| `build/misra/raw.txt`                | Raw cppcheck stderr. |
| `build/misra/misra-raw.txt`          | Raw misra.py stdout. |
