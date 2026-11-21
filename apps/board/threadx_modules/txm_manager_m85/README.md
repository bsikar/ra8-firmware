# txm_manager_m85

The RA8D2's Cortex-M85 (CPU0) runs the ThreadX Module Manager. It loads the
hello-world module (`txm_hello_m33`, RA8FW-430) in place from its own MRAM,
starts it, and reports on the VCOM console once the module's start thread has
run 10 times:

```
txm_manager_m85: module ran 10 times PASS
```

Everything is Zig (RA8FW-795):

- `src/main.zig`: `main`, `tx_application_define` and the Module Manager
  thread (initialize, object pool, in-place load, start), the sequence from
  upstream's `sample_threadx_module_manager.c`. The kernel is
  `threadx_m85_modules` (RA8FW-426, with the RA8FW-481 scheduler and the
  RA8FW-484 MPU budget), swapped in for `threadx` because the app sets
  `CrossApp.txm_module` (RA8FW-796).
- `linker_append.ld`: `.txm_module` in MRAM, 1 KiB aligned, where the packed
  module sits.

The module is the same blob `txm_manager_cpu1` loads on CPU1, built as
`txm_hello_m33` and packed into `.txm_module` by the build graph. There is no
CPU1 image.

There is no CMakeLists.txt: `ra8_add_app()` cannot declare a Zig main, so this
app exists only in the Zig build graph. `zig build arm` emits
`txm_manager_m85.elf`.

Not yet validated on hardware, hence `hw_pending`.
