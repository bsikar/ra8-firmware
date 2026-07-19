# RCBZ -- The Per-Page Comic Container

**Magic:** `RCBZ` &nbsp;|&nbsp;
**Library:** `libs/ra8_book` &nbsp;|&nbsp;
**Extension:** `.rcbz` &nbsp;|&nbsp;
**Producer:** `tools/epub_compile/cbz_container.py`

---

## 1. Synopsis

RCBZ is the comic-book cousin of @ref md_docs_2formats_2RBKC, and it exists
because of one observation about CBZ files that makes them **easier** to page
than a book, not harder.

A CBZ is just a ZIP of page images -- `p01.png`, `p02.jpg`, and so on. Unlike a
book blob, there is no single archive-wide compression stream to chunk: **ZIP
already compresses every entry independently.** The natural unit of random
access is therefore already present in the source. It is not a 64 KiB slice of
some flat byte space; it is *a page*.

RCBZ is the on-device half of that observation. The host tool walks the source
CBZ's central directory once, and for each page it:

1. decodes the image (PNG or JPEG) fully, on the host,
2. **pre-decodes it to the panel-native 4 bpp grayscale raster** the display
   actually wants,
3. independently zlib-compresses that raster,
4. records the page's compressed extent plus its decode metadata.

Point 2 is the one that matters most. The device never runs a PNG or JPEG
decoder for comics. It inflates a chunk of zlib and hands the bytes to the
display more or less directly, because they are already in the panel's format.
All the codec risk, all the unbounded parsing, all the colour conversion, has
been moved off the device permanently.

One `ra8_cbz_page_read` call is **one SD read burst plus one inflate**, so a
multi-gigabyte omnibus is read with a working set of a few pages and is never
resident in full.

If you stop reading here: *RCBZ is a page-indexed container of independently
zlib-compressed, pre-decoded 4 bpp rasters plus a per-page manifest -- so the
device can jump to any page with one bounded read and one bounded inflate, and
never decodes an image codec at runtime.*

---

## 2. Design rationale

### The comparison that makes it click

Three ways to show page 400 of a 900-page comic:

@dot
digraph cbz_ways {
  bgcolor="transparent";
  rankdir=TB;
  fontname="Helvetica";
  node [shape=box, fontname="Helvetica", fontsize=10, style="filled,rounded"];
  edge [fontname="Helvetica", fontsize=9];

  subgraph cluster_zip {
    label="A: read the CBZ on device -- works, but drags a codec along";
    fontsize=11; fontname="Helvetica-Bold";
    color="#b8913f"; style="rounded"; bgcolor="#fbf6ea";
    z0 [label="parse ZIP central directory\n(variable-length records,\nscan from the end)", fillcolor="#f5e6c8", color="#b8913f"];
    z1 [label="inflate entry 400", fillcolor="#f5e6c8", color="#b8913f"];
    z2 [label="run a PNG/JPEG decoder\non untrusted data,\nunbounded work + RAM", fillcolor="#efd9a8", color="#b8913f", width=4];
    z3 [label="convert colour -> 4bpp gray", fillcolor="#f5e6c8", color="#b8913f"];
    z4 [label="pixels", fillcolor="#d6efd9", color="#5f9e72"];
    z0 -> z1 -> z2 -> z3 -> z4;
  }

  subgraph cluster_uniform {
    label="B: treat it as a flat blob and chunk it uniformly (the RBKC approach)";
    fontsize=11; fontname="Helvetica-Bold";
    color="#b06a6a"; style="rounded"; bgcolor="#fbeeee";
    u0 [label="page 400 spans chunks 87..93\n-- page and chunk boundaries\ndo not line up", fillcolor="#e8c9c9", color="#b06a6a", width=4];
    u1 [label="7 reads + 7 inflates\nfor one page", fillcolor="#e8c9c9", color="#b06a6a"];
    u2 [label="pixels", fillcolor="#d6efd9", color="#5f9e72"];
    u0 -> u1 -> u2;
  }

  subgraph cluster_rcbz {
    label="C: RCBZ -- index by the unit the reader actually asks for";
    fontsize=11; fontname="Helvetica-Bold";
    color="#5f9e72"; style="rounded"; bgcolor="#eef7f0";
    r0 [label="offset[400], offset[401]\n-- fixed-width table,\ndirect index", fillcolor="#dff0e4", color="#5f9e72"];
    r1 [label="1 read + 1 inflate", fillcolor="#dff0e4", color="#5f9e72"];
    r2 [label="pixels -- already 4bpp,\nno codec, no conversion", fillcolor="#d6efd9", color="#5f9e72"];
    r0 -> r1 -> r2;
  }
}
@enddot

