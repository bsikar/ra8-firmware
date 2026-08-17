# MISRA-C 2012 Compliance

The MISRA-C 2012 audit policy for ra8-firmware: why the project holds itself to
a language subset, what the one available open-source checker can and cannot
see, and how the remaining debt is stopped from growing.

The findings themselves are not in this document. `.github/misra-baseline.txt`
owns the per-file, per-rule counts, and
[`docs/qualification/MISRA_DEVIATIONS.md`](qualification/MISRA_DEVIATIONS.md)
owns the formal disposition of each rule.

## Why MISRA-C 2012

This firmware targets **IEC 61508 SIL 3** and **DO-178C up to Level B**. Both
expect a documented coding standard with mechanical enforcement of
language-subset rules:

* **IEC 61508-3 Annex A.4** -- "Use of language subset" is a Highly Recommended
  technique at SIL 3, and the clause names MISRA-C as the reference subset for C.
* **DO-178C section 11.8 / 6.3** -- the Software Coding Standard must define
  which language constructs are permitted. MISRA-C 2012 is the industry answer
  for C and is cited by FAA AC 20-115D, EASA AMC 20-115D and TC AC 525-006.
* **ISO 26262-6 table 1** -- automotive ASIL C/D adopt the same language-subset
  requirement, with MISRA-C 2012 as the named exemplar.

MISRA-C 2012 categorises every rule as **Mandatory**, **Required** or
**Advisory** (sec. 5.1). For SIL 3 / Level B every mandatory and required rule
must be either obeyed or formally **deviated** under sec. 5.2, which demands a
rationale, a scope, an alternative mitigation, and review sign-off.

## The cppcheck-MISRA limitation

cppcheck is the only open-source MISRA-C 2012 checker. The commercial
alternatives -- LDRA Testbed, Helix QAC, Polyspace, Coverity -- are
**explicitly out of scope** here; see the deviation register's "Tooling policy"
section and [`docs/CERTIFICATION_SCOPE.md`](CERTIFICATION_SCOPE.md). A
downstream party who adopts this codebase for a regulated product is responsible
for procuring their own qualified checker.

cppcheck's MISRA addon implements a subset of the rules: essentially all of the
mandatory ones (mostly syntactic bans on goto, setjmp and dynamic memory, which
are easy to detect), somewhat over half the required ones, and about half the
advisory ones. The uncovered required rules are the ones that need whole-program
type-flow analysis -- the essential-type model -- or interprocedural reasoning;
the standard-library rules are checked syntactically only. Expect roughly two
thirds of the mandatory-plus-required set to be detected. The remaining third is
**accepted as residual risk** under IEC 61508-7 Annex D.7, "achievable assurance
with available tools".

Two further limitations shape how the numbers read:

* **The pinned cppcheck does not parse C23.** This codebase uses typed enums
  (`enum : uint8_t`) and `[[nodiscard]]` / `[[maybe_unused]]` attributes.
  cppcheck raises `syntaxError` on the affected declarations and carries on
  parsing the rest of the translation unit, so coverage is lost on the
  declaration line but retained in the function bodies. That simultaneously
  understates a small number of true violations and inflates the tooling-gap
  rules. Newer cppcheck releases do accept `--std=c23`, but moving the pin means
  a full rebaseline -- finding sets are not comparable across checker versions
  or `--std` modes -- so it is a reviewed decision recorded in the deviation
  register, never a quiet version bump.
* **The rule texts cannot be redistributed** (copyright MISRA Ltd), so the audit
  prints the rule ID (`[misra-c2012-15.5]`) with no accompanying text. A
  licensee of the standard can supply `--rule-texts=<file>` to upgrade the
  diagnostics; that file is deliberately not committed here.

## Running the audit

```sh
make misra           # the audit
make misra-check     # the audit, then the ratchet comparison
make misra-baseline  # the audit, then regenerate the committed baseline
```

The audit covers first-party translation units and excludes
`libs/third_party/`. Per-violation output and the raw checker logs land under
`build/misra/`, regenerated on every run; none of it is committed.

## The ratchet -- the load-bearing mechanism

