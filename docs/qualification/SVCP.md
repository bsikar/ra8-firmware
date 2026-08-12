# Software Verification Cases & Procedures (SVCP)

**Document ID**: ra8d2-svcp-001
**Version**: 0.1 (first draft, Phase 7 of `docs/QUALIFICATION_ROADMAP.md`).
**Last refreshed**: 2026-05-03 (test counts and HIL posture re-synced).
**Date**: 2026-05-02.
**Author**: Brighton Sikarskie.
**DO-178C reference**: Section 11.13 (Software Verification Cases and
Procedures).
**IEC 61508-3 reference**: Clause 7.9.2 (Software verification --
test specification).
**ISO 26262-6 reference**: Clause 9.4 (Verification of software
unit and integration).

## Scope

This document is the static verification specification: it lists
every test case run against the codebase, describes the procedure
for executing each verification activity, and binds each verification
case to the requirement (or DO-178C Annex A objective) it discharges.
The captured outputs of running these procedures live in
`docs/qualification/SVR.md`.

The verification cases below cover three layers:

1. Host-side unit and module tests under `tests/test_*.c`
   (190 files at the 2026-05-03 refresh; 190/190 PASS), executed
   natively on the host under clang-18 + ctest with MC/DC
   instrumentation enabled.
2. Host-side application-shape integration tests under
   `tests/test_app_*.c` (25 files), one per EVM-tier app.
3. On-target hardware smoke run via `make smoke` against the 26
   apps under `examples/ek_ra8d2/`, classified by halt-PC inspection.

## 1. Test case catalogue

The catalogue is partitioned by module / category. Every `tests/test_*.c`
file is the unit-test specification for the matching `libs/<module>` or
`port/<module>` translation unit. The naming convention is mechanical:
`tests/test_<module>.c` verifies `libs/<module>/src/<module>.c` (or the
equivalent under `src/secure_app/` and `port/`).

### 1.1 Hardware abstraction layer (`libs/ra8_hal`)

