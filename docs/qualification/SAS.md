# Software Accomplishment Summary (SAS)

**Document ID**: ra8d2-sas-001
**Version**: 0.1 (first draft, Phase 7 of `docs/QUALIFICATION_ROADMAP.md`).
**Last refreshed**: 2026-08-22 (migration qualification inventory refresh).
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
that the planned process was followed. This is a maintained working draft, not
a claim that the current candidate has been restamped. The release commit is
frozen only when the evidence pack is issued; per-section detail distinguishes
live authorities from dated retained artifacts.

## 1. System overview

`ra8-firmware` is bare-metal firmware for the Renesas RA8D2 MCU
group, targeting the EK-RA8D2 evaluation kit (Renesas part number
`968-K7EKA8D2S01001BE`). The MCU is a dual-core
`R7KA8D2KFLCAC` (Arm Cortex-M85 @ 1 GHz primary +
Cortex-M33 @ 250 MHz secondary) with 1 MB on-chip MRAM, 1664 KiB SRAM
with ECC, an Octo-SPI port to 64 MB external NOR flash, and a
parallel-RGB port to a 1024 x 600 TFT plus an OV5640 5 MP camera.
The firmware exercises the chip's HAL surface from a single
hand-written register-level driver tree (no Renesas FSP source in
the repo; FSP is reference material only). It supports a TrustZone
secure / non-secure split, hosts an ARM-validated SOUP RTOS
(ThreadX) and a SOUP networking stack (NetX Duo / lwIP). The live
emulator-in-the-loop population is derived by `scripts/dev/ra8_apps.py`.

## 2. Software identification

| Item                | Value                                                        |
|---------------------|--------------------------------------------------------------|
| Product             | ra8-firmware                                               |
| Repository          | https://github.com/bsikar/ra8-firmware                     |
| Baseline commit     | `402253efae5b0a6742b37eb14e6224f339d1cfbf` (2026-05-02)      |
| Target MCU          | Renesas R7KA8D2KFLCAC (RA8D2 group)                          |
| Target board        | EK-RA8D2 v1 (Renesas 968-K7EKA8D2S01001BE)                   |
| Primary core        | Arm Cortex-M85 @ 1 GHz, with Helium / MVE                    |
| Secondary core      | Arm Cortex-M33 @ 250 MHz                                     |
| Code memory         | 1 MB MRAM (non-volatile, on-chip)                            |
| System RAM          | 1664 KiB (1.625 MiB) SRAM with ECC                          |
| External memory     | 64 MB Octo-SPI NOR (IS25LX512M-JHLE), 64 MB SDRAM            |
| Cross toolchain     | ARM GNU Toolchain `arm-none-eabi-gcc` (qualified per         |
|                     | `docs/qualification/TOOL_QUALIFICATION.md` Section 1)        |
| Host toolchain      | `gcc-14` and `clang-18`                                      |
| Build orchestrator  | Just + CMake + Ninja                                         |
| RTOS                | None at firmware level; SOUP `ThreadX` available per app      |
| Debug probe         | On-board SEGGER J-Link OB (serial in .env JLINK_SN)          |

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
| Verification specification   | SVCP                               | Working draft                           |
| Verification results         | SVR                                | Working draft                           |
| Tool qualification           | TOOL_QUALIFICATION                 | Working draft                           |
| MISRA-C 2012 deviations      | MISRA_DEVIATIONS                   | Active (D-001 through D-012)            |

The SAS will be re-issued at the close of Phase 7 once every plan
has its first complete revision.

## 4. Software characteristics

### 4.1 Size and shape

| Characteristic                        | Value                              | Source                       |
|---------------------------------------|------------------------------------|------------------------------|
| Test source files                     | **693** (689 C, 4 C++)             | retained 2026-08-22 snapshot |
| Registered CTest cases                | **689** on clean standalone macOS and Linux configurations | retained 2026-08-22 snapshot |
| Linux/devcontainer host execution     | **689/689 passed in 8.66 s** on 2026-08-22 | retained unit-gate result; macOS execution not claimed |
| EIL application inventory             | Derived by `scripts/dev/ra8_apps.py` | live app authority          |
| RA8D2 physical-app builds             | **118/118 built**                  | retained historical snapshot; current matrix pending |
| Real HIL execution                    | **Pending**                        | run `hil-all` on the current candidate |
| Remote GDB lifecycle                  | **Historical pass**                | restamp for release evidence  |

### 4.2 Archived coverage snapshot

| Coverage criterion              | Achieved          | Target              | Archived evidence date |
|---------------------------------|-------------------|---------------------|------------------------|
| MC/DC, reachable decision-complete (gate) | **100.00%** | **100%** | 2026-05-03 |
| MC/DC, all decision regions complete | 92.29%       | (informational)     | 2026-05-03 |
| Deactivated decision regions (6.4.4.3) | 58          | -                   | 2026-05-03 |

These values are the archived 2026-05 snapshot, not a qualification result for
the migrated tree. MC/DC is the Annex A Table A-7 objective 5 criterion that
drives DO-178C Level B credit; the release evidence pack must rerun and restamp
the current measurement.

