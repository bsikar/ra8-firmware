# RBKC -- The Chunked `.rabook` Container

**Magic:** `RBKC` &nbsp;|&nbsp;
**Library:** `libs/ra8_book` &nbsp;|&nbsp;
**Extension:** `.rabook` &nbsp;|&nbsp;
**Producer:** `tools/epub_compile/epub_compile.py`

---

## 1. Synopsis

RBKC is how a whole book gets onto the device in a form that can be read
**without ever being resident in full**.

An EPUB is a ZIP of XHTML, CSS, fonts and images. Parsing that on a
microcontroller -- unzipping, tokenising XML, resolving a CSS cascade -- is a
large, unbounded, allocation-hungry job, and it is the same job every single
time the book is opened. So the device does not do it. `epub_compile.py` does it
once on the host and emits a **flat blob**: a single contiguous byte image with
fixed-width tables (nodes, attributes, chapters, stylesheets) and string pools,
where every cross-reference is an integer index rather than a pointer. That blob
is designed to be *used in place* -- the renderer reads a node by indexing into
it, with no parsing at all.

That solves parsing but creates a new problem: the blob is big, and it is one
object. The Walden fixture inflates to 67 KB; a real illustrated book runs to
many megabytes, well past the 1.6 MB of SRAM. Compressing it as one zlib stream
would make it small on disk but force a whole-blob inflate to use any of it.

RBKC is the wrapper that fixes exactly that, and nothing else. It slices the
flat blob into **fixed-size chunks, each independently zlib-compressed**, and
records where each chunk's stream begins in a table at the front. The reader
inflates **one chunk at a time**, on demand, into a page-cache frame.

The number that makes it click: chunks are produced at exactly the size of the
reader's `ra8_vmem` cache frame, so

> **one chunk == one cache frame == one inflate call.**

There is no double-buffering, no second decompressed-chunk cache, and no
partial-chunk arithmetic. A page fault in the book's byte space maps to exactly
one bounded read plus one bounded inflate.

If you stop reading here: *RBKC is a demand-paging transport wrapped around a
pre-parsed flat book blob -- uniform independently-compressed chunks plus an
offset table, sized so one chunk fills one page-cache frame, letting a book far
larger than RAM be read with a bounded working set.*

---

## 2. Design rationale

### Two layers, and why they are separate

The single most important thing to understand about a `.rabook` file is that
there are **two formats stacked**, and they have different jobs:

```
  .rabook file
  +--------------------------------------------------+
  |  RBKC container   <- TRANSPORT: compression +     |
  |                      chunking + demand paging     |
  |  +--------------------------------------------+  |
  |  |  RABOOK1 flat blob                         |  |
  |  |  <- CONTENT: node/attr/chapter tables,     |  |
  |  |     string pools, index-based references   |  |
  |  +--------------------------------------------+  |
  +--------------------------------------------------+
```

The inner blob starts with the ASCII magic `RABOOK1` and is described by
`ra8_book_header_t`. The outer container knows *nothing* about books -- it sees
an opaque byte range it must be able to serve any window of. That separation is
what lets the outer container serve any opaque inner payload, and it is why this
page specifies only the outer layer. It is also why a `.rabook` needs this outer
chunking at all: unlike a ZIP (whose entries are already independently
compressed and individually seekable, so a comic archive needs no
whole-archive chunking), the inner `RABOOK1` blob is a single contiguous stream,
so the seekable boundaries have to be imposed around it.

You can see both layers in a hexdump, which is the fastest way to convince
yourself: the file begins `52 42 4B 43` (`RBKC`), and the first chunk's zlib
stream inflates to bytes beginning `52 41 42 4F 4F 4B 31` (`RABOOK1`). Section 6
shows exactly that.

### The comparison that makes it click

Consider reading a few hundred bytes near the *end* of a 4 MB book blob -- a
node the renderer needs for the chapter the reader jumped to.

