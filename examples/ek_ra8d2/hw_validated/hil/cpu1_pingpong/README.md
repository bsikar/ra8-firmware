# cpu1_pingpong (hw_pending)

Dual-core ping-pong: CPU0 (M85) sends a 0x1234 IPC ping to CPU1
(M33), CPU1 replies with 0x4321 pong. g_cpu1_pingpong_match
increments on each successful round-trip.

> **Memory-map correction (re-validate on bench):** the CPU1 SRAM origin
> (`SRAM_CPU1`) was moved from a **reserved** address (`0x223F0000`, above the
> 1.6 MB on-chip ECC SRAM that ends at `0x221A0000`) to `0x22190000`, the top
> 64 KiB of physical SRAM. ra8_emulator masked the original error with a wide SRAM
> window, so the JTAG bench results below predate this fix and must be
> re-confirmed on hardware.

## Status

JTAG probing on 2026-05-19 confirmed:

- ra8_cpu1_release returns k_ra8_ok.
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
  ra8_ipc_regs.h (SAIPCIR2), bit 18 clear = "channel 2 is
  secure-only". If CPU1's M33 boots non-secure (default for the
  secondary core), it cannot read channel 2's RXD even though
  the FIFO has 0x1234 waiting.

CPU0 is non-secure-tagged (per main.c `{World: NS}`) yet writes
to channel 2 succeed -- suggesting in the RA8_TRUSTZONE_ENABLE=OFF
build the SAU stays in reset and the IDAU rule (bit 28 of addr)
governs: 0x40020100 has bit 28 clear, so IDAU says SECURE, but
with SAU disabled the CPU's current security state determines
access. CPU0 (M85) and CPU1 (M33) likely boot with DIFFERENT
default security states, which is the missing piece.

## How to graduate back

### 2026-05-19 attempt: IPCSAR write from main, dropped

Tried `*ra8_ipc_ipcsar() = 0x000F0303` immediately after entry to
`main()`, before `ra8_cpu1_release`. JTAG post-flash readback
showed:

- IPCSAR @ 0x40008610 still 0x00000000 (write silently dropped)
- IPCPAR @ 0x40008614 = 0x000F0303 (write took)

Both writes used the same secure-alias address pattern and
adjacent registers in the CPSCU window. The asymmetry says
**IPCSAR is secure-only-writable** and **IPCPAR is not**. With
`RA8_TRUSTZONE_ENABLE=OFF` the SAU stays in reset and CPU0's
current security state determines write access -- and on this
chip CPU0 evidently runs non-secure once the boot ROM hands off,
because the IPCSAR write is dropped just like an SAU-blocked
access would be (no SecureFault is taken because no SAU rule
covers the address).

The fix path is therefore deeper than a single MMIO write:

1. Enable RA8_TRUSTZONE_ENABLE for this app and put the SAU in
   "everything secure" mode so the secure-alias write goes
   through, then drop CPU1 into the secure world to match. The
   trustzone_init.c scaffold under `RA8_TRUSTZONE_ENABLE` already
   programs four NS regions -- it needs to either flip to
   default-secure or carve out a secure subregion that covers
   0x40008000 (CPSCU) plus the IPC peripheral.
2. Once IPCSAR sticks, channel 2 should drain on the next ping
   and `g_cpu1_pingpong_match` will start advancing.
3. Promote back to hw_validated/hil/ once the probe is steady.

Skipped this turn -- requires non-trivial TrustZone bring-up
work that overlaps the larger TZ scaffold rewrite tracked
elsewhere.

### 2026-05-19 (later): HUM architectural constraint pinned

Read HUM Ch 2 deeper. **CPU1 on this chip is wired with
SECEXT (Security Extension) disabled** -- it can ONLY run
non-secure. So the cpu1_pingpong TZ design is fixed:

- CPU0 (M85) is Secure side.
- CPU1 (M33) is Non-Secure side.

### 2026-05-20: SAIPCIRn semantics pinned -- the demo design itself is incompatible

Confirmed from HUM Ch 3.2.1 IPCSAR register description that
SAIPCIRn is **mutually exclusive**:

- 0: Secure -- only Secure code can access the channel's
  register file (TXD, RXD, STA, ISET, CLR).
- 1: Non-secure -- only Non-secure code can access them.

So **a single IPC channel cannot bridge CPU0(S) and CPU1(NS)**
because whichever attribution wins, the other CPU is denied.
The cpu1_pingpong ping/pong topology (CPU0 writes ch2.TXD, CPU1
reads ch2.RXD, CPU1 writes ch0.TXD, CPU0 reads ch0.RXD) is
**architecturally incompatible with the SECEXT-disabled CPU1**.

## How to graduate back

The demo needs a redesign:

1. **NSC veneer wrapper** -- CPU0 exposes secure-side TX/RX
   functions as NSC veneers, CPU1 calls those instead of doing
   raw MMIO. Channels stay S-only.
2. **Shared-NS-SRAM payload + IPCSEMn semaphore** -- payload
   sits in a shared NS-SRAM region with appropriate cache
   fences; the IPC layer just signals readiness via
   IPCSEMn (HUM Ch 3.2.3, has its own SAIPCSEMg attribution
   that may allow asymmetric S+NS access).
3. **NMI-only pingpong** -- use IPC0NMI / IPC1NMI for one-shot
   wake signals, no payload through IPC at all.

Once any of these designs lands and CPU1 actually drains the
channel-2 FIFO, the existing memprobe + match/mismatch counters
will validate the round-trip. Move back to hw_validated/hil/
once g_cpu1_pingpong_match advances at the configured rate.
