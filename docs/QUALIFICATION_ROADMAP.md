# Qualification Roadmap -- IEC 61508 SIL 3 / DO-178C Level B

**Status**: planning baseline, 2026-05-02
**Owner**: Brighton Sikarskie
**Horizon**: 22 weeks (six-month sprint window) with audit pack
landing at week 22.

This document is the master tracker for taking `ra8d2-firmware` from
its current advisory-quality posture to a state where the codebase,
its tests, and its development records would survive a third-party
qualification audit against the assurance levels named below.

It supersedes ad-hoc references in `docs/ROADMAP.md` and
`docs/PHASE7_ROADMAP.md` for everything related to certification
readiness. Engineering roadmap items remain in those files.

---

## 1. Standards target

The primary anchor is **IEC 61508 SIL 3** (industrial functional
safety, hardware-platform agnostic). Two adjacent standards are
mapped in parallel because the bulk of the supporting evidence
(planning documents, MC/DC coverage, MISRA-C subset, SOUP register)
is shared:

- **IEC 61508-3 SIL 3** -- general-purpose industrial functional
  safety, the standard most likely to consume RA8D2 hardware in the
  field (motor control, programmable logic, safety I/O).
- **DO-178C Level B (DAL B)** -- "Hazardous failure condition" tier,
  used for avionics. Drives the MC/DC structural-coverage
  requirement (Table A-7 objective 5) and the document set described
  in Section 4 below.
- **ISO 26262 ASIL C / ASIL D** -- automotive equivalent. RA8D2 is
  marketed for ASIL B+ ECUs but the subset of evidence required to
  reach ASIL C with this codebase is the same superset as
  SIL 3 / DAL B.

### Why IEC 61508 is the anchor

IEC 61508 is sector-neutral. The more specialised standards
(DO-178C, ISO 26262, IEC 62304, EN 50128) all derive their software
clauses from the same source body of practice (V-model lifecycle,
language subset, structural coverage, configuration management,
SOUP/COTS register, tool qualification). Picking IEC 61508 as the
authoritative baseline lets every artifact double as evidence for
whichever sector this firmware ships into first.

### Side-by-side objective map

| Lifecycle area              | IEC 61508-3 (SIL 3)           | DO-178C (Level B)                     | ISO 26262-6 (ASIL C/D)              | This repo's artifact                                  |
|-----------------------------|-------------------------------|----------------------------------------|--------------------------------------|--------------------------------------------------------|
| Planning                    | Annex B, FSM plan             | Section 4 (PSAC, SDP, SVP, SCMP, SQAP) | Part 6 cl. 5, project planning       | `docs/qualification/PSAC.md` + plan family             |
| Requirements                | 7.2 software safety reqs      | Section 5.1 high-level reqs            | Part 6 cl. 6, sw safety reqs         | `docs/ARCHITECTURE.md`, per-module `@brief` corpus     |
| Architecture / design       | 7.4.3 software architecture   | Section 5.2 low-level design           | Part 6 cl. 7, sw architectural design| `docs/RING_AND_WORLD.md`, `docs/MEMORY_MAP.md`         |
| Coding standard             | Annex A.4 language subset     | Section 11.8 / 6.3 coding standard     | Part 6 cl. 5.4.7 language subset     | `docs/MISRA.md`, `docs/STYLE_GUIDE.md`                 |
| Code verification (review)  | 7.9.2.7 module review         | Section 6.3.4 code reviews             | Part 6 cl. 9 verification            | PR review history + `clang-tidy`/`cppcheck` gates      |
| Structural coverage         | Annex C statement+branch (3); MC/DC strong-recommended for SIL 3 | Section 6.4.4.2 MC/DC at Level B | Part 6 cl. 9.4.5 MC/DC at ASIL C/D | `docs/MCDC.md`, `make mcdc`                            |
| Test cases (req-based)      | 7.4.7 / 7.7 testing           | Section 6.4.2 requirements-based test  | Part 6 cl. 9 test specification      | `tests/test_*.c` (requirements-traced)                 |
| Integration / HW-SW         | 7.5 integration               | Section 6.4.3 integration test         | Part 6 cl. 10 sw integration         | `docs/HARDWARE_BRINGUP.md`, `make smoke`               |
| Configuration management    | 6.2.3 / Annex B.2             | Section 7 SCM process                  | Part 8 cl. 7 sw CM                   | git + signed tags + `docs/qualification/SCMP.md`       |
| Quality assurance           | 6.2.5                         | Section 8 SQA process                  | Part 2 cl. 5 / Part 8 cl. 5          | CI gates + `docs/qualification/SQAP.md`                |
| Tool qualification          | 7.4.4 / Annex D               | Section 12.2 + DO-330                  | Part 8 cl. 11                        | Section 5 below                                        |
| Pre-existing software       | 7.4.2.12 (SOUP)               | Section 12.1.4 + DO-278A               | Part 8 cl. 12 SEooC                  | `docs/SOUP/`                                           |
| Certification liaison       | (independent assessor 8.2)    | Section 9 SOI 1-4 + SAS                | Part 2 cl. 6 confirmation reviews    | `docs/qualification/SAS.md` (week 22)                  |

