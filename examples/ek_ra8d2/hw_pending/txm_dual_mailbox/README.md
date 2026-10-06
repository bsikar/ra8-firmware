# txm_dual_mailbox

A ThreadX module on each core of one image (RA8FW-843), and calls from one
core's module answered on the other core (RA8FW-844), under RA8EMU-159.

- The Cortex-M85 links `threadx_m85_modules` and carries txm_rpc_m33, the
  `ra8_rpc` client module from txm_rpc_cpu1, in its own `.txm_module` in MRAM
  (`linker_append.ld`). Its Module Manager thread loads and starts it in
  place. The module creates two queues and attaches them through an
  application request, greets the server, then calls `add` once a tick and
  reports each sum.
- CPU1, the Cortex-M33, links `threadx_m33_modules` and carries txm_hello_m33
  in `.txm_module` in MRAM_CPU1 (`linker_script_cpu1.ld`). Its Module Manager
  thread loads and starts it, then makes two kernel queues and runs an
  `ra8_rpc` server answering `add` on them.
- The mailbox block at 0x2210_0000 carries two one-message slots, request
  and reply. Once a tick each manager thread moves its queue messages into
  one slot and the other slot's message into a queue (`src/pump.zig`). A
  slot holds one ThreadX queue message (sixteen words), and the sender waits
  for the receiver's ack before writing the next, so `ra8_rpc` frames cross
  untouched.
- Each side checks the start thread's TX_THREAD_ID before trusting its run
  count: instance + 0xC0 in the M85's threadx_m85_modules build (RA8FW-825),
  instance + 0xD0 in CPU1's threadx_m33_modules build (measured, and what
  txm_manager_cpu1 reads). CPU1 rewrites the block's signature every tick.

The M85 prints one line once both start threads have run 10 times, then the
verdict once the module has reported 10 sums, each checked against its step:

```
txm_dual_mailbox: modules ran 10 times on both cores PASS
txm_dual_mailbox: 10 requests crossed the mailbox PASS
```

`txm_dual_mailbox: FAIL cpu1` means CPU1's release, one of its manager steps,
its queues or its server failed (the step is in the block);
`txm_dual_mailbox: FAIL` means the M85's own manager failed, the module
reported a failure or a wrong sum, or the run did not finish in 2000 ticks.

- `src/main.zig`: the M85 application, its Module Manager thread and the
  module's application requests.
- `src/cpu1_main.zig`: CPU1's `tx_application_define`, manager thread and
  server.
- `src/pump.zig`: queue messages into and out of the mailbox slots.
- `src/service.zig`: a copy of txm_rpc_cpu1's service, which txm_rpc_m33 is
  built against; it has to stay the same below its header.
- `src/shared.zig`: the mailbox block and the instance offsets both use.

Zig throughout, so no CMakeLists.txt. Build with `zig build arm`; the pair is
`zig-out/arm/txm_dual_mailbox.elf` and `txm_dual_mailbox_cpu1.elf`.

Next: the server moves into a module on CPU1, so the calls end in CPU1's
module; RA8FW-842 kills a faulting module on one core while the other runs
on.

Not yet validated on hardware, hence `hw_pending`.
