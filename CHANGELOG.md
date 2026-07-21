# Changelog

All notable changes to **ra8-firmware** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to a personal-project versioning scheme (no public API,
breaking changes encouraged -- see `CLAUDE.md`). All entries are written in
pure 7-bit ASCII per the project character-encoding policy.

## [0.2.0] - 2026-05-03

Qualification-baseline release. The host-mock half of the codebase now
meets the IEC 61508 SIL 3 / DO-178C Level B evidence bar this project
self-assesses against.

### Added

- **100% reachable MC/DC milestone** -- `make mcdc` reports
  473 / 473 reachable decisions covered (58 deactivated, fully
  rationalised in `docs/MCDC_DEACTIVATIONS.md`). Absolute MC/DC
  89.08%; the gap to absolute is entirely deactivated/defensive code.
- **Eight new pre-commit gates (all STRICT)**:
  `doxy_audit --check`, `check_obsolete_standards.py`,
  `check_mcdc_block.py`, `check_new_compound_has_mcdc.py`,
  `cite_check.py`, `check_world_tags.py`,
  `check_line_citations.py`, `stack_usage_check.py` (warn-only on
  third-party + ra8_epub).
- **Vendor blob policy enacted** -- RSIP vendored under
  `libs/third_party/` with provenance + license documentation; the
  BLE controller blob is blocked-license and is left out of the
  tree. See `docs/VENDOR_BLOBS.md` and `docs/SOUP/`.
- **Citation gate STRICT** -- every register write in
  `libs/ra8_hal/src/` carries a `/* HUM Ch X.Y p NNNN */` reference,
  enforced line-by-line by `check_line_citations.py`.

### Fixed

- **33 -> 0 host-test failures** -- closed every flake and TODO in
  the host-mock test suite; current run is 190/190 green.
- **USB clock handshake fix** -- `ra8_cgc` PLL2 path now produces the
  48 MHz USBFS reference correctly; `ra8_usb_fs` enumeration in the
  host-mock world matches the HUM 35.2 sequence.

### Changed

- **Qualification artifacts refreshed** -- `docs/SOUP/`,
  `docs/MCDC_GAPS.md`, `docs/MCDC_DEACTIVATIONS.md`,
  `docs/VENDOR_BLOBS.md`, `docs/DRIVER_STATUS.md`,
  `docs/ROADMAP.md`, and `docs/ROADMAP_DASHBOARD.md` all regenerated.
- **Stale docs cleaned** -- removed obsolete sweep-in-progress
  scratch notes; consolidated wave-table into the roadmap final-sweep
  status block.

### Hardware-blocked

These items are code-complete in the host-mock world but await
bench hardware before final sign-off:

- USB enumeration end-to-end verification (requires bus analyzer).
- LevelX IS25LX512M xSPI bring-up (requires logic analyzer + device).
- `lcd_demo` + `ereader` graphics demos (requires Parallel Graphics
  Expansion Board).

## [0.1.0] - 2026-05-01

First tagged baseline of the RA8D2 hand-written HAL, TrustZone substrate,
ThreadX/X-Ware vendor integration, board BSP, and the example app fleet. This
release captures roughly 235 commits of greenfield work landed in the last
three weeks.

### Added

- **Core HAL drivers (libs/ra8_hal)** brought up to FSP-peer level across many
  iterative sweeps: `ra8_sci`, `ra8_iic_b`, `ra8_spi_b`, `ra8_gpio`, `ra8_gpt`,
  `ra8_poeg`, `ra8_adc`, `ra8_dac_b`, `ra8_acmphs`, `ra8_crc`, `ra8_ulpt`, `ra8_agt`,
  `ra8_wdt`, `ra8_iwdt`, `ra8_rtc`, `ra8_cac`, `ra8_xspi`, `ra8_sdramc`, `ra8_canfd`,
  `ra8_sdhi`, `ra8_i3c`, `ra8_glcdc`, `ra8_pdm`, `ra8_usb_fs`, `ra8_usb_hs`,
  `ra8_eth` (split into swm/mfwd/coma/gwca/gptp sub-drivers), `ra8_dmac`,
  `ra8_dtc`, `ra8_elc`, `ra8_icu`, `ra8_cgc`, `ra8_pfs`, `ra8_mpc`, `ra8_ssie`,
  `ra8_dac`, `ra8_mipi_phy`, `ra8_dsi`, `ra8_ctsu` (touch), `ra8_pdc` (camera),
  `ra8_sce` (crypto), `ra8_rsip`, `ra8_flash` (MRAM), `ra8_mpu` (Cortex-M85
  configuration helper), and the `ra8_touch` / `ra8_gt911` capacitive-touch
  driver for the EK-RA8D2 panel.
