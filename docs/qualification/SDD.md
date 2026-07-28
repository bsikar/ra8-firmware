# Software Design Description (SDD)

**Last refreshed**: 2026-05-03 (refresh stamp recorded for audit-trail
consistency with the rest of the qualification document set; design-
level descriptions did not require number changes).

**Status**: First draft, 2026-05-02. Authored against the Phase 7 schedule
in [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) Section 3.
**DO-178C reference**: Section 11.10 (Design Description).
**IEC 61508-3 reference**: Clause 7.4 (Software design and development).
**ISO 26262-6 reference**: Clause 7 (Software architectural design) and
Clause 8 (Software unit design).
**Project**: `ra8-firmware`.
**Maintainer**: Brighton Sikarskie (single developer).

This SDD is the design-level companion to the requirements in
[`./SRS.md`](./SRS.md). It explains **how** the codebase satisfies each
REQ-XXX item: which module owns each requirement, which lower-ring
modules it consumes, what data it persists, and which algorithm /
state machine the implementation runs.

The architectural baseline of seven rings + three TrustZone worlds is
established in [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md); this SDD
does not duplicate that text -- it consumes it. Section 1 below points
into the relevant chapters of the existing design corpus rather than
re-stating them.

---

## 1. Architecture overview

### 1.1 Ring model (lower ring = closer to silicon)

Per [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md):

| Ring | Layer            | Index entry                                                                 | Owns REQ family   |
|-----:|------------------|------------------------------------------------------------------------------|-------------------|
| 0    | BSP boot         | `examples/<app>/{vector_table,system_init,secure_exception,trustzone_init}.c` + `linker_script.ld` | REQ-CHIP-XXX      |
| 1    | Core utilities   | [`../../libs/ra8_core/`](../../libs/ra8_core/)                                | REQ-CORE-XXX      |
| 2    | Register layouts | [`../../libs/ra8_hal/inc/ra8d2_*_regs.h`](../../libs/ra8_hal/inc/) (62 files) | REQ-CHIP-006      |
| 3    | HAL drivers      | [`../../libs/ra8_hal/src/ra8_*.c`](../../libs/ra8_hal/src/) (93 TUs) + PALs    | REQ-DRV-XXX, REQ-HAL-XXX |
| 4    | NSC veneers      | [`../../libs/ra8_nsc/`](../../libs/ra8_nsc/)                                  | REQ-PORT-001..009 |
| 5    | Secure-app       | [`../../src/secure_app/`](../../src/secure_app/)                            | REQ-PORT-010..013 |
| 6    | Application      | [`../../examples/ek_ra8d2/`](../../examples/ek_ra8d2/) (27 apps)             | REQ-APP-XXX       |

Higher rings may include lower-ring headers freely; the inverse is a
layering violation rejected by `scripts/checks/check_world_tags.py`.
This is the design-rule basis for IEC 61508-3 Clause 7.4.3.

### 1.2 TrustZone-M worlds

Three worlds: **S** (Secure), **NS** (Non-Secure), **NSC** (Non-Secure-
Callable veneer surface). The boundary is enforced by the SAU
configured in each app's `trustzone_init.c`. All NSC entry points
carry `__attribute__((cmse_nonsecure_entry))` and SHALL live under
[`../../libs/ra8_nsc/`](../../libs/ra8_nsc/) (REQ-PORT-001).

### 1.3 Boot flow

```
Reset
  -> Reset_Handler              (examples/<app>/vector_table.c)
  -> SystemInit                 (examples/<app>/system_init.c)
       -> ra8_cgc PLL bring-up       (libs/ra8_hal/src/ra8_cgc.c)
       -> SAU + IDAU partition      (examples/<app>/trustzone_init.c)
       -> ECC SRAM enable           (libs/ra8_hal/src/ra8_sram.c)
  -> __libc_init_array          (newlib startup glue)
  -> ra8_infrastructure_init     (libs/ra8_core/src/ra8_infrastructure.c)
       -> ra8_log_init
       -> ra8_time_init
       -> ra8_pin_validator_init
       -> ra8_register_protection_init
  -> board_init                 (libs/ra8_board_ek_ra8d2/src/...)
  -> main                       (examples/<app>/main.c)
```

