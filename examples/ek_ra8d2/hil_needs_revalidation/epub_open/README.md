# epub_open

Opens a real `.epub` off a microSD card and runs the `ra8_epub` parse stack on
it, on the target (issue #114). `epub_parse` (#139) proved the parser runs
on the M85 from a baked in-memory blob; this app closes the storage gap by
reading the book through `ra8_fs` -- the exact path the e-reader uses -- so the
byte-twiddling parse layer meets real SD timing and the FAT read path, not a
`.rodata` array.

## What it does

1. Brings up the Pmod2 microSD over the `ra8_sdmmc_spi` SPI-mode driver and mounts
   an `ra8_fs` volume (formatting it FAT32 first if the card is blank).
2. Self-provisions a known 2-chapter `.epub` (the `seed_two_chapters` seed, baked
   into `epub_fixture.h`) onto the card as `BOOK.EPB` if it is not already there,
   then reads it back.
3. `ra8_epub_open_streamed_fs()` it -- the production streamed open (#230): no
   whole-file buffer, every ZIP read seeks the card on demand -- and asserts:
   - chapter (spine) count == 2,
   - chapter 0's decompressed XHTML CRC-32 == `0xCF23AEEE` (byte-exact for the
     seed -- `zlib.crc32` of `OEBPS/chapter1.xhtml`),
   - Dublin Core metadata parses with a non-empty title.

miniz (ZIP + DEFLATE) and tinyxml2 (XML) run zero-heap through the
`ra8_epub_miniz_alloc` arena + the arena-backed `operator new` (#139); no
allocation reaches the trapped firmware heap.

## The gate is memprobe, not the console

An SD app drives the SCI0 Simple-SPI bus, and board_sim folds every SCI channel
into one console line, so on the simulator the SCI8 banner is interleaved with
SPI traffic. The sibling SD HIL apps (`sd_font_render`, `fs_format_mount`) gate
on SWD globals for the same reason, and so does this one:

- `g_eoh_heartbeat` advances (~10 Hz) ONLY on the success idle loop, reached only
  after every assertion passed; the failure path parks without bumping it.
- `g_eoh_err` stays 0 (each failing stage stamps a non-zero `eoh_err_t` code).
- `g_eoh_chapters` / `g_eoh_crc` latch the parsed results for triage.

A steadily-advancing heartbeat with `g_eoh_err == 0` proves the whole
SD -> `ra8_fs` -> `ra8_epub` pipeline ran and the bytes were correct. The console
banner `epub-hil: chapters=2 ch0_crc=CF23AEEE PASS` is still emitted for a
real-bench scope and human triage.

## Build + flash

From the repo root:

```sh
make epub_open                         # cross-compile -> build/epub_open.elf
make -C examples/epub_open flash       # flash via on-board J-Link OB
```

## Run on the M85 simulator (no hardware)

board_sim has a writable SD model; `--dump-sym` reads result globals after the
run (the headless equivalent of a J-Link memprobe):

```sh
./tools/board_sim/build/board_sim build/epub_open.elf --sd-new 64:fat32 \
    --dump-sym g_eoh_heartbeat --dump-sym g_eoh_err \
    --dump-sym g_eoh_chapters --dump-sym g_eoh_crc
# expect: heartbeat advancing, err 0, chapters 2, crc 0xCF23AEEE
```

`--sd-new 64:fat32` is a blank card (provision path). Persist it with
`--save-sd book.img` then re-run with `--sd book.img` to exercise the
already-present path.

## Pass / fail

| `g_eoh_err` | `g_eoh_heartbeat` | Verdict |
|---|---|---|
| 0 | advancing | Full SD -> parse pipeline passed; CRC byte-exact |
| 1 | frozen | CGC / time / console / SPI bring-up failed |
| 2 | frozen | SD card SPI init failed (card / wiring) |
| 3 | frozen | Mount (and format-if-blank) failed |
| 4 | frozen | Provisioning the `.epub` onto the card failed |
| 5 | frozen | `ra8_epub_open_streamed_fs` rejected the archive |
| 6 | frozen | Spine count != 2 (OPF parse wrong) |
| 7 | frozen | Chapter-0 DEFLATE load failed |
| 8 | frozen | Chapter-0 CRC mismatch (decompressor produced wrong bytes) |
| 9 | frozen | Metadata parse / empty title |

## What this does NOT test

- Rendering / reflow (that is `ereader_ui`); this is open + parse only.
- USB media; the book lives on SD.
- Large / adversarial archives -- the fuzzer corpus covers those on the host;
  this is one fixed known-good book.

## BSP / console

SCI8 async UART console (TXD = PD02, RXD = PD03, 115200-8N1) for the banner;
Pmod2 / J25 SCI0 Simple-SPI for the microSD via `ra8_sdmmc_spi`.

Validated on board_sim (the Unicorn-based M85 simulator): across the blank
(provision) and persisted (already-present) card paths and repeated runs,
`g_eoh_heartbeat` advances with `g_eoh_err == 0`, `g_eoh_chapters == 2`, and
`g_eoh_crc == 0xCF23AEEE`. Real-EK-RA8D2 bench confirmation (Pmod2 microSD) is
the next step; the `hil.conf` already gates it over J-Link.
