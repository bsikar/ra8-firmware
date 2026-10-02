# txm_manager_cpu1

CPU1, the RA8D2's Cortex-M33, runs the ThreadX Module Manager. It loads the
hello-world module (`txm_hello_m33`, RA8FW-430) in place from its own MRAM and
starts it. The Cortex-M85 releases CPU1 and reports on the VCOM console once
the module's start thread has run 10 times:

```
txm_manager_cpu1: module ran 10 times PASS
```

Everything is Zig (RA8FW-431):

- `src/main.zig`: the M85 application.
- `src/cpu1_main.zig`: CPU1's `tx_application_define` and the Module Manager
  thread (initialize, object pool, in-place load, start), the sequence from
  upstream's `sample_threadx_module_manager.c`. The vector table, reset path
  and SysTick retune come from the `threadx_cpu1` glue; the kernel is
  `threadx_m33_modules`.
- `src/shared.zig`: the shared-SRAM block at 0x2210_0000. It records the
  first manager step that failed and the module's run count.
- `linker_script_cpu1.ld`: the board's M33 map plus `.txm_module`, where the
  packed module sits (`Cpu1Image.txm_module`).

The module is `examples/ek_ra8d2/hw_pending/txm_hello_m33/module_start.zig`,
built as `txm_hello_m33` and packed into `.txm_module` by the build graph.

There is no CMakeLists.txt: `ra8_add_app()` cannot declare a Zig main, so this
app exists only in the Zig build graph. `zig build arm` emits
`txm_manager_cpu1.elf` (with CPU1's image embedded) and
`txm_manager_cpu1_cpu1.elf`.

Not yet validated on hardware, hence `hw_pending`.