Each step is owned by a single TU. The transitions are enforced by
the per-app linker script + the `__attribute__((constructor))` order
in `ra8_infrastructure.c`.

---

## 2. Module decomposition

Each module = one subdirectory under `libs/`. The columns are:

- **Public API** -- the header(s) under the module's `inc/` directory.
- **Internal helpers** -- non-exported `.c` files or `_internal.h`.
- **Depends on (lower rings)** -- the headers the module consumes.
- **Consumed by (higher rings)** -- the modules / apps that include
  the public API.
- **Implements** -- the REQ-XXX rows in [`./SRS.md`](./SRS.md).

### 2.1 Ring 1 -- core

| Module      | Public API (`libs/ra8_core/inc/`)                                                                                                                                   | Internal | Depends on | Consumed by | Implements              |
|-------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------|------------|-------------|-------------------------|
| ra8_core     | `ra8_err.h`, `ra8_check.h`, `ra8_log.h`, `ra8_time.h`, `ra8_time_interface.h`, `ra8_pin_interface.h`, `ra8_pin_validator.h`, `ra8_register_protection.h`, `ra8_register_guard.h`, `ra8_exception.h`, `ra8_error_handler.h`, `ra8_error_interface.h`, `ra8_infrastructure.h`, `ra8_stack_budget.h`, `ra8_bit_constants.h`, `ra8_gpio_constants.h`, `ra8_port_constants.h`, `ra8_time_constants.h`, `ra8_off_target_config.h` | -        | (none)     | every Ring 2/3 driver | REQ-CORE-001..014       |

Notes:
- `ra8_time_interface.h` and `ra8_pin_interface.h` are the DIP injection
  seams (intentional NASA P10 Rule 9 deviation, REQ-SAFE-009).
- `ra8_off_target_config.h` is the host-side substitute for the SysTick
  + IOPORT registers; it is included only when `RA8_BUILD_HOST` is set.

### 2.2 Ring 2 -- register layouts

| Module          | Public API                                                                | Implements                                                                            |
|-----------------|---------------------------------------------------------------------------|---------------------------------------------------------------------------------------|
| ra8_hal/regs     | `libs/ra8_hal/inc/ra8d2_*_regs.h` (62 files, one per peripheral family)    | REQ-CHIP-006 (every base address declared as `uintptr_t` typed enum per CLAUDE.md)    |

### 2.3 Ring 3 -- HAL drivers

The full per-driver enumeration is the REQ-DRV-XXX table in
[`./SRS.md`](./SRS.md) Section 4.3. Each driver TU is the smallest
testable unit; one driver = one REQ-DRV row = one host test.

Cross-cutting design points:

- Every driver returns `ra8_err_t` and propagates via `RA8_RETURN_ON_ERROR`.
- Every driver consumes its peripheral base via the typed enum in
  the matching `ra8d2_<name>_regs.h` header (no magic numbers).
- IRQ-bearing drivers register their handlers through `ra8_isr.c`, not
  by direct vector-table writes.
- All `MSTPCRx` clear/set operations route through `ra8_mstp.c` so that
  the bit-position is declared once.
- Protected-write windows (PRCR/PWPR) are entered exclusively via
  `ra8_register_protection.h` helpers; nested entry is detected by
  `ra8_register_guard.h`.

### 2.4 Ring 3 -- HAL aggregations and PALs

