# txm_sd_hello_m85

The SD hello-world module proof of concept (RA8FW-829 and RA8FW-830, under
RA8FW-290). CPU0:

1. brings up the micro-SD card on SDHI0, mounts its FAT volume with ra8_fs
   and reads `txm_hello_m33.ra8app` (`src/card.zig`);
2. admits it through `ra8_appimg_verify` with a software Ed25519 backend and
   the pinned test public key from `libs/ra8_app/tools/test_key.zig`'s seed
   (`src/verify.zig`), then flips the file's last payload byte in RAM,
   checks the gate refuses that copy, and restores it (RA8FW-831);
3. reads and admits `txm_fault_m33.ra8app` from the same card the same way;
4. memory-loads the hello module through the ThreadX Module Manager, starts
   it unprivileged, waits for its start thread to run ten times, then stops
   and unloads it (`src/module.zig`);
5. memory-loads the fault module with a memory-fault callback registered.
   Its start thread stores outside its MPU regions, the port's MemManage
   handler kills it and calls the callback, and the manager ticks ten more
   times (RA8FW-837).

The app sets `txm_manager` in the build table: it links
`threadx_m85_modules` but packs no module, so the only copy that can run is
the one read off the card.

The card needs `txm_hello_m33.ra8app` and `txm_fault_m33.ra8app` in its
root. `zig build txm-hello-m33` installs both under `zig-out/arm/`.

Console, on a pass, three lines:
`txm_sd_hello_m85: tampered image refused PASS`,
`txm_sd_hello_m85: signed module loaded, ran, exited PASS`, then
`txm_sd_hello_m85: faulting module killed, firmware ran on PASS`. Otherwise
`txm_sd_hello_m85: FAIL <step>` naming pins, card, mount, open, size, read,
header, signature, tamper, fault_signature, manager, notify, load, start,
run, stop, unload or fault.

In the emulator: `--sd-dir DIR` with both files in DIR. An empty DIR prints
`FAIL open`.
