# i2c_i3c_combined

Brings up both of the RA8D2's I2C-capable controllers in one image and proves
they run side by side: RIIC channel 1 against the on-board U15 expander at
7-bit `0x43`, and the I3C block on channel 0 in I2C-compatibility mode out to
the J27 header.

The two halves are asymmetric on purpose. RIIC gets a real controller-to-device
round trip, because U15 is on that bus and ACKs. J27 carries no on-board
device, so the I3C half is a controller bring-up check only -- reaching the
scan at all is the evidence, since an init failure panic-halts before the loop.

`i2c_loopback` and `i3c_loopback` cover those two buses one at a time. This app
exists to show that neither peripheral's bring-up disturbs the other.
