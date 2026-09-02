# Software Verification Cases & Procedures (SVCP)

**Document ID**: ra8d2-svcp-001
**Version**: 0.1 (first draft, Phase 7 of `docs/QUALIFICATION_ROADMAP.md`).
**Last refreshed**: 2026-08-22 (distributed-test and HIL inventory refresh).
**Date**: 2026-05-02.
**Author**: Brighton Sikarskie.
**DO-178C reference**: Section 11.13 (Software Verification Cases and
Procedures).
**IEC 61508-3 reference**: Clause 7.9.2 (Software verification --
test specification).
**ISO 26262-6 reference**: Clause 9.4 (Verification of software
unit and integration).

## Scope

This document is the static verification specification: it describes the
authoritative test inventory and representative traced cases, plus the procedure
for executing each verification activity, and binds each verification
case to the requirement (or DO-178C Annex A objective) it discharges.
The captured outputs of running these procedures live in
`docs/qualification/SVR.md`.

The verification cases below cover three layers:

1. A distributed host corpus under `tests/`, `apps/**/tests/`, and the small
   `examples/**/tests/` population. The retained 2026-08-22 snapshot
   counted 693 test sources and 689 registrations on clean standalone macOS and
   Linux; its Linux/devcontainer unit gate passed all 689 in 8.66 s. The live
   shrink-only registration floor is enforced by `tests/run_tests.sh`.
   The Alphabet Soup `/proc/self/mem` and closed-stdout cases remain registered
   but disabled on macOS; their Linux-specific paths are included in the
   structural inventory, not in any macOS execution claim.
   macOS execution is not claimed because the low-address peripheral-mock tests
   require Linux/container execution. Downloader helper translation units are
   compiled into their canonical owning tests, not registered alone.
2. Module- and application-local integration tests in the same distributed
   inventory.
3. Emulator- and hardware-in-the-loop procedures selected by the authoritative
   app inventory and invoked through `just hil::run` for real target execution.

## 1. Test case catalogue

The catalogue is partitioned by module / category. Test sources are colocated
under the category or module they verify; filename alone is not an authoritative
path. The inventory scripts and CMake registration are the complete source of
truth, while the rows below are representative trace anchors.

### 1.1 Hardware abstraction layer (`libs/ra8_hal`)

