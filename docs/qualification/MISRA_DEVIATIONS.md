# MISRA-C 2012 Deviation Register

**Status**: stub. Populated during Phase 4 of
`docs/QUALIFICATION_ROADMAP.md`.

**Procedure**: MISRA-C:2012 sec. 5.2.
**Cross-references**: `docs/MISRA.md`, `docs/MISRA_GAPS.csv`.

## Format

Each deviation entry shall record:

- Rule number (e.g. 15.5).
- Rule category (Mandatory / Required / Advisory).
- Scope (project-wide / module / file / line).
- Rationale (why the rule is impractical here).
- Mitigation (alternative verification that ensures the rule's
  intent is met).
- Reviewer / sign-off date.

## Pre-seeded entries (Phase 4 candidates)

### D-001: Rule 15.5 -- single point of exit

- **Rule**: 15.5 Advisory.
- **Scope**: project-wide.
- **Count**: 751 violations in baseline.
- **Rationale**: project enforces NASA Power-of-10 Rule 7
  (check-and-return on every fallible call) via the
  `RA_RETURN_ON_ERROR` macro. A single-exit refactor would
  require deeply nested `if` blocks that violate Rule 4 (cyclo
  bound) and reduce readability.
- **Mitigation**: clang-tidy LineThreshold = 60 (NASA P10
  Rule 4) plus 100% MC/DC at Phase 1 / 95%+ at Phase 2 prove
  every exit path is reached.
- **Sign-off**: TBD (Phase 4).

(Additional entries appended as Phase 4 progresses.)
