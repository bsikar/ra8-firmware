# uart_hello

UART smoke test for the EK-RA8D2. Brings up CGC + SCI8 in async mode
at 115200 8N1 and prints `"hello, ra8d2!\r\n"` once a second while
toggling LED1 as a heartbeat. The on-board J-Link OB on the
EK-RA8D2 bridges SCI8 (TXD8 = PD02, RXD8 = PD03) to a USB CDC
virtual serial port on the host -- connect a terminal to that port
and the stream should land without any extra wiring.

## Status: silent on the wire, blocked on hardware verification

The SCI_B retrofit (HUM Ch 38, p 2174+) is in. The driver and pinout
have been verified bit-by-bit via SWD halt-and-read against FSP's
`r_sci_b_uart` reference and the Renesas FSP `ek_ra8d2_ep` quickstart:

- **Channel + pins correct.** EK-RA8D2 v1 User's Manual table 13
  ("Debug Serial Port Assignment") confirms PD02=TXD, PD03=RXD,
  bridged to debugger MCU U7's matching pins.
- **PFS routing correct.** PD02 PFS reads `PMR=1, PSEL=0x04` (SCI8
  async).
- **Module-stop cleared.** MSTPCRB[SCI8] reads 0.
- **Driver writes verified.** `CCR0=0x11 (TE|RE)`,
  `CCR1=0x30 (SPB2DT|SPB2IO)` (TXD idles HIGH), `CCR2.BRR=0x0F`,
  `CCR3=0x1200 (LSBF|CHR=8-bit)`, `CSR.TEND=1+TDRE=1` after each
  write -- chip thinks it shifted bytes out.
- **CCR1.SPB2 + CCR3.LSBF bugs found and fixed** by direct
  comparison against FSP's `r_sci_b_uart` -- without those, TXD
  would idle low (host sees a permanent break) and bytes would
  ship MSB-first.

What's still wrong: nothing reaches the host CDC port. After also
hand-poking SCICKCR via SWD to switch SCICLK from reset-default
MOCO (~8 MHz) to PLL1R/8 (~60 MHz) and resetting the chip, sweep of
host baud rates (9600 / 19200 / 38400 / 57600 / 115200 / 230400 /
460800) all returned 0 bytes on `/dev/cu.usbmodem*`.

`JLinkExe ... VCOM enable` was accepted and the user power-cycled
the probe; no change.

Best remaining hypotheses, in order of likelihood:

1. The J-Link OB virtual COM bridge isn't actually engaging on the
   J-Link-OB-RA4M2 firmware shipped on this kit, despite the
   `VCOM enable` command being accepted. Confirmation needs a
   different probe firmware revision OR an external USB-UART
   wired to the Pmod headers.
2. The SCI_B engine's TCLK isn't actually clocking the bit shifter
   even after the SCICKCR change. The OPERATING-CLOCK-vs-PCLK
   selector on SCI_B isn't fully understood -- there may be an
   additional bit (CCR4 or a SoC-level register) that gates TCLK
   to the shifter. Confirmation needs a logic analyzer on PD02.
3. A board-level link cut or solder bridge between PD02 and U7 is
   missing on this specific kit revision. Confirmation needs the
   schematic and a continuity check.

Until one of those three is in hand, this app stays "builds + links
+ all SWD-visible state correct, line silent."

## What was useful from this trace

- `libs/ra_hal/src/ra_sci.c` is now a real SCI_B driver against the
  HUM register window, not a placeholder. The FSP-cross-referenced
  CCR1.SPB2 / CCR3.LSBF bits were genuine bugs.
- The pinout file in `r7ka8d2kflcac_pinout.txt` now records the
  correct J-Link OB CDC <-> SCI8 / PD02-PD03 mapping cited from the
  EK-RA8D2 v1 User's Manual.
- `examples/uart_hello/main.c` panic-halts on every init failure
  and uses a hardcoded `pclka_hz = 60_000_000U` derived from a live
  SCKDIVCR readback (`ra_cgc_get_clock_hz()` reports a different
  value -- separate CGC-driver bug).

## Build + flash

From the repo root:

