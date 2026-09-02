# Software Verification Plan (SVP)

**Last refreshed**: 2026-08-22 (test inventory and execution evidence refresh).

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
| 3     | HLR compatible with target computer              | `docs/MEMORY_MAP.md`, `cmake/toolchain-ra8d2.cmake`, guarded HIL workflow                          | Guarded automatic HIL scheduling is enabled; release-specific rig evidence remains pending. |
| 4     | HLR verifiable                                   | Doxygen tag audit per `CLAUDE.md` rules                                                    | Archived 2026-05 result is not restamped; current release audit pending. |
| 5     | HLR conform to standards                         | `docs/STYLE_GUIDE.md`, `docs/MISRA.md`, pre-commit hook                                           | MISRA backlog tracked in `docs/qualification/MISRA_DEVIATIONS.md`.|
| 6     | HLR traceable to system reqs                     | `scripts/checks/cite_check.py` (HUM citation validator)                                            | Bidirectional trace matrix not yet generated.                    |
| 7     | Algorithms are accurate                          | Distributed test corpus under `tests/`, `apps/**/tests/`, and `examples/**/tests/`                   | Coverage of algorithm corner cases tracked via MC/DC gap list.   |

### 1.2 Table A-4 -- Verification of outputs of software design process

| Obj # | Subject                                | Evidence                                                                | Gap                                                              |
|------:|----------------------------------------|-------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | LLR comply with HLR                    | Per-module headers under `libs/*/inc/` cross-reference `docs/`          | Trace matrix pending.                                            |
| 2     | LLR are accurate, consistent           | `clang-tidy` naming + complexity gates                                  | Threshold tuning ongoing.                                        |
| 3     | LLR compatible with target computer    | `docs/STACK_USAGE.md`, `-Wstack-usage` gate, `.su` aggregator           | Heap proof: `scripts/checks/check_no_dynamic_alloc.py` already gates. |
| 4     | LLR verifiable                         | Category- and module-local `test_*.{c,cpp}` sources                     | Bidirectional app/test trace remains pending.                    |
| 5     | LLR conform to standards               | `.clang-format`, `.clang-tidy`, `docs/STYLE_GUIDE.md`                   | Pre-commit enforces; CI mirrors.                                 |
| 6     | LLR traceable to HLR                   | `scripts/checks/cite_check.py` HUM page-citation tags                    | Bidirectional trace matrix pending.                              |
| 7     | Algorithms are accurate                | Targeted category- and module-local `test_*_mcdc.c` sources             | Current MC/DC evidence must be restamped for release.            |
| 8     | Software architecture compat with HLR  | `docs/RING_AND_WORLD.md`, `docs/MEMORY_MAP.md`                          | Section 8 (Partitioning) below.                                  |
| 9     | Software architecture consistent       | `scripts/checks/check_world_tags.py --strict`                            | Strict mode is gate-enforced.                                    |
| 10    | Software architecture compat with target| Cross-build matrix in `.github/workflows/firmware.yml::build-cross`     | HW-in-the-loop pending.                                          |
| 11    | Software architecture verifiable       | Module-level integration tests under `tests/`                           | See Phase 5.                                                     |
| 12    | Software architecture conforms to std  | NASA P10 + SOLID-for-C policy in `CLAUDE.md`                            | Manual review only; no automated structural check yet.           |
| 13    | Software partitioning integrity        | TrustZone S/NS split via SAU (`examples/**/src/trustzone_init.c`)       | See Section 8.                                                   |

### 1.3 Table A-5 -- Verification of outputs of coding and integration