| Test ID | Source                              | Subject under test                       |
|--------:|-------------------------------------|------------------------------------------|
| UT-HAL-ACMPHS-001 | `tests/misc/src/test_ra8_acmphs.c`  | `libs/ra8_hal/src/ra8_acmphs.c`            |
| UT-HAL-ADC-001    | `tests/hal/src/test_ra8_adc.c`     | `libs/ra8_hal/src/adc.c`                   |
| UT-HAL-AGT-001    | `tests/misc/src/test_ra8_agt.c`     | `libs/ra8_hal/src/ra8_agt.c`               |
| UT-HAL-BKUP-001   | `tests/misc/src/test_ra8_bkup.c`    | `libs/ra8_hal/src/ra8_bkup.c`              |
| UT-HAL-BSCAN-001  | `tests/misc/src/test_ra8_bscan.c`   | `libs/ra8_hal/src/ra8_bscan.c`             |
| UT-HAL-CAC-001    | `tests/misc/src/test_ra8_cac.c`     | `libs/ra8_hal/src/ra8_cac.c`               |
| UT-HAL-CANFD-001  | `tests/hal/src/test_ra8_canfd.c`   | `libs/ra8_hal/src/ra8_canfd.c`             |
| UT-HAL-CEU-001    | `tests/misc/src/test_ra8_ceu_capture.c`, `tests/misc/src/test_ra8_ceu_config.c` | `libs/ra8_hal/src/ra8_ceu.c`               |
| UT-HAL-CGC-001    | `tests/hal/src/test_ra8_cgc.c`     | `libs/ra8_hal/src/ra8_cgc.c`               |
| UT-HAL-CNECC-001  | `tests/misc/src/test_ra8_cnecc.c`   | `libs/ra8_hal/src/ra8_cnecc.c`             |
| UT-HAL-CRC-001    | `tests/misc/src/test_ra8_crc.c`     | `libs/ra8_hal/src/ra8_crc.c`               |
| UT-HAL-DAC-001    | `tests/hal/src/test_ra8_dac_b.c`   | `libs/ra8_hal/src/ra8_dac_b.c`             |
| UT-HAL-DMA-001    | `tests/hal/src/test_ra8_dma.c`     | `libs/ra8_hal/src/ra8_dma.c`               |
| UT-HAL-DMAC-001   | `tests/hal/src/test_ra8_dmac.c`    | `libs/ra8_hal/src/ra8_dmac.c`              |
| UT-HAL-DOC-001    | `tests/misc/src/test_ra8_doc.c`     | `libs/ra8_hal/src/ra8_doc.c`               |
| UT-HAL-DOTF-001   | `tests/misc/src/test_ra8_dotf.c`    | `libs/ra8_hal/src/ra8_dotf.c`              |
| UT-HAL-DRW-001    | `tests/misc/src/test_ra8_drw.c`     | `libs/ra8_hal/src/ra8_drw.c`               |
| UT-HAL-DTC-001    | `tests/misc/src/test_ra8_dtc.c`     | `libs/ra8_hal/src/ra8_dtc.c`               |
| UT-HAL-ELC-001    | `tests/misc/src/test_ra8_elc.c`     | `libs/ra8_hal/src/ra8_elc.c`               |
| UT-HAL-FLASH-001  | `tests/misc/src/test_ra8_flash.c`   | `libs/ra8_hal/src/ra8_flash.c`             |
| UT-HAL-GPIO-001   | `tests/hal/src/test_ra8_gpio.c`    | `libs/ra8_hal/src/gpio.c`                  |
| UT-HAL-IPC-001    | `tests/hal/src/test_ra8_ipc.c`     | `libs/ra8_hal/src/ra8_ipc.c`               |
| UT-HAL-ISR-001    | `tests/hal/src/test_ra8_isr.c`     | `libs/ra8_hal/src/ra8_isr.c`               |
| UT-HAL-MIPI-DSI-001 | `tests/hal/src/test_ra8_mipi_dsi_cmd.c`, `tests/hal/src/test_ra8_mipi_dsi_video.c`, `tests/hal/src/test_ra8_mipi_dsi_mcdc.c` | `libs/ra8_hal/src/ra8_mipi_dsi.c`     |
| UT-HAL-MIPI-PHY-001 | `tests/hal/src/test_ra8_mipi_phy_init.c`, `tests/hal/src/test_ra8_mipi_phy_lanes.c` | `libs/ra8_hal/src/ra8_mipi_phy.c`     |
| UT-HAL-MPU-001    | `tests/misc/src/test_ra8_mpu.c`     | `libs/ra8_mpu/src/ra8_mpu.c`               |
| UT-HAL-RSIP-001   | `tests/security/src/test_ra8_rsip_core.c`, `tests/security/src/test_ra8_rsip_sym.c`, `tests/security/src/test_ra8_rsip_devsec.c` | `libs/ra8_hal/src/ra8_rsip.c`              |
| UT-HAL-RTC-001    | `tests/hal/src/test_ra8_rtc.c`     | `libs/ra8_hal/src/ra8_rtc.c`               |
| UT-HAL-SCI-001    | `tests/hal/src/test_ra8_sci.c`     | `libs/ra8_hal/src/ra8_sci.c`               |
| UT-HAL-SDCARD-001 | `tests/storage/src/test_ra8_sdcard.c`  | `libs/ra8_hal/src/ra8_sdcard.c`            |
| UT-HAL-SMBUS-001  | `tests/misc/src/test_ra8_smbus.c`   | `libs/ra8_hal/src/ra8_smbus.c`             |
| UT-HAL-SPI-001    | `tests/hal/src/test_ra8_spi.c`     | `libs/ra8_hal/src/ra8_spi_b.c`             |
| UT-HAL-USB-001    | `tests/usb/src/test_ra8_usb.c`     | `libs/ra8_hal/src/ra8_usb.c`               |
| UT-HAL-WDT-001    | `tests/hal/src/test_ra8_wdt.c`     | `libs/ra8_hal/src/ra8_wdt.c`               |
| UT-HAL-XSPI-001   | `tests/hal/src/test_ra8_xspi.c`    | `libs/ra8_hal/src/ra8_xspi.c`              |

