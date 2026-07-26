# pdg_delay_demo

PWM Delay Generation Circuit (PDG) bring-up + delay-program demo for the
bare EK-RA8D2 EVM. Exercises the `ra8_pdg` driver.

## What it does

Brings up SCI8 + LEDs + the PDG DLL, then:

1. `ra8_pdg_init` -- selects the 80..160 MHz FRANGE band, enables the DLL,
   and un-bypasses PDG channel 0.
2. `ra8_pdg_set_delay` -- stages a mid-range delay code (`0x40`) on the
   GTIOC0A rising edge (DLY[6:0], ~1/128 of the GPT core-clock period per
   step).
3. Reads the code back and the PDG status, and once a second reports
   `pdg: dll=on ch0=on delay=0x40 cfg=ok` on the J-Link OB CDC channel.

- LED1 toggles while the configuration reads back clean; LED2 otherwise.
- `g_pdg_cfg_ok` / `g_pdg_delay_readback` / `g_pdg_dll` /
  `g_pdg_heartbeat` mirror the result for headless J-Link probing.

No external hardware required for the bring-up.

## Validation

Validated on a real EK-RA8D2 (2026-06-28): the PDG DLL locks and channel 0
reads back powered and un-bypassed, so the gate is green
(`pdg: dll=on ch0=on delay=0x40 cfg=ok`).

A silicon finding fixed here: the delay code register `GTDLYRnA` is **not
read-exposed on this silicon** -- a write stages the delay, but the register
returns its `0x0000` reset value to both firmware and a J-Link debugger (FSP
never reads it back either). `tools/board_sim` shadows `GTDLYRnA` as plain
R/W, which is why the read-back appeared to work on the emulator. The verdict
therefore gates on the software-observable bring-up (DLL + channel power +
un-bypass), not on a delay read-back.

The PDG has **no software-readable "edge was delayed" status** -- its actual
effect, the *timing* of a GPT output edge, still needs a logic analyzer /
oscilloscope and a running GPT32_0 PWM source to measure (the bench plan
below). That edge-shift measurement is bench/instrument-dependent and is not
part of the headless gate.

## Registers (HUM R01UH1065EJ0130 Rev.1.30, Ch 23 "PDG")

- `ra8_pdg_init` clears MSTPD6, programs GTDLYCR (DLLEN + FRANGE) and
  GTDLYCR2 (per-channel bypass), per HUM Figure 23.2 p 1160, Ch 23.2.1
  p 1154 / 23.2.2 p 1155.
- `ra8_pdg_set_delay` writes the GTDLYRnA / GTDLYRnB temporary register;
  it propagates to the live delay on the next GPT overflow / underflow /
  trough (HUM Ch 23.3.2 Figure 23.3 p 1161).

## On-silicon bench plan

1. `make pdg_delay_demo`, then flash the EK-RA8D2.
2. Confirm the headless config gate first: J-Link CDC prints
   `pdg: dll=on ch0=on delay=0x40 cfg=ok` (or probe `g_pdg_cfg_ok == 1`).
   Note: `g_pdg_delay_readback` reads `0` on silicon (`GTDLYRnA` is not
   read-exposed; it reads `0x40` only on `tools/board_sim`).
3. **Delay measurement (the real acceptance, needs an instrument):**
   - Bring up GPT32_0 as a PWM source on GTIOC0A (e.g. mirror
     `gpt_dma_demo`), with the PDG bound so the staged delay propagates.
   - Probe GTIOC0A on a scope / logic analyzer; sweep the delay code
     0x00 -> 0x40 -> 0x7F and confirm the rising edge shifts by the
     expected `code x (T_gptclk / 128)`.
4. Once the edge shift is confirmed, move the app to `hw_validated/hil/`.

Build / flash:

```
make pdg_delay_demo
make -C examples/ek_ra8d2/hil_needs_revalidation/pdg_delay_demo flash
```