| Obj # | Subject                                  | Evidence                                                                                  | Gap                                                              |
|------:|------------------------------------------|-------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | Source code complies with LLR            | Source under `libs/`, `examples/`, `apps/`                                                | Trace matrix pending.                                            |
| 2     | Source code complies with architecture   | `scripts/checks/check_world_tags.py --strict`, `docs/RING_AND_WORLD.md`                    | Strict mode is gate-enforced.                                    |
| 3     | Source code is verifiable                | Dated 2026-08-22 snapshot: 693 test sources, 689 registrations on clean macOS/Linux, and Linux/devcontainer 689/689 in 8.66 s | Historical evidence only; release-specific execution log retention pending and macOS execution not claimed. |
| 4     | Source code conforms to standards        | clang-format, clang-tidy, cppcheck (MISRA via `just quality::local::misra`)               | MISRA backlog under deviation register.                          |
| 5     | Source code is traceable to LLR          | Doxygen `@see` cross-references                                                           | Audit script not yet automated.                                  |
| 6     | Source code is accurate and consistent   | `-Wall -Wextra -Werror`, `-Wstack-usage`, `clang-tidy`                                    | Zero-warning build enforced by CI.                               |
| 7     | Output of integration is complete        | `just apps::example::build all`; matrix derived by `scripts/dev/ra8_apps.py`                | Retained 118/118 historical build; current matrix and full-fleet HIL execution pending. |
| 8     | Parameter Data Items are correct         | No PDI subsystem in scope today.                                                          | Re-evaluate if a parameter file format is added later.           |
| 9     | Parameter Data Items have file structure | Same as above.                                                                            | Same as above.                                                   |

### 1.4 Table A-6 -- Testing of outputs of integration process

| Obj # | Subject                                  | Evidence                                                                                  | Gap                                                              |
|------:|------------------------------------------|-------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | Executable Object Code complies with HLR | `just hil::run` and the mode contracts in `docs/HIL_SUITE.md`                             | Path-filtered trusted changes schedule HIL automatically; release-specific results remain pending. |
| 2     | EOC robust with HLR                      | Negative-path tests in the distributed test inventory                                      | Coverage tracked per-module in MC/DC report.                     |
| 3     | EOC complies with LLR                    | Application-shape tests under `tests/mocks/` and app-local `tests/`                        | Bidirectional app/test trace remains pending.                    |
| 4     | EOC robust with LLR                      | Same as 2.                                                                                | Same as 2.                                                       |
| 5     | EOC compatible with target               | Retained historical 118/118 RA8D2 build + guarded `just hil::run`                         | Current-matrix build and real-HIL execution are pending.         |

### 1.5 Table A-7 -- Verification of verification process results

| Obj # | Subject                                  | Evidence                                                                                  | Gap                                                              |
|------:|------------------------------------------|-------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| 1     | Test procedures are correct              | PR review history + `tests/build_tests.sh`, `tests/run_tests.sh`                          | Independence pending.                                            |
| 2     | Test results are correct, discrepancies  | CI logs (GitHub Actions, retained per workflow retention policy)                          | SVR (Verification Results) document populated in roadmap Phase 7.|
| 3     | Test coverage of HLR achieved            | Linux/devcontainer `unit-tests` gate passed 689/689 in 8.66 s on 2026-08-22              | Bidirectional requirements trace remains pending.                |
| 4     | Test coverage of LLR achieved            | Same as 3 plus per-module decision tests                                                  | Same as 3.                                                       |
| 5     | Test coverage of structure achieved -- MC/DC | `just quality::local::mcdc` (clang-18 `-fcoverage-mcdc`), `docs/MCDC.md`, baseline at `.github/mcdc-baseline.txt` | Release measurement must be restamped. |
| 6     | Test coverage of structure achieved -- branch | gcovr branch coverage gated per file by `just quality::gate::run coverage-tree`                    | Release gate result pending.                                    |
| 7     | Test coverage of structure achieved -- statement | Same as 6.                                                                         | Release gate result pending.                                    |
| 8     | Test coverage of structure -- data coupling and control coupling | Manual review during PR; no tool automation yet               | Documented as residual risk.                                     |
| 9     | Verification of additional code          | 20 SOUP component qualification pages under `docs/SOUP/` (excluding README)               | Per-component re-review cadence enforced (12 months).            |

### 1.6 Objective coverage summary

