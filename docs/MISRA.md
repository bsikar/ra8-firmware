# MISRA-C 2012 Compliance

The MISRA-C 2012 audit policy for ra8-firmware: why the project holds itself to
a language subset, what the one available open-source checker can and cannot
see, and how the remaining debt is stopped from growing.

The findings themselves are not in this document. `.github/misra-baseline.txt`
owns the per-file, per-rule counts, and
[`docs/qualification/MISRA_DEVIATIONS.md`](qualification/MISRA_DEVIATIONS.md)
owns the formal deviation and tooling-gap records.

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

### Pinned C23 empty-initializer compatibility correction

cppcheck 2.13's bundled Rule 9 MISRA add-on predates C23 6.7.10. Its empty-brace
branch marks every `= {}` aggregate as under-braced, even though an empty
initializer contains no initializer clause that could omit a nested brace.
The audit therefore verifies the exact cppcheck 2.13.0 bytes for the MISRA
driver, its Rule 9 helper, their Python data dependency, and the POSIX library
model. It copies those four upstream assets into a disposable build directory,
verifies the repository-owned patch, applies its one-predicate correction, and
verifies the patched bytes before running them. The installed add-on and library
model are never changed. Any tool-version, dependency, source-addon, library-model,
patch, or patched-output drift fails closed.

The correction was measured across all 390 translation-unit dumps represented
by a Rule 9.2 baseline row. The stock addon reported 2,107 unique file-and-line
findings; the corrected addon reported five. A mechanical whole-initializer
classifier traced all 2,102 removed sites to an actual empty `{}` construct,
with zero additions and zero unexplained removals. The five non-empty,
under-braced findings remained. The selftest independently exercises
top-level, nested, designated-member, and array empty initializers. Each must
remain quiet for Rules 9.2 and 9.4, while under-braced and excess-brace
aggregates still raise Rule 9.2 and a duplicate designated member still raises
Rule 9.4. It also proves that addon, dependency, and patch drift are rejected.

The same staged authority supplies cppcheck's POSIX declaration model to
`port/posix/src/fw_if_fs_posix_common.c` only. Cppcheck 2.13 does not retain
the hosted system-header declarations in its ordinary C11 dump, which creates
four false Rule 17.3 findings. A global POSIX model also changes unrelated
type inference, so the audit replaces only that translation unit's ordinary
dump with a fresh dump using the digest-pinned, absolute staged `posix.cfg`
path. The selftest proves the common exclusions, suppressions, include roots,
and parser options are identical in both passes; exactly four Rule 17.3
findings disappear, the existing Rule 21.1 finding remains, and no other
focused diagnostic changes.

The final pinned audit and its derived qualification records freeze the measured
result. Relative to the 22,097-finding baseline, the C23 compatibility correction
removes 2,102 modeled false positives, the targeted POSIX model removes four
false Rule 17.3 findings, and reviewed source fixes remove 53 genuine findings.
The committed 19,938-finding baseline records the complete 2,159-finding
reduction with zero bucket growth.

## Running the audit

```sh
just quality::local::misra                 # the audit
just quality::local::misra 1               # the audit, then ratchet comparison
just quality::local::misra_baseline        # audit, then regenerate baseline
```

The audit covers first-party translation units under `libs/`, `port/`,
`tools/`, and `apps/`, and excludes `libs/third_party/`. The repository-root
`tests/` and `examples/` trees are outside this scan, while app-local tests
under `apps/` are in scope with their product code. Per-violation output and
the raw checker logs land under `build/misra/`, regenerated on every run; none
of it is committed.

### 2026-08-22 source-layout reconciliation

The `src/`/`inc/` migration moved 116 test translation units from the
repository-root `tests/` tree into their owning `apps/**/tests/` trees. That is
a deliberate audit-scope expansion: app-local tests are product code for this
policy and remain in scope. They were not excluded to make the gate pass.

The migration audit first rewrote only the old baseline paths and verified that
the mapped baseline still contained exactly 20,976 findings in 3,011
file/rule rows. Comparing the pinned 2.13.0 scan against that count-preserving
map separated the growth into two populations:

