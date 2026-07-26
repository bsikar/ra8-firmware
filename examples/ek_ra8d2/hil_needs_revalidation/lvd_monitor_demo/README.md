# lvd_monitor_demo

Low-Voltage / Programmable Voltage Detection (LVD / PVD) VCC monitor for
the bare EK-RA8D2 EVM. Configures voltage monitor 1 (PVD1) in the safest
flags-only mode and reports the live rail status once a second.

## What it does

Brings up SCI8 + LEDs, then configures **PVD1** once:

- Threshold `Vdetm = 2.80 V` (`k_ra8_lvd_pvdlvl_2_80v`), low enough that a
  healthy 3.3 V rail sits well above it.
- Response = **`k_ra8_lvd_response_none`** -- flags only. **No reset, no
  NMI, no maskable interrupt is ever armed**, so watching a brown-out can
  never reset or brick the board.
- The PVD control registers are write-protected by `PRCR.PRC3`, so the
  demo unlocks that protection group around `ra8_lvd_channel_init` and
  re-locks it immediately after.

Each second it reads `PVD1SR` and prints one of:

```
lvd: pvd1 thr=2.80V mon=above det=0 ok=Y     # healthy: VCC > 2.80 V, no crossing
lvd: pvd1 thr=2.80V mon=below ok=N           # VCC at/under threshold or crossing latched
```

- LED1 toggles on every healthy read; LED2 toggles on a below/again read.
- `g_lvd_ok` / `g_lvd_mon_above` / `g_lvd_det` / `g_lvd_cfg_err` /
  `g_lvd_heartbeat` mirror the result for headless J-Link probing.

No external hardware required.

## Validation

Confirmed on a real EK-RA8D2 (2026-06-28): the analog comparator drives
`PVD1SR.MON` above the 2.80 V threshold on a healthy 3.3 V rail, so the
gate is green (`lvd: pvd1 thr=2.80V mon=above det=0 ok=Y`).

`tools/ra8_emulator` also models the PVD status
(`tools/ra8_emulator/src/periph/board_periph_lvd.c`): `PVD1SR.MON` reads "above
threshold" with `DET` clear -- the steady state of a healthy 3.3 V rail --
so the headless `board_sim_smoke.sh` gate sees the same `mon=above ok=Y`
banner. The configuration path and the status-decode / verdict logic are
also host-tested (`tests/test_app_lvd_monitor_demo.c`).

## Configuration (HUM R01UH1065EJ0130 Rev.1.30, Ch 8 "PVD")

- Monitor channel PVD1 (m-series, has a status register), threshold
  `PVDLVL = 0x09` = 2.80 V (HUM Ch 8.2.2 "PVDmCMPCR" p 303).
- `response = none` keeps `PVDmCR0.RIE` clear -- no reset/IRQ
  (HUM Ch 8.2.4 "PVDmCR0" p 305).
- `PVDmSR.MON` = VCC-above-Vdetm, `PVDmSR.DET` = latched crossing
  (HUM Ch 8.2.7 "PVDmSR" p 307).
- Every PVD register requires `PRCR.PRC3 = 1` before writing; the demo
  writes `0xA508` (key 0xA5 | PRC3) to unlock and `0xA500` to re-lock
  (HUM Ch 8.2.4 p 305 note; PRCR = R_SYSTEM + 0x3FA).

## On-silicon bench plan

1. `make lvd_monitor_demo`, then flash the EK-RA8D2.
2. Open the J-Link OB CDC channel at 115200 8N1; expect
   `lvd: pvd1 thr=2.80V mon=above det=0 ok=Y` once a second, LED1 toggling.
3. Or probe headless over SWD: `g_lvd_ok == 1`, `g_lvd_mon_above == 1`,
   `g_lvd_det == 0`, `g_lvd_cfg_err == 0`, `g_lvd_heartbeat` advancing.
4. Optional live test: with a bench supply, lower VCC toward 2.80 V and
   confirm the banner flips to `mon=below ok=N` and `PVD1SR.DET` latches
   (`g_lvd_det == 1`). Keep VCC within the MCU's absolute limits -- this
   demo never arms an LVD reset, so the board stays up across the dip.
5. Once the gate is green, move the app to `hw_validated/hil/` and switch
   `hil.conf` to the active uart_scrape gate.

Build / flash:

```
make lvd_monitor_demo
make -C examples/ek_ra8d2/hil_needs_revalidation/lvd_monitor_demo flash
```
