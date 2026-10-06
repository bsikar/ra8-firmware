# threadx_fpu_context

This Cortex-M85 ThreadX corpus image keeps two distinct floating-point contexts
live across real PendSV switches. Each equal-priority worker loads unique raw
values into S0-S31, relinquishes to its peer eight times, and checks every
register after it resumes. FPCCR.ASPEN and FPCCR.LSPEN are enabled and each
round verifies CONTROL.FPCA before the switch, so the run exercises ThreadX's
S16-S31 save together with the architecture's lazy S0-S15 frame.

Expected SCI8 console output ends with:

```text
fpctx: thread=A PASS
fpctx: thread=B PASS
fpctx: PASS
```

Either per-thread line may appear first. Any mismatch identifies the thread,
round, and S-register index and prints `FAIL` instead.

Build it with the repository Zig cross graph:

```sh
zig build arm
```

The image is `zig-out/arm/threadx_fpu_context.elf`. It remains in
`hil_needs_revalidation` because the current HIL staging path only accepts
CMake/C application entries; its consumer-visible acceptance run is the
RA8EMU-29 emulator console and RTOS-trace check.
