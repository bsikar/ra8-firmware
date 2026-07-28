# lin_commander_hil

LIN (Local Interconnect Network) **commander** frame-driver demo for the
EK-RA8D2. Drives the SCI_B Simple-LIN sub-mode (`libs/ra8_hal/ra8_sci_lin`) on
**SCI2**, whose `TXD2` / `RXD2` pins are broken out on the **Pmod1** header
(J26): `P801` = TXD2, `P802` = RXD2.

Once every 500 ms the app emits one LIN frame:

1. **Header** -- break field + SYNC (`0x55`) + protected identifier (PID),
   rotating through frame ids `{0x01, 0x02, 0x11, 0x3C}`.
2. **Response** -- a two-byte payload `{0xA5, 0x5A}` plus the enhanced
   (PID-folded) LIN checksum, published by the commander.

Progress is printed on the J-Link OB console (SCI8 @ 115200 8N1) and LED1
blinks in lock-step.

## Status: hw_pending (NOT yet validated on silicon)

LIN is a physical single-wire automotive bus. The SCI's `TXD2` / `RXD2` are
plain logic-level UART lines, so a real LIN segment needs:

- an external **LIN transceiver** (e.g. NXP TJA1021, Microchip MCP2003)
  between `P801` / `P802` and the single-wire LIN bus (with the bus pull-up
  to `VBAT`), and
- at least one **responder node** to answer subscribe frames.

Without that partner this app exercises only the **commander transmit path**.
A logic analyzer on `P801` (TXD2) can confirm the break / SYNC / PID / data /
checksum framing at the UART level. The **responder detection path**
(`ra8_sci_lin_wait_break` -> `ra8_sci_lin_read_response` -> checksum verify) is
proven by the host unit tests (`tests/test_ra8_sci_lin.c`), not yet on silicon.

`ra8_emulator` does not model the SCI_B Simple-LIN registers (break-field timer,
`XSR0.BFDF` detection), so there is no `make emu-` gate for this app -- hence
`hw_pending` rather than a ra8_emulator-gated example.

Pmod1 shares pins with the Octo-SPI bus; set the board's Pmod1 / OSPI mux
(SW4) to the Pmod1 side before use.

## Build / flash

```sh
cd examples/ek_ra8d2/hw_pending/lin_commander_hil
make            # -> build/lin_commander_hil.elf / .hex / .bin
make flash      # JLinkExe load via scripts/dev/flash.sh
make ozone      # SEGGER Ozone GUI debugger
```

## Promotion checklist

Move to `hw_validated/hil/` only after:

- wiring a LIN transceiver + a responder node on Pmod1,
- tuning `k_lin_hil_break_len` (XCR2.BFLW) to the actual SCICLK so the break
  is >= 13 bit-times at the bus baud, and
- confirming a full commander/responder exchange on the wire.
