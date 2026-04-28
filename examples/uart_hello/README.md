# uart_hello

UART smoke test for the EK-RA8D2. Brings up CGC + SCI8 in async mode
at 115200 8N1 and prints `"hello, ra8d2!\r\n"` once a second while
toggling LED1 as a heartbeat. The on-board J-Link OB on the
EK-RA8D2 bridges SCI8 (TXD8 = PD02, RXD8 = PD03) to a USB CDC
virtual serial port on the host -- connect a terminal to that port
and the stream should land without any extra wiring.

## Blocked: needs SCI_B retrofit

This app builds and links cleanly, and SWD halt-and-read confirms
the firmware reaches its main loop (`ra_cgc_init`, `ra_sci_init`,
the `ra_sci_write_polling` call all return `k_ra_ok`). **But no
bytes appear on the CDC port.**

Root cause: `libs/ra_hal/src/ra_sci.c` targets the legacy SCI
register layout. The RA8D2 actually uses SCI_B, whose register
offsets are different (HUM Ch 38 "Serial Communications Interface",
p 2174 onwards). The unit-test suite passes because the host
simulator backs MMIO with ordinary RAM -- offsets don't matter for
read-then-read-back functional tests. On real silicon the byte
writes land at the wrong addresses and the SCI_B engine never
shifts anything out.

The header `libs/ra_hal/inc/ra_sci.h` already flags this at
line 34.

## Fix path

1. Open HUM Ch 38 (SCI_B) and re-derive
   `libs/ra_hal/inc/ra8d2_sci_regs.h` against the real layout
   (RDR / TDR / CCR0..CCR4 / FCR / CFCLR / etc.).
2. Update `libs/ra_hal/src/ra_sci.c` to use the new field names and
   programming order (CCR0.RE/TE bits move, the BRR formula
   changes -- SCI_B uses CCR2.BRR / MDDR / ABCS).
3. Rerun this app -- the chip-side wiring (channel 8 / PD02 / PD03)
   is already correct.

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
