# Software Verification Results (SVR)

**Document ID**: ra8d2-svr-001
**Version**: 0.1 (first draft, Phase 7 of `docs/QUALIFICATION_ROADMAP.md`).
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
| Host test files (`tests/test_*.c`)           | 180                  | `ls tests/test_*.c \| wc -l`                       |
| First-party MC/DC, total                     | **70.40%**           | `build/mcdc-report/summary.txt` TOTAL row          |
| MC/DC condition pairs total                  | 1196                 | same -- "MC/DC Conditions"                         |
| MC/DC condition pairs missed                 | 354                  | same -- "Missed"                                   |
| Branch coverage, total                       | 77.30%               | same TOTAL row                                     |
| Function coverage, total                     | 95.22%               | same TOTAL row                                     |
| Region coverage, total                       | 85.96%               | same TOTAL row                                     |
| Line coverage, total                         | 86.28%               | same TOTAL row                                     |
| Doxygen functions audited                    | 2666                 | `docs/DOXYGEN_GAPS.md` Summary section             |
| Doxygen functions with gaps                  | **429**              | same                                               |
| Doxygen total missing-tag instances          | 2919                 | same                                               |
| MISRA-C 2012 unique findings                 | **1271**             | `docs/MISRA.md` audit results section              |
| MISRA active deviations registered           | 5 (D-001..D-005)     | `docs/qualification/MISRA_DEVIATIONS.md`           |
| EVM-tier apps swept on hardware              | 26                   | `docs/HARDWARE_BRINGUP.md` evening + night sweeps  |
| Hardware sweep PASS                          | **20**               | same                                               |
| Hardware sweep WIP                           | **4**                | same                                               |
| Hardware sweep UNKNOWN                       | **2**                | same                                               |
| Hardware sweep FAIL                          | **0**                | same                                               |
| Hardware sweep NOBUILD                       | **0**                | same                                               |

The MC/DC fraction reported here (70.40%) is below the Phase 1
target of 100% on the hazard-path modules and below the Phase 2
target of 95% project-wide. Closure is tracked in
`docs/QUALIFICATION_ROADMAP.md` Section 3, Phases 1 and 2.

## 2. Per-test-suite results (host unit / integration tests)

The host test suite is rooted at `tests/`. Categories follow the
SVCP catalogue:

- **`tests/test_ra_*.c`** -- per-module unit tests for `libs/ra_*`
  (HAL drivers, core, security, PAL, OTA, TLS).
- **`tests/test_app_*.c`** (25 files) -- application-shape
  integration tests, one per EVM-tier app under
  `examples/ek_ra8d2/`.
- **`tests/test_lwip_sys_arch.c`, `tests/test_lx_nor_driver_ra_xspi.c`**,
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
history table, 2026-05-02 evening row) is **149 of 178 host tests
pass** (the post-evening test count of 180 includes two new files
authored after the last documented measurement run; rerun pending).
The 29 currently-failing tests are concentrated in the modules that
have known WIP integration gaps (XSPI NOR, USB-FS chapter-9, RSIP
BIST -- see Section 5).

## 3. Per-app hardware results (on-target smoke)

Source of truth: `docs/HARDWARE_BRINGUP.md`, "2026-05-02 night sweep"
section (the latest end-to-end run). Probe: on-board J-Link OB SN
1086567198 -> EK-RA8D2 v1, JLinkExe v9.38a.

