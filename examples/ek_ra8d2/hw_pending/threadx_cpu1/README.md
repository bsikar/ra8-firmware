# threadx_cpu1

CPU1, the RA8D2's Cortex-M33, runs the ThreadX kernel with one thread. The
Cortex-M85 releases it and reports on the VCOM console once CPU1's kernel has
ticked 10 times:

```
threadx_cpu1: 10 ticks PASS
```

Both halves are Zig (RA8FW-404):

- `src/main.zig`: the M85 application (`CrossApp.zig_main`, RA8FW-408).
- `src/cpu1_main.zig`: CPU1's `tx_application_define` and its thread. The
  vector table, reset path and SysTick retune come from
  `port/threadx/src/cortex_m33/threadx_cpu1.zig` (RA8FW-409), imported as
  `threadx_cpu1` because the image names `uses = threadx_m33`.
- `src/shared.zig`: the shared-SRAM block at 0x2210_0000.

There is no CMakeLists.txt: `ra8_add_app()` cannot declare a Zig main, so this
app exists only in the Zig build graph. `zig build arm` emits
`threadx_cpu1.elf` (with CPU1's image embedded) and `threadx_cpu1_cpu1.elf`.

Not yet validated on hardware, hence `hw_pending`.
