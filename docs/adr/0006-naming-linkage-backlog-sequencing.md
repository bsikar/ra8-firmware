# ADR-0006: Sequencing the naming/linkage legacy backlog

## Status

Proposed -- 2026-09-17. The decision in this ADR is Brighton's to
confirm; nothing here changes a gate.

## Context

Issue #711 filed the largest untracked number in the tree: a
whole-tree AST audit of the naming and linkage contracts, run on
2026-08-16 during the #665 / #708 work, reporting **30,982 findings
(30,974 fatal) across 1,623 translation units**, of which 30,966 are
`ra8_naming`. The inventory was left deliberately un-baselined,
because per `CLAUDE.md` the tree does not fix a gate by editing its
baseline, and a 13k-row grandfather file would be a monument rather
than a burn-down. The consequence is that the debt is invisible: no
gate fails on it, no ratchet counts it, and nothing stops it growing.

The issue asks for a sequencing policy, not a mega-refactor. Four of
its five checkboxes turn out to depend on facts that have moved since
it was filed. This ADR records what those facts are on the current
tree, and puts the sequencing question in a form that can be answered.

### What was verified against `dev` at 013631d

Verified by reading the current tree, not by re-running the audit.
The audit itself was **not** re-run for this ADR: it needs the
`libclang` Python package, which is absent from the environment this
was written in, and `scripts/checks/check_annotations.py` refuses to
start without it. No number below is presented as a fresh measurement.

**1. The "cheapest real win" no longer exists as described.** The
issue's second checkbox expects 21 first-party *library* findings, all
generated protobuf descriptor names, fixable once in the generator
rather than by 21 hand edits. Both protoc-c outputs,
`libs/ra8_c6link/inc/ra8_media_download.pb-c.h` and
`libs/ra8_c6link/src/ra8_media_download.pb-c.c`, are now classified
`generated-source` in `scripts/checks/lint_coverage_rules.py` (lines
167 and 168) and are therefore excluded from the annotation checker by
`is_generated_source()` in `scripts/checks/annot_scope.py`. That
classification landed on 2026-08-18 in commit `2de18993`, **two days
after** the audit that produced the 21 findings. The debt did not get
burnt down by a generator fix; it left the checker's field of view by
exemption. The exemption is defensible on its own terms, and
`scripts/checks/annot_scope.py` states them (protoc-c owns those
identifiers and
regenerated spellings cannot satisfy the project's rules), but it
means the issue's designated first win is void and the remaining
backlog is entirely `tests/`, `examples/`, HAL, FS and reflow legacy.

**2. Nothing in the tree can reproduce the number.** The inventory
exists only as a rescued 7,124,930-byte JSON in one home directory.
No file under version control references
`ra8_linkage_naming_inventory`, and no `just` recipe, Makefile target,
CI workflow or documentation page invokes `--naming-audit`. The
figures in #711 cannot currently be confirmed or refuted by anyone.

**3. The scope moved.** Counting first-party `.c` and `.cpp` under the
six roots the checker walks, excluding vendored trees, the tree holds
**1,511** translation units today against the 1,623 the audit saw:
`tests/` 592, `examples/` 372, `libs/` 361, `tools/` 149, `port/` 37,
`src/` 0. A 7% move in the denominator is enough that the per-tree
split in the issue should not be quoted as current.

**4. The `ra8_parse_integrity` finding no longer matches literally.**
It reported that `port/esp32_c6/src/mdl_service.c` includes the
ESP-IDF certificate-bundle header and that the include does not
resolve. That file no longer includes it. Its seven includes today are
three C standard headers plus
`port/esp32_c6/src/esp_idf_mdl_compat_internal.h`,
`libs/ra8_core/inc/ra8_attributes.h`,
`libs/ra8_c6link/inc/ra8_c6link_mdl_msg.h` and
`port/esp32_c6/inc/ra8_mdl_service.h`. It still calls
`esp_crt_bundle_attach` at line 296, so the underlying condition holds
-- the ESP-IDF half of the C6 port remains outside the checker's
include path and its declarations stay invisible to the call graph --
but the specific include named in the issue is gone.

## Decision

Two questions need Brighton's answer. The options and the constraint
that orders them are recorded here; neither is being chosen in this
ADR.

### Question A -- what comes first

* **Option A1: measure, then decide.** Land a reproducible naming-audit
  target (a `just` recipe over the checker's `--naming-audit --json`
  entry point, plus a durable home for the output) and record the live
  figure on #711 before any policy is set.
* **Option A2: ratchet `libs/` now.** Freeze the first-party library
  half at whatever it measures so the clean side cannot regress, and
  let `tests/` and `examples/` burn down behind it.
* **Option A3: fix the generator and drop the pb-c exemption**, so the
  protobuf descriptors satisfy the naming contract instead of being
  excluded from it.
* **Option A4: whole-tree baseline.** Rejected, and not by preference:
  `CLAUDE.md` forbids fixing a gate by editing its baseline.

**The constraint that decides it:** a ratchet is a promise about a
number, and no number in this issue is currently reproducible by
anyone but the author of the rescued JSON. A1 is therefore a
precondition for A2 and for any honest claim about A3's size, not an
alternative to them. Option A3 additionally cannot be scoped until the
audit runs with the exemption temporarily lifted, because the 21 is a
figure from before the exemption existed.

### Question B -- the policy for `tests/` and `examples/`

At peak these were 22,725 of 30,982 findings, 73% of the backlog, and
they are 64% of the in-scope translation units today. `CLAUDE.md` is
explicit that they are held to the same bar, so "tests are different"
is not on the table; only sequencing is.

* **Option B1: same ratchet, later.** One rule, `libs/` and `tools/`
  ratcheted first, `tests/` and `examples/` joining once the library
  half is at zero.
* **Option B2: per-tree ratchets from day one**, each tree frozen at
  its own measured number, so no tree can regress while any other
  burns down.
* **Option B3: burn down by rule rather than by tree**, taking
  `ra8_di_slot` (8) and `ra8_linkage` (7) to zero tree-wide first,
  since together they are 15 findings and the remaining 30,966 are all
  one rule.

B3 is the only one of the three that produces a closed sub-problem
this quarter, and it is compatible with either B1 or B2 afterwards.

## Consequences

* Until a reproducible target exists, #711's headline numbers stay
  unverifiable and should be quoted with their 2026-08-16 date
  attached, exactly as the issue already does for the peak-versus-later
  pair.
* Whichever option lands, the pb-c exemption should be recorded on the
  issue as the reason the 21-finding win disappeared, so it is not
  re-planned as available work.
* A per-tree ratchet (A2 or B2) adds a second class of baseline file to
  the tree. That is honest where a whole-tree baseline is not, but it
  is still state that has to be regenerated and reviewed, and the
  existing ratchet tooling would need to learn the per-tree split.
* Leaving the decision open has a running cost: nothing detects growth
  in the backlog, so every week the number moves in an unknown
  direction.

## References

* Issue #711 -- naming/linkage legacy backlog.
* `scripts/checks/check_annotations.py` -- the checker, and the
  `--naming-audit --json` entry point that produces the inventory.
* `scripts/checks/annot_scope.py` -- `is_generated_source()`, the
  exact-path generated-source exclusion.
* `scripts/checks/lint_coverage_rules.py` -- `PATH_CLASS`, the registry
  that classifies the two protoc-c outputs.
* `docs/ANNOTATIONS.md` -- the naming triad the audit measures.
* ADR-0002 -- the precedent for recording a deliberately narrowed
  enforcement scope rather than a silent one.