| Test ID | Source                              | Subject under test                       |
|--------:|-------------------------------------|------------------------------------------|
| UT-HAL-ACMPHS-001 | `tests/test_ra8_acmphs.c`  | `libs/ra8_hal/src/ra8_acmphs.c`            |
| UT-HAL-ADC-001    | `tests/test_ra8_adc.c`     | `libs/ra8_hal/src/ra8_adc.c`               |
| UT-HAL-AGT-001    | `tests/test_ra8_agt.c`     | `libs/ra8_hal/src/ra8_agt.c`               |
| UT-HAL-BKUP-001   | `tests/test_ra8_bkup.c`    | `libs/ra8_hal/src/ra8_bkup.c`              |
| UT-HAL-BSCAN-001  | `tests/test_ra8_bscan.c`   | `libs/ra8_hal/src/ra8_bscan.c`             |
| UT-HAL-CAC-001    | `tests/test_ra8_cac.c`     | `libs/ra8_hal/src/ra8_cac.c`               |
| UT-HAL-CANFD-001  | `tests/test_ra8_canfd.c`   | `libs/ra8_hal/src/ra8_canfd.c`             |
| UT-HAL-CEU-001    | `tests/test_ra8_ceu_capture.c`, `tests/test_ra8_ceu_config.c` | `libs/ra8_hal/src/ra8_ceu.c`               |
| UT-HAL-CGC-001    | `tests/test_ra8_cgc.c`     | `libs/ra8_hal/src/ra8_cgc.c`               |
| UT-HAL-CNECC-001  | `tests/test_ra8_cnecc.c`   | `libs/ra8_hal/src/ra8_cnecc.c`             |
| UT-HAL-CRC-001    | `tests/test_ra8_crc.c`     | `libs/ra8_hal/src/ra8_crc.c`               |
| UT-HAL-DAC-001    | `tests/test_ra8_dac_b.c`   | `libs/ra8_hal/src/ra8_dac_b.c`             |
| UT-HAL-DMA-001    | `tests/test_ra8_dma.c`     | `libs/ra8_hal/src/ra8_dma.c`               |
| UT-HAL-DMAC-001   | `tests/test_ra8_dmac.c`    | `libs/ra8_hal/src/ra8_dmac.c`              |
| UT-HAL-DOC-001    | `tests/test_ra8_doc.c`     | `libs/ra8_hal/src/ra8_doc.c`               |
| UT-HAL-DOTF-001   | `tests/test_ra8_dotf.c`    | `libs/ra8_hal/src/ra8_dotf.c`              |
| UT-HAL-DRW-001    | `tests/test_ra8_drw.c`     | `libs/ra8_hal/src/ra8_drw.c`               |
| UT-HAL-DTC-001    | `tests/test_ra8_dtc.c`     | `libs/ra8_hal/src/ra8_dtc.c`               |
| UT-HAL-ELC-001    | `tests/test_ra8_elc.c`     | `libs/ra8_hal/src/ra8_elc.c`               |
| UT-HAL-FLASH-001  | `tests/test_ra8_flash.c`   | `libs/ra8_hal/src/ra8_flash.c`             |
| UT-HAL-GPIO-001   | `tests/test_ra8_gpio.c`    | `libs/ra8_hal/src/ra8_gpio.c`              |
| UT-HAL-IIC-001    | `tests/test_ra8_iic.c`     | `libs/ra8_hal/src/ra8_iic.c`               |
| UT-HAL-IPC-001    | `tests/test_ra8_ipc.c`     | `libs/ra8_hal/src/ra8_ipc.c`               |
| UT-HAL-ISR-001    | `tests/test_ra8_isr.c`     | `libs/ra8_hal/src/ra8_isr.c`               |
| UT-HAL-MIPI-DSI-001 | `tests/test_ra8_mipi_dsi_cmd.c`, `tests/test_ra8_mipi_dsi_video.c`, `tests/test_ra8_mipi_dsi_mcdc.c` | `libs/ra8_hal/src/ra8_mipi_dsi.c`     |
| UT-HAL-MIPI-PHY-001 | `tests/test_ra8_mipi_phy_init.c`, `tests/test_ra8_mipi_phy_lanes.c` | `libs/ra8_hal/src/ra8_mipi_phy.c`     |
| UT-HAL-MPU-001    | `tests/test_ra8_mpu.c`     | `libs/ra8_mpu/src/ra8_mpu.c`               |
| UT-HAL-PMC-001    | `tests/test_ra8_pmc.c`     | `libs/ra8_hal/src/ra8_pmc.c`               |
| UT-HAL-RSIP-001   | `tests/test_ra8_rsip_core.c`, `tests/test_ra8_rsip_sym.c`, `tests/test_ra8_rsip_devsec.c` | `libs/ra8_hal/src/ra8_rsip.c`              |
| UT-HAL-RTC-001    | `tests/test_ra8_rtc.c`     | `libs/ra8_hal/src/ra8_rtc.c`               |
| UT-HAL-SCI-001    | `tests/test_ra8_sci.c`     | `libs/ra8_hal/src/ra8_sci.c`               |
| UT-HAL-SDCARD-001 | `tests/test_ra8_sdcard.c`  | `libs/ra8_hal/src/ra8_sdcard.c`            |
| UT-HAL-SDRAM-001  | `tests/test_ra8_sdram.c`   | `libs/ra8_hal/src/ra8_sdram.c`             |
| UT-HAL-SMBUS-001  | `tests/test_ra8_smbus.c`   | `libs/ra8_hal/src/ra8_smbus.c`             |
| UT-HAL-SPI-001    | `tests/test_ra8_spi.c`     | `libs/ra8_hal/src/ra8_spi.c`               |
| UT-HAL-USB-001    | `tests/test_ra8_usb.c`     | `libs/ra8_hal/src/ra8_usb.c`               |
| UT-HAL-WDT-001    | `tests/test_ra8_wdt.c`     | `libs/ra8_hal/src/ra8_wdt.c`               |
| UT-HAL-XSPI-001   | `tests/test_ra8_xspi.c`    | `libs/ra8_hal/src/ra8_xspi.c`              |

(Truncated for brevity. The complete row set is the union of the
`tests/test_ra8_*.c` glob with the matching `libs/ra8_hal/src/*.c` --
190 host-test files in total at the 2026-05-03 refresh.)

### 1.2 Core, security and PAL libraries

