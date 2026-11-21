# txm_table_cpu1

CPU1, the RA8D2's Cortex-M33, runs the ThreadX Module Manager and loads a
module that keeps addresses in its initialised data: a constant table of two
function pointers (`txm_table_m33`, RA8FW-539). The Cortex-M85 releases CPU1
and reports on the VCOM console once the module has reported ten values, each
one the value expected:

```
txm_table_cpu1: table returned 10 100 200 40000 PASS
```

The module starts a counter at five, doubles it on even steps and squares it
on odd ones, choosing the function from the table each time. It sends each
result through a ThreadX queue it created, receives it back, and reports it
to the resident image as an application request. Those four are its first
four results.

The table is the point. A module is loaded wherever the manager finds room,
and upstream's start-up rebases its GOT and copies its data byte for byte, so
the table's two words would still hold link-time addresses. This module is
built by the route in `tests/zig_build_graph/txm_module_object.zig`: its Zig
goes through C and gcc with the module flags, and its start-up rebases every
data word the module's rebase table names. A call through an address that was
not rebased would not come back with these values.

- `src/main.zig`: the M85 application.
- `src/cpu1_main.zig`: CPU1's `tx_application_define`, the Module Manager
  thread, and `_txm_module_manager_application_request`, which works each
  value out for itself and records whether the module's matched. The vector
  table, reset path and SysTick retune come from the `threadx_cpu1` glue; the
  kernel is `threadx_m33_modules`.
- `src/shared.zig`: the shared-SRAM block at 0x2210_0000. It records the
  first manager step that failed, how many values were reported, how many
  were wrong, and the first four.
- `linker_script_cpu1.ld`: the board's M33 map plus `.txm_module`, where the
  packed module sits (`Cpu1Image.txm_module`).

The module is `tests/zig_build_graph/txm_module_probes/table.zig`, built as
`txm_table_m33` and packed into `.txm_module` by the build graph. It is the
same source `zig build txm-module-check` holds to the relocation check.

There is no CMakeLists.txt: `ra8_add_app()` cannot declare a Zig main, so this
app exists only in the Zig build graph. `zig build arm` emits
`txm_table_cpu1.elf` (with CPU1's image embedded) and
`txm_table_cpu1_cpu1.elf`.

Not yet validated on hardware, hence `hw_pending`.
