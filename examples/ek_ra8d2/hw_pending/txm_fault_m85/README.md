# txm_fault_m85

The negative Module Manager case on the RA8D2's Cortex-M85 (CPU0). The manager
loads `txm_fault_m33` (RA8FW-459) in place from its own MRAM and starts it. That
module's start thread stores outside its MPU regions, so it takes MemManage. The
port's handler terminates only the module thread and calls the manager's fault
callback; the manager then ticks on and reports:

```
txm_fault_m85: module faulted, manager ran 10 more ticks PASS
```

It is `txm_fault_cpu1`'s check with the manager on CPU0 instead of CPU1
(RA8FW-805), built the same way as `txm_manager_m85`:

- `src/main.zig`: `main`, `tx_application_define`, the Module Manager thread
  (initialize, object pool, fault notify, in-place load, start) and the fault
  callback. The kernel is `threadx_m85_modules`, swapped in for `threadx`
  because the app sets `CrossApp.txm_module` (RA8FW-796).
- `linker_append.ld`: `.txm_module` in MRAM, 1 KiB aligned, where the packed
  module sits.

There is no CMakeLists.txt; the app exists only in the Zig build graph, and
`zig build arm` emits `txm_fault_m85.elf`.
