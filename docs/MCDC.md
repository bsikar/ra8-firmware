# MC/DC Coverage (DO-178C Level B)

This document describes how `ra8-firmware` measures **Modified
Condition/Decision Coverage** (MC/DC) and how to add MC/DC test
vectors for new code.

MC/DC is the structural coverage criterion mandated by **DO-178C Level
B** (Hazardous failure condition) and **DO-178C Table A-7 objective
5**. The infrastructure described here is the foundation for
qualifying portions of this codebase under DO-178C; per-module MC/DC
test vectors are tracked separately.

## Coverage target

**Complete MC/DC for 100% of reachable decision regions.** A decision region
is complete only when llvm-cov reports 100% MC/DC for every condition in that
decision. Deactivated decision regions (DO-178C 6.4.4.3) are exempted from the
gate provided each one carries a documented rationale in
[`docs/MCDC_DEACTIVATIONS.md`](MCDC_DEACTIVATIONS.md). The gate is

```
fully_covered_reachable_decisions / reachable_decisions_total >= 100%
```

where
`reachable_decisions_total = total_decisions - deactivated_decisions`.

This decision-complete percentage is deliberately separate from the
condition-level percentage in llvm-cov's `TOTAL` row. The latter counts
individual MC/DC condition obligations and is reported as an informational
absolute percentage; it is not used as the reachable-decision gate.

`scripts/fix/regen_mcdc_gaps.py` auto-classifies each MC/DC gap as
`deactivated` (defensive guard already enforced upstream) or
`reachable` (still needs a test vector). The gate, the per-decision
catalog, and the deactivation rationale list all derive from the same
live `just quality::local::mcdc` report -- there is no hand-curated allow-list.

Industry mappings: this policy is the IEC 61508-3:2010 7.4.7
"defensive code" exemption and the ISO 26262-6:2018 9.4.5 "deactivated
branches" treatment under different names.

## What is MC/DC?

For a compound boolean decision with `N` conditions, MC/DC requires:

1. **Decision coverage** -- the decision has evaluated to both `true`
   and `false`.
2. **Condition coverage** -- every condition has evaluated to both
   `true` and `false`.
3. **Independence** -- for every condition `Ci`, there is a pair of
   test cases where `Ci` flips and the *decision outcome* also flips,
   while every other condition is held constant.

The third requirement is what distinguishes MC/DC from plain
"condition coverage". For an `N`-condition decision, MC/DC typically
requires `N + 1` test cases (vs. `2^N` for full multi-condition
coverage).

Statement coverage and branch coverage are necessary but not
sufficient: they cannot detect that a condition was masked by
short-circuit evaluation, and they cannot prove that each condition
*independently* drives the outcome.

## Toolchain

| Tool          | Version             | Role                                |
|---------------|---------------------|-------------------------------------|
| `clang`       | >= 18               | Source-based MC/DC instrumentation  |
| `llvm-profdata` | matching $CC      | Merge `.profraw` per-test files     |
| `llvm-cov`    | matching $CC        | Render MC/DC report                 |

The flag combination is:

```
-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc
```

`-fcoverage-mcdc` is what enables MC/DC bookkeeping. The first two
flags are the standard clang source-based coverage flags that MC/DC
piggy-backs on. See LLVM's
"[Source-based Code Coverage](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#mc-dc-instrumentation)"
documentation.

The report driver fails closed unless clang >= 18 and matching LLVM profile
tools are available. Plain gcc 14 `-fcondition-coverage` can be explored only
through an explicit manual CMake configuration; it is **not** MC/DC, is not
DO-178C-compliant, and cannot satisfy this gate.

## How to run

```sh
just quality::local::mcdc
```

This wraps `scripts/report/mcdc_report.sh`, which:

1. Configures `tests/` with `cmake -DRA8_MCDC=ON`.
2. Builds every host test with the MC/DC flag trio.
3. Runs each test binary with `LLVM_PROFILE_FILE` set so each emits
   its own `.profraw`.