Path B is the interesting comparison, because it is what you get if you reach
for the book container out of habit. Uniform chunking is the right answer when
the underlying object is *one flat byte space with no natural seams*. A comic is
the opposite: it is a sequence of independent objects of wildly varying size.
Forcing a uniform grid over it means page boundaries and chunk boundaries never
line up, so a single page costs several reads and several inflates, and the
cache holds fragments of neighbouring pages you did not ask for.

**Index by the unit the caller actually requests.** For a book that is a byte
range; for a comic it is a page. Same paging machinery, different key.

### Why pre-decode to 4 bpp on the host

Two independent arguments, either of which would be sufficient:

- **Security.** Image decoders are historically the richest source of
  memory-safety bugs in any reader application. A malicious JPEG is a real
  attack; a malicious *raster of known length* is not much of one. Moving the
  decoder to the host removes the entire category from the device.
- **Bounded work.** A JPEG's decode cost and memory use are properties of the
  file, discovered while decoding. A pre-decoded raster's cost is
  `width * height / 2` bytes, known from the manifest *before* reading anything.
  That is what makes the working set statically budgetable.

The panel is 4 bpp grayscale (16 levels), so a pre-decoded page is exactly
`w * h / 2` bytes -- two pixels per byte. Section 6 shows a 120 x 160 page
storing `raw_size = 9600 = 120 * 160 / 2`, which is the cheapest possible
confirmation that the transcode really did land in the panel's format.

### Why a separate metadata table

Offsets alone would let the reader *fetch* a page but not *use* it: it would not
know the page's dimensions or how to interpret the bytes. So each page carries a
12-byte metadata record alongside its offset -- `raw_size`, `width`, `height`,
`format`.

Keeping these in a **separate parallel table** rather than interleaving them
with the compressed streams means the reader can load the entire manifest at
open with two contiguous reads, then answer "how big is page 400?" with no I/O
at all. That is what lets a UI show a page-count, a scrollbar, or a
pan/zoom viewport's extents without touching the payload.

### Where RCBZ deliberately stops

A page is served **whole**. If a page's raster is larger than one cache frame --
a full-resolution manga page under a zoom loupe -- slicing it further is
`ra8_tile_cache`'s job (via @ref md_docs_2formats_2JOF), not this container's.
RCBZ exposes each page's dimensions precisely so a tiler can slice a page this
reader hands back. Two formats, one clean seam, neither duplicating the other.

---

## 3. Wire format

All integers are **little-endian**. Offsets in the page table are relative to
the **start of the payload**.

```
  +--------------------------------------------------+  offset 0
  |  header                              16 bytes    |
  +--------------------------------------------------+  offset 16
  |  page table    (page_count + 1) x 8 bytes        |
  +--------------------------------------------------+  meta_off
  |  meta table    page_count x 12 bytes             |
  +--------------------------------------------------+  payload_off
  |  page 0 zlib stream                              |
  |  page 1 zlib stream                              |   concatenated
  |  ...                                             |
  +--------------------------------------------------+  end of file
```

with

```
  meta_off    = 16 + 8 * (page_count + 1)
  payload_off = meta_off + 12 * page_count
```

### 3.1 Header (16 bytes, at offset 0)