| Test ID            | Source                          | Subject                                    |
|--------------------|---------------------------------|--------------------------------------------|
| UT-CORE-ERR-001    | `tests/test_ra8_err.c`           | `libs/ra8_core/src/ra8_err.c`                |
| UT-CORE-LOG-001    | `tests/test_ra8_log.c`           | `libs/ra8_core/src/ra8_log.c`                |
| UT-CORE-TIME-001   | `tests/test_ra8_time.c`          | `libs/ra8_core/src/ra8_time.c`               |
| UT-NSC-VENEER-001  | `tests/test_ra8_nsc_*.c`         | `libs/ra8_nsc/src/*.c` (TZ NSC veneers)     |
| UT-PSA-CRYPTO-001  | `tests/test_ra8_psa_crypto_api.c`, `tests/test_ra8_psa_crypto_guards.c`, `tests/test_ra8_psa_crypto_aead_mcdc.c` | `libs/ra8_psa_crypto/src/ra8_psa_crypto.c`   |
| UT-TLS-001         | `tests/test_ra8_tls.c`           | `libs/ra8_tls/src/ra8_tls.c`                 |
| UT-OTA-001         | `tests/test_ra8_ota.c`           | `libs/ra8_ota/src/ra8_ota.c`                 |
| UT-USBPAL-001      | `tests/test_ra8_usb_pal.c`       | `libs/ra8_usb_pal/src/ra8_usb_pal.c`         |
| UT-NETPAL-001      | `tests/test_ra8_net_pal.c`       | `libs/ra8_net_pal/src/ra8_net_pal.c`         |
| UT-WDTSUP-001      | `tests/test_ra8_wdt_supervisor.c`| `libs/ra8_wdt_supervisor/src/*.c`           |
| UT-SECAPP-KV-001   | `tests/test_secure_app_key_vault.c` | `src/secure_app/key_vault.c`           |
| UT-SECAPP-KI-001   | `tests/test_secure_app_key_import.c`| `src/secure_app/key_import.c`          |
| UT-SECAPP-OTA-001  | `tests/test_secure_app_ota_commit.c`| `src/secure_app/ota_commit.c`          |
| UT-SECAPP-TRNG-001 | `tests/test_secure_app_trng.c`  | `src/secure_app/secure_trng.c`             |

### 1.3 Port and SOUP integration tests

| Test ID         | Source                         | Subject                                        |
|-----------------|--------------------------------|------------------------------------------------|
| IT-LWIP-001     | `tests/test_lwip_sys_arch.c`   | `port/lwip/arch/sys_arch.c` (lwIP shim)        |
| IT-LXNOR-001    | `tests/test_lx_nor_driver_ra8_xspi.c` | `port/levelx/src/lx_nor_driver_ra8_xspi.c` |
| IT-NIMBLE-001   | `tests/test_nimble_npl_threadx.c` | `port/nimble/src/nimble_npl_threadx.c`          |
| IT-USBX-DCD-001 | `tests/test_ux_dcd_ra8_usb.c`   | `port/usbx/src/ux_dcd_ra8_usb.c`                    |

### 1.4 Application-shape integration tests (one per EVM-tier app)

`tests/test_app_<app>.c` instantiates the app's public surface with
mock peripherals from `libs/ra8_*_pal/`. 25 files cover all 26
EVM-tier apps under `examples/ek_ra8d2/` (the `blink` app shares
the `blink_hal` shape harness).