References for this table: IEC 61508-3:2010 Annex A/B/C; RTCA
DO-178C:2011 Sections 4-12 + Annex A; ISO 26262-6:2018 Clauses 5-11.

---

## 2. Current-state snapshot (2026-05-02)

All numbers below are pulled from the live audit artifacts in this
tree. Where a tool can be re-run cheaply, the cited count is the
output of that tool today. Where the cost is non-trivial (e.g.
running `make mcdc` end-to-end), the cited count is the most recent
committed summary.

### Structural coverage (MC/DC)

- **First-party MC/DC**: 47.74% baseline + roughly 9% gain per
  audited round (per `docs/MCDC_GAPS.md` and the latest
  `build/mcdc-report/summary.txt`). This is well below the 100%
  bar that DO-178C 6.4.4.2 / IEC 61508 Annex C set for SIL 3 / DAL B.
- **Compound decisions in scope**: 609 across 106 first-party files
  (`docs/MCDC_GAPS.md`).
- **Estimated MC/DC vectors needed to close the gap**: 1956 (sum of
  N+1 across all compound decisions).
- **High-priority modules with MC/DC gaps** (from MCDC_GAPS.md
  "high" priority): `ra_usb` (11 decisions / 34 vectors), `ra_sci`
  (8 / 25), `ra_mpu` (7 / 21), `ra_xspi` (4 / 12), `ra_isr`
  (1 / 3), plus `ra_psa_crypto` from the top-10 module table
  (21 / 72).
- Tool basis: clang-18 `-fcoverage-mcdc` + `llvm-profdata`
  + `llvm-cov`, gated by `make mcdc` with default threshold 100%.
  gcc-14 `-fcondition-coverage` is a non-DO-178C fallback (see
  `docs/MCDC.md`).

### Documentation (Doxygen audit)

From `docs/DOXYGEN_GAPS.md`, scope `libs/`, `src/`, `port/`
(third-party excluded):

- **Functions audited**: 2588.
- **Functions with at least one missing tag**: 2557.
- **Total missing-tag instances**: 20328.
- Most-frequent missing tags: `@param` (3432), `@post` (2467),
  `@pre` (2413), `@note` (2382), `@since` (2238).
- Worst three modules: `libs/ra_hal` (1809 functions with gaps),
  `libs/ra_ble_host` (64), `libs/ra_net` (59).

### Coding-standard conformance (MISRA-C 2012)

From `docs/MISRA.md` and `docs/MISRA_GAPS.csv`:

- **Total unique violations**: 1371 (cppcheck-misra advisory pass).
- **Rule 15.5 (single-exit)**: 751 violations -- advisory; clashes
  intentionally with NASA Power-of-10 Rule 7 + the
  `RA_RETURN_ON_ERROR` macro pattern. Disposition is to formally
  deviate per MISRA-C:2012 sec. 5.2.
- **Rule 8.4 (declaration before definition)**: 196 violations.
- **Rule 17.3 (implicit function declaration)**: 170 violations.
- **Rule 12.1 (operator precedence)**: 101 violations -- advisory.
- **Rule 9.2 (initializer braces)**: 35 violations -- required.
- Coverage caveat: cppcheck implements roughly two thirds of
  mandatory + required rules. The remaining one third is
  **accepted as residual risk** per IEC 61508-7 Annex D.7; a
  commercial checker (LDRA, Helix QAC, Polyspace) is **explicitly
  out of scope** for this project. See
  `docs/qualification/MISRA_DEVIATIONS.md` Section "Tooling
  policy" for the final decision and rationale.

