# txm_helium_m85 (module)

`module_start.zig` is a ThreadX module for the RA8D2's Cortex-M85, built
with MVE (RA8FW-820). Its start thread loads its own Q0-Q7 lanes and
VPR.P0, then spins checking them without calling the kernel, so every time
it runs again it was preempted and resumed by the scheduler. A wrong value
stops it (it sleeps forever), which freezes its run count.

The manager image that loads it and holds a different pattern in its own
kernel thread is RA8FW-821. Not yet validated on hardware, hence
`hw_pending`.
