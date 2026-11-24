# ADR-0019: First-party dfu_bootloader instead of MCUboot

## Status

Proposed -- 2026-11-23. Records the rationale and trade-offs behind the
shipping `dfu_bootloader` architecture (`2d9b6b60d`, `ab8d86d8a`) for
repository-owner confirmation. Merging this record documents existing code
and links its follow-on epics without changing firmware behaviour.

## Context

`examples/ek_ra8d2/hw_validated/hil/dfu_bootloader` and `libs/ra8_dfu` were
written first-party (promoted to hardware-validated HIL in `2d9b6b60d`,
2026-06-16, and switched to copy-to-run in `ab8d86d8a`, 2026-06-16) rather
than integrating Zephyr/Trusted Firmware MCUboot (`bootutil`). No ADR or commit
message previously explained why MCUboot was passed over, nor recorded which
MCUboot capabilities are deliberately absent versus left as future gaps.

All facts below were verified against `dev` and the cited commits:

* **Memory budget (128 KiB bank):** `dfu_bootloader/CMakeLists.txt` pins the
  bootloader partition to `MRAM_LENGTH 128K` at `0x0200_0000`, splitting the
  remaining 896 KiB of the EK-RA8D2's 1 MiB code-MRAM evenly into two 448 KiB
  staging slots (Slot A at `0x0202_0000`, Slot B at `0x0209_0000`). Trimming
  `tf-psa-crypto` via an app-local `tf_psa_crypto_config_dfu.h` (`f43a741ae`)
  brought the `MinSizeRel` image to `61828 B` (47.17% of the bank), while
  `Debug` (`-O0 -g3`) headroom remains tracked in `RA8FW-353`.
* **Built-in recovery and staging transport:** When no valid slot exists or a
  DFU trigger is asserted, `dfu_bootloader` brings up ThreadX, USBX DFU 1.1 on
  USB-FS (`libs/ra8_dfu_device`), and `libs/ra8_dfu`'s MRAM slot programmer to
  stream an image straight into the inactive slot over USB.
* **RA8D2 MRAM hardware model:** The EK-RA8D2 stores code in 1 MiB of on-chip
  MRAM with a 32-byte write unit (`page_size = 0x20`) and no sector-erase /
  rewritable data-flash array (`libs/ra8_hal/inc/ra8_flash_regs.h`). Writes do
  not require erasing multi-kilobyte flash sectors or reserving a scratch area.
* **Header-last torn-write safety:** `libs/ra8_dfu` places the 32-byte
  `"RA8D"` slot header (`magic`, `seq`, `img_len`, `img_crc32`, `entry`) at the
  base of each slot and programmes it *last*, after every body block and CRC
  pass finishes. A power loss mid-transfer leaves `magic` unprogrammed without
  requiring swap-status state pages.
* **Copy-to-run execution (`ab8d86d8a`):** Because interface structs across
  this codebase store absolute function pointers in `.rodata` (incompatible
  with ROPI/RWPI), payloads are linked once against a fixed SRAM run window
  (`0x2202_0000`) and verified (`ra8_rot_launch`: ECDSA-P256 over SHA-256 plus
  extra-MRAM anti-rollback counter at `0x02E0_7600`) before `dfu_bootloader`
  copies the winning slot into SRAM with DSB/ISB barriers (I/D caches kept off)
  and branches to it.

## Decision

Keep the first-party `dfu_bootloader` + `libs/ra8_dfu` + `libs/ra8_rot_launch`
design instead of adopting MCUboot.

### Primary Drivers (For Owner Confirmation)

1. **Self-contained USB-DFU inside the 128 KiB bootloader bank:** `dfu_bootloader`
   needs an out-of-the-box USB-FS DFU 1.1 recovery and provisioning transport
   reusing the repository's existing USBX + ThreadX stack inside a single
   128 KiB bank. Adopting MCUboot would still require either integrating
   MCUmgr/SMP + serial recovery or writing custom USB-DFU glue around
   `bootutil` alongside `tf-psa-crypto` and ThreadX/USBX.
2. **MRAM fit over sector-erase swap:** MCUboot's standard swap-using-scratch /
   swap-using-move state machine is designed around NOR/flash erase sectors
   and multi-step swap status trailers. On RA8D2 MRAM, 32-byte page writes and
   copy-to-run staging let each slot be written sequentially and activated in a
   single 32-byte atomic commit at the slot header without flash-erase geometry,
   swap scratch space, or trailer wear.