- **Board support package** `libs/ra8_board_ek_ra8d2` providing on-board
  helpers for J-Link OB VCOM console, RGMII Ethernet pinmux, USBHS
  device/host CGC + MSTP wiring, MIPI-DSI panel bring-up via the PHY/DSI HAL,
  SSIE0 audio init + sample-block playback, and SW1/SW2 attach-IRQ via
  IRQ12/13 ELC events.
- **TrustZone substrate**: SAU bring-up scaffold, `secure_app` key vault,
  secure-fault handler, NS_MRAM / NS_SRAM linker partitions, and a full
  `libs/ra8_nsc` veneer set (comms drivers, I/O drivers, xspi_read,
  live `cmse` veneers).
- **Platform abstraction layers** `ra8_net_pal` and `ra8_usb_pal` with
  in-memory loopback rings, plus `ra8_sim_world` host mock.
- **Third-party stacks vendored as git subtrees**: Eclipse ThreadX kernel,
  FileX, LevelX, NetX, USBX (CDC, HID, MSC, audio, MSC host), lwIP
  (with ThreadX `sys_arch` port and `ra8_etha` netif glue), NimBLE host
  + Mesh + SMP/bonding + GATT client + patch loader stub, mbedTLS / TF-PSA
  -Crypto 4.x, FAT-FS adapter, LiteHTML for the e-reader.
- **Higher-level libraries**: `ra8_modem_at`, `ra8_power_profile`,
  `ra8_psa_crypto`, `ra8_wdt_supervisor`, `ra8_dotf`, `ra8_epub`, `ra8_reflow` v2
  (LiteHTML port + paginate-by-viewport-height), `ra8_ble_host`, OTA
  orchestration (Phase 5) with secure-side commit veneers, and the
  IT8951 e-paper driver with sleep/wake.
- **Example apps** under `examples/`: `blink`, `blink_hal`, `uart_hello`,
  `clock_check`, native USB CDC echo, USB device HID/MSC, USB host CDC/MSC/HID
  and USB host audio, lcd/motor/audio/ethernet/ptp/`ptp_master`/
  `ethernet_tcp_echo`, NimBLE central + mesh demos, ThreadX demos
  (`threadx_blink`, `threadx_filex_demo`, `threadx_levelx_demo`,
  `threadx_netx_tcp_echo`, `threadx_lwip_tcp_echo`, `threadx_https_client`,
  `threadx_usbx_cdc`, `threadx_sdcard_demo`, `threadx_canfd_demo`,
  `threadx_ota_demo`, `threadx_mpu_partition_demo`), `ra8_bootloader`, and
  IPC/audio/clock samples. Hardware-flashable example sweeps added
  six new apps in one batch (Sweep 7).
- **Tooling**: `tools/ra8_qe` JSON-driven configurator (Renesas QE-style),
  `scripts/dev/openocd` GPL alternative to the SEGGER flash/debug path,
  `scripts/builders/all_examples.sh`, Doxygen build target, roadmap dashboard,
  cppcheck/MISRA helpers, `make example-<name>` targets, `make test-docker`
  for macOS host-test runtime, and `VERSION`-driven `@since` consistency
  checking.

### Changed

- **Mass example migration to BSP**: 23 example apps were migrated to
  `ra8_board_ek_ra8d2` BSP helpers across five batches (blink/blink_hal/
  uart_hello, clock_check + 4 ThreadX demos, 5 ThreadX/BLE demos, lcd/motor/
  audio/ethernet/ptp, 5 USB device/host demos, threadx_usbx_cdc + usb_msc/hid,
  threadx_filex/levelx, threadx_netx + threadx_lwip TCP echo, threadx_https,
  audio_loopback + usb_audio_device).
