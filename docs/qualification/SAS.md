# Software Accomplishment Summary (SAS)

**Document ID**: ra8d2-sas-001
**Version**: 0.1 (first draft, Phase 7 of `docs/QUALIFICATION_ROADMAP.md`).
**Date**: 2026-05-02.
**Author**: Brighton Sikarskie.
**DO-178C reference**: Section 11.20 (Software Accomplishment Summary).
**IEC 61508-3 reference**: Clause 6.2.6 (Functional safety
assessment summary).
**ISO 26262-6 reference**: Clause 11 (Confirmation review).

## Scope

Roll-up document presented to the certification authority or the
independent assessor at SOI-4 / final assessment. Summarises what
was planned, what was done, and the evidence supporting the claim
that the planned process was followed. This first draft is
populated against the HEAD `402253ef` baseline; per-section detail
references the live audit artifacts in this tree.

## 1. System overview

`ra8d2-firmware` is bare-metal firmware for the Renesas RA8D2 MCU
group, targeting the EK-RA8D2 evaluation kit (Renesas part number
`968-K7EKA8D2S01001BE`). The MCU is a dual-core
`R7KA8D2KFLCAC` (Arm Cortex-M85 @ 1 GHz primary +
Cortex-M33 @ 250 MHz secondary) with 1 MB on-chip MRAM, 2 MB SRAM
with ECC, an Octo-SPI port to 64 MB external NOR flash, and a
parallel-RGB port to a 1024 x 600 TFT plus an OV5640 5 MP camera.
The firmware exercises the chip's HAL surface from a single
hand-written register-level driver tree (no Renesas FSP source in
the repo; FSP is reference material only). It supports a TrustZone
secure / non-secure split, hosts an ARM-validated SOUP RTOS
(ThreadX) and a SOUP networking stack (NetX Duo / lwIP), and
includes 26 EVM-tier example applications and 11 shelved /
unsupported applications.

## 2. Software identification

| Item                | Value                                                        |
|---------------------|--------------------------------------------------------------|
| Product             | ra8d2-firmware                                               |
| Repository          | https://github.com/bsikar/ra8d2-firmware                     |
| Baseline commit     | `402253efae5b0a6742b37eb14e6224f339d1cfbf` (2026-05-02)      |
| Target MCU          | Renesas R7KA8D2KFLCAC (RA8D2 group)                          |
| Target board        | EK-RA8D2 v1 (Renesas 968-K7EKA8D2S01001BE)                   |
| Primary core        | Arm Cortex-M85 @ 1 GHz, with Helium / MVE                    |
| Secondary core      | Arm Cortex-M33 @ 250 MHz                                     |
| Code memory         | 1 MB MRAM (non-volatile, on-chip)                            |
| System RAM          | 2 MB SRAM with ECC                                           |
| External memory     | 64 MB Octo-SPI NOR (IS25LX512M-JHLE), 64 MB SDRAM            |
| Cross toolchain     | ARM GNU Toolchain `arm-none-eabi-gcc` (qualified per         |
|                     | `docs/qualification/TOOL_QUALIFICATION.md` Section 1)        |
| Host toolchain      | `gcc-14` and `clang-18`                                      |
| Build orchestrator  | `cmake` + `make`                                             |
| RTOS                | None at firmware level; SOUP `ThreadX` available per app      |
| Debug probe         | On-board SEGGER J-Link OB (SN 1086567198 on bench EVM)       |

## 3. Software life cycle compliance

The software life cycle is the V-model defined in
`docs/qualification/SDP.md` (Software Development Plan). Each life
cycle phase is verified against the activities in
`docs/qualification/SVP.md` (Software Verification Plan) and
recorded against the cases / procedures in
`docs/qualification/SVCP.md` and the captured outputs in
`docs/qualification/SVR.md`. Configuration management follows
`docs/qualification/SCMP.md`; quality assurance follows
`docs/qualification/SQAP.md`. The full plan family is indexed by
`docs/qualification/PSAC.md` (Plan for Software Aspects of
Certification).

| Life cycle area              | Plan / process document            | Status (2026-05-02)                     |
|------------------------------|------------------------------------|-----------------------------------------|
| Planning                     | PSAC                               | Stub (Phase 7 deliverable)              |
| Development                  | SDP                                | Stub (Phase 7 deliverable)              |
| Verification                 | SVP                                | Stub (Phase 7 deliverable)              |
| Configuration management     | SCMP                               | Stub (Phase 7 deliverable)              |
| Quality assurance            | SQAP                               | Stub (Phase 7 deliverable)              |
| Verification specification   | SVCP                               | First draft (this commit)               |
| Verification results         | SVR                                | First draft (this commit)               |
| Tool qualification           | TOOL_QUALIFICATION                 | First draft (this commit)               |
| MISRA-C 2012 deviations      | MISRA_DEVIATIONS                   | Active (D-001 through D-005)            |