| App                            | Result   | PC          | Symbol                                       |
|--------------------------------|----------|-------------|----------------------------------------------|
| blink                          | PASS     | 0x02000B2E  | `ra_delay_ms` libs/ra_core/src/ra_time.c:94  |
| blink_hal                      | PASS     | 0x02000B8A  | `ra_delay_ms` libs/ra_core/src/ra_time.c:203 |
| clock_check                    | PASS     | 0x02000BEE  | `ra_delay_ms` libs/ra_core/src/ra_time.c:203 |
| ereader                        | WIP      | 0x0200042C  | `ereader_panic_halt` main.c:524              |
| ethernet_tcp_echo              | PASS     | 0x020018AE  | `ra_delay_ms` libs/ra_core/src/ra_time.c:203 |
| lcd_demo                       | WIP      | 0x02000204  | `lcd_demo_panic_halt` main.c:353             |
| ra_bootloader                  | UNKNOWN  | 0x02000452  | `internal_write32` system_init.c:89          |
| threadx_blink                  | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| threadx_canfd_demo             | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| threadx_filex_demo             | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| threadx_filex_levelx_demo      | WIP      | 0x0200035C  | `demo_panic_halt` main.c:151                 |
| threadx_guix_demo              | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| threadx_ipc_demo               | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| threadx_levelx_demo            | WIP      | 0x02000326  | `demo_panic_halt` main.c:134                 |
| threadx_lwip_tcp_echo          | PASS     | 0x020002A4  | `__tx_ts_wait` tx_thread_schedule.S:294      |
| threadx_mpu_partition_demo     | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| threadx_netx_tcp_echo          | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| threadx_ota_demo               | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| threadx_usbx_cdc_demo          | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |
| uart_hello                     | PASS     | 0x02000CAA  | `ra_delay_ms` libs/ra_core/src/ra_time.c:94  |
| usb_cdc_echo                   | PASS     | 0x0200029A  | `__tx_ts_wait` tx_thread_schedule.S:264      |
| usb_hid_device                 | UNKNOWN  | 0x02001C8C  | `ra_usb_fs` libs/ra_hal/inc/ra8d2_usb_regs.h:297 |
| usb_host_cdc_echo              | PASS     | 0x0200117E  | `ra_delay_ms` libs/ra_core/src/ra_time.c:203 |
| usb_host_keyboard              | PASS     | 0x02001156  | `ra_delay_ms` libs/ra_core/src/ra_time.c:203 |
| usb_host_msc_browse            | PASS     | 0x0200142E  | `ra_delay_ms` libs/ra_core/src/ra_time.c:203 |
| usb_msc_device                 | PASS     | 0x020002A0  | `__tx_ts_wait` tx_thread_schedule.S:268      |

Tally: 20 PASS / 4 WIP / 2 UNKNOWN / 0 FAIL of 26 EVM apps. Zero
hard faults observed (no `Default_Handler`, `HardFault_Handler`,
`MemManage_Handler`, `BusFault_Handler`, `UsageFault_Handler`,
`SecureFault_Handler`, or `0xEFFFFFFE` lockup across all 26).

The 11 apps under `examples/_unsupported/` (audio_loopback,
ble_peripheral, motor_3phase, ptp_master, threadx_ble_central,
threadx_ble_mesh_node, threadx_https_client, threadx_nimble_peripheral,
threadx_sdcard_demo, usb_audio_device) are not part of the EVM
sweep -- they require hardware not present on a stock EK-RA8D2 v1
and are documented as such in `docs/HARDWARE_BRINGUP.md` and
`docs/VENDOR_BLOBS.md`.

## 4. Coverage results (MC/DC -- per-file)

Source: `build/mcdc-report/summary.txt` (clang-18, `-fcoverage-mcdc`,
2026-05-02 evening run). Selected high-priority and high-impact rows:

| File                                            | MC/DC Conditions | Missed | MC/DC % |
|-------------------------------------------------|-----------------:|-------:|--------:|
| `libs/ra_nsc/src/ra_nsc_log.c`                  | 21 fn / 100% br  | 0      | -       |
| `libs/ra_nsc/src/ra_nsc_ota.c`                  | 9 fn / 100% br   | 0      | 100.00% |
| `libs/ra_nsc/src/ra_nsc_xspi.c`                 | 13 fn / 100% br  | 0      | 100.00% |
| `libs/ra_nsc/src/ra_nsc_periph_init.c`          | 28 fn / 57.14% br| 12     | -       |
| `libs/ra_nsc/src/ra_nsc_key_vault.c`            | 7 fn / 100% br   | 0      | -       |
| `libs/ra_ota/src/ra_ota.c`                      | 17               | 12     | 29.41%  |
| `libs/ra_psa_crypto/src/ra_psa_crypto.c`        | 51               | 6      | 88.24%  |
| `libs/ra_reflow/src/ra_reflow_layout.c`         | 29               | 10     | 65.52%  |
| `libs/ra_reflow/src/ra_reflow_render.c`         | 2                | 2      | 0.00%   |
| `libs/ra_reflow/src/ra_reflow_xml_shim.cpp`     | 12               | 10     | 16.67%  |
| `libs/ra_tls/src/ra_tls.c`                      | 8                | 2      | 75.00%  |
| `libs/ra_touch_cal/src/ra_touch_cal.c`          | 30               | 11     | 63.33%  |
| `libs/ra_usb_pal/src/ra_usb_pal.c`              | 18               | 2      | 88.89%  |
| `libs/ra_wdt_supervisor/src/ra_wdt_supervisor.c`| 4                | 1      | 75.00%  |
| `src/secure_app/key_import.c`                   | 2                | 0      | 100.00% |
| `src/secure_app/ota_commit.c`                   | 2                | 0      | 100.00% |
| `src/secure_app/secure_trng.c`                  | 4                | 0      | 100.00% |
| **TOTAL**                                       | **1196**         | **354**| **70.40%** |