| Module              | Public API (`libs/<mod>/inc/`)                                                                | Depends on (Ring 3 drivers)                                | Implements      |
|---------------------|-----------------------------------------------------------------------------------------------|------------------------------------------------------------|-----------------|
| ra8_gfx              | `ra8_gfx.h`, `ra8_gfx_font.h`                                                                   | `ra8_glcdc`                                                  | REQ-HAL-001     |
| ra8_fs               | `ra8_fs.h`                                                                                     | FileX (SOUP), `ra8_xspi`, `ra8_sdhi`                          | REQ-HAL-002     |
| ra8_mpu              | `ra8_mpu.h`                                                                                    | core MPU regs (Ring 2)                                       | REQ-HAL-003     |
| ra8_wdt_supervisor   | `ra8_wdt_supervisor.h`                                                                         | `ra8_iwdt`, `ra8_wdt`                                          | REQ-HAL-004     |
| ra8_power_profile    | `ra8_power_profile.h`                                                                          | `ra8_lpm`, `ra8_pwr`, `ra8_vreg`                                | REQ-HAL-005     |
| ra8_net_pal          | `ra8_net_pal.h`                                                                                | `ra8_eth*`, `ra8_usb_hcdc_ecm`, `ra8_modem_at`                  | REQ-HAL-006     |
| ra8_usb_pal          | `ra8_usb_pal.h`                                                                                | `ra8_usb` and class drivers                                   | REQ-HAL-007     |
| ra8_net              | `ra8_net.h`                                                                                    | `ra8_net_pal`                                                  | REQ-HAL-008     |
| ra8_tls              | `ra8_tls.h`                                                                                    | Mbed TLS (SOUP), `ra8_psa_crypto`                              | REQ-HAL-009     |
| ra8_psa_crypto       | `ra8_psa_crypto.h`                                                                             | TF-PSA-Crypto (SOUP), `ra8_rsip*` (when HW path available)     | REQ-HAL-010     |
| ra8_ota              | `ra8_ota.h`                                                                                    | `ra8_flash`, `ra8_psa_crypto`, NSC `ra8_nsc_ota`                 | REQ-HAL-011     |
| NimBLE host (SOUP, via `port/nimble`) | NimBLE `host/ble_*.h` (`libs/third_party/nimble/`)                                          | `ra8_ble` (HCI transport seam; controller on ESP32-C6 companion), Apache NimBLE (SOUP) | REQ-HAL-012     |
| ra8_modem_at         | `ra8_modem_at.h`                                                                               | `ra8_sci` / `ra8_uart`                                          | REQ-HAL-013     |
| ra8_epub             | `ra8_epub.h`                                                                                   | `ra8_fs`, miniz (SOUP), TinyXML-2 (SOUP) via xml shim          | REQ-HAL-014     |
| ra8_reflow           | `ra8_reflow.h`                                                                                 | `ra8_gfx`, litehtml (SOUP) via xml shim                        | REQ-HAL-015     |
| ra8_touch_cal        | (header only, no public API beyond the calibration call)                                       | `ra8_touch`                                                    | REQ-HAL-016     |

### 2.5 Ring 4 -- board support

| Module              | Public API                                                                                    | Depends on                                                  | Implements           |
|---------------------|-----------------------------------------------------------------------------------------------|-------------------------------------------------------------|----------------------|
| ra8_board_ek_ra8d2   | `ra8_board_ek_ra8d2.h`                                                                          | `ra8_cgc`, `ra8_sdramc`, `ra8_glcdc`, `ra8_mpc`, `gpio.c`, `ra8_pin_validator` | REQ-BSP-001..004    |

### 2.6 Ring 4 -- NSC veneers

| Module             | Public API (`libs/ra8_nsc/inc/`)                              | Implements                                                       |
|--------------------|--------------------------------------------------------------|------------------------------------------------------------------|
| ra8_nsc             | `ra8_nsc.h`, `ra8_nsc_comms.h`, `ra8_nsc_io.h`, `ra8_nsc_veneer.h` | REQ-PORT-001..009 (one veneer TU per row, see SRS Section 4.6)   |

Each veneer TU contains exactly one `__attribute__((cmse_nonsecure_entry))`
function group; `__cmse_nonsecure_entry` outside this directory is
rejected by `check_world_tags.py`.

### 2.7 Ring 5 -- secure-app

