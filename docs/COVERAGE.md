# Coverage (statement + branch)

This document describes the plain statement + branch coverage flow,
which is layered alongside (NOT instead of) the DO-178C Level B MC/DC
flow described in `docs/MCDC.md`.

## Why two coverage flows?

| Flow                     | Tool          | Goal                              | Speed |
|--------------------------|---------------|-----------------------------------|-------|
| Statement + branch (this)| gcc + gcovr   | Quick regression ratchet          | Fast  |
| MC/DC (`make mcdc`)      | clang-18 + llvm-cov | DO-178C Level B / IEC 61508 SIL 3 audit gold standard | Slow  |

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

## Running locally

```sh
make coverage
```

This wraps `scripts/report/coverage_report.sh`, which:

1. Configures `tests/build-cov/` with `cmake -DRA8_COVERAGE=ON
   -DRA8_MCDC=OFF`.
2. Builds the host test suite with `--coverage -fprofile-arcs
   -ftest-coverage`.
3. Runs every test via ctest.
4. Invokes `gcovr` filtered to first-party `libs/ra8_*` and `src/`.
5. Emits:
   - `build/coverage/index.html` -- per-file annotated source
   - `build/coverage/coverage.xml` -- Cobertura XML (consumed by the gate)
   - `build/coverage/summary.txt` -- text summary

After the report is generated, `make coverage` also runs the gate:

```sh
python3 scripts/checks/check_coverage.py
```

## On macOS

The host test fake uses `MAP_FIXED` below 4 GiB, which arm64
macOS rejects with SIGKILL. The script transparently re-execs
itself inside the project's Linux devcontainer (Ubuntu 24.04 with
gcc 13.3 and gcovr 7.0). Docker (and colima, if installed) is
auto-started.

## Targets

| Goal                       | Today  | Target |
|----------------------------|--------|--------|
| Statement coverage         | 89.9%  | 100%   |
| Branch coverage            | 81.1%  | 100%   |
| MC/DC                      | tracked separately in `docs/MCDC_GAPS.md` | 100% |

100% statement and branch coverage on first-party code is a
hard goal but is feasible -- the existing 227+ test suite already
exercises most code paths. The remaining gaps are:

- Defensive `assert(false)` / "unreachable" branches that exist
  only to satisfy NASA Power of 10 Rule 5 (validation).
- Hardware-only error paths that cannot be triggered from the host
  test harness.

## Ratchet policy

The baseline lives in `.github/coverage-baseline.txt` as two
whitespace-separated numbers:

```
<statement_pct> <branch_pct>
```

`scripts/checks/check_coverage.py` enforces:

- statement coverage MUST NOT drop below baseline (slack 0.5pp)
- branch coverage MUST NOT drop below baseline (slack 0.5pp)

If a PR earns more than 1pp over baseline on both metrics, the
gate prints a `HINT` suggesting the new numbers. Updating the
baseline downward is forbidden; the only acceptable direction is
upward.

`WARN_ONLY_MODE = True` in `check_coverage.py` keeps the gate
in advisory mode for the first few CI runs while we verify the
baseline is reproducible on GitHub Actions. Once stable, flip to
`False` to make the gate blocking.

## CI

`.github/workflows/coverage.yml` runs `make coverage` on every PR
and push to main, uploads the HTML report as a workflow artifact,
and runs the gate.

## See also

- `docs/MCDC.md` -- the DO-178C Level B MC/DC flow
- `docs/MCDC_GAPS.md` -- per-file MC/DC coverage gap list
- `scripts/report/coverage_report.sh` -- coverage report generator
- `scripts/checks/check_coverage.py` -- coverage gate
- `.github/coverage-baseline.txt` -- baseline numbers