### SOUP / third-party register

- **14 components catalogued** under `docs/SOUP/` with one
  Markdown justification each: ThreadX, NetX Duo, FileX, USBX,
  GUIX, LevelX, Mbed TLS, TF-PSA-Crypto, lwIP, Apache NimBLE,
  litehtml, miniz, stb (image + truetype), TinyXML-2.
- Every component has a written qualification basis citing
  IEC 61508-3 sec. 7.4.2.12 and DO-178C sec. 12.1.4.
- Re-review cadence: at most 12 months from each doc's "Last
  review" stamp (see `docs/SOUP/README.md`).

### Test corpus

- **Test files**: 157 under `tests/` (matching `test_*.c`).
- **First-party C/C++ source files**: 143 under `libs/` + `src/`
  (third-party excluded).
- **Test-to-source ratio**: roughly 1.10 : 1 today (target 1 : 1
  minimum maintained). The roadmap-quoted 0.76 : 1 figure is
  superseded by this re-count.

### EVM application matrix

- **EVM-validated apps**: 27 under `examples/ek_ra8d2/` (smoke
  harness target).
- **Unsupported / shelved apps**: 11 under `examples/_unsupported/`.
- HW-in-the-loop coverage: `make smoke` exists and is
  documented in `docs/HARDWARE_BRINGUP.md` but is not yet wired
  into CI on a self-hosted runner.

---

## 3. Gap-closure plan (22 weeks)

Phases are sequential per dependency; Phases 3 and 4 may overlap
once Phase 2 lands. Total span is 22 weeks of focused effort.

### Phase 1 -- Critical-path MC/DC closure (weeks 1-2)

- **Goal**: 100% first-party MC/DC on the modules that sit on the
  hazard path -- ISR dispatch, MPU bring-up, external XSPI,
  USB host/device control, SCI, PSA crypto.
- **Modules**: `ra_isr`, `ra_mpu`, `ra_xspi`, `ra_usb`, `ra_sci`,
  `ra_psa_crypto`.
- **Deliverables**: per-module `tests/test_<module>_mcdc.c`
  following the existing `test_*.c` convention; updated
  `docs/MCDC_GAPS.md` showing zero "high" priority decisions.
- **Acceptance gate**: `make mcdc RA_MCDC_THRESHOLD=100`
  passes on the listed modules.
- **Estimated vectors**: ~167 added (sum of high-priority module
  N+1 totals plus ra_psa_crypto).

### Phase 2 -- Remaining first-party MC/DC (weeks 3-6)

- **Goal**: 95%+ first-party MC/DC across all remaining first-party
  modules. Any uncovered condition is recorded as a deactivated
  condition under DO-178C 6.4.4.3 with a written rationale.
- **Approach**: top-down by module gap count (jpeg_sw, mipi_dsi,
  mipi_phy, fs_fat, flash, rsip, etha, sys_arch, vin).
- **Deliverables**: ~1700 additional MC/DC vectors; updated
  `docs/MCDC_GAPS.md` with deactivated-condition register
  appended.
- **Acceptance gate**: `make mcdc RA_MCDC_THRESHOLD=95` green;
  deactivated-condition list reviewed.

### Phase 3 -- Doxygen pass (weeks 7-10)

- **Goal**: drive the 2557-function gap to zero across the three
  worst modules first, then sweep the long tail.
- **Sub-phases**:
  - 3a (week 7-8): `libs/ra_hal` -- 1809 functions, focused on
    register-driver `@param`/`@retval`/`@pre`/`@post` tags.
  - 3b (week 9): `libs/ra_ble_host` (64) and `libs/ra_net` (59).
  - 3c (week 10): residual modules (`ra_core`, `ra_fs`,
    `port/nimble`, `port/lwip`, `ra_nsc`, `ra_psa_crypto`,
    `ra_ota` and the long tail).
- **Acceptance gate**: `scripts/utils/doxy_audit` reports zero
  functions with gaps; CI gate flips from advisory to blocking.

### Phase 4 -- MISRA-C deviation register (weeks 11-14)

- **Goal**: bring the 1371-finding advisory baseline into a
  formal MISRA-C:2012 sec. 5.2 deviation register, fix everything
  that does not warrant a deviation.
