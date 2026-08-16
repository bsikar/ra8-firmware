# uart_hello

UART HIL test for the EK-RA8D2. Brings up CGC + SCI8 in async mode
and prints `"hello, ra8d2!\r\n"` once a second while toggling LED1
as a heartbeat. The on-board J-Link OB on the EK-RA8D2 bridges
**SCI8 (TXD8 = PD02, RXD8 = PD03) to a USB CDC virtual serial port**
on the host -- connect a terminal and the stream should land without
any extra wiring.

## Status: working

After flashing, open the J-Link OB CDC port at **115200 8N1**:

```sh
# macOS
stty -f /dev/cu.usbmodem* 115200 cs8 -cstopb -parenb -ixon -ixoff raw
# Use a tool that opens RDWR (not RDONLY) -- some macOS CDC bridges
# don't pass data unless DTR is asserted, which sysopen O_RDWR
# triggers automatically:
perl -e 'use Fcntl; sysopen(my $f, "/dev/cu.usbmodem...", O_RDWR) or die $!;
         while (sysread($f, my $b, 1024)) { print $b }'
# or picocom / screen / cu
picocom -b 115200 /dev/cu.usbmodem...
```

You should see one line per second:

```
hello, ra8d2!
hello, ra8d2!
hello, ra8d2!
...
```

LED1 (P6_00) toggles in lock-step with each line.

## Clock tree

`ra8_cgc_init()` brings up the FSP-quickstart clock tree:

| Clock     | Source              | Rate        |
|-----------|---------------------|-------------|
| XTAL      | EK-RA8D2 24 MHz     | 24 MHz      |
| PLL1P     | XTAL / 3 * 250 / 2  | 1 GHz       |
| PLL1Q     | XTAL / 3 * 250 / 6  | ~333 MHz    |
| PLL1R     | XTAL / 3 * 250 / 5  | 400 MHz     |
| CPUCLK0   | PLL1P / 1           | 1 GHz       |
| CPUCLK1   | PLL1P / 4           | 250 MHz     |
| ICLK      | PLL1P / 4           | 250 MHz     |
| PCLKA     | PLL1P / 8           | 125 MHz     |
| PCLKB     | PLL1P / 16          | 62.5 MHz    |
| MRICLK    | PLL1P / 4           | 250 MHz     |
| SCICLK    | PLL1R / 4           | 100 MHz     |

`ra8_sci_init` reads `PCLKA = 125 MHz` from `ra8_cgc_get_clock_hz` and
programs `BRR = 33` for 115200 baud, giving an actual wire rate of
114890 baud (0.27 % off, well within UART tolerance).

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

## Pinout

```
TXD8 -- PD_02       (PFS PSEL = k_ra8_psel_sci_async, 0x04)
RXD8 -- PD_03       (PFS PSEL = k_ra8_psel_sci_async, 0x04)
```

These pins are wired straight to the J-Link OB on the EK-RA8D2 and
cannot be re-routed at runtime. The board-side fact lives in
`libs/ra8_board_ek_ra8d2`; for the chip-side ball map -- which ball
PD02 is, and everything else it can be muxed to -- see
[`docs/pinouts/ra8d2_bga289_mipi.txt`](../../../../../docs/pinouts/ra8d2_bga289_mipi.txt)
(the EK-RA8D2 carries an `R7KA8D2KFLCAC`). Hardware flow control is also wired
(PD04 RTS, PD05 CTS) but the demo runs without it.

## Build + flash

From the repo root:

```sh
make uart_hello                       # cross-compile -> examples/ek_ra8d2/hw_validated/hil/uart_hello/build/uart_hello.elf
make -C examples/uart_hello flash     # flash via on-board J-Link OB
```

Or standalone:

```sh
cd examples/ek_ra8d2/hw_validated/hil/uart_hello/
make
make flash
make clean
```

## What the firmware does

1. `ra8_cgc_init()` brings up XTAL + PLL1 (CPUCLK0 = 1 GHz, PCLKA =
   125 MHz, SCICLK = PLL1R / 4). Includes MRMS PFB flush, VSCR
   voltage scaling, MRMS wait-state programming, and SCICLK
   routing -- no per-app workarounds required.
2. `ra8_pfs_route_peripheral()` puts PD02 / PD03 in SCI8 async mode
   (PSEL = `k_ra8_psel_sci_async` = 0x04).
3. `ra8_sci_init(8, ...)` programs CCR0/1/2/3 with the bits in the
   table above. `pclk_hz` comes from `ra8_cgc_get_clock_hz` so the
   BRR calculator never goes stale.
4. `ra8_time_init()` sets up SysTick for `ra8_delay_ms`.
5. `ra8_board_led_init(k_ra8_board_led1)` for the heartbeat (LED1 = P600
   per EK-RA8D2 v1 UM Table 24 p 31).
6. Loop: write the greeting, toggle LED1, sleep 1 s.

## BSP usage

Uses `ra8_board_ek_ra8d2` BSP for LED1 init/toggle (P600). The SCI8
console pins (PD02 / PD03) are routed via `ra8_pfs_route_peripheral`
directly rather than through the BSP `ra8_board_uart_console_init`
veneer (which currently forwards to SCI3 -- the BSP veneer's channel
selection lags behind the FSP-aligned SCI8 wiring this app uses on the
J-Link OB CDC bridge per UM Table 13 p 24).

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
mem32 0x4001E0AC 1    # PLLCCR  -- expected 0xFA02
mem16 0x4001E04C 1    # PLLCCR2 -- expected 0x0451
mem32 0x4013C000 1    # MRCPFB  -- expected 0x01 (PFB on)
mem32 0x4013C004 1    # MRCFREQ -- expected 0xFA  (250 MHz, key stripped)
mem32 0x4013C008 1    # MREFREQ -- expected 0x7D  (125 MHz, key stripped)
mem32 0x4001E014 1    # VSCR    -- expected bit 0 set (VSCM=1)
```

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 13 "Debug Serial Port Assignment" p 24 + Table 24
"LED Functions" p 31, and HUM (R01UH1065EJ0130) Ch 38 "SCI" / Ch 8
"CGC".