* 251 file/rule rows and 1,580 findings came solely from those 116 translation
  units crossing from the excluded root-test scope into `apps/`. This is the
  only population accepted as source-layout migration debt. Before the final
  baseline was generated, ordinary source fixes removed one Rule 15.5 finding
  from each of three reflow tests. Exact C23-parser suppressions then removed 77
  spurious findings in three migrated test rows, leaving 1,500 findings in 248
  rows in the final pinned baseline.
* 23 file/rule rows and 103 findings were new or had genuinely grown. They were
  dispositioned independently: source changes removed 10 findings in 9 rows,
  evidence-backed C23-parser suppressions removed 57 false-positive findings in
  10 rows, and the remaining 36 findings in 4 rows are genuine early-return /
  dependency-injection idioms already accepted by D-001 and D-010.

Final frozen refactors exposed seven additional file/rule buckets containing 16
findings of those same documented C23-parser false-positive classes. Their exact
file/rule suppressions also burned down 84 already-baselined Rule 9.2 findings.
Two real Rule 15.4 findings introduced by coverage seams were fixed in source;
the Alphabet CLI split transferred nine D-001 findings from `main.c` to its
private helper without changing the Rule 15.5 population.

The final pinned baseline therefore includes those 1,500 scope-expansion
findings and the separately dispositioned D-001/D-010 additions; it does not
label the latter as migration debt. Any simultaneous finding reductions remain
ordinary ratchet burn-down and are retained when the baseline is regenerated.

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

The deviation register owns the formal deviations and tooling-gap records.
Rules outside those records remain ratchet-held **Code change** debt; an index
row's population is not blanket acceptance of every finding under that rule.
The substantive records include:

| Rule                            | Disposition                      | Rationale |
|---------------------------------|----------------------------------|-----------|
| 15.5 single point of exit       | Project deviation (D-001)        | Single-exit conflicts with NASA Power-of-10 Rule 7 and with the `RA8_RETURN_ON_ERROR` early-return idiom that is the project's default error propagation. Mitigation: at least two pre/post-condition checks per function plus MC/DC, which gives equivalent assurance against missed cleanup paths. |
| 17.3 implicit declaration       | Tooling gap (D-002)              | cppcheck reports an implicit declaration when a header fails to parse on a C23 line and a downstream call therefore looks undeclared. |
| 9.2 initializer braces          | Tooling gap (D-003)              | The C23 `= {}` empty initializer is read as an under-braced aggregate initializer. |
| 12.1 operator precedence        | Partial deviation + code change (D-004) | Implicit precedence accepted for `* /` over `+ -`, unary over binary, member access over any, and postfix call over any. Redundant parentheses added everywhere else; clang-format will not re-flatten them. |
| 8.4 compatible declaration visible | Tooling gap (D-005)           | Every hit traces to `syntaxError` on the `[[nodiscard]]` attribute of the matching public-header prototype, or to a third-party header deliberately excluded from the audit. The cross compiler rejects any real Rule 8.4 violation as a build error, so the source obeys the rule. |
| 11.6 pointer/integer conversion | Narrow project deviation (D-011) | The XZ caller-workspace installer converts one `void*` value to `uintptr_t` solely to reject an address that cannot satisfy the decoder arena's alignment contract. No integer is converted back to a pointer. |
| 21.1 reserved identifiers       | Narrow project deviation (D-012) | A guarded first-party XZ porting macro preserves the exact `__always_inline` spelling consumed by byte-identical upstream SOUP. Other Rule 21.1 findings are not accepted by this record. |

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
| `just quality::local::gate misra` | Registered CI entry point; runs the pinned audit, deviation-integrity checks, and ratchet comparison. |
| `scripts/checks/misra_check.sh` | Developer front end for the pinned audit; `--check` also runs the committed ratchet. |
| `scripts/checks/misra_check_inner.sh` | The audit itself. Writes `build/misra/` and prints a per-rule tally. Invoked by `just quality::local::misra`. |
| `scripts/checks/misra_ratchet.py` | Ratchet comparator; `--update` regenerates the baseline. |
| `scripts/checks/check_misra_deviations.py` | Re-derives the deviation register's machine-checked claims from the baseline and the suppression list. |
| `.github/misra-baseline.txt` | Committed per-file-per-rule counts plus the generating cppcheck version. |
| `.cppcheck-suppressions` | Project-wide suppressions with justification comments. Every `misra-c2012-*` family here must be owned by the deviation register's suppression-ownership list, and that ownership is gated. |