4. Merges them via `llvm-profdata merge -sparse`.
5. Renders both a verbose per-file dump
   (`build/mcdc-report/mcdc.txt`) and a numeric summary
   (`build/mcdc-report/summary.txt`).
6. Applies a 100% reachable-decision floor to every represented first-party
   production root: `libs/`, `apps/shared_libs/`, `examples/`, `port/`, and
   `tools/` (override via `RA8_MCDC_THRESHOLD=NN`). Each root also has a
   non-vacuity check.

The existing `just quality::local::test` and `just quality::local::mcdc` flows are untouched --
MC/DC instrumentation is opt-in.

## How to read the report

`build/mcdc-report/summary.txt` is a `llvm-cov report` table with an
extra **MC/DC Coverage** column (added by `--show-mcdc-summary`). A
typical row looks like:

```
Filename                                  Regions   Missed Regions   Cover     ...   MC/DC Conditions   Missed   Cover
libs/ra8_core/src/ra8_log.c                 142       8                94.37%    ...   12                 4        66.67%
```

A line is fully MC/DC-covered when **Missed = 0** in the MC/DC
column. The verbose `build/mcdc-report/mcdc.txt` shows, per decision,
the truth-table rows that have and have not been observed.

## Adding MC/DC test vectors -- worked example

`libs/ra8_core/src/ra8_log.c` contains:

```c
while (value != 0U && i < k_ra8_u32_max_digits) {
  buf[i++] = (char)('0' + (char)(value % (uint32_t)k_ra8_decimal_base));
  value /= (uint32_t)k_ra8_decimal_base;
}
```

This decision has two conditions:

- `C1: value != 0U`
- `C2: i < k_ra8_u32_max_digits`

The decision short-circuits on `C1`, so the MC/DC test set must
exercise:

| Test | C1   | C2   | Decision | Notes                                                 |
|------|------|------|----------|-------------------------------------------------------|
| T1   | F    | -    | F        | C1 false; C2 not evaluated (short-circuit)            |
| T2   | T    | F    | F        | Independence pair for C2: with C1=T, C2 flips outcome |
| T3   | T    | T    | T        | Independence pair for C1: with C2=T, C1 flips outcome |

Three tests cover MC/DC for the loop guard:

- **T1**: log a `uint32_t` value of `0`. The function takes the
  early-return path before the loop, but the loop guard's `C1` is
  still observed `false`.
- **T2**: log a `uint32_t` value with more than
  `k_ra8_u32_max_digits` significant decimal digits (impossible for
  `uint32_t` -- but reachable if the buffer were artificially shrunk
  by adjusting `k_ra8_u32_max_digits` in a test fixture, OR by calling
  the loop in a wrapper that pre-loads `i`). On real `uint32_t` this
  branch is unreachable, which means the MC/DC obligation translates
  to a **deactivated code** justification under DO-178C Section 6.4.4.3
  -- annotate it in the per-module Software Verification Plan rather
  than chasing impossible coverage.
- **T3**: log any non-zero `uint32_t` (e.g. `123U`). The loop runs
  normally and exits when `value` reaches zero before the index cap.

To add the vectors, drop new assertions into
`tests/core/src/test_ra8_log.c` and re-run `just quality::local::mcdc`. The MC/DC column for
`ra8_log.c` should advance as soon as the new tests execute the
required truth-table rows.

### Citing the decision

The pre-commit gate `check_new_compound_has_mcdc.py` requires every new
compound decision to be cited from a `test_mcdc_*` function's
`@par MC/DC:` block, in the form **`path@function`** -- the source path
and the *enclosing function* of the decision, e.g.
`libs/ra8_ui/src/ra8_ui.c@ra8_ui_rect_contains`. The gate resolves a
decision's enclosing function and looks for a citation naming it.

