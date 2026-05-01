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
