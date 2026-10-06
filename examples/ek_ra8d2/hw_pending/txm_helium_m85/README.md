# txm_helium_m85

The RA8D2's Cortex-M85 (CPU0) runs the ThreadX Module Manager and starts
`txm_helium_m85`, a module built for the M85 with MVE (RA8FW-820). It shows
that a switch between a kernel thread and a module thread keeps each
side's Helium state (RA8FW-428):

```
txm_helium_m85: Q0-Q7 and VPR kept across 10 switches PASS
```

- `module_start.zig`: the module's start thread (priority 1). It loads its
  own S0-S31 (Q0-Q7) lanes and VPR.P0, then spins checking them without
  calling the kernel, so every time it runs again it was preempted and
  resumed through the scheduler's exception return. A wrong value stops it
  (it sleeps forever), which freezes its run count.
- `src/main.zig`: the manager (RA8FW-821), at priority 0 so it preempts the
  module every tick. Each wake it loads a different pattern into every
  lane and P0, sleeps a tick in the same asm block, and checks S16-S31
  (Q4-Q7, the callee-saved half; Q0-Q3 and VPR are caller-saved, so only
  the module can hold them across a switch). PASS needs 10 wakes in a row
  with its own lanes intact and the module's run count rising each time.
  A failure prints `FAIL load`, `FAIL kernel Q4-Q7` or `FAIL module
  stopped`.
- `linker_append.ld`: `.txm_module` in MRAM, where the packed module sits.

There is no CMakeLists.txt: the app exists only in the Zig build graph.
`zig build arm` emits `txm_helium_m85.elf`. Not yet validated on hardware,
hence `hw_pending`.
