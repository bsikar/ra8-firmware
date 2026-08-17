# Tool Qualification Dossier

**Document ID**: ra8d2-toolq-001
**Version**: 0.1 (first draft, Phase 7 of `docs/QUALIFICATION_ROADMAP.md`).
**Last refreshed**: 2026-05-03 (HIL posture re-stated as developer-laptop pre-push).
**Date**: 2026-05-02.
**Author**: Brighton Sikarskie.
**DO-178C reference**: Section 12.2 + RTCA DO-330.
**IEC 61508-3 reference**: Clause 7.4.4 + Annex D.
**ISO 26262-8 reference**: Clause 11 (Confidence in software tools).

## Scope and approach

This document records the per-tool qualification basis for every
software tool in the development and verification chain. The
high-level summary lives in `docs/QUALIFICATION_ROADMAP.md`
Section 5; this document carries per-tool detail.

DO-330 (and DO-178C section 12.2 by reference) classifies tools by
**Tool Qualification Level (TQL)**:

- **TQL-1** -- tool whose output is the certified software and
  whose error could go undetected. Requires the most stringent
  qualification.
- **TQL-2** through **TQL-4** -- intermediate categories.
- **TQL-5** -- tool whose output is verified by another process
  (Criterion 3 of DO-178C 12.2.1: "the tool could insert an error
  into the airborne software but the tool's output is verified by
  another method"). Lightest qualification burden.

Every tool below is justified at TQL-5 because every tool's output
is independently verified by a downstream check: the cross compiler
output is verified by hardware execution (`make smoke`), the host
compiler output is verified by ctest pass/fail, the MC/DC
instrumentation output is verified by manual spot-check against
hand-traced decisions, and so on.

## 1. arm-none-eabi-gcc (production cross-compiler)

| Attribute                       | Value                                                              |
|---------------------------------|--------------------------------------------------------------------|
| Vendor                          | Arm Ltd. (GNU Arm Embedded Toolchain)                              |
| Tool version pinned             | Arm GNU Toolchain **13.3.rel1** (gcc `13.3.1`), pinned +           |
|                                 | enforced on every host (#178). `cmake/toolchain-ra8d2.cmake`       |
|                                 | asserts `arm-none-eabi-gcc -dumpfullversion` major.minor           |
|                                 | `13.3` and is a FATAL configure error on a mismatch by             |
|                                 | default (`RA8_STRICT_TOOLCHAIN`, ON); the devcontainer fetches      |
|                                 | the tarball by URL + sha256. See `docs/TOOLCHAIN.md` (3.1).        |
| Intended use                    | Cross-compile every `.c` / `.cpp` source under `libs/`,           |
|                                 | `port/`, `examples/` to Cortex-M85 / M33 production object code.   |
| TQL classification              | **TQL-5**                                                          |
| DO-330 Criterion                | Criterion 3 ("output is verified by other means").                 |
| Qualification basis             | Every emitted binary is hardware-tested via `make smoke` against   |
|                                 | the EK-RA8D2 v1 (`docs/HARDWARE_BRINGUP.md`). The compiler is      |
|                                 | required to be warning-clean at `-Wall -Wextra -Werror` so any     |
|                                 | code-generation surprise that the compiler itself diagnoses is     |
|                                 | a build-stopping event.                                            |
| Compensating verification       | (a) `make smoke` halt-PC sweep across 26 EVM apps in               |
|                                 | `docs/HARDWARE_BRINGUP.md`. (b) Host ctest run reproduces the      |
|                                 | same logic on a different compiler. (c) Cross-compiler version     |
|                                 | bump triggers a re-run of the full smoke + MC/DC suite.            |
| Re-qualification trigger        | Any major-version bump (e.g. 13.x -> 14.x). Minor-version bumps    |
|                                 | trigger a smoke re-run only.                                       |
| Output integrity controls       | The `.elf` and `.hex` are reproducible from the configuration-     |
|                                 | managed source tree; build logs are archived per CI artifact       |
|                                 | retention policy.                                                  |

## 2. clang-18 + llvm-cov (host MC/DC instrumentation)

| Attribute                       | Value                                                              |
|---------------------------------|--------------------------------------------------------------------|
| Vendor                          | LLVM Project                                                       |
| Tool version pinned             | clang `>= 18` (we use 22 in the devcontainer per `docs/MCDC.md`).  |
| Intended use                    | Build host test binaries with the flag trio                        |
|                                 | `-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc`.     |
|                                 | Render the MC/DC report via `llvm-cov report --show-mcdc-summary`. |
| TQL classification              | **TQL-5**                                                          |
| DO-330 Criterion                | Criterion 3.                                                       |
| Qualification basis             | The clang output is **test-only** -- no production code path uses  |
|                                 | clang artefacts. The MC/DC report is reviewed by hand against      |
|                                 | known decisions in `libs/ra8_core/src/ra8_log.c` (worked example     |
|                                 | in `docs/MCDC.md` "Adding MC/DC test vectors") so any silent       |
|                                 | MC/DC accounting error is detectable.                              |
| Compensating verification       | (a) Manual spot-check against hand-traced truth tables at          |
|                                 | Phase 1 / Phase 2 boundaries. (b) gcc-14 `-fcondition-coverage`    |
|                                 | fallback path provides an independent (but weaker) coverage check  |
|                                 | when clang is unavailable.                                         |
| Re-qualification trigger        | Major-version bump or any change to LLVM's MC/DC accounting        |
|                                 | (release-note review).                                             |
| Limitation noted                | Per `docs/MCDC.md`, gcc-14 `-fcondition-coverage` is **not** a     |
|                                 | DO-178C-compliant MC/DC implementation. The fallback exists so     |
|                                 | the script runs end-to-end on machines without modern clang; it    |
|                                 | does not produce certification-grade coverage evidence.            |

## 3. cppcheck + MISRA addon (advisory checker)

| Attribute                       | Value                                                              |
|---------------------------------|--------------------------------------------------------------------|
| Vendor                          | Cppcheck team (open source) + MISRA-C 2012 addon                   |
| Tool version pinned             | cppcheck 2.20 (Homebrew); addon path                               |
|                                 | `/opt/homebrew/share/Cppcheck/addons/misra.py`.                    |
| Intended use                    | Advisory MISRA-C 2012 audit invoked via `make misra`.              |
| TQL classification              | **TQL-5**                                                          |
| DO-330 Criterion                | Criterion 3 (advisory; not the authoritative coding-standard       |
|                                 | gate).                                                             |
| Qualification basis             | (a) cppcheck implements roughly two thirds of the mandatory +      |
|                                 | required MISRA-C 2012 rules per `docs/MISRA.md` -- partial         |
|                                 | coverage is acknowledged. (b) Every emitted finding is reviewed    |
|                                 | by hand and recorded in                                            |
|                                 | `docs/qualification/MISRA_DEVIATIONS.md` (D-001..D-010).           |
|                                 | (c) The arm-none-eabi-gcc cross build with `-std=gnu23             |
|                                 | -Wimplicit-function-declaration -Werror -Wmissing-prototypes`      |
|                                 | is the **authoritative checker** for the Mandatory rules           |
|                                 | (e.g. 17.3, 8.4) -- cppcheck is supplementary.                     |
| Compensating verification       | (a) Cross-compiler `-Werror` gate. (b) Ten formal MISRA            |
|                                 | deviations covering every persistent finding class.                |
|                                 | (c) Quarterly re-audit cadence per `docs/MISRA.md`.                |
| Re-qualification trigger        | cppcheck major-version bump, especially the release that adds     |
|                                 | `--std=c23` (D-002, D-003, D-005 and D-007 retire as soon as the  |
|                                 | parser accepts C23; D-009 retires when the MISRA addon resolves   |
|                                 | enum-named array extents). Tracked at MAR 2026-11-02 in            |
|                                 | MISRA_DEVIATIONS.md.                                               |
| Upgrade path                    | None. Commercial MISRA checkers (LDRA / Helix QAC / Polyspace /   |
|                                 | PVS-Studio) are explicitly out of scope for this MIT-licensed,    |
|                                 | non-certifying project. See                                        |
|                                 | `docs/qualification/MISRA_DEVIATIONS.md` "Tooling policy" and     |
|                                 | `docs/CERTIFICATION_SCOPE.md`.                                    |

## 4. JLinkExe (SEGGER J-Link OB)

| Attribute                       | Value                                                              |
|---------------------------------|--------------------------------------------------------------------|
| Vendor                          | SEGGER Microcontroller GmbH                                        |
| Tool version pinned             | JLinkExe v9.38a (per `docs/HARDWARE_BRINGUP.md`).                  |
| Intended use                    | Programming MRAM (`scripts/dev/flash.sh`) and halting / register-      |
|                                 | dumping the CPU during the HIL sweep                               |
|                                 | (`scripts/hil/all.sh`). Ozone debugger backend                     |
|                                 | (`scripts/dev/ozone.sh`).                                              |
| TQL classification              | **TQL-5**                                                          |
| DO-330 Criterion                | Criterion 3.                                                       |
| Qualification basis             | The tool is read / write to the device's MRAM and to its debug    |
|                                 | port. Any write step (flash) is verified by post-flash readback   |
|                                 | implicit in the next smoke step (PC halt -> addr2line -> the      |
|                                 | resolved symbol must lie in the just-flashed image). Any read     |
|                                 | step (halt + register dump) is read-only with respect to the      |
|                                 | certified bits.                                                   |
| Compensating verification       | (a) Halt-PC classification rubric in `docs/HARDWARE_BRINGUP.md`   |
|                                 | distinguishes PASS / WIP / FAIL / UNKNOWN; UNKNOWN never          |
|                                 | promotes to PASS silently. (b) Cross-check against ELF symbol     |
|                                 | table when classification is ambiguous.                           |
| Re-qualification trigger        | Major-version bump of JLinkExe or any change to the host          |
|                                 | classification rubric.                                            |
| Known issue                     | OP-008 in `docs/qualification/SVR.md`: `make smoke` hangs in the  |
|                                 | current bench environment when invoked top-level. Manual          |
|                                 | per-app invocation reproduces the harness rubric and was used     |
|                                 | for the 2026-05-02 evening and night sweeps.                      |

## 5. scripts/git/pre-commit (qualified internal tool)

| Attribute                       | Value                                                              |
|---------------------------------|--------------------------------------------------------------------|
| Vendor                          | In-house (Brighton Sikarskie / project author)                     |
| Tool version pinned             | Git-managed; HEAD `402253ef` ships the pre-commit gate as          |
|                                 | `scripts/git/pre-commit` plus per-check helpers under              |
|                                 | `scripts/checks/`.                                                 |
| Intended use                    | Block any commit that violates ASCII-only, clang-format,           |
|                                 | clang-tidy, cppcheck (without MISRA addon), no-dynamic-allocation, |
|                                 | world-tag balance, since-version stamping, or obsolete-standards   |
|                                 | references (e.g. obsolete-standard names are forbidden -- only     |
|                                 | the current revisions are used).                                   |
| TQL classification              | **TQL-5**                                                          |
| DO-330 Criterion                | Criterion 3.                                                       |
| Qualification basis             | Every check inside the hook is independently re-runnable from     |
|                                 | the command line (each helper script accepts `--all` to scan the  |
|                                 | full tree). The CI pipeline reruns the same scripts on the        |
|                                 | merged commit, so any local hook bypass is caught at the           |
|                                 | server-side gate. The check inputs (source files) are              |
|                                 | configuration-managed via git.                                     |
| Compensating verification       | (a) Server-side CI re-runs every gate. (b) Each helper script     |
|                                 | carries its own host test under `scripts/checks/` where             |
|                                 | applicable. (c) The `check_obsolete_standards.py` script is the   |
|                                 | sole gatekeeper for the "no obsolete-standard names" rule and is  |
|                                 | exercised by every commit that touches `docs/`.                    |
| Re-qualification trigger        | Any change to a helper script or to the hook orchestrator.        |
|                                 | Reviewed alongside the changing PR; no separate cadence.          |

## 6. GitHub Actions runners (CI environment)

| Attribute                       | Value                                                              |
|---------------------------------|--------------------------------------------------------------------|
| Vendor                          | GitHub (Microsoft) -- hosted runners.                              |
| Tool version pinned             | Workflow files under `.github/workflows/` pin OS image (e.g.       |
|                                 | `ubuntu-22.04`) and toolchain installer versions.                  |
| Intended use                    | Re-run every pre-commit gate, host build, host ctest, MC/DC,      |
|                                 | MISRA, and Doxygen audit on every PR + main push.                  |
| TQL classification              | **TQL-5**                                                          |
| DO-330 Criterion                | Criterion 3 (verification environment, not production code         |
|                                 | path).                                                             |
| Qualification basis             | The runner is a verification environment: every step's output     |
|                                 | (build log, ctest log, coverage report) is observable in the      |
|                                 | run summary and downloadable as an artifact. The runner          |
|                                 | reproduces the same scripts a developer runs locally; any         |
|                                 | environment skew between developer machine and CI surfaces as     |
|                                 | a CI-only failure that blocks merge.                              |
| Compensating verification       | (a) Local pre-commit hook reproduces the same gates. (b) HW       |
|                                 | smoke is the developer-laptop pre-push workflow                    |
|                                 | (`docs/HIL_DEVELOPER_WORKFLOW.md`).                                |
|                                 | (c) Artifact retention preserves the build log for post-mortem.   |
| Re-qualification trigger        | Major OS-image bump on the hosted runner; toolchain installer     |
|                                 | bump captured by the workflow file's version pin.                 |
| Open item                       | None. HIL is **permanently** developer-laptop pre-push per         |
|                                 | `docs/CERTIFICATION_SCOPE.md`; a self-hosted runner is out of      |
|                                 | scope.                                                             |

## 7. Adjunct tools (recorded for completeness)

The tools below are part of the chain but already covered in
`docs/QUALIFICATION_ROADMAP.md` Section 5 at summary level. Each
inherits the same TQL-5 + Criterion 3 disposition because each
output is downstream-verified.

| Tool                          | Use                                  | Compensating verification                         |
|-------------------------------|--------------------------------------|---------------------------------------------------|
| `clang-tidy`                  | Naming + complexity gate             | Advisory only; no autofix in CI; LineThreshold    |
|                               |                                      | cross-checked against NASA P10 Rule 4.            |
| `clang-format`                | Style enforcement                    | Idempotent; reviewed by humans on every PR.       |
| `llvm-profdata`               | Merge MC/DC raw profiles             | Output consumed only by `llvm-cov`; spot-checked. |
| `cmake` + `make`              | Build orchestrator                   | Output is the same arm-none-eabi object as a      |
|                               |                                      | manual invocation; build log archived per CI run. |
| `arm-none-eabi-addr2line`     | Smoke-test PC resolution             | Cross-checked against ELF symbol table when       |
|                               |                                      | classification is ambiguous.                      |
| `python3` (audit scripts)     | Doxygen / MC/DC / MISRA gap reports  | Output reviewed by hand; helper scripts under     |
|                               |                                      | `scripts/checks/` carry their own host tests.     |

No tool in the chain currently requires TQL-1 because none of them
emit certified production code without a downstream verification
step. The closest call is the cross compiler; the mitigation is the
developer-laptop pre-push hardware-in-the-loop smoke
(`docs/HIL_DEVELOPER_WORKFLOW.md`) plus the host-side integration
tests (25/26 EVM apps covered).

## 8. Re-qualification cadence summary

| Tool                       | Re-qualification trigger                                          |
|----------------------------|-------------------------------------------------------------------|
| arm-none-eabi-gcc          | Major-version bump.                                               |
| clang-18 / llvm-cov        | Major-version bump or LLVM MC/DC accounting change.               |
| cppcheck + misra addon     | Cppcheck major-version bump (esp. C23 support); MAR 2026-11-02.   |
| JLinkExe                   | Major-version bump or rubric change.                              |
| scripts/git/pre-commit     | Per-PR; reviewed alongside the change.                            |
| GitHub Actions runners     | OS-image bump on hosted runner; per-workflow file change.         |
| clang-tidy / clang-format  | Major-version bump.                                               |
| llvm-profdata              | Bundled with clang version pin.                                   |
| cmake / make               | Major-version bump.                                               |
| arm-none-eabi-addr2line    | Bundled with arm-none-eabi-gcc version pin.                       |
| python3                    | Per-PR for any helper-script change; tracked in `scripts/checks/`. |

## 9. Change log

| Date       | Author             | Change                                            |
|------------|--------------------|---------------------------------------------------|
| 2026-05-02 | Brighton Sikarskie | Initial first-draft population (Phase 7 kickoff). |
| 2026-05-03 | Brighton Sikarskie | Re-stated HIL posture as developer-laptop pre-push (`docs/HIL_DEVELOPER_WORKFLOW.md`); self-hosted runner out of scope. |