The SAS will be re-issued at the close of Phase 7 once every plan
has its first complete revision.

## 4. Software characteristics

### 4.1 Size and shape

| Characteristic                        | Value                              | Source                       |
|---------------------------------------|------------------------------------|------------------------------|
| First-party C / C++ source files      | 143 (under `libs/` + `src/`)       | QUALIFICATION_ROADMAP.md S2  |
| Host test files                       | 180 (under `tests/`)               | `ls tests/test_*.c \| wc -l` |
| Test-to-source ratio                  | ~1.26 : 1                          | derived                      |
| EVM-tier applications                 | 26                                 | `ls examples/ek_ra8d2/`      |
| Unsupported / shelved applications    | 11                                 | `ls examples/_unsupported/`  |
| SOUP components catalogued            | 14 (under `docs/SOUP/`)            | QUALIFICATION_ROADMAP.md S2  |

### 4.2 Coverage achieved

| Coverage criterion          | Achieved | Target              | Source                          |
|-----------------------------|----------|---------------------|---------------------------------|
| Statement / line            | 86.28%   | (n/a -- subset of)  | `build/mcdc-report/summary.txt` |
| Branch                      | 77.30%   | -                   | same                            |
| Function                    | 95.22%   | -                   | same                            |
| Region                      | 85.96%   | -                   | same                            |
| MC/DC (DO-178C Level B obj) | **70.40%** | **100% (Phase 1) / 95% (Phase 2)** | same       |

MC/DC is the Annex A Table A-7 objective 5 criterion that drives
DO-178C Level B credit. The current 70.40% baseline is below both
Phase 1 and Phase 2 targets and is the largest single open item
on the certification path. See `docs/MCDC_GAPS.md` for the
per-decision burn-down and `docs/QUALIFICATION_ROADMAP.md` for
the closure schedule.

### 4.3 Coding standard compliance

The project uses MISRA-C 2012 as its coding language subset, per
IEC 61508-3 Annex A.4 (Highly Recommended at SIL 3) and DO-178C
section 11.8 (mandatory coding standard). The 2026-05-02 audit
(`docs/MISRA.md`) reports 1271 unique findings; every finding maps
to one of five active deviations (D-001 through D-005) recorded in
`docs/qualification/MISRA_DEVIATIONS.md`. The dispositions are:

- **D-001 (Rule 15.5, single-exit)**: project-wide formal deviation
  per MISRA-C:2012 sec. 5.2. Mitigation = NASA P10 Rule 5 (>= 2
  pre/post checks per function), MC/DC at 100% on hazard-path,
  clang-tidy LineThreshold = 60. 751 findings.
- **D-002 (Rule 17.3, implicit declaration)**: cppcheck-2.20
  C23-parser tooling gap. 170 findings.
- **D-003 (Rule 9.2, initializer braces)**: cppcheck-2.20 false
  positive on `= {}`. 35 findings.
- **D-004 (Rule 12.1, operator precedence)**: partial deviation +
  101 advisory hits closed by per-line suppression with redundant-
  parenthesis disposition.
- **D-005 (Rule 8.4, declaration before definition)**: cppcheck-2.20
  C23-parser tooling gap. 196 findings.

The complete deviation register is the authoritative artifact.

### 4.4 Documentation completeness

`docs/DOXYGEN_GAPS.md` (2026-05-02) reports 429 functions with at
least one missing required tag, out of 2666 audited (16.1% gap).
Closure is scheduled for Phase 3 of the roadmap.

### 4.5 SOUP inventory

14 SOUP components are catalogued under `docs/SOUP/`, each with a
written qualification basis citing IEC 61508-3 sec. 7.4.2.12 and
DO-178C sec. 12.1.4: ThreadX, NetX Duo, FileX, USBX, GUIX, LevelX,
Mbed TLS, TF-PSA-Crypto, lwIP, Apache NimBLE, litehtml, miniz, stb,
TinyXML-2. Re-review cadence is at most 12 months from each doc's
"Last review" stamp. The behavioural-boundary verification basis
for each SOUP is the integration test suite under
`tests/test_*.c` -- the SOUP itself is treated as black-box and
is excluded from MC/DC instrumentation per `docs/MCDC.md` Section
"Currently exempted code (SOUP)".

## 5. Open problem reports and deferred work

Cross-reference: `docs/qualification/SVR.md` Section 5 carries the
full anomaly log. Summary of items deferred from v1 release:

| ID     | Severity | Topic                                            | Disposition                |
|--------|----------|--------------------------------------------------|----------------------------|
| OP-001 | Major    | USB device chapter-9 enumeration not implemented | Deferred (multi-day port)  |
| OP-002 | Major    | LevelX xSPI NOR returns 0xFFFFFF (chip silent)   | Deferred (logic-analyzer)  |
| OP-003 | Major    | 4 EVM apps stuck in `*_panic_halt`               | Deferred                   |
| OP-004 | Critical | 5 BLE apps blocked on Renesas patch image (NDA)  | Blocked (vendor blob)      |
| OP-005 | Critical | RSIP-E BIST blocked on AMC blob (NDA)            | Blocked (vendor blob)      |
| OP-006 | Minor    | 429 functions missing Doxygen tags               | Scheduled (Phase 3)        |
| OP-007 | Major    | MC/DC at 70.40% vs 95-100% target                | Scheduled (Phases 1-2)     |
| OP-008 | Minor    | `make smoke` hangs in current bench environment  | Scheduled (Phase 6)        |

Deferred items OP-001, OP-002, OP-003 are all init-failure paths
caught by an in-app `*_panic_halt` -- in every case the chip itself
remains alive (zero hard faults observed across all 26 EVM apps in
the latest sweep). The deferred functions therefore cannot leak
into a runtime hazard from a working app; they are absent
features, not active defects.

## 6. Configuration index

This document is the entry point to the certification evidence
pack. The full pack consists of the artifacts below.

| Artifact                              | Path                                          |
|---------------------------------------|-----------------------------------------------|
| Plan for Software Aspects of Cert.    | `docs/qualification/PSAC.md`                  |
| Software Development Plan             | `docs/qualification/SDP.md`                   |
| Software Verification Plan            | `docs/qualification/SVP.md`                   |
| Software Configuration Mgmt Plan      | `docs/qualification/SCMP.md`                  |
| Software Quality Assurance Plan       | `docs/qualification/SQAP.md`                  |
| Software Verification Cases & Procs.  | `docs/qualification/SVCP.md`                  |
| Software Verification Results         | `docs/qualification/SVR.md`                   |
| Software Accomplishment Summary       | `docs/qualification/SAS.md` (this document)   |
| MISRA-C 2012 deviation register       | `docs/qualification/MISRA_DEVIATIONS.md`      |
| Tool qualification dossier            | `docs/qualification/TOOL_QUALIFICATION.md`    |
| Architectural baseline                | `docs/RING_AND_WORLD.md`                      |
| Memory map                            | `docs/MEMORY_MAP.md`                          |
| MC/DC infrastructure / status         | `docs/MCDC.md`, `docs/MCDC_GAPS.md`           |
| MISRA infrastructure / status         | `docs/MISRA.md`, `docs/MISRA_GAPS.csv`        |
| Doxygen audit                         | `docs/DOXYGEN_GAPS.md`, `docs/DOXYGEN_GAPS.csv`|
| Hardware bring-up record              | `docs/HARDWARE_BRINGUP.md`                    |
| SOUP register                         | `docs/SOUP/`                                  |
| Vendor-blob blockers                  | `docs/VENDOR_BLOBS.md`                        |
| Roadmap (master tracker)              | `docs/QUALIFICATION_ROADMAP.md`               |
| Build outputs (regenerable)           | `build/`, `tests/build/`, `tests/build-cov/`  |

The git repository at HEAD `402253ef` is the configuration-managed
baseline. CM is via git per `docs/qualification/SCMP.md`; signed
release tags will be added at SOI-4.

## 7. Statement of conformity

The project targets **IEC 61508 SIL 3** (industrial functional
safety, primary anchor) with parallel mappings to **DO-178C Level
B** (avionics, drives MC/DC) and **ISO 26262 ASIL C / ASIL D**
(automotive). The full side-by-side objective map is in
`docs/QUALIFICATION_ROADMAP.md` Section 1.

**Current claim**: this codebase has been developed against the
process documented in CLAUDE.md and the planning family above. As
of HEAD `402253ef` it is **not yet submitted for independent
assessment**. The gap analysis between the current state and the
SIL 3 / DAL B / ASIL C-D objective set is recorded in
`docs/QUALIFICATION_ROADMAP.md` Sections 2 and 3, with a 22-week
closure schedule.

**Known non-conformances at v0.1 baseline**:

1. MC/DC structural coverage is 70.40% versus a 95-100% target.
2. 429 functions are short of full Doxygen tag coverage versus a
   zero-gap target.
3. cppcheck-MISRA gives roughly two-thirds rule coverage; a
   qualified commercial checker (LDRA / Helix QAC / Polyspace) is
   required before SOI-3.
4. The independent assessor (IEC 61508-1 cl. 8.2) has not been
   nominated.
5. The hardware-in-the-loop CI runner (Phase 6) is not yet
   stood up; the smoke sweep is currently executed manually.
6. Vendor-blob-blocked features (BLE controller patch, RSIP-E AMC
   image) are out of scope for v1 -- the corresponding apps are
   under `examples/_unsupported/` and are not built into the
   release image.

This SAS will be re-issued at the close of each Phase to record
progress against these items.

## 8. Change history

| Date       | Author             | Change                                            |
|------------|--------------------|---------------------------------------------------|
| 2026-05-02 | Brighton Sikarskie | Initial first-draft population (Phase 7 kickoff). |
