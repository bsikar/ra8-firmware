# lin_commander_hil

LIN commander frame driver over the SCI_B Simple-LIN sub-mode
(`libs/ra8_hal/inc/ra8_sci_lin.h`) on **SCI2**, whose `TXD2` / `RXD2` are broken out on
the Pmod1 header (J26) as `P801` and `P802`. It emits one LIN frame periodically:
a break field, SYNC (`0x55`) and protected identifier, then a short payload plus
the enhanced (PID-folded) LIN checksum, rotating through a handful of frame ids.

Pmod1 shares pins with the Octo-SPI bus, so the board's SW4 mux must be set to
the Pmod1 side before use.

## Blocked on

LIN is a physical single-wire automotive bus, and `TXD2` / `RXD2` are plain
logic-level UART lines. A real LIN segment needs an external **LIN transceiver**
(NXP TJA1021, Microchip MCP2003 or similar) between those pins and the
single-wire bus, with the bus pull-up to `VBAT`, plus at least one **responder
node** to answer subscribe frames.

Without that partner this app exercises only the commander transmit path; a logic
analyzer on `P801` can confirm the break / SYNC / PID / data / checksum framing at
the UART level. The responder detection path is proven by host unit tests
(`tests/hal/src/test_ra8_sci_lin.c`), not on silicon. Nothing off-target models the
Simple-LIN registers (break-field timer, `XSR0.BFDF` detection) either.

Before a real exchange will work, `k_lin_hil_break_len` (XCR2.BFLW) has to be
tuned to the actual SCICLK so the break is at least 13 bit-times at the bus baud.
