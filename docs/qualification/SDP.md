# Software Development Plan (SDP)

**Last refreshed**: 2026-08-22 (test inventory and execution evidence refresh).

**Status**: First draft, 2026-05-02. Authored against the Phase 7 schedule
in [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) Section 3.
**DO-178C reference**: Section 11.2.
**IEC 61508-3 reference**: Clause 7.1.2 + Clause 7.4 (software design
and development).
**ISO 26262-6 reference**: Clause 5.4 (general tailoring of the software
development process).
**Project**: `ra8-firmware`.
**Maintainer**: Brighton Sikarskie (single developer).

This SDP defines the development environment, the standards the source
must meet, and the lifecycle gates the source passes through on its way
from a contributor's working copy to the `main` branch. It is the
authoritative companion to the PSAC ([`./PSAC.md`](./PSAC.md)) and the
roadmap ([`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)).

---

## 1. Standards

### 1.1 Coding standards

The authoritative coding standard is the combination of:

- [`../../CLAUDE.md`](../../CLAUDE.md) -- enforceable rule restatement
  (C23 typed enums, no magic numbers, NASA Power-of-10 mapping, SOLID
  for C, character encoding policy, backward-compatibility policy,
  doxygen-coverage policy).
- [`../STYLE_GUIDE.md`](../STYLE_GUIDE.md) -- human-facing style guide
  (formatting, file headers, naming, `#pragma once`).
- the SRS requirement rows -- where MISRA deviations are recorded now
  (`docs/MISRA.md` and the deviation register were removed from dev).

The C language baseline is **C23** with the GCC/Clang dialect required
by `arm-none-eabi-gcc`. The standard headers (`<stdbool.h>`,
`_Static_assert`, `= {0}` zero-initialiser) explicitly forbidden by
[`../../CLAUDE.md`](../../CLAUDE.md) "Critical Rules" are not used.

### 1.2 Design standards

Architecture is governed by [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md)
(ring 0..6 layering, TrustZone S/NS/NSC world tagging) and by the
memory layout in [`../MEMORY_MAP.md`](../MEMORY_MAP.md). Per-feature
design intent is captured in the corresponding `docs/<feature>.md`
files (e.g. [`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md))
and in the per-module `@brief`/`@details` doxygen corpus
(`docs/MCDC.md` was removed from dev).

### 1.3 Requirements standards

`ra8-firmware` does not maintain a separate requirements management
system. Requirements are captured in two places:

- Per-feature design notes under `docs/` (e.g.
  [`../ARCHITECTURE.md`](../ARCHITECTURE.md)).
- Function-level `@brief` / `@param` / `@retval` / `@pre` / `@post`
  blocks, audited by review (`scripts/checks/doxy_audit.py` was
  removed from dev).

This is consistent with DO-178C Section 11.9 (software requirements
data) being an aggregate of design notes plus the source-of-truth
declarations in headers.

### 1.4 Documentation standards

Doxygen rules are stated in
[`../../CLAUDE.md`](../../CLAUDE.md) "Doxygen Documentation
Requirements" -- every applicable tag is mandatory. The current gap
report is generated under `build/reports/doxygen/`. Regenerate it for the
release evidence pack.

### 1.5 Hardware citation standard

Every register-level write must cite the Renesas Hardware User's
Manual R01UH1065EJ section that authorises the bit pattern. Citations
use the `@cite` doxygen tag; the automated strict audit
(`scripts/checks/cite_check.py`) was removed from dev, so citations are
reviewed by hand on each PR.

---

## 2. Software development environment

### 2.1 Toolchain

| Tool                       | Role                                       | Version basis        |
|----------------------------|--------------------------------------------|----------------------|
| `arm-none-eabi-gcc`        | Cortex-M85 / M33 cross-compiler            | ARM GNU Toolchain    |
| `arm-none-eabi-ld`         | Linker (per-app linker scripts)            | bundled with gcc     |
| `arm-none-eabi-objcopy`    | ELF -> HEX for `JLinkExe loadfile`         | bundled with gcc     |
| `arm-none-eabi-addr2line`  | Smoke-test PC resolution                   | bundled with gcc     |
| `arm-none-eabi-nm`         | Symbol-table extraction (diag global addrs)| bundled with gcc     |
| `cmake` >= 3.20            | Build system                               | top-level + per-app  |
| `just`                     | Build orchestrator (`just apps::build <app>`) | host                 |
| `clang-format`             | Style enforcement                          | matches `.clang-format` |
| `clang-tidy`               | Naming + complexity gate                   | matches `.clang-tidy`   |
| `clang` >= 18              | Host MC/DC instrumentation (`-fcoverage-mcdc`) | clang-18 profile in pinned devcontainer |
| `llvm-profdata`/`llvm-cov` | MC/DC measurement                          | matches clang        |
| `cppcheck` 2.13.0          | MISRA-C 2012 audit + general static check  | pinned devcontainer  |
| `python3`                  | Audit scripts under `scripts/checks/`      | 3.11+                |
| `JLinkExe`                 | Flash + halt + register dump               | installed rig version recorded with evidence |
| `gdb-multiarch`            | Interactive debug                          | host                 |

The cross-compile settings are pinned in
[`../../cmake/toolchain-ra8d2.cmake`](../../cmake/toolchain-ra8d2.cmake).
Host (test) builds use the platform-default `gcc`/`clang`.

### 2.2 Host environment

Development is performed on macOS (Apple Silicon) and Linux. A
devcontainer is checked in under [`.devcontainer/`](../../.devcontainer/) and
uses the canonical image preparation/execution helpers under `scripts/ci/`.
Native development remains supported, but qualification commands should use
the pinned environment when host tool versions differ.

### 2.3 Continuous integration

CI runs in GitHub Actions via
[`../../.github/workflows/regen-drift.yml`](../../.github/workflows/regen-drift.yml),
the only workflow on dev (the former `firmware.yml` suite and the
pre-commit hook are removed; per-commit enforcement is local). HIL uses
the dedicated native listener on the dev box to build, then drives the
Raspberry Pi 5 instrument host as a direct bench run (the guarded
workflow and its document were removed from dev).

### 2.4 Probe and target

The reference probe is the on-board J-Link OB-RA4M2 (its serial is bench
configuration, set as `JLINK_SN` in the gitignored `.env`; see
`.env.example`). Target is EK-RA8D2 v1
(R7KA8D2KFLCAC). VCOM is exposed on `/dev/cu.usbmodem<JLINK_SN>1`
on macOS and bridges SCI8 per the verified UART bring-up in
[`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md) "2026-05-02
follow-up: UART working".

---

## 3. Software development methods

### 3.1 Bare-metal HAL written from the HUM

Per [`../../CLAUDE.md`](../../CLAUDE.md) "Development Approach", every
source file in `libs/` is hand-written against the Renesas
Hardware User's Manual R01UH1065EJ. Renesas FSP source is reference-
only and is **not** copied into this tree. The cite-check script
(`scripts/checks/cite_check.py`) verifies that register-level
translation units carry HUM citations (the automated checker was
removed from dev; review-only today).

### 3.2 Vendor SOUP behind PAL layers

Third-party libraries are admitted to the build under the SOUP
register at [`../SOUP/`](../SOUP/). Each library is wrapped behind a
first-party platform abstraction layer (`libs/ra8_*_pal/`,
`libs/ra8_tls/`, `libs/ra8_ota/`, `port/netxduo/`, `port/usbx/`,
`port/levelx/`, `port/nimble/`) so that:

- The first-party code remains the only code in scope for MC/DC
  instrumentation (the MC/DC measurement was removed from dev).
- Vendor APIs can be replaced or upgraded without touching call sites
  outside the PAL.

### 3.3 No dynamic allocation in firmware

NASA Power-of-10 Rule 3 is enforced by
[`../../scripts/checks/check_freestanding_runtime.py`](../../scripts/checks/check_freestanding_runtime.py)
(the allocator-symbol gate; the former dedicated script and pre-commit
hook are removed from dev). The xorshift32 `rand()` override added in
commit `6d2ebbfac` is the canonical example of how third-party calls
into newlib heap functions are intercepted (see
[`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md) "ra8_rand_stub
fix"). Failures of this rule are recorded by `ra8_sbrk_trap` as
`fatal_error` events on hardware.

### 3.4 Per-app boot files

Each selected application carries
`examples/ek_ra8d2/<tier>/.../<app>/src/main.c` (or the RA8P1 equivalent)
and a thin root-level
`CMakeLists.txt`. Application implementations live under `src/`, interfaces
under `inc/`, and linker or manifest files at the app root. The selected
`libs/ra8_board_<board>/` layer supplies the default boot files and linker
script. An application carries a local boot or linker file only when it
intentionally overrides that default.

### 3.5 Inclusive terminology

[`../../CLAUDE.md`](../../CLAUDE.md) "Terminology Standard" mandates
controller/peripheral, COPI/CIPO, CS, primary/main throughout the
codebase. Renesas reference text is mapped in comments where it uses
legacy terms.

---

## 4. Software development lifecycle activities

### 4.1 Requirements

Captured per Section 1.3. There is no separate requirements tool; the
`@brief` corpus and the per-feature design docs serve that role. The
traceability artefact is the doxygen-generated cross reference
(produced by Phase 3 once the gap list is closed).

### 4.2 Design

Captured per Section 1.2. Architectural design lives in
[`../RING_AND_WORLD.md`](../RING_AND_WORLD.md) and
[`../MEMORY_MAP.md`](../MEMORY_MAP.md); detailed design lives in
header `@brief`/`@details` blocks.

### 4.3 Code

Code is hand-written against the standards in Section 1. Every commit
must pass:

- ASCII-only character encoding (pre-commit hook).
- Defensive macro paren check (pre-commit hook).
- `clang-format --check` (pre-commit hook).
The automated gates below were all part of the pre-commit hook and CI
suite that was removed from dev; they currently have no tool on dev and
are re-establishment items: `clang-tidy`, `cppcheck` (with the MISRA
addon), `ra8ci since`, `check-copyright.py`, `cite_check.py --strict`,
`check_world_tags.py --strict`, `check_obsolete_standards.py`, and
`check_mcdc_block.py`. Per-commit enforcement today is `clang-format`
plus the `scripts/checks/` suite (file size, comment format,
freestanding runtime, RTOS symbol isolation, SG offsets, tool versions,
lint targets, stack usage, init-order audit).

### 4.4 Test

Host-side tests are distributed across `tests/`, `apps/**/tests/`, and the
small `examples/**/tests/` population. The retained 2026-08-22 snapshot
contains 693 source files (689 C, 4 C++) and 689 CTest registrations on clean
standalone macOS and Linux configurations. Its authoritative
Linux/devcontainer unit gate passed 689/689 in 8.66 s. This is historical
evidence rather than a current-tree census. macOS execution was not claimed:
the low-address
peripheral-mock tests require Linux/container execution, as documented in
[`../TOOLCHAIN.md`](../TOOLCHAIN.md).
`tests/build_tests.sh` + `tests/run_tests.sh` run the native suite
(the `quality::` just wrappers were removed; MC/DC measurement is not
available on dev).

### 4.5 Integration

Integration uses the live `scripts/dev/ra8_apps.py` inventory, distributed
app-local and mock tests, and guarded target execution. The retained 118/118
RA8D2 build is historical evidence. The
selected-app HIL and remote-GDB lifecycle results are historical;
current-candidate target execution, the complete per-app trace, and full-fleet
execution remain pending. Hardware bring-up is documented in
[`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md).

### 4.6 Verification

See Section 6.

---

## 5. Configuration management

### 5.1 Version control

All source, scripts, plans, and reference PDFs are tracked in this
git repository. The long-lived `dev` branch is the working integration branch
and `main` is the release branch. Feature work is done in issue worktrees,
lands on `dev` after validation, and is promoted to `main` through a pull
request.
There is no separate release branch and no semantic-version tag
discipline yet (this is a personal-project codebase per
[`../../CLAUDE.md`](../../CLAUDE.md) "Backward Compatibility
Policy"); signed tags will be introduced when the SCMP
([`./SCMP.md`](./SCMP.md)) lands.

### 5.2 Commit policy

Commit messages follow a `<scope>: <subject>` convention seen in the
recent history (e.g. `secure_app+tests: properly order key_import
enums; wire ra8_psa_crypto`). Per
[`../../CLAUDE.md`](../../CLAUDE.md) "Git Commits and Pull Requests",
no AI attribution is added to commit messages or PR descriptions.

### 5.3 Backward-compatibility policy

`ra8-firmware` is a personal in-house codebase with **zero**
backward-compatibility requirements. The full policy is in
[`../../CLAUDE.md`](../../CLAUDE.md) "Backward Compatibility Policy".
Net effect on this SDP: APIs may be renamed freely in the same
commit that updates all call sites; no deprecation shims are
permitted.

### 5.4 Branch protection

`main` is protected by the `regen-drift` workflow (the only CI on
dev) and by rebase-merge policy. The pre-commit hook was removed from
dev; re-establishing the fuller gate set is a qualification gap.

---

## 6. Verification activities

### 6.1 Unit tests

`tests/build_tests.sh` + `tests/run_tests.sh` run the host-side suite.
Coverage targets are not measured on dev (the MC/DC infrastructure was
removed). Test files follow `test_*.c` or
`test_*.cpp` naming and are discovered across the root test categories and
module-local test directories by [`../../tests/CMakeLists.txt`](../../tests/CMakeLists.txt).
The retained 2026-08-22 Linux/devcontainer unit-gate result passed all 689
registered tests in 8.66 s. A release evidence pack must retain its own log; this
result does not restamp MC/DC or other coverage evidence.

### 6.2 Integration tests

Applications under [`../../examples/ek_ra8d2/`](../../examples/ek_ra8d2/)
exercise the full software stack (HAL + PAL + middleware + app). The live EIL
inventory comes from `scripts/dev/ra8_apps.py`; the 118/118 RA8D2 build and
selected-app run are retained historical evidence;
current-candidate real target execution and full-fleet execution remain
pending.

### 6.3 MC/DC measurement

No MC/DC measurement is available on dev: the driver, its baseline and
its CI job were removed. The **100.00 % reachable / 92.29 % absolute**
measurement from 2026-05 is historical and does not restamp structural
coverage for the migrated tree. When measurement is re-established, the
release evidence pack must provide a current reachable decision-complete
result.

### 6.4 Hardware smoke

HIL runs as a direct bench run on the dev-box listener and the
Raspberry Pi 5 instrument host (the `hil::run` recipe, its driver
script and the guarded workflow were removed from dev).

### 6.5 MISRA-C 2012

The MISRA audit is suspended on dev: the `misra_check_inner.sh` driver,
the cppcheck-MISRA invocation and the deviation register were removed.
The project's permanent MISRA posture is **cppcheck-only** (no
commercial checker -- LDRA / Helix QAC / Polyspace are out of scope
under the MIT-licensed personal-project policy in
[`../CERTIFICATION_SCOPE.md`](../CERTIFICATION_SCOPE.md)); deviations
are recorded in the SRS requirement rows today.

### 6.6 Stack-usage analysis

`scripts/checks/stack_usage_check.py` analyses the per-function
`.su` files emitted by `arm-none-eabi-gcc -fstack-usage`. Results
roll up into [`../STACK_USAGE.md`](../STACK_USAGE.md). This satisfies
IEC 61508-3 Annex B (control of coding-time error sources, stack
overflow).

### 6.7 Doxygen audit

The Doxygen completeness audit is suspended on dev (`doxy_audit.py` and
the Doxygen gate were removed). When re-established, the release
evidence pack must retain the regenerated result.

### 6.8 Coverage caveats

cppcheck-MISRA implements roughly two thirds of the mandatory +
required rules (the cppcheck-MISRA limitation; `docs/MISRA.md` was
removed from dev). The project's permanent policy is **cppcheck-only**;
commercial checker procurement is **out of scope** per
[`../CERTIFICATION_SCOPE.md`](../CERTIFICATION_SCOPE.md). Downstream
adopters who need full mandatory + required coverage must engage their
own qualified checker.

---

## 7. Compliance demonstration

### 7.1 Pre-commit gates (local, mandatory)

Source: the pre-commit hook (removed from dev; the table records what it enforced).

| Gate                                             | Tool / script                                              |
|--------------------------------------------------|------------------------------------------------------------|
| ASCII-only encoding                              | none on dev (the pre-commit hook was removed)              |
| C23 typed-enum + zero-init pattern               | none on dev (the pre-commit hook was removed)              |
| Defensive macro paren                            | none on dev (the pre-commit hook was removed)              |
| clang-format                                     | `clang-format --check` (the wrapper script was removed)    |
| clang-tidy                                       | none on dev (removed)                                      |
| `@since` tag presence                            | none on dev (`ra8ci` was removed) |
| Copyright header presence                        | none on dev (removed) |
| cppcheck (general)                               | none on dev (removed) |
| HUM citations                                    | none on dev (removed); review-only |
| Ring/world tags                                  | none on dev (removed); review-only |
| Roadmap stats freshness                          | `tools/roadmap_stats --check`                               |
| Obsolete-standard references                     | none on dev (removed) |
| MC/DC-blocking pattern check                     | none on dev (removed) |

### 7.2 CI gates (GitHub Actions, mandatory)

Source:
[`../../.github/workflows/firmware.yml`](../../.github/workflows/firmware.yml).

CI re-runs the pre-commit gate set on every PR plus the configured cross-build
matrix. The separate `hil.yml` workflow automatically schedules HIL-relevant
pushes and trusted same-repository pull requests on the dedicated dev-box
listener, while the shared lock serialises access to the remote bench.

### 7.3 Periodic verification (manual + roadmap-scheduled)

| Activity                                    | Cadence                | Owner    |
|---------------------------------------------|------------------------|----------|
| `just quality::local::mcdc` end-to-end                      | per Phase-1/2 sprint   | dev      |
| `just quality::local::misra`                                | quarterly              | dev      |
| `just hil::run` full sweep                     | per hardware session   | dev      |
| `scripts/checks/doxy_audit.py` regen         | per Phase-3 sprint     | dev      |
| SOUP re-review                              | <= 12 months per entry | dev      |
| Stack-usage report regen                    | per HAL change         | dev      |

---

## 8. Cross-references

- [`./PSAC.md`](./PSAC.md) -- gateway document; this SDP feeds Section 4 (life cycle) of the PSAC.
- [`../../CLAUDE.md`](../../CLAUDE.md) -- coding rules and policies.
- [`../STYLE_GUIDE.md`](../STYLE_GUIDE.md) -- human-facing style guide.
- [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md) -- architecture standard.
- [`../MISRA.md`](../MISRA.md) -- MISRA-C 2012 audit baseline.
- [`./MISRA_DEVIATIONS.md`](./MISRA_DEVIATIONS.md) -- live deviation register.
- [`../MCDC.md`](../MCDC.md) -- MC/DC instrumentation.
- [`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md) -- hardware sweep results and probe configuration.
- [`../SOUP/`](../SOUP/) -- pre-existing software register.
- [`scripts/ci/gates/checks.sh`](../../scripts/ci/gates/checks.sh) -- pre-commit gate set.
- [`../../.github/workflows/firmware.yml`](../../.github/workflows/firmware.yml) -- CI gate set.
- [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) -- 22-week schedule.
