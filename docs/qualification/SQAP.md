# Software Quality Assurance Plan (SQAP)

**Last refreshed**: 2026-05-03 (independence + HIL posture re-stated).

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

1. **Automated gates as the primary SQA control.** The pre-commit
   hook (`scripts/git/pre-commit`) and CI workflow
   (`.github/workflows/firmware.yml`) execute identical checks on
   every change irrespective of authorship. Automation is the
   independent reviewer in lieu of a separate person.
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
- Deviation-acceptance authority (recorded inline in
  `docs/qualification/MISRA_DEVIATIONS.md` and similar registers).

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
| Roadmap progress audit               | `scripts/report/roadmap_stats.py --check` (refuses stale ROADMAP summaries)       |
| Pre-commit gate audit                | `scripts/git/pre-commit` exit status; CI mirror in `pre-commit-checks` job       |
| Coding-standard audit                | `clang-format`, `clang-tidy`, `cppcheck`                                         |
| MISRA-C 2012 process audit           | `make misra` quarterly + `docs/MISRA.md` baseline table                          |
| World-tag (architecture) audit       | `scripts/checks/check_world_tags.py`                                              |
| Obsolete-standards audit             | `scripts/checks/check_obsolete_standards.py` (rejects superseded safety-standard references) |
| MC/DC vector pattern audit on tests  | `scripts/checks/check_mcdc_block.py`                                              |
| HUM citation audit                   | `scripts/checks/cite_check.py`                                                    |
| Doxygen `@since` audit               | `scripts/checks/check-since-version.py`                                           |
| Copyright header audit               | `scripts/checks/check-copyright.py`                                               |

A failed gate is the audit finding. The CI log is the audit record;
the corrective-action loop is the developer's response on the same
PR.

### 2.2 Product audits

The "product" is the firmware itself plus its documentation.
Product audits are the periodic refresh of the gap registers:

| Audit                  | Refresh tool / artifact                                                  | Cadence              |
|------------------------|--------------------------------------------------------------------------|----------------------|
| Doxygen completeness   | `scripts/checks/doxy_audit.py` -> `docs/DOXYGEN_GAPS.csv` + `docs/DOXYGEN_GAPS.md` | Per release    |
| MC/DC coverage         | `make mcdc` -> `build/mcdc-report/summary.txt` + `docs/MCDC_GAPS.md`     | Per PR (CI) + per release |
| MISRA conformance      | `make misra` -> `build/misra/results.txt` + `docs/MISRA_GAPS.csv`        | Quarterly            |
| Stack usage            | `make stack-usage` -> `build/stack_usage.csv`                            | Per release          |
| SOUP register          | Per-component review under `docs/SOUP/<name>.md`                         | At most 12 months per file |
| Hardware-smoke results | `make smoke` -> `build/smoke/results.md`                                 | Developer-laptop pre-push (`docs/HIL_DEVELOPER_WORKFLOW.md`) |

The refresh cadence is the project's product-audit cadence. A stale
gap register is itself a finding.

### 2.3 Conformance reviews

Per DO-178C 8.2 the SQA function performs a conformance review
before each baseline release. The review confirms:

- The CI on the release commit was green end-to-end.
- The MC/DC baseline at `.github/mcdc-baseline.txt` was respected.
- The MISRA deviation register
  (`docs/qualification/MISRA_DEVIATIONS.md`) covers every
  outstanding finding.
- The SOUP register entries are within the 12-month re-review
  window.
- The Doxygen, MC/DC, MISRA, and stack-usage gap registers are
  refreshed in the release commit.

The conformance-review checklist will be added under
`docs/qualification/release/<tag>/conformance.md` at the time of
the first release. No releases exist today.

### 2.4 Transition criteria audits

DO-178C 8.2.b "transition criteria audits" are interpreted in this
project as the per-phase acceptance gates defined in
`docs/QUALIFICATION_ROADMAP.md` Section 3:

- Phase 1 -> 2: critical-path MC/DC at 100% (met).
- Phase 2 -> 3: first-party reachable MC/DC at 100% (met.
- Phase 3 -> 4: Doxygen audit clean (met -- 0 functions with gaps).
- Phase 4 -> 5: MISRA audit stable modulo cppcheck-only policy
  (`docs/CERTIFICATION_SCOPE.md`); D-001..D-005 cover the long tail.
- Phase 5 -> 6: every EVM app has at least one host integration test
  (25/26 today).
- Phase 6 -> 7: HIL is developer-laptop pre-push, not a CI gate
  (`docs/HIL_DEVELOPER_WORKFLOW.md`).
- Phase 7 -> close: planning + verification + accomplishment
  document set complete and refresh-stamped.

Each transition is gated on the tool-driven evidence above. The SQA
function (the developer for now) confirms the gate before declaring
a phase complete.

---

## 3. Software life cycle process audits

The audits below run automatically on every commit and every PR,
producing an audit trail without manual SQA intervention.

### 3.1 Per-commit audits (pre-commit hook)

The hook at `scripts/git/pre-commit` runs the following audits in
sequence and refuses the commit on any failure:

1. ASCII-only source-file check.
2. C23 pattern enforcement (no `_Static_assert`, no `= {0}`, no
   `<stdbool.h>`).
3. Defensive-paren on numeric `#define` values.
4. `clang-format` (no diff).
5. `clang-tidy` (no warnings, NASA P10 Rule 4 LineThreshold = 60).
6. `cppcheck` warning/style/performance/portability.
7. `@since` Doxygen tag enforcement on public headers.
8. Copyright + SPDX header enforcement.
9. HUM citation validator (`cite_check.py --warn`).
10. World-tag validator (`check_world_tags.py --warn`).
11. ROADMAP summary freshness (`roadmap_stats.py --check`, strict).
12. Obsolete-standards reference scan (rejects superseded
    safety-standard references, strict).
13. `@par MC/DC:` block on staged tests
    (`check_mcdc_block.py`).

### 3.2 Per-PR audits (CI workflow)

`.github/workflows/firmware.yml` runs the following jobs and blocks
merge on any failure:

| Job                  | Purpose                                                                         |
|----------------------|----------------------------------------------------------------------------------|
| `ascii`              | Repository-wide ASCII scan.                                                     |
| `copyright`          | Repository-wide copyright header scan.                                          |
| `since`              | `@since` tag enforcement on every public header.                                |
| `format`             | `clang-format --check`.                                                         |
| `tidy`               | `clang-tidy --check`.                                                           |
| `unit-tests`         | Host unit tests via ctest.                                                      |
| `coverage`           | `gcovr` 90/90 line+branch gate.                                                 |
| `mcdc`               | clang-18 `-fcoverage-mcdc` gate against `.github/mcdc-baseline.txt`.            |
| `pre-commit-checks`  | Repository-wide mirror of the per-commit hook.                                  |
| `build-discover`     | Enumerates example apps for matrix build.                                       |
| `build-cross`        | Cross-build every example with arm-none-eabi-gcc.                               |
| `docs`               | Doxygen warning gate.                                                           |
| `cppcheck`           | Repository-wide cppcheck.                                                       |
| `coverage-comment`   | PR-only per-file MC/DC delta comment.                                           |

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
| Coverage HTML report            | Uploaded by `coverage` job                                          | 14 days (configured retention)       |
| MC/DC report                    | Uploaded by `mcdc` job                                              | 14 days (configured retention)       |
| MISRA audit baseline            | `docs/MISRA.md` table + `docs/MISRA_GAPS.csv`                       | Versioned in git, indefinite         |
| MC/DC measurement history       | `docs/MCDC.md` measurement-history table                            | Versioned in git, indefinite         |
| Doxygen completeness gap list   | `docs/DOXYGEN_GAPS.csv` and `docs/DOXYGEN_GAPS.md`                  | Versioned in git, indefinite         |
| Stack-usage report              | `build/stack_usage.csv` (regenerated) + `docs/STACK_USAGE.md` table | Tables versioned, raw rebuilt        |
| MISRA deviation register        | `docs/qualification/MISRA_DEVIATIONS.md`                            | Versioned in git, indefinite         |
| SOUP qualification basis        | `docs/SOUP/<name>.md` per component                                 | Versioned in git, indefinite         |
| Per-release audit pack          | `docs/qualification/release/<tag>/` (planned)                       | Versioned in git, indefinite         |

### 5.2 Visibility of audit findings

- Pre-commit failures: visible to the developer immediately at
  commit time; not surfaced upstream because the commit is refused.
- CI failures: visible on the PR conversation as a red gate. The CI
  log holds the diagnostic.
- Refresh-cadence misses (a stale MISRA or MC/DC table): caught by
  the relevant `--check` mode of the audit script (e.g.
  `roadmap_stats.py --check`) or, for tables not yet wired into the
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
- `docs/qualification/MISRA_DEVIATIONS.md` -- MISRA deviation
  register.
- `docs/qualification/TOOL_QUALIFICATION.md` -- tool TQL dossier.
- `docs/SOUP/` -- pre-existing software register.
- `scripts/git/pre-commit` -- authoritative pre-commit gate suite.
- `.github/workflows/firmware.yml` -- authoritative CI gate suite.
- IEC 61508-3:2010 Clause 6.2.5 and IEC 61508-1:2010 Clause 8.2.
- RTCA DO-178C:2011 Sections 8 and 11.5.
- ISO 26262-2:2018 Clause 5 and ISO 26262-8:2018 Clause 5.
