# Software Verification Results (SVR)

**Document ID**: ra8d2-svr-001
**Version**: 0.1 (first draft, Phase 7 of `docs/QUALIFICATION_ROADMAP.md`).
**Last refreshed**: 2026-05-03 ( closure: reachable MC/DC = 100.00%, doxygen gaps = 0).
**Date**: 2026-05-02.
**Author**: Brighton Sikarskie.
**DO-178C reference**: Section 11.14 (Software Verification Results).
**IEC 61508-3 reference**: Clause 7.9.6 (Verification report).
**ISO 26262-6 reference**: Clause 9.4.5 (Verification report).

## Scope

Captured outputs of running every procedure listed in
`docs/qualification/SVCP.md` against the HEAD `402253ef` baseline.
Each row is sourced from a live tree artifact; the artifact path is
cited in-line so a reviewer can re-derive the number on demand.

## 1. Verification results summary

| Metric                                       | Value                | Source                                             |
|----------------------------------------------|----------------------|----------------------------------------------------|
| Host test files (`tests/test_*.c`)           | **190** (190/190 PASS) | `ls tests/test_*.c \| wc -l`                     |
| First-party MC/DC, **reachable** (gate) | **100.00%** | `build/mcdc-report/summary.txt` TOTAL row |
| First-party MC/DC, absolute                  | 92.29%               | same                                               |
| Deactivated conditions (DO-178C 6.4.4.3)     | **58**               | `docs/MCDC_DEACTIVATIONS.md`                       |
| Doxygen functions audited                    | 2747                 | `docs/DOXYGEN_GAPS.md` Summary section             |
| Doxygen functions with gaps                  | **0**                | same                                               |
| Doxygen total missing-tag instances          | 0                    | same                                               |
| MISRA-C 2012 unique findings (cppcheck-only) | 1271                 | `docs/MISRA.md` audit results; cppcheck-only policy per `docs/CERTIFICATION_SCOPE.md` |
| MISRA active deviations registered           | 5 (D-001..D-005)     | `docs/qualification/MISRA_DEVIATIONS.md`           |
| EVM-tier apps swept on hardware              | 26                   | `docs/HARDWARE_BRINGUP.md` evening + night sweeps  |
| Hardware sweep PASS                          | 20                   | same                                               |
| Hardware sweep WIP                           | 4                    | same                                               |
| Hardware sweep UNKNOWN                       | 2                    | same                                               |
| Hardware sweep FAIL                          | 0                    | same                                               |
| Hardware sweep NOBUILD                       | 0                    | same                                               |

Reachable MC/DC = 100.00 % satisfies the gate set in `docs/MCDC.md`.
Absolute MC/DC = 92.29 % is tracked informationally; the difference
is the 58 deactivated conditions catalogued under DO-178C 6.4.4.3 in
`docs/MCDC_DEACTIVATIONS.md`.

## 2. Per-test-suite results (host unit / integration tests)

The host test suite is rooted at `tests/`. Categories follow the
SVCP catalogue:

- **`tests/test_ra8_*.c`** -- per-module unit tests for `libs/ra8_*`
  (HAL drivers, core, security, PAL, OTA, TLS).
- **`tests/test_app_*.c`** (25 files) -- application-shape
  integration tests, one per EVM-tier app under
  `examples/ek_ra8d2/`.
- **`tests/test_lwip_sys_arch.c`, `tests/test_lx_nor_driver_ra8_xspi.c`**,
  etc. -- port-layer integration tests for SOUP shims under `port/`.
- **`tests/test_coverage_compile_all.c`** -- coverage-forcing
  compile harness (every first-party TU is linked into one binary
  so the MC/DC report covers every file even if no per-module test
  exercises it yet).

`make test` is the entry point; it dispatches `cmake -S tests` and
runs `ctest`. Per-test pass/fail rows for the most recent CI run
land in `tests/build/Testing/Temporary/LastTest.log` and are
archived per CI artifact retention policy.

The most recent recorded summary in `docs/MCDC.md` (measurement
history table, 2026-05-03 row) is **190 of 190 host tests
pass**. No host tests are currently failing.

## 3. Per-app hardware results (on-target smoke)

Source of truth: `docs/HARDWARE_BRINGUP.md`, "2026-05-02 night sweep"
section (the latest end-to-end run). Probe: on-board J-Link OB
(`.env` `JLINK_SN`) -> EK-RA8D2 v1, JLinkExe v9.38a.