- **Workstreams**:
  - 4a: accept the 751 rule-15.5 (single-exit) violations as
    project-wide deviations; rationale = NASA P10 Rule 7 +
    `RA_RETURN_ON_ERROR` macro idiom; mitigation = MC/DC + cyclo
    bound enforced by clang-tidy LineThreshold = 60.
  - 4b: fix the 170 rule-17.3 (implicit declaration) findings --
    these are real bugs, not deviations.
  - 4c: fix the 196 rule-8.4 (declaration before definition)
    findings via header inclusion or static qualification.
  - 4d: review the 35 rule-9.2 (initializer braces) findings
    file-by-file; deviate or fix.
  - 4e: dispose of the long-tail advisory rules.
- **Deliverable**: `docs/qualification/MISRA_DEVIATIONS.md` with
  one row per accepted deviation (rule number, scope, rationale,
  mitigation, sign-off date).
- **Acceptance gate**: `make misra` returns either a clean run
  or only entries that match the deviation register.

### Phase 5 -- Integration test layer (weeks 15-16)

- **Goal**: every EVM app under `examples/ek_ra8d2/` has at least
  one host-side integration test, in addition to the on-target
  smoke. Today around 15 of the 27 are covered.
- **Deliverable**: 12 new `tests/integration_<app>.c` (or
  equivalent) wiring the app's public surface into the host
  test harness with mock peripherals from `libs/ra_*_pal/`.
- **Acceptance gate**: `make test` exercises every app's
  application-level entry point.

### Phase 6 -- Hardware-in-the-loop coverage (weeks 17-18)

- **Goal**: every PR that touches HAL or example code is gated on a
  hardware smoke run. **Per the 2026-05-02 decision this is a
  developer-local workflow, not a CI runner** -- see
  `docs/HIL_DEVELOPER_WORKFLOW.md` and Section 6 blocker 5 below.
- **Tasks**:
  - Land `scripts/hw_smoke_test.sh` (already in tree).
  - Land `scripts/git/pre-push` opt-in hook gated on
    `git config ra.hw-smoke true`.
  - Document the pre-push checklist in
    `docs/HIL_DEVELOPER_WORKFLOW.md`.
  - Convert `.github/workflows/hardware-smoke.yml` to a doc-only
    workflow that explains the policy.
- **Acceptance gate**: a PR that touches `libs/ra_hal/` or
  `examples/ek_ra8d2/` carries a `<!-- hw-smoke-comment -->`
  block in the PR thread, or an explicit no-board declaration
  from the author.

### Phase 7 -- Formal review packs (weeks 19-22)

- **Goal**: produce the planning + verification + accomplishment
  document set described in Section 4. Each document in its own
  PR for traceable review.
- **Sub-deliverables** (one document per week, with overlap):
  - week 19: PSAC + SDP first drafts.
  - week 20: SVP + SCMP + SQAP first drafts.
  - week 21: SVCP + SVR populated from the actual MC/DC, MISRA,
    smoke, and integration runs.
  - week 22: SAS rolled up from the above; Stage of Involvement
    (SOI) review checklist closed.

**Cumulative schedule**: Phase 1 (2) + Phase 2 (4) + Phase 3 (4)
+ Phase 4 (4) + Phase 5 (2) + Phase 6 (2) + Phase 7 (4) = **22
weeks**.

---

## 4. Deliverable artifacts (audit document set)

All planning documents live under `docs/qualification/`. Each
file is a stub today; Phase 7 fills them in. The structure
mirrors DO-178C Section 11 with IEC 61508-3 cross-references in
each document's preamble.

| Document                                          | Path                                              | DO-178C ref      | IEC 61508-3 ref      |
|---------------------------------------------------|---------------------------------------------------|------------------|----------------------|
| Plan for Software Aspects of Certification (PSAC) | `docs/qualification/PSAC.md`                      | 11.1             | 7.1, Annex B         |
| Software Development Plan (SDP)                   | `docs/qualification/SDP.md`                       | 11.2             | 7.1.2                |
| Software Verification Plan (SVP)                  | `docs/qualification/SVP.md`                       | 11.3             | 7.9                  |
| Software Configuration Management Plan (SCMP)     | `docs/qualification/SCMP.md`                      | 11.4             | 6.2.3                |
| Software Quality Assurance Plan (SQAP)            | `docs/qualification/SQAP.md`                      | 11.5             | 6.2.5                |
| Software Verification Cases & Procedures (SVCP)   | `docs/qualification/SVCP.md`                      | 11.13            | 7.9.2                |
| Software Verification Results (SVR)               | `docs/qualification/SVR.md`                       | 11.14            | 7.9.6                |
| Software Accomplishment Summary (SAS)             | `docs/qualification/SAS.md`                       | 11.20            | 6.2.6 (assessment)   |
| MISRA-C 2012 deviation register                   | `docs/qualification/MISRA_DEVIATIONS.md`          | 11.8             | Annex A.4            |
| Tool qualification dossier                        | `docs/qualification/TOOL_QUALIFICATION.md`        | 12.2 + DO-330    | 7.4.4 + Annex D      |