| Module             | Public API (`src/secure_app/`) | Implements                                                       |
|--------------------|---------------------------------|------------------------------------------------------------------|
| key_vault          | `key_vault.h`                   | REQ-PORT-010 (256-bit symmetric key store)                       |
| key_import         | `key_import.h`                  | REQ-PORT-011 (wrapped-blob import + key-class enum)              |
| ota_commit         | `ota_commit.h`                  | REQ-PORT-012 (atomic MRAM bank swap)                              |
| secure_trng        | `secure_trng.h`                 | REQ-PORT-013 (entropy source for PSA-Crypto)                     |

### 2.8 Ring 6 -- applications

Each app under `examples/ek_ra8d2/<name>/` contains its own copy of
the per-app boot files (Ring 0) and a single `main.c` (Ring 6). Apps
do not share `vector_table.c` / `system_init.c` so two apps may
diverge memory layouts without coupling. The full inventory is the
REQ-APP-XXX table in [`./SRS.md`](./SRS.md) Section 4.7.

---

## 3. Data design

### 3.1 Memory map

The authoritative memory map is [`../MEMORY_MAP.md`](../MEMORY_MAP.md).
The salient SDD-level placement decisions are:

| Region            | Base         | Use                                                                                                    | Sizing source                                       |
|-------------------|--------------|--------------------------------------------------------------------------------------------------------|-----------------------------------------------------|
| ITCM (64 KiB)     | `0x00000000` | Hot-path code marked `__attribute__((section(".itcm.text")))` in selected TUs.                          | Fixed by silicon.                                   |
| MRAM-S (1 MiB)    | `0x02000000` | `.vectors`, `.text`, `.rodata`, OFS bytes. Linker script in each app.                                   | Fixed by silicon.                                   |
| MRAM-NS alias     | `0x02080000` | NS image alias for the single-image TrustZone build.                                                    | Linker script.                                      |
| DTCM (64 KiB)     | `0x20000000` | DMA descriptor pools, `ra8_log` ring buffer, scratch tied to ISR fast paths.                             | Per-app linker script.                              |
| SRAM-S (2 MiB)    | `0x22000000` | `.data`, `.bss`, ThreadX pools (when ThreadX is linked), framebuffers spilled out of SDRAM.              | `libs/ra8_core/inc/ra8_stack_budget.h`                |
| SRAM-NS alias     | `0x22100000` | NS-side `.data`/`.bss` for the single-image build.                                                      | Linker script.                                      |
| SDRAM (64 MiB)    | `0x68000000` | Primary framebuffer (1024x600x4 = 2.34 MiB per layer x N), GLCDC layer ping-pong.             | `ra8_sdramc.c`, `ra8_glcdc.c`.                        |
| Octo-SPI XIP      | (TBD enum)   | Optional XIP read window for large rodata blobs (apps that need it).                                     | `ra8_xspi.c`.                                        |
| Peripheral window | `0x40000000` | Hand-written register layouts in `libs/ra8_hal/inc/ra8d2_*_regs.h`.                                       | HUM Ch 7+.                                          |
| Core MPU regs     | `0xE000ED90` | Cortex-M85 MPU control accessed by `libs/ra8_mpu/`.                                                       | Armv8-M ARM.                                        |

### 3.2 Stack budgeting

Per-task stack sizes are declared in
[`../../libs/ra8_core/inc/ra8_stack_budget.h`](../../libs/ra8_core/inc/ra8_stack_budget.h)
and reproduced in [`../STACK_USAGE.md`](../STACK_USAGE.md). The
`-fstack-usage` outputs (`*.su` files) are aggregated by
`scripts/checks/stack_usage_check.py`. Build fails if any function
exceeds its declared bucket (REQ-PERF-008).

### 3.3 Persistent storage