(Truncated for brevity. The complete row set is derived from the distributed
test discovery and CTest registrations, not from a flat glob.)

### 1.2 Core, security and PAL libraries

| Test ID            | Source                          | Subject                                    |
|--------------------|---------------------------------|--------------------------------------------|
| UT-CORE-ERR-001    | `tests/misc/src/test_ra8_err.c`           | `libs/ra8_core/inc/ra8_err.h`                |
| UT-CORE-LOG-001    | `tests/core/src/test_ra8_log.c`           | `libs/ra8_core/src/ra8_log.c`                |
| UT-CORE-TIME-001   | `tests/hal/src/test_ra8_time.c`          | `libs/ra8_core/src/ra8_time.c`               |
| UT-NSC-VENEER-001  | `tests/net/src/test_ra8_nsc*.c`  | `libs/ra8_nsc/src/*.c` (TZ NSC veneers)     |
| UT-PSA-CRYPTO-001  | `tests/security/src/test_ra8_psa_crypto_api.c`, `tests/security/src/test_ra8_psa_crypto_guards.c`, `tests/security/src/test_ra8_psa_crypto_aead_mcdc.c` | `libs/ra8_psa_crypto/src/ra8_psa_crypto.c`   |
| UT-TLS-001         | `tests/wireless/src/test_ra8_tls.c`           | `libs/ra8_tls/src/ra8_tls.c`                 |
| UT-OTA-001         | `tests/misc/src/test_ra8_ota.c`           | `libs/ra8_ota/src/ra8_ota.c`                 |
| UT-USBPAL-001      | `tests/usb/src/test_ra8_usb_pal.c`       | `libs/ra8_usb_pal/src/ra8_usb_pal.c`         |
| UT-NETPAL-001      | `tests/net/src/test_ra8_net_pal.c`       | `libs/ra8_net_pal/src/ra8_net_pal.c`         |
| UT-WDTSUP-001      | `tests/hal/src/test_ra8_wdt_supervisor.c`| `libs/ra8_wdt_supervisor/src/*.c`           |
| UT-SECAPP-KV-001   | `tests/security/src/test_ra8_key_vault.c` | `libs/ra8_secure_app/src/key_vault.c`        |
| UT-SECAPP-KI-001   | `tests/security/src/test_secure_app_key_import.c`| `libs/ra8_secure_app/src/key_import.c`          |
| UT-SECAPP-OTA-001  | `tests/security/src/test_secure_app_ota_commit.c`| `libs/ra8_secure_app/src/ota_commit.c`          |
| UT-SECAPP-TRNG-001 | `tests/security/src/test_secure_app_secure_trng.c` | `libs/ra8_secure_app/src/secure_trng.c` |

### 1.3 Port and SOUP integration tests

| Test ID         | Source                         | Subject                                        |
|-----------------|--------------------------------|------------------------------------------------|
| IT-LXNOR-001    | `tests/misc/src/test_lx_nor_driver_ra8_xspi.c` | `port/levelx/src/lx_nor_driver_ra8_xspi.c` |
| IT-USBX-DCD-001 | `tests/usb/src/test_ux_dcd_ra8_usb.c`   | `port/usbx/src/ux_dcd_ra8_usb.c`                    |

### 1.4 Application-shape integration tests

Application-shape tests now live under `tests/mocks/` and app-local `tests/`
directories. Their registration is protected by the shrink-only floor in
`tests/run_tests.sh`. This revision does not claim a one-test-per-app trace
ratio; EIL selection comes from `scripts/dev/ra8_apps.py`, and the trace matrix
remains open evidence.

### 1.5 Hardware smoke (on-target)

| Test ID         | Source                            | Subject                                        |
|-----------------|-----------------------------------|------------------------------------------------|
| HW-HIL-RA8D2-001 | `scripts/hil/all.sh`             | Guarded selected-app execution on the RA8D2 rig |

### 1.6 Coverage forcing test

