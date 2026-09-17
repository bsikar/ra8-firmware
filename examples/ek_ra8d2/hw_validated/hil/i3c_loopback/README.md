# i3c_loopback

I3C controller bring-up self-test, run in I2C-compatibility mode on channel 0.
Proves the I3C block's controller powers up and clocks a real START / address /
STOP on silicon -- the counterpart to `i2c_loopback`'s RIIC test.

Channel 0 (P400 SCL0 / P401 SDA0) routes to the J27 header, which carries no
on-board device: U15 is on RIIC ch1 (#46). On a bare EVM the bus floats with
neither a target nor pull-ups, so the scan cannot ACK, and a no-ACK result is
EXPECTED here rather than a failure. What the run actually proves is that the
scan loop was reached, since an init failure panic-halts first. Attach an I2C
device to J27 and the same firmware starts reporting an ACK.
