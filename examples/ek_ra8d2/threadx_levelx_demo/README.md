# threadx_levelx_demo

ThreadX + LevelX OSPI-flash wear-levelling demo for the EK-RA8D2.

## What this app does

1. Brings the chip up the same way `uart_hello` does (CGC + SCI8 @
   115200 8N1 on PD_02 / PD_03).
2. Hands control over to ThreadX (`tx_kernel_enter`).
3. The single worker thread:
   - Initialises `ra_xspi` against the on-board EK-RA8D2 Macronix
     MX25LM512 octal-SPI flash chip.
   - Calls `lx_nor_flash_format()` to lay down a fresh LevelX
     partition (64 blocks * 4 KiB = 256 KiB), then `lx_nor_flash_open()`
     to mount it.
   - Heartbeats every second: programs an incrementing counter into
     LevelX logical sector 0, reads it back, and prints the value to
     SCI8.
   - After 10000 heartbeat cycles, prints LevelX's wear-levelling
     statistics (write/read counts, min/max erase counts, free /
     mapped / obsolete physical sector counts, sector + extended
     cache hit/miss counts).

The point of the burn-in is to demonstrate that LevelX has spread
those 10000 writes across all 64 blocks rather than letting any
single block exceed its erase budget.

## Recipe

1. Connect a USB cable to the J-Link OB CDC port on the EK-RA8D2.
2. Open a 115200 8N1 terminal (`picocom -b 115200 /dev/cu.usbmodem...`
   on macOS, `minicom -D /dev/ttyACM0 -b 115200` on Linux).
3. Build and flash:

   ```
   cd examples/threadx_levelx_demo
   make
   make flash
   ```

4. Watch the per-100-cycle progress prints stream out, then the final
   wear-levelling statistics dump.

## Build dependencies

This app cannot build with the default bare-metal-only configuration.
The per-app `Makefile` forces `-DRA_USE_THREADX=ON -DRA_USE_LEVELX=ON`
into the cmake configure step, which in turn pulls in
`libs/third_party/threadx/` and `libs/third_party/levelx/`.

If the top-level `make threadx_levelx_demo` complains about missing
ThreadX targets, that means the sibling Wave 13 ThreadX port has not
landed yet -- see `cmake/threadx.cmake`.

Note: the EK-RA8D2 v1 board carries an **Infineon IS25LX512M-JHLE**
64 MB Octo-SPI flash (UM Section 6.3 + Table 29 p 35), not a Macronix
MX25LM512 -- the README/main.c reference to "Macronix MX25LM512" is a
copy-paste from the FSP example and should be read as "the on-board
Octo-SPI NOR flash" regardless of vendor part.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED init/toggle (LEDs per EK-RA8D2
v1 UM Table 24 p 31). On-board Octo-SPI flash pins per UM Table 29
"Octo-SPI Flash Assignments" p 35 (SW4-3 selects Octo-SPI vs Arduino/
Pmod1).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Table 13 p 24 + Table 24 p 31 + Section 6.3 + Table 29 p 35,
and HUM (R01UH1065EJ0130) Ch "Octo-SPI / OSPI".
