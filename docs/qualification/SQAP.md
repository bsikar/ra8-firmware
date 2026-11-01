# Software Quality Assurance Plan (SQAP)

**Last refreshed**: 2026-08-22 (test inventory and execution evidence refresh).

**Status**: First draft, 2026-05-02. Populated during Phase 7 of
`docs/QUALIFICATION_ROADMAP.md`. Subject to revision after the first
external assessor review.

**DO-178C reference**: Section 11.5 (SQAP content) and Section 8
(Software Quality Assurance Process).
**IEC 61508-3 reference**: Clause 6.2.5 (Software quality assurance).
**ISO 26262-2 reference**: Clause 5 and ISO 26262-8 Clause 5
(Quality management).

**Owner**: Brighton Sikarskie (single developer / maintainer).

---

## 1. SQA organization

### 1.1 Personnel

This is a single-developer project. Brighton Sikarskie is the
developer, the reviewer, and the SQA function. There is **no
separate SQA team** today.

### 1.2 Independence

DO-178C 8.1.b and IEC 61508-1 cl. 8.2.12 require that the SQA
function be independent of the development function. That
independence is **out of scope, permanently**, per
`docs/CERTIFICATION_SCOPE.md` (MIT-licensed personal project; paid
third-party assessor engagement is not pursued). The mitigation,
recorded throughout the planning document family, is:

1. **Automated gates as the primary SQA control.** The gates that
   survive on dev (`clang-format`, `tools/roadmap_stats --check`, the
   `scripts/checks/` suite, and the `regen-drift` CI workflow) execute
   on every change irrespective of authorship. Automation is the
   independent reviewer in lieu of a separate person; the pre-commit
   hook suite and the former `firmware.yml` jobs are removed and are
   re-establishment items.
2. **Manual review as the secondary control.** Where a check cannot
   be automated (architectural review, deviation acceptance, SOUP
   re-review), the reviewer-equals-author posture is recorded as a
   gap.
3. **Independent assessor engagement out of scope, permanently.**
   Engagement of a third-party functional-safety assessor (IEC 61508-1
   cl. 8.2) or a Designated Engineering Representative (DO-178C
   analogue) is **not pursued** per `docs/CERTIFICATION_SCOPE.md`.
   Downstream adopters who require independence must engage their own
   assessor.

### 1.3 Authority

The single developer holds:

- Quality-policy authority (defined by `CLAUDE.md` and
  `docs/STYLE_GUIDE.md`).
- Acceptance authority over individual PRs.
- Stop-work authority (refusing to land a PR with red gates).
- Deviation-acceptance authority (recorded inline in the SRS
  requirement rows; the `MISRA_DEVIATIONS.md` register was removed
  from dev).

When the assessor role is filled the authority for deviation
acceptance and final acceptance transitions to the assessor;
day-to-day per-PR authority remains with the developer.

---

## 2. SQA activities

### 2.1 Process audits

The "process" being audited is the development life cycle defined
by `docs/QUALIFICATION_ROADMAP.md` and `docs/STYLE_GUIDE.md`.
Process-audit evidence is generated automatically by the following
tools, all of which run on every commit and every PR:

| Audit                                | Tool / artifact                                                                  |
|--------------------------------------|----------------------------------------------------------------------------------|
| Roadmap progress audit               | `tools/roadmap_stats --check` (refuses stale ROADMAP summaries)                   |
| Pre-commit gate audit                | suspended: the hook suite was removed from dev; per-commit audits are local (`scripts/checks/`) |
| Coding-standard audit                | `clang-format` (`.clang-format`); clang-tidy and cppcheck are not on dev         |
| MISRA-C 2012 process audit           | suspended: `docs/MISRA.md` and the MISRA gate were removed from dev              |
| World-tag (architecture) audit       | suspended: `check_world_tags.py` was removed from dev                             |
| Obsolete-standards audit             | suspended: `check_obsolete_standards.py` was removed from dev                     |
| MC/DC vector pattern audit on tests  | suspended: `check_mcdc_block.py` was removed from dev                             |
| HUM citation audit                   | suspended: `cite_check.py` was removed from dev                                   |
| Doxygen `@since` audit               | suspended: no tool on dev since the ra8ci gate was removed                        |
| Copyright header audit               | suspended: `check-copyright.py` was removed from dev                              |

A failed gate is the audit finding. The CI log is the audit record;
the corrective-action loop is the developer's response on the same
PR.