@dot
digraph whole_vs_chunked {
  bgcolor="transparent";
  rankdir=TB;
  fontname="Helvetica";
  node [shape=box, fontname="Helvetica", fontsize=10, style="filled,rounded"];
  edge [fontname="Helvetica", fontsize=9];

  subgraph cluster_whole {
    label="One zlib stream over the whole blob: any byte costs the whole book";
    fontsize=11; fontname="Helvetica-Bold";
    color="#b06a6a"; style="rounded"; bgcolor="#fbeeee";
    w0 [label="open file", fillcolor="#f0dada", color="#b06a6a"];
    w1 [label="one DEFLATE stream over ALL 4 MB\n(the sliding window carries state\nfrom byte 0 forward)",
        fillcolor="#e8c9c9", color="#b06a6a", width=5];
    w2 [label="inflate ~4 MB to reach the tail\n-> needs 4 MB resident, or a\nthrow-away scan every seek",
        fillcolor="#e8c9c9", color="#b06a6a"];
    w3 [label="the 200 bytes you wanted", fillcolor="#d6efd9", color="#5f9e72"];
    w0 -> w1 -> w2 -> w3;
  }

  subgraph cluster_chunked {
    label="RBKC: chunk index = offset / chunk_bytes, then ONE inflate";
    fontsize=11; fontname="Helvetica-Bold";
    color="#5f9e72"; style="rounded"; bgcolor="#eef7f0";
    c0 [label="chunk table is already resident\n(8 bytes per chunk)", fillcolor="#dff0e4", color="#5f9e72"];
    c1 [label="i = offset / chunk_bytes\n(a divide -- no search)", fillcolor="#dff0e4", color="#5f9e72"];
    c2 [label="read table[i]..table[i+1],\ninflate ONE chunk\ninto one cache frame", fillcolor="#dff0e4", color="#5f9e72"];
    c3 [label="the 200 bytes you wanted", fillcolor="#d6efd9", color="#5f9e72"];
    c0 -> c1 [label="O(1)"];
    c1 -> c2 [label="1 read"];
    c2 -> c3 [label="1 inflate"];
  }
}
@enddot

DEFLATE's compression comes from a sliding window over everything already seen,
so a single stream is inherently sequential: byte *N* cannot be produced without
having produced every byte before it. Chunking **deliberately discards** that
cross-chunk history. Each chunk restarts the window, which costs a few percent
of compression ratio, and buys random access. That trade -- *a little ratio for
O(1) seek* -- is the entire design.

### Why the chunk table is at the front (and JOF's index is at the back)

This looks like an inconsistency with @ref md_docs_2formats_2JOF, which puts its
index at the *end*. It is not: the two producers have different constraints.

| | JOF | RBKC |
|---|---|---|
| Producer knows the total size up front? | No -- depends on how each tile compresses | **Yes** -- `chunk_count` is `ceil(inflated_total / chunk_bytes)`, known before compressing anything |
| Therefore | index must trail the data | table size is known, so it can be reserved at the front and filled in |
| Reader benefit | reads a fixed-size footer from the end | reads the header and table in **one contiguous front-of-file read** |

Because `inflated_total` and `chunk_bytes` are both decided before compression
starts, the producer knows exactly how many table entries it needs and can
reserve the space. Putting the table first then lets the reader slurp the header
and the whole table in one sequential read from offset 0 -- which is the cheapest
possible access pattern on an SD card, where seeks cost far more than sequential
bytes.

### Why zlib here and raw DEFLATE in JOF

RBKC chunks are **zlib (RFC 1950)** streams -- they begin `78 xx` and carry a
trailing Adler-32 checksum. JOF tiles are **raw DEFLATE (RFC 1951)** with no
wrapper. The difference is a deliberate 6-byte-per-unit trade:

- A JOF atlas can hold thousands of small tiles, and it already validates every
  tile by requiring the inflate to produce an exact byte count. A per-tile
  checksum would add overhead on every tile to re-detect what the size check
  already catches.
- An RBKC chunk is 64 KiB, so 6 bytes is negligible, and the Adler-32 gives a
  genuine **integrity** check on data that came off removable media and may have
  been sitting there for months. A silently bit-rotted chunk fails the checksum
  rather than rendering as corrupt text.

### Why `uint64` offsets

