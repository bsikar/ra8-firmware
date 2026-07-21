# Software Verification Plan (SVP)

**Last refreshed**: 2026-05-03 (numbers re-synced to live audit
artefacts after closure).

**Status**: First draft, 2026-05-02. Populated during Phase 7 of
`docs/QUALIFICATION_ROADMAP.md`. Subject to revision after the first
external assessor review.

**DO-178C reference**: Section 11.3 (SVP content) and Section 6.4
(Verification Process activities).
**IEC 61508-3 reference**: Clause 7.9 (Software verification) and
Annex C (Properties for systematic capability).
**ISO 26262-6 reference**: Clause 9 (Software unit verification) and
Clause 11 (Verification of software safety requirements).

**Owner**: Brighton Sikarskie (single developer / maintainer).
**Independence note**: see Section 3 below.

---

## 1. Verification objectives

This SVP claims partial coverage of DO-178C Annex A Tables A-3 through
A-7 at Level B. Each objective below lists the table reference, the
DO-178C objective number, the artifact in this repository that
constitutes the evidence, and the gap between the current evidence and
"objective with independence satisfied" status.

The IEC 61508-3 Annex A/B technique tables are mapped in
`docs/QUALIFICATION_ROADMAP.md` Section 1; this SVP refines that map
into a verification-activity register.

### 1.1 Table A-3 -- Verification of outputs of software requirements process

| Obj # | Subject                                          | Evidence                                                                                          | Gap                                                              |
|------:|--------------------------------------------------|---------------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | High-level requirements comply with system reqs  | `docs/ARCHITECTURE.md`, per-module `@brief` corpus, `docs/RING_AND_WORLD.md`                      | No system requirements document exists yet (PSAC sec. 5 task).   |
| 2     | High-level requirements are accurate, consistent | `docs/STYLE_GUIDE.md` review checklist, PR review history                                         | Independence not yet established (Section 3).                    |
| 3     | HLR compatible with target computer              | `docs/MEMORY_MAP.md`, `cmake/toolchain-ra8d2.cmake`                                               | Hardware-in-the-loop CI not yet wired (Phase 6 of roadmap).      |
| 4     | HLR verifiable                                   | Each `@brief` accompanied by `@param`/`@retval`/`@pre`/`@post` per `CLAUDE.md` Doxygen rules      | Closed: 0 functions with gaps (2747 audited).                    |
| 5     | HLR conform to standards                         | `docs/STYLE_GUIDE.md`, `docs/MISRA.md`, pre-commit hook                                           | MISRA backlog tracked in `docs/qualification/MISRA_DEVIATIONS.md`.|
| 6     | HLR traceable to system reqs                     | `scripts/checks/cite_check.py` (HUM citation validator)                                            | Bidirectional trace matrix not yet generated.                    |
| 7     | Algorithms are accurate                          | `tests/test_*.c` requirements-based tests                                                         | Coverage of algorithm corner cases tracked via MC/DC gap list.   |

### 1.2 Table A-4 -- Verification of outputs of software design process