The tables above are the living objective register; no separate hand-counted
closure total is claimed. Open items include system
requirements upstream of software (A-3#1), parameter data items (A-5#8/#9,
not currently in scope), and data/control coupling automation (A-7#8). The
guarded self-hosted HIL rig now supplies bounded target evidence rather than an
objective gap.

---

## 2. Verification methods

DO-178C 6.3 enumerates four verification methods: review, analysis,
test, and (for Level B) requirement-based test. IEC 61508-3 Table A.5
adds dynamic analysis and probabilistic testing. The mapping for this
repository is:

### 2.1 Reviews

| Activity              | Tool / artifact                                         | Cadence            |
|-----------------------|---------------------------------------------------------|--------------------|
| Code review           | GitHub PR review on changes targeting `dev`; reviewed release promotion from `dev` to `main` | Per PR |
| Document review       | Diff review on `docs/**/*.md`                           | Per PR             |
| Coding-standard check | `clang-format`, `clang-tidy`, `cppcheck` (pre-commit + CI) | Per commit + per PR |
| Naming + complexity   | `.clang-tidy` LineThreshold = 60 (NASA P10 Rule 4)      | Per commit         |
| Header hygiene        | `scripts/checks/check-since-version.py`, `scripts/checks/check-copyright.py` | Per commit |
| World-tag review      | `scripts/checks/check_world_tags.py --strict` (fail closed) | Per commit       |
| HUM citation review   | `scripts/checks/cite_check.py --strict` (fail closed)    | Per commit         |

### 2.2 Analyses

| Activity                          | Tool / artifact                                                                | Cadence            |
|-----------------------------------|--------------------------------------------------------------------------------|--------------------|
| MISRA-C 2012 conformance          | cppcheck-misra via `just quality::local::misra`                                    | Quarterly + on PR  |
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
| MC/DC measurement                 | `scripts/report/mcdc_report.sh` (`just quality::local::mcdc`); clang-18 `-fcoverage-mcdc`       | Per PR             |
| Branch / statement coverage       | `just quality::gate::run coverage-tree` (gcovr, per file)                             | Per PR             |
| HW-in-the-loop smoke              | `just hil::run` (`docs/HIL_SUITE.md`)                                           | Pre-push or manual workflow dispatch |

### 2.4 Simulation

No formal model-in-the-loop simulation environment is in scope for this
release. The HAL test doubles under `libs/ra8_*_pal/` (mock register
files in host tests) provide an off-target equivalent layer for unit
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
| Static analyzer       | cppcheck 2.13.0 with MISRA addon (pinned devcontainer)                 |
| Linter                | clang-tidy 18                                                          |
| Formatter             | clang-format 18                                                        |
| Doxygen               | doxygen 1.16.1 with graphviz (pinned tool authority)                  |

### 4.2 Cross-build environment

| Property              | Value                                                                  |
|-----------------------|------------------------------------------------------------------------|
| Cross compiler        | Arm GNU Toolchain 13.3.rel1 (gcc 13.3.1); pinned + enforced (#178)     |
| Cross libc            | newlib bundled in the Arm GNU Toolchain 13.3.rel1 release              |
| Build configuration   | `cmake/toolchain-ra8d2.cmake`                                          |
| Build matrix          | Live inventory from `scripts/dev/ra8_apps.py`                           |

### 4.3 Hardware-in-the-loop environment

| Property              | Value                                                                  |
|-----------------------|------------------------------------------------------------------------|
| Target board          | EK-RA8D2 (Renesas part 968-K7EKA8D2S01001BE) revision v1               |
| MCU                   | Renesas R7KA8D2KFLCAC, Cortex-M85 + Cortex-M33                         |
| Debug probe           | On-board SEGGER J-Link OB (serial in .env JLINK_SN)                    |
| Flash tool            | `JLinkExe` invoked via `scripts/hil/flash.sh`                            |
| Smoke harness         | `just hil::run`; CI/terminal log plus `/tmp/hil_all_*` diagnostics      |
| PC resolution         | `arm-none-eabi-addr2line` against the per-app ELF                      |

QEMU is not used (see Section 2.4). HIL is available as a guarded developer
run and through the dedicated native listener on the dev box, which drives the
remote instrument host. HIL-relevant pushes and trusted same-repository pull
requests schedule it automatically; manual dispatch remains available. The
current candidate's real-HIL result is pending until that hardware gate runs.

---

## 5. Coverage analysis

### 5.1 Statement and branch coverage

- Tool: `gcovr` driven by `scripts/report/tree_coverage.sh`.
- Gate: `scripts/checks/check_tree_coverage.py` -- one per-file row for every
  first-party unit; frozen debt may not grow, a new unit enters at 90/80.
- CI job: `coverage.yml::coverage-tree`.
- Artifact: `build/coverage/coverage/` HTML report uploaded per CI run
  with 14-day retention.

### 5.2 MC/DC coverage

- Tool: clang-18 `-fcoverage-mcdc` (per `docs/MCDC.md`); the gate fails closed
  when clang or matching LLVM profile tools are missing. A separately invoked
  gcc-14 condition-coverage experiment is **not** DO-178C-compliant.
- Driver: `scripts/report/mcdc_report.sh` (`just quality::local::mcdc`).
- CI job: `firmware.yml::mcdc`.
- Gate: cannot regress below baseline at `.github/mcdc-baseline.txt`.
- Per-PR feedback: `firmware.yml::coverage-comment` posts a
  per-file MC/DC delta on every pull request.
- The archived 2026-05 measurement is retained in `docs/MCDC.md`; the
  Linux/devcontainer unit gate's 689/689 pass does not restamp structural
  coverage for the migrated tree.
- Gate: the release evidence pack must provide the current reachable
  decision-complete result and llvm-cov's absolute condition-level MC/DC
  result.
- Deactivated-decision register: the generated
  `docs/MCDC_DEACTIVATIONS.md` inventory, per DO-178C 6.4.4.3. Its entry count
  is derived from the current source rather than copied into this plan.

### 5.3 Data coupling and control coupling (DO-178C 6.4.4.2.c)

Currently verified by manual review during PR. No automated check.
Recorded as residual risk for the SVR.

### 5.4 Coverage of SOUP (DO-178C 12.1.4)

SOUP is exempt from source-level MC/DC. Each component under
`docs/SOUP/` carries a written qualification basis. Verification at
the integration boundary is via:

- Host unit tests of the wrapper layer (`libs/ra8_*_pal/`,
  `libs/ra8_tls/`, `libs/ra8_psa_crypto/` in-tree shims).
- Hardware-in-the-loop smoke via the guarded Raspberry Pi 5 rig and its manual
  workflow (`docs/HIL_DEVELOPER_WORKFLOW.md`).

---

## 6. Tool qualification

Per DO-178C 12.2 + DO-330, the tool qualification dossier lives at
`docs/qualification/TOOL_QUALIFICATION.md`. That document classifies
each tool as TQL-5 with a documented compensating verification step.

The summary table from `docs/QUALIFICATION_ROADMAP.md` Section 5 is
authoritative; this SVP cross-references it rather than duplicating
it. The salient points for verification planning are:

- All verification tools are proposed at TQL-5 (output is intended to be
  independently verified by another process). The Linux/devcontainer unit
  gate passed 689/689 on 2026-08-22, while final qualification remains open for release-log
  retention, structural coverage, trace, and the other pending evidence.
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
The current component population is derived from
`scripts/gen/sbom_registry.py` and checked against the generated SBOM; no
hand-maintained count is authoritative. Each Markdown file documents:

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
`libs/ra8_cache_store/` for LevelX) and is gated by the host unit tests
for that wrapper.

---

## 8. Verification of partitioning

### 8.1 Architectural baseline

The firmware uses the Cortex-M85 TrustZone-M security extension to
partition the address space into Secure (S) and Non-Secure (NS)
worlds. The architectural baseline is described in
`docs/RING_AND_WORLD.md`; the SAU (Security Attribution Unit)
configuration that enforces the partition comes from the selected board layer
unless an app overrides it (for example,
`libs/ra8_board_ek_ra8d2/src/boot/trustzone_init.c`).

### 8.2 Verification activities

| Property to verify                                      | Evidence                                                              |
|---------------------------------------------------------|------------------------------------------------------------------------|
| Each function carries a world tag (S, NS, or NSC)       | `scripts/checks/check_world_tags.py --strict` (fail closed)             |
| Secure-side state is unreachable from NS without veneer | `libs/ra8_nsc/` veneers + SAU config review                            |
| Veneer set is closed (no unintentional NSC exposure)    | Linker-script review; `arm-none-eabi-nm` of the secure ELF             |
| Secure faults trap to the secure exception handler      | Board defaults under `libs/ra8_board_*/src/boot/secure_exception.c`, ereader override, and smoke fault-injection |
| Key-vault operations occur in S only                    | `libs/ra8_secure_app/src/key_vault.c` review + ring-tag audit                   |

### 8.3 Gaps

- World-tag checking is gate-enforced in strict mode.
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
