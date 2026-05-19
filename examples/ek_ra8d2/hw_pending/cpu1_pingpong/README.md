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

### 2026-05-19 follow-up

Re-probed under the canfd-fix sweep:

- g_cpu1_pingpong_mismatch DOES advance (~1 per 1.5 s), not stuck
  at 0. The earlier "both zero" reading was a stale snapshot.
- IPC channel 2 (CPU0 -> CPU1, addr 0x40020100) STA reads
  0x00030000 (FULL bit 16 + FERR bit 17). CPU0 successfully wrote
  0x1234 to TXD (visible in JTAG dump) but CPU1 never drains.
- IPC channel 0 (CPU1 -> CPU0, addr 0x400200C0) STA reads
  0x01000000 -- some upper status bit, no RDY (bit 0).
- IPCSAR @ 0x40008610 = 0x00000000. Per
  ra8d2_ipc_regs.h:267, bit 18 (SAIPCIR2) clear = "channel 2 is
  secure-only". If CPU1's M33 boots non-secure (default for the
  secondary core), it cannot read channel 2's RXD even though
  the FIFO has 0x1234 waiting.

CPU0 is non-secure-tagged (per main.c `{World: NS}`) yet writes
to channel 2 succeed -- suggesting in the RA_TRUSTZONE_ENABLE=OFF
build the SAU stays in reset and the IDAU rule (bit 28 of addr)
governs: 0x40020100 has bit 28 clear, so IDAU says SECURE, but
with SAU disabled the CPU's current security state determines
access. CPU0 (M85) and CPU1 (M33) likely boot with DIFFERENT
default security states, which is the missing piece.

## How to graduate back

1. Write IPCSAR=0x000F0303 (clear-secure for all IPC channels +
   NMIs + semaphores) before calling ra_cpu1_release, granting
   non-secure access to whichever world CPU1 boots in.
2. Reflash + JTAG-probe; if channel 2 STA.FULL drains and channel
   0 STA.RDY asserts, the security attribution was the blocker
   and the per-side ra_ipc_init_attribution / matching IPCPAR
   bits go into ra_dual_core.c so any dual-core app gets them
   for free.
3. Once g_cpu1_pingpong_match advances >=5 per 5 s window with 0
   mismatch, promote back to hw_validated/hil/.