| Obj # | Subject                                | Evidence                                                                | Gap                                                              |
|------:|----------------------------------------|-------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | LLR comply with HLR                    | Per-module headers under `libs/*/inc/` cross-reference `docs/`          | Trace matrix pending.                                            |
| 2     | LLR are accurate, consistent           | `clang-tidy` naming + complexity gates                                  | Threshold tuning ongoing.                                        |
| 3     | LLR compatible with target computer    | `docs/STACK_USAGE.md`, `-Wstack-usage` gate, `.su` aggregator           | Heap proof: `scripts/checks/check_no_dynamic_alloc.py` already gates. |
| 4     | LLR verifiable                         | `tests/test_<module>.c` per module                                      | 25 of 26 EVM apps have host integration tests today.             |
| 5     | LLR conform to standards               | `.clang-format`, `.clang-tidy`, `docs/STYLE_GUIDE.md`                   | Pre-commit enforces; CI mirrors.                                 |
| 6     | LLR traceable to HLR                   | `scripts/checks/cite_check.py` HUM page-citation tags                    | Bidirectional trace matrix pending.                              |
| 7     | Algorithms are accurate                | Targeted unit tests (`tests/test_ra8_*_mcdc.c` class)                    | MC/DC reachable = 100.00% (gate met); absolute = 92.29%.         |
| 8     | Software architecture compat with HLR  | `docs/RING_AND_WORLD.md`, `docs/MEMORY_MAP.md`                          | Section 8 (Partitioning) below.                                  |
| 9     | Software architecture consistent       | `scripts/checks/check_world_tags.py`                                     | Strict mode pending (currently `--warn`).                        |
| 10    | Software architecture compat with target| Cross-build matrix in `.github/workflows/firmware.yml::build-cross`     | HW-in-the-loop pending.                                          |
| 11    | Software architecture verifiable       | Module-level integration tests under `tests/`                           | See Phase 5.                                                     |
| 12    | Software architecture conforms to std  | NASA P10 + SOLID-for-C policy in `CLAUDE.md`                            | Manual review only; no automated structural check yet.           |
| 13    | Software partitioning integrity        | TrustZone S/NS split via SAU (`examples/*/trustzone_init.c`)            | See Section 8.                                                   |

### 1.3 Table A-5 -- Verification of outputs of coding and integration

| Obj # | Subject                                  | Evidence                                                                                  | Gap                                                              |
|------:|------------------------------------------|-------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | Source code complies with LLR            | Source under `libs/`, `src/`, `examples/`                                                 | Trace matrix pending.                                            |
| 2     | Source code complies with architecture   | `scripts/checks/check_world_tags.py`, `docs/RING_AND_WORLD.md`                             | Strict mode pending.                                             |
| 3     | Source code is verifiable                | Host test corpus under `tests/` (190 `test_*.c` files; 190/190 PASS)                      | Test-to-source ratio sustained above 1:1.                        |
| 4     | Source code conforms to standards        | clang-format, clang-tidy, cppcheck (with MISRA addon via `make misra`)                    | MISRA backlog under deviation register.                          |
| 5     | Source code is traceable to LLR          | Doxygen `@see` cross-references                                                           | Audit script not yet automated.                                  |
| 6     | Source code is accurate and consistent   | `-Wall -Wextra -Werror`, `-Wstack-usage`, `clang-tidy`                                    | Zero-warning build enforced by CI.                               |
| 7     | Output of integration is complete        | `make` cross-build per app; `examples/ek_ra8d2/*` matrix                                  | HW-smoke not yet on CI runner (Phase 6).                         |
| 8     | Parameter Data Items are correct         | No PDI subsystem in scope today.                                                          | Re-evaluate if a parameter file format is added later.           |
| 9     | Parameter Data Items have file structure | Same as above.                                                                            | Same as above.                                                   |

### 1.4 Table A-6 -- Testing of outputs of integration process

| Obj # | Subject                                  | Evidence                                                                                  | Gap                                                              |
|------:|------------------------------------------|-------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | Executable Object Code complies with HLR | `make smoke` scripts referenced in `docs/HARDWARE_BRINGUP.md`                             | HIL is developer-laptop pre-push (`docs/HIL_DEVELOPER_WORKFLOW.md`); self-hosted runner out of scope. |
| 2     | EOC robust with HLR                      | Negative-path tests under `tests/test_*.c` (return-value pollution, NULL injection)       | Coverage tracked per-module in MC/DC report.                     |
| 3     | EOC complies with LLR                    | Host integration tests (`tests/test_app_*.c` family)                                      | 25 of 26 EVM apps have host integration tests today.             |
| 4     | EOC robust with LLR                      | Same as 2.                                                                                | Same as 2.                                                       |
| 5     | EOC compatible with target               | Cross-build matrix (`firmware.yml::build-cross`) + planned HW-smoke                       | Phase 6 closes the loop.                                         |

### 1.5 Table A-7 -- Verification of verification process results

