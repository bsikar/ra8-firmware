# canfd_loopback

Puts CANFD0 into Self-test 1 internal loopback (`CFDC[0].CTR.CTME = 1`,
`CTMS = 11b`) at 500 kbps and round-trips an 8-byte frame at standard ID 0x123
once a second. LED1 toggles on a completed round-trip, LED2 on a TX or RX
failure.

No transceiver, shield or harness is needed -- the frame never leaves the chip.
That is the point: it proves the CAN-FD controller, its bit timing and its RX
FIFO without a bus, so a failure here is never the wiring.
`canfd_filter_demo` adds acceptance filtering on top of the same loopback.
