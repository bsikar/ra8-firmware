# uart_hello

Brings up CGC + SCI8 in async mode and prints `hello, ra8d2!` once a second
while toggling LED1 as a heartbeat. The EK-RA8D2's on-board J-Link OB bridges
SCI8 to a USB CDC port, so a terminal at **115200 8N1** sees the stream with no
extra wiring.

> On macOS, open the CDC port **RDWR**. Some bridges gate forwarding on DTR,
> which only asserts on a read-write open -- `cat /dev/cu.usbmodem*` is RDONLY
> and shows zero bytes. `picocom`, `screen` and `cu` all work.

## Pinout

TXD8 is `PD02` and RXD8 is `PD03`, both at `PFS PSEL = k_ra8_psel_sci_async`
(0x04). They are wired straight to the J-Link OB and cannot be re-routed at
runtime; the board-side fact lives in `libs/ra8_board_ek_ra8d2`, and the
chip-side ball map is
[`docs/pinouts/ra8d2_bga289_mipi.txt`](../../../../../docs/pinouts/ra8d2_bga289_mipi.txt).
Flow control is wired too (PD04 RTS, PD05 CTS); the demo runs without it.

LED1 is P600. The console pins are routed by `ra8_pfs_route_peripheral` rather
than the BSP's `ra8_board_uart_console_init` veneer, which still forwards to
SCI3.

## Clock tree

`ra8_cgc_init()` brings up the FSP-quickstart tree from the board's 24 MHz XTAL:

| Clock | Source | Rate |
|---|---|---|
| PLL1P | XTAL / 3 * 250 / 2 | 1 GHz |
| PLL1Q | XTAL / 3 * 250 / 6 | ~333 MHz |
| PLL1R | XTAL / 3 * 250 / 5 | 400 MHz |
| CPUCLK0 | PLL1P / 1 | 1 GHz |
| CPUCLK1 / ICLK / MRICLK | PLL1P / 4 | 250 MHz |
| PCLKA | PLL1P / 8 | 125 MHz |
| PCLKB | PLL1P / 16 | 62.5 MHz |
| SCICLK | PLL1R / 4 | 100 MHz |

`ra8_sci_init` reads PCLKA back from `ra8_cgc_get_clock_hz` and programs
`BRR = 33`, giving 114890 baud against a nominal 115200 -- 0.27% off, well
inside UART tolerance.

## The bits that were not obvious

All verified live over SWD and cross-referenced against FSP `r_sci_b_uart`.

| Bit | Set to | Why |
|---|---|---|
| `CCR1.SPB2DT` | 1 | TXD idles HIGH when `TE=0`. Without it the line floats LOW between frames and the host decodes a permanent break. |
| `CCR1.SPB2IO` | 1 | Enables the SPB2DT idle-level driver. |
| `CCR3.LSBF` | 1 | LSB-first. SCI_B resets to MSB-first, so without it every byte arrives bit-reversed. |
| `CCR3.BPEN` | 1 | Synchronizer bypass, required when PCLK is the operation clock (HUM 38.2.8). Without it the shifter waits forever for an SCICLK edge that does not exist and CSR sits at `TDRE=0, TEND=0`. |

Pin assignments follow EK-RA8D2 v1 User's Manual (R20UT5523EG0101 Rev 1.01)
Table 13 "Debug Serial Port Assignment" p 24 and Table 24 "LED Functions" p 31;
the peripherals are HUM R01UH1065EJ0130 Ch 38 "SCI" and Ch 8 "CGC".