| Test ID            | Source                                       | App under test                               |
|--------------------|----------------------------------------------|----------------------------------------------|
| IT-APP-BLINK-001   | `tests/test_app_blink_hal.c`                 | `examples/ek_ra8d2/{blink, blink_hal}`       |
| IT-APP-CLOCK-001   | `tests/test_app_clock_check.c`               | `examples/ek_ra8d2/clock_check`              |
| IT-APP-EREADER-001 | `tests/test_app_ereader.c`                   | `examples/ek_ra8d2/ereader`                  |
| IT-APP-ETHTCP-001  | `tests/test_app_ethernet_tcp_echo.c`         | `examples/ek_ra8d2/ethernet_tcp_echo`        |
| IT-APP-LCD-001     | `tests/test_app_lcd_demo.c`                  | `examples/ek_ra8d2/lcd_demo`                 |
| IT-APP-BOOT-001    | `tests/test_app_ra8_bootloader.c`             | `examples/ek_ra8d2/ra8_bootloader`            |
| IT-APP-TXBLINK-001 | `tests/test_app_threadx_blink.c`             | `examples/ek_ra8d2/threadx_blink`            |
| IT-APP-TXCAN-001   | `tests/test_app_threadx_canfd_demo.c`        | `examples/ek_ra8d2/threadx_canfd_demo`       |
| IT-APP-TXFS-001    | `tests/test_app_threadx_fs_demo.c`           | `examples/ek_ra8d2/threadx_fs_demo`          |
| IT-APP-TXFSLX-001  | `tests/test_app_threadx_fs_levelx_demo.c`    | `examples/ek_ra8d2/threadx_fs_levelx_demo`   |
| IT-APP-TXIPC-001   | `tests/test_app_threadx_ipc_demo.c`          | `examples/ek_ra8d2/threadx_ipc_demo`         |
| IT-APP-TXLX-001    | `tests/test_app_threadx_levelx_demo.c`       | `examples/ek_ra8d2/threadx_levelx_demo`      |
| IT-APP-TXLWIP-001  | `tests/test_app_threadx_lwip_tcp_echo.c`     | `examples/ek_ra8d2/threadx_lwip_tcp_echo`    |
| IT-APP-TXMPU-001   | `tests/test_app_threadx_mpu_partition_demo.c`| `examples/ek_ra8d2/threadx_mpu_partition_demo`|
| IT-APP-TXNETX-001  | `tests/test_app_threadx_netx_tcp_echo.c`     | `examples/ek_ra8d2/threadx_netx_tcp_echo`    |
| IT-APP-TXOTA-001   | `tests/test_app_threadx_ota_demo.c`          | `examples/ek_ra8d2/threadx_ota_demo`         |
| IT-APP-TXUSBX-001  | `tests/test_app_threadx_usbx_cdc_demo.c`     | `examples/ek_ra8d2/threadx_usbx_cdc_demo`    |
| IT-APP-UART-001    | `tests/test_app_uart_hello.c`                | `examples/ek_ra8d2/uart_hello`               |
| IT-APP-USBCDC-001  | `tests/test_app_usb_cdc_echo.c`              | `examples/ek_ra8d2/usb_cdc_echo`             |
| IT-APP-USBHID-001  | `tests/test_app_usb_hid_device.c`            | `examples/ek_ra8d2/usb_hid_device`           |
| IT-APP-USBHKBD-001 | `tests/test_app_usb_host_keyboard.c`         | `examples/ek_ra8d2/usb_host_keyboard`        |
| IT-APP-USBHMSC-001 | `tests/test_app_usb_host_msc_browse.c`       | `examples/ek_ra8d2/usb_host_msc_browse`      |
| IT-APP-USBMSCD-001 | `tests/test_app_usb_msc_device.c`            | `examples/ek_ra8d2/usb_msc_device`           |

### 1.5 Hardware smoke (on-target)

| Test ID         | Source                            | Subject                                        |
|-----------------|-----------------------------------|------------------------------------------------|
| HW-HIL-EVM-001  | `scripts/hil/all.sh`              | Every `hil.conf` app under `examples/ek_ra8d2/hw_validated/hil/` |

### 1.6 Coverage forcing test

| Test ID              | Source                                | Purpose                                   |
|----------------------|---------------------------------------|-------------------------------------------|
| UT-COMPILE-ALL-001   | `tests/test_coverage_compile_all.c`   | Forces every `libs/` and `src/` TU into   |
|                      |                                       | the link image so MC/DC instrumentation   |
|                      |                                       | covers every first-party file.            |

## 2. Test procedure -- host unit / module tests (`make test`)

**Procedure VP-HOST-001.**

1. From the repository root, run `cmake -S tests -B tests/build`
   with the host toolchain (`gcc-14` or `clang-18` -- both are
   exercised by CI).
2. Build via `cmake --build tests/build -- -j`.
3. Execute via `ctest --test-dir tests/build --output-on-failure`.
4. Record the per-test pass/fail outcome and elapsed time.
5. Pass criterion: every test binary returns exit code 0; build is
   clean under `-Wall -Wextra -Werror -Wimplicit-function-declaration
   -Wmissing-prototypes -Wshadow -Wconversion`.

**Inputs**: source under `libs/`, `src/`, `port/`, `tests/`.
**Outputs**: `tests/build/` artefacts; ctest log captured by SVR.
**Tools**: `cmake`, `gcc-14` or `clang-18`, `ctest` (see
`docs/qualification/TOOL_QUALIFICATION.md`).

## 3. Test procedure -- MC/DC measurement (`make mcdc`)

**Procedure VP-MCDC-001.**

1. From the repository root, run `make mcdc`. This wraps
   `scripts/report/mcdc_report.sh`, which:
   1. Configures `tests/` with `cmake -S tests -B tests/build-cov
      -DRA8_MCDC=ON`.
   2. Builds every host test with the clang flag trio
      `-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc`.
   3. Executes each test binary with a per-binary `LLVM_PROFILE_FILE`.
   4. Merges all `.profraw` files via `llvm-profdata merge -sparse`.
   5. Renders `build/mcdc-report/summary.txt` and `mcdc.txt`.