### PSAC outline (placeholder structure)

1. System overview (RA8D2, EK-RA8D2 board).
2. Software overview (Cortex-M85 secure + non-secure split,
   ring/world tagging).
3. Certification basis: SIL 3 / DAL B / ASIL C-D side-by-side.
4. Software life cycle (V-model, planning -> review -> code ->
   verify -> integrate -> certify).
5. Software life cycle data (full document index).
6. Schedule (this document, Section 3).
7. Additional considerations: SOUP, deactivated code,
   user-modifiable components (none).

### SDP, SVP, SCMP, SQAP, SVCP, SVR, SAS

Stubs created in this commit. Each contains its DO-178C section
header, IEC 61508 cross-reference, and a placeholder body for
Phase 7. They are deliberately minimal so future edits remain
diff-reviewable.

---

## 5. Tooling chain qualification

DO-178C 12.2 + DO-330 classify development tools by Tool
Qualification Level (TQL):

- **TQL-5** -- tool whose output is verified by another process
  (e.g. compiler whose output is exhaustively tested).
- **TQL-1** -- tool whose output is not independently verified
  and whose failure could lead to an undetected error in the
  certified software.

| Tool                      | Role                                  | TQL basis | Compensating verification                                                                          |
|---------------------------|---------------------------------------|-----------|-----------------------------------------------------------------------------------------------------|
| `arm-none-eabi-gcc`       | Cross-compiler -> production object   | TQL-5     | Object code re-verified against requirements via `make smoke` on real hardware (Phase 6).           |
| `clang-18` (host)         | MC/DC instrumentation + host tests    | TQL-5     | Output is test-only; no production code path. Instrumentation re-verified by host tests passing.    |
| `cppcheck` (with misra)   | MISRA-C 2012 advisory checker         | TQL-5     | Findings reviewed manually + `MISRA_DEVIATIONS.md`. Sole MISRA tool: commercial checkers (LDRA /    |
|                           |                                       |           | Polyspace / Helix QAC) are explicitly out of scope per `docs/qualification/MISRA_DEVIATIONS.md`.    |
| `clang-tidy`              | Naming + complexity gate              | TQL-5     | Advisory only; no autofix in CI; line-threshold gate cross-checked against NASA P10 Rule 4.         |
| `clang-format`            | Style enforcement                     | TQL-5     | Idempotent; reviewed by humans on every PR.                                                         |
| `llvm-profdata` / `llvm-cov` | MC/DC measurement                  | TQL-5     | Coverage results spot-checked against hand-traced decisions during Phase 1 and Phase 2.             |
| `cmake` + `make`          | Build orchestrator                    | TQL-5     | Output is the same arm-none-eabi object as a manual invocation; build log archived per CI run.      |
| `JLinkExe`                | Flash + register dump for smoke       | TQL-5     | Read-only with respect to certified bits. Any write step (flash) is verified by post-flash readback.|
| `arm-none-eabi-addr2line` | Smoke-test PC resolution              | TQL-5     | Cross-check against ELF symbol table when classification is ambiguous.                              |
| `python3` (audit scripts) | Doxygen, MC/DC, MISRA gap reports     | TQL-5     | Output reviewed; scripts under `scripts/utils/` carry their own host tests.                         |

No tool in the current chain requires TQL-1 because none of them
emit certified production code without a downstream verification
step. The compiler is the closest call; the mitigation is the
hardware-in-the-loop smoke (Phase 6) plus integration tests
(Phase 5).

---

## 6. Open questions and blockers

### Vendor-blob blockers -- CLOSED 2026-05-02

