# txm_dual_mailbox

A ThreadX module on each core of one image (RA8FW-843, under RA8EMU-159).

- The Cortex-M85 links `threadx_m85_modules` and carries txm_hello_m33 in
  its own `.txm_module` in MRAM (`linker_append.ld`). Its Module Manager
  thread loads and starts that module in place.
- CPU1, the Cortex-M33, links `threadx_m33_modules` and carries its own copy
  in `.txm_module` in MRAM_CPU1 (`linker_script_cpu1.ld`). Its Module Manager
  thread loads and starts it, then copies the start thread's run count into
  the mailbox block at 0x2210_0000 once per tick.
- Each side checks the start thread's TX_THREAD_ID before trusting its run
  count: instance + 0xC0 in the M85's threadx_m85_modules build (RA8FW-825),
  instance + 0xD0 in CPU1's threadx_m33_modules build (measured, and what
  txm_manager_cpu1 reads). CPU1 rewrites the block's signature every tick.

The M85 prints the verdict on the SCI8 VCOM console once both start threads
have run 10 times:

```
txm_dual_mailbox: modules ran 10 times on both cores PASS
```

`txm_dual_mailbox: FAIL cpu1` means CPU1's release or one of its manager steps
failed (the step is in the block); `txm_dual_mailbox: FAIL` means the M85's
own manager failed or the modules did not reach 10 runs in 2000 ticks.

- `src/main.zig`: the M85 application and its Module Manager thread.
- `src/cpu1_main.zig`: CPU1's `tx_application_define` and manager thread.
- `src/shared.zig`: the mailbox block and the instance offsets both use.

Zig throughout, so no CMakeLists.txt. Build with `zig build arm`; the pair is
`zig-out/arm/txm_dual_mailbox.elf` and `txm_dual_mailbox_cpu1.elf`.

Next: RA8FW-844 sends the M85 module's requests across the mailbox to CPU1's
module and back; RA8FW-842 kills a faulting module on one core while the
other runs on.