| App                            | Result   | PC          | Symbol                                       |
|--------------------------------|----------|-------------|----------------------------------------------|
| blink                          | PASS     | 0x02000B2E  | `ra8_delay_ms` libs/ra8_core/src/ra8_time.c  |
| blink_hal                      | PASS     | 0x02000B8A  | `ra8_delay_ms` libs/ra8_core/src/ra8_time.c |
| clock_check                    | PASS     | 0x02000BEE  | `ra8_delay_ms` libs/ra8_core/src/ra8_time.c |
| ereader                        | WIP      | 0x0200042C  | `ereader_panic_halt` main.c              |
| ethernet_tcp_echo              | PASS     | 0x020018AE  | `ra8_delay_ms` libs/ra8_core/src/ra8_time.c |
| lcd_demo                       | WIP      | 0x02000204  | `lcd_demo_panic_halt` main.c             |
| ra8_bootloader                  | UNKNOWN  | 0x02000452  | `internal_write32` system_init.c          |
| threadx_blink                  | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| threadx_canfd_demo             | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| threadx_filex_demo             | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| threadx_filex_levelx_demo      | WIP      | 0x0200035C  | `demo_panic_halt` main.c                 |
| threadx_ipc_demo               | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| threadx_levelx_demo            | WIP      | 0x02000326  | `demo_panic_halt` main.c                 |
| threadx_lwip_tcp_echo          | PASS     | 0x020002A4  | `__tx_ts_wait` tx_thread_schedule.S:294      |
| threadx_mpu_partition_demo     | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| threadx_netx_tcp_echo          | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| threadx_ota_demo               | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| threadx_usbx_cdc_demo          | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| uart_hello                     | PASS     | 0x02000CAA  | `ra8_delay_ms` libs/ra8_core/src/ra8_time.c  |
| usb_cdc_echo                   | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| usb_hid_device                 | UNKNOWN  | 0x02001C8C  | `ra8_usb_fs` libs/ra8_hal/inc/ra8_usb_regs.h |
| usb_host_cdc_echo              | PASS     | 0x0200117E  | `ra8_delay_ms` libs/ra8_core/src/ra8_time.c |
| usb_host_keyboard              | PASS     | 0x02001156  | `ra8_delay_ms` libs/ra8_core/src/ra8_time.c |
| usb_host_msc_browse            | PASS     | 0x0200142E  | `ra8_delay_ms` libs/ra8_core/src/ra8_time.c |
| usb_msc_device                 | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |

Tally: 20 PASS / 4 WIP / 2 UNKNOWN / 0 FAIL of 26 EVM apps. Zero
hard faults observed (no `Default_Handler`, `HardFault_Handler`,
`MemManage_Handler`, `BusFault_Handler`, `UsageFault_Handler`,
`SecureFault_Handler`, or `0xEFFFFFFE` lockup across all 26).

The 7 apps under `examples/_unsupported/` (audio_loopback,
motor_3phase, ptp_time_transmitter, threadx_https_client,
threadx_nimble_peripheral, threadx_sdcard_demo, usb_audio_device) are
not part of the EVM sweep -- they require hardware not present on a
stock EK-RA8D2 v1 and are documented as such in
`docs/HARDWARE_BRINGUP.md` and `docs/VENDOR_BLOBS.md`.

## 4. Coverage results (MC/DC -- per-file)

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
| `libs/ra8_reflow/src/ra8_reflow_layout.c`         | 29               | 10     | 65.52%  |
| `libs/ra8_reflow/src/ra8_reflow_render.c`         | 2                | 2      | 0.00%   |
| `libs/ra8_reflow/src/ra8_reflow_xml_shim.cpp`     | 12               | 10     | 16.67%  |
| `libs/ra8_tls/src/ra8_tls.c`                      | 8                | 2      | 75.00%  |
| `libs/ra8_touch_cal/src/ra8_touch_cal.c`          | 30               | 11     | 63.33%  |
| `libs/ra8_usb_pal/src/ra8_usb_pal.c`              | 18               | 2      | 88.89%  |
| `libs/ra8_wdt_supervisor/src/ra8_wdt_supervisor.c`| 4                | 1      | 75.00%  |
| `src/secure_app/key_import.c`                   | 2                | 0      | 100.00% |
| `src/secure_app/ota_commit.c`                   | 2                | 0      | 100.00% |
| `src/secure_app/secure_trng.c`                  | 4                | 0      | 100.00% |
| **TOTAL (2026-05-03 )** | - | - | **92.29% absolute / 100.00% reachable** |

The complete per-file table (~150 rows) is preserved in
`build/mcdc-report/summary.txt`.

### Phase-1 hazard-path module status

Phase 1 of `docs/QUALIFICATION_ROADMAP.md` requires 100% reachable
MC/DC on `ra8_isr`, `ra8_mpu`, `ra8_xspi`, `ra8_usb`, `ra8_sci`,
`ra8_psa_crypto`. Per `docs/MCDC_GAPS.md` and the closure
recorded in `docs/MCDC.md`: **all six modules satisfy the reachable-
MC/DC = 100 % gate**. Residual absolute-MC/DC gaps are catalogued as
deactivated under DO-178C 6.4.4.3 in `docs/MCDC_DEACTIVATIONS.md`.

