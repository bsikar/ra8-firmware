# ESP32-C6 firmware spike

> **Status: SPIKE / under owner review.** This directory is a from-scratch
> ESP32-C6 firmware built with the *same methodology* as the RA8 tree: our own
> register-level drivers behind SOLID/DIP seams, our own build + flash integrated
> into a plain `Makefile`, **no `idf.py`, no ESP-IDF build system**, a standard
> low-level RISC-V toolchain. It is deliberately isolated on the `spike/esp32c6`
> branch and touches nothing under the RA8 tree.
>
> **Read next:** `docs/DIRECTION.md` -- the researched decision document on
> whether this from-scratch lane, ESP-IDF, or a vendor co-processor firmware
> should be the product path (recommendation: co-processor; this spike re-homes
> as the bench/recovery substrate). `docs/UPDATE_PIPELINE.md` -- the unified
> OTA + USB update design. `docs/reference/` -- the committed C6 TRM v1.2 and
> datasheet; every register access below now carries a real chapter/page
> citation verified against them.

## Why a second chip, and why this shape

The e-reader's companion-IC story wants a small wireless MCU next to the RA8.
The ESP32-C6 (single-core **RISC-V rv32imac** @160 MHz HP core + a LP core,
Wi-Fi 6 / BLE 5 / 802.15.4) is the candidate. The owner's constraints, carried
over verbatim from the RA8 work:

1. **Our own drivers.** Register-level, hand-written under the same rules as
   `libs/ra8_hal/` (typed C23 enums, HUM-equivalent = TRM citations above every
   register access, full docs). No vendor driver copied in.
2. **The driver layer must be SWAPPABLE.** The owner may later prefer the
   official Espressif drivers. So drivers sit *behind an interface* (`hal/`,
   function-pointer vtables = the DIP seam), exactly like `ra8_io_*` in the RA8
   tree. `drivers/ours/` implements the interface today; `drivers/idf/` is a
   stub showing an ESP-IDF-backed implementation drops in without touching a
   single caller.
3. **Our own build/flash in the Makefile.** `make -C esp32` cross-compiles with
   `riscv64-elf-gcc` (multilib `rv32imac`/`ilp32`) + our linker script + our
   `start.S`, then `make -C esp32 flash` runs our packer + downloader. No IDF.
4. **A simulator.** `sim/` runs the firmware host-side (the RA8 `board_sim`
   philosophy) with modeled peripherals, and models the link to the companion
   IC so the two-chip protocol can be exercised with no hardware.
5. **OTA + USB-HS update.** The fast USB (480 Mbps HS) is on the **RA8, not the
   C6** (the C6 only has a 12 Mbps Full-Speed USB-Serial-JTAG), so the USB update
   is **exposed from the RA8's USB HS** -- our fastest port -- and the C6 is
   updated *through* the RA8 over the companion link. The self-test needs only
   the EK-RA8D2: its two USB peripherals (HS + FS) loop **one port sends the
   image, the other receives it**, driving the host -> RA8 -> companion -> C6
   commit/rollback chain, then feeding the same image into OTA and the simulator.
   (See `update/README.md`. The C6's own USB is a dev-only bring-up console.)

The north star is **one cross-chip firmware**: the application and the protocol
layer are written against the SOLID interfaces and compile for *either* the RA8
(Arm) or the ESP32-C6 (RISC-V), with only the driver implementations differing.

## Layout

```
esp32/
  README.md                 -- this spec
  docs/
    DIRECTION.md            -- decision document: the product path for the C6
    UPDATE_PIPELINE.md      -- one update pipeline (OTA + USB as transports)
    reference/              -- committed C6 TRM v1.2 + datasheet (PDF)
  Makefile                  -- our-own build + flash (no idf.py)
  cmake/toolchain-esp32c6.cmake
  boot/
    start.S                 -- crt0: stack, .bss zero, .data copy, call app_main
    esp32c6.ld              -- memory map (HP SRAM 512K @0x40800000, flash XIP)
  hal/
    esp_hal.h               -- the SWAPPABLE interface: gpio/uart vtables (DIP)
  drivers/
    ours/                   -- OUR OWN register drivers implementing esp_hal
      esp_gpio.c, esp_uart.c, esp_soc.h (TRM base addresses, typed enums)
    idf/                    -- STUB: how an ESP-IDF-backed impl swaps in
      README.md
  src/
    main.c                  -- app_main: blink + UART hello, via the HAL only
  tools/
    esp_mkimage.py          -- our-own Espressif image packer (magic 0xE9)
    esp_flash.py            -- our-own downloader (ROM serial protocol) / esptool shim
  sim/
    README.md               -- simulator + companion-IC-link plan
  update/
    README.md               -- OTA (A/B) + USB-update + two-USB self-test plan
```

## Boot chain (ESP32-C6, our-own path)

The ROM bootloader reads flash offset 0x0. Two ways to run *our* code with no
ESP-IDF 2nd-stage bootloader:

- **Image format (primary):** `esp_mkimage.py` emits the Espressif app image
  (magic `0xE9`, segment table, entry point, appended SHA-256). A minimal
  our-own 2nd-stage (roadmap) or the ROM loads it. This is the path that also
  carries the OTA/USB update payload.
- **Direct boot (fast spike path):** the C6 ROM will jump straight into flash
  when the image is arranged for it -- lets a bare `.bin` run with no image
  header while the packer/2nd-stage are still being built. Used only to bring
  the first blink up on the bench.

`start.S` then sets `gp`/`sp`, zeroes `.bss`, copies `.data` from flash to SRAM,
and calls `app_main()`.

## What this spike delivers vs. what is roadmap

**Delivered (this branch):**
- The build system: toolchain file + `Makefile` + linker + `start.S` that
  cross-compile a minimal firmware to an ELF + `.bin` with `riscv64-elf-gcc`.
- The SOLID/DIP HAL seam (`hal/esp_hal.h`) + our-own GPIO + UART drivers +
  an `app_main` that only ever calls through the interface.
- `esp_mkimage.py` producing a first-cut Espressif image.
- This spec + the sim/update design notes.

**Roadmap (for the morning + beyond):**
- On-bench bring-up (needs the C6 EVM): confirm the boot chain + blink + UART.
- Our-own serial downloader (the ROM SLIP protocol) so `make flash` needs no
  esptool; until then `esp_flash.py` shims esptool as the transport.
- The simulator (`sim/`) + the modeled companion-IC link.
- OTA (A/B partitions + rollback, mirroring `ra8_dfu`/`ra8_ota` from the RA8 side)
  and the USB-update two-port self-test.
- Wi-Fi/BLE/802.15.4 driver work (large; behind the same HAL seams).

## Build (once `riscv64-elf-gcc` is installed)

```
make -C esp32            # -> build/esp32c6-blink.elf + .bin
make -C esp32 image      # -> build/esp32c6-blink.app.bin (Espressif format)
make -C esp32 flash      # -> downloads to a connected C6 (esp_flash.py)
make -C esp32 sim        # -> host-side run (roadmap)
make -C esp32 clean
```

The RA8 root `Makefile` gains an `esp32` passthrough target so the monorepo
builds both chips from the top (roadmap; kept out of the RA8 CI matrix so the
two toolchains stay independent).
