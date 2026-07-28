# ereader_rabook

Headless HIL gate that **loads a compiled `.rabook` and renders it** -- the
on-silicon proof of the `ra8_book` pipeline (libs/ra8_book + tools/epub_compile).

## What it does

A small two-chapter demo book is compiled from a tiny EPUB into the `ra8_book`
format and baked (already inflated) into `rabook_fixture.h`. On boot the app:

1. `ra8_book_validate()` -- accepts the flat blob (magic, table bounds, CRC-32).
2. For each chapter, `ra8_book_chapter_to_xhtml()` walks the pre-parsed DOM and
   re-emits XHTML (the bridge into the existing, untouched `ra8_reflow` engine).
3. `ra8_reflow_layout_chapter()` paginates, then every page is rendered into a
   128x160 RGB565 framebuffer and folded into an FNV-1a-32.

The first chapter is short and the second longer, so it renders **small to
large** in one book. Ahem's fixed metrics make it deterministic; the banner on
the SCI8 J-Link OB console is identical every boot and matches ra8_emulator:

```
ereader-rabook-hil: chapters=2 ch0 p=7 crc=60BD27C5 ch1 p=28 crc=26512625 ok
```

`ch0 p=7` vs `ch1 p=28` is the small-to-large pagination.

## Build & run

```sh
make ereader_rabook          # cross-compile -> build/ereader_rabook.elf
make emu-ereader_rabook      # run under ra8_emulator, scrape the banner
```

The gate is `hil.conf` (`HIL_MODE=uart_scrape`, exact banner in `HIL_EXPECT`).

## Regenerating the fixture

`rabook_fixture.h` is generated -- compile any EPUB and bake the inflated blob:

```sh
python3 tools/epub_compile/epub_compile.py demo.epub demo.rabook
# strip the "RBKC" container header + chunk table, zlib-inflate each chunk, emit as a
# const uint8_t[] (see how it was produced in the git history of this dir).
```

## Loading a real / larger book from SD

This gate bakes the blob **inflated** so it needs no decompressor. A real device
reads the compressed `.rabook` off SD and inflates it with the bundled miniz via
`ra8_book_open()`:

```c
/* inflate adapter: the container payload is a zlib stream -> mz_uncompress */
static ra8_err_t app_book_inflate(const void* src, size_t src_len, void* dst,
                                  size_t dst_cap, size_t* out_len) {
  mz_ulong n = (mz_ulong)dst_cap;
  if (mz_uncompress((unsigned char*)dst, &n,
                    (const unsigned char*)src, (mz_ulong)src_len) != MZ_OK) {
    return k_ra8_err_invalid_size;
  }
  *out_len = (size_t)n;
  return k_ra8_ok;
}

/* read BOOK.RBK off SD, inflate into an SDRAM scratch, validate, then walk */
static uint8_t s_sdram_scratch[k_ra8_book_library_max_inflated]; /* place in SDRAM */
const void* base = NULL;
size_t      sz   = 0U;
ra8_book_open(file_bytes, file_len, app_book_inflate,
             s_sdram_scratch, sizeof s_sdram_scratch, &base, &sz);
```

Then list LIBS `ra8_book ra8_reflow ra8_gfx ra8_epub` in CMakeLists.txt (ra8_epub
pulls in miniz), size `s_sdram_scratch` to `k_ra8_book_library_max_inflated`
(from the generated `ra8_book_library.h`), and put the framebuffer + scratch in
SDRAM (`0x68000000`, after `ra8_sdramc_init()`) rather than SRAM.
