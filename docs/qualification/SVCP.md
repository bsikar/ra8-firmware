# Software Verification Cases & Procedures (SVCP)

**Status**: stub. Populated during Phase 7 of
`docs/QUALIFICATION_ROADMAP.md`.

**DO-178C reference**: Section 11.13.
**IEC 61508-3 reference**: Clause 7.9.2.

## Scope

The concrete test cases, test procedures, and analysis
procedures that implement the SVP.

## Sections to populate

1. Test case catalogue (one row per `tests/test_*.c`).
2. Procedure for executing host tests (`make test`).
3. Procedure for measuring MC/DC (`make mcdc`).
4. Procedure for the on-target smoke (`make smoke`).
5. Procedure for the MISRA pass (`make misra`).
6. Procedure for the Doxygen audit
   (`scripts/utils/doxy_audit`).
7. Requirements traceability matrix (req ID -> test ID).
