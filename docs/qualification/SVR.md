# Software Verification Results (SVR)

**Document ID**: ra8d2-svr-001
**Version**: 0.1 (first draft, Phase 7 of `docs/QUALIFICATION_ROADMAP.md`).
**Last refreshed**: 2026-08-22 (migration qualification evidence refresh).
**Date**: 2026-05-02.
**Author**: Brighton Sikarskie.
**DO-178C reference**: Section 11.14 (Software Verification Results).
**IEC 61508-3 reference**: Clause 7.9.6 (Verification report).
**ISO 26262-6 reference**: Clause 9.4.5 (Verification report).

## Scope

Current structurally verified outputs for the procedures listed in
`docs/qualification/SVCP.md`. The authoritative Linux/devcontainer unit gate
passed the complete 689-case suite in 8.66 s on 2026-08-22. This is not a
macOS execution claim: clean macOS configuration establishes registration
parity, while low-address tests require Linux/container execution. The release
commit and release-specific retained log remain pending evidence-pack inputs.

## 1. Verification results summary

| Metric                                       | Value                | Source                                             |
|----------------------------------------------|----------------------|----------------------------------------------------|
| Test source files                            | **693** (689 C, 4 C++) | retained 2026-08-22 snapshot                    |
| CTest registration, clean standalone macOS configure | **689**       | retained 2026-08-22 snapshot                 |
| CTest registration, clean standalone Linux configure | **689**       | retained 2026-08-22 snapshot                 |
| Linux/devcontainer 689-case host execution   | **689/689 passed**   | retained unit-gate result, 8.66 s on 2026-08-22 |
| macOS host execution                         | **Not claimed**      | low-address tests require Linux/container execution |
| EIL application inventory                    | Derived by `scripts/dev/ra8_apps.py` | live app authority                         |
| RA8D2 physical applications built            | **118/118**          | retained historical snapshot; current matrix pending |
| Real HIL execution                           | **Pending**          | run `hil-all` on the current candidate             |
| Remote GDB lifecycle                         | **Historical pass**  | restamp attach/continue/detach/stop for release    |

Coverage, documentation, and MISRA measurements remain governed by their live
artifacts and must be restamped with the release evidence pack; historical
values below are retained only for trend context.

## 2. Per-test-suite results (host unit / integration tests)

The host test suite is distributed across `tests/`, `apps/**/tests/`, and the
small `examples/**/tests/` population. Categories follow the SVCP catalogue:

- **`tests/<category>/src/test_*.c`** -- core and
  cross-module host tests for `libs/ra8_*`
  (HAL drivers, core, security, PAL, OTA, TLS).
- **`apps/**/tests/` and `examples/**/tests/`** -- app-local unit and
  integration tests; first-party libraries use the repository-level suites.
- **`tests/misc/src/test_lx_nor_driver_ra8_xspi.c`** and related category-local
  tests -- port-layer integration tests for SOUP shims under `port/`.
- **`tests/misc/src/test_coverage_compile_all.c`** -- coverage-forcing
  compile harness (every first-party TU is linked into one binary
  so the MC/DC report covers every file even if no per-module test
  exercises it yet).

`just quality::local::test` is the authoritative native entry point; a focused
or isolated run uses `just tests::local <selector>` or
`just tests::devcontainer <selector>`. Clean standalone macOS and Linux
configurations both register 689 CTest cases. The authoritative
Linux/devcontainer `unit-tests` gate passed 689/689 in 8.66 s on 2026-08-22.
The Alphabet Soup `/proc/self/mem` and closed-stdout cases remain registered
but disabled on macOS, so their Linux-only execution is not implied by that
registration parity.
No macOS execution is claimed because its address-space policy cannot run the
low-address peripheral mocks; use the Linux devcontainer as specified in
[`../TOOLCHAIN.md`](../TOOLCHAIN.md). The release pack still must retain its
own execution log.

## 3. Per-app hardware results (on-target smoke)

Candidate evidence has four distinct bounds and does not extrapolate beyond
them:

| Activity | Result |
|----------|--------|
| Emulator-in-the-loop selection | Derived at gate time by `scripts/dev/ra8_apps.py` |
| RA8D2 physical application build | Historical 118/118 snapshot; current matrix pending |
| Real target HIL execution | Pending current `hil-all` run |
| Remote GDB lifecycle | Historical pass; current-candidate restamp pending |