3. **Copy-to-run for `.rodata` vtables:** Both `dfu_bootloader` and MCUboot's
   `BOOT_RAM_LOAD` mode handle location-independent staging with fixed-address
   `.rodata` function pointers; adopting copy-to-run (`ab8d86d8a`) solved that
   problem in ~80 lines of Zig/C (`libs/ra8_dfu/src/internal/launch.zig`)
   without porting MCUboot's flash-map, OS/crypto HAL shims, or trailer format.
4. **Tight alignment with repo C23/Zig/RoT substrate:** Keeping verification and
   slot state inside `libs/ra8_dfu`, `libs/ra8_dfu_device`, `libs/ra8_rot`, and
   `libs/ra8_rot_launch` preserves direct Zig unit/MC-DC test coverage,
   shared linker/MRAM abstractions, and unified policy control for the future
   OTA + DFU convergence (`RA8FW-286`).

### Alternatives Considered

* **Adopt MCUboot (`bootutil`) with `BOOT_RAM_LOAD` + serial/USB recovery:**
  Provides proven trial-boot/revert, encrypted images, multi-image dependency
  manifests, measured boot, compression, and fault-injection hardening (FIH)
  out of the box. Rejected because MCUboot brings a second SOUP surface,
  flash-sector trailer assumptions ill-suited to MRAM, and still leaves
  USBX-based USB-DFU integration work inside the 128 KiB bank budget.

## Feature Comparison: MCUboot vs dfu_bootloader

| Capability | MCUboot (`bootutil`) | `dfu_bootloader` (current `dev`) | Disposition |
| --- | --- | --- | --- |
| Built-in USB-DFU recovery | Requires external transport / MCUmgr | USB-FS DFU 1.1 via USBX + ThreadX | Strength kept |
| Storage assumption | Erase-sector flash + status trailers | 32B-page MRAM; header-last commit | MRAM native |
| Slot-invariant payloads | `BOOT_RAM_LOAD` or swap into slot 0 | Fixed SRAM copy-to-run (`0x2202_0000`) | Parity via `ab8d86d8a` |
| Signature + rollback gate | ECDSA/RSA/Ed25519 + NV counter | ECDSA-P256 + SHA-256 + extra-MRAM counter | Implemented |
| Trial boot and revert | `test`/`confirm` state + fallback | Missing (highest seq wins permanently) | Gap epic `RA8FW-873` |
| Encrypted images | AES-128/256 (RSA-OAEP / ECIES / KW) | Plaintext slots and USB-DFU stream | Gap epic `RA8FW-874` |
| Multi-image bundle | Coordinated primary/secondary checks | Single M85 payload only | Deferred (`RA8FW-286`) |
| Measured boot | Shared-SRAM CBOR boot measurements | None | Deferred |
| Image compression | Optional compressed upgrade slots | Uncompressed staging only | Deferred |
| Fault injection hardening | Configurable FIH profiles/delays | Standard fail-closed checks | Deferred |

## Consequences

* **Positive:** The bootloader fits cleanly in 128 KiB (`61828 B` in
  `MinSizeRel`), shares USBX/ThreadX/TF-PSA-Crypto with the rest of the
  repository, requires zero NOR-sector swap overhead on MRAM, and uses a
  simple header-last commit protocol verified on EK-RA8D2 hardware.
* **Negative / Technical Debt:** Two high-priority security and reliability
  capabilities present in MCUboot are missing today and must be built first-
  party:
  * **Trial boot with automatic revert (`RA8FW-873`):** Currently, any validly
    signed slot with the highest sequence number runs permanently, with no
    self-test confirmation or watchdog-triggered rollback. Copy-to-run makes
    trial state external to the immutable 32-byte header.
  * **Encrypted slot images (`RA8FW-874`):** Images sit in plaintext in MRAM
    and over USB-DFU. Because `dfu_bootloader` already decrypts/copies into
    the SRAM run base (`0x2202_0000`), ciphertext-at-rest decryption fits
    naturally in the copy-to-run path without writing plaintext back to MRAM.
* **Cross-cutting links:** `RA8FW-353` tracks closing `dfu_bootloader`'s Debug
  build MRAM overflow within the 128 KiB bank as these features land, and
  `RA8FW-286` tracks unifying `libs/ra8_ota` and `libs/ra8_dfu` around a single
  declared pipeline and N-image bundle format.
