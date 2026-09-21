# Architectural Decision Records

This directory captures the major architectural decisions made in
`ra8-firmware`. Each ADR is a short, immutable document that
records:

* **Status** -- Proposed / Accepted / Superseded.
* **Context** -- the forces that made the decision necessary.
* **Decision** -- what was actually chosen.
* **Consequences** -- the trade-offs the decision locks in.

ADRs are numbered sequentially and are never deleted. A decision
that is later overturned is recorded by adding a new ADR with
status `Supersedes ADR-NNNN`; the original keeps its number and
its history.

The format follows Michael Nygard's original ADR template:
https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions

## Index

| ID  | Title                                                                                              | Status   |
|-----|----------------------------------------------------------------------------------------------------|----------|
| [0001](./0001-iec-61508-sil-3-do-178c-level-b-target.md) | Adopt IEC 61508 SIL 3 / DO-178C Level B as the qualification target | Accepted |
| [0002](./0002-cppcheck-only-misra-enforcement.md)        | cppcheck-only MISRA enforcement under a FOSS-only budget            | Accepted |
| [0003](./0003-test-only-internal-headers.md)             | Test-only access to internal symbols via `<module>_internal.h`      | Accepted |
| [0004](./0004-deactivated-condition-policy.md)           | DO-178C 6.4.4.3 deactivated-condition policy with regen-script auto-classification | Accepted |

## Authoring a new ADR

1. Pick the next free number.
2. Copy the structure of `0001-...md` (Status / Context / Decision /
   Consequences / References).
3. Add a row to the index table above.
4. Commit the ADR alone (do not bundle with code changes -- the
   ADR should land before or with the first code change that
   relies on it, but as its own commit so the decision history
   is reviewable in isolation).