The complete per-file table (~150 rows) is preserved in
`build/mcdc-report/summary.txt`.

### Phase-1 hazard-path module status

Phase 1 of `docs/QUALIFICATION_ROADMAP.md` requires 100% MC/DC on
`ra_isr`, `ra_mpu`, `ra_xspi`, `ra_usb`, `ra_sci`, `ra_psa_crypto`.
Per `docs/MCDC_GAPS.md`:

- `ra_usb`: 11 decisions / 34 vectors outstanding (high priority).
- `ra_sci`: 8 decisions / 25 vectors outstanding (high priority).
- `ra_mpu`: 7 decisions / 21 vectors outstanding (high priority).
- `ra_xspi`: 4 decisions / 12 vectors outstanding (high priority).
- `ra_isr`: 1 decision / 3 vectors outstanding (high priority).
- `ra_psa_crypto`: 21 decisions / 72 vectors outstanding (top-10
  module table). Current MC/DC % is 88.24% per the summary above.

None of the six Phase-1 modules currently satisfies the 100%
acceptance gate. Tracked in the roadmap; not eligible for SOI-3
release until closed.

## 5. Open problem reports

The following items are open at the time of this draft and are
recorded as deferred work for the SAS to roll up.

### OP-001 -- USB device enumeration not host-visible

- **Symptom**: `usb_hid_device` boots without fault and `INTSTS0`
  ticks on the bus, but macOS never enumerates VID `0x1209`. The
  diag struct `g_ra_usb_dcd_diag` shows `dvst_count = 1`,
  `setup_count = 0`, `ctrt_count = 0` after 8 s settle.