| Asset                       | Location                                | Owner                                                    |
|-----------------------------|-----------------------------------------|----------------------------------------------------------|
| Active firmware image       | MRAM bank A or B at `0x02000000`         | `libs/ra8_ota/` + `src/secure_app/ota_commit.c`           |
| Wrapped key blobs           | Last MRAM block, S-only                  | `src/secure_app/key_import.c` + `key_vault.c`            |
| OFS bytes                   | MRAM offset per HUM Ch 6                 | `libs/ra8_hal/src/ra8_ofs.c` + per-app linker script        |
| TSN factory cal             | `0x02C1EDA0`                             | `libs/ra8_hal/src/ra8_tsn.c`                                |
| External NOR (LevelX-backed) | xSPI memory window                       | LevelX SOUP via `libs/ra8_fs/`                             |
| External SD card data       | FAT volume on SD-card via SDHI            | FileX SOUP via `libs/ra8_fs/src/ra8_fs_fat.c`              |

### 3.4 Configuration data

All compile-time configuration is C23 typed enums per
[`../../CLAUDE.md`](../../CLAUDE.md) "Constants and Macros". No
EEPROM-backed parameter file is in scope (PSAC Section 7.4).

---

## 4. Interface design (between rings)

### 4.1 Ring rule

Cross-ring calls SHALL go from a higher ring to a strictly lower ring
(N -> M with M < N). The check is enforced by
`scripts/checks/check_world_tags.py` reading the per-file `[Ring X / ...]`
header tags. `scripts/checks/cite_check.py` audits the corresponding
`@cite HUM-Ch-NN` references on Ring 2/3 TUs.

### 4.2 Public API contracts

Every public API across rings honours:

1. **Error domain** -- single `ra8_err_t` enum, success = `k_ra8_ok`,
   any non-success is propagated via `RA8_RETURN_ON_ERROR`.
2. **Direction-tagged params** -- `[in]`, `[out]`, `[in,out]` per
   `CLAUDE.md` Doxygen rules.
3. **NULL preconditions** -- explicit `RA8_CHECK_NULL_PTR` at function
   entry, never an `assert` (per NASA P10 Rule 5).
4. **Re-entrancy** -- documented per function in `@par Thread Safety`.
5. **No hidden global state** -- module state is encapsulated in a
   `static` struct inside the implementation TU; the public API takes
   a context pointer where re-entrancy is required.

### 4.3 NSC veneer contracts

NSC veneers SHALL:

- Sanitise NS-supplied pointers via the `cmse_check_address_range`
  intrinsic before any dereference.
- Copy NS-supplied scalars into S-side stack locals before use.
- Never return an S-side pointer to the NS caller.

The current pattern is shown in
[`../../libs/ra8_nsc/src/ra8_nsc_comms.c`](../../libs/ra8_nsc/src/ra8_nsc_comms.c)
and tested in [`../../tests/test_ra8_nsc_comms.c`](../../tests/test_ra8_nsc_comms.c).

---

## 5. State machines

### 5.1 OTA orchestrator (`libs/ra8_ota/`)

States: `idle -> fetching -> staged -> verifying -> committing -> done`.
Failure transitions return to `idle` with the cause logged via
`ra8_log_error`. Implementation: `libs/ra8_ota/src/ra8_ota.c`. Test:
`tests/test_ra8_ota.c`.

```
idle  -- ra8_ota_begin()        --> fetching
fetching -- bytes >= image_len --> staged
staged   -- ra8_ota_verify()    --> verifying
verifying -- hash_ok           --> committing
verifying -- hash_fail         --> idle (error)
committing -- bank_swap_ok     --> done
done -- ra8_ota_reboot()        --> (reset)
```

### 5.2 USB device CDC (`libs/ra8_hal/src/ra8_usb_cdc.c`)

States follow the standard USB enumeration FSM: `attached -> powered ->
default -> address -> configured -> suspended/resumed/disconnected`.
Implementation in `ra8_usb.c` + `ra8_usb_cdc.c`. Test:
`tests/test_ra8_usb_cdc.c`.

### 5.3 BLE host (Apache NimBLE via `port/nimble/`)

