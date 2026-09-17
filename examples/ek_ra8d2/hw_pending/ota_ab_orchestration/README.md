# ota_ab_orchestration

Exercises the `libs/ra8_ota` A/B slot state machine end to end over the on-chip
extra-MRAM bank, and shows **both** terminal outcomes -- a successful commit and
a safe rollback -- in a single boot (#260). This is the OTA *orchestration*
layer, distinct from the `dfu_bootloader` apps that own the USB-DFU transport and
the reset-time boot decision.

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

## What each backend really is

| Interface | Backing | Honestly |
|-----------|---------|----------|
| `flash`   | The real `ra8_flash` extra-MRAM driver plus direct read-back. The persistent boot-select record stands in for the BTFLG boot-area swap. | Real. The MACI program/erase sequence is modelled off-target, so the stage / re-hash / boot-select path drives the same register sequence a bench build does. |
| `crypto`  | SHA-256 via the real software backend. | Real, and identical on host, emulator and silicon -- there is no RSIP hash hardware on this part. Update **integrity**, the digest match that triggers the rollback, is genuinely verified. |
| `net`     | A local in-RAM image source. | **Not** a network. |
| authenticity | A demo `ecdsa_verify` gating on the demo key handle and tag width. | **Not** real ECDSA-P256. The rollback demonstration does not depend on it. |

## Not brick-territory

The demo persists its A/B selection in a small boot-select record in extra-MRAM
data flash -- **not** by touching the option-setting anchors, the BTFLG
configuration set, or permanent block-protect fuses. It is safe to run on a stock
EK-RA8D2.