| Offset | Size | Field | Meaning and valid range |
|--------|------|-------|-------------------------|
| 0 | 4 | `magic` | The bytes `52 43 42 5a` (`"RCBZ"`). Compared with `memcmp`. |
| 4 | 4 | `page_count` | Number of pages. Must be `> 0`. |
| 8 | 4 | `flags` | `ra8_book_flag_t` bits, e.g. `k_ra8_book_flag_rtl` for right-to-left manga order. |
| 12 | 4 | `reserved` | Must be `0`. |

Record sizes are pinned by `ra8_cbz_layout_t`.

`flags` is shared with the book format on purpose: reading direction is a
property of the *work*, not of the container, so a right-to-left comic and a
right-to-left book set the identical bit and the UI has one code path.

### 3.2 Page table

`page_count + 1` little-endian `uint64` payload-relative offsets. Page *i*
occupies

```
  [ payload_off + offset[i], payload_off + offset[i+1] )
```

Same end-sentinel design as RBKC: one extra entry, lengths are subtractions
rather than stored fields, so a length can never contradict the next offset.

**Invariants:** `offset[0] == 0`; strictly increasing;
`offset[page_count] == file_len - payload_off`.

### 3.3 Metadata table (12 bytes per page)

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 4 | `raw_size` | Inflated raster length in bytes. Page *i*'s stream must inflate to exactly this. |
| 4 | 2 | `width` | Page width in pixels |
| 6 | 2 | `height` | Page height in pixels |
| 8 | 1 | `format` | `ra8_book_image_format_t`; `0` = 4 bpp grayscale |
| 9 | 3 | `reserved` | Must be `0` |

For the 4 bpp format, `raw_size` must equal `width * height / 2`. The reader
checks this rather than trusting `raw_size`, so a manifest cannot describe a
buffer larger than its own geometry justifies.

### 3.4 Page streams

Each page is one standalone **zlib (RFC 1950)** stream -- `78 xx` header,
DEFLATE body, Adler-32 trailer -- inflating to exactly `meta[i].raw_size` bytes
of pre-decoded raster. No page references any other page's state.

---

## 4. Algorithms

### 4.1 Producing (`cbz_container.py`)

@dot
digraph rcbz_produce {
  bgcolor="transparent"; rankdir=TB;
  node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
        fillcolor="#e8eef7", color="#5a7ca6"];
  edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
  a [label="1. Walk the source CBZ central directory once\n   -> page order + entry list"];
  b [label="2. For each page:\n   decode PNG/JPEG fully (host)\n   transcode to 4bpp panel raster\n   zlib-compress independently\n   record offset + (raw_size, w, h, format)"];
  c [label="3. page_count known from step 1,\n   so both table sizes are known"];
  d [label="4. Write header, page table, meta table,\n   then the concatenated payload"];
  a -> b -> c -> d;
  b -> b [label="next page"];
}
@enddot

Because `page_count` is known from the central directory before any page is
compressed, both table sizes are known up front -- so, exactly as with RBKC, the
tables go at the **front** and the file is written in one forward pass.

### 4.2 Opening (`ra8_cbz_open()`)

1. Read and validate the 16-byte header: magic, `page_count > 0`, `reserved == 0`.
2. Load `page_count + 1` offsets into the caller's buffer (capacity checked).
3. Validate the offset table: `offset[0] == 0`, strictly increasing,
   `offset[page_count]` equal to the payload length implied by `file_len`.
4. Load the `page_count x 12` metadata records.
5. Validate each record: `format` known, `raw_size` consistent with
   `width * height` for that format, and every page's compressed length within
   `staging_cap`.

After open, every question about *any* page -- its size, its dimensions, whether
it fits a frame -- is answerable with zero I/O.

### 4.3 Reading a page (`ra8_cbz_page_bind()` + `ra8_cbz_page_read()`)

`ra8_cbz_page_bind()` makes a cursor for one page; `ra8_cbz_page_read()` has the
`ra8_vsource_read_fn` signature and serves that page's raster byte space. Reads
are **page-granular by contract**: `offset` must be `0` and `len` must equal the
page's `raw_size`.