The HIL entry point is `just hil::run`; remote IDE validation uses
`just hil::remote_gdb`. Neither the historical selected-app result nor the
remote-GDB pass is current-candidate evidence until those hardware operations
are rerun and their logs retained.

## 4. Archived 2026-05 coverage results (MC/DC -- per-file)

Source: `build/mcdc-report/summary.txt` (clang-18, `-fcoverage-mcdc`,
2026-05-03 run). The TOTAL row reports **92.29 % absolute /
100.00 % reachable** first-party MC/DC. Selected high-priority and
high-impact rows from the prior 2026-05-02 evening snapshot are
preserved below for trend comparison; the live per-file table is in
`build/mcdc-report/summary.txt`:

| File                                            | MC/DC Conditions | Missed | MC/DC % |
|-------------------------------------------------|-----------------:|-------:|--------:|
| `libs/ra8_nsc/src/ra8_nsc_log.c`                  | 21 fn / 100% br  | 0      | -       |
| `libs/ra8_nsc/src/ra8_nsc_ota.c`                  | 9 fn / 100% br   | 0      | 100.00% |
| `libs/ra8_nsc/src/ra8_nsc_xspi.c`                 | 13 fn / 100% br  | 0      | 100.00% |
| `libs/ra8_nsc/src/ra8_nsc_periph_init.c`          | 28 fn / 57.14% br| 12     | -       |
| `libs/ra8_nsc/src/ra8_nsc_key_vault.c`            | 7 fn / 100% br   | 0      | -       |
| `libs/ra8_ota/src/ra8_ota.c`                      | 17               | 12     | 29.41%  |
| `libs/ra8_psa_crypto/src/ra8_psa_crypto.c`        | 51               | 6      | 88.24%  |
| `apps/shared_libs/reflow/src/reflow_layout.c`         | 29               | 10     | 65.52%  |
| `apps/shared_libs/reflow/src/reflow_render.c`         | 2                | 2      | 0.00%   |
| `libs/ra8_tls/src/ra8_tls.c`                      | 8                | 2      | 75.00%  |
| `libs/ra8_touch_cal/src/ra8_touch_cal.c`          | 30               | 11     | 63.33%  |
| `libs/ra8_usb_pal/src/ra8_usb_pal.c`              | 18               | 2      | 88.89%  |
| `libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c`| 4                | 1      | 75.00%  |
| `libs/ra8_secure_app/src/key_import.c`                   | 2                | 0      | 100.00% |
| `libs/ra8_secure_app/src/ota_commit.c`                   | 2                | 0      | 100.00% |
| `libs/ra8_secure_app/src/secure_trng.c`                  | 4                | 0      | 100.00% |
| **TOTAL (2026-05-03 )** | - | - | **92.29% absolute / 100.00% reachable** |

The current migrated-tree report must be regenerated; this document does not
claim that the transient `build/mcdc-report/summary.txt` still contains the
archived table.

### Phase-1 hazard-path module status

Phase 1 of `docs/QUALIFICATION_ROADMAP.md` requires 100% reachable
MC/DC on `ra8_isr`, `ra8_mpu`, `ra8_xspi`, `ra8_usb`, `ra8_sci`,
`ra8_psa_crypto`. Per `docs/MCDC_GAPS.md` and the closure
recorded in `docs/MCDC.md`: **all six modules were recorded as satisfying the
reachable-MC/DC = 100 % gate in that snapshot**. Residual absolute-MC/DC gaps were catalogued as
deactivated under DO-178C 6.4.4.3 in `docs/MCDC_DEACTIVATIONS.md`.

## 5. Archived 2026-05 problem reports

The following items are open at the time of this draft and are
recorded as deferred work for the SAS to roll up.

### OP-001 -- USB device enumeration not host-visible

- **Symptom**: `usb_hid_device` boots without fault and `INTSTS0`
  ticks on the bus, but macOS never enumerates VID `0x1209`. The
  diag struct `g_ra8_usb_dcd_diag` shows `dvst_count = 1`,
  `setup_count = 0`, `ctrt_count = 0` after 8 s settle.
