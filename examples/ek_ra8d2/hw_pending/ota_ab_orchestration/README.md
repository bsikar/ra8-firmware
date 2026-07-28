# ota_ab_orchestration

A/B OTA orchestration demo for the EK-RA8D2. It exercises the `libs/ra8_ota`
A/B slot state machine end to end and shows **both** terminal outcomes -- a
successful **commit** and a safe **rollback** -- in a single boot, over the
on-chip extra-MRAM (data-flash) bank at `0x27000000`.

Issue: [#260](https://github.com/bsikar/ra8-firmware/issues/260) (the ra8_ota
example-coverage gap). This is the OTA *orchestration* layer, distinct from the
`dfu_bootloader` HIL apps that own the USB-DFU transport + the reset-time boot
decision.

## What it does

`ra8_ota` is Dependency-Inversion pure: it drives injected `net` / `crypto` /
`flash` interfaces. This app supplies concrete backends and runs the A/B flow
twice:

1. **COMMIT path** -- a good image is streamed into the inactive bank
   (`ra8_ota_download_to_inactive_bank`), the freshly programmed bank is
   re-hashed and its signature checked (`ra8_ota_verify_signature`), and the
   inactive bank is latched as the next boot bank
   (`ra8_ota_commit_and_reboot` -> `flash.set_startup`). The persistent
   boot-select record flips from bank **A** to bank **B**.
2. **ROLLBACK path** -- a corrupted image is streamed into the inactive bank.
   The re-hash no longer matches the manifest digest, so verification fails
   (`k_ra8_err_crc_mismatch`), the machine parks in `error`, commit is never
   reached, and the boot-select record is left pointing at the still-good
   active bank **A**.

A healthy run prints, once per report cycle, on the J-Link OB VCOM console:

```
ota_ab: stage=ok commit=Y rollback=Y ok=Y
```

## Backends (EIL==HIL)

| Interface | Backing | Fidelity |
|-----------|---------|----------|
| `flash`   | Real `ra8_flash` extra-MRAM driver (`ra8_flash_extra_mram_write` / `_erase` + direct read-back). The persistent boot-select record stands in for the BTFLG boot-area swap, on brick-safe data-flash. | `tools/ra8_emulator` models the MACI program/erase sequencer (`board_periph_mram.c`), so the stage / re-hash / boot-select path runs the exact register sequence a bench build drives. |
| `crypto`  | SHA-256 via the real software backend (`ra8_rsip_sha256*`). | Identical on host, emulator and silicon (there is no RSIP hash-hardware on this part). Update **integrity** -- the digest match that triggers the rollback -- is verified for real. |
| `net`     | Local in-RAM image source. | **Not** a network. `TODO`: bind to a TLS stream once `ra8_tls` lands. |
| authenticity | Demo `ecdsa_verify` stub gating on the demo key handle + tag width. | **Not** real ECDSA-P256. `TODO`: swap in a `tf-psa-crypto` / `ra8_psa_crypto` verifier (already proven in `secure_boot_hil` / `psa_crypto_hil`). The rollback demonstration does not depend on it. |

## Build / run

```sh
make                 # cross-compile build/ota_ab_orchestration.elf / .hex / .bin
```

Emulator (headless, no board):

```sh
scripts/emu/smoke.sh ota_ab_orchestration     # or: scripts/emu/eil_all.sh
```

The `hil.conf` (`HIL_MODE=uart_scrape`) is checked by both `scripts/hil/all.sh`
(bench) and `scripts/emu/eil_all.sh` (emulator), and the EIL==HIL parity gate
(`scripts/checks/check_hil_eil_parity.py`) enforces that this app is exercised in
both.

## Not brick-territory

The demo persists its A/B selection in a small boot-select **record in
extra-MRAM data-flash**, not by touching the option-setting anchors, the BTFLG
configuration-set, or permanent block-protect fuses. It is safe to run on a
stock EK-RA8D2.