| Obj # | Subject                                  | Evidence                                                                                  | Gap                                                              |
|------:|------------------------------------------|-------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | Test procedures are correct              | PR review history + `tests/build_tests.sh`, `tests/run_tests.sh`                          | Independence pending.                                            |
| 2     | Test results are correct, discrepancies  | CI logs (GitHub Actions, retained per workflow retention policy)                          | SVR (Verification Results) document populated in roadmap Phase 7.|
| 3     | Test coverage of HLR achieved            | `make test`, `tests/test_*.c` requirements-based tests                                    | Trace matrix pending.                                            |
| 4     | Test coverage of LLR achieved            | Same as 3 plus per-module decision tests                                                  | Same as 3.                                                       |
| 5     | Test coverage of structure achieved -- MC/DC | `make mcdc` (clang-18 `-fcoverage-mcdc`), `docs/MCDC.md`, baseline at `.github/mcdc-baseline.txt` | Reachable = 100.00% (gate met); absolute = 92.29%; 58 conditions deactivated under DO-178C 6.4.4.3. |
| 6     | Test coverage of structure achieved -- branch | gcovr branch coverage gated at 90/90 by `scripts/checks/coverage.sh --gate`                  | Pass today.                                                      |
| 7     | Test coverage of structure achieved -- statement | Same as 6.                                                                         | Pass today.                                                      |
| 8     | Test coverage of structure -- data coupling and control coupling | Manual review during PR; no tool automation yet               | Documented as residual risk.                                     |
| 9     | Verification of additional code          | SOUP register under `docs/SOUP/` covers 14 third-party components                         | Per-component re-review cadence enforced (12 months).            |

### 1.6 Objective coverage summary

Of the 43 numbered objectives across Tables A-3 through A-7 above, this
repository provides at least partial evidence for 38. The five
objectives without first-draft evidence are A-3#1 (system requirements
upstream of software), A-3#3 (HIL is developer-laptop pre-push, not
self-hosted CI), A-5#8/#9 (parameter data items, not in scope), and
A-7#8 (data/control coupling automation).

---

## 2. Verification methods

DO-178C 6.3 enumerates four verification methods: review, analysis,
test, and (for Level B) requirement-based test. IEC 61508-3 Table A.5
adds dynamic analysis and probabilistic testing. The mapping for this
repository is:

### 2.1 Reviews

| Activity              | Tool / artifact                                         | Cadence            |
|-----------------------|---------------------------------------------------------|--------------------|
| Code review           | GitHub PR review on every change targeting `main`       | Per PR             |
| Document review       | Diff review on `docs/**/*.md`                           | Per PR             |
| Coding-standard check | `clang-format`, `clang-tidy`, `cppcheck` (pre-commit + CI) | Per commit + per PR |
| Naming + complexity   | `.clang-tidy` LineThreshold = 60 (NASA P10 Rule 4)      | Per commit         |
| Header hygiene        | `scripts/checks/check-since-version.py`, `scripts/checks/check-copyright.py` | Per commit |
| World-tag review      | `scripts/checks/check_world_tags.py` (warn mode today)   | Per commit         |
| HUM citation review   | `scripts/checks/cite_check.py` (warn mode today)         | Per commit         |

### 2.2 Analyses