The BLE host is Apache NimBLE (SOUP, `libs/third_party/nimble/`),
consumed directly by applications through the `port/nimble/` ThreadX +
HCI-over-`ra8_ble` port; the former first-party BLE-host facade
was retired. NimBLE runs the host and the ESP32-C6 companion runs the
controller across the HCI transport seam. End-to-end coverage is
HW-blocked (REQ-HAL-012): on-wire BLE needs the ESP32-C6 companion. The
HCI transport seam (`ra8_ble`) is tested in `tests/test_ra8_ble.c`;
NimBLE host code is SOUP (see `docs/SOUP/nimble.md`).

### 5.4 Power profile (`libs/ra8_power_profile/`)

States mirror the RA8D2 LPM modes: `run -> sleep -> standby -> deep_standby ->
software_standby`. Wake events route through `ra8_lpm.c`. Test:
`tests/test_ra8_power_profile.c`.

### 5.5 Watchdog supervisor (`libs/ra8_wdt_supervisor/`)

States: `armed -> petting -> overdue -> reset_pending`. Each
registered task posts a heartbeat; the supervisor refuses to refresh
the IWDT if any task is `overdue`. Test:
`tests/test_ra8_wdt_supervisor.c`.

---

## 6. Algorithm design

### 6.1 HUM-cited algorithms

Every Ring 3 driver cites the HUM section it implements via an
`@cite HUM-Ch-NN` doxygen tag, audited by
`scripts/checks/cite_check.py`. Examples:

| Driver        | Algorithm / sequence                                | HUM reference                       |
|---------------|------------------------------------------------------|-------------------------------------|
| `ra8_cgc.c`    | PLL-from-MOSC bring-up sequence                      | HUM Ch 9 ("Clock Generation")       |
| `ra8_sdramc.c` | SDRAM mode-register write + auto-refresh setup       | HUM Ch 53 ("SDRAMC")                |
| `ra8_xspi.c`   | xSPI calibration + 8-line DDR mode select            | HUM Ch 56 ("xSPI")                  |
| `ra8_glcdc.c`  | Layer config + dot-clock divisor calculation         | HUM Ch 60 ("GLCDC")                 |
| `ra8_mipi_dsi.c`| DSI link bring-up + low-power escape                 | HUM Ch 61 ("MIPI DSI")              |
| `ra8_etha.c`   | ETHA descriptor-ring init + frame TX/RX              | HUM Ch 39 ("Ethernet Agent")        |
| `ra8_iic_b.c`  | I2C controller-mode bit-timing                       | HUM Ch 36 ("IIC-B")                 |
| `ra8_sci.c`    | UART baud-rate divisor selection                     | HUM Ch 35 ("SCI")                   |
| `ra8_flash.c`  | MRAM erase + program (HP-flash semantics)            | HUM Ch 50 ("Flash Memory")          |
| `ra8_iwdt.c`   | IWDT enable + refresh window                         | HUM Ch 32 ("IWDT")                  |
| `ra8_lpm.c`    | LPM transition gating (sleep/standby/deep-standby)    | HUM Ch 12 ("LPM")                   |

`cite_check.py` enforces the presence (warn mode today; planned
strict-mode flip during roadmap Phase 4).

### 6.2 Crypto algorithm choices

Per [`./PSAC.md`](./PSAC.md) Section 2.4 the crypto stack is Mbed TLS
+ TF-PSA-Crypto, both admitted as SOUP. The default cipher suite for
`ra8_tls` is **TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256** (RFC 5289 sec.
3.2). Hash defaults to SHA-256. The PSA key types in use are:

| Use                     | PSA key type                              | Algorithm                                  | Source                               |
|-------------------------|-------------------------------------------|--------------------------------------------|--------------------------------------|
| TLS handshake signing   | `PSA_KEY_TYPE_ECC_KEY_PAIR(SECP256R1)`    | `PSA_ALG_ECDSA(PSA_ALG_SHA_256)`           | `libs/ra8_tls/src/ra8_tls.c`           |
| OTA image authenticity  | `PSA_KEY_TYPE_ECC_PUBLIC_KEY(SECP256R1)`  | `PSA_ALG_ECDSA(PSA_ALG_SHA_256)`           | `src/secure_app/ota_commit.c`        |
| Symmetric key wrap      | `PSA_KEY_TYPE_AES`                        | `PSA_ALG_GCM`                              | `src/secure_app/key_import.c`        |
| Entropy                 | n/a                                       | TRNG via `secure_trng.c`                   | `src/secure_app/secure_trng.c`       |