- **Root cause** (per `docs/HARDWARE_BRINGUP.md` "USB device
  enumeration root cause"): the chapter-9 standard-request state
  machine (`GET_DESCRIPTOR` / `SET_ADDRESS` / `SET_CONFIGURATION`)
  has no handler. `ra_usb_phid_handle_setup` covers class SETUP
  only.
- **Disposition**: deferred. Multi-day port (~2000 LOC of
  `r_usb_pdriver.c` + `r_usb_pstd_*` equivalents).

### OP-002 -- LevelX xSPI NOR returns `0x00FFFFFF` for RDID

- **Symptom**: `threadx_filex_levelx_demo` and `threadx_levelx_demo`
  panic at `lx_nor_flash_format`. `g_ra_xspi_rdid_observed` reads
  `jedec_id = 0x00FFFFFF` (chip silent on the data bus).
- **Hypotheses ruled out** (per HW bring-up doc): controller is
  healthy (CMDCMP fires, dual-protocol soft-reset OK).
- **Hypothesis remaining**: pin routing in `s_xspi_octa_pins[]`
  for at least one of 12 OCTA pins, or DQS not clocked back.
  Logic-analyzer-class debug required.
- **Disposition**: deferred. Affects the two LevelX apps; both
  are classified `WIP` in the smoke sweep.

### OP-003 -- Four EVM apps in `*_panic_halt` (WIP)

- `ereader` (main.c:524), `lcd_demo` (main.c:353),
  `threadx_filex_levelx_demo` (main.c:151),
  `threadx_levelx_demo` (main.c:134).
- Each is an init-time failure caught by the app's own panic-halt
  sink. The two LevelX apps share OP-002 root cause; ereader and
  lcd_demo halt earlier in their respective init paths and need
  per-app investigation.
- **Disposition**: WIP -- treated as warning by `make smoke`,
  blocking for SOI-3 release.

### OP-004 -- BLE apps blocked on Renesas vendor patch image

- **Affected apps**: 5 BLE apps under `examples/_unsupported/`
  (`ble_peripheral`, `threadx_nimble_peripheral`,
  `threadx_ble_central`, `threadx_ble_mesh_node`, plus the
  hosted nimble work under `port/nimble/`).
- **Root cause**: the on-chip BLE controller requires the Renesas
  patch-image binary blob (NDA-distributed via FSP).
  Tracked in `docs/VENDOR_BLOBS.md`.
- **Disposition**: deferred. Requires either NDA acquisition or
  removing BLE from the v1 certification scope.

### OP-005 -- RSIP BIST returns hardware-init-failed on silicon

- **Affected app**: `threadx_https_client` (under
  `examples/_unsupported/`).
- **Root cause** (per `docs/HARDWARE_BRINGUP.md`
  "threadx_https_client RSIP BIST root cause"): the hand-rolled
  CTRL/STATUS register layout in `libs/ra_hal/inc/ra8d2_rsip_regs.h`
  is inferred from a host-sim hack and does not match the AMC
  firmware sequence the RSIP-E engine actually requires. Multi-day
  port comparable to OP-001 in scope.
- **Disposition**: deferred. Tracked alongside OP-004 because both
  block by missing vendor blobs.

### OP-006 -- Doxygen gap of 429 functions

- 429 functions across `libs/`, `src/`, `port/` are missing one or
  more required Doxygen tags (`docs/DOXYGEN_GAPS.md`).
- Worst three modules: `libs/ra_hal` (72), `port/nimble` (59),
  `port/lwip` (38).
- **Disposition**: scheduled for Phase 3 (weeks 7-10) of
  `docs/QUALIFICATION_ROADMAP.md`.

### OP-007 -- MC/DC under target

- First-party MC/DC is 70.40%, below the Phase 1 target of 100%
  on hazard-path modules and the Phase 2 target of 95% project-wide.
- **Disposition**: scheduled for Phases 1-2 of the roadmap (weeks
  1-6). Estimated 1956 additional MC/DC vectors required to close
  the gap.

### OP-008 -- `make smoke` hangs in the current bench environment

- **Symptom**: the top-level `make smoke` target hangs inside the
  `JLinkExe` invocation in the current bench setup. The "evening"
  and "night" sweeps in `docs/HARDWARE_BRINGUP.md` were both
  executed manually via the same per-app procedure the script
  uses.
- **Disposition**: bench-environment issue, not a firmware
  defect. Tracked for Phase 6 (HW-in-the-loop CI) -- the
  self-hosted runner standup will need to reproduce + fix this
  before the gate can flip green.

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

Per `docs/DOXYGEN_GAPS.md` (2026-05-02):

- Functions audited: 2666.
- Functions with at least one missing tag: 429 (16.1%).
- Total missing-tag instances: 2919.
- Most-frequently-missing tags: `@param` (500), `@post` (408),
  `@pre` (378), `@note` (372), `@details` (341), `@since` (268),
  `@retval` (258), `@brief` (209), `@return` (185).
- Worst three modules (per gap count): `libs/ra_hal` (72),
  `port/nimble` (59), `port/lwip` (38).

Acceptance gate (zero functions with gaps) not yet met. Closure
plan is Phase 3 of `docs/QUALIFICATION_ROADMAP.md`.

## 8. Anomaly log

| ID   | Severity | Module / area              | Status                                   |
|------|----------|----------------------------|------------------------------------------|
| OP-001 | Major  | USB-FS DCD chapter-9       | Open / deferred                          |
| OP-002 | Major  | LevelX xSPI NOR (LX_NOR)   | Open / deferred                          |
| OP-003 | Major  | 4 EVM apps in WIP panic    | Open / deferred                          |
| OP-004 | Critical | BLE controller patch     | Open / blocked (vendor blob)             |
| OP-005 | Critical | RSIP-E BIST              | Open / blocked (vendor blob)             |
| OP-006 | Minor  | Doxygen completeness       | Open / scheduled (Phase 3)               |
| OP-007 | Major  | MC/DC under threshold      | Open / scheduled (Phases 1-2)            |
| OP-008 | Minor  | `make smoke` hang in bench | Open / scheduled (Phase 6)               |

No anomaly currently classified as `closed` for v1 release scope.
The SAS Section 5 references this anomaly log and rolls each entry
up against the deferred-work narrative.

## 9. Change log

| Date       | Author             | Change                                            |
|------------|--------------------|---------------------------------------------------|
| 2026-05-02 | Brighton Sikarskie | Initial first-draft population (Phase 7 kickoff). |
