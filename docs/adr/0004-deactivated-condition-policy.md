# ADR-0004: DO-178C 6.4.4.3 deactivated-condition policy with regen-script auto-classification

## Status

Accepted -- 2026-04-08.

## Context

The MC/DC requirement (ADR-0001) treats every compound boolean
decision in first-party code as a coverage obligation. In practice,
several classes of compound decisions are *intentionally
unreachable* under any execution that satisfies the rest of the
firmware's invariants:

* **Defensive guards on already-validated invariants.** A peripheral
  driver receives a port-and-pin pair from a higher layer that has
  already validated the pin against the package map. The driver
  re-validates anyway as a defence-in-depth measure, but the second
  validation cannot fail in practice because the first validation
  already rejected every illegal pin.
* **Hardware-error branches that the silicon cannot exhibit.** A
  status-register read returns a value the silicon's reference
  manual lists as "reserved -- never produced". The branch exists
  to fail closed if a future silicon revision changes that.
* **Integer-overflow branches on values bounded by an enum.** A
  helper takes a `uint16_t` count whose value is always a
  `k_*` enum constant <= 4096. The "count > UINT16_MAX / 2" branch
  is mathematically unreachable.

DO-178C section **6.4.4.3 ("Deactivated Code")** explicitly permits
this pattern: defensive code may exist in the executable image
without coverage *if* it is documented as "deactivated by design",
the deactivation rationale is recorded, and the code is re-evaluated
whenever its invariants change.

The risk if this is handled informally:

* Coverage % drifts downward as defensive guards accumulate.
* CI gates either reject every defensive guard (forcing authors to
  delete safety code) or whitelist them silently (eroding the bar).
* When an invariant changes, no one remembers which guards depend
  on it.

The candidates considered:

* **Forbid defensive guards entirely.** Rejected: deletes
  safety-relevant code to make a coverage report cleaner. Wrong
  trade.
* **Accept all coverage gaps without classification.** Rejected:
  indistinguishable from "we forgot to write the test".
* **Manual deactivation list maintained by hand.** Rejected:
  drifts immediately, no traceability to the source line, no
  re-evaluation trigger.
* **Auto-classified deactivation list, re-generated on demand,
  reviewed at commit.** Chosen.

## Decision

* The project maintains a **machine-generated deactivation list**
  at `docs/MCDC_DEACTIVATIONS.md` (human-readable) +
  `docs/MCDC_GAPS.csv` (machine-readable).
* The regen script `scripts/fix/regen_mcdc_gaps.py`
  walks every gap reported by `clang -fcoverage-mcdc`, classifies
  each one against a fixed taxonomy:

  | Class                         | Meaning                                                                 | Action                                          |
  |-------------------------------|-------------------------------------------------------------------------|-------------------------------------------------|
  | `defensive-revalidation`      | Guard re-checks a precondition already validated by the caller.         | Deactivated per DO-178C 6.4.4.3.                |
  | `hardware-reserved-branch`    | Branch fires only on silicon states the reference manual marks reserved.| Deactivated per DO-178C 6.4.4.3.                |
  | `bounded-by-enum`             | Branch is unreachable given the input type's enum constraint.           | Deactivated per DO-178C 6.4.4.3.                |
  | `unreachable-by-invariant`    | Branch contradicts a static_assert / enum invariant.                    | Deactivated per DO-178C 6.4.4.3.                |
  | `MISSING-TEST`                | None of the above. The author owes vectors.                             | Counts toward the open MC/DC gap.               |

* **The regen script is the source of truth.** It is not edited
  by hand. Authors who add a defensive guard run

      python3 scripts/fix/regen_mcdc_gaps.py

  and inspect the diff. Any new entry classified
  `MISSING-TEST` requires the author to either (a) write the
  vector, or (b) annotate the source with a `// MCDC-DEACTIVATED:
  <class> -- <rationale>` comment that the regen script will
  pick up on the next pass.
* **The pre-commit hook re-runs the regen check** (in `--check`
  mode) and rejects commits that leave the docs out of sync with
  the source-tree annotations.
* **Re-evaluation trigger.** When a deactivation's underlying
  invariant changes (e.g. the validating layer is removed, the
  silicon revision is updated), the matching `// MCDC-DEACTIVATED:`
  comment must be deleted in the same commit. The regen script
  then reclassifies the now-reachable branch as `MISSING-TEST`,
  forcing authors to add real vectors before the commit lands.

## Consequences

### Positive

* The project can keep defence-in-depth guards without paying a
  coverage tax for code that is provably unreachable.
* Every deactivation is traceable to a source comment with a
  written rationale and a class -- this is exactly the evidence
  DO-178C 6.4.4.3 requires.
* A reviewer scanning the regen-script diff sees, in one place,
  every change in the deactivation set introduced by a commit.
* MC/DC % stays meaningful: it is the ratio of *covered
  reachable* decisions, not the ratio of *covered total*
  decisions.

### Negative

* Authors must learn a new comment convention
  (`// MCDC-DEACTIVATED: <class> -- <rationale>`) and the
  classifier taxonomy. The taxonomy is documented at the top of
  `regen_mcdc_gaps.py` and in `docs/MCDC.md`.
* Mis-classification is possible. Mitigation: the class names
  are verbose and the per-line rationale is mandatory, so a
  reviewer can spot a mis-applied class on the diff.
* The deactivation set is large enough (currently dozens of
  entries) that no single reviewer holds it all in their head.
  This is acceptable -- the regen script makes the *delta*
  reviewable, which is what matters.

### Neutral

* The taxonomy is extensible. Adding a new class is a script
  change plus a paragraph in `docs/MCDC.md`. The existing
  classes are not assumed to be final.

## References

* DO-178C / ED-12C, section 6.4.4.3 ("Deactivated Code").
* `docs/MCDC.md` -- MC/DC measurement and the deactivation taxonomy.
* `docs/MCDC_DEACTIVATIONS.md` -- generated list of deactivated
  decisions (do not edit by hand).
* `docs/MCDC_GAPS.md`, `docs/MCDC_GAPS.csv` -- generated open-gap
  reports.
* `scripts/fix/regen_mcdc_gaps.py` -- the regen + classifier.
* ADR-0001 -- the qualification target that creates this
  obligation in the first place.
* ADR-0003 -- the test-seam pattern that lets MC/DC vectors
  reach the helpers where most defensive guards live.
