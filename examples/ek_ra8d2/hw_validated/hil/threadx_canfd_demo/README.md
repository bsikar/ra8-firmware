# threadx_canfd_demo

Two ThreadX threads around CANFD0 in internal-loopback mode (HUM Ch 41,
`CFDCnCTR` p 2710 -- transmitted frames are echoed back inside the chip, so no
transceiver, no bus and no second node are needed): a TX thread sends an
8-byte standard-ID frame on a heartbeat cadence, and an RX poller consumes the
mirrored frame. Each half toggles its own LED.

It exists to show the `ra8_canfd` driver wires up cleanly under the kernel and
that frames really make the round trip. It is deliberately minimal and is not a
CAN stack. Both threads use static stacks (NASA Power of 10 Rule 3), and
`SysTick_Handler` forwards into `_tx_timer_interrupt` for the kernel tick.

LEDs per EK-RA8D2 v1 UM Table 24 p 31.
