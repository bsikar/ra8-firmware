# uart_hello

UART smoke test for the EK-RA8D2. Brings up CGC + SCI8 in async mode
and prints `"hello, ra8d2!\r\n"` once a second while toggling LED1
as a heartbeat. The on-board J-Link OB on the EK-RA8D2 bridges
**SCI8 (TXD8 = PD02, RXD8 = PD03) to a USB CDC virtual serial port**
on the host -- connect a terminal and the stream should land without
any extra wiring.

## Status: working

After flashing, open the J-Link OB CDC port at **38400 8N1**:

```sh
# macOS
stty -f /dev/cu.usbmodem* 38400 cs8 -cstopb -parenb -ixon -ixoff raw
# Use a tool that opens RDWR (not RDONLY) -- some macOS CDC bridges
# don't pass data unless DTR is asserted, which sysopen O_RDWR
# triggers automatically:
perl -e 'use Fcntl; sysopen(my $f, "/dev/cu.usbmodem...", O_RDWR) or die $!;
         while (sysread($f, my $b, 1024)) { print $b }'
# or picocom / screen / cu
picocom -b 38400 /dev/cu.usbmodem...
```

You should see one line per second:

```
hello, ra8d2!
hello, ra8d2!
hello, ra8d2!
...
```

LED1 (P6_00) toggles in lock-step with each line.

## Why 38400 (and not 115200)?

The chip is configured for `BRR=15` against PCLKA, which gives a
nominal `baud = PCLKA / (32 * 16) = PCLKA / 512`. Working backward
from the empirically-verified 38400 wire rate, **PCLKA is
approximately 20 MHz** on this board.

That's lower than the rated peripheral clock, because
`ra_cgc_init()` currently brings PLL1 up at ~160 MHz (with
SCKDIVCR's PCKA=/8 giving 20 MHz) rather than the rated 480 MHz /
1 GHz the part can do. Tightening the CGC driver's PLL setup is
deferred to its own pass; once PLL1 is at 480+ MHz, change
`k_uart_hello_baud` back to 115200 and the example will print at
the conventional rate.

## Findings worth keeping

The bring-up needed several non-obvious bits, all verified live via
SWD and cross-referenced against FSP `r_sci_b_uart` and the
EK-RA8D2 v1 User's Manual:

| Bit | Setting | Why |
|---|---|---|
| `CCR1.SPB2DT` | `1` | TXD idles HIGH when `TE=0`. Without this the line floats LOW between frames and a host UART decodes that as a permanent break. |
| `CCR1.SPB2IO` | `1` | Enables the SPB2DT idle-level driver. |
| `CCR3.LSBF` | `1` | LSB-first transfer. SCI_B's reset state is MSB-first; without this every byte arrives bit-reversed. |
| `CCR3.BPEN` | `1` | Synchronizer Bypass Enable -- required when PCLK is the operation clock (TCLK), per HUM 38.2.8. Without this the shifter waits forever for an SCICLK edge that doesn't exist; CSR shows `TDRE=0, TEND=0` indefinitely. |
| `CCR3.LSBF + CCR3.CHR=8` | `0x1280` | LSB-first 8-bit data. |
| `CCR0.TE | CCR0.RE` | `0x11` | TX + RX enabled. |

`SCI8` channel + `PD02 / PD03` pinout is from the EK-RA8D2 v1 User's
Manual table 13 ("Debug Serial Port Assignment").

The macOS CDC port also needs **opening RDWR** (not RDONLY) -- some
CDC drivers gate forwarding on DTR, which only asserts when the port
is opened for writes too. Standard `cat /dev/cu.usbmodem*` does
RDONLY and shows zero bytes. `picocom`, `screen`, and `cu` all do
RDWR and work.

## Pinout (`r7ka8d2kflcac_pinout.txt`)

```
TXD8 -- PD_02       (PFS PSEL = k_ra_psel_sci_async, 0x04)
RXD8 -- PD_03       (PFS PSEL = k_ra_psel_sci_async, 0x04)
```

These pins are wired straight to the J-Link OB on the EK-RA8D2 and
cannot be re-routed at runtime. Hardware flow control is also wired
(PD04 RTS, PD05 CTS) but the demo runs without it.

## Build + flash

From the repo root:

```sh
make uart_hello                       # cross-compile -> examples/uart_hello/build/uart_hello.elf
make -C examples/uart_hello flash     # flash via on-board J-Link OB
```

Or standalone:

```sh
cd examples/uart_hello/
make
make flash
make clean
```

## What the firmware does

1. `ra_cgc_init()` brings up HOCO + PLL1 (currently at ~160 MHz).
2. `ra_pfs_route_peripheral()` puts PD02 / PD03 in SCI8 async mode
   (PSEL = `k_ra_psel_sci_async` = 0x04).
3. `ra_sci_init(8, ...)` programs CCR0/1/2/3 with the bits in the
   table above. `pclka_hz` is hardcoded to 20 MHz to match the
   actual chip state.
4. `ra_time_init()` sets up SysTick for `ra_delay_ms`.
5. `ra_gpio_output_init(k_ra_pin_led1, low)` for the heartbeat.
6. Loop: write the greeting, toggle LED1, sleep 1 s.

## Debugging

```sh
make -C examples/uart_hello ozone   # SEGGER Ozone GUI
make -C examples/uart_hello debug   # gdb attached via JLinkGDBServer
```

Useful SWD probes:

```
mem32 0x40358800 32   # SCI8 register window
mem32 0x40358848 1    # CSR -- TDRE=bit29, TEND=bit30, RDRF=bit31, FER=bit28
mem32 0x40358804 1    # TDR -- low byte = next byte to shift
mem32 0x40400B48 1    # PD02 PFS -- bits 16=PMR, 26..24=PSEL
mem8  0x4001E054 1    # SCICKDIVCR
mem8  0x4001E055 1    # SCICKCR (SCICLK source select)
mem32 0x4001E020 1    # SCKDIVCR (peripheral clock dividers)
mem8  0x4001E026 1    # SCKSCR (system clock source: 0=HOCO,1=MOCO,5=PLL1)
```