| Activity                          | Tool / artifact                                                                | Cadence            |
|-----------------------------------|--------------------------------------------------------------------------------|--------------------|
| MISRA-C 2012 conformance          | cppcheck-misra via `scripts/checks/misra_check_inner.sh` (`make misra`)               | Quarterly + on PR  |
| Static analysis                   | `cppcheck --enable=warning,style,performance,portability` (CI + pre-commit)    | Per commit + per PR |
| Stack-usage bound                 | `-Wstack-usage`, `-fstack-usage`, `scripts/checks/stack_usage_check.py`         | Per build          |
| No dynamic allocation (NASA P10 #3) | `scripts/checks/check_no_dynamic_alloc.py`                                    | Per commit         |
| Obsolete-standard reference scan  | `scripts/checks/check_obsolete_standards.py` (rejects superseded safety-standard references) | Per commit |
| Doxygen completeness audit        | `scripts/checks/doxy_audit.py` -> `docs/DOXYGEN_GAPS.md`                           | On demand          |
| MC/DC vector pattern declaration  | `scripts/checks/check_mcdc_block.py`                                            | Per commit         |
| SOUP qualification basis review   | One Markdown file per component under `docs/SOUP/`                             | Annual per file    |

### 2.3 Tests

| Activity                          | Tool / artifact                                                                | Cadence            |
|-----------------------------------|--------------------------------------------------------------------------------|--------------------|
| Host unit tests                   | `tests/build_tests.sh`, `tests/run_tests.sh`, ctest                            | Per commit + per PR |
| Cross-compile sanity              | `firmware.yml::build-cross` matrix over every example app                      | Per PR             |
| MC/DC measurement                 | `scripts/report/mcdc_report.sh` (`make mcdc`); clang-18 `-fcoverage-mcdc`       | Per PR             |
| Branch / statement coverage       | `scripts/checks/coverage.sh --gate` (gcovr 90/90)                                     | Per PR             |
| HW-in-the-loop smoke              | `make smoke` (docs/HARDWARE_BRINGUP.md)                                        | Manual today; CI in roadmap Phase 6 |

### 2.4 Simulation

No formal model-in-the-loop simulation environment is in scope for this
release. The HAL test doubles under `libs/ra8_*_pal/` (mock register
files in host tests) provide a simulation-equivalent layer for unit
verification; they are not a replacement for hardware-in-the-loop
smoke.

QEMU is not used. The Cortex-M85 with Helium / MVE is not yet supported
by upstream QEMU at a fidelity sufficient to substitute for the
EK-RA8D2 board.

---

## 3. Verification independence

DO-178C 6.2 requires that, for Level B, certain verification
objectives be performed by personnel different from the author. This
project is a single-developer effort (Brighton Sikarskie), so formal
independence is **out of scope, permanently**, per
`docs/CERTIFICATION_SCOPE.md` (MIT-licensed personal project; paid
third-party assessor engagement is not pursued).

### 3.1 Current state

- Author = reviewer for all activities listed in Section 2.
- The independence requirement is **out of scope** per the project's
  permanent posture in `docs/CERTIFICATION_SCOPE.md`. Downstream
  adopters who require independence must engage their own assessor.
- Mitigation: every verification activity is automated (pre-commit
  hook + CI). Automation is itself a form of independent reviewer:
  the gate runs identical checks on every change irrespective of
  authorship.

### 3.2 When independence is required

The following objectives **require** personnel independence at
Level B (DO-178C Table A-3 through A-7 "with independence" columns):

- A-3 #2, #6 (HLR review for accuracy/consistency and traceability).
- A-4 #1, #6 (LLR review for compliance and traceability).
- A-5 #1, #2 (source code compliance review).
- A-7 #1, #3, #4, #5 (test-procedure correctness and coverage review).

Until an independent assessor is engaged, this SVP records these
objectives as **partially met by automated gates only**. The PSAC
captures the same gap.

### 3.3 Path to closure

Independent-assessor engagement is **out of scope, permanently**, per
`docs/CERTIFICATION_SCOPE.md`. Any downstream party that needs an
external certification claim must engage their own assessor; the
artefacts in this directory exist to make that re-use as low-friction
as possible.

---

## 4. Test environment

### 4.1 Host test environment

| Property              | Value                                                                  |
|-----------------------|------------------------------------------------------------------------|
| OS                    | Ubuntu 24.04 LTS (CI runner) and macOS 14+ (local dev)                 |
| C compiler (host)     | clang-18 with `-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc` |
| Coverage merger       | `llvm-profdata` 18                                                     |
| Coverage renderer     | `llvm-cov` 18 with `--show-mcdc-summary`                               |
| Build system          | CMake >= 3.20 + Ninja                                                  |
| Test driver           | ctest                                                                  |
| Static analyzer       | cppcheck 2.20+ with MISRA addon                                        |
| Linter                | clang-tidy 18                                                          |
| Formatter             | clang-format 18                                                        |
| Doxygen               | doxygen 1.9+ with graphviz                                             |

### 4.2 Cross-build environment

| Property              | Value                                                                  |
|-----------------------|------------------------------------------------------------------------|
| Cross compiler        | Arm GNU Toolchain 13.3.rel1 (gcc 13.3.1); pinned + enforced (#178)     |
| Cross libc            | newlib bundled in the Arm GNU Toolchain 13.3.rel1 release              |
| Build configuration   | `cmake/toolchain-ra8d2.cmake`                                          |
| Build matrix          | All `examples/ek_ra8d2/*` and `examples/_unsupported/*` with `main.c`  |

### 4.3 Hardware-in-the-loop environment

| Property              | Value                                                                  |
|-----------------------|------------------------------------------------------------------------|
| Target board          | EK-RA8D2 (Renesas part 968-K7EKA8D2S01001BE) revision v1               |
| MCU                   | Renesas R7KA8D2KFLCAC, Cortex-M85 + Cortex-M33                         |
| Debug probe           | On-board SEGGER J-Link OB, serial number 1086567198                    |
| Flash tool            | `JLinkExe` invoked via `scripts/dev/flash.sh`                              |
| Smoke harness         | `make smoke` -> `build/smoke/results.md`                               |
| PC resolution         | `arm-none-eabi-addr2line` against the per-app ELF                      |

QEMU is not used (see Section 2.4). HIL is **permanently** developer-
laptop pre-push (`docs/HIL_DEVELOPER_WORKFLOW.md`); a self-hosted CI
runner is out of scope.

---

## 5. Coverage analysis

### 5.1 Statement and branch coverage

- Tool: `gcovr` driven by `scripts/checks/coverage.sh --gate`.
- Gate: 90% line + 90% branch (the script's `--gate` mode).
- CI job: `firmware.yml::coverage`.
- Artifact: `build/coverage/coverage/` HTML report uploaded per CI run
  with 14-day retention.

### 5.2 MC/DC coverage

- Tool: clang-18 `-fcoverage-mcdc` (per `docs/MCDC.md`); fallback to
  gcc-14 `-fcondition-coverage` is **not** DO-178C-compliant and is
  used only on developer machines without modern clang.
- Driver: `scripts/report/mcdc_report.sh` (`make mcdc`).
- CI job: `firmware.yml::mcdc`.
- Gate: cannot regress below baseline at `.github/mcdc-baseline.txt`.
- Per-PR feedback: `firmware.yml::coverage-comment` posts a
  per-file MC/DC delta on every pull request.
- Current measurement: **100.00 % reachable / 92.29 % absolute**
  first-party MC/DC (per `docs/MCDC.md` measurement-history table,
 2026-05-03; 190/190 host tests pass).
- Gate: reachable MC/DC at 100% (met). Absolute target tracked
  informationally as additional wave work surfaces new decisions.
- Deactivated-decision register: 58 entries in
  `docs/MCDC_DEACTIVATIONS.md`, per DO-178C 6.4.4.3.

### 5.3 Data coupling and control coupling (DO-178C 6.4.4.2.c)

Currently verified by manual review during PR. No automated check.
Recorded as residual risk for the SVR.

### 5.4 Coverage of SOUP (DO-178C 12.1.4)

SOUP is exempt from source-level MC/DC. Each component under
`docs/SOUP/` carries a written qualification basis. Verification at
the integration boundary is via:

- Host unit tests of the wrapper layer (`libs/ra8_*_pal/`,
  `libs/ra8_tls/`, `libs/ra8_psa_crypto/` in-tree shims).
- Hardware-in-the-loop smoke via the developer-laptop pre-push
  workflow (`docs/HIL_DEVELOPER_WORKFLOW.md`).

---

## 6. Tool qualification

Per DO-178C 12.2 + DO-330, the tool qualification dossier lives at
`docs/qualification/TOOL_QUALIFICATION.md`. That document classifies
each tool as TQL-5 with a documented compensating verification step.

The summary table from `docs/QUALIFICATION_ROADMAP.md` Section 5 is
authoritative; this SVP cross-references it rather than duplicating
it. The salient points for verification planning are:

- All verification tools are TQL-5 (output is independently verified
  by another process).
- The compiler (`arm-none-eabi-gcc`) is the closest call. Mitigation
  is the HW-in-the-loop smoke (Phase 6).
- The MC/DC measurement chain (clang-18 + llvm-profdata + llvm-cov)
  is verified by spot-checking the report against hand-traced
  decisions during Phase 1 and Phase 2 closure work.

The project's permanent MISRA posture is **cppcheck-only** per
`docs/CERTIFICATION_SCOPE.md`; commercial-checker procurement is out
of scope.

---

## 7. Reused software (SOUP)

Per IEC 61508-3 sec. 7.4.2.12 and DO-178C sec. 12.1.4, every
pre-existing software component is registered under `docs/SOUP/`.
Fourteen components are catalogued today (see
`docs/SOUP/README.md` index). Each Markdown file documents:

- Upstream origin and licence.
- Pinned version (matched in `libs/third_party/`).
- Qualification basis (deployment scope + change-control statement).
- Known issues / advisories reviewed at the last review date.
- Re-review cadence (12 months maximum).

The `tests/CMakeLists.txt` MC/DC instrumentation explicitly excludes
`libs/third_party/` so the MC/DC gate measures first-party code only,
matching the DO-178C 12.1.4 division.

Verification at the SOUP integration boundary is the responsibility
of the in-tree wrapper layer (e.g. `libs/ra8_tls/` for Mbed TLS,
`libs/ra8_fs/` for FileX) and is gated by the host unit tests for that
wrapper.

---

## 8. Verification of partitioning

### 8.1 Architectural baseline

The firmware uses the Cortex-M85 TrustZone-M security extension to
partition the address space into Secure (S) and Non-Secure (NS)
worlds. The architectural baseline is described in
`docs/RING_AND_WORLD.md`; the SAU (Security Attribution Unit)
configuration that enforces the partition is in each app's
`trustzone_init.c` (e.g. `examples/ek_ra8d2/blink/trustzone_init.c`).

### 8.2 Verification activities

| Property to verify                                      | Evidence                                                              |
|---------------------------------------------------------|------------------------------------------------------------------------|
| Each function carries a world tag (S, NS, or NSC)       | `scripts/checks/check_world_tags.py` (warn mode today; strict planned)  |
| Secure-side state is unreachable from NS without veneer | `libs/ra8_nsc/` veneers + SAU config review                            |
| Veneer set is closed (no unintentional NSC exposure)    | Linker-script review; `arm-none-eabi-nm` of the secure ELF             |
| Secure faults trap to the secure exception handler      | `examples/*/secure_exception.c` per-app handler + smoke fault-injection|
| Key-vault operations occur in S only                    | `src/secure_app/key_vault.c` review + ring-tag audit                   |

### 8.3 Gaps

- World-tag check is currently advisory (`--warn`); transition to
  strict mode is tracked in roadmap Phase 4.
- Fault-injection smoke is documented in `docs/HARDWARE_BRINGUP.md`
  but not yet automated.
- A formal TrustZone partitioning argument (data-flow proof that no
  S asset can leak through an NSC veneer) is not yet authored. This
  is required by DO-178C A-4 #13 with independence and is a Phase 7
  deliverable.

---

## 9. References

- `CLAUDE.md` -- coding standard and NASA P10 / SOLID-for-C policy.
- `docs/QUALIFICATION_ROADMAP.md` -- phase plan and gap analysis.
- `docs/MCDC.md`, `docs/MCDC_GAPS.md` -- structural-coverage program.
- `docs/MISRA.md`, `docs/qualification/MISRA_DEVIATIONS.md` --
  language-subset conformance.
- `docs/STACK_USAGE.md` -- resource-bound analysis.
- `docs/SOUP/` -- pre-existing software register.
- `docs/RING_AND_WORLD.md` -- architectural partitioning baseline.
- `docs/qualification/TOOL_QUALIFICATION.md` -- tool TQL dossier.
- `.github/workflows/firmware.yml` -- CI gate definitions.
- `scripts/git/pre-commit` -- per-commit gate definitions.
- IEC 61508-3:2010 Clauses 7.9 and Annex C.
- RTCA DO-178C:2011 Sections 6 and 11.3, Annex A Tables A-3 through A-7.
- RTCA DO-330:2011 (tool qualification considerations).
- ISO 26262-6:2018 Clauses 9 and 11.
