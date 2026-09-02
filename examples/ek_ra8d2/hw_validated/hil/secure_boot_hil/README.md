# secure_boot_hil

On-silicon proof that the root of trust **enforces** the boot.

With `RA8_ENABLE_ROOT_OF_TRUST`, `ra8_dfu_launch` verifies an image's
ECDSA-P256 signature (SHA-256 body digest, via tf-psa-crypto) **and** the
extra-MRAM anti-rollback floor *before* copying it to the SRAM run base and
branching, default-denying on any failure. This app embeds one RoT-signed
copy-to-run image and runs both sides of that:

1. It flips one body byte and hands the corrupted copy to `ra8_dfu_launch`. The
   signature check fails, the launch returns without copying or branching, and
   the app reports the rejection.
2. It hands over the genuine image. Signature and anti-rollback both pass, so
   the launch branches to the run base and never returns. The launched payload
   advances a heartbeat at `0x22010004` as a mem-probe bonus.

Reject-then-accept is the whole design: an enforcement path only ever fed good
input proves nothing, and one that silently accepts is exactly the failure that
matters. Going quiet after the accept banner is the success signal, because the
app branched away and the anti-rollback commit stuck.

`inc/signed_payload.h` is generated from `dfu_copy_to_run`'s payload via
`examples/ek_ra8d2/hw_validated/hil/dfu_copy_to_run/scripts/build_payload.sh` and
`tools/rot/src/rot_sign.py`. Regenerate and re-sign it whenever that payload
changes.
