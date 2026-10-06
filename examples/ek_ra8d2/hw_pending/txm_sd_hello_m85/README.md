# txm_sd_hello_m85

The SD hello-world module proof of concept (RA8FW-829 and RA8FW-830, under
RA8FW-290). CPU0:

1. brings up the micro-SD card on SDHI0, mounts its FAT volume with ra8_fs
   and reads `txm_hello_m33.ra8app` (`src/card.zig`);
2. admits it through `ra8_appimg_verify` with a software Ed25519 backend and
   the pinned test public key from `libs/ra8_app/tools/test_key.zig`'s seed
   (`src/verify.zig`), then flips the file's last payload byte in RAM,
   checks the gate refuses that copy, and restores it (RA8FW-831);
3. memory-loads the module through the ThreadX Module Manager, starts it
   unprivileged, waits for its start thread to run ten times, then stops and
   unloads it (`src/module.zig`).

The app sets `txm_manager` in the build table: it links
`threadx_m85_modules` but packs no module, so the only copy that can run is
the one read off the card.

The card needs `txm_hello_m33.ra8app` in its root. `zig build txm-hello-m33`
installs it under `zig-out/arm/`.

Console, on a pass, two lines:
`txm_sd_hello_m85: tampered image refused PASS` then
`txm_sd_hello_m85: signed module loaded, ran, exited PASS`. Otherwise
`txm_sd_hello_m85: FAIL <step>` naming pins, card, mount, open, size, read,
header, signature, tamper, manager, load, start, run, stop or unload.

In the emulator: `--sd-dir DIR` with the file in DIR. An empty DIR prints
`FAIL open`.

Next: RA8FW-837 adds the faulting-module negative (a module read off the
card that stores outside its MPU regions is killed; the firmware runs on).
