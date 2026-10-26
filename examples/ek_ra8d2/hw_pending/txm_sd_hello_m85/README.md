# txm_sd_hello_m85

The SD hello-world module proof of concept (RA8FW-829 and RA8FW-830, under
RA8FW-290). CPU0:

1. brings up the micro-SD card on SDHI1, mounts its FAT volume with ra8_fs
   and reads `txm_hello_m33.ra8app` (`src/card.zig`);
2. admits it through `ra8_appimg_verify` with a software Ed25519 backend and
   the pinned test public key from `libs/ra8_app/tools/test_key.zig`'s seed
   (`src/verify.zig`), then flips the file's last payload byte in RAM,
   checks the gate refuses that copy, and restores it (RA8FW-831);
3. reads and admits `txm_fault_m33.ra8app` from the same card the same way;
4. memory-loads the hello module through the ThreadX Module Manager, starts
   it unprivileged, waits for its start thread to run ten times, then stops
   and unloads it (`src/module.zig`), and does all of that a second time from
   the same image (RA8FW-1022). After each unload the manager's module-memory
   byte pool must be back to the free bytes it had before the load, or the run
   fails `leak`;
5. memory-loads the fault module with a memory-fault callback registered.
   Its start thread stores outside its MPU regions, the port's MemManage
   handler kills it and calls the callback, and the manager ticks ten more
   times (RA8FW-837);
6. reports the kill from the fault info the port saved (the address the
   module stored to, its PC, CFSR), stops and unloads the killed module with
   the pool coming back whole, and loads and runs the hello module once more:
   the firmware goes on to the next app (RA8FW-1024). This is the isolation
   demo: the fault module's store goes to 0x2210_0040, resident-image SRAM it
   was never given.

The app sets `txm_manager` in the build table: it links
`threadx_m85_modules` but packs no module, so the only copy that can run is
the one read off the card.

The card needs `txm_hello_m33.ra8app` and `txm_fault_m33.ra8app` in its
root. `zig build txm-hello-m33` installs both under `zig-out/arm/`.

Console, on a pass:

```
txm_sd_hello_m85: tampered image refused PASS
txm_sd_hello_m85: run 1 exited, unload returned N bytes, module pool F of F free
txm_sd_hello_m85: run 2 exited, unload returned N bytes, module pool F of F free
txm_sd_hello_m85: unloaded and reloaded the same image PASS
txm_sd_hello_m85: signed module loaded, ran, exited PASS
txm_sd_hello_m85: fault module stored to 0x22100040 from pc 0x........, MPU killed it (CFSR 0x00000082)
txm_sd_hello_m85: next app loaded and ran after the kill PASS
txm_sd_hello_m85: faulting module killed, firmware ran on PASS
```

N is what the loaded module held in the pool and F the pool's free bytes;
each unload must return all of N. Otherwise `txm_sd_hello_m85: FAIL <step>`
naming pins, card, mount, open, size, read, header, signature, tamper,
fault_signature, manager, notify, pool, load, start, run, stop, unload, leak
or fault.

In the emulator: `--sd-dir DIR` with both files in DIR. An empty DIR prints
`FAIL open`.
