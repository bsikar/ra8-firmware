# i2c_loopback

RIIC (`ra8_i2c`) controller self-test for the EK-RA8D2. Brings up RIIC
channel 1 and probes the on-board PI4IOE5V6408 I/O expander (U15) at
7-bit address `0x43`. A successful ACK emits `i2c: scan 0x43 ack=1` once
a second over SCI8, toggling LED1.

## HIL status: validated

Bench-verified on 2026-05-29 -- `i2c: scan 0x43 ack=1` scraped off SCI8.

U15 sits on **RIIC ch1 (P512 SCL1 / P511 SDA1)**, not the I3C bus. An
earlier version of this app drove the I3C peripheral (ch0 / J27), where
U15 is absent -- that is why it used to fail (see issue #46). It now
reuses the board's validated U15 bring-up
(`ra8_board_io_expander_apply_project_sw4_defaults`): bus-recover,
P109/P311 pull-ups, P512/P511 route + NCODR, `ra8_i2c_init(ch1)`, then
loops `ra8_i2c_scan(1, 0x43)`.

## Gate

`hil.conf` (uart_scrape): `HIL_EXPECT="i2c: scan 0x43 ack=1"`. Hardware:
bare EK-RA8D2 v1 -- U15 is on-board, no jumpers required.

## See also

`i3c_loopback` -- the same idea on the I3C peripheral in I2C-compat mode.
