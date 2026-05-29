# i2c_loopback

IIC_B (I3C-in-I2C-mode) self-test smoke app for the EK-RA8D2.
Brings up `ra_i3c` on channel 0 and probes the on-board
PI4IOE5V6408 I/O expander (U15) at 7-bit address `0x43`. Success
emits `i2c: scan 0x43 ack=1` once a second.

## Why this is `hw_pending`

Multiple iterations against the live HIL board all produce
`i2c: scan ERROR` (the bus times out waiting for `NTST.TDBEF0`
after START never completes). Tested combinations on **2026-05-19**:

| SW4-5 | Chip pins | Pull-up enable | NCODR | MSTPB9 IIC0 | Result |
|-------|-----------|----------------|-------|-------------|--------|
| OFF   | P512/P511 | P109+P311 HIGH | yes   | (pre-fix)   | ERROR  |
| OFF   | P400/P401 | (none)         | yes   | yes         | ERROR  |
| OFF   | P512/P511 | P109+P311 HIGH | yes   | yes         | ERROR  |
| **ON**| P400/P401 | (none)         | yes   | yes         | ERROR  |
| **ON**| P400/P401 | (none, I3CCLK=8MHz)| yes | yes      | ERROR  |

Two **chip-level fixes have already landed** during this debug:

1. **MSTPB9 IIC0 ungate** -- `ra_i3c_init` now ungates both
   MSTPB4 (I3C) AND MSTPB9 (IIC0). Without MSTPB9 the channel-0
   IIC controller stays powered down. Committed in `0d827f17`.
2. **HAL clock semantics flagged** -- `ra_i3c_cfg_t.pclka_hz`
   is used by `internal_i3c_i2c_half_period` to divide
   `STDBR.SBR{LO,HO}`, but HUM Ch 9.10.21 (I3CCLK) says the
   IIC_B controller is actually clocked from **I3CCLK** (MOCO
   ~8 MHz by reset default, configured via I3CCKCR/I3CCKDIVCR
   at SYSC + 0x078 / 0x038), NOT PCLKA (~125 MHz). Passing
   PCLKA clamps STDBR to 0xFF and gives a ~16 kHz bus instead of
   the requested 100 kHz, but switching the demo to 8 MHz did
   not unstick the bus, so this is necessary-but-not-sufficient.

The remaining symptom (no SCL clocking, no TDBEF0, no NACKDF) is
consistent with the IIC controller never seeing a clock OR the
chip+bus state having something other than what the schematic
comment in `ra_board_ek_ra8d2.c` claims about P400/P401 reaching
U15.

## Bench-debug steps needed

What I can't do remotely:

1. Scope SCL/SDA at the EK-RA8D2 IIC test points to see if SCL
   even toggles when `ra_i3c_scan` fires. If not, the bus is
   electrically dead (no pull-ups, no controller clock).
2. From the host Pi, run `i2cdetect -y 1` on Pi I2C-1 wired in
   parallel to the EK-RA8D2 IIC pins to confirm U15 actually
   ACKs `0x43`. If U15 doesn't ACK from the Pi side either, the
   schematic comment in the board library is wrong about U15
   being on this bus.
3. With a J-Link memory probe, dump `STDBR`, `BCTL`, `NTST`,
   `BST`, `RSTCTL`, `PRTS`, `CECTL` and `I3CCKCR`/`I3CCKDIVCR`
   to check (a) STDBR is non-zero, (b) BCTL.BUSE=1, (c) the I3C
   reset bit cleared, (d) PRTS.PRTMD=1, (e) I3CCLK is actually
   running.
4. Try forcing I3CCKCR.I3CCKSEL = PLL1P (gives a much higher
   I3CCLK), recompute STDBR accordingly, and see if the bus
   clocks.

## How to graduate back to `hw_validated/hil/`

1. Identify whichever set of {SW4-5 state, MCU pins, I3CCLK
   source, STDBR value} actually makes U15 ACK at 0x43.
2. Pin the demo + the board lib to that exact configuration.
3. Move the directory back to `examples/ek_ra8d2/hw_validated/hil/`.