```
  1. Range-check page_idx < page_count.
  2. Require offset == 0 and len == meta[page_idx].raw_size.
  3. comp_len = offset[i+1] - offset[i]
  4. Read comp_len bytes from payload_off + offset[i] into staging.
  5. Inflate staging directly into the caller's frame.
  6. Require the inflated byte count to equal raw_size exactly.
```

As with RBKC, the strict contract is not a burden on the caller: it is exactly
what `ra8_vsource_loader` passes for a per-page object whose `frame_bytes` is at
least the page's raster length.

---

## 5. Memory behaviour

Resident cost of an open comic:

| Buffer | Size | Notes |
|--------|------|-------|
| offset table | `(page_count + 1) * 8` | 900 pages -> 7.2 KB |
| metadata table | `page_count * 12` | 900 pages -> 10.8 KB |
| staging | >= largest compressed page | one page |
| cache frame(s) | >= `raw_size` of the page in view | one or a few pages |
| `ra8_cbz_t` | ~80 bytes | |

A 900-page comic at 1072 x 1448 (a typical 6-inch panel page) has
`raw_size = 1072 * 1448 / 2` = 776 KB per page. Manifest overhead is 18 KB for
the entire book; the dominant term is however many pages you choose to keep
decoded. Reading page-by-page with two frames resident costs roughly **1.6 MB**,
which is why full-size comic pages live in SDRAM rather than SRAM -- but the
cost is a function of *frames held*, chosen by the caller, and never of the
number of pages in the file.

Compare the two containers directly:

| | RBKC | RCBZ |
|---|---|---|
| Unit of access | uniform `chunk_bytes` slice | one page |
| Unit size | fixed | varies per page |
| Sizes known before read? | from the header formula | from the metadata table |
| Table overhead | 8 B per chunk | 20 B per page (8 offset + 12 meta) |
| Payload | zlib streams | zlib streams |
| Device runs an image codec? | n/a | **no** -- pre-decoded on host |

Zero allocation throughout (NASA P10 Rule 3): the caller supplies the offset,
metadata and staging buffers at open.

---

## 6. Worked example -- real bytes

Reproducible end to end. Build a three-page CBZ of synthetic pages, compile it,
and inspect the result:

```
python3 - <<'EOF'
import struct, zlib, zipfile
def png(W, H, f):
    px = bytearray()
    for y in range(H):
        px.append(0)                       # filter: none
        for x in range(W):
            px.append(f(x, y))
    def ch(t, d):
        b = t + d
        return struct.pack('>I', len(d)) + b + struct.pack('>I', zlib.crc32(b))
    return (b'\x89PNG\r\n\x1a\n'
            + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0))
            + ch(b'IDAT', zlib.compress(bytes(px), 9))
            + ch(b'IEND', b''))
z = zipfile.ZipFile('pages.cbz', 'w', zipfile.ZIP_DEFLATED)
z.writestr('p01.png', png(120, 160, lambda x, y: 32 if (x//10 + y//10) % 2 else 224))
z.writestr('p02.png', png(120, 160, lambda x, y: (x*2) % 256))
z.writestr('p03.png', png(120, 160, lambda x, y: 255 if 20 < x < 100 and 30 < y < 130 else 0))
z.close()
EOF
```

```
$ python3 tools/epub_compile/cbz_container.py pages.cbz pages.rcbz --stats
p01.png .. 3 pages [ltr]
  cbz=0 KB -> rcbz=0 KB (37%); inflated rasters=28 KB

$ ./build/ra8_fmt/ra8_fmt inspect pages.rcbz --verbose
RCBZ comic container: 298 bytes
  page_count : 3
  flags      : 0x00000000
  reserved   : 0
   page   raw_size    width   height  format
      0       9600      120      160       0
      1       9600      120      160       0
      2       9600      120      160       0
  entry     start        end       length
      0         84        156         72
      1        156        246         90
      2        246        298         52
verdict: VALID (page table monotonic and complete)
```