### 2.2 Product audits

The "product" is the firmware itself plus its documentation.
Product audits are the periodic refresh of the gap registers:

| Audit                  | Refresh tool / artifact                                                  | Cadence              |
|------------------------|--------------------------------------------------------------------------|----------------------|
| Doxygen completeness   | suspended: `doxy_audit.py` and the Doxygen gate were removed from dev | — |
| MC/DC coverage         | suspended: the `-fcoverage-mcdc` gate and its gap register were removed from dev | — |
| MISRA conformance      | suspended: the MISRA gate, its baseline and `docs/MISRA.md` were removed from dev | — |
| Stack usage            | `scripts/checks/stack_usage_check.py` -> `docs/STACK_USAGE.md` (the `quality::` just wrappers were removed) | Per release |
| SOUP register          | Per-component review under `docs/SOUP/<name>.md`                         | At most 12 months per file |
| Hardware-smoke results | direct bench run (the guarded-workflow document was removed from dev)     | Pre-push or manual |

The refresh cadence is the project's product-audit cadence. A stale
gap register is itself a finding.

### 2.3 Conformance reviews

Per DO-178C 8.2 the SQA function performs a conformance review
before each baseline release. The review confirms:

- The CI on the release commit was green end-to-end.
- MC/DC is currently unmeasured on dev (the gate and its baseline were removed).
- The MISRA deviation register was removed from dev; deviations are
  recorded in the SRS requirement rows.
- The SOUP register entries are within the 12-month re-review
  window.
- Stack-usage evidence is refreshed for the release candidate; Doxygen,
  MC/DC and MISRA evidence is currently unmeasured on dev.

The conformance-review checklist will be added under
`docs/qualification/release/<tag>/conformance.md` at the time of
the first release. No releases exist today.

### 2.4 Transition criteria audits

DO-178C 8.2.b "transition criteria audits" are interpreted in this
project as the per-phase acceptance gates defined in
`docs/QUALIFICATION_ROADMAP.md` Section 3:

- Phase 1 -> 2 and Phase 2 -> 3: MC/DC evidence must be restamped for the
  release; the archived 2026-05 measurements do not qualify the migrated tree.
- Phase 3 -> 4: Doxygen evidence must likewise be regenerated for release.
- Phase 4 -> 5: MISRA audit stable modulo cppcheck-only policy
  (`docs/CERTIFICATION_SCOPE.md`); D-001..D-012 are the active records, with
  findings outside their exact scopes retained as ratchet-held code-change debt.
- Phase 5 -> 6: bidirectional app/test trace remains pending. The retained
  2026-08-22 snapshot measured 693 test sources and 689 registrations per clean
  host OS, with 689/689 passing in the Linux/devcontainer gate in 8.66 s;
  macOS was registration-only because low-address tests require
  Linux/container execution. This dated evidence is not a current-tree census.
- Phase 6 -> 7: the retained 118/118 RA8D2 build, selected 2/2 HIL run, and
  remote-GDB lifecycle result are historical. The matrix is derived by
  `scripts/dev/ra8_apps.py`; a current full-matrix build and full-fleet
  execution remain pending.
- Phase 7 -> close: planning + verification + accomplishment
  document set complete and refresh-stamped.

Each transition is gated on the tool-driven evidence above. The SQA
function (the developer for now) confirms the gate before declaring
a phase complete.

---

## 3. Software life cycle process audits

The audits below run automatically on every commit and every PR,
producing an audit trail without manual SQA intervention.

### 3.1 Per-commit audits

The pre-commit hook suite (`.pre-commit-config.yaml` and the
`pre-commit-checks` CI gate) was removed from dev. The per-commit
enforcement that survives is local and partial: `clang-format`
(`.clang-format`), ROADMAP summary freshness (`tools/roadmap_stats
--check`), and the `scripts/checks/` suite (file size, comment format,
freestanding runtime and allocator isolation, RTOS symbol isolation, SG
offsets, tool versions, lint targets, stack usage, init-order audit).
The removed audits (ASCII scan, C23 and defensive-paren patterns,
clang-tidy, cppcheck, `@since`, copyright/SPDX, HUM citations, world
tags, obsolete standards, MC/DC blocks) currently have no tool on dev;
re-establishing them is a qualification gap, not a silently-passed
check.

### 3.2 Per-PR audits (CI workflow)