The chunk table stores `uint64` offsets and `inflated_total` is `uint64`, where
JOF uses `uint32` throughout. JOF is bounded by its geometry caps to well under
4 GiB. A book is not bounded that way -- an omnibus with full-resolution art can
exceed 4 GiB inflated, and a format that silently wraps at that boundary would
be a latent corruption bug. The table costs 8 bytes per 64 KiB chunk, which is
0.012% overhead; paying it to remove a whole class of overflow is trivially
worth it.

---

## 3. Wire format

All integers are **little-endian**. Offsets in the chunk table are relative to
the **start of the payload**, not to byte 0 of the file.

```
  +--------------------------------------------------+  offset 0
  |  header                              24 bytes    |
  +--------------------------------------------------+  offset 24
  |  chunk table   (chunk_count + 1) x 8 bytes       |
  +--------------------------------------------------+  payload_off
  |  chunk 0 zlib stream                             |
  |  chunk 1 zlib stream                             |   concatenated,
  |  ...                                             |   back to back
  |  chunk N-1 zlib stream                           |
  +--------------------------------------------------+  end of file
```

with

```
  payload_off = 24 + 8 * (chunk_count + 1)
```

### 3.1 Header (24 bytes, at offset 0)

| Offset | Size | Field | Meaning and valid range |
|--------|------|-------|-------------------------|
| 0 | 4 | `magic` | The bytes `52 42 4b 43` (`"RBKC"`). Compared with `memcmp`. |
| 4 | 4 | `chunk_bytes` | Inflated bytes per chunk; the last chunk is short. Must be non-zero. Equals the reader's `ra8_vmem` frame size. |
| 8 | 8 | `inflated_total` | Total length of the flat blob this container serves. |
| 16 | 4 | `chunk_count` | Must equal `ceil(inflated_total / chunk_bytes)`. |
| 20 | 4 | `reserved` | Must be `0`. |

The record sizes are pinned by `ra8_book_container_t`:
`k_ra8_book_container_header_len = 24`, `k_ra8_book_container_entry_len = 8`,
`k_ra8_book_container_magic_len = 4`.

### 3.2 Chunk table

`chunk_count + 1` little-endian `uint64` entries, immediately after the header.

Entry *i* is the **payload-relative** byte offset where chunk *i*'s zlib stream
begins. Chunk *i* therefore occupies

```
  [ payload_off + offset[i], payload_off + offset[i+1] )
```

which is why there is one more entry than there are chunks: the final entry is
the end sentinel, and every chunk's length is a subtraction rather than a stored
field. A length can disagree with the next offset; a subtraction cannot.

**Invariants**, all enforced at open:

- `offset[0] == 0`
- strictly increasing (so every chunk has non-zero length)
- `offset[chunk_count] == file_len - payload_off` (the table closes the file)
- every chunk's length fits the caller's `staging_cap`

### 3.3 Chunk streams

Each chunk is one standalone **zlib (RFC 1950)** stream -- `78 xx` header,
DEFLATE body, Adler-32 trailer -- that inflates to exactly

```
  min(chunk_bytes, inflated_total - i * chunk_bytes)
```

bytes. Only the final chunk is ever short. No chunk references any other
chunk's compression state; that independence is the property the whole format
exists to provide.

### 3.4 The inner blob

The concatenation of every inflated chunk is the `RABOOK1` flat blob described
by `ra8_book_header_t` -- an 8-byte magic `"RABOOK1"` (7 chars plus NUL), a
`format_version` (currently `k_ra8_book_format_version` = 1), a `flags` word of
`ra8_book_flag_t` bits, and then the tables and pools. The container neither
inspects nor validates any of it; `ra8_book_validate()` does that after the
bytes are available.

### 3.5 Image pool: gray4, or full-resolution gray8 for zoomable content

The image pool holds one payload per `ra8_book_image_t`. An SVG entry keeps its
verbatim vector source. A raster entry (`format = k_ra8_book_image_gray4`) stores
grayscale pixels at **source resolution** -- no downscale by default; the long-edge
clamp is an opt-in compile knob -- at a depth chosen by the descriptor's
`pixel_format` (`ra8_book_image_pixfmt_t`):

- **`gray4`** (2 pixels/byte): panel-native for the 16-level e-ink target, half the
  storage. Correct for content that is *never* zoomed, but the 256 -> 16 quantise
  is destructive -- the discarded tones cannot be recovered.
