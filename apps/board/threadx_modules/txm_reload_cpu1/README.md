# txm_reload_cpu1

CPU1, the RA8D2's Cortex-M33, runs the ThreadX Module Manager through the
whole load/unload round trip of upstream's `sample_threadx_module_manager.c`.
It loads the hello-world module (`txm_hello_m33`) in place from its own MRAM
and starts it. Once the start thread has run 10 times it stops and unloads the
module, then loads and starts it again from the same blob. The Cortex-M85
releases CPU1 and reports on the VCOM console once the second load's start
thread has run 10 times:

```
txm_reload_cpu1: module loaded 2 times, ran 10 times each PASS
```

This shows that unload leaves the manager in a state a second load succeeds
from (RA8FW-774, the firmware half of the emulator's second-load check).

Everything is Zig:

- `src/main.zig`: the M85 application.
- `src/cpu1_main.zig`: CPU1's `tx_application_define` and the Module Manager
  thread (initialize, object pool, then load, start, stop, unload, load,
  start). The vector table, reset path and SysTick retune come from the
  `threadx_cpu1` glue; the kernel is `threadx_m33_modules`.
- `src/shared.zig`: the shared-SRAM block at 0x2210_0000. It records the
  first manager step that failed, the round (1 or 2) and that round's run
  count.
- `linker_script_cpu1.ld`: the board's M33 map plus `.txm_module`, where the
  packed module sits (`Cpu1Image.txm_module`).

It is `txm_manager_cpu1` (RA8FW-431) plus the stop/unload/reload steps.
There is no CMakeLists.txt; the app exists only in the Zig build graph.
`zig build arm` emits `txm_reload_cpu1.elf` (with CPU1's image embedded) and
`txm_reload_cpu1_cpu1.elf`.

Not yet validated on hardware, hence `hw_pending`.
