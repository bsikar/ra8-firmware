# epub_parse

First firmware app to run the `ra8_epub` EPUB parse stack on the target
(issue #139). It bakes a known-good 2-chapter `.epub` (the
`seed_two_chapters` fuzzer-corpus seed) into a C array and parses it
entirely in memory -- no SD card, no USB, no display -- then prints a
deterministic pass banner over the SCI8 J-Link OB console:

```
epub: chapters=2 ch0_crc=CF23AEEE PASS
```

## What it tests

The EPUB parse path is bounded C (vendored miniz + the pure-C XML reader)
that had only ever run on the x86 host. This gate runs it on the
Cortex-M85, exercising the bounded storage paths that the zero-heap
firmware (NASA Rule 3: `_sbrk` traps) uses:

- **`ra8_epub_open()`** drives `mz_zip_reader_init_mem` to read the ZIP
  central directory, then the bounded XML reader parses `META-INF/container.xml` and
  the OPF (spine + Dublin Core metadata).
- **`ra8_epub_load_chapter(0)`** drives `mz_zip_reader_extract_to_mem`,
  i.e. the miniz DEFLATE (tinfl) decompressor, on chapter 0.
- **`ra8_epub_get_metadata()`** reads the parsed Dublin Core fields.

miniz's allocations go through the `ra8_epub_miniz_alloc` static first-fit
arena. XML parsing uses explicit caller-owned workspace and performs no
allocation. Neither path reaches the trapped heap.

The CRC-32 over chapter 0's decompressed XHTML is `0xCF23AEEE`, which is
byte-exact for the baked seed (`zlib.crc32` of `OEBPS/chapter1.xhtml`),
so the banner proves the *bytes* are right, not just that nothing
crashed.

## Build + flash

From the repo root:

```sh
make epub_parse                          # cross-compile -> build/epub_parse.elf
make -C examples/epub_parse flash        # flash via on-board J-Link OB
```

Or standalone, from inside the app directory:

```sh
cd examples/ek_ra8d2/hil_needs_revalidation/epub_parse/
make
make flash
make clean
```

## Run on the M85 emulator (no hardware)

```sh
cmake --build tools/ra8_emulator/build -j
./tools/ra8_emulator/build/ra8_emulator build/epub_parse.elf
```

ra8_emulator runs the firmware on Unicorn's Cortex-M33 core and emulates the
Armv8.1-M instructions the M85 uses but the M33 lacks. miniz's ZIP
central-directory math leans on the 64-bit **long-shift** family
(`LSLL` / `LSRL` / `ASRL`); ra8_emulator's long-shift seam (added for #139)
emulates them, without which the ZIP parse fails with miniz error 9
(`INVALID_HEADER_OR_CORRUPTED`).

## Pass / fail

| What you see (SCI8 @ 115200) | Verdict |
|---|---|
| `epub: chapters=2 ch0_crc=CF23AEEE PASS` | Full parse + decompress are byte-exact |
| `epub: FAIL open` | `ra8_epub_open` rejected the archive (miniz / container / OPF) |
| `epub: FAIL chapters` | Spine count != 2 (OPF spine parse wrong) |
| `epub: FAIL load` | Chapter-0 DEFLATE extract failed |
| `epub: FAIL meta` | Dublin Core metadata parse failed |
| `epub: chapters=2 ch0_crc=<other> PASS` | Decompressor produced wrong bytes (CRC mismatch) |
| Banner never appears, board hangs | HardFault -- most likely a `malloc` reaching `_sbrk` (an un-arena'd allocation) |

## What this does NOT test

- SD-card or USB media (the `.epub` is baked into `.rodata`).
- Rendering / reflow (that is `ereader_ui`); this is parse-only.
- Trustzone / SAU partition (skipped, as in the sibling HIL apps).
- Large / adversarial archives -- the fuzzer corpus covers those on the
  host; this is one fixed known-good book.

## BSP / console

SCI8 async UART, TXD = PD02, RXD = PD03, 115200-8N1, routed to the
on-board J-Link OB virtual COM port.

Validated on ra8_emulator (the Unicorn-based M85 emulator): deterministic
`PASS` with the byte-exact `0xCF23AEEE` CRC across repeated runs, no
invalid opcode or fault. Real-EK-RA8D2 bench confirmation is the next
step (flash + scrape the SCI8 banner; the `hil.conf` already gates it).
