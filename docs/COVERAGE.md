# Coverage (statement + branch)

This document describes the plain statement + branch coverage flow,
which is layered alongside (NOT instead of) the DO-178C Level B MC/DC
flow described in `docs/MCDC.md`.

## Why two coverage flows?

| Flow                     | Tool          | Goal                              | Speed |
|--------------------------|---------------|-----------------------------------|-------|
| Statement + branch (this)| gcc + gcovr   | Quick regression ratchet          | Fast  |
| MC/DC (`just quality::local::mcdc`)      | clang + llvm-cov | DO-178C Level B / IEC 61508 SIL 3 audit gold standard | Slow  |

In theory MC/DC subsumes branch coverage, so once 100% MC/DC is
achieved, statement and branch coverage are guaranteed to be 100%
as well. In practice we keep both because:

1. **Speed.** A full MC/DC pass takes several minutes; gcovr is much
   faster and is a reasonable per-PR sanity check.
2. **Independent baselines.** The two flows can drift -- a refactor
   that adds new compound boolean decisions can leave statement
   coverage at 100% while MC/DC drops. Separate ratchets catch
   either regression.
3. **Tooling fall-back.** If clang's MC/DC instrumentation breaks on a
   future compiler upgrade, we still have a working baseline measure
   from gcov.

## Coverage hierarchy

```
Statement  <  Branch  <  Decision  <  Condition  <  MC/DC  <  Multiple-Condition
   |           |            |            |             |              |
  100%       100%         100%         100%          100%          2^N coverage
                                                  (DO-178C Level B)  (overkill)
```

- **Statement (line) coverage** -- every executable source line is
  hit at least once. Easiest to achieve, weakest guarantee.
- **Branch coverage** -- every basic-block branch edge (true and
  false sides of every if/while/for/switch) is exercised.
- **MC/DC** -- for every compound boolean decision (`a && b`,
  `c || d`, ...), each individual condition is shown to
  independently affect the outcome. Required by DO-178C Level B,
  IEC 61508 SIL 3, ISO 26262 ASIL C/D.


## One census, one baseline, one bar

There is **one** statement+branch policy for the whole first-party codebase.
Every `.c` / `.cc` / `.cpp` under `libs/`, `port/`, `tools/`, `apps/` and
`examples/` is enrolled in it, and no tier gets a softer bar. It replaced
three overlapping regimes -- an aggregate `gcovr --fail-under-line 90
--fail-under-branch 80` plus a narrow reusable-code per-file line floor, a
second full coverage build ratcheted against a two-number project-wide
baseline, and an mdl-specific per-file ratchet -- which between them built the
same translation units twice and still left most of the tree unmentioned by
any policy.

The census is derived from `git ls-files` via
`scripts/checks/lint_targets.py`, so a directory added tomorrow is enrolled the
day it lands. Only three things are subtracted, and each is subtracted
elsewhere first:

- vendored SOUP and generated tables (`libs/third_party/`,
  `apps/shared_libs/third_party/`, `port/threadx/`, `libs/ra8_fonts/`, and
  `tools/vela/generated/`);
- the individually registered generated sources in
  `scripts/checks/lint_coverage_rules.py`;
- test sources -- a file under a `tests/` directory at any depth is the
  instrument, not the thing measured.

Headers carry no row: inline code in a header is measured through the TUs that
include it.

## Row kinds

`.github/tree-coverage-baseline.txt` carries exactly one row per census unit,
TAB-separated and sorted by path. It is emitted by the checker; never hand-edit
it.

```
<file>	MEASURED	<line-covered>	<line-total>	<branch-covered>	<branch-total>
<file>	UNMEASURED	<reason-class>
```

**MEASURED** freezes what the unit measures today. Uncovered debt may not grow
and the ratio may not fall, so a unit at 100% keeps 100% and a unit at 41%
burns down toward the 90% line / 80% branch floor instead of sliding. An
improvement is welcome and silent; `--update` is how it gets frozen in, and it
only ever tightens. A unit with **no row at all** is new and must enter at the
full 90/80 floor -- historical debt is not something a new file can inherit.

**UNMEASURED** is explicit, so nothing is silently absent. The reason is one of
four machine-derived classes, re-derived from the tree on every run so a row
cannot be hand-written into something friendlier:

| Class | Meaning |
|---|---|
| `firmware-composition` | Only ever cross-compiled into an image (`examples/`, the firmware products under `apps/`). No host process, no exit status. |
| `platform-cross-only`  | Platform code (`libs/`, `port/`) no host coverage build compiles: board boot code, RTOS/USB stack ports, drivers with no host double. |
| `hosted-no-coverage-build` | Host tool or product code whose CMake project is not wired into a measurement project. Pure debt -- the fix is to add the project. |
| `compiled-not-executed` | A measurement build compiled it and no test executed it, so gcov wrote a `.gcno` and never a `.gcda`. Usually a static-archive member no test binary pulls in. |

Gaining measurement is **one-way**: a unit that starts producing execution data
must move to MEASURED and can never move back.

## Running locally

```sh
just quality::local::coverage
```

That runs `scripts/report/tree_coverage.sh` (which MEASURES) and then
`scripts/checks/check_tree_coverage.py` (which JUDGES). Keeping the two apart
is what keeps the tree at one policy surface.

The producer builds every project in `tree_coverage_model.PROJECTS`
separately -- the host test suite and mdl form today -- runs each
under ctest, reports each into its own gcovr trace, and then **merges the
traces**. The merge is the point, not an optimisation: the mdl core is
compiled by both builds, so a single sweep over one build tree reports
whichever half it happened to see (`mdl_config.c` measures 77.3% from the host
suite alone and 90.1% from the union). One row per unit means one number per
unit.

Outputs land under `build/tree-coverage/`:

| Path | What |
|---|---|
| `summary.json` | the merged per-file summary the checker reads |
| `traces/<project>.json` | per-project gcovr trace (the merge input) |
| `summaries/<project>.json` | per-project summary (its non-vacuity floor) |
| `<project>/` | the CMake build tree, incl. `compile_commands.json` |
| `html/index.html`, `summary.txt` | human-readable forms |

Re-freezing after you have improved something:

```sh
python3 scripts/checks/check_tree_coverage.py --update
```

`--update` refuses to write over a real regression, so it can only tighten.

## What keeps the gate honest

- **Non-vacuity floors per root.** A checker that enumerates nothing reports a
  clean tree because it looked at nothing. Each census root has its own floor,
  so a collapse confined to a single root still fails; a tree-wide total would
  stay healthy while `tools/` silently dropped to zero.
- **Per-project floors.** Each measurement project's own report must carry a
  minimum number of census units, so a build that silently stopped
  instrumenting cannot merge cleanly into a healthy-looking total.
- **A scope guard.** Any listfile declaring `option(RA8_COVERAGE ...)` must be
  claimed by a measurement project. A new measurable product cannot land and
  then never be measured.
- **A both-directions `--selftest`**, run by the gate before the scan.

## On macOS

The host test fake uses `MAP_FIXED` below 4 GiB, which arm64 macOS rejects with
SIGKILL, and the counts are gcc-14-specific. Run the gate through the
containerised suite (`just quality::devcontainer::gate coverage-tree`) rather than natively.

## Targets

| Goal | Current authority | Target |
|---|---|---|
| Census units measured | `scripts/checks/check_tree_coverage.py` classifies every row in `.github/tree-coverage-baseline.txt` | every host-reachable unit |
| MC/DC | tracked separately in `docs/MCDC_GAPS.md` | 100% |

The two largest unmeasured populations are `examples/` (firmware compositions,
covered instead by the emulator matrix and the HIL suite) and `tools/`
(host-executable, and therefore real debt: those projects have ctest suites but
no coverage instrumentation wired up yet).

## CI

`.github/workflows/coverage.yml` runs `just quality::local::gate coverage-tree`
on every push to `main`/`dev` and every PR to either, and uploads the merged
HTML report and summary as workflow artifacts.

## See also

- `docs/MCDC.md` -- the DO-178C Level B MC/DC flow
- `docs/MCDC_GAPS.md` -- per-file MC/DC coverage gap list
- `scripts/report/tree_coverage.sh` -- the measurement
- `scripts/checks/check_tree_coverage.py` -- the policy
- `scripts/checks/tree_coverage_model.py` -- the census and its reason classes
- `.github/tree-coverage-baseline.txt` -- the one baseline