| Test ID              | Source                                | Purpose                                   |
|----------------------|---------------------------------------|-------------------------------------------|
| UT-COMPILE-ALL-001   | `tests/misc/src/test_coverage_compile_all.c`   | Forces every `libs/` TU into              |
|                      |                                       | the link image so MC/DC instrumentation   |
|                      |                                       | covers every first-party file.            |

## 2. Test procedure -- host unit / module tests (`just quality::local::test`)

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

**Inputs**: source under `libs/`, `port/`, `tests/`.
**Outputs**: `tests/build/` artefacts; ctest log captured by SVR.
**Tools**: `cmake`, `gcc-14` or `clang-18`, `ctest` (see
`docs/qualification/TOOL_QUALIFICATION.md`).

## 3. Test procedure -- MC/DC measurement (`just quality::local::mcdc`)

**Procedure VP-MCDC-001.**

1. From the repository root, run `just quality::local::mcdc`. This wraps
   `scripts/report/mcdc_report.sh`, which:
   1. Configures `tests/` with `cmake -S tests -B tests/build-cov
      -DRA8_MCDC=ON`.
   2. Builds every host test with the clang flag trio
      `-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc`.
   3. Executes each test binary with a per-binary `LLVM_PROFILE_FILE`.
   4. Merges all `.profraw` files via `llvm-profdata merge -sparse`.
   5. Renders `build/mcdc-report/summary.txt` and `mcdc.txt`.
2. Pass criterion: first-party MC/DC fraction in the `TOTAL` row of
   `build/mcdc-report/summary.txt` is greater than or equal to
   `RA8_MCDC_THRESHOLD` (the producer default is 100%). The release evidence
   pack must record the actual result; this procedure does not inherit the
   obsolete phase-specific percentages.
3. Tool failure: if `clang-18` or its matching LLVM profile tools are
   unavailable, the script exits nonzero without producing an MC/DC verdict.
   A manual gcc-14 condition-coverage experiment is not certification-grade
   and cannot satisfy the gate (see `docs/MCDC.md`).

**Tooling basis**: clang-18 with LLVM source-based MC/DC, per
`https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#mc-dc-instrumentation`
and DO-178C section 6.4.4.2.

## 4. Test procedure -- on-target HIL (`just hil::run`)

**Procedure VP-HW-001.**

1. Configure the guarded rig in the gitignored `.env` and confirm it is
   reachable with `just hil::status`. Do not bypass a held bench lock.
2. From the repository root, run `just hil::run`. This builds locally, stages
   artifacts to the rig, and invokes `scripts/hil/all.sh`, which for each app:
      - Builds the app and calls `scripts/hil/flash.sh` to program MRAM.
      - Sources the app's `hil.conf` to learn its verification mode.
      - Verifies per mode: scrapes the SCI console for `HIL_EXPECT`
        (`uart_scrape`), halts and reads a probed progress counter
        (`jlink_memprobe`), or confirms host-side enumeration
        (`usb_cdc` / `usb_hid` / `usb_msc`).
      - Classifies the app PASS / FAIL, honouring `HIL_FAULT_EXPECTED`.
3. Per-app logs are written under `/tmp/hil_all_*`; the aggregate
   PASS / FAIL table is printed to the console at the end of the run.

### Pass / fail classification rule

| Result | Meaning |
|---|---|
| `PASS` | The app's declared verifier returned zero and all positive/negative assertions held. |
| `FAIL` | Build failure, missing or invalid `hil.conf`, unknown mode, or any declared verifier returning non-zero. The suite exits non-zero. |

**Tooling basis**: the rig's installed SEGGER `JLinkExe` (version captured with
the run), `arm-none-eabi-gcc` GNU Arm Toolchain, and
`arm-none-eabi-addr2line` from the same toolchain.
TQL classifications in `docs/qualification/TOOL_QUALIFICATION.md`.

## 5. Test procedure -- MISRA-C 2012 advisory pass

**Procedure VP-MISRA-001.**

