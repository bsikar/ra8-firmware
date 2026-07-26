# epub_toc

Runs the `ra8_epub` table-of-contents path on the EK-RA8D2 (issue #116),
against real `.epub` files staged on a microSD card. #74 added titled
TOC parsing -- EPUB2 NCX (`<navMap>`) and EPUB3 `nav.xhtml`
(`<nav epub:type="toc">`) -- but it had only ever run on the x86 host.
This app, building on `epub_open` (#114), exercises both forms plus
the malformed-TOC fallback on silicon.

## What it tests

Three baked books are self-provisioned onto the card (if absent), opened
through `ra8_fs` -> the streamed `ra8_epub_open_streamed_fs` (#230), and asserted:

| File on SD | Form | Asserts |
|---|---|---|
| `TOCNCX.EPB` | EPUB2 NCX | `toc_kind == ncx`, `toc_count == 2`, `crc32(entry0 "Intro") == 0xDBC4EA24`, entry0 -> spine 0 |
| `TOCNAV.EPB` | EPUB3 nav | `toc_kind == nav`, `toc_count == 3`, `crc32(entry0 "Cover") == 0x4CC9A9C1`, entry0 -> spine 0 (the `#fragment` stripped) |
| `TOCBAD.EPB` | no TOC doc | `toc_kind == none`, spine still readable (`chapter_count == 2`) -- graceful degradation, no HardFault |

The entry-label CRCs are byte-exact (`zlib.crc32` of the label string),
so the gate proves the parsed *bytes* are right, not just that nothing
crashed. The fixtures are generated reproducibly and cross-checked
against the host parser before baking.

## Build + flash

```sh
make epub_toc                          # cross-compile -> build/epub_toc.elf
make -C examples/epub_toc flash        # flash via on-board J-Link OB
```

## Run on the M85 simulator (no hardware)

```sh
cmake --build tools/ra8_emulator/build -j
./tools/ra8_emulator/build/ra8_emulator build/epub_toc.elf --sd-new 64:fat32 \
  --dump-sym g_etoc_err --dump-sym g_etoc_ncx_kind --dump-sym g_etoc_nav_kind \
  --dump-sym g_etoc_bad_kind --dump-sym g_etoc_heartbeat
```

`--sd-new 64:fat32` attaches a blank FAT32 card; the app formats/mounts
it, provisions the three books, and parses them. miniz's ZIP 64-bit math
relies on the Armv8.1-M long shifts board_sim emulates (the long-shift
seam added for #139).

## Pass / fail (HIL gate)

The gate is **memprobe**, not the console: an SD app drives the SCI0
Simple-SPI bus and board_sim folds every SCI channel into one console
line, so the SCI8 banner is interleaved with SPI traffic there (the same
reason `epub_open` / `sd_font_render` / `fs_format_mount` gate on SWD
globals). `g_etoc_heartbeat` advances only after all three books pass;
`g_etoc_err` stays 0.

| Memprobe state | Verdict |
|---|---|
| `g_etoc_heartbeat` advancing, `g_etoc_err == 0` | All three TOC paths correct |
| `g_etoc_err == 5` | NCX TOC assertion failed |
| `g_etoc_err == 6` | nav TOC assertion failed |
| `g_etoc_err == 7` | malformed-TOC fallback failed |
| `g_etoc_err == 2/3/4` | SD card / mount / provision failure |
| heartbeat frozen at 0 | early bring-up fault (CGC / console / SPI) |

The success banner `toc-hil: ncx+nav+fallback PASS` is also emitted for a
real-bench scope.

## What this does NOT test

- Deeply nested / multi-level TOC trees (the fixtures are flat).
- Pagination cache (#117) or rendering (#78); this is TOC-parse-only.

## BSP / console

Pmod2 (J25) microSD over `ra8_sdmmc_spi` (SCI0 Simple-SPI); SCI8 async
console (TXD=PD02, RXD=PD03, 115200-8N1) on the J-Link OB virtual COM.

Validated on board_sim (the Unicorn-based M85 simulator): deterministic
across repeated `--sd-new` runs -- `g_etoc_err 0`, ncx (kind 1, n 2, crc
0xDBC4EA24, ch0 0), nav (kind 2, n 3, crc 0x4CC9A9C1, ch0 0), bad (kind
0, chapters 2), heartbeat advancing, no invalid opcode or fault.
Real-EK-RA8D2 bench confirmation is the next step (the `hil.conf` already
gates it over J-Link).
