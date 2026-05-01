# ereader -- Phase-6 e-reader skeleton (EK-RA8D2)

End-to-end skeleton tying every X-Ware component (ThreadX, FileX,
GUIX) plus `libs/ra_epub` and (when present) `libs/ra_reflow` into a
working e-reader running on the EK-RA8D2's on-board parallel TFT
panel. The E-Ink panel and the custom carrier board are deferred to
Phase 6.2.1.

## What it does

1. `ra_cgc_init` brings the chip up at the canonical 1 GHz CPUCLK0 /
   100 MHz PCLKA layout.
2. `ra_pfs_route_peripheral` routes:
   - the SCI8 J-Link OB CDC pins (PD_02 / PD_03);
   - the GLCDC parallel-RGB pins (LCDD0..23, HSYNC, VSYNC, DE, CLK);
   - the SDHI pins for the micro-SD slot (CMD, CLK, DAT0..3, WP, CD).
3. SCI8 is brought up at 115200 8N1 for the `[ereader] ...` console.
4. LED1 (P6_00, "page-render heartbeat") and LED2 (P3_03, "book
   loaded") are configured as digital outputs.
5. `tx_kernel_enter` hands control to ThreadX. Three threads run:

   | Thread          | Priority | Role                                                                                                                                                |
   | --------------- | -------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
   | `library`       | 5        | Walks `/books/*.epub` on the SD card via FileX, opens each with `ra_epub_open`, pulls Dublin Core metadata, builds an in-memory catalogue.          |
   | `ui`            | 4        | Drives GUIX. Two windows on the same canvas: "Library View" (vertical list) and "Reader View" (current page).                                       |
   | `reader`        | 6        | On `k_ereader_msg_load_book` opens the EPUB, calls `ra_epub_load_chapter(0)`, and (when `ra_reflow` exists) reflows + renders into the framebuffer. |

6. SCI8 prints two telemetry lines:
   - `[ereader] loaded N books from SD card`  -- emitted by
     `library_thread` once the catalogue is built;
   - `[ereader] rendering chapter 0 page M`   -- emitted by
     `reader_thread` per render request.

## Touch input is deferred

The EK-RA8D2 v1 board does not wire its touch-controller IC to a
chip-side I2C bus that we have brought up yet. Until Phase 6.2.1
lands a confirmed touch wiring, **the on-screen "left arrow" /
"right arrow" widgets in Reader View are simulated by the on-board
buttons**:

- **SW1** -- "page-prev" while a book is open; "load first book"
  before any book has been picked.
- **SW2** -- "page-next" while a book is open.

The buttons are routed through ICU external-IRQ pins via
`ra_icu_enable_external_irq`; the IRQ handler posts
`k_ereader_msg_sw1` / `k_ereader_msg_sw2` to `s_ui_queue` and the
UI thread translates them into reader-thread messages.

## Reflow gating

`libs/ra_reflow` is being written by a sibling sweep. The build
auto-detects it: if `libs/ra_reflow/src/*.c` exists and is
non-empty, CMake sets `RA_HAS_REFLOW` and `main.c` calls
`ra_reflow_layout_chapter` + `ra_reflow_render_page`. Without it,
the demo paints a fallback `Hello world (reflow pending)` GUIX
prompt so the build still passes.

## Build / flash

```sh
cd examples/ereader
make             # cross-compile ereader.elf + .hex + .bin
make flash       # JLinkExe load via scripts/flash.sh
make size        # arm-none-eabi-size on the ELF
make clean       # rm -rf build/
```

The Makefile forces `RA_USE_THREADX=ON`, `RA_USE_FILEX=ON`, and
`RA_USE_GUIX=ON`. Without them the demo cannot link.

## Test recipe (on real hardware)

1. Format a micro-SD card as FAT32 and create a top-level `/books/`
   directory.
2. Drop one or more `.epub` files into `/books/` (each must be no
   larger than 256 KiB -- the skeleton's static blob cap).
3. Insert the card into the EK-RA8D2 micro-SD slot (J6).
4. Connect a 115200 8N1 terminal to the J-Link OB CDC port.
5. `make flash` and reset the board.
6. The console should show:
   ```
   [ereader] booting ThreadX + FileX + GUIX + ra_epub...
   [ereader] loaded N books from SD card
   ```
   where N is the count of valid `.epub` files. The TFT panel shows
   the Library View with one line per book ("Title -- Author").
7. Press SW1 to load the first book. LED2 lights and the reader
   thread emits `[ereader] rendering chapter 0 page 0`. LED1 toggles
   on every render.
8. Press SW2 / SW1 to step page-next / page-prev. Each press emits
   another rendering line and toggles LED1.

## Verify gates (host CI)

The skeleton is part of the standard Wave-11 verification set:

```sh
make clean
make blink \
  && make blink_hal \
  && make clock_check \
  && make uart_hello \
  && make threadx_blink \
  && make threadx_filex_demo \
  && make ereader

bash scripts/format_code.sh
bash scripts/format_code.sh --check

rm -rf build/host-docker build/tidy
bash scripts/test-docker.sh

rm -rf build/tidy
docker run --rm -v "$PWD":/work -w /work ra8d2-firmware-test:latest \
  bash -lc "bash scripts/clang_tidy.sh --check"
```