- **Per-app boot refactor**: each `examples/<app>/` is now self-contained
  with its own `vector_table.c`, `system_init.c`, `secure_exception.c`,
  `trustzone_init.{c,h}`, `linker_script.ld`, `CMakeLists.txt`, `Makefile`.
- **CGC + SCI** rebuilt as a full FSP-aligned RA8 Gen2 PLL bring-up with
  proper clock switching and SCI clear/drain; SCI_B, IIC_B, and SPI_B
  retrofits for native register layouts.
- **`ra8_eth` split** into `swm`, `mfwd`, `coma`, `gwca`, `gptp` sub-drivers
  to match the ESWM topology.
- **Crypto stack** upgraded from mbedTLS to **TF-PSA-Crypto (Mbed TLS 4.x)**.
- **NimBLE direct APIs** wired behind `RA8_TARGET_BUILD` in `ra8_ble_host`.
- **`ra8_dotf`** refactored to extract `internal_*` helpers so `ra8_dotf_open`
  meets the NASA Rule 4 size threshold.
- **`ra8_wdt_supervisor`** shim canary swapped from magic numbers to a
  typed enum and wired into the host build.
- **`ra8_epub`** rewritten to remove `malloc`/`free` (NASA Rule 3).
- **HUM citations normalized** across HAL drivers; `cite_check --strict`
  gate adopted; `--strict` cite_check + Doxygen-clean status marked DONE in
  the roadmap.
- **Repository layout** flattened: all apps moved into `examples/`,
  `examples/` swept by format/lint/cite-check/world-tag scans, and
  `docs/ARCHITECTURE.md` added explaining `src/` vs `examples/` vs `libs/`.

### Fixed

- **Register layout corrections** discovered during FSP cross-verification
  sweeps: `PFS.PWPR` offset 0x003 -> 0x00C; ICU NMI + IELSR offsets and
  32-bit NMI register widths; six wrong peripheral base addresses corrected
  in `ra8d2_*_regs`; GLCDC offsets re-derived against `R_GLCDC_Type`;
  `CRCCR1` offset, `CRCSAR` width, and reset-bit corrections; `DTCST`
  offset + `DTCADMOD` added; `ELC` `ELSEGR`/`ELSR` strides; `system.PLLCCR2`
  offset 0x0B0 -> 0x04C; SPI `phantom padding` and `SPCMD` base; DMAC
  per-channel layout; SDHI switched to 32-bit registers with full layout;
  CANFD `CFDCNCFG` bit positions + widths; DAC_B re-derived as two
  single-channel instances; ULPT layout re-derived against `R_ULPT0_Type`;
  ACMPHS `CMPCTL` bit layout fixed and the fictional `CMPFIR` deleted;
  ADC_B re-derived to the real layout; OSPI re-derived against
  `R_XSPI0_Type`.
- **Fictional drivers removed**: `ra8_trng` and `ra8_sce` (initial scaffolds)
  deleted as not corresponding to real RA8D2 IP.
- **PFS write sequence** corrected to follow HUM Ch 20.2.4
  (PMR-clear-first), unblocking GPIO bring-up.
- **IRQ flow + Secure-PFS unlock** fixed so `blink_hal` LED actually
  toggles on hardware; `uart_hello` brought to "hello, ra8d2!" on the wire
  after multi-step SCI_B / TCLK debugging captured in the commit history.
- **Linker heap removed**: `_sbrk` trap added in `ra8_core` and the 4 KB
  heap region deleted from the linker script.
- **`ra8_sdramc`** SDC citation pages reverted to the chapter start
  (Ch 15 p 583).
- **README audit pass** across 30+ examples (blink/audio/ble/clock,
  uart/threadx_blink+filex+levelx, https/ipc/lwip/netx,
  nimble/sdcard/ble_central/mesh/usb_audio, ra8_bootloader/threadx canfd+
  mpu+ota+usbx_cdc) added validated footers and corrected drift.