2. Pass criterion: first-party MC/DC fraction in the `TOTAL` row of
   `build/mcdc-report/summary.txt` is greater than or equal to the
   threshold `RA8_MCDC_THRESHOLD` (default 100% for Phase 1 modules,
   95% for Phase 2 modules; see `docs/QUALIFICATION_ROADMAP.md`).
3. Fallback: if `clang-18` is unavailable, the script falls back to
   `gcc-14 -fcondition-coverage` and prints a loud warning. The
   fallback is not certification-grade and the gate is skipped --
   results from the fallback path are advisory only (see
   `docs/MCDC.md`).

**Tooling basis**: clang-18 with LLVM source-based MC/DC, per
`https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#mc-dc-instrumentation`
and DO-178C section 6.4.4.2.

## 4. Test procedure -- on-target hardware smoke (`make smoke`)

**Procedure VP-HW-001.**

1. Connect EK-RA8D2 board (Renesas part 968-K7EKA8D2S01001BE) to
   the host via the on-board J-Link OB USB port. Confirm `JLinkExe
   -device R7KA8D2KF_CPU0 -if SWD -speed 4000 -autoconnect 1` returns
   chip ID without error.
2. From the repository root, run `make hil-all`. This top-level target
   invokes `scripts/hil/all.sh`, which for each discovered app:
      - Builds the app and calls `scripts/hil/flash.sh` to program MRAM.
      - Sources the app's `hil.conf` to learn its verification mode.
      - Verifies per mode: scrapes the SCI console for `HIL_EXPECT`
        (`uart_scrape`), halts and reads a probed progress counter
        (`jlink_memprobe`), or confirms host-side enumeration
        (`usb_cdc` / `usb_hid` / `usb_msc`).
      - Classifies the app PASS / FAIL, honouring `HIL_FAULT_EXPECTED`.
3. Per-app logs are written under `/tmp/hil_all_*`; the aggregate
   PASS / FAIL table is printed to the console at the end of the run.

### Pass / WIP / FAIL classification rule

| Result    | Meaning                                                                 |
|-----------|-------------------------------------------------------------------------|
| `PASS`    | PC resolves into `tx_thread_schedule.S` (ThreadX idle), `ra8_time.c::ra8_delay_ms`, or any user `main.c` / `internal_*` loop that is not a `*_panic_halt` sink. |
| `WIP`     | PC parked in `panic_halt`, `demo_panic_halt`, `usb_hid_panic_halt`, `usb_msc_panic_halt`, or `internal_ra8_fatal_error`. Init failed in a caught path; warning, not failure. |
| `UNKNOWN` | PC matched no PASS or WIP keyword. Grouped with WIP for exit-code purposes -- never silently promoted to PASS. |
| `FAIL`    | PC == `0xEFFFFFFE` (Cortex-M lockup) or in any fault handler (`Default_Handler`, `HardFault_Handler`, `MemManage_Handler`, `BusFault_Handler`, `UsageFault_Handler`, `SecureFault_Handler`). `make smoke` exits non-zero. |
| `NOBUILD` | The `.elf` or `.hex` is missing. Warning, not failure.                  |

**Tooling basis**: SEGGER `JLinkExe` v9.38a, `arm-none-eabi-gcc` GNU
Arm Toolchain, `arm-none-eabi-addr2line` from the same toolchain.
TQL classifications in `docs/qualification/TOOL_QUALIFICATION.md`.

## 5. Test procedure -- MISRA-C 2012 advisory pass (`make misra`)

**Procedure VP-MISRA-001.**

1. From the repository root, run `make misra`. This wraps
   `scripts/checks/misra_check_inner.sh`:
   1. Walks `libs/`, `src/`, and `port/` (excluding
      `libs/third_party/`).
   2. Invokes `cppcheck --addon=misra` with project-wide includes
      and the `.cppcheck-suppressions` file.
   3. Writes raw output to `build/misra/raw.txt` and
      `build/misra/misra-raw.txt`.
   4. Renders `build/misra/results.txt` (TSV) and
      `docs/MISRA_GAPS.csv` (capped at 1000 rows + tail summary).
2. Pass criterion: every reported violation appears in
   `docs/qualification/MISRA_DEVIATIONS.md` (active deviation D-001
   through D-005), or the script is updated and the deviation
   register receives a new entry.