```sh
make uart_hello                       # cross-compile -> examples/uart_hello/build/uart_hello.elf
make -C examples/uart_hello flash     # flash via on-board J-Link OB
```

Or standalone, from inside `examples/uart_hello/`:

```sh
cd examples/uart_hello/
make
make flash
make clean
```

## What it tests

- `ra_cgc_init()` -- prerequisite to get PCLKB at the rated rate so
  the BRR computation works.
- `ra_cgc_get_clock_hz(k_ra_clock_id_pclkb, ...)` -- baud divisor is
  computed from PCLKB.
- `ra_pfs_route_peripheral()` for two pins (TXD8 = PD_02, RXD8 =
  PD_03) into SCI async mode (`PSEL = k_ra_psel_sci_async`, 0x04).
- `ra_sci_init(0, ...)` -- BRR programming, SMR + SCMR config,
  TIE / RIE / TE / RE bits, end-to-end.
- `ra_sci_write_polling()` -- hot loop that pushes bytes through the
  TX FIFO, waiting on TDRE between each.

Unlike the unit-test harness, this exercises the live PERI clock
tree end-to-end -- if the BRR rounded the wrong direction, the host
sees garbage.

## Connect a terminal

The EK-RA8D2's J-Link OB exposes one USB CDC channel. On macOS:

```sh
ls /dev/cu.usbmodem*                     # find the J-Link CDC port
screen /dev/cu.usbmodem<...> 115200      # 115200 8N1, no flow control
```

On Linux:

```sh
ls /dev/ttyACM*
minicom -D /dev/ttyACM0 -b 115200
```

You should see one line per second:

```
hello, ra8d2!
hello, ra8d2!
...
```

LED1 (P6_00) toggles in lock-step with each line.

## Build + flash

From the repo root:

```sh
make uart_hello                       # cross-compile -> examples/uart_hello/build/uart_hello.elf
make -C examples/uart_hello flash     # flash via on-board J-Link OB
```

Or standalone, from inside `examples/uart_hello/`:

```sh
cd examples/uart_hello/
make
make flash
make clean
```

## Pass / fail

| What you see | Verdict |
|---|---|
| Clean greeting at 115200 8N1, LED1 toggles in time | SCI8 + CGC + PCLKB are healthy (post-SCI_B retrofit) |
| Garbled bytes (e.g. `hello, ra8d2!` -> random characters) | BRR is computed against the wrong PCLK; either `ra_cgc_init` brought PCLKB up at a different rate than expected, or `ra_sci_init`'s BRR formula has a rounding bug |
| Single character then silence | TX state machine isn't waiting for TDRE; check `ra_sci_putc_polling` poll loop |
| LED1 toggles but no UART output | Most likely: SCI_B retrofit hasn't landed yet (see "Blocked" above). Otherwise check the `ra_pfs_route_peripheral` claim in the validator log, or whether another tool has the J-Link CDC port open. |
| Board hangs immediately, no LED | `ra_cgc_init` HardFault -- run `clock_check` first to isolate |

## Pinout (`r7ka8d2kflcac_pinout.txt`)

```
TXD8 -- PD_02       (PFS PSEL = k_ra_psel_sci_async, 0x04)
RXD8 -- PD_03       (PFS PSEL = k_ra_psel_sci_async, 0x04)
```

These pins are wired straight to the J-Link OB on the EK-RA8D2 and
cannot be re-routed at runtime. Confirmed against the Renesas FSP
`ek_ra8d2_ep` quickstart project (board nets `VCOMM_TXD_TO_EXT` /
`VCOMM_RXD_FROM_EXT`).

## What this does NOT test

- RX path (driver supports it; the demo is TX-only to keep the
  output channel one-way and not need a host-side test fixture).
- Interrupt-driven TX / RX (uses polling for simplicity; the
  attach-handler API is exercised by the unit tests).
- DMA path -- see `ra_sci_write_dma` in the unit tests.
- TrustZone / NS-side SCI access via NSC veneers.

## Debugging

```sh
make -C examples/uart_hello ozone   # SEGGER Ozone GUI
make -C examples/uart_hello debug   # gdb attached via JLinkGDBServer
```
