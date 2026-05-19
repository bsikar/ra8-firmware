# cpu1_pingpong (hw_pending)

Dual-core ping-pong: CPU0 (M85) sends a 0x1234 IPC ping to CPU1
(M33), CPU1 replies with 0x4321 pong. g_cpu1_pingpong_match
increments on each successful round-trip.

## Status

JTAG probing on 2026-05-19 confirmed:

- ra_cpu1_release returns k_ra_ok.
- CPU1INITVTOR = 0x020C0000 (correct MRAM_CPU1 base).
- CPU1WAITCR = 0x00, CPU1ACTCSR = 0x00000080 (ACT bit latched).
- g_cpu1_pingpong_step = 4 (CPU0 reached the main while loop).
- g_cpu1_pingpong_release_err = 0.

But g_cpu1_pingpong_match stays at 0 with g_cpu1_pingpong_mismatch
also at 0. recv_blocking has a 1M-iteration bounded poll
(~1 ms at 1 GHz) so a 5 s window should produce thousands of
timeouts. Both counters stay zero, which means CPU0 is blocking
inside ra_ipc_recv_message and the bounded poll never fires.

Most likely: a peripheral-register read inside ra_ipc_recv_message
stalls (waiting for some IPC peripheral status bit that never
flips) instead of returning no_data when the FIFO is empty. The
RA8D2 IPC peripheral has secure/non-secure attribution via
IPCSAR/IPCPAR (at 0x40008000) -- if those are mis-programmed,
reads from the wrong-world alias might never complete.

## How to graduate back

1. Dump IPCSAR / IPCPAR / IPC FIFO registers (HUM Ch 3.2.x) via
   JTAG and confirm the IPC channels CPU0 + CPU1 use are visible
   from the world the firmware runs in.
2. Bound ra_ipc_recv_message's internal polling with the same
   pattern other RA HALs use (k_ra_*_spin), so the call returns
   no_data instead of stalling on a never-arriving bit.
3. Re-run; once g_cpu1_pingpong_match advances >=5 in 5 s, move
   the dir back to hw_validated/hil/.

The dual-core release path itself (ra_cpu1_release) is verified
working by the JTAG dump, so step 2 is the only firmware change
needed.