Both vendor-blob items are **CLOSED**: the project pulls the
blobs **directly from `renesas/fsp` as SOUP** per IEC 61508-3
sec. 7.4.2.12 and DO-178C sec. 12.1.4. No NDA route, no
clean-room rewrite. The runtime stubs in `libs/ra_hal/src/ra_rsip*.c`
and `libs/ra_hal/src/ra_ble_patch.c` remain in place for host unit
tests; the FSP-vendored blobs are dropped into
`libs/third_party/fsp_blobs/` for any hardware build that needs
them.

1. **RSIP-E50D firmware blobs** -- CLOSED. Vendored from
   `renesas/fsp` as SOUP. SOUP entry:
   `docs/SOUP/r_sce_AMC_firmware.md`. Drop-in path:
   `libs/third_party/fsp_blobs/r_sce_AMC/` (procurement plan in
   `libs/third_party/fsp_blobs/README.md`; the actual binary copy
   is a follow-up commit when network and a tagged FSP release are
   available).

2. **Renesas BLE controller patch image** -- CLOSED. Vendored from
   `renesas/fsp` as SOUP. SOUP entry:
   `docs/SOUP/ble_patch_image.md`. Drop-in path:
   `libs/third_party/fsp_blobs/ble_patch/` (same procurement plan
   as above).

### Process blockers

3. **Commercial MISRA checker procurement** -- **CLOSED 2026-05-02:
   never**. `cppcheck` (FOSS) remains the sole MISRA enforcement
   tool. LDRA / Helix QAC / Polyspace / PVS-Studio are explicitly
   out of scope: this is an MIT-licensed, $0 personal/research
   project that will not seek certification (see
   `docs/CERTIFICATION_SCOPE.md`). The ~30 % of MISRA-C:2012 rules
   not covered by `cppcheck` are accepted as residual risk per
   IEC 61508-7 Annex D.7 ("achievable assurance with available
   tools"). Full rationale in
   `docs/qualification/MISRA_DEVIATIONS.md` Section "Tooling
   policy".

4. **Independent assessor selection** (IEC 61508-1 cl. 8.2) --
   **CLOSED 2026-05-02: never**. This MIT-licensed personal /
   research project will not engage a paid third-party assessor
   (TUV SUD / exida / Verocel / SGS-TUV Saar -- typical cost USD
   $30k-$150k per campaign). The project achieves technical
   compliance with IEC 61508 SIL 3 / DO-178C Level B / ISO 26262
   ASIL C-D substantive software requirements but does not pursue
   certification body signoff. Downstream adopters who require an
   assessor are responsible for engaging their own. Full rationale
   in `docs/CERTIFICATION_SCOPE.md` and the
   `docs/qualification/PSAC.md` Section 3.2.1 restatement.

5. **Self-hosted CI runner hardware** -- **CLOSED 2026-05-02:
   never**. The project will not provision a self-hosted GitHub
   Actions runner. Hardware-in-the-loop coverage is achieved via a
   developer-local pre-push workflow:
   `bash scripts/hw_smoke_test.sh` plus the opt-in
   `scripts/git/pre-push` hook (enabled with
   `git config ra.hw-smoke true`). Each contributor with an
   EK-RA8D2 attached runs the harness before push and pastes the
   results into the PR per `docs/HIL_DEVELOPER_WORKFLOW.md`. The
   `.github/workflows/hardware-smoke.yml` workflow is now a
   documentation stub that explains the policy. Rationale:
   incompatible with the MIT-licensed, $0 personal/research scope
   (see `docs/CERTIFICATION_SCOPE.md`).

---

## 7. Cross-references

- `CLAUDE.md` -- coding rules and the existing NASA P10 +
  SOLID-for-C policy that this roadmap layers onto.
- `docs/MCDC.md`, `docs/MCDC_GAPS.md` -- MC/DC infrastructure
  and live gap list driving Phases 1-2.
- `docs/DOXYGEN_GAPS.md` -- input to Phase 3.
- `docs/MISRA.md`, `docs/MISRA_GAPS.csv` -- input to Phase 4.
- `docs/SOUP/` -- pre-existing software register, already
  conformant with IEC 61508-3 sec. 7.4.2.12.
- `docs/HARDWARE_BRINGUP.md` -- input to Phase 6.
- `docs/VENDOR_BLOBS.md` -- blocker register cited in Section 6.
- `docs/RING_AND_WORLD.md` -- architectural baseline for the
  PSAC system-overview section.
