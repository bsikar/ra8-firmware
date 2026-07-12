# canfd_loopback

CANFD0 internal-loopback HIL test for the bare EK-RA8D2 EVM.

Brings up CANFD0 at 500 kbps, forces the channel into Self-test 1 /
internal-loopback mode (`CFDC[0].CTR.CTME=1, CTMS=11b`) via
`ra8_canfd_set_test_mode`, and once a second transmits an 8-byte
heartbeat frame at standard ID `0x123`, polling the RX FIFO for the
mirrored frame.

- LED1 toggles on every successful TX/RX round-trip.
- LED2 toggles on TX or RX failure.

No external transceiver, board, or harness is required -- the demo
runs against the on-chip CANFD IP only.

Build / flash:

```
make canfd_loopback
make -C examples/ek_ra8d2/canfd_loopback flash
```