- **`gray8`** (1 byte/pixel): the **full-resolution, continuous-tone** source, kept
  verbatim. This is the representation the compiler retains for any image that may
  be zoomed (issue #476), because both the tap-to-zoom loupe (#478) and the
  blue-noise e-ink dither (#477) need the 256-level source that `gray4` throws away.

**Why full-resolution gray8 and not an embedded tiled JOF atlas.** The live-EPUB
image path (`ra8_jof`) tiles each image into a jump-offset atlas because it reads
from a ZIP entry with no random access, and a page can exceed the SDRAM working
set. The compiled path does not have that problem: the RBKC container is *already*
a random-access tiling -- any chunk inflates independently into one `ra8_vmem`
frame -- and `ra8_book_src_image_rect()` reads an arbitrary sub-rectangle of a pool
image through it with a bounded working set (one packed span, or one gray8 row, per
`ra8_book_src_read`). Embedding a second JOF atlas inside the pool would be
tiling-on-tiling: it re-solves a problem the chunk layer already solved, and it
would fight the one-luma / one-quantiser parity the on-device (`stb_image`) and
desktop (Pillow + `gray4_kernel`) compilers share (the JOF PNG decoder keeps RGB888
rather than folding to luma). So the compiled path keeps the container simplest and
delivers the same "full-resolution tiled random access" the epic's storage decision
calls for through the flat pool plus the chunk layer plus the sub-rect reader.

**Storage tradeoff.** `gray8` is ~2x the raw bytes of `gray4` (one byte vs one
nibble per pixel). Most of that is recovered on disk: the whole blob is
chunk-DEFLATE-wrapped, and continuous-tone photographic/screentone content
compresses better than the 4bpp-quantised banding, so the packed-file delta is well
under 2x. The reader pays nothing extra -- a `gray8` sub-rect read is *cheaper* than
`gray4` (no nibble unpack) -- and in exchange the zoom and dither paths get a source
they can actually use. The inline (flow-size) reflow view is derived from the same
retained source at render time (nearest-neighbour scale), never baked in place of it.

---

## 4. Algorithms

### 4.1 Producing (`epub_compile.py`)

@dot
digraph rbkc_produce {
  bgcolor="transparent"; rankdir=TB;
  node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
        fillcolor="#e8eef7", color="#5a7ca6"];
  edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
  a [label="1. Parse the EPUB\n(unzip, XHTML -> nodes, CSS cascade)"];
  b [label="2. Serialise the RABOOK1 flat blob\n-> inflated_total is now known"];
  c [label="3. chunk_count = ceil(inflated_total / chunk_bytes)\n-> table size is now known"];
  d [label="4. For each chunk:\n   slice chunk_bytes of the blob\n   zlib-compress independently\n   append; record running offset"];
  e [label="5. Write header + table at the front,\n   then the payload"];
  a -> b -> c -> d -> e;
  d -> d [label="next chunk"];
}
@enddot

Because step 3 resolves the table size before any compression happens, the
producer never has to seek backwards -- unlike JOF, it can emit the file strictly
in order once it holds the blob.

### 4.2 Opening (`ra8_book_chunked_open()`)

1. Read the 24-byte header through the caller's `file_read` seam.
2. Check the magic, `chunk_bytes != 0`, and that `chunk_count` agrees with
   `ceil(inflated_total / chunk_bytes)`.
3. Load `chunk_count + 1` table entries into the caller's `table_buf`
   (which must have capacity for all of them).
4. Validate the table: `offset[0] == 0`, strictly increasing, and
   `offset[chunk_count]` equal to the payload length implied by `file_len`.
5. Check every chunk's compressed length against `staging_cap`, so no later read
   can overflow the staging buffer.

Note this is the **opposite** split from JOF, which defers per-entry validation
to read time. The reason is a size argument, not a philosophical one: a JOF
index can be 512 KiB, too large to hold; an RBKC table for a 4 MB book at 64 KiB
chunks is 64 entries -- 512 bytes. It is cheap to validate the whole table once
at open and never think about it again.

### 4.3 Reading (`ra8_book_chunked_read()`)

This function has exactly the `ra8_vsource_read_fn` signature, so it plugs
straight into the paging stack. Reads are **chunk-aligned by contract**:
`offset` must be a multiple of `chunk_bytes`, and `len` must equal that chunk's
exact inflated span.

```
  1. i = offset / chunk_bytes                     (no search)
  2. Range-check i < chunk_count.
  3. Require len == min(chunk_bytes,
                        inflated_total - i * chunk_bytes).
  4. comp_len = table[i+1] - table[i]
  5. Read comp_len bytes from payload_off + table[i] into staging.
  6. Inflate staging directly into the caller's frame.
  7. Require the inflated byte count to equal len exactly.
```

That contract sounds restrictive but it is precisely what `ra8_vsource_loader`
passes when the cache's `frame_bytes` equals the container's `chunk_bytes` -- the
loader clips the final frame to the object end, which is exactly the short last
chunk. The alignment requirement is therefore not a limitation the caller must
work around; it is the invariant that removes buffering.

---

## 5. Memory behaviour

The resident cost of an open chunked book is:

| Buffer | Size | Owner |
|--------|------|-------|
| chunk table | `(chunk_count + 1) * 8` bytes | caller |
| staging (one compressed chunk) | `staging_cap`, >= largest compressed chunk | caller |
| one cache frame | `chunk_bytes` | `ra8_vmem` |
| `ra8_book_chunked_t` | ~64 bytes | caller |

For a 4 MB book at 64 KiB chunks: 64 chunks, table = 520 B, staging ~64 KiB, frame 64 KiB
-- about **130 KB resident to read a 4 MB book**, and the only term that grows
with book size is the table, at 8 bytes per 64 KiB (0.012%).

@dot
digraph rbkc_residency {
  bgcolor="transparent"; rankdir=LR;
  node [fontname="Helvetica", fontsize=10, style=filled];
  edge [fontname="Helvetica", fontsize=9];

  sd [shape=box, style="rounded,filled", fillcolor="#f0f0f0", color="#888888",
      label="SD card\nwhole .rabook\nnever resident"];
  ram [shape=record, fillcolor="#dff0e4", color="#5f9e72",
       label="{SRAM|{table\n8 B/chunk|staging\n1 chunk|vmem frames\nN x chunk_bytes}}"];
  rend [shape=box, style="rounded,filled", fillcolor="#e8eef7", color="#5a7ca6",
        label="renderer\nreads nodes by index\ninto the flat blob"];

  sd -> ram [label="1 read per chunk fault"];
  ram -> rend [label="inflated bytes,\nused in place"];
}
@enddot

There is a second, simpler entry point worth knowing about: `ra8_book_open()`
inflates **every** chunk into one resident SDRAM buffer. That is the right
choice for a small book on a device with 64 MB of SDRAM, and the wrong choice
for a large one. The chunked reader exists for the second case; both consume the
identical container.

Zero allocation throughout (NASA P10 Rule 3) -- the caller supplies the table
and staging storage at open, which is why `ra8_book_chunked_open()` takes so
many buffer arguments.

---

## 6. Worked example -- real bytes

Reproducible from the in-tree fixture. `tests/fixtures/rabook_realbook` is a real
book (Thoreau's *Walden*) in unpacked EPUB form:

```
$ cd tests/fixtures/rabook_realbook && zip -q -X -r /tmp/real.epub . && cd -
$ python3 tools/epub_compile/epub_compile.py /tmp/real.epub /tmp/real.rabook --stats
Walden -- Henry David Thoreau
  chapters=2 nodes=338 attrs=47 css=2 images=0
  epub=27 KB -> rabook=25 KB (95%); inflated=66 KB

$ ./build/rabook_imagepack/rabook_imagepack inspect /tmp/real.rabook --verbose
RBKC rabook container: 26315 bytes
  chunk_bytes    : 65536
  inflated_total : 68527
  chunk_count    : 2
  reserved       : 0
  entry     start        end       length
      0         48      24796      24748
      1      24796      26315       1519
verdict: VALID (chunk table monotonic and complete)
```

### 6.1 The header and table, byte by byte

```
00000000  52 42 4B 43 00 00 01 00 AF 0B 01 00 00 00 00 00 |RBKC............|
00000010  02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 |................|
00000020  AC 60 00 00 00 00 00 00 9B 66 00 00 00 00 00 00 |.`.......f......|
```

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 0-3 | `52 42 4B 43` | `magic` | `"RBKC"` -- a byte-string magic, reads forwards |
| 4-7 | `00 00 01 00` | `chunk_bytes` | `0x00010000` = **65536** (64 KiB) |
| 8-15 | `AF 0B 01 00 00 00 00 00` | `inflated_total` | `0x00010BAF` = **68527** bytes |
| 16-19 | `02 00 00 00` | `chunk_count` | **2** |
| 20-23 | `00 00 00 00` | `reserved` | zero, as required |
| 24-31 | `00 ... 00` | `offset[0]` | **0** -- required |
| 32-39 | `AC 60 00 00 ...` | `offset[1]` | `0x60AC` = **24748** |
| 40-47 | `9B 66 00 00 ...` | `offset[2]` | `0x669B` = **26267** -- end sentinel |

Cross-check `chunk_count`: `ceil(68527 / 65536) = 2`. Matches.

Cross-check the payload origin: `payload_off = 24 + 8 * (2 + 1) = 48`, which is
exactly where `inspect` says entry 0 starts.

Cross-check closure: `payload_off + offset[2] = 48 + 26267 = 26315` = the file
length. The table closes the file exactly.

### 6.2 Chunk lengths are subtractions

| Chunk | `offset[i]` | `offset[i+1]` | Compressed length | Inflates to |
|-------|-------------|---------------|-------------------|-------------|
| 0 | 0 | 24748 | **24748** | `min(65536, 68527 - 0)` = **65536** |
| 1 | 24748 | 26267 | **1519** | `min(65536, 68527 - 65536)` = **2991** |

Note chunk 1 is the short final chunk -- 2991 bytes, not 65536. `65536 + 2991 =
68527 = inflated_total`. The two chunks tile the blob exactly, which is the same
"coverage is exact" property JOF gets from its edge clamp.

### 6.3 Both layers, visible

The first chunk's stream begins at file offset 48:

```
00000030  78 da cd bd 0b b8 ...
          ^^^^^
          zlib (RFC 1950) header -- contrast with a JOF tile,
          which begins straight into raw DEFLATE with no 78 xx
```

Inflate that chunk and the first sixteen bytes of the result are:

```
52 41 42 4F 4F 4B 31 00  01 00 00 00 AF 0B 01 00
|  R  A  B  O  O  K  1 \0|  ^^^^^^^^^^ ^^^^^^^^^^
                           format_version=1
                                        blob length 0x00010BAF = 68527
```

There is the inner layer: magic `RABOOK1`, `format_version` 1, and a length word
that agrees with the container's `inflated_total`. Two independent formats, one
file, each checkable on its own.

---

## 7. Edge cases, failure modes and security

The threat model is a malformed or hostile `.rabook` on an SD card causing the
application to crash, hang or read out of bounds -- not remote code execution.

| Malformed input | What could go wrong | What actually happens |
|-----------------|---------------------|-----------------------|
| Wrong `RBKC` magic | Misparse as another format | Rejected at the first check |
| `chunk_bytes == 0` | Division by zero computing `chunk_count` / chunk index | Non-zero is checked before any division |
| `chunk_count` disagrees with `ceil(inflated_total / chunk_bytes)` | Table shorter than assumed; OOB table read | Cross-checked at open |
| `table_cap_entries < chunk_count + 1` | Table write past the caller's buffer | Capacity checked before the table is loaded |
| `offset[0] != 0` | First chunk read from the wrong place | Required to be 0 |
| Non-monotonic table | Negative length via subtraction, huge unsigned wrap | Strictly-increasing check at open |
| `offset[chunk_count]` != payload length | Truncated or padded file | Must equal `file_len - payload_off` exactly |
| A chunk longer than `staging_cap` | Staging-buffer overflow | Every chunk's length checked against `staging_cap` **at open**, before any read |
| Unaligned `offset` in a read | Reading a chunk boundary the format cannot serve | Contract requires `offset % chunk_bytes == 0`; violation rejected |
| `len` != the chunk's exact inflated span | Short/long frame fill, uninitialised bytes | Required to match exactly |
| **Decompression bomb** | Tiny stream inflating to gigabytes | Two limits, below |
| Chunk inflating to the wrong size | Frame holds foreign or stale bytes | Inflated count must equal the expected span exactly |
| Bit rot inside a chunk | Silently corrupt text rendered as if valid | zlib Adler-32 fails the inflate |
| Inner blob corrupt but container valid | Renderer indexes garbage tables | Container passes; `ra8_book_validate()` independently checks the `RABOOK1` magic, version and flag mask |

### The decompression-bomb caps

As with JOF, two mechanisms stack:

1. **The format's own bound.** The reader knows the exact expected inflated size
   (`min(chunk_bytes, inflated_total - i * chunk_bytes)`) *before* inflating,
   and the destination is a cache frame already sized to `chunk_bytes`. A bomb
   cannot exceed the frame because the frame was sized from validated header
   geometry, never from the compressed stream.
2. **`ra8_decomp_limits_t`** -- the shared **64 MiB** output ceiling and
   **1024:1** expansion-ratio ceiling enforced inside `ra8_io_decompress()`.
   This is the backstop: even if a caller's size arithmetic were wrong, the
   inflate still cannot run away.

Here mechanism 2 is more load-bearing than it is for JOF, because
`inflated_total` is read *from the file*. A hostile header claiming a 400 GB
`inflated_total` is caught by the ratio and absolute ceilings rather than by
geometry.

### What is deliberately *not* defended

- **Semantic garbage.** A structurally valid book whose text is nonsense renders
  nonsense. Not a failure mode.
- **Integrity of the whole file.** Adler-32 per chunk detects bit rot; it is not
  a cryptographic checksum and does not detect deliberate tampering. A
  `.rabook` is parsed safely but is **not** authenticated. If a book must be
  trusted rather than merely parsed safely, it belongs inside something signed
  (see @ref md_docs_2formats_2ROT1).
- **Compression-ratio fairness.** A book that compresses badly is merely large.

---

## 8. Versioning

`RBKC` follows the discriminator-byte scheme from
@ref md_docs_2formats_2BINARY__FORMATS, with a twist worth spelling out because
the two layers version **independently**:

```
   "RBKC"                         "RABOOK1" + format_version
    \__/ |                         \_____/    \___________/
     |   |                            |             |
   family|                          family      inner revision
         |                                      (currently 1)
    "Chunked" -- names the container
    variant, not a revision number
```

- The **outer** container is identified by the four bytes `RBKC`. The `C` marks
  it as the *chunked* variant of the `.rabook` family rather than revision
  three. An incompatible container change spends a new fourth byte; every
  existing reader `memcmp`s and rejects it cleanly.
- The **inner** blob carries an explicit `format_version` field
  (`k_ra8_book_format_version`, currently 1) alongside its `RABOOK1` magic.
  Because the content layout is expected to evolve far more often than the
  transport, a numeric field is the better tool there -- and
  `ra8_book_validate()` checks it independently of anything the container did.
- **Feature flags extend without a version bump.** `ra8_book_header_t.flags`
  carries `ra8_book_flag_t` bits, and `ra8_book_validate()` rejects any blob
  setting a bit outside `k_ra8_book_flag_mask_known`. The rule is that a flag
  may only change how content is *presented* (for example `k_ra8_book_flag_rtl`
  for right-to-left reading order), never where a table or pool lives. So old
  firmware refuses a book depending on a semantic it does not implement, rather
  than silently mis-rendering it.

Under the project's zero-backward-compatibility policy the expected path for a
breaking change is: bump the magic or `format_version`, update
`epub_compile.py` and the reader in the same commit, regenerate fixtures, delete
the old handling. There is no dual-version reader.

---

## See also

- `ra8_book.h` -- container constants, the inner `RABOOK1` blob, flags
- `ra8_book_chunked.h` -- the demand-paged chunk reader specified here
- `ra8_book_paged.h` -- binds the paged object as a book source
- `ra8_vsource.h` -- the paging registry the reader plugs into
- @ref md_docs_2formats_2JOF -- the same seekability problem solved for an
  *image grid*, with the index at the end instead of the front
