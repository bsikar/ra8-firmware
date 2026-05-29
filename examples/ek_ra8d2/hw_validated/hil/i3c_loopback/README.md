# i3c_loopback

I3C (`ra_i3c`) controller bring-up self-test for the EK-RA8D2, run in
I2C-compatibility mode (`mode = i2c`) on channel 0. Proves the I3C
peripheral's controller powers up and clocks a real START / address /
STOP on real silicon -- the I3C counterpart to `i2c_loopback`'s RIIC
test.

## HIL status: validated

Bench-verified on 2026-05-29 -- `i3c: ctrl init ok | bus idle on J27
(bare EVM)` scraped off SCI8.

The I3C peripheral's ch0 bus (P400 SCL0 / P401 SDA0) routes to the J27
header, which has **no on-board device** (U15 is on RIIC ch1 -- see
`i2c_loopback` and #46). On a bare EVM the bus floats (no device, no
pull-ups), so `ra_i3c_scan` cannot ACK -- `ack=0` / "bus idle" is the
EXPECTED result and is not a failure. Reaching the scan loop at all
proves `ra_i3c_init` configured the controller (an init failure
panic-halts first). Attach an I2C device to J27 to upgrade the banner to
`ack=1`.

## Gate

`hil.conf` (uart_scrape): `HIL_EXPECT="i3c: ctrl init ok"`. Only a HAL
init failure or a fault (`hw_init_failed` / `HardFault`) fails the gate.

## See also

`i2c_loopback` -- the RIIC (`ra_i2c`) controller against U15.