Citing by function (not a line number) is deliberate: unrelated edits
that shift lines never invalidate the citation, and because the form
carries no `:line` it is not flagged by `check_line_citations.py` and
needs no `CITES-OK` escape. Brittle `path:line` anchors are not
accepted.

### How this is enforced in CI: the ratchet

`check_new_compound_has_mcdc.py --staged` is the *local* pre-commit
half, and it is bypassable with `--no-verify`. The blocking CI half is
`scripts/checks/mcdc_compound_ratchet.py --check`, a step of the
`pre-commit-checks` gate.

It scans the whole tree with the same detector, buckets every uncovered
decision by `(file, enclosing function)` -- the granularity a citation
uses -- and compares those counts against
`.github/mcdc-compound-baseline.txt`. **Any increase fails.** So a
newly-added compound decision that arrives without vectors fails the
push, while the pre-existing backlog is tolerated and can only shrink.

The structural citation ratchet covers `libs/`, `port/`,
`apps/shared_libs/`, and the firmware product directories derived by
`lint_targets.firmware_app_dirs()` (currently the e-reader under `apps/`). It
excludes both canonical `third_party` roots, generated code, nested tests,
standalone examples, and host tools. Examples and tools are still subject to
the executed 100% per-file floor described above; they are simply outside this
separate citation-ratchet census.

Adding the previously omitted app-shared scope exposed 1,195 pre-existing
decisions in 586 buckets and zero growth outside `apps/shared_libs/`. That
measurement corrected gate visibility; it was not a coverage regression or a
waiver, and the newly visible rows are frozen by the same no-growth rule.

A ratchet rather than a straight whole-tree check because a delta scan
keys on new source *lines*: with a backlog this size it would fail on a
mere reformat of a pre-existing uncovered decision, which is a cliff,
and a cliff gets bypassed. This is the same shape `tidy_ratchet.py` and
`misra_ratchet.py` use for their measured debts.

Burning one down:

```sh
python3 scripts/checks/mcdc_compound_ratchet.py --list   # what is left
# ... write the test_mcdc_* function and its @par MC/DC: citation ...
python3 scripts/checks/mcdc_compound_ratchet.py --update # lock the gain in
```

`--update` refuses to raise any bucket, so the baseline can only move
downward. Closing this out means the file reaching zero rows and being
deleted -- never regenerated larger. The outstanding count is tracked in
issue #426.

Renaming a function that still carries baselined decisions retires its
row and creates a new one, which reads as growth, so the gate fails and
`--update` refuses. Rename the row by hand, keeping the count identical.
That is deliberate rather than an oversight: automatic rename detection
that guessed wrong would silently absorb a genuinely new uncovered
decision, and a hand edit leaves a reviewable diff.

## Exempted code (SOUP)

DO-178C Section 12.1.4 ("Software of Unknown Pedigree") allows
unmodified third-party libraries to be used without source-level MC/DC
provided their behaviour is verified at the integration boundary.

Everything under `libs/third_party/` or
`apps/shared_libs/third_party/` is excluded from MC/DC instrumentation and
from the `llvm-cov` report; `tests/CMakeLists.txt` is the authority for that
exclusion list. Those components are SOUP under DO-178C, and each carries its
own justification under `docs/SOUP/`.

First-party production files represented in the live report under `libs/`,
`apps/shared_libs/`, `examples/`, `port/`, and `tools/` are **in scope** for
the executed MC/DC floor. Nested tests, generated font tables, build output,
and both canonical third-party roots are excluded.

## Known gaps

- New compound decisions **are** gated: locally by the pre-commit hook
  and in CI by the ratchet above. What remains is burning the baselined
  backlog down (issue #426), module by module rather than in one mass
  fix.
- `ra8_psa_crypto` constant-time paths intentionally evaluate every
  condition (no short-circuit) for side-channel reasons; they are
  documented as "deactivated short-circuit" rather than gated on
  MC/DC.