`.github/workflows/regen-drift.yml` is the only workflow on dev: it
re-runs the regenerators and fails on drift. The former `firmware.yml`
job suite (ascii, coverage, mcdc, docs/doxygen, cppcheck, build-cross,
unit-tests, pre-commit-checks, coverage-comment) was removed from dev.
Per-PR verification is currently local: `tests/build_tests.sh` +
`tests/run_tests.sh` and the `scripts/checks/` suite. The job table
below is therefore retired with the workflow.

### 3.3 Audit-finding handling

A failed gate is a non-conformance. Resolution path:

1. Developer pushes a fix to the same PR branch.
2. CI re-runs; the fix is verified by the same automated audit.
3. The corrective-action record is the commit and the green CI run.

There is no separate non-conformance ticket workflow; the GitHub PR
review thread serves that purpose.

---

## 4. Software conformity review

Per DO-178C 8.3 a final software conformity review is performed
before the SAS is signed. For this project:

- The **first** conformity review will be performed at the end of
  roadmap Phase 7. A pre-release checklist will be added to
  `docs/qualification/release/<tag>/conformance.md` at that time.
- Until then, conformity is asserted commit-by-commit by the
  automated gates above. No formal conformity-review record exists
  yet because no certification-targeted release exists yet.
- The conformity review is **deferred** -- not skipped -- and is
  explicitly recorded as such here so the gap is visible to a
  future assessor.

---

## 5. SQA records

### 5.1 Authoritative records

| Record                          | Source                                                              | Retention                            |
|---------------------------------|---------------------------------------------------------------------|--------------------------------------|
| Audit trail of all changes      | `git log` on the GitHub remote                                      | Indefinite                           |
| Per-PR review history           | GitHub PR conversation                                              | Indefinite (GitHub default)          |
| CI run logs                     | GitHub Actions run history                                          | 90 days (GitHub default)             |
| Stack-usage report              | `build/stack_usage.csv` (regenerated) + `docs/STACK_USAGE.md` table | Tables versioned, raw rebuilt        |
| SOUP qualification basis        | `docs/SOUP/<name>.md` per component                                 | Versioned in git, indefinite         |
| Per-release audit pack          | `docs/qualification/release/<tag>/` (planned)                       | Versioned in git, indefinite         |

### 5.2 Visibility of audit findings

- Pre-commit failures: visible to the developer immediately at
  commit time; not surfaced upstream because the commit is refused.
- CI failures: visible on the PR conversation as a red gate. The CI
  log holds the diagnostic.
- Refresh-cadence misses (a stale MISRA or MC/DC table): caught by
  the relevant `--check` mode of the audit script (e.g.
  `roadmap_stats --check`) or, for tables not yet wired into the
  hook, by the next per-quarter review.

### 5.3 Long-term storage

The git repository (and its GitHub mirror) is the long-term store.
The qualification document set under `docs/qualification/` is part
of that repository and travels with it for the lifetime of the
project. CI artifacts older than the configured retention windows
are not preserved automatically; for any release that targets
external certification, the release-time audit pack
(`docs/qualification/release/<tag>/`) captures the artifacts that
must outlive the CI window.

---

## 6. References

- `CLAUDE.md` -- coding standard, character-encoding policy,
  no-AI-attribution policy, zero-backward-compatibility policy,
  Doxygen documentation requirements.
- `docs/STYLE_GUIDE.md` -- authoritative human-facing style guide.
- `docs/QUALIFICATION_ROADMAP.md` -- phase plan and gap analysis.
- `docs/qualification/SVP.md` -- verification objectives.
- `docs/qualification/SCMP.md` -- configuration management.
- the SRS requirement rows -- where MISRA deviations are recorded now
  (the `MISRA_DEVIATIONS.md` register was removed from dev).
- `docs/qualification/TOOL_QUALIFICATION.md` -- tool TQL dossier.
- `docs/SOUP/` -- pre-existing software register.
- `.github/workflows/regen-drift.yml` -- the only CI workflow on dev
  (the pre-commit hook suite and the former `firmware.yml` were removed
  from dev; re-establishing them is a qualification gap).
- IEC 61508-3:2010 Clause 6.2.5 and IEC 61508-1:2010 Clause 8.2.
- RTCA DO-178C:2011 Sections 8 and 11.5.
- ISO 26262-2:2018 Clause 5 and ISO 26262-8:2018 Clause 5.
