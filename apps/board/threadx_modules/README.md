# ThreadX module apps

The ThreadX module system's apps for the RA8 boards. The module apps are
packaged as `.ra8app` (identity, capability set, entry, stack) and built
through the cross image steps in `build.zig`; the manager and loader apps
demonstrate the load path.

Module apps:

- `txm_hello_m33/`: the hello-world module for CPU1 (RA8FW-430, under
  RA8FW-290), the first module image a CPU1 Module Manager can load.
- `txm_fault_m33/`: the negative case, a module whose start thread stores
  outside its MPU regions (RA8FW-459).
- `txm_table_m33/`: source under `tests/zig_build_graph/txm_module_probes/`,
  the module that keeps a function-pointer table in its data (RA8FW-539).
- `txm_rpc_m33/`: entry under `txm_rpc_cpu1/src/module_start.zig`, the
  module that calls a resident-image service through ra8_rpc (RA8FW-544).
- `txm_tour_m33/`: the syscall-tour module (RA8FW-1023, under RA8FW-289).
- `txm_helium_m85/`: the M85 module with MVE that checks Q0-Q7 and VPR
  across preemption (RA8FW-820, under RA8FW-428).
- `txm_dual_mailbox/`: the mailbox pair, its M85 half and CPU1 halves
  (RA8FW-843, RA8FW-844 and RA8FW-842, under RA8FW-428).
- `txm_translate_m33/`: the translation module's shell for CPU1, packaged
  with display and storage and no network (RA8FW-680). Its manager example
  lives in `examples/ra8p1_foundation/txm_manager_ra8p1`.

Manager and loader apps:

- `txm_manager_cpu1/`: the CPU1 Module Manager sample, in-place load of
  `txm_hello_m33` from MRAM (RA8FW-458).
- `txm_fault_cpu1/`: the CPU1 manager's fault path on `txm_fault_m33`
  (RA8FW-459).
- `txm_table_cpu1/`: the CPU1 manager's table module run (RA8FW-539).
- `txm_rpc_cpu1/`: the CPU1 manager's rpc module run (RA8FW-544).
- `txm_reload_cpu1/`: the CPU1 manager's reload round trip on
  `txm_hello_m33` (RA8FW-850).
- `txm_manager_m85/`: the M85 Module Manager sample, in-place load from its
  own MRAM (RA8FW-821, under RA8FW-428).
- `txm_fault_m85/`: the M85 manager's fault path.
- `txm_sd_hello_m85/`: SD load and run of `txm_hello_m33` and
  `txm_fault_m33` from a micro-SD card (RA8FW-290).
- `txm_sd_tour_m85/`: SD load and run of `txm_tour_m33`, admitted through
  ra8_app's gate (RA8FW-965, under RA8FW-290).

Setup examples for the `.ra8app` build and load path land here too as
they move over from `examples/`.