The audit is enforced in CI as a **ratchet** (#240), not as a clean-run gate.
That distinction is the whole design: it lets a large pre-existing debt burn
down over time while making it impossible to add to.

* The `misra` job re-runs the audit on every push and pull request and compares
  the result against the committed baseline in `.github/misra-baseline.txt`
  using `scripts/checks/misra_ratchet.py`.
* Counts are keyed on **(file, rule)**, not on `file:line`. Line drift from an
  unrelated edit therefore does not churn the baseline. The accepted coarseness
  is that a new violation offset by a simultaneous fix of the same rule in the
  same file nets zero.
* **Any (file, rule) count above the baseline fails the job.** A new finding
  cannot land, however much untouched debt sits beside it in the same file.
* Counts may only shrink. When they do the gate passes and prints a notice to
  regenerate and commit the smaller baseline. The baseline header records the
  cppcheck version that produced it, because a baseline generated by a different
  version is not a baseline.
* There is no pre-commit hook. A full cppcheck pass is far too slow to run per
  commit, so this gate rides CI.

`scripts/checks/check_misra_deviations.py` closes the loop from the other side:
it re-derives every machine-checked claim in the deviation register from the
baseline and the suppression list, so the register cannot drift away from the
measurement it claims to describe.

## Dispositions

Per MISRA-C:2012 sec. 5.2 a finding has exactly three possible outcomes:

1. **Code change** -- modify the source to obey the rule.
2. **Project deviation** -- formal sign-off that the rule is not followed, with
   rationale, scope and mitigation.
3. **Suppression** -- record that the finding is a tool false positive.
   Legitimate only when the violation is provably absent from the code, never
   when MISRA is genuinely violated.

The deviation register holds the current disposition of every rule. The
substantive ones:

| Rule                            | Disposition                      | Rationale |
|---------------------------------|----------------------------------|-----------|
| 15.5 single point of exit       | Project deviation (D-001)        | Single-exit conflicts with NASA Power-of-10 Rule 7 and with the `RA8_RETURN_ON_ERROR` early-return idiom that is the project's default error propagation. Mitigation: at least two pre/post-condition checks per function plus MC/DC, which gives equivalent assurance against missed cleanup paths. |
| 17.3 implicit declaration       | Tooling gap (D-002)              | cppcheck reports an implicit declaration when a header fails to parse on a C23 line and a downstream call therefore looks undeclared. |
| 9.2 initializer braces          | Tooling gap (D-003)              | The C23 `= {}` empty initializer is read as an under-braced aggregate initializer. |
| 12.1 operator precedence        | Partial deviation + code change (D-004) | Implicit precedence accepted for `* /` over `+ -`, unary over binary, member access over any, and postfix call over any. Redundant parentheses added everywhere else; clang-format will not re-flatten them. |
| 8.4 compatible declaration visible | Tooling gap (D-005)           | Every hit traces to `syntaxError` on the `[[nodiscard]]` attribute of the matching public-header prototype, or to a third-party header deliberately excluded from the audit. The cross compiler rejects any real Rule 8.4 violation as a build error, so the source obeys the rule. |

A tooling-gap disposition is not a permanent excuse. Each carries an
early-review trigger that fires when the pinned checker gains the capability it
was missing; at that point the residual findings are re-audited and whatever
survives is reclassified.

## No commercial re-audit

cppcheck-MISRA at roughly two-thirds rule coverage is this project's permanent
MISRA enforcement stance, not an interim one. The project does not seek
certification itself (see [`docs/CERTIFICATION_SCOPE.md`](CERTIFICATION_SCOPE.md));
an adopter who does is responsible for their own qualified-tool re-audit.

## Tooling reference

| Asset | Purpose |
|---|---|
| `scripts/checks/misra_check.sh` | Developer front end for the pinned audit; `--check` also runs the committed ratchet. |
| `scripts/checks/misra_check_inner.sh` | The audit itself. Writes `build/misra/` and prints a per-rule tally. Invoked by `make misra`. |
| `scripts/checks/misra_ratchet.py` | Ratchet comparator; `--update` regenerates the baseline. |
| `scripts/checks/check_misra_deviations.py` | Re-derives the deviation register's machine-checked claims from the baseline and the suppression list. |
| `.github/misra-baseline.txt` | Committed per-file-per-rule counts plus the generating cppcheck version. |
| `.cppcheck-suppressions` | Project-wide suppressions with justification comments. Every `misra-c2012-*` family here must be owned by the deviation register's suppression-ownership list, and that ownership is gated. |
| [`MISRA_GAPS.csv`](MISRA_GAPS.csv) | A capped, hand-trimmed excerpt of an early audit. Kept for shape, not for currency. |
