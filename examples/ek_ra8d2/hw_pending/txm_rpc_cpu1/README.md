# txm_rpc_cpu1

CPU1, the RA8D2's Cortex-M33, runs the ThreadX Module Manager and loads a
module that calls a service in the resident image through `ra8_rpc`
(`txm_rpc_m33`, RA8FW-544). The Cortex-M85 releases CPU1 and reports on the
VCOM console once the module has reported ten sums, each one the sum
expected:

```
txm_rpc_cpu1: add returned 101 202 303 404 PASS
```

The module creates two ThreadX queues in its own memory, one each way, and
hands them to the resident image as an application request. It runs an
`ra8_rpc` client over a `QueueTransport` on those queues, bound through
`ra8_rpc_tx`'s module Api, so every queue call goes through the module's
kernel-call dispatcher. The resident image runs an `ra8_rpc` server on the
same queues from its Module Manager thread, bound to the kernel's own entry
points. The module greets the server, then calls `add(n, 100 n)` once a tick
for n from one and checks each answer. Those four are its first four sums.

The client reaches its transport, and the transport its queues, through
constant vtables: seven words of the module's data that hold function
addresses. The module is built through C, and its start-up rebases those
words (RA8FW-539). `zig build txm-module-check` holds the same source to the
relocation check, and a copy with one record left out has to fail it.

- `src/main.zig`: the M85 application.
- `src/cpu1_main.zig`: CPU1's `tx_application_define`, the Module Manager
  thread that loads the module and then serves it, and
  `_txm_module_manager_application_request`, which takes the module's queues
  and records each sum, and whether it was right, in the shared block.
- `src/module_start.zig`: the module's start thread, the client.
- `src/service.zig`: what both sides agree on: the method, its messages, the
  queue geometry and the application requests.
- `src/shared.zig`: the shared-SRAM block at 0x2210_0000.
- `linker_script_cpu1.ld`: the board's M33 map plus `.txm_module`, where the
  packed module sits (`Cpu1Image.txm_module`).

There is no CMakeLists.txt: `ra8_add_app()` cannot declare a Zig main, so this
app exists only in the Zig build graph. `zig build arm` emits
`txm_rpc_cpu1.elf` (with CPU1's image embedded) and `txm_rpc_cpu1_cpu1.elf`.

Not yet validated on hardware, hence `hw_pending`.
