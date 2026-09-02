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
- [`../MISRA.md`](../MISRA.md) -- MISRA-C 2012 audit baseline,
  cppcheck-MISRA limitations, and the deviation workflow per
  MISRA-C:2012 Section 5.2.
- [`./MISRA_DEVIATIONS.md`](./MISRA_DEVIATIONS.md) -- the live deviation
  register (D-001 .. D-005 today).

The C language baseline is **C23** with the GCC/Clang dialect required
by `arm-none-eabi-gcc`. The standard headers (`<stdbool.h>`,
`_Static_assert`, `= {0}` zero-initialiser) explicitly forbidden by
[`../../CLAUDE.md`](../../CLAUDE.md) "Critical Rules" are not used.

### 1.2 Design standards

Architecture is governed by [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md)
(ring 0..6 layering, TrustZone S/NS/NSC world tagging) and by the
memory layout in [`../MEMORY_MAP.md`](../MEMORY_MAP.md). Per-feature
design intent is captured in the corresponding `docs/<feature>.md`
files (e.g. [`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md),
[`../MCDC.md`](../MCDC.md)) and in the per-module `@brief`/`@details`
doxygen corpus.

### 1.3 Requirements standards

`ra8-firmware` does not maintain a separate requirements management
system. Requirements are captured in two places:

- Per-feature design notes under `docs/` (e.g.
  [`../ARCHITECTURE.md`](../ARCHITECTURE.md)).
- Function-level `@brief` / `@param` / `@retval` / `@pre` / `@post`
  blocks, audited by `scripts/checks/doxy_audit.py`.

This is consistent with DO-178C Section 11.9 (software requirements
data) being an aggregate of design notes plus the source-of-truth
declarations in headers.

### 1.4 Documentation standards

Doxygen rules are stated in
[`../../CLAUDE.md`](../../CLAUDE.md) "Doxygen Documentation
Requirements" -- every applicable tag is mandatory. The current gap
list is [`../DOXYGEN_GAPS.md`](../DOXYGEN_GAPS.md). Its archived measurement
must be regenerated for the release evidence pack.

### 1.5 Hardware citation standard

Every register-level write must cite the Renesas Hardware User's
Manual R01UH1065EJ section that authorises the bit pattern. Citations
use the `@cite` doxygen tag and are audited in strict mode by
`scripts/checks/cite_check.py`.

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
[`../../.github/workflows/firmware.yml`](../../.github/workflows/firmware.yml).
The pre-commit hook ([`../../scripts/git/pre-commit`](../../scripts/git/pre-commit))
enforces the same gates locally. Hardware-in-the-loop uses the dedicated native
listener on the dev box to build, then drives the guarded Raspberry Pi 5
instrument host documented in
[`../HIL_DEVELOPER_WORKFLOW.md`](../HIL_DEVELOPER_WORKFLOW.md). HIL-relevant
pushes and trusted same-repository pull requests schedule `.github/workflows/hil.yml`
automatically; manual dispatch and selected local-app runs remain available.

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
translation units carry HUM citations.

### 3.2 Vendor SOUP behind PAL layers

Third-party libraries are admitted to the build under the SOUP
register at [`../SOUP/`](../SOUP/). Each library is wrapped behind a
first-party platform abstraction layer (`libs/ra8_*_pal/`,
`libs/ra8_tls/`, `libs/ra8_ota/`, `port/netxduo/`, `port/usbx/`,
`port/levelx/`, `port/nimble/`) so that:

- The first-party code remains the **only** code in scope for MC/DC
  instrumentation (see [`../MCDC.md`](../MCDC.md) "Currently exempted
  code (SOUP)").
- Vendor APIs can be replaced or upgraded without touching call sites
  outside the PAL.

### 3.3 No dynamic allocation in firmware

NASA Power-of-10 Rule 3 is enforced by
[`../../scripts/checks/check_no_dynamic_alloc.py`](../../scripts/checks/check_no_dynamic_alloc.py)
in the pre-commit hook. The xorshift32 `rand()` override added in
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
- `clang-tidy --check` (pre-commit hook).
- `cppcheck` general checks (pre-commit hook; MISRA addon is `just quality::local::misra`
  only -- see Section 6).
- `check-since-version.py` (every public symbol carries a `@since`
  tag; pre-commit hook).
- `check-copyright.py` (every source file carries the project header;
  pre-commit hook).
- `cite_check.py --strict` (HUM citations on register code; fail-closed
  pre-commit policy).
- `check_world_tags.py --strict` (ring/world tag discipline; fail-closed
  pre-commit policy).
- `check_obsolete_standards.py` (rejects references to superseded
  safety standards; pre-commit hook).
- `check_mcdc_block.py` (MC/DC-blocking patterns; pre-commit hook).

The pre-commit hook is at
[`../../scripts/git/pre-commit`](../../scripts/git/pre-commit) and is
the source of truth for the gate set.

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
`just quality::local::test` runs the native suite, and
`just quality::local::mcdc` re-runs it under clang's MC/DC instrumentation.

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

`main` is protected by the CI gate set
([`../../.github/workflows/firmware.yml`](../../.github/workflows/firmware.yml))
and by the local pre-commit hook. The hook is committed under
[`../../scripts/git/`](../../scripts/git/) and must be installed by
each contributor with `just hooks`.

---

## 6. Verification activities

### 6.1 Unit tests

`just quality::local::test` runs the host-side suite. Coverage targets are tracked in
[`../MCDC.md`](../MCDC.md) (MC/DC) and the broader gap list is in
[`../MCDC_GAPS.md`](../MCDC_GAPS.md). Test files follow `test_*.c` or
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

`just quality::local::mcdc` invokes
[`../../scripts/report/mcdc_report.sh`](../../scripts/report/mcdc_report.sh)
which builds tests with `clang -fcoverage-mcdc`, runs each test
binary with `LLVM_PROFILE_FILE` set, merges via `llvm-profdata
merge -sparse`, and renders the report under `build/mcdc-report/`.
Default threshold is 100 % (per DO-178C Section 6.4.4.2 for Level B);
the threshold is overridable via `RA8_MCDC_THRESHOLD=NN` for
intermediate phase gates. The **100.00 % reachable / 92.29 % absolute**
measurement from 2026-05 is historical; use the current gate output for the
candidate under review (see [`../MCDC.md`](../MCDC.md) measurement history).
The archived 2026-05 result classified 58 conditions as deactivated under
DO-178C 6.4.4.3. The current decision set is derived in
[`../MCDC_DEACTIVATIONS.md`](../MCDC_DEACTIVATIONS.md).

### 6.4 Hardware smoke

`just hil::run` invokes
[`../../scripts/hil/all.sh`](../../scripts/hil/all.sh)
which auto-discovers every app under
`examples/ek_ra8d2/hw_validated/hil/` carrying a `hil.conf`, flashes
each to the bench board, and verifies it by the mode that `hil.conf`
declares (`uart_scrape`, `jlink_memprobe`, `usb_cdc`, `usb_hid`,
`usb_msc`, ...). An app under `hil/` with no `hil.conf` fails the run
rather than being skipped.
Hardware-in-the-loop is guarded by the shared bench lock. HIL-relevant pushes
and trusted same-repository pull requests schedule the managed dev-box listener
automatically; a developer checkout and manual workflow dispatch remain
available for targeted operation. GitHub Actions retains the current run logs
and results as the execution evidence; this plan does not freeze a historical
pass count.

### 6.5 MISRA-C 2012

`just quality::local::misra` invokes
[`../../scripts/checks/misra_check_inner.sh`](../../scripts/checks/misra_check_inner.sh)
which runs cppcheck with the MISRA addon and writes
`build/misra/results.txt`. The current result and ratchet are produced by the
gate and must not be copied from the archived 2026-05 audit. The deviation register at
[`./MISRA_DEVIATIONS.md`](./MISRA_DEVIATIONS.md) tracks the formal
disposition (D-001 .. D-005 today). The project's permanent MISRA
posture is **cppcheck-only** (no commercial checker -- LDRA / Helix
QAC / Polyspace are out of scope under the MIT-licensed personal-
project policy in [`../CERTIFICATION_SCOPE.md`](../CERTIFICATION_SCOPE.md)).

### 6.6 Stack-usage analysis

`scripts/checks/check_stack_usage.sh` plus
`scripts/checks/stack_usage_check.py` analyse the per-function
`.su` files emitted by `arm-none-eabi-gcc -fstack-usage`. Results
roll up into [`../STACK_USAGE.md`](../STACK_USAGE.md). This satisfies
IEC 61508-3 Annex B (control of coding-time error sources, stack
overflow).

### 6.7 Doxygen audit

`scripts/checks/doxy_audit.py` walks `libs/`, `port/`, `tools/`, `apps/` (third
party excluded) and reports per-function missing-tag counts to
[`../DOXYGEN_GAPS.md`](../DOXYGEN_GAPS.md). The release evidence pack must
regenerate and restamp the result.

### 6.8 Coverage caveats

cppcheck-MISRA implements roughly two thirds of the mandatory +
required rules ([`../MISRA.md`](../MISRA.md) "The cppcheck-MISRA
limitation"). The project's permanent policy is **cppcheck-only**;
commercial checker procurement is **out of scope** per
[`../CERTIFICATION_SCOPE.md`](../CERTIFICATION_SCOPE.md). Downstream
adopters who need full mandatory + required coverage must engage their
own qualified checker.

---

## 7. Compliance demonstration

### 7.1 Pre-commit gates (local, mandatory)

Source: [`../../scripts/git/pre-commit`](../../scripts/git/pre-commit).

| Gate                                             | Tool / script                                              |
|--------------------------------------------------|------------------------------------------------------------|
| ASCII-only encoding                              | inline check in pre-commit hook                            |
| C23 typed-enum + zero-init pattern               | inline check in pre-commit hook                            |
| Defensive macro paren                            | inline check in pre-commit hook                            |
| clang-format                                     | `scripts/checks/format_code.sh --check`                           |
| clang-tidy                                       | `scripts/checks/clang_tidy.sh --check`                            |
| `@since` tag presence                            | `scripts/checks/check-since-version.py`                     |
| Copyright header presence                        | `scripts/checks/check-copyright.py`                         |
| cppcheck (general)                               | `cppcheck --enable=warning,style,performance,portability`  |
| HUM citations                                    | `scripts/checks/cite_check.py --strict`                     |
| Ring/world tags                                  | `scripts/checks/check_world_tags.py --strict`               |
| Roadmap stats freshness                          | `scripts/report/roadmap_stats.py --check`                   |
| Obsolete-standard references                     | `scripts/checks/check_obsolete_standards.py`                |
| MC/DC-blocking pattern check                     | `scripts/checks/check_mcdc_block.py`                        |

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
- [`../../scripts/git/pre-commit`](../../scripts/git/pre-commit) -- pre-commit gate set.
- [`../../.github/workflows/firmware.yml`](../../.github/workflows/firmware.yml) -- CI gate set.
- [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) -- 22-week schedule.
