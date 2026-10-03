# txm_fault_cpu1

The negative case for CPU1's ThreadX Module Manager (RA8FW-459, under
RA8FW-290). CPU1, the RA8D2's Cortex-M33, loads `txm_fault_m33` in place and
starts it. The module's start thread stores one word outside every MPU region
the manager gave it. That store must take MemManage; the port's handler
terminates the module thread and calls the fault callback this app registers,
and the kernel carries on. The Cortex-M85 reports on the VCOM console once the
fault is in, names this app's module, and the manager has ticked 10 more times:

```
txm_fault_cpu1: module faulted, manager ran 10 more ticks PASS
```

Everything is Zig:

- `src/main.zig`: the M85 application.
- `src/cpu1_main.zig`: CPU1's `tx_application_define`, the Module Manager
  thread (initialize, object pool, fault notify, in-place load, start) and the
  fault callback. Glue and kernel as in txm_manager_cpu1 (`threadx_cpu1`,
  `threadx_m33_modules`).
- `src/shared.zig`: the shared-SRAM block at 0x2210_0000, with the fault count,
  CFSR, MMFAR and the ticks since the fault.
- `linker_script_cpu1.ld`: the board's M33 map plus `.txm_module`.

The module is `examples/ek_ra8d2/hw_pending/txm_fault_m33/module_start.zig`. Its
store goes to 0x2210_0040, past the shared block, so a store an absent MPU lets
through cannot fake the verdict: the module just keeps sleeping and the M85
times out on FAIL.

There is no CMakeLists.txt; `zig build arm` emits `txm_fault_cpu1.elf` and
`txm_fault_cpu1_cpu1.elf`. Not yet validated on hardware, hence `hw_pending`.