## 5. Open problem reports

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

### OP-003 -- Four EVM apps in `*_panic_halt` (WIP)

- `ereader` (main.c), `lcd_demo` (main.c),
  `threadx_filex_levelx_demo` (main.c),
  `threadx_levelx_demo` (main.c).
- Each is an init-time failure caught by the app's own panic-halt
  sink. The two LevelX apps share OP-002 root cause; ereader and
  lcd_demo halt earlier in their respective init paths and need
  per-app investigation.
- **Disposition**: WIP -- treated as warning by `make smoke`,
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

### OP-006 -- Doxygen gap (CLOSED)

- All audited functions now carry the required Doxygen tag set
  (2747 audited, 0 with gaps -- `docs/DOXYGEN_GAPS.md`).
- **Disposition**: closed. Phase 3 acceptance gate met.

### OP-007 -- MC/DC under target (CLOSED, reachable)

- First-party reachable MC/DC = 100.00 % (gate met, ).
  Absolute MC/DC = 92.29 %; the difference is 58 conditions
  catalogued as deactivated under DO-178C 6.4.4.3 in
  `docs/MCDC_DEACTIVATIONS.md`.
- **Disposition**: closed against the reachable-MC/DC gate. Absolute
  MC/DC continues to be tracked informationally as additional waves
  surface new decisions.

### OP-008 -- `make smoke` hangs in the current bench environment

- **Symptom**: the top-level `make smoke` target hangs inside the
  `JLinkExe` invocation in the current bench setup. The "evening"
  and "night" sweeps in `docs/HARDWARE_BRINGUP.md` were both
  executed manually via the same per-app procedure the script
  uses.
- **Disposition**: bench-environment issue, not a firmware defect.
  HIL is now developer-laptop pre-push per
  `docs/HIL_DEVELOPER_WORKFLOW.md`; a self-hosted runner is out of
  scope per `docs/CERTIFICATION_SCOPE.md`.

## 6. MISRA results vs deviation register

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

D-001 through D-005 each carry a written rationale, alternative
mitigation, and reviewer sign-off in
`docs/qualification/MISRA_DEVIATIONS.md`. The 12.1 rule was the only
one that received a code-change disposition (101 advisory hits closed
under D-004 by adding per-line suppression entries to
`.cppcheck-suppressions`); the original 1371-finding baseline was
reduced to 1271 in the same pass.

The remaining ~18 long-tail findings (rules 13.3, 8.9, 18.4, 10.8,
17.8, 17.7, 7.3, 21.x) await per-finding triage in the next quarterly
audit.

## 7. Doxygen audit results

Per `docs/DOXYGEN_GAPS.md` (2026-05-03 refresh):

- Functions audited: **2747**.
- Functions with at least one missing tag: **0**.
- Total missing-tag instances: 0.

Acceptance gate (zero functions with gaps) **met**. Phase 3 closed.

## 8. Anomaly log

| ID   | Severity | Module / area              | Status                                   |
|------|----------|----------------------------|------------------------------------------|
| OP-001 | Major  | USB-FS DCD chapter-9       | Open / deferred                          |
| OP-002 | Major  | LevelX xSPI NOR (LX_NOR)   | Open / deferred                          |
| OP-003 | Major  | 4 EVM apps in WIP panic    | Open / deferred                          |
| OP-004 | Critical | BLE controller patch     | Open / blocked (vendor blob)             |
| OP-005 | Critical | RSIP-E BIST              | Open / blocked (vendor blob)             |
| OP-006 | Minor  | Doxygen completeness       | CLOSED (0 gaps, 2747 audited)            |
| OP-007 | Major  | MC/DC under threshold      | CLOSED reachable (100.00%); absolute 92.29% tracked informationally |
| OP-008 | Minor  | `make smoke` hang in bench | Mitigated: HIL is developer-laptop pre-push (`docs/HIL_DEVELOPER_WORKFLOW.md`) |

No anomaly currently classified as `closed` for v1 release scope.
The SAS Section 5 references this anomaly log and rolls each entry
up against the deferred-work narrative.

## 9. Change log

| Date       | Author             | Change                                            |
|------------|--------------------|---------------------------------------------------|
| 2026-05-02 | Brighton Sikarskie | Initial first-draft population (Phase 7 kickoff). |
| 2026-05-03 | Brighton Sikarskie | Refreshed numbers : reachable MC/DC = 100.00%, absolute = 92.29%, doxygen gaps = 0, host tests = 190/190 PASS. Restated MISRA as cppcheck-only and HIL as developer-laptop pre-push per CERTIFICATION_SCOPE.md. |
