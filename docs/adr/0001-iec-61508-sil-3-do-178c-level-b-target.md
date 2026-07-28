# ADR-0001: Adopt IEC 61508 SIL 3 / DO-178C Level B as the qualification target

## Status

Accepted -- 2026-01-15. Re-affirmed 2026-05-02.

## Context

`ra8-firmware` is a personal research project that explores what a
hand-written, command-line-only firmware stack for the Renesas RA8D2
looks like when it is held to a real safety bar from day one. There is
no certification body involved and no shipping product, so the project
has the unusual freedom to pick its own qualification target.

The candidate bars considered:

* **None / "ship it"** -- typical for hobby firmware. Rejected: the
  whole point of the project is to learn what the gap actually
  looks like.
* **MISRA C only** -- rejected as insufficient. MISRA constrains
  *style*; it does not require coverage, traceability, or
  documented deviations.
* **IEC 61508 EIL 2 / DO-178C Level C** -- requires statement +
  branch coverage but not MC/DC. Already the bar most "embedded
  with tests" projects implicitly target. Picking this bar would
  not stretch the codebase.
* **IEC 61508 SIL 3 / DO-178C Level B / ISO 26262 ASIL C-D** --
  requires MC/DC (Modified Condition / Decision Coverage),
  documented dynamic-allocation policy, traceable requirements,
  bounded loops, and full deviation justifications. This is the
  highest bar that is achievable for a parts-on-the-bench
  research project without a certification authority in the loop.
* **IEC 61508 EIL 4 / DO-178C Level A** -- requires source-to-object
  traceability of every machine instruction. Rejected as
  unachievable on the GCC + LTO toolchain in use without a
  qualified compiler.

The pre-2011 revision of DO-178 was superseded by DO-178C in December
2011 and is therefore deliberately excluded from this project's
documentation and tests (see
`scripts/checks/check_obsolete_standards.py`, which fails the
pre-commit hook on any new reference to the obsolete revision).

## Decision

The project targets **IEC 61508 SIL 3** as its industry-agnostic
safety bar. **DO-178C Level B** (avionics) and **ISO 26262 ASIL C/D**
(automotive) are treated as equivalent industry-specific derivatives
that the same evidence package satisfies once a certification body is
named.

In practice this means:

1. **MC/DC for every compound boolean decision** in first-party code.
   Each `&&` or `||` requires N+1 test vectors that demonstrate each
   condition independently affects the outcome. Vectors are declared
   in a `@par MC/DC:` block in the test's Doxygen header. Coverage is
   measured with `clang -fcoverage-mcdc` via `make mcdc`.
2. **No dynamic memory after init** (NASA Power-of-10 Rule 3). All
   buffers statically allocated.
3. **All loops have provable upper bounds** (NASA Rule 2). Either a
   constant in `for(...; i < k_x; i++)` or a watchdog-refreshed
   `while(1)` in main control loops.
4. **Functions stay short** (NASA Rule 4 + clang-tidy
   `LineThreshold = 60`).
5. **All return values checked** (NASA Rule 7) via
   `RA8_RETURN_ON_ERROR` or explicit `(void)` cast.
6. **Documented deviations** for every place the code knowingly
   diverges from the standard's defaults (e.g. function pointers
   for DIP, deactivated guards on already-validated invariants).
   These live under `docs/MCDC_DEACTIVATIONS.md` and `docs/SOUP/`.

## Consequences

### Positive

* The project produces real qualification evidence (MC/DC reports,
  stack-usage budgets, SOUP justifications) rather than just
  "tests pass".
* The CI gates (cite_check, world_tags, MC/DC gap regen, stack
  usage strict mode) compose into a tractable per-commit rejection
  rule that prevents the qualification posture from rotting.
* Picking SIL 3 specifically (rather than the highest possible bar)
  keeps the toolchain unconstrained: GCC + LTO + clang-coverage
  is acceptable; a qualified compiler is not required.

### Negative

* Every new compound boolean decision requires a paired test;
  see `scripts/checks/check_new_compound_has_mcdc.py`. This raises
  the cost of a one-line change from "type the change" to "type
  the change + author N+1 vectors".
* Third-party code (`libs/third_party/`) cannot meet the bar by
  source-level re-test, so each direct subdirectory needs a
  written SOUP justification under `docs/SOUP/`.

### Neutral

* The project will never carry a *certificate* (no DER / authority).
  The qualification target is internal evidence quality, not a
  formal sign-off.

## References

* `CLAUDE.md` -- "IEC 61508 SIL 3 / DO-178C Level B Qualification"
  section (the operational restatement of this ADR).
* `docs/MCDC.md` -- how MC/DC is measured.
* `docs/QUALIFICATION_ROADMAP.md` -- per-module qualification status.
* `docs/CERTIFICATION_SCOPE.md` -- what is and is not in scope.
* IEC 61508-3:2010 (Functional safety of electrical/electronic/
  programmable electronic safety-related systems -- Part 3:
  Software requirements).
* DO-178C / ED-12C (Software Considerations in Airborne Systems and
  Equipment Certification, December 2011).
* ISO 26262-6:2018 (Road vehicles -- Functional safety -- Part 6:
  Product development at the software level).
