<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# Developer tooling (`tools/`)

Host-side dev tools that run the real firmware (and feed it data) without a
board, plus the gate that keeps them honest.

| You want to... | Use | How |
|----------------|-----|-----|
| Run a firmware **`.elf`** with no board: boot it, drive the panel, preview its UI | **`tools/ra8_emulator`** (Unicorn CPU emulator) | `make emu-<app> [PANEL=<name>]` |
| Build a FAT **SD-card image** carrying a font (for `ra8_emulator --sd` or a real card) | **`tools/mkfontimg`** | `cmake -S tools/mkfontimg -B tools/mkfontimg/build && cmake --build tools/mkfontimg/build` |
| Build an **exFAT volume image** through the real `ra8_fs` and hand it to a real OS to mount, so the on-disk names are judged by someone other than us | **`tools/exfat_mkimage`** | `cmake -S tools/exfat_mkimage -B tools/exfat_mkimage/build && cmake --build tools/exfat_mkimage/build`; `scripts/dev/exfat_macos_interop.sh` drives it end to end on macOS |
| Give an **MCP-aware assistant** live repo context (app catalogue, build/test/HIL workflows, code search, project docs) | **`tools/mcp`** (zero-dependency MCP server) | `make mcp` self-tests it; an MCP client auto-loads it via the repo `.mcp.json` |
| Compare **page-cache eviction policies** for the #147 memory hierarchy (the SLRU decision record) | **`tools/cache_bench`** | `make -C tools/cache_bench run` |
| Measure the **block/frame/chunk size** for the chunked `.rabook` container / `ra8_vmem` `frame_bytes` (#208) | **`tools/cache_bench`** (`--sweep-block`) | `make -C tools/cache_bench sweep` |
| Confirm SLRU on a **real reader workload** by driving the actual `ra8_vmem` + emitting a replayable trace | **`tools/reader_vmem`** | `make -C tools/reader_vmem run` |
| Size the **glyph-cache budget** by sweeping the real `ra8_glyph_atlas` under a text-render workload | **`tools/glyph_bench`** | `make -C tools/glyph_bench run` |
| Convert **one** image to a **`.jof`** tile atlas, or dump any first-party container's structure (header, tile/chunk table, offsets, lengths, validity verdict) when a render looks wrong | **`tools/rabook_imagepack`** | `cmake -S tools/rabook_imagepack -B tools/rabook_imagepack/build && cmake --build tools/rabook_imagepack/build`, then `rabook_imagepack inspect <file> --verbose` |
| Prove a produced container is **not** the cause of a rendering bug (encode -> decode -> compare, byte for byte) | **`tools/rabook_imagepack verify`** | `rabook_imagepack verify --format jof --in page.jpg` |

`make apps` lists every firmware app; `make help` is the grouped target
reference. Git hooks auto-install on first `make` (or `make hooks`).

## `tools/ra8_emulator` -- the board emulator

Boots the **unmodified cross-compiled firmware `.elf`** on an emulated Cortex-M
(Unicorn), models the RA8D2 peripheral space, ticks SysTick, and presents the
real GLCDC output. Highest fidelity -- it exercises the actual bring-up
(clocks/SDRAM/GLCDC) and driver code, so the panel/UI you see is exactly what the
flashed firmware draws. Because it runs the real binary, the firmware renders at
the resolution it was *built* for: pointing a fixed-panel app at a different
`--panel` shows the genuine mismatch, not a magically re-laid-out screen.

Flags: `--view` (live macOS window), `--ppm <file>` (headless frame),
`--panel <file.toml>` / `--size WxH` (model a given display), `--click X Y`
(inject touch through the real GT911 path), `--sd <image>` (attach a FAT
SD-card image to the modelled SD-over-SPI device -- see `tools/mkfontimg`).
Panel descriptors live in `tools/ra8_emulator/panels/<name>.toml`. From the repo
root, `make emu-<app>` builds the app and opens its live window; a chrome app
(e.g. `make emu-ereader_ui`) doubles as the UI preview. Layout: `inc/`
(headers) + `src/` (sources), matching the `libs/` convention. Details:
`tools/ra8_emulator/README.md`.

For heavy headless runs (a large SD font read + glyph rasterisation is slow
under the CPU emulator), the run budget is env-overridable:
`RA8_EMU_WALL_S=<seconds>` and `RA8_EMU_MAX_CHUNKS=<n>` (no effect in
`--view`).

## `tools/mkfontimg` -- FAT SD-card image builder

Writes a font file into a FAT16 image **through the real `ra8_fs`**, so the
on-card layout is byte-for-byte what the firmware reads back (same code path as
`tests/test_ra8_sdmmc_card_reflow.c`). Used to feed `ra8_emulator --sd` for the
`sd_font_render` app, or to format a physical card.

```sh
cmake -S tools/mkfontimg -B tools/mkfontimg/build && cmake --build tools/mkfontimg/build
tools/mkfontimg/build/mkfontimg libs/ra8_fonts/Literata-Regular.ttf font.img FONT.OTF
tools/ra8_emulator/build/ra8_emulator <app>.elf --sd font.img --ppm out.ppm
```

## `#147` memory-hierarchy benchmarking -- `cache_bench` + `reader_vmem` + `glyph_bench`

The decision-record and confirmation toolchain behind the #147/#162 memory
hierarchy. All three are host C programs (no board); each has a `Makefile`
with a `run` target.

- **`tools/cache_bench`** -- replays reader access traces through every
  candidate Layer-2 page-cache eviction policy (FIFO / Random / LRU / CLOCK /
  SLRU / SRRIP) at swept cache sizes and prints a hit-rate matrix + WCET /
  metadata summary. This is the **decision record** that picked SLRU. It also
  loads captured traces: `cache_bench <name>=<path>` (file format: one
  `<object> <page>` per line). Its second mode, `cache_bench --sweep-block`
  (#208), sweeps the block/frame/chunk **size in bytes** (512 B..256 KiB)
  through the REAL `ra8_vmem` + `ra8_vsource` stack -- including an in-memory
  RBKC chunked-`.rabook` backend with real zlib streams, so the measured
  decompress-per-miss knee picks the container chunk size. See
  `tools/cache_bench/README.md` for the methodology, the backend seam the SD
  hardware leg plugs into, and a sample run.

- **`tools/reader_vmem`** -- drives the **actual** firmware Layer-1/Layer-2
  cache (`ra8_vmem` + `ra8_vsource`, not a re-modelled policy) with a reader
  navigation session over a modelled book, and writes the captured
  `ra8_vmem_get` trace for `cache_bench` to replay. It is the **confirmation**
  that SLRU wins on a real workload and that the shipped cache matches the
  benched model:

  ```sh
  make -C tools/reader_vmem run            # writes reader_vmem.trace + prints ra8_vmem stats
  make -C tools/cache_bench && \
    tools/cache_bench/cache_bench reader=tools/reader_vmem/reader_vmem.trace
  ```

- **`tools/glyph_bench`** -- sweeps the **actual** Layer-3 glyph atlas
  (`ra8_glyph_atlas`) under a realistic text-render glyph stream and reports
  hit rate + rasterisations saved versus the cell budget, to size the glyph
  cache for the `ra8_reflow` integration.

## Regression gate

- `scripts/emu/smoke.sh` -- boots each display app on the emulator and
  asserts it runs to its main loop without faulting (no invalid opcode / unmapped
  access, not parked in the panic-halt loop), and for the chrome UI app renders one
  frame and asserts the panel drew rich content.

Run the tool with `--help`/no args for its full flag set.