3. Caveat (recorded in `docs/MISRA.md`): cppcheck-MISRA implements
   roughly two thirds of the mandatory + required MISRA-C 2012 rules
   and does not yet support `--std=c23`. A qualified commercial
   checker (LDRA, Helix QAC, Polyspace) is required before SOI-3.

## 6. Test procedure -- Doxygen audit (`scripts/checks/doxy_audit.py`)

**Procedure VP-DOXY-001.**

1. From the repository root, run `python3 scripts/checks/doxy_audit.py`.
2. The script audits every C / C++ function under `libs/`, `src/`,
   and `port/` against the Doxygen Documentation Requirements in
   `CLAUDE.md`. It writes:
   - `docs/DOXYGEN_GAPS.md` (Markdown summary).
   - `docs/DOXYGEN_GAPS.csv` (per-function gap rows).
3. Pass criterion (Phase 3 of `docs/QUALIFICATION_ROADMAP.md`):
   the "Functions with gaps" line in `DOXYGEN_GAPS.md` reads zero.
   Today's value is **0** (gate met, 2747 functions audited).

## 7. Requirements traceability matrix

The project's requirements are inferred from the per-module file
`@brief` corpus and from the public-API headers (`libs/<module>/inc/`).
A formal requirements baseline is part of Phase 7's PSAC; until that
lands, the matrix below traces verification cases to DO-178C Annex
A objectives so the SVR can roll up Annex A coverage.

| Verification case set         | DO-178C Annex A objective(s)                  | IEC 61508-3 clause | Status   |
|-------------------------------|-----------------------------------------------|--------------------|----------|
| UT-* (unit tests)             | A-7 obj 1, 2 (test cases & procedures)        | 7.4.7 / 7.9.2      | Active   |
| IT-* (integration tests)      | A-7 obj 3, 4 (integration test execution)     | 7.5                | Active   |
| HW-SMOKE-EVM-001              | A-6 obj 1, 2, 3 (executable-object verif.)    | 7.5 / 7.9.2.7      | Active   |
| MC/DC report (VP-MCDC-001)    | A-7 obj 5, 6, 7, 8 (structural coverage)      | Annex C            | Active   |
| MISRA pass (VP-MISRA-001)     | A-5 obj 4 (coding standard compliance)        | Annex A.4 / 7.4.4  | Active w/ deviations |
| Doxygen audit (VP-DOXY-001)   | A-2 obj 7 (low-level requirement traceability)| 7.2 / 7.4.3        | Active w/ gaps |

## 8. Pass / fail criteria summary

| Activity             | Pass criterion                                                                |
|----------------------|-------------------------------------------------------------------------------|
| Cross compile        | `make <app>` exits 0; zero warnings under `-Wall -Wextra -Werror`.            |
| Host build           | `cmake --build tests/build` exits 0; zero warnings under same flag set.       |
| Host run             | `ctest` exits 0; every test binary returns 0.                                 |
| MC/DC                | First-party MC/DC fraction in `summary.txt` >= `RA8_MCDC_THRESHOLD`.           |
| Hardware smoke       | Zero rows classified `FAIL` in `build/smoke/results.md`; WIP / UNKNOWN allowed with written follow-up in `docs/HARDWARE_BRINGUP.md`. |
| MISRA                | cppcheck-only policy; every finding maps to an active D-### deviation in MISRA_DEVIATIONS.md (`docs/CERTIFICATION_SCOPE.md`). |
| Doxygen              | `DOXYGEN_GAPS.md` "Functions with gaps" trends to zero (Phase 3 acceptance).  |

## 9. Test environment configuration

| Environment             | Toolchain                          | Build path                       |
|-------------------------|------------------------------------|----------------------------------|
| Host unit / integration | `gcc-14` or `clang-18` + `cmake`   | `tests/build/`                   |
| Host MC/DC              | `clang-18` + `llvm-profdata` + `llvm-cov` | `tests/build-cov/`        |
| Cross production        | `arm-none-eabi-gcc` + `cmake`      | `examples/ek_ra8d2/<app>/build/` |
| Hardware smoke          | `JLinkExe` v9.38a + `arm-none-eabi-addr2line` | `build/smoke/`        |

The host environment runs natively (no QEMU); the cross environment
emits MRAM / SRAM-resident binaries for the EK-RA8D2 v1; the hardware
environment is the EK-RA8D2 v1 driven by the on-board J-Link OB.

## 10. Change log

| Date       | Author             | Change                                            |
|------------|--------------------|---------------------------------------------------|
| 2026-05-02 | Brighton Sikarskie | Initial first-draft population (Phase 7 kickoff). |