### 4.3 Archived coding-standard snapshot

The project uses MISRA-C 2012 as its coding language subset, per
IEC 61508-3 Annex A.4 (Highly Recommended at SIL 3) and DO-178C
section 11.8 (mandatory coding standard). The archived 2026-05-02 audit
reported 1271 findings. That number is historical and is neither the current
population nor a claim that every finding was accepted. The current audit is
pinned to cppcheck 2.13.0, and
`docs/qualification/MISRA_DEVIATIONS.md` records D-001 through D-012;
its machine-checked index and `.github/misra-baseline.txt` are the current
population authorities. D-001 through D-005 date from the archived audit;
D-006 through D-012 were registered afterwards as scope and reviewed
interfaces expanded. The dispositions are:

- **D-001 (Rule 15.5, single-exit)**: project-wide formal deviation
  per MISRA-C:2012 sec. 5.2. Mitigation = NASA P10 Rule 5 (>= 2
  pre/post checks per function), the hazard-path MC/DC obligation, and
  clang-tidy LineThreshold = 60. The archived audit reported 751 findings.
- **D-002 (Rule 17.3, implicit declaration)**: pinned cppcheck 2.13.0
  C23-parser tooling gap. The archived audit reported 170 findings.
- **D-003 (Rule 9.2, initializer braces)**: pinned cppcheck 2.13.0 false
  positive on `= {}`. The archived audit reported 35 findings.
- **D-004 (Rule 12.1, operator precedence)**: partial deviation +
  code-change disposition outside the accepted idiom classes. The archived
  audit reported 101 advisory findings.
- **D-005 (Rule 8.4, declaration before definition)**: pinned cppcheck 2.13.0
  C23-parser tooling gap. The archived audit reported 196 findings.
- **D-006 (Rule 20.5, `#undef`)**: project-wide formal deviation for
  the single `RA8_NSC_VENEER` redefinition site that keeps the CMSE
  attribute authoritative regardless of include order.
- **D-007 (Rule 14.2, for-loop form)**: cppcheck C23 `[[nodiscard]]`
  parse defect mischarges the well-formed `RA8_PROTECTED_WRITE`
  run-once guard loop. Tooling gap.
- **D-008 (Rule 17.1, stdarg)**: formal deviation for three bounded
  variadic adapters (esp-hosted log bridge, emulator host I/O,
  cache_bench I/O).
- **D-009 (Rule 9.5, explicit array size)**: enum-named array extents
  are explicit sizes the cppcheck MISRA addon cannot resolve. Tooling
  gap.
- **D-010 (Rule 11.5, void-pointer conversion)**: formal deviation
  for dependency-injection seams recovering typed context from
  `void *`, per the project's NASA P10 Rule 9 deviation.
- **D-011 (Rule 11.6, pointer/integer conversion)**: narrow formal deviation
  for the XZ caller-workspace alignment check; it does not accept other Rule
  11.6 sites or an integer-to-pointer conversion.
- **D-012 (Rule 21.1, reserved identifier)**: narrow formal deviation for the
  guarded `__always_inline` adapter required by byte-identical XZ Embedded
  SOUP; all other Rule 21.1 findings remain code-change debt.

The complete deviation register is the authoritative artifact; this section
does not claim a current migrated-tree population.

### 4.4 Documentation completeness

The archived `docs/DOXYGEN_GAPS.md` 2026-05 refresh reported no gaps in its
then-current population. Regenerate the audit for the candidate under review;
the archived count is not current evidence.

### 4.5 SOUP inventory

The current SOUP component population is derived from
`scripts/gen/sbom_registry.py` and checked against the qualification pages
under `docs/SOUP/` (excluding its README). Each page has a
written qualification basis citing IEC 61508-3 sec. 7.4.2.12 and
DO-178C sec. 12.1.4: ThreadX, NetX Duo, USBX, LevelX,
Mbed TLS, TF-PSA-Crypto, lwIP, Apache NimBLE, litehtml, miniz, stb,
TinyXML-2. Re-review cadence is at most 12 months from each doc's
"Last review" stamp. The behavioural-boundary verification basis
for each SOUP is the distributed integration-test corpus under `tests/`,
`apps/**/tests/`, and `examples/**/tests/` -- the SOUP itself is treated as black-box and
is excluded from MC/DC instrumentation per `docs/MCDC.md` Section
"Currently exempted code (SOUP)".

## 5. Open problem reports and deferred work

Cross-reference: `docs/qualification/SVR.md` Section 5 carries the
full anomaly log. Summary of items deferred from v1 release:

| ID     | Severity | Topic                                            | Archived 2026-05 disposition |
|--------|----------|--------------------------------------------------|------------------------------|
| OP-001 | Major    | USB device chapter-9 enumeration not implemented | Deferred (multi-day port)  |
| OP-002 | Major    | LevelX xSPI NOR returns 0xFFFFFF (chip silent)   | Deferred (logic-analyzer)  |
| OP-003 | Major    | EVM apps stuck in `*_panic_halt`                 | Deferred                   |
| OP-004 | Critical | BLE apps attributed to a nonexistent Renesas patch image | CLOSED INVALID (`6f6209a95`): no on-chip BLE radio; controller is the ESP32-C6 companion |
| OP-005 | Critical | RSIP-E BIST blocked on AMC blob                  | Blocked (not vendored; public FSP, BSD-3-Clause) |
| OP-006 | Minor    | Doxygen tag completeness                         | Recorded closed in the archived snapshot; current audit pending |
| OP-007 | Major    | MC/DC reachable target                           | Recorded closed in the archived snapshot; current audit pending |
| OP-008 | Minor    | Legacy smoke harness hung in the 2026-05 bench environment | Superseded by guarded `just hil::run`; manual HIL workflow remains available |

OP-001 through OP-003 describe the archived 2026-05 bench sweep and are not
current full-fleet qualification results. Retained evidence is narrower: the
118/118 RA8D2 build, selected-app HIL, and remote-GDB lifecycle results are
historical; current-matrix build and hardware execution are
pending.

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
| MISRA infrastructure / status         | `docs/MISRA.md`, `.github/misra-baseline.txt` |
| Doxygen audit                         | `docs/DOXYGEN_GAPS.md`, `docs/DOXYGEN_GAPS.csv`|
| Hardware bring-up record              | `docs/HARDWARE_BRINGUP.md`                    |
| SOUP register                         | `docs/SOUP/`                                  |
| Vendor-blob blockers                  | `docs/VENDOR_BLOBS.md`                        |
| Roadmap (primary tracker)              | `docs/QUALIFICATION_ROADMAP.md`               |
| Build outputs (regenerable)           | `build/`, `tests/build/`, `tests/build-cov/`  |

The current git working tree is the configuration-managed
baseline. CM is via git per `docs/qualification/SCMP.md`; signed
release tags will be added at SOI-4.

## 7. Statement of conformity

The project targets **IEC 61508 SIL 3** (industrial functional
safety, primary anchor) with parallel mappings to **DO-178C Level
B** (avionics, drives MC/DC) and **ISO 26262 ASIL C / ASIL D**
(automotive). The full side-by-side objective map is in
`docs/QUALIFICATION_ROADMAP.md` Section 1.

**Bounded claim**: this codebase is developed against the process documented
in CLAUDE.md and the planning family above, but this draft does not declare the
current candidate qualified. The project is **not yet submitted for independent
assessment**. The gap analysis between the maintained state and the
SIL 3 / DAL B / ASIL C-D objective set is recorded in
`docs/QUALIFICATION_ROADMAP.md` Sections 2 and 3, with a 22-week
closure schedule.

**Known non-conformances at the 2026-05-03 refresh**:

1. Current MC/DC and branch/statement coverage must be regenerated and
   restamped for the migrated tree; archived values are not release evidence.
2. cppcheck-MISRA gives roughly two-thirds rule coverage. The
   project's permanent policy is **cppcheck-only** per
   `docs/CERTIFICATION_SCOPE.md`; commercial checker procurement is
   out of scope.
3. Independent-assessor engagement is **out of scope, permanently**,
   per `docs/CERTIFICATION_SCOPE.md`.
4. Hardware-in-the-loop uses the managed dev-box listener and guarded
   Raspberry Pi 5 instrument host in `docs/HIL_DEVELOPER_WORKFLOW.md`.
   HIL-relevant pushes and trusted same-repository PRs schedule `hil.yml`
   automatically. The selected 2/2 result is historical; current-candidate
   full-fleet HIL evidence remains pending.
5. No vendor binary blob is shipped in this tree. Hardware-backed RSIP
   key wrap would need the RSIP-E AMC firmware, obtainable from public
   FSP under BSD-3-Clause but deliberately not vendored (see
   `docs/VENDOR_BLOBS.md`); the software backend is what is built and
   host-tested. The RA8D2 has no BLE radio, so there is no controller
   patch image to obtain.

This SAS will be re-issued at the close of each Phase to record
progress against these items.

## 8. Change history

| Date       | Author             | Change                                            |
|------------|--------------------|---------------------------------------------------|
| 2026-05-02 | Brighton Sikarskie | Initial first-draft population (Phase 7 kickoff). |
| 2026-05-03 | Brighton Sikarskie | Refreshed the then-current coverage, documentation, and HIL posture. |
| 2026-08-21 | Brighton Sikarskie | Replaced the flat-test and legacy app counts with the distributed 692-source / 673-registration inventory and current build, HIL, and remote-GDB evidence. |
| 2026-08-21 | Brighton Sikarskie | Added the authoritative Linux/devcontainer 673/673 unit result (46.92 s) while retaining macOS as registration-only. |
| 2026-08-21 | Brighton Sikarskie | Corrected the archived MISRA narrative to the pinned cppcheck 2.13.0 and current D-001..D-012 register without promoting historical counts. |
| 2026-08-22 | Brighton Sikarskie | Added the example-local runtime-provisioner test and recorded the 693-source / 689-registration inventory and Linux/devcontainer 689/689 result (8.66 s). |