1. From the repository root, run `just quality::local::gate misra`. This
   registered CI entry point runs the deviation-integrity checks,
   `scripts/checks/misra_check_inner.sh`, and then the committed-baseline
   ratchet:
   1. Walks `libs/`, `port/`, `tools/` and `apps/` (excluding
      `libs/third_party/`).
   2. Runs cppcheck dumps through its bundled MISRA addon with project-wide
      includes and the `.cppcheck-suppressions` file.
   3. Writes raw output to `build/misra/raw.txt` and
      `build/misra/misra-raw.txt`.
   4. Renders `build/misra/results.txt` (TSV) for the current run.
2. Pass criterion: `scripts/checks/misra_ratchet.py --check` finds no growth
   against the committed `.github/misra-baseline.txt`. Finding dispositions
   remain governed by `docs/qualification/MISRA_DEVIATIONS.md`.
3. Caveat (recorded in `docs/MISRA.md`): cppcheck-MISRA implements
   roughly two thirds of the mandatory + required MISRA-C 2012 rules
   and does not yet support `--std=c23`. A qualified commercial
   checker (LDRA, Helix QAC, Polyspace) is required before SOI-3.

## 6. Test procedure -- Doxygen audit (`scripts/checks/doxy_audit.py`)

**Procedure VP-DOXY-001.**

1. From the repository root, run `python3 scripts/checks/doxy_audit.py`.
2. The script audits every C / C++ function under `libs/`, `port/`,
   `tools/` and `apps/` against the Doxygen Documentation Requirements in
   `CLAUDE.md`. It writes:
   - `docs/DOXYGEN_GAPS.md` (Markdown summary).
   - `docs/DOXYGEN_GAPS.csv` (per-function gap rows).
3. Pass criterion (Phase 3 of `docs/QUALIFICATION_ROADMAP.md`):
   the "Functions with gaps" line in `../DOXYGEN_GAPS.md` reads zero. The
   generated report is the live function census; zero functions may have
   documentation gaps or missing-tag instances.

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
| Cross compile        | `just apps::build <app>` exits 0; zero warnings under `-Wall -Wextra -Werror`. |
| Host build           | `cmake --build tests/build` exits 0; zero warnings under same flag set.       |
| Host run             | `ctest` exits 0 for every registered test; the authoritative Linux/devcontainer gate passed 689/689 in 8.66 s on 2026-08-22. |
| MC/DC                | First-party MC/DC fraction in `summary.txt` >= `RA8_MCDC_THRESHOLD`.           |
| Hardware smoke       | `just hil::run` exits 0; every selected app reports `PASS`.                    |
| MISRA                | cppcheck-only policy; every finding maps to an active D-### deviation in MISRA_DEVIATIONS.md (`docs/CERTIFICATION_SCOPE.md`). |
| Doxygen              | `../DOXYGEN_GAPS.md` "Functions with gaps" trends to zero (Phase 3 acceptance). |

## 9. Test environment configuration

| Environment             | Toolchain                          | Build path                       |
|-------------------------|------------------------------------|----------------------------------|
| Host unit / integration | `gcc-14` or `clang-18` + `cmake`   | `tests/build/`                   |
| Host MC/DC              | `clang-18` + `llvm-profdata` + `llvm-cov` | `tests/build-cov/`        |
| Cross production        | `arm-none-eabi-gcc` + `cmake`      | selected application directory's `build/` |
| Hardware smoke          | Rig-installed `JLinkExe` + mode-specific HIL tools | CI/terminal log + `/tmp/hil_all_*` diagnostics |

The host environment runs natively (no QEMU); the cross environment
emits MRAM / SRAM-resident binaries for the EK-RA8D2 v1; the hardware
environment is the EK-RA8D2 v1 driven by the on-board J-Link OB.

## 10. Change log

| Date       | Author             | Change                                            |
|------------|--------------------|---------------------------------------------------|
| 2026-05-02 | Brighton Sikarskie | Initial first-draft population (Phase 7 kickoff). |
| 2026-08-21 | Brighton Sikarskie | Recorded two-OS 673-case registration parity and the Linux/devcontainer 673/673 pass in 46.92 s; macOS execution remains out of scope. |
| 2026-08-22 | Brighton Sikarskie | Added the runtime-provisioner test to the distributed inventory and recorded the 693-source / 689-registration floor plus the Linux/devcontainer 689/689 pass in 8.66 s. |
