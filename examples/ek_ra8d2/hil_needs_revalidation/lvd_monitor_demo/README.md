# lvd_monitor_demo

Watches the VCC rail through voltage monitor 1 (PVD1) in the safest possible
configuration and reports live status once a second. `g_lvd_ok` /
`g_lvd_mon_above` / `g_lvd_det` / `g_lvd_cfg_err` / `g_lvd_heartbeat` mirror the
result for a headless J-Link probe; LED1 toggles on a healthy read and LED2 on a
below-threshold one.

The safety property is the design: the response is `k_ra8_lvd_response_none`, so
no reset, no NMI and no maskable interrupt is ever armed. Watching a brown-out
therefore cannot reset or brick the board, which means you can lower VCC toward
the 2.80 V threshold with a bench supply and watch `PVDmSR.DET` latch the
crossing while the board stays up. The threshold is chosen low enough that a
healthy 3.3 V rail sits well above it.

The PVD control registers are write-protected by `PRCR.PRC3`, so the demo
unlocks that protection group around channel init and re-locks it immediately
after. Forgetting either half is the usual reason a PVD configuration appears to
have been accepted and then reads back as its reset value.

## Configuration (HUM R01UH1065EJ0130 Rev.1.30, Ch 8 "PVD")

- Monitor channel PVD1 (an m-series channel, so it has a status register),
  `PVDLVL = 0x09` = 2.80 V (Ch 8.2.2 "PVDmCMPCR" p 303).
- A `none` response keeps `PVDmCR0.RIE` clear, so no reset or IRQ (Ch 8.2.4
  "PVDmCR0" p 305).
- `PVDmSR.MON` is VCC-above-Vdetm, `PVDmSR.DET` is the latched crossing
  (Ch 8.2.7 "PVDmSR" p 307).
- Every PVD register write needs `PRCR.PRC3 = 1` first; the demo writes `0xA508`
  to unlock and `0xA500` to re-lock (Ch 8.2.4 p 305 note; PRCR is
  R_SYSTEM + 0x3FA).

No external hardware required for the steady-state read.