### 6.1 Header and tables, byte by byte

```
00000000  52 43 42 5A 03 00 00 00 00 00 00 00 00 00 00 00 |RCBZ............|
00000010  00 00 00 00 00 00 00 00 48 00 00 00 00 00 00 00 |........H.......|
00000020  A2 00 00 00 00 00 00 00 D6 00 00 00 00 00 00 00 |................|
00000030  80 25 00 00 78 00 A0 00 00 00 00 00 80 25 00 00 |.%..x........%..|
00000040  78 00 A0 00 00 00 00 00 80 25 00 00 78 00 A0 00 |x........%..x...|
00000050  00 00 00 00 78 DA ED D4 31 11 00 40 08 03 41 2F |....x...1..@..A/|
```

**Header** (offsets 0-15):

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 0-3 | `52 43 42 5A` | `magic` | `"RCBZ"` |
| 4-7 | `03 00 00 00` | `page_count` | **3** |
| 8-11 | `00 00 00 00` | `flags` | `0` -- left-to-right (no `k_ra8_book_flag_rtl`) |
| 12-15 | `00 00 00 00` | `reserved` | zero, as required |

**Page table** (offsets 16-47, four `uint64` entries):

| Bytes | Value | Entry |
|-------|-------|-------|
| 16-23 | `0` | `offset[0]` -- required to be 0 |
| 24-31 | `0x48` = **72** | `offset[1]` |
| 32-39 | `0xA2` = **162** | `offset[2]` |
| 40-47 | `0xD6` = **214** | `offset[3]` -- end sentinel |

**Metadata table** (offsets 48-83, three 12-byte records). The first record:

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 48-51 | `80 25 00 00` | `raw_size` | `0x2580` = **9600** |
| 52-53 | `78 00` | `width` | `0x78` = **120** |
| 54-55 | `A0 00` | `height` | `0xA0` = **160** |
| 56 | `00` | `format` | `0` = 4 bpp grayscale |
| 57-59 | `00 00 00` | `reserved` | zero |

### 6.2 The checks, done by hand

**Table origins.** `meta_off = 16 + 8 * (3 + 1) = 48` -- exactly where the first
metadata record sits. `payload_off = 48 + 12 * 3 = 84` -- exactly where `inspect`
says page 0 starts.

**Closure.** `payload_off + offset[3] = 84 + 214 = 298` = the file length. The
tables close the file precisely.

**Lengths are subtractions.**

| Page | `offset[i]` | `offset[i+1]` | Compressed | Inflates to |
|------|-------------|---------------|------------|-------------|
| 0 | 0 | 72 | **72** | 9600 |
| 1 | 72 | 162 | **90** | 9600 |
| 2 | 162 | 214 | **52** | 9600 |

The three pages compress to 72, 90 and 52 bytes -- *wildly* different, from
identical uncompressed sizes, purely because their content differs. That
variance is precisely why a uniform chunk grid is the wrong tool here.

**The 4 bpp proof.** `width * height / 2 = 120 * 160 / 2 = 9600 = raw_size`.
Exactly. The page really is stored as two pixels per byte in the panel's native
format -- no colour, no codec, nothing left for the device to interpret.

**Payload codec.** The first page's stream begins at offset 84 with `78 DA` --
a zlib (RFC 1950) header, matching RBKC and differing from a JOF tile's raw
DEFLATE.

---

## 7. Edge cases, failure modes and security

