# battery_monitor_demo

Reads battery state-of-charge from a MAX17048-class fuel gauge (7-bit
address `0x36`) through the shared `ra8_fuelgauge` device layer on the IIC_B
bus, on the Pmod1 I2C side (`P511/SDA1` + `P512/SCL1`), and folds each reading
into the `ra8_batt` nag policy.

The app encodes none of the MAX17048 register map: it opens one gauge handle
over the injected Ring-3 I2C seam (bound through `ra8_io_i2c_bus` to the I3C
block's I2C-compat mode) and takes one `ra8_fuelgauge_read` sample per poll,
which the driver decodes from three register reads:

- **SOC** (`0x04`) -- the high byte *is* the integer percent (`soc_pct`).
- **CRATE** (`0x16`) -- decoded by the driver into the `charging` flag, the
  same high-byte sign test the app used to write by hand.

A NAK -- no fuel gauge fitted -- halts on a BKPT before any PASS line, so the
gate cannot pass by accident. `g_bat_soc`, `g_bat_chg`, `g_bat_nag` and
`g_bat_heartbeat` mirror the result for headless probing.

## Low-battery nag

`libs/ra8_batt` raises a one-shot warning on the descent into each band. It is
edge-triggered with hysteresis: a band warns once, stays quiet while the battery
sits in it, and re-arms only after SOC recovers past the threshold by
`k_ra8_batt_rearm_margin` or while charging -- so a steady or jittering low
battery does not spam, and charging suppresses warnings outright. The decision
logic is pure (no MMIO), so it is host-unit-tested with full MC/DC in
`tests/misc/src/test_ra8_batt.c`; this app is the on-target consumer.

## Blocked on

The EK-RA8D2 has no native fuel gauge, so a stock board only ever prints the
NAK banner -- the part has to be fitted before a bench run means anything.
`ra8_emulator` does model the gauge on the modelled IIC_B bus, so the gauge
sample (VCELL / SOC / CRATE, VERSION probe included) returns through the
genuine `ra8_fuelgauge` -> `ra8_io_i2c_bus` -> `ra8_i3c` path with no stub, and
the percent and charge state can be driven off-target.
