# ota_ab_orchestration

Exercises the `libs/ra8_ota` A/B slot state machine end to end over rewritable
upper Code-MRAM (`0x02080000` inactive bank, `0x02082000` boot-select record),
and shows **both** terminal outcomes -- a successful commit and a safe rollback
-- in a single boot. This is the OTA *orchestration* layer, distinct from the
`dfu_bootloader` apps that own the USB-DFU transport and the reset-time boot
decision.

`ra8_ota` is Dependency-Inversion pure: it drives injected `net` / `crypto` /
`flash` interfaces, so this app supplies concrete backends and runs the flow
twice.

1. **Commit.** A good image is streamed into the inactive bank, the freshly
   programmed bank is re-hashed and its signature checked, and the inactive bank
   is latched as the next boot bank. The persistent boot-select record flips.
2. **Rollback.** A corrupted image is streamed in. The re-hash no longer matches
   the manifest digest, so verification fails with `k_ra8_err_crc_mismatch`, the
   machine parks in `error`, commit is never reached, and the boot-select record
   is left pointing at the still-good active bank.

## Why upper Code-MRAM

On RA8D2, the extra-MRAM window (`0x02E07600`) is one-time-programmable
option-setting/OTP storage with only sparse 16-byte program targets listed in
HUM Ch 59.7.4.5 Table 59.15 (p 3592); arbitrary 32-byte erases and re-writes do
not work there on silicon or in the emulator. Instead, the demo stages its A/B
inactive bank and boot-select record in the upper half of the 1 MiB Code-MRAM
(`0x02000000..0x02100000`, HUM Ch 59.1 / Ch 59.4.2 Figure 59.4 p 3548), far
above the firmware image (~22 KiB), with `ra8_flash.c` running out of SRAM via
`SRAM_TEXT` and fenced by `ra8_flash_set_window`.

## What each backend really is

| Interface | Backing | Honestly |
|-----------|---------|----------|
| `flash`   | The real `ra8_flash` Code-MRAM driver (`ra8_flash_write_block` / `ra8_flash_erase_block`, `k_ra8_flash_world_s` over `MRCPC1`) plus direct read-back. The persistent boot-select record stands in for the BTFLG boot-area swap. | Real. The Code-MRAM high-speed program sequence is executed out of SRAM and verified against read-back, driving the same register sequence on emulator and bench. |
| `crypto`  | SHA-256 via the real software backend. | Real, and identical on host, emulator and silicon -- there is no RSIP hash hardware on this part. Update **integrity**, the digest match that triggers the rollback, is genuinely verified. |
| `net`     | A local in-RAM image source. | **Not** a network. |
| authenticity | A demo `ecdsa_verify` gating on the demo key handle and tag width. | **Not** real ECDSA-P256. The rollback demonstration does not depend on it. |

## Not brick-territory

The demo stages into upper Code-MRAM (`0x02080000` / `0x02082000`) guarded by
`ra8_flash_set_window` -- **not** over the lower application `.text`, **not** by
touching option-setting anchors, the BTFLG configuration set, or permanent
block-protect fuses. It is safe to run on a stock EK-RA8D2.