| Malformed input | What could go wrong | What actually happens |
|-----------------|---------------------|-----------------------|
| Wrong `RCBZ` magic | Misparse as another format | Rejected at the first check |
| `page_count == 0` | Empty tables, zero-length sentinel arithmetic | Must be `> 0` |
| `page_count` huge | Table read far past the file | Table capacity checked against the caller's buffer before loading |
| `offset[0] != 0` | First page read from the wrong place | Required to be 0 |
| Non-monotonic offsets | Unsigned wrap producing a huge length | Strictly-increasing check at open |
| `offset[page_count]` != payload length | Truncated or padded file | Must match exactly |
| A page longer than `staging_cap` | Staging overflow | Every page's compressed length checked **at open** |
| `raw_size` != `width * height / 2` | Frame under- or over-filled; uninitialised pixels shown | Cross-checked against geometry per record |
| `width` or `height` = 0 | Zero-size raster, degenerate arithmetic | Rejected by the geometry cross-check |
| Unknown `format` | Bytes interpreted with the wrong bit depth | `format` must be a known `ra8_book_image_format_t` |
| Non-zero reserved bytes | A future field silently reinterpreted | Required to be zero |
| `offset != 0` or `len != raw_size` in a read | Partial page fill | Contract requires both; violation rejected |
| **Decompression bomb** | Tiny stream inflating to gigabytes | Expected size known from validated geometry before inflating; plus `ra8_decomp_limits_t` (64 MiB / 1024:1) |
| Page inflating to the wrong size | Stale or foreign pixels rendered | Inflated count must equal `raw_size` exactly |
| Bit rot in a page stream | Corrupt page rendered as valid | zlib Adler-32 fails the inflate |
| Hostile source image (huge JPEG bomb) | Decoder blow-up | **Not reachable on device** -- decoding happened on the host at import; the device only ever sees a fixed-size raster |

That last row is the security headline. The most dangerous input in the whole
comic pipeline -- an arbitrary attacker-supplied JPEG -- is handled by a host
tool that can afford to fail, and never reaches the firmware.

### What is deliberately *not* defended

- **Semantic garbage.** A valid container of noise renders noise.
- **Authenticity.** Adler-32 detects bit rot, not tampering. An `.rcbz` is parsed
  safely but is not authenticated; if it must be trusted, wrap it in something
  signed (see @ref md_docs_2formats_2ROT1).
- **Page-order sanity.** The container preserves whatever order the source CBZ's
  central directory implied. A mis-sorted comic is a producer-side problem.

---

## 8. Versioning

`RCBZ` uses the discriminator-byte scheme from
@ref md_docs_2formats_2BINARY__FORMATS. The fourth byte `Z` marks the family
(*comic/CBZ-derived*) rather than a revision number, exactly as `RBKC`'s `C`
marks the chunked variant. There is no separate `version` field: the container
is small and stable, and the content it carries -- a raster plus dimensions --
has no independent structure to revise.

Extension points, in order of preference:

- **`format` values.** The metadata record's `format` byte is the intended
  growth path. Adding, say, an 8 bpp grayscale or a 1 bpp mode means spending a
  new `ra8_book_image_format_t` value. An old reader rejects the unknown value
  cleanly instead of misinterpreting the bit depth -- which is exactly what you
  want, since a wrong bit depth would render a plausible-looking but incorrect
  page.
- **`flags` bits.** Shared with the book format and bound by the same rule: a
  flag may only change how content is *presented*, never where a table lives.
- **`reserved` fields.** Four header bytes and three per-record bytes, all
  required to be zero today, so an old reader refuses a file that sets them.
- **A new magic.** Any change to the table layout itself spends a new fourth
  byte; every existing reader `memcmp`s and rejects it with no code change.

Under the zero-backward-compatibility policy, a breaking change updates
`cbz_container.py` and `ra8_cbz_container.c` in the same commit and deletes the
old handling. There is no dual-version reader.

---

## See also

- `ra8_cbz_container.h` -- the reader specified here
- `ra8_book.h` -- `ra8_book_flag_t` / `ra8_book_image_format_t`, shared
- `ra8_vsource.h` -- the paging registry `ra8_cbz_page_read` plugs into
- @ref md_docs_2formats_2RBKC -- the uniform-chunk flat-blob container this
  mirrors, and the direct comparison for *why* page-indexing wins here
- @ref md_docs_2formats_2JOF -- sub-page tiling, for when a single page is
  larger than a cache frame
