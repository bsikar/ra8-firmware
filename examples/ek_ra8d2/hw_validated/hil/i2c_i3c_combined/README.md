# i2c_i3c_combined

RIIC + I3C coexistence self-test for the EK-RA8D2. Brings up **both**
I2C-capable controllers in one firmware and proves they run side by side:

- `ra_i2c` (RIIC) ch1 -> on-board U15 expander at `0x43` -- a real
  controller<->device round-trip (U15 ACKs).
- `ra_i3c` (I3C) ch0 in I2C-compatibility mode -> J27 header -- a
  controller bring-up self-test (no on-board device, so no ACK).

Each second SCI8 emits a single combined banner.

## HIL status: validated

`hil.conf` (uart_scrape): `HIL_EXPECT="combo: i2c ack=1 i3c initok"` --
emitted only when RIIC ACKs U15 **and** the I3C controller initialised
(an init failure panic-halts before the loop). A RIIC bus error or a
fault fails the gate.

## See also

- `i2c_loopback` -- RIIC (`ra_i2c`) alone against U15.
- `i3c_loopback` -- I3C (`ra_i3c`, i2c-mode) controller alone.
