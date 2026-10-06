# txm_sd_hello_m85

Slice 1 of the SD hello-world module proof of concept (RA8FW-829, under
RA8FW-290). CPU0 brings up the micro-SD card on SDHI0, mounts its FAT volume
with ra8_fs, reads `txm_hello_m33.ra8app` and checks its `.ra8app` header:
the magic, and that the declared code and data sizes match the file.

The card needs `txm_hello_m33.ra8app` in its root. `zig build txm-hello-m33`
installs it under `zig-out/arm/`.

Console: `txm_sd_hello_m85: read N bytes, header ok PASS`, or
`txm_sd_hello_m85: FAIL <step>` naming pins, card, mount, open, size, read
or header.

In the emulator: `--sd-dir DIR` with the file in DIR. An empty DIR prints
`FAIL open`.

Next: RA8FW-830 verifies the signature and runs the module through the
ThreadX Module Manager.
