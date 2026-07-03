# secure_boot_hil

On-silicon proof that the root of trust **enforces** the boot on the EK-RA8D2.

With `RA_ENABLE_ROOT_OF_TRUST`, `ra_dfu_launch` verifies an image's ECDSA-P256
signature (SHA-256 body digest, via tf-psa-crypto) **and** the extra-MRAM
anti-rollback floor *before* copying it to the SRAM run base and branching --
default-deny on any failure. This app embeds one RoT-signed copy-to-run image
(`signed_payload.h`) and:

1. flips one body byte and hands the corrupted copy to `ra_dfu_launch` -- the
   signature check fails, the launch returns without copying/branching, and it
   prints `secure-boot: tampered REJECTED`;
2. hands the genuine signed image to `ra_dfu_launch` -- signature + anti-rollback
   pass, so it prints `secure-boot: ENFORCING OK` and branches to the run base
   (never returns). The launched payload advances a heartbeat at `0x22010004`
   (J-Link mem-probe bonus).

Validated on silicon (star HIL rig): `tampered REJECTED` then `ENFORCING OK`,
then silence (branched). No `LAUNCH FAILED` => the authentic image launched and
the flash-backed anti-rollback commit succeeded on hardware.

`signed_payload.h` is generated from `dfu_copy_to_run`'s 32-byte payload:
`build_payload.sh` -> `payload.bin` -> `tools/rot_sign.py sign --img-version 7`
-> embed. Regenerate + re-sign if that payload changes.

```
make secure_boot_hil
make hil-flash APP=secure_boot_hil
```
