# txm_dual_mailbox

A ThreadX module on each core of one image (RA8FW-843), and calls from one
core's module answered by the other core's module (RA8FW-844, RA8FW-849),
under RA8EMU-159.

- The Cortex-M85 links `threadx_m85_modules` and carries txm_dual_client_m33,
  the `ra8_rpc` client module built through C from `src/client_module.zig`
  (it began as txm_rpc_cpu1's), in its own `.txm_module` in MRAM
  (`linker_append.ld`). Its Module Manager thread loads and starts it in
  place. The module creates two queues and attaches them through an
  application request, greets the server, then calls `add` ten times, a tick
  apart, and reports each sum. Then it calls `fault` (RA8FW-842).
- CPU1, the Cortex-M33, links `threadx_m33_modules` and carries
  txm_dual_server_m33 in `.txm_module` in MRAM_CPU1 (`linker_script_cpu1.ld`),
  built through C from `src/server_module.zig` (RA8FW-849). Its Module Manager
  thread loads and starts it. The module creates two queues in its own memory,
  attaches them through an application request, and runs the `ra8_rpc` server
  answering `add` on them; it reports each answer so CPU1 can count it. CPU1's
  resident image serves nothing itself, it only moves messages.
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

- `fault` asks CPU1's server module to store at 0x2210_0100, just past the
  block and outside its MPU regions. CPU1's manager takes the MemManage and
  kills the module; the memory-fault callback CPU1 registered counts it in the
  block. From then on CPU1 stops feeding the module's queues and refuses every
  call itself with an `ra8_rpc` fault frame carrying `module_gone` (0x0100),
  starting with the call the module died on. The client reports that refusal.

The M85 prints one line once both start threads have run 10 times, one once
the module has reported 10 sums, each checked against its step, and the
verdict once CPU1 counted exactly one fault, the module heard the refusal, and
the M85 ticked 10 more times:

```
txm_dual_mailbox: modules ran 10 times on both cores PASS
txm_dual_mailbox: 10 requests crossed the mailbox PASS
txm_dual_mailbox: CPU1 module faulted, M85 ran on PASS
```

`txm_dual_mailbox: FAIL cpu1` means CPU1's release or one of its manager steps
failed, its module never attached its queues or reported a failure, CPU1
could not refuse a call, or CPU1 counted more than one fault (the step is in
the block);
`txm_dual_mailbox: FAIL` means the M85's own manager failed, the module
reported a failure or a wrong sum, or the run did not finish in 2000 ticks.

- `src/main.zig`: the M85 application, its Module Manager thread and the
  module's application requests.
- `src/cpu1_main.zig`: CPU1's `tx_application_define`, manager thread, the
  module's application requests, and the pump.
- `src/server_module.zig`: txm_dual_server_m33's start thread, the server.
- `src/client_module.zig`: txm_dual_client_m33's start thread, the client.
- `src/pump.zig`: queue messages into and out of the mailbox slots.
- `src/service.zig`: the service both modules are built against, txm_rpc_cpu1's
  plus `fault`.
- `src/shared.zig`: the mailbox block and the instance offsets both use.

Zig throughout, so no CMakeLists.txt. Build with `zig build arm`; the pair is
`zig-out/arm/txm_dual_mailbox.elf` and `txm_dual_mailbox_cpu1.elf`.

Not yet validated on hardware, hence `hw_pending`.
