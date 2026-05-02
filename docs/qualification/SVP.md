# Software Verification Plan (SVP)

**Status**: stub. Populated during Phase 7 of
`docs/QUALIFICATION_ROADMAP.md`.

**DO-178C reference**: Section 11.3.
**IEC 61508-3 reference**: Clause 7.9.

## Scope

Defines how the firmware is verified: reviews, analyses, test,
structural coverage, and the link between each verification
activity and its DO-178C / IEC 61508-3 objective.

## Sections to populate

1. Verification methods (review, analysis, test).
2. Independence of verification (reviewer != author).
3. Requirements-based test strategy.
4. Structural coverage strategy: MC/DC at Level B per
   `docs/MCDC.md`.
5. Verification of verification (test coverage of test code).
6. Re-verification strategy (regression on every PR).
7. Tool support and tool-qualification cross-reference
   (Section 5 of `docs/QUALIFICATION_ROADMAP.md`).