- **Root cause** (per `docs/HARDWARE_BRINGUP.md` "USB device
  enumeration root cause"): the chapter-9 standard-request state
  machine (`GET_DESCRIPTOR` / `SET_ADDRESS` / `SET_CONFIGURATION`)
  has no handler. `ra8_usb_phid_handle_setup` covers class SETUP
  only.
- **Disposition**: deferred. Multi-day port (~2000 LOC of
  `r_usb_pdriver.c` + `r_usb_pstd_*` equivalents).

### OP-002 -- LevelX xSPI NOR returns `0x00FFFFFF` for RDID

- **Symptom**: `threadx_filex_levelx_demo` and `threadx_levelx_demo`
  panic at `lx_nor_flash_format`. `g_ra8_xspi_rdid_observed` reads
  `jedec_id = 0x00FFFFFF` (chip silent on the data bus).
- **Hypotheses ruled out** (per HW bring-up doc): controller is
  healthy (CMDCMP fires, dual-protocol soft-reset OK).
- **Hypothesis remaining**: pin routing in `s_xspi_octa_pins[]`
  for at least one of 12 OCTA pins, or DQS not clocked back.
  Logic-analyzer-class debug required.
- **Disposition**: deferred. Affects the two LevelX apps; both
  are classified `WIP` in the smoke sweep.

### OP-003 -- EVM apps in `*_panic_halt` (archived WIP)

- `ereader` (main.c), `lcd_demo` (main.c),
  `threadx_filex_levelx_demo` (main.c),
  `threadx_levelx_demo` (main.c).
- Each is an init-time failure caught by the app's own panic-halt
  sink. The two LevelX apps share OP-002 root cause; ereader and
  lcd_demo halt earlier in their respective init paths and need
  per-app investigation.
- **Disposition**: WIP -- treated as a warning by the archived 2026-05 smoke sweep,
  blocking for SOI-3 release.

### OP-004 -- BLE app blocked on ESP32-C6 controller

- **Affected app**: `threadx_nimble_peripheral` under
  `examples/_unsupported/`, plus the hosted NimBLE work under
  `port/nimble/`. (The first-party BLE-host facade and its
  `ble_peripheral` / `threadx_ble_central` / `threadx_ble_mesh_node`
  demos were retired; NimBLE is now the BLE host, consumed directly.)
- **Root cause**: the RA8D2 has no on-chip Bluetooth radio. BLE
  standardizes on Apache NimBLE as the host with the ESP32-C6 as the
  controller over the HCI transport seam; the C6 controller link is
  not yet wired up.
- **Disposition**: deferred. Requires wiring the ESP32-C6 controller
  link or removing BLE from the v1 certification scope.

### OP-005 -- RSIP BIST returns hardware-init-failed on silicon

- **Affected app**: `threadx_https_client` (under
  `examples/_unsupported/`).
- **Root cause** (per `docs/HARDWARE_BRINGUP.md`
  "threadx_https_client RSIP BIST root cause"): the hand-rolled
  CTRL/STATUS register layout in `libs/ra8_hal/inc/ra8_rsip_regs.h`
  is inferred from a off-target hack and does not match the AMC
  firmware sequence the RSIP-E engine actually requires. Multi-day
  port comparable to OP-001 in scope.
- **Disposition**: deferred. Tracked alongside OP-004 as a deferred,
  hardware-blocked BLE/crypto enablement item.

### OP-006 -- Doxygen gap (recorded CLOSED in the archived snapshot)

- All audited functions now carry the required Doxygen tag set
  (2747 audited, 0 with gaps -- `docs/DOXYGEN_GAPS.md`).
- **Disposition**: recorded closed for the archived snapshot; current evidence
  must be regenerated.

### OP-007 -- MC/DC under target (recorded CLOSED, reachable, in the archived snapshot)

- The archived snapshot recorded first-party reachable MC/DC = 100.00%.
  Its decision-complete rate was 92.29% when the 58 decision regions
  catalogued as deactivated under DO-178C 6.4.4.3 in
  `docs/MCDC_DEACTIVATIONS.md` remained in the denominator.
- **Disposition**: recorded closed against the then-current reachable-MC/DC gate. Absolute
  condition-level MC/DC continues to be tracked informationally as additional waves
  surface new decisions.

### OP-008 -- legacy smoke harness hung in the 2026-05 bench environment

- **Symptom**: the former top-level smoke target hung inside the
  `JLinkExe` invocation in the current bench setup. The "evening"
  and "night" sweeps in `docs/HARDWARE_BRINGUP.md` were both
  executed manually via the same per-app procedure the script
  uses.
- **Disposition**: bench-environment issue, not a firmware defect.
  The guarded replacement is `just hil::run`; HIL-relevant pushes and trusted
  same-repository PRs schedule the managed dev-box listener automatically,
  while manual dispatch remains available.

## 6. Archived 2026-05 MISRA results vs deviation register

Per `docs/MISRA.md` (2026-05-02 audit), 1271 unique violations.
Top-5 violated rules and their disposition:

| Rule              | Count | Category  | Disposition       | Deviation ID |
|-------------------|------:|-----------|-------------------|--------------|
| misra-c2012-15.5  | 751   | Advisory  | Project deviation | D-001        |
| misra-c2012-8.4   | 196   | Required  | Tooling gap       | D-005        |
| misra-c2012-17.3  | 170   | Mandatory | Tooling gap       | D-002        |
| misra-c2012-12.1  | 101   | Advisory  | Partial deviation | D-004        |
| misra-c2012-9.2   | 35    | Required  | Tooling gap       | D-003        |
| (long-tail rules) | ~18   | Mixed     | Per-finding triage pending | -    |

D-001 through D-012 each carry a written rationale, alternative
mitigation, and reviewer sign-off in
`docs/qualification/MISRA_DEVIATIONS.md`. D-006 through D-012 were
registered after the audit snapshot above; their current populations
are tracked in the register's deviation index. In the archived closure pass,
Rule 12.1 was the code-change disposition: 101 advisory findings were closed
under D-004 by adding reviewed redundant-parenthesis dispositions, reducing
the then-current 1371-finding population to 1271. Those figures remain history,
not the current cppcheck 2.13.0 population.

The remaining ~18 long-tail findings (rules 13.3, 8.9, 18.4, 10.8,
17.8, 17.7, 7.3, 21.x) await per-finding triage in the next quarterly
audit.

## 7. Archived 2026-05 Doxygen audit results

Per `docs/DOXYGEN_GAPS.md` (2026-05-03 refresh):

- Functions audited: **2747**.
- Functions with at least one missing tag: **0**.
- Total missing-tag instances: 0.

The acceptance gate was recorded as met for that snapshot. Current migrated-tree
evidence must be regenerated before Phase 3 can be claimed closed for release.

## 8. Archived 2026-05 anomaly log

| ID   | Severity | Module / area              | Status                                   |
|------|----------|----------------------------|------------------------------------------|
| OP-001 | Major  | USB-FS DCD chapter-9       | Open / deferred                          |
| OP-002 | Major  | LevelX xSPI NOR (LX_NOR)   | Open / deferred                          |
| OP-003 | Major  | EVM apps in WIP panic      | Open / deferred                          |
| OP-004 | Critical | BLE controller patch     | CLOSED INVALID (`6f6209a95`): no on-chip BLE radio, no such patch image |
| OP-005 | Critical | RSIP-E BIST              | Open / blocked (not vendored; public FSP, BSD-3-Clause) |
| OP-006 | Minor  | Doxygen completeness       | CLOSED (0 gaps, 2747 audited)            |
| OP-007 | Major  | MC/DC under threshold      | CLOSED reachable (100.00%); absolute 92.29% tracked informationally |
| OP-008 | Minor  | Legacy smoke harness hang | Superseded by guarded `just hil::run` (`docs/HIL_DEVELOPER_WORKFLOW.md`) |

This table is retained as history and is not the anomaly status for the current
release evidence pack. SAS Section 5 separates the archived bench sweep from
the bounded current build, HIL, and remote-GDB evidence.

## 9. Change log

| Date       | Author             | Change                                            |
|------------|--------------------|---------------------------------------------------|
| 2026-05-02 | Brighton Sikarskie | Initial first-draft population (Phase 7 kickoff). |
| 2026-05-03 | Brighton Sikarskie | Refreshed the then-current coverage, documentation, MISRA, and HIL snapshot. |
| 2026-08-21 | Brighton Sikarskie | Replaced flat-test and legacy sweep claims with the 692-source / 673-registration floor and bounded current build, HIL, and remote-GDB evidence. |
| 2026-08-21 | Brighton Sikarskie | Recorded the authoritative Linux/devcontainer 673/673 pass in 46.92 s and clarified that macOS evidence is registration-only. |
| 2026-08-22 | Brighton Sikarskie | Added the runtime-provisioner test and recorded the 693-source / 689-registration floor and Linux/devcontainer 689/689 pass in 8.66 s. |
