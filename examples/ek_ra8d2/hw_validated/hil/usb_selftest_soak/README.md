# usb_selftest_soak

The endurance and benchmark member of the USB self-loop matrix. Same dual-stack
image as `usb_selftest_hs_host` -- HS host on J7, FS MSC device on J11, the loop
cable between them, no PC involved -- but the host worker repeats the 1 MiB MRAM
integrity sweep back to back instead of once.

Every `READ(10)` burst is still memcmp'd against the real MRAM window, so a
single corrupted or dropped transfer anywhere in the soak fails the run. Volume
and elapsed time are summed across iterations for a throughput figure that is
stable enough to compare between builds.

The read-only `WRITE(10)` rejection is confirmed **once at the end**: the write
STALLs the bulk-OUT pipe, so it has to run last (#92).

`hs_host` proves one clean pass; this proves the transport stays byte-perfect
under sustained load. `k_selftest_soak_iters` sets the soak length -- bump it
for a longer endurance run.

Pinout and VID/PID are identical to `usb_selftest_hs_host`. Bench use only.
