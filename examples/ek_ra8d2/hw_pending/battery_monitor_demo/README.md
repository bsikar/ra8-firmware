# battery_monitor_demo

Reads battery state-of-charge from a MAX17048-class I2C fuel gauge (7-bit
address `0x36`) through `ra8_smbus` on the IIC_B bus, on the Pmod1 I2C side
(`P511/SDA1` + `P512/SCL1`), and folds each reading into the `ra8_batt` nag
policy.

The gauge is a plain register-indexed I2C device, so it answers the SMBus byte
protocols natively. Two registers carry everything the app needs, each in a
single byte read:

- **SOC** (`0x04`) -- the high byte *is* the integer percent.
- **CRATE** (`0x16`) -- the high byte's sign bit is `0` while charging and `1`
  while discharging, so one byte gives the charge direction.

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
`ra8_emulator` does model the gauge on the modelled IIC_B bus, so the SOC and
CRATE reads return through the genuine `ra8_smbus` -> `ra8_i3c` path with no
stub, and the percent and charge state can be driven off-target.