The RSIP HW path for key wrap is BLOCKED-VENDOR (REQ-DRV-061/062); the
software fallback (`RA8_RSIP_SOFTWARE_BACKEND`) is emulator-only and
SHALL NOT ship in a certified build (PSAC Section 7.2).

### 6.3 Filesystem algorithm choices

`libs/ra8_fs/` wraps FileX (FAT) for SD-card volumes and LevelX (NOR
flash translation layer) for the EK-RA8D2 64 MiB Octo-SPI NOR. Both
are SOUP per [`../SOUP/filex.md`](../SOUP/filex.md) and
[`../SOUP/levelx.md`](../SOUP/levelx.md).

### 6.4 Network algorithm choices

`libs/ra8_net/` is a first-party ARP/IPv4/ICMP/UDP/TCP stack used for
the loopback path and for one Ethernet-attached app. Apps that need a
production-grade stack (e.g. `ethernet_tcp_echo`) link NetX Duo or
lwIP from the SOUP catalogue.

---

## 7. Error handling

### 7.1 Single-bottleneck pattern

Every cross-function error follows the pipeline:

```
inner_call()  --> ra8_err_t err = ...
                  if (err != k_ra8_ok) {
                      RA8_RETURN_ON_ERROR(err, TAG, "context message");
                  }
                  ...
```

The macro logs (level = error) via `ra8_log_error` and returns the
unmodified `err` value. This implements REQ-CORE-003 / REQ-SAFE-007
(NASA P10 Rule 7 -- check every return value).

### 7.2 Fault path

Hard fault / bus fault / usage fault / mem-manage / secure fault all
land in `libs/ra8_core/src/ra8_exception.c`, which captures the
processor exception-stack frame, logs it through `ra8_log_error`, and
hands off to `ra8_error_handler.c`. The latter is the single
controlled-halt bottleneck (REQ-CORE-010) and is the only place that
spins or resets after a fatal error.

### 7.3 Watchdog escape

`ra8_wdt_supervisor` (REQ-HAL-004) refuses to refresh the IWDT once any
registered task heartbeat is `overdue`. The IWDT therefore expires
and resets the chip rather than letting a wedged task hold the system
indefinitely. Reset cause is preserved in the RSTSR registers and
read back by `ra8_reset.c` (REQ-DRV-057) on the next boot.

### 7.4 SecureFault

A SecureFault SHALL trap to the per-app `secure_exception.c`
(REQ-CHIP-003) which feeds the same `ra8_error_handler` bottleneck.
This guarantees no S-side state is left in an indeterminate condition
after an SAU violation.

---

## 8. Concurrency

### 8.1 ThreadX scheduling (when linked)

Apps under `examples/ek_ra8d2/threadx_*/` link ThreadX 6.5.0 from the
SOUP catalogue ([`../SOUP/threadx.md`](../SOUP/threadx.md)). The
ThreadX timer-tick is driven from the same SysTick that backs
`ra8_time` so monotonic time is consistent in either bare-metal or
RTOS builds.

### 8.2 Bare-metal main loops

Apps without ThreadX run a simple `while(1)` main loop with optional
`__WFI()` between events; IRQs do all real work. The IWDT is refreshed
from the main loop only after `ra8_wdt_supervisor` confirms heartbeats.

### 8.3 IRQ priority assignment

NVIC priority assignment is centralised in `libs/ra8_hal/src/ra8_isr.c`
(REQ-DRV-040). Default priorities:

| Source class                       | Priority (lower = higher)  | Rationale                                  |
|------------------------------------|----------------------------|--------------------------------------------|
| HardFault / NMI                    | (architectural, fixed)     | Cortex-M architecture.                     |
| ECC fault, MMPU/MPU fault          | 0                          | Safety-critical detection path.            |
| SCI RX/TX                          | 4                          | UART back-pressure budget.                 |
| Ethernet ETHA RX                   | 5                          | Frame loss avoidance.                      |
| GLCDC line interrupt                | 6                          | Display tearing avoidance.                 |
| GPT / AGT                           | 8                          | General timer events.                      |
| ICU IRQn (board-level GPIO)         | 12                         | User-button class.                         |
| SysTick                             | 14                         | Coarsest tick.                             |

These are typed enums in `libs/ra8_core/inc/ra8_isr_priority.h` (when
that header lands during Phase 3 Doxygen sweep) and consumed by every
HAL driver through `ra8_isr_install`.

### 8.4 TrustZone S/NS boundary semantics

- All NS calls into S go through NSC veneers in
  [`../../libs/ra8_nsc/`](../../libs/ra8_nsc/) (Section 4.3).
- The `xxxSAR` peripheral security-attribution registers are written
  exclusively from S during boot (`trustzone_init.c` per app).
- IRQs are routed to S unless the corresponding `ICUSAR` bit clears
  the route to NS; this assignment is fixed at boot.
- The ITM / DWT trace ports are S-only.

### 8.5 Re-entrancy classification

| Module class                | Re-entrant | Notes                                                                     |
|-----------------------------|------------|---------------------------------------------------------------------------|
| `ra8_log`                    | Yes        | Lock-free ring buffer with single-producer per priority.                   |
| `ra8_time`                   | Yes        | SysTick read is a single 32-bit load.                                      |
| `ra8_pin_validator`          | No         | Init-time only; caller must mask IRQs.                                     |
| Driver `_init` functions    | No         | Init-time only; caller must mask IRQs.                                     |
| Driver `_read`/`_write`     | Per driver | Documented in each driver's `@par Thread Safety`.                           |
| NSC veneers                 | Yes        | Re-entrant from NS; veneer body is short and copies its inputs to S stack. |
| `ra8_error_handler`          | One-shot   | First call wins; subsequent calls spin.                                     |

---

## 9. Verification mapping

Every REQ-XXX item in [`./SRS.md`](./SRS.md) maps to either a host
test under [`../../tests/`](../../tests/) or a verification gap. The
gap inventory is:

- [`./SVP.md`](./SVP.md) Section 1 -- DO-178C Annex A objective tables.
- [`./SRS.md`](./SRS.md) Section 8.2 -- per-REQ untraced / blocked items.
- [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) Section 2 -- live metrics.

The SDD does not own gap closure; it owns the design choices that the
gap-closure work is verifying.

---

## 10. References

- [`../../CLAUDE.md`](../../CLAUDE.md) -- coding rules and NASA P10 mapping.
- [`../STYLE_GUIDE.md`](../STYLE_GUIDE.md) -- human-facing style guide.
- [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md) -- architectural-ring + TrustZone-world tagging.
- [`../MEMORY_MAP.md`](../MEMORY_MAP.md) -- memory map and partition assignments.
- [`../STACK_USAGE.md`](../STACK_USAGE.md) -- per-task stack budgets.
- [`../ARCHITECTURE.md`](../ARCHITECTURE.md) -- complementary architectural notes.
- [`../DRIVER_STATUS.md`](../DRIVER_STATUS.md) -- per-driver maturity table.
- [`./SRS.md`](./SRS.md) -- requirements satisfied by this design.
- [`./PSAC.md`](./PSAC.md) -- gateway artefact; references this SDD in Section 5.
- [`./SVP.md`](./SVP.md) -- verification plan.
- [`../SOUP/`](../SOUP/) -- pre-existing software register.
- [`../VENDOR_BLOBS.md`](../VENDOR_BLOBS.md) -- vendor-blob blocker register.
- [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) -- 22-week schedule.
- IEC 61508-3:2010 Clause 7.4 (Software design and development).
- RTCA DO-178C:2011 Section 11.10 (Design Description).
- ISO 26262-6:2018 Clauses 7 and 8.
