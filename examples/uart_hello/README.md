# uart_hello

UART smoke test for the EK-RA8D2. Brings up CGC + SCI0 in async mode
at 115200 8N1 and prints `"hello, ra8d2!\r\n"` once a second while
toggling LED1 as a heartbeat. The on-board J-Link OB exposes SCI0
as a USB CDC virtual serial port, so connecting a terminal to that
port shows the stream without any additional wiring.

## What it tests

- `ra_cgc_init()` -- prerequisite to get PCLKB at the rated rate so
  the BRR computation works.
- `ra_cgc_get_clock_hz(k_ra_clock_id_pclkb, ...)` -- baud divisor is
  computed from PCLKB.
- `ra_pfs_route_peripheral()` for two pins (TXD0 = P1_01, RXD0 =
  P1_02) into SCI async mode (`PSEL = k_ra_psel_sci_async`, 0x04).
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
| Clean greeting at 115200 8N1, LED1 toggles in time | SCI0 + CGC + PCLKB are healthy |
| Garbled bytes (e.g. `hello, ra8d2!` -> random characters) | BRR is computed against the wrong PCLK; either `ra_cgc_init` brought PCLKB up at a different rate than expected, or `ra_sci_init`'s BRR formula has a rounding bug |
| Single character then silence | TX state machine isn't waiting for TDRE; check `ra_sci_putc_polling` poll loop |
| LED1 toggles but no UART output | TXD0 pin isn't routed; check the `ra_pfs_route_peripheral` claim in the validator log, or board-level wiring (the J-Link OB CDC may have been claimed by another tool) |
| Board hangs immediately, no LED | `ra_cgc_init` HardFault -- run `clock_check` first to isolate |

## Pinout (`r7ka8d2kflcac_pinout.txt`)

```
TXD0 -- P1_01       (PFS PSEL = k_ra_psel_sci_async, 0x04)
RXD0 -- P1_02       (PFS PSEL = k_ra_psel_sci_async, 0x04)
```

These pins are wired straight to the J-Link OB on the EK-RA8D2 and
cannot be re-routed at runtime.

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
