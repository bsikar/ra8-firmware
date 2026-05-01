# threadx_filex_demo

ThreadX + FileX SD-card mount demo for the EK-RA8D2.

## What this app does

1. Brings the chip up the same way `uart_hello` does (CGC + SCI8 @
   115200 8N1 on PD_02 / PD_03).
2. Hands control over to ThreadX (`tx_kernel_enter`).
3. The single worker thread:
   - Calls `fx_media_open` against the FileX <-> `ra_sdhi` driver
     shim (`fx_media_driver_ra_sdhi`).
   - Walks the root directory with
     `fx_directory_first_full_entry_find` /
     `fx_directory_next_full_entry_find` and prints each entry name to
     SCI8.
   - If a `README.TXT` exists at the root, opens it with
     `fx_file_open` and dumps its contents to SCI8.

## Recipe

1. Format a micro-SD card as FAT32 from your laptop:
   - macOS: Disk Utility -> Erase -> "MS-DOS (FAT)".
   - Linux: `sudo mkfs.vfat -F 32 /dev/sdX1`.
2. Drop a plain ASCII text file named `README.TXT` (uppercase, 8.3
   short name) into the root of the card. Anything you put in here
   gets streamed back over SCI8.
3. Insert the card into the EK-RA8D2 micro-SD slot (J6 on the EK
   silkscreen).
4. Connect a USB cable to the J-Link OB CDC port on the EK.
5. Open a 115200 8N1 terminal (`picocom -b 115200 /dev/cu.usbmodem...`
   on macOS, `minicom -D /dev/ttyACM0 -b 115200` on Linux).
6. Build and flash:

   ```
   cd examples/threadx_filex_demo
   make
   make flash
   ```

7. Watch the listing + README contents stream out over the terminal.

## Build dependencies

This app cannot build with the default bare-metal-only configuration.
The per-app `Makefile` forces `-DRA_USE_THREADX=ON -DRA_USE_FILEX=ON`
into the cmake configure step, which in turn pulls in
`libs/third_party/threadx/` and `libs/third_party/filex/`.

If the top-level `make threadx_filex_demo` complains about missing
ThreadX targets, that means the sibling Wave 13 / Phase 4.1 ThreadX
port has not landed yet -- see `cmake/threadx.cmake`.
