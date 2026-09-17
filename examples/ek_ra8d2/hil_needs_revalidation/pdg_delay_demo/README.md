# pdg_delay_demo

PWM Delay Generation Circuit (PDG) bring-up: selects the 80-160 MHz FRANGE band,
enables the DLL, un-bypasses PDG channel 0, and stages a mid-range delay code on
the GTIOC0A rising edge (DLY[6:0], roughly 1/128 of the GPT core-clock period
per step). `g_pdg_cfg_ok` / `g_pdg_delay_readback` / `g_pdg_dll` /
`g_pdg_heartbeat` mirror the result for a headless J-Link probe.

## The delay register is not read-exposed on this silicon

`GTDLYRnA` accepts a write that stages the delay, but reads back its `0x0000`
reset value -- to firmware and to a J-Link debugger alike. FSP never reads it
back either. Anything that shadows the register as plain read/write will appear
to confirm a read-back that the chip does not actually provide, so the verdict
here gates on the software-observable bring-up (DLL locked, channel powered and
un-bypassed) rather than on the staged code.

That leaves the PDG with **no software-readable "the edge was delayed" status at
all**. Its actual effect is the timing of a GPT output edge, which needs a
running GPT32_0 PWM source on GTIOC0A and a scope or logic analyzer to observe:
sweep the delay code across its range and confirm the rising edge shifts by
roughly `code x (T_gptclk / 128)`. That measurement is instrument-dependent and
cannot be part of a headless gate, which is why this app is hard to keep green
automatically.

## Registers (HUM R01UH1065EJ0130 Rev.1.30, Ch 23 "PDG")

- `ra8_pdg_init` clears MSTPD6 and programs GTDLYCR (DLLEN + FRANGE) and
  GTDLYCR2 (per-channel bypass) -- Figure 23.2 p 1160, Ch 23.2.1 p 1154,
  Ch 23.2.2 p 1155.
- `ra8_pdg_set_delay` writes the GTDLYRnA / GTDLYRnB temporary register; it
  propagates to the live delay on the next GPT overflow, underflow or trough
  (Ch 23.3.2 Figure 23.3 p 1161).

No external hardware required for the bring-up half.