- **Edge-case test coverage** added for `ra8_iic_b` (bus-busy, NAK addr/data,
  repeated-start, clock-stretch), `ra8_etha` + `ra8_rmac` (TX ring ceiling,
  RX overrun saturation, AN-wait race, pause framing, jumbo limits),
  `ra8_flash` (blank-check partial/boundary, page-cross writes, config_set
  rollback), and `ra8_rsip` (key-import collisions, AES non-block-multiple,
  AES-GCM null matrix, TRNG silent-failure guard).
- **`secure_app`** `key_import` enums reordered correctly and `ra8_psa_crypto`
  wired in.
- **USBHS test** assertion relaxed after USBHS init was promoted into
  `ra8_board_ek_ra8d2`.
- **`ra8_spi_b`** sim-mode short-circuit added; `test_ra8_epaper` happy-path
  re-enabled. `test_ra8_smbus` and `test_ra8_touch_cal` re-enabled.

### Documentation

- **`docs/STYLE_GUIDE.md`** and **`docs/RING_AND_WORLD.md`** added as the
  human-facing source of truth for the architectural-ring + TrustZone-world
  tagging system; `CLAUDE.md` restates the most-violated rules.
- **`docs/ARCHITECTURE.md`** explaining the `src/` vs `examples/` vs `libs/`
  split.
- **`docs/VENDOR_BLOBS.md`** documenting the RSIP-E50D and BLE patch
  binary blobs vendored in the tree.
- **`docs/ROADMAP.md`** progressively closed: WDT/IWDT/RTC/CAC, CRC/ULPT/
  AGT, ADC/DAC_B/ACMPHS, XSPI/SDRAMC, CANFD/SDHI/I3C, GLCDC/PDM,
  USB_FS/USB_HS, SCI/IIC/SPI/GPIO/GPT/POEG, plus DONE markers for
  cite_check --strict and Doxygen.
- **`README`** refresh with examples, plus per-app `README.md` for new apps
  (`blink`, `blink_hal`, `uart_hello`, `clock_check`, etc.).
- **`libs/README.md`** describing each subdirectory's role.
- **Roadmap stance** clarified across multiple commits: no e2 studio, vendor
  third-party libs and hand-write integration shims, ThreadX picked as the
  RTOS, all-in on ThreadX X-Ware + USBX, ePub e-reader as Phase 6.

### Internal

- **clang-tidy cleanup** to a zero-warning baseline repo-wide:
  - Wave A: lint our register headers + tests/mocks; strip ~2.3k redundant
    casts.
  - Wave B: drop 10 `NOLINT` directives via real refactors.
  - Wave C: re-enable 4 disabled clang-tidy checks; tighten rationale on
    the rest.
  - Per-driver clang-tidy clears for `ra8_modem_at` (with helper extraction),
    `ra8_psa_crypto`, `ra8_wdt_supervisor` (with cppcheck suppression), and
    `ra8_board_ek_ra8d2` audio init.
- **Project-wide `clang-format` baseline** applied; `@since` consistency and
  C23 init style fixes.
- **HAL fallout cleanup** after clang-tidy and strict-warning passes;
  externs hoisted and prototypes added in examples; ThreadX/X-Ware
  strict-warning relaxation to allow the vendored stacks to build clean.
- **Defensive-macro policy** finalized in HAL with refined pre-commit
  regexes; unified warning management and defensive-macro checks added to
  the build infrastructure.
- **CI / dev infrastructure**: `make test-docker` for macOS host-test
  runtime via colima; per-app boot refactor; pre-commit hook covers ASCII,
  format, tidy, and C23 patterns; `cite_check`, `check_world_tags`, and
  `check-since-version` utilities under `scripts/checks/`.
- **Coverage gate + Doxygen build + test backfill** landed as the closure
  pass for the v0.1.0 baseline.
- **Wave-N milestone references** stripped from source comments and docs in
  preparation for tagging.

[0.1.0]: https://github.com/bsikar/ra8-firmware/releases/tag/v0.1.0
