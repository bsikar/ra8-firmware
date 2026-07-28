<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# compile_on_m33 -- run the `.rabook` emitter back-end on the M33 (#149b)

Demonstrates the first increment of issue #149(b): offloading the on-device
EPUB->`.rabook` compiler's **back-end** -- the RABOOK1 emitter
(`libs/ra8_rabook_compile`) -- onto the Cortex-M33 secondary core. On a real
import the heavy conversion runs on the M33 @ 250 MHz so the Cortex-M85 @ 1 GHz
stays free to keep the e-reader UI live. Here the M33 builds a small compiled
book and the M85 proves it is well-formed.

## What it teaches

- **Compiler back-end on the slow core.** The M33 drives the zero-heap emitter
  API (`ra8_rabook_compile_init` / `intern` / `add_element` / `add_text` /
  `link_child` / `link_sibling` / `add_chapter` / `set_metadata` / `finalize`)
  over a hand-built 2-chapter DOM and lays a complete RABOOK1 blob into shared
  SRAM.
- **Zero-allocation arenas (NASA Rule 3).** The emitter never `malloc`s; it only
  appends into caller-owned arenas. The scratch tables live in the M33 image's
  own `SRAM_CPU1` `.bss` (~1.5 KiB); only the finished blob lands in the shared
  output buffer so the M85 reads it with no copy.
- **Cross-core handoff via shared SRAM.** `compile_on_m33.h` pins a mailbox at
  `0x22100000` and an 8 KiB output buffer at `0x22100100` (both inside the
  shared upper-SRAM window, below the M33's 64 KiB bank at `0x22190000`). The
  M33 writes; the M85 reads after `done`.
- **The M85 validates the result.** It runs `ra8_book_validate` over the shared
  blob (magic, version, table extents, body CRC-32) and cross-checks the
  M33-reported CRC and chapter count -- the same gate the real reader applies
  before walking a book.

## Memory budget (shared upper SRAM, `0x22100000`)

| Region          | Address      | Size            | Owner / use                         |
|-----------------|--------------|-----------------|-------------------------------------|
| mailbox         | `0x22100000` | 32 B            | cross-core handoff (8 words)        |
| output blob     | `0x22100100` | 8 KiB cap       | M33 writes the finalized RABOOK1    |
| emitter scratch | `SRAM_CPU1`  | ~1.5 KiB `.bss` | M33-private tables (not shared)     |

## How to run (no hardware needed)

```sh
make emu-compile_on_m33
```

This cross-builds Debug (so the `[itm]` log lines are compiled in), builds the
ra8_emulator, and boots the M85 ELF. ra8_emulator sees the embedded
`.cpu1_image` and spins up a second Unicorn engine for the M33 sharing the SRAM.

## Expected output

```
  cpu1 engine   : Cortex-M33, shared SRAM + peripherals (dual-core)
[itm] [M85] INFO: ==== RA8D2 compile_on_m33 demo (#149b emitter offload) ====
[itm] [M85] INFO: Cortex-M85 primary core online
[itm] [M85] INFO: shared mailbox + output blob in SRAM at 0x22100000
[itm] [M85] INFO: releasing Cortex-M33 to run the RABOOK1 emitter ...
[itm] [M85] INFO: ra8_cpu1_release rc (0 = ok)=0
[itm] [M85] INFO: M33 emitter is alive
[itm] [M85] INFO: M85 yielding -- M33 is compiling the book ...
[itm] [M85] INFO: M33 reported blob length (bytes)=...
[itm] [M85] INFO: ra8_book_validate OK; chapters in blob=2
[itm] [M85] INFO: compile_on_m33 PASS
```

## Next increment

This proves the **emitter** (serialization back-end) runs on the M33. A full
`ra8_epub_open` on the M33 -- unzip + XHTML-parse + image-transcode the source
`.epub` -- is a heavier later increment of #149(b): it needs miniz, tinyxml2 and
stb_image linked into the second core, none of which this freestanding image
pulls in today.

## Files

| File                    | Role                                                  |
|-------------------------|-------------------------------------------------------|
| `main.c`                | M85: release M33, wait, validate the blob, log verdict|
| `cpu1_main.c`           | M33: build the DOM, drive the emitter, publish result |
| `compile_on_m33.h`      | Shared mailbox + addresses + constants                |
| `linker_script.ld`      | M85 memory map; pins `.cpu1_image` at MRAM_CPU1        |
| `linker_script_cpu1.ld` | M33 memory map (MRAM_CPU1 + SRAM_CPU1)                 |
| `system_init.c`         | M85 core bring-up (D-cache off)                       |
| `vector_table.c`        | M85 vector table + Reset_Handler                      |
| `trustzone_init.c`      | SAU scaffold (not invoked in single-world build)      |
| `CMakeLists.txt`        | Builds both images; links the emitter into the M33    |
| `Makefile`              | Per-app build / flash / debug wrapper                 |
