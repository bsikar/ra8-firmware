# JOF -- The Jump-Offset Band-Tile Atlas

**Magic:** `JOF1` (header), `JOFE` (footer) &nbsp;|&nbsp;
**Library:** `libs/ra8_jof` &nbsp;|&nbsp;
**Extension:** `.jof` &nbsp;|&nbsp;
**Issues:** #231 (full-resolution pages), #289 (longstrip scroll), #290 (codec policy)

---

## 1. Synopsis

JOF is the format this device converts every raster image into **before** it
ever needs to display one. The name is the access pattern: *jump to an offset*.

The problem it solves is a hard collision between two facts. First, the content
this e-reader shows includes manga pages and webtoon-style longstrips that can
be **800 x 12260 pixels** -- a single image whose decoded form is roughly 29 MB
at 3 bytes per pixel. Second, the RA8D2 has **1.6 MB of SRAM**. The image is
about eighteen times larger than all the RAM on the chip. Even with the 64 MB
of external SDRAM, decoding one whole page to show a 1024 x 600 slice of it is
absurd, and the working set the renderer is actually allowed is closer to 10 MB.

The obvious answer -- "just decode part of it" -- does not work with the formats
the content actually arrives in. **JPEG and PNG have no random access.** PNG is
a single DEFLATE stream over the whole image with per-row filters that reference
the previous row, so to get row 12000 you must decompress rows 0 through 11999
first. Baseline JPEG is a single entropy-coded scan whose DC coefficients are
differentially coded across the entire image, so the same applies. Neither
format lets you say "give me the pixels from y=8000 to y=8256" without doing
almost all the work.

JOF fixes this once, at **import time**. The source image is transcoded into a
grid of **independently decodable tiles**, plus a **per-tile byte index**. A
reader parses a fixed 16-byte footer, learns where the index is, reads one
8-byte index entry, and jumps straight to the tile it wants. One bounded read,
one bounded inflate, and the resident cost is *one tile* -- not one image.

Import runs on the **host** (`rabook_imagepack convert`) *or* on the **device**
(`ra8_epub_tile_binder_import()`, when a book arrives with ordinary JPEG/PNG
inside it). That is not an afterthought: the producer is a streaming,
zero-heap, fixed-arena transcoder precisely so the device can run it, and
[section 5.1](#51-memory-behaviour-of-the-writer) is the half of the memory
story that makes on-device import possible at all.

That gives full-resolution random access with a working set that does not grow
with the image. A 12260-pixel-tall longstrip and a 600-pixel-tall page cost the
same RAM to scroll through. Crucially, **there is never a downscale**: the
pixels you see are the source pixels, which is what makes a zoom loupe on a
manga page possible at all.

One format serves three consumers, which is the other half of the point:

- **Full-resolution in-EPUB manga pages** (#231): a 2-D tile grid paged through
  `ra8_tile_cache` by the `ra8_epub_img_tiles` binder.
- **Longstrip band-scroll** (#289): a *band-tile* is just a tile as wide as the
  whole image (`tile_w == width`), so there is one tile column and the tile
  index **is** the band index -- O(1) seek to any scroll position.
- **Normalized on-device representation** (#290): every source codec converges
  on this one format at import, so the render path has exactly one decoder.

If you stop reading here: *JOF is a tiled, indexed, losslessly-recompressed
image container that trades a one-time host transcode for O(1) random access
and a constant, image-size-independent memory footprint on device.*

---

## 2. Design rationale -- why not PNG, and why not tiled TIFF?

This is the section worth reading even if you never touch the parser. Two
comparisons matter, and they are not the same argument: **PNG** is what the
content actually arrives as, and **tiled TIFF** is the format that genuinely
competes with what JOF *is*. The family-level version of this question -- why
every format in this section is bespoke, and what that costs -- is answered
once in @ref md_docs_2formats_2BINARY__FORMATS.

### The comparison that makes it click

Consider fetching the pixel rows for one screenful of an 800 x 12260 longstrip
-- say the band at y = 8192.

@dot
digraph png_vs_jof {
  bgcolor="transparent";
  rankdir=TB;
  fontname="Helvetica";
  node [shape=box, fontname="Helvetica", fontsize=10, style="filled,rounded"];
  edge [fontname="Helvetica", fontsize=9];

  subgraph cluster_png {
    label="Whole-image PNG: to read ONE band you decode EVERYTHING before it";
    fontsize=11; fontname="Helvetica-Bold";
    color="#b06a6a"; style="rounded"; bgcolor="#fbeeee";
    p0 [label="file header", fillcolor="#f0dada", color="#b06a6a"];
    p1 [label="one DEFLATE stream over ALL 12260 rows\n(row N filter references row N-1)",
        fillcolor="#e8c9c9", color="#b06a6a", width=5];
    p2 [label="inflate rows 0 .. 8191\n= ~19 MB of work, discarded",
        fillcolor="#e8c9c9", color="#b06a6a"];
    p3 [label="finally: the band you wanted", fillcolor="#d6efd9", color="#5f9e72"];
    p0 -> p1 -> p2 -> p3;
  }

  subgraph cluster_jof {
    label="JOF: the footer tells you where the index is; the index tells you where the tile is";
    fontsize=11; fontname="Helvetica-Bold";
    color="#5f9e72"; style="rounded"; bgcolor="#eef7f0";
    j0 [label="read last 16 bytes\n= footer", fillcolor="#dff0e4", color="#5f9e72"];
    j1 [label="seek index_off,\nread ONE 8-byte entry\nfor band 32", fillcolor="#dff0e4", color="#5f9e72"];
    j2 [label="seek that offset,\ninflate ONE tile", fillcolor="#dff0e4", color="#5f9e72"];
    j3 [label="the band you wanted", fillcolor="#d6efd9", color="#5f9e72"];
    j0 -> j1 [label="1 read"];
    j1 -> j2 [label="1 read"];
    j2 -> j3 [label="1 inflate"];
  }
}
@enddot

The PNG path does work proportional to **how far into the image you are**. The
JOF path does work proportional to **one tile**, no matter where you are. That
is the whole design in one picture: JOF converts a sequential format into a
seekable one by paying a one-time cost on a machine that can afford it.

### Why the index lives at the end

The index trails the tile streams rather than preceding them. This looks
backwards until you consider the producer: a writer does not know a tile's
compressed length until it has compressed it, and it does not know any tile's
byte offset until every earlier tile is written. Putting the index first would
force either a two-pass write (compress everything to a temporary buffer, then
write) or seeking backwards to patch offsets in.

With the index last, the producer emits the whole atlas through an
**append-only sink** in one forward pass -- which is exactly what writing to an
SD file or storing into a ZIP entry gives you. The reader pays nothing for
this: it reads the fixed-size footer from a known position (the last 16 bytes),
and the footer says where the index starts.

This is the same reason ZIP puts its central directory at the end, and it is
worth recognising the pattern: **append-only producer, seek-from-the-end
reader.**

### Why DEFLATE and not JPEG for the tile codec

Re-encoding tiles as JPEG would stack a **second lossy generation** on top of
whatever the source already lost. The no-quality-loss rule forbids that: a
manga page that has already been JPEG-compressed once should not acquire fresh
ringing artifacts just because the device wanted smaller tiles.

DEFLATE is lossless, it is already in the tree (miniz, reached through
`ra8_io_compress()` / `ra8_io_decompress()`), it inflates a 64 KiB tile in
bounded RAM with **zero heap**, and it compresses the flat colour regions and
sharp line art that dominate manga extremely well. Codec 0 (raw) remains for
atlases small enough that decode time matters more than size.

The tile codec is also **intra-coded** by construction -- no tile references
any other tile's state. That is not an optimisation, it is the load-bearing
property: cross-tile references would destroy the random access the whole
format exists to provide.

### Why a band-tile is the same thing as a tile

A pleasing consequence of the geometry: set `tile_w == width` and the grid
collapses to one column. Tile *n* is then simply band *n*, the tile index
becomes a band index, and 2-D paging code and 1-D scroll code share one reader
with no special case. `rabook_imagepack inspect` reports this as `longstrip: YES`.

### Why not tiled TIFF?

PNG is the honest comparison for how the content *arrives*, but a weak one for
what JOF *is*: PNG has neither tiling nor an index, so of course a tiled format
wins. The format that actually competes is **tiled TIFF** (TIFF 6.0 section
15), and a reader who knows it will notice it already has everything above.
`TileOffsets` (tag 324) and `TileByteCounts` (tag 325) *are* a jump-offset
index; `TileWidth` / `TileLength` set the grid; tiles are compressed
individually; and `COMPRESSION_ADOBE_DEFLATE` (8) is lossless DEFLATE.

So the following are **not** reasons to prefer JOF. They are the arguments this
section already made against PNG, and they do not transfer:

- *"A constant, image-size-independent memory footprint."* Tiled TIFF has this.
- *"O(1) random access through an index."* That is exactly what
  `TileOffsets` / `TileByteCounts` provide.
- *"Lossless DEFLATE, so no second lossy generation."* TIFF supports DEFLATE.

Three things actually decide it.

**1. The parser is the threat surface.**
[Section 7](#7-edge-cases-failure-modes-and-security) states the threat model:
not remote code execution from an SD card, but the application or an EPUB
**crashing or hanging** on a malformed file. Under that model the shape of the
parser *is* the risk.

Every JOF field sits at an offset known when the firmware is compiled -- the
`k_ra8_jof_ofs_*` enum in `ra8_jof.h` is the entire layout. A 32-byte header at
offset 0, 8-byte index entries, a 16-byte footer at `total_size - 16`. The
format has exactly two indirections (`index_off`, and each tile's `offset`),
both `uint32`, both validated against a window that must close the file
exactly. `ra8_jof_parse()` is a 27-line function.

TIFF's structure is instead *discovered by following the file*:

| | JOF | Tiled TIFF (TIFF 6.0) |
|---|---|---|
| Byte order | Little-endian, always | `II` **or** `MM`, chosen by bytes 0-1; readers must handle both |
| Where the metadata is | Fixed: offset 0, and `total_size - 16` | Bytes 4-7 point at the first IFD, which "may be at any location in the file after the header" -- including *after* the image data |
| How many fields | Fixed by the spec | A 2-byte count read **from the file** |
| Field size | Fixed width | 12 bytes each -- but the *value* is inline iff it fits in 4 bytes, decided by `Type` x `Count`, both read from the file |
| More metadata after? | No | A 4-byte "next IFD" offset: a linked list, which a hostile file can make cyclic |
| Variants | One | Plus BigTIFF (version 43, 64-bit offsets) -- a different parse |

TIFF 6.0 instructs readers, in the specification's own words, to *"follow the
pointers wherever they may lead."* That is a reasonable thing to ask of a
desktop application with a heap and an allocator that can fail safely. It is
the opposite of what a zero-allocation reader on a 1.6 MB part wants to hear.

None of this says TIFF cannot be parsed safely. It says the safe subset has to
be defined **by us**, because the format does not define one -- which leads
straight to the second point.

**2. It would be our parser either way.** libtiff is not a candidate: it is
built on `_TIFFmalloc` and allocates throughout, colliding directly with NASA
P10 Rule 3 (zero dynamic allocation). Note that **size is not the objection** --
this tree vendors NetX Duo (~481k lines) and ThreadX (~468k), either of which
dwarfs libtiff, so a large vendored dependency is plainly not banned here. The
objections are the heap and a CVE surface that must be tracked forever for a
decoder eating untrusted input.

Choosing TIFF therefore means hand-writing a bounded, hostile-input-safe TIFF
*subset* parser: accept `II` only, require a single IFD, bound the entry count,
reject unknown tags, and reconcile `TileOffsets` / `TileByteCounts` arrays that
the spec permits to be either `SHORT` or `LONG`. That parser is strictly larger
than JOF's 27 lines, and the files it accepts would no longer be TIFFs that
arbitrary tools produce -- they would be *our dialect wearing a TIFF magic
number*. That is the worst of both: nominal compatibility, and the parser is
still ours to own.

**3. The producer must never seek.** This point has nothing to do with
security. TIFF's header is at offset 0 and must point at the IFD, so a writer
either buffers the whole image or **seeks back** to patch that offset once the
tile data is written. JOF puts the pointer in the *footer* (see
[why the index lives at the end](#why-the-index-lives-at-the-end)), so the
producer emits header, tiles, index and footer in one forward pass.

That is load-bearing rather than elegant, because import also runs **on the
device**. The type signature is the proof: `ra8_jof_sink_fn` is
`(ctx, buf, len)` -- it has **no offset parameter at all**, so it is
structurally incapable of seeking, and neither real sink (an SD file, a SDRAM
memstore) needs one. Compare the *reader's* `ra8_jof_pread_fn`, which does take
a `uint64_t offset`. The asymmetry is deliberate, and it is what lets a page
whose decoded size exceeds SDRAM transcode without ever being resident
([section 5.1](#51-memory-behaviour-of-the-writer)).

There is a smaller fourth point, recorded because it bites the band-tile trick
specifically: TIFF requires `TileWidth` and `TileLength` to be **multiples of
16**, and pads boundary tiles out to full size. JOF clamps instead
([section 3.2](#32-tile-streams)), so tile areas sum to the image area exactly.
The band-tile identity above -- set `tile_w == width` and the tile index
*becomes* the band index -- is only expressible in TIFF when the image width
happens to be a multiple of 16.

**And the cost.** A tiled TIFF opens in any image viewer. A `.jof` opens in
nothing that already exists, which is why this tree ships both
`tools/rabook_imagepack` to inspect the bytes and `tools/rabook_viewer` to look at a page
-- two first-party tools recovering what TIFF gets for free. That is a real
and recurring cost, paid every time something needs debugging, and it is
accounted for with the rest of the bill in
@ref md_docs_2formats_2BINARY__FORMATS.

---

## 3. Wire format

All integers are **little-endian**. Offsets are absolute from byte 0 of the
atlas unless stated otherwise.

```
  +--------------------------------------------------+  offset 0
  |  header                              32 bytes    |
  +--------------------------------------------------+  offset 32
  |  tile 0 stream        index[0].length bytes      |
  |  tile 1 stream        index[1].length bytes      |
  |  ...                                             |   tile streams,
  |  tile N-1 stream                                 |   back to back
  +--------------------------------------------------+  footer.index_offset
  |  index    tile_count entries x 8 bytes           |
  +--------------------------------------------------+  total_size - 16
  |  footer                              16 bytes    |
  +--------------------------------------------------+  total_size
```

### 3.1 Header (32 bytes, at offset 0)

| Offset | Size | Field | Meaning and valid range |
|--------|------|-------|-------------------------|
| 0 | 4 | `magic` | Must be the bytes `4a 4f 46 31` (`"JOF1"`). Compared with `memcmp`. |
| 4 | 2 | `width` | Image width in pixels. `1 .. 32768` |
| 6 | 2 | `height` | Image height in pixels. `1 .. 32768` |
| 8 | 2 | `tile_w` | Tile width in pixels. `1 .. width` cap |
| 10 | 2 | `tile_h` | Tile height in pixels. `1 .. height` cap |
| 12 | 1 | `bpp` | Bytes per pixel. `1` = gray8, `3` = RGB888, `4` = RGBA8888 |
| 13 | 1 | `codec` | `0` = raw, `1` = raw DEFLATE (RFC 1951) |
| 14 | 2 | `reserved` | Must be `0` |
| 16 | 4 | `tile_count` | Must equal `tile_cols * tile_rows`. `1 .. 65536` |
| 20 | 12 | `reserved2` | Must be all `0` |

The offsets above are mirrored by the `k_ra8_jof_ofs_*` enumerators in
`ra8_jof.h`, which are what the code actually indexes with. The limits
are `ra8_jof_limits_t`.

The grid dimensions are **derived, not stored**:

```
  tile_cols = ceil(width  / tile_w)
  tile_rows = ceil(height / tile_h)
```

They are not fields because storing a value you can compute is storing a value
that can disagree with the values it was computed from. `tile_count` *is*
stored -- but only so the reader has something to cross-check the derivation
against, and a mismatch is a hard rejection.

### 3.2 Tile streams

Tile `n` is addressed in **row-major** order:

```
  n = tile_y * tile_cols + tile_x
```

and occupies the byte range `[index[n].offset, index[n].offset + index[n].length)`.

A tile's **decoded payload** is exactly `tw * th * bpp` bytes of tightly packed
row-major pixels, where the dimensions are **clamped at the right and bottom
edges**:

```
  tw = min(tile_w, width  - tile_x * tile_w)
  th = min(tile_h, height - tile_y * tile_h)
```

This clamp is the single most important line in the format. Without it, an
image whose dimensions are not exact multiples of the tile size would need
padding, and the reader would have to know how much padding to strip. With it,
edge tiles are simply *smaller*, the decoded byte count is exact, and the sum
of all tile areas equals the image area precisely -- which is what
`rabook_imagepack inspect` checks before reporting "coverage exact, no duplicate tiles".

By codec:

- **Codec 0 (raw)**: the stored stream *is* the payload, verbatim. Stored
  length equals `tw * th * bpp`.
- **Codec 1 (deflate)**: the stored stream is one standalone **raw DEFLATE**
  stream (RFC 1951 -- *no* zlib or gzip wrapper, no Adler-32) that inflates to
  exactly the payload size. Note the contrast with `RBKC`, which uses zlib
  (RFC 1950) and therefore begins with a `78 xx` header; a JOF tile does not.

### 3.3 Index (8 bytes per entry, at `footer.index_offset`)

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 4 | `offset` | Absolute byte offset of the tile stream, from atlas byte 0 |
| 4 | 4 | `length` | Stored byte length of the tile stream |

`tile_count` entries, in the same row-major order as the tiles.

### 3.4 Footer (16 bytes, the last 16 bytes of the atlas)

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 4 | `index_offset` | Absolute offset where the index begins |
| 4 | 4 | `tile_count` | Must equal the header's `tile_count` |
| 8 | 4 | `total_size` | Whole atlas length in bytes -- a self-check |
| 12 | 4 | `magic` | Must be the bytes `4a 4f 46 45` (`"JOFE"`) |

The duplicated `tile_count` is deliberate redundancy: the header and footer are
written at opposite ends of a potentially large file, and a mismatch is strong
evidence of truncation or splicing. `total_size` lets the reader detect
truncation even when the caller's idea of the file length is wrong.

### 3.5 Structural caps

| Cap | Value | Why |
|-----|-------|-----|
| Max width / height | 32768 px | Bounds every producer and reader loop (NASA P10 Rule 2) |
| Max tiles | 65536 | Bounds the index at 512 KiB (8 bytes/entry) |
| Max bpp | 4 | Largest legal pixel width |
| Max atlas size | 4 GiB | Offsets are `uint32` |

---

## 4. Algorithms

### 4.1 Producing (`ra8_jof_produce()` -- host *or* device)

The same function runs in both places. On the host it is driven by
`rabook_imagepack convert` / the media_dl core (`apps/shared/media_dl`); on
the device it is driven by
`ra8_epub_tile_binder_import()` when an EPUB turns out to contain ordinary
JPEG/PNG. There is no separate device transcoder and no reduced device mode --
the memory contract in [section 5.1](#51-memory-behaviour-of-the-writer) is
what lets one implementation serve both.

@dot
digraph produce {
  bgcolor="transparent"; rankdir=TB;
  node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
        fillcolor="#e8eef7", color="#5a7ca6"];
  edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
  a [label="1. Sniff the source magic (JPEG / PNG / WebP),\nread the geometry, pick the tile grid"];
  b [label="2. Reserve 32 bytes for the header\n(fields not all known yet)"];
  c [label="3. For each tile in row-major order:\n   clamp tw, th at the edges\n   extract payload\n   compress (codec 1) or copy (codec 0)\n   append to sink\n   record (offset, length)"];
  d [label="4. Append the index:\n   tile_count x 8 bytes"];
  e [label="5. Append the 16-byte footer:\n   index_offset, tile_count,\n   total_size, JOFE"];
  f [label="6. Patch the header in place\n(single known-position write)"];
  a -> b -> c -> d -> e -> f;
  c -> c [label="next tile"];
}
@enddot

The only backward write is step 6, into a fixed 32-byte window at offset 0. A
pure append-only sink (`ra8_jof_memstore_sink`, or a ZIP store entry) can
do this by keeping those 32 bytes addressable; everything else is strictly
forward.

### 4.2 Parsing (`ra8_jof_parse()`)

Parsing is cheap and touches only 48 bytes of the file. It reads the 32-byte
header at offset 0 and the 16-byte footer at `total_size - 16`, then
cross-checks, in order:

1. Both magics (`JOF1` at the head, `JOFE` at the tail).
2. `total_size` is large enough to hold header + footer, and within the
   `uint32` cap.
3. Geometry is non-zero and within `ra8_jof_limits_t`.
4. `bpp` is 1, 3 or 4; `codec` is 0 or 1.
5. Both reserved runs are entirely zero.
6. `tile_cols`/`tile_rows` derived by ceil-division; `tile_count == cols * rows`
   in **both** the header and the footer.
7. The footer's `total_size` equals the caller-supplied backing size.
8. The index window closes the file exactly:
   `index_off + 8 * tile_count + 16 == total_size`.

Note what parse does **not** do: it does not validate individual index entries.
The index can be up to 512 KiB, which is larger than any bounded parse buffer
the device is willing to hold. Per-tile offsets and lengths are validated
**per read**, by `ra8_jof_read_tile()`, at the moment they are used. This
is a deliberate split -- validate structure eagerly, validate contents lazily
but always before use.

### 4.3 Reading one tile (`ra8_jof_read_tile()`)

```
  1. Range-check tile_x, tile_y against the grid.
  2. Compute the clamped tw, th and the exact payload size tw*th*bpp.
  3. Check out_cap covers the payload.
  4. pread 8 bytes: the index entry at index_off + 8*n.
  5. Validate [offset, offset+length) lies entirely inside the tile-stream
     region [32, index_off) -- this is the check that stops a hostile index
     from pointing into the footer, the index itself, or past the file.
  6. codec 0: pread length bytes straight into out_px.
     codec 1: pread length bytes into scratch, then ra8_io_decompress()
              into out_px.
  7. Require the decoded byte count to equal the payload size exactly.
     Not "at least" -- exactly. A short or long inflate fails closed.
```

Step 5 is the security-critical one and step 7 is the correctness-critical one.
Together they mean a corrupt index can waste a read but cannot produce pixels
the caller will misinterpret.

`ra8_jof_stored_bound()` sizes the scratch buffer. For deflate it returns
`raw + raw/8 + 256`, the safe over-estimate of miniz's worst-case expansion on
incompressible input -- because DEFLATE on random data is slightly *larger*
than the input, and a scratch buffer sized at exactly `raw` would fail on a
noise tile.

### 4.4 Auditing a complete atlas (`ra8_jof_audit()`)

Normal reading validates only the tile being consumed. A qualification tool,
import pipeline, or storage transaction may instead need proof that the whole
atlas is coherent before publication. `ra8_jof_audit()` performs that bounded
full-object pass through the same injected `ra8_jof_pread_fn` abstraction:

1. `ra8_jof_audit_requirements()` parses the atlas and reports exact counts for
   the caller-owned record array, decoded-tile buffer, and DEFLATE scratch.
2. The audit rejects undersized, wrapping, or overlapping caller spans before
   changing workspace or result storage.
3. Every index entry is decoded in row-major order. Stored windows must cover
   the tile-stream region contiguously from byte 32 to `index_off`, with no
   gaps, overlap, reordering, or trailing unindexed data.
4. Every tile is decoded through `ra8_jof_read_tile()` and its dimensions are
   compared with the edge-clamped geometry from `ra8_jof_tile_dims()`.
5. A bounded content fingerprint records possible non-uniform duplicate tiles
   for diagnostics. A hash match is evidence only and never rejects valid
   repeated artwork.

The audit owns no file handle, mount, allocator, or device branch. A host file,
microSD entry, RAM image, flash object, or RPC-backed store supplies the same
positioned-read callback. All memory remains caller-owned and the implementation
retains no pointer after returning. Backend/decode failures preserve the public
result; a completed pass publishes its diagnostic counts even when structural
coverage or geometry fails.

---

## 5. Memory behaviour

This is the section that justifies the format's existence, so it is worth being
concrete. There are **two** memory stories, not one: reading a tile (below) and
*producing* the atlas in the first place ([section 5.1](#51-memory-behaviour-of-the-writer)).
The second is the one people ask about, because it is the one that sounds
impossible.

### 5.0 Reading

**Resident cost of reading a tile is `scratch_cap + out_cap`, and nothing
else.** It does not depend on the image dimensions, the tile count, or the file
size. The index is *not* held resident -- each read fetches its own 8-byte
entry. The parsed `ra8_jof_info_t` is 24 bytes.

For a 256 x 256 RGB888 tile:

```
  payload   = 256 * 256 * 3            = 196 608 bytes
  scratch   = 196608 + 196608/8 + 256  = 221 440 bytes
  resident  = payload + scratch        = 418 048 bytes  (~408 KiB)
```

Compare against decoding the whole 800 x 12260 longstrip:

| Approach | Resident bytes | Fits in 1.6 MB SRAM? |
|----------|----------------|----------------------|
| Whole-image decode, RGB888 | 29 424 000 (~28 MB) | No -- 18x over |
| JOF, one 256x256 RGB tile | 418 048 (~408 KiB) | Yes |
| JOF, one 800x64 RGB band | 153 600 payload + 173 056 scratch = 326 656 (~319 KiB) | Yes |

And the property that matters most: **the second column does not change when
the image gets taller.** A 100000-pixel-tall strip costs the same per tile.

@dot
digraph residency {
  bgcolor="transparent"; rankdir=LR;
  node [shape=record, fontname="Helvetica", fontsize=10, style=filled];
  edge [fontname="Helvetica", fontsize=9];

  store [shape=box, style="rounded,filled", fillcolor="#f0f0f0", color="#888888",
         label="Backing store\n(SD file / ZIP entry / SDRAM)\nWHOLE atlas lives here\nnever resident"];
  ram [shape=record, fillcolor="#dff0e4", color="#5f9e72",
       label="{SRAM working set|{<s>scratch\n(stored tile)|<o>out_px\n(decoded tile)|<i>info\n24 B}}"];
  cache [shape=box, style="rounded,filled", fillcolor="#e8eef7", color="#5a7ca6",
         label="ra8_tile_cache\n(N decoded tiles,\ncaller-sized)"];

  store -> ram [label="pread: 8 B index entry,\nthen length B tile"];
  ram -> cache [label="decoded tile"];
}
@enddot

Zero heap throughout (NASA P10 Rule 3): the caller owns `scratch` and `out_px`,
sizes them from `ra8_jof_stored_bound()`, and the reader never allocates.

### 5.1 Memory behaviour of the writer

Everything above is the *read* side. The obvious next question, and the one
this section exists to answer, is:

> The device imports media. Import means transcoding. There is no `malloc` --
> so what happens when the file to transcode is bigger than the RAM?

The answer is that **it never needs to be resident**, and the transcoder's
memory ceiling is a number you can compute *before you start*.

#### The streaming write model

For JPEG and PNG the producer is a **single forward pass** with a constant RAM
high-water. Nothing in the chain ever holds a whole image:

@dot
digraph produce_streaming {
  bgcolor="transparent"; rankdir=LR;
  node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
        fillcolor="#e8eef7", color="#5a7ca6"];
  edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];

  src [label="encoded source\n(SD file / ZIP entry)\npulled in bounded chunks\nNEVER held whole",
       fillcolor="#f0f0f0", color="#888888"];

  subgraph cluster_arena {
    label="cfg.work -- ONE fixed arena, sized up front by ra8_jof_work_bytes()";
    fontsize=11; fontname="Helvetica-Bold";
    color="#5f9e72"; style="rounded"; bgcolor="#eef7f0";
    dec  [label="stripe decoder\nJPEG: 128 KiB window\n+ one MCU-row stripe\nPNG: ring + 3 rows",
          fillcolor="#dff0e4", color="#5f9e72"];
    band [label="band accumulator\nwidth x tile_h x bpp\nONE tile row",
          fillcolor="#d6efd9", color="#5f9e72"];
    cut  [label="cut + intra-code\neach tile in the band",
          fillcolor="#dff0e4", color="#5f9e72"];
    dec -> band [label="scanline rows"];
    band -> cut [label="band full"];
    band -> band [label="drain, reuse"];
  }

  sink [label="append-only sink\nheader, tiles,\nindex, footer\n(never seeks)",
        fillcolor="#f0f0f0", color="#888888"];

  src -> dec;
  cut -> sink;
}
@enddot

The band accumulator is the heart of it. Rows arrive from the decoder in
scanline order; they are copied into a buffer exactly `tile_h` rows tall and as
wide as the image. When that buffer fills, every tile in the band is cut out,
compressed and appended, the buffer is reset, and decoding continues. The
index and footer **trail** the tile data (see
[why the index lives at the end](#why-the-index-lives-at-the-end)), which is
what lets the sink be strictly append-only -- an SD file or a SDRAM memstore,
no seeking.

The consequence is the headline: **a page whose decoded size exceeds SDRAM
transcodes without ever being resident.** The 800 x 12260 longstrip decodes to
~29 MB; the producer never holds more than one 800 x 256 band of it.

#### The arena contract

The producer **allocates nothing**. Every byte of state -- decoder buffers,
band, tile stage, compressor scratch, tile index -- is carved by an internal
bump allocator from one caller-supplied buffer, `cfg.work`. That is the entire
RAM cost, and `ra8_jof_work_bytes()` computes it exactly, up front, from
the caller's budget caps:

```c
uint32_t need = ra8_jof_work_bytes(max_w, max_h, tile_w, tile_h);
```

A caller therefore knows *before starting* whether an import fits. If the arena
is short the transcode fails closed with `k_ra8_err_invalid_size` before any
pixel is decoded -- it never overruns and never half-writes an atlas it cannot
finish.

The resident working set, for the 800 x 12260 longstrip at `tile_h = 256`
(every figure below is the real return of the sizing function, not an estimate):

| Carve | Bytes | What sets it |
|---|---:|---|
| Decoder set (worst of JPEG / PNG) | 169 472 | JPEG: 128 KiB input window + one MCU-row stripe (`width x 16 x 3`). PNG: inflate state (8 376) + 64 KiB ring + 4 KiB input + three row buffers |
| Band accumulator | 819 200 | `width x tile_h x bpp` |
| Tile stage | 819 200 | one uncompressed tile |
| Compressed-tile bound | 921 856 | `stage + stage/8 + 256` |
| Tile index | 384 | 8 bytes x 48 tiles |
| Deflate scratch | 319 352 | one `tdefl_compressor` (codec 1 only) |
| Alignment slack | 128 | per-carve rounding |
| **Total** | **3 049 592** | **~2.91 MiB** |

#### Size the arena from the function, never from a round number

A fixed-arena caller declares two things: the caps it advertises (`max_width` /
`max_height`) and the arena it provides (`work_cap`). **These are one decision,
not two.** Write the arena as the computed requirement for the declared cap:

```c
/* Not "2 MiB looks about right" -- the return of the sizing function for the
 * cap this caller actually advertises. */
uint32_t need = ra8_jof_work_bytes(max_w, max_h, tile_w, tile_h);
```

The reason this matters more than it looks is a subtlety in *when* an
under-sized arena is discovered. The producer carves from the **decoded**
geometry, not from the caps -- a source narrower than `max_width`, or one that
decodes to 1 bpp instead of the format's maximum 4, uses far less than the
sizing function reserves. So an arena that is too small for the advertised cap
does **not** fail on every source. It fails only on one wide enough, or deep
enough in bpp, to exhaust it. Everything else imports fine, and the caps read
as if they were honoured.

That makes an under-sized arena a **latent capability lie** rather than an
obvious bug: the caller advertises a cap it will fail closed on. It is still
fail-closed -- `k_ra8_err_invalid_size`, no overrun, no torn atlas, which is
why it can sit undetected -- but the declared capability is not the delivered
one.

The gap is not small. At `tile_w = tile_h = 256`:

| Advertised cap | Arena actually required |
|---|---:|
| 1 016 x 1 016 | 2 097 144 (8 bytes under 2 MiB) |
| 1 024 x 1 024 | 2 105 720 |
| 1 536 x 1 536 | 2 654 744 |
| 2 048 x 2 048 | **3 203 832** (~3.06 MiB) |
| 4 096 x 4 096 | 5 400 824 |

A round 2 MiB arena backs a declared cap of just **1 016 px**. A caller that
pairs 2 MiB with a declared 2 048 is over-claiming by a factor of two in each
axis -- and `examples/ek_ra8d2/hw_pending/ereader_manga` shipped exactly that
pairing until the arena was re-derived. The carve set for that 2 048 cap:

| Carve | Bytes | What sets it |
|---|---:|---|
| Decoder set (worst of JPEG / PNG) | 229 376 | JPEG wins here: 128 KiB window + `2048 x 16 x 3` stripe |
| Band accumulator | 2 097 152 | `2048 x 256 x 4` -- the dominant term |
| Tile stage | 262 144 | `256 x 256 x 4` |
| Compressed-tile bound | 295 168 | `stage + stage/8 + 256` |
| Tile index | 512 | 8 bytes x 64 tiles |
| Deflate scratch | 319 352 | one `tdefl_compressor` |
| Alignment slack | 128 | per-carve rounding |
| **Total** | **3 203 832** | **~3.06 MiB** |

Because the firmware cannot call the sizing function at compile time, a
static arena has to hold a literal -- so pin that literal with checks that
fail when it drifts from the cap beside it:

- a `static_assert` on the **band term** (`cap x tile_h x bpp_max`), which is a
  strict lower bound on the full requirement and needs only public constants.
  It catches "raised the cap, forgot the arena" at build time. It cannot
  certify a *sufficient* arena -- it is a lower bound, not the total;
- a **host test** asserting the arena against the real
  `ra8_jof_work_bytes()` return. This is the exact, authoritative check,
  and it is the one that runs in CI;
- a **boot-time** re-check of the same call, so a mismatch that somehow reaches
  silicon reports a hard failure instead of silently under-delivering.

`tests/test_app_ereader_manga.c` is the worked instance of all three.

#### What the ceiling actually depends on

The header describes this arena as "independent of the image". That is the
right intuition but it is worth stating precisely, because *which* dimension it
is independent of is the entire point:

| Varying | Arena | Effect |
|---|---:|---|
| height 1 000 | 3 049 240 | -- |
| height 4 000 | 3 049 336 | +96 B |
| height 12 260 | 3 049 592 | +352 B |
| height 32 768 (format cap) | 3 050 232 | +992 B |

Growing the image **32x taller costs 992 additional bytes** -- and those bytes
are entirely the 8-byte-per-tile index, the one term that scales with tile
count. The pixel path does not move at all. So the precise claim is:

> The arena is a function of **width**, **`tile_h`** and **tile count**. It is
> independent of image *height* to within 8 bytes per band.

That is exactly the property that makes unbounded-length longstrips importable,
and it is why the interesting cap is `max_width`, not `max_height`. Width, by
contrast, is *not* free: it scales the band, the stage and the compressed bound
together, so a 1600-wide page costs roughly double an 800-wide one.

One consequence that surprises people: **`tile_w == width` is a reader
optimisation, and it costs the writer.** Band-tiles give the reader an O(1)
seek (the tile index *is* the band index), but the writer streams a full-width
band regardless of `tile_w` -- narrow tiles are simply cut out of that band. So
choosing `tile_w == width` does not enable streaming; it just enlarges the tile
stage until it equals a whole band. For a 1600 x 2300 page:

| Tile geometry | Arena | |
|---|---:|---|
| `1600 x 256` (band-tile) | 5 647 680 | ~5.39 MiB |
| `256 x 256` (square tile) | 2 723 568 | ~2.60 MiB |

Square tiles more than halve the import cost. Band-tiles buy scroll-seek
simplicity in return. Pick deliberately.

#### WebP is the genuine exception

WebP cannot stream, and this is a property of **the codec, not of this
implementation**. VP8L (lossless WebP) encodes with 2-D backward references
that may point anywhere in the frame already decoded, so there is no bounded
output window through which the image can be emitted -- unlike a PNG scanline
or a JPEG MCU row, a VP8L pixel can depend on a pixel thousands of rows back.

So a WebP source is normalised through the *same* JOF tile path, but the decode
in front of it is whole-frame, and it is paid for out of a **second, separate**
arena sized by `ra8_jof_webp_work_bytes()`. That arena holds three things
at once: the compressed source, the decoded RGBA8888 frame, and libwebp's
internal scratch.

| WebP source | `webp_work` needed |
|---|---:|
| 800 x 1200, 512 KiB compressed | 13 092 992 (~12.49 MiB) |
| 1600 x 2300, 2 MiB compressed | 47 305 856 (~45.11 MiB) |
| 8192 x 8192 (axis cap), 4 MiB compressed | 810 549 376 (~773 MiB) |
| 8193 wide (over cap) | 0 -- rejected |

That last row is the design working. The cost is honest and it is bounded by
declared caps; an over-cap source returns 0 from the sizing function and
`k_ra8_err_not_supported` from the producer, rather than being quietly
downscaled into something affordable.

The critical detail is the default: **`webp_work == NULL` fail-closed rejects
every WebP source** with `k_ra8_err_not_supported`. A streaming-only caller
passes NULL, pays *nothing*, and cannot accidentally blow its budget on a
whole-frame decode it never provisioned for. WebP support is opt-in by
supplying memory, which is the only honest way to expose a whole-frame codec
inside a fixed-arena system.

#### The other fail-closed rejections

The same principle covers every source variant that cannot be striped. Each is
rejected with `k_ra8_err_not_supported` *before* any pixel work:

| Rejected | Why it cannot stream |
|---|---|
| Progressive JPEG | Coefficients arrive across multiple scans over the whole image; no row is final until the last scan |
| Interlaced (Adam7) PNG | Pixels arrive in seven passes scattered across the frame, not in scanline order |
| 16-bit PNG | Outside the supported 8-bit sample path |
| Anything not JPEG / PNG / WebP | No decoder |

The caller's fallback is a whole-decode path, used only for images small enough
to afford it -- a deliberate, explicit choice rather than a silent one.

**No downscaling, ever.** Output pixels are the decoded pixels, at full
resolution, losslessly recompressed. Shrinking an oversized page to fit the
arena is *not* an available mitigation and will not be added: full-resolution
pixels are what make a zoom loupe on a manga page possible, which is the
product requirement the format exists to serve (issues #210-#213).

#### Import on the device

`ra8_epub_tile_binder_import()` (`libs/ra8_epub`) is the device-side driver,
and it is a thin one -- there is no separate device transcoder:

1. **Classify.** A 4-byte positioned read sniffs the entry. If it already
   starts with `JOF1` the entry is registered **in place** and there is no
   transcode and no arena at all. (A *deflated* ZIP entry reports
   `k_ra8_err_not_supported` to that positioned read, which correctly
   classifies it as "not an in-place atlas" and routes it to the transcode
   path.)
2. **Stream.** Otherwise it opens an entry cursor and wraps
   `ra8_epub_entry_read` as the producer's `pull` seam. The encoded source is
   pulled straight out of the ZIP entry in bounded chunks -- stored or
   deflated, decompressed on the fly, **never staged whole**.
3. **Produce.** It forwards the caller's knobs verbatim into
   `ra8_jof_produce_cfg_t`: `tile_w`, `tile_h`, `codec`, `max_width`,
   `max_height`, `work` / `work_cap`, and `webp_work` / `webp_work_cap`.
4. **Register.** The finished atlas is bound into the tile binder through the
   store's `pread` seam. The entry cursor is always closed, with the first
   error winning.

Note what the EPUB layer does *not* do: **it supplies no memory of its own.**
The arena is entirely the application's, arriving through
`ra8_epub_atlas_import_cfg_t`. The e-reader application places it in external
SDRAM -- which is the real answer to "we can't allocate more". The device does
not allocate; it is *given* a fixed arena at build time, sized by
`ra8_jof_work_bytes()` for the caps that application intends to support,
and any source outside those caps is refused rather than accommodated.

> **Normative split.** The wire format in [section 3](#3-wire-format) is
> normative here -- this document defines the bytes. The producer's *memory
> contract* is normative in **`ra8_jof_produce.h`**: the carve set,
> the arena sizing functions and the fail-closed conditions are defined by
> that header and its implementation, and this section is explanatory. If the
> two ever disagree, the header wins and this section is the bug. Every figure
> in section 5.1 is a computed return value of
> `ra8_jof_work_bytes()` / `ra8_jof_webp_work_bytes()`.

---

## 6. Worked example -- real bytes

Every byte below is real output from the in-tree tools, and the whole example is
**reproducible from scratch** -- the source image is generated by a deterministic
script rather than being some file you do not have. Run these four steps and you
will get the identical bytes.

**Step 1 -- build the tool.**

```
cmake -S tools/rabook_imagepack -B build/rabook_imagepack && cmake --build build/rabook_imagepack
```

**Step 2 -- generate the source image.** A 200 x 300 RGB PNG, an 8-pixel
checkerboard. The pattern is deliberately line-art-like: large flat runs of one
value, which is what DEFLATE handles well and what manga actually looks like.

```
python3 - <<'EOF'
import struct, zlib
W, H = 200, 300
px = bytearray()
for y in range(H):
    px.append(0)                      # PNG filter byte: none
    for x in range(W):
        ink = (x // 8 + y // 8) % 2   # 8 px checker
        v = 32 if ink else 224
        px += bytes([v, v, v])
def chunk(tag, data):
    body = tag + data
    return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(bytes(px), 9))
       + chunk(b'IEND', b''))
open('sample.png', 'wb').write(png)
EOF
```

**Step 3 -- convert.** The converter picked a band geometry (`tile_w == width`)
on its own, giving one column and two rows:

```
$ ./build/rabook_imagepack/rabook_imagepack convert --format jof --in sample.png --out sample.jof
convert: 200x300 bpp=3 band=256 tiles=2 -> sample.jof (1264 bytes)
```

**Step 4 -- inspect.** `--verbose` adds the raw header/footer hexdumps and the
per-tile table, so the tool itself produces the annotated dump:

```
$ ./build/rabook_imagepack/rabook_imagepack inspect sample.jof --verbose
JOF atlas: 1264 bytes
  image      : 200 x 300 px
  tile       : 200 x 256 px
  grid       : 1 cols x 2 rows
  bpp        : 3
  codec      : 1 (deflate)
  tile_count : 2
  index_off  : 1232
  total_size : 1264
  longstrip  : YES (tile_w == width)
header (32 bytes at +0):
  00000000  4A 4F 46 31 C8 00 2C 01 C8 00 00 01 03 01 00 00  |JOF1..,.........|
  00000010  02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
footer (16 bytes at +1248):
  000004E0  D0 04 00 00 02 00 00 00 F0 04 00 00 4A 4F 46 45  |............JOFE|
  tile      offset    length   payload   tw    th   hash
       0        32      1003    153600   200   256   EF567DC5
       1      1035       197     26400   200    44   D82BF045
verdict: VALID (coverage exact, no duplicate tiles)
```

The rest of this section takes that output apart field by field.

### 6.1 The header, byte by byte

```
00000000  4a 4f 46 31 c8 00 2c 01 c8 00 00 01 03 01 00 00  |JOF1..,.........|
00000010  02 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
```

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 0-3 | `4a 4f 46 31` | `magic` | `"JOF1"` -- a byte-string magic, so it reads forwards |
| 4-5 | `c8 00` | `width` | `0x00c8` = **200** px |
| 6-7 | `2c 01` | `height` | `0x012c` = **300** px |
| 8-9 | `c8 00` | `tile_w` | `0x00c8` = **200** px -- equal to `width`, hence longstrip |
| 10-11 | `00 01` | `tile_h` | `0x0100` = **256** px |
| 12 | `03` | `bpp` | 3 = RGB888 |
| 13 | `01` | `codec` | 1 = raw DEFLATE |
| 14-15 | `00 00` | `reserved` | zero, as required |
| 16-19 | `02 00 00 00` | `tile_count` | **2** |
| 20-31 | all `00` | `reserved2` | twelve zero bytes, as required |

Cross-check the derivation: `tile_cols = ceil(200/200) = 1`,
`tile_rows = ceil(300/256) = 2`, so `cols * rows = 2` -- which matches the
stored `tile_count`. The parse accepts.

### 6.2 The footer

Located at `total_size - 16` = `1264 - 16` = **1248**:

```
000004E0  D0 04 00 00 02 00 00 00 F0 04 00 00 4A 4F 46 45  |............JOFE|
```

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 0-3 | `D0 04 00 00` | `index_offset` | `0x04D0` = **1232** |
| 4-7 | `02 00 00 00` | `tile_count` | **2** -- matches the header |
| 8-11 | `F0 04 00 00` | `total_size` | `0x04F0` = **1264** -- matches the real file length |
| 12-15 | `4A 4F 46 45` | `magic` | `"JOFE"` |

The closure check: `index_off + 8 * tile_count + 16` =
`1232 + 16 + 16` = `1264` = `total_size`. Exact. The file has no slack bytes.

### 6.3 The index

Two entries, 8 bytes each, at offset 1232:

```
000004D0  20 00 00 00 EB 03 00 00 0B 04 00 00 C5 00 00 00  | ...............|
```

| Entry | Offset field | Length field | Decoded |
|-------|--------------|--------------|---------|
| `index[0]` | `20 00 00 00` = `0x20` = **32** | `EB 03 00 00` = `0x03EB` = **1003** | tile 0 at byte 32, 1003 bytes stored |
| `index[1]` | `0B 04 00 00` = `0x040B` = **1035** | `C5 00 00 00` = `0x00C5` = **197** | tile 1 at byte 1035, 197 bytes stored |

These are the same `offset` / `length` columns the `--verbose` tile table
printed. Tile 0 starts at 32 -- immediately after the header, as expected. Tile
1 starts at `32 + 1003 = 1035`, immediately after tile 0. The last tile ends at
`1035 + 197 = 1232`, which is exactly `index_off`. The tile-stream region is
contiguous and closes precisely where the index begins.

### 6.4 The edge clamp, demonstrated

This is where the `min()` from section 3.2 earns its keep. The `tw` / `th` /
`payload` columns below are read straight off the `--verbose` tile table:

| Tile | `tile_y` | Nominal | Clamp calculation | Actual (`tw` x `th`) | Payload bytes | Stored | Ratio |
|------|----------|---------|-------------------|----------------------|---------------|--------|-------|
| 0 | 0 | 200 x 256 | `min(256, 300 - 0*256)` = 256 | **200 x 256** | 153 600 | 1003 | 153.1:1 |
| 1 | 1 | 200 x 256 | `min(256, 300 - 1*256)` = **44** | **200 x 44** | 26 400 | 197 | 134.0:1 |

Tile 1 is *not* 256 rows tall. The image is 300 rows, tile 0 consumed 256, so
44 remain -- and the tool's `th` column agrees. The reader computes this
independently and requires the inflate to produce exactly 26 400 bytes: if a
corrupt stream inflated to 153 600 (a full tile's worth), the read would fail
closed rather than hand back 127 200 bytes of whatever followed.

Coverage check: `153600 + 26400 = 180000 = 200 * 300 * 3`. Exactly the image,
no padding, no overlap. That is the invariant behind `inspect`'s
`coverage exact, no duplicate tiles` verdict.

### 6.5 Start of a tile stream

```
00000020  ed cf 31 0d c0 30 00 c4 40 2a e5 8f 2a 50 4a a0  |..1..0..@*..*PJ.|
```

Note there is **no `78 xx` zlib header** -- the stream starts immediately with
DEFLATE-compressed data. This is the RFC 1951 raw stream promised by codec 1,
and it is the visible difference from `RBKC` payloads, which do begin `78 da`.

### 6.6 Proving the transcode is lossless

`inspect` proves the container is *structurally* sound. It does not prove the
pixels survived. That is what `verify` is for, and note it takes the **source
image**, not the atlas -- it re-runs the transcode and compares against a
single-tile reference decode of the original:

```
$ ./build/rabook_imagepack/rabook_imagepack verify --format jof --in sample.png
verify: 200x300 bpp=3 | reference 1 tile | banded 2 tiles of 256 rows
verdict: ROUND-TRIP EXACT -- the produced file is correct (0 differing bytes)
```

**Zero differing bytes.** This is the no-quality-loss rule from section 2 held
to mechanically: banding an image into tiles and inflating them back yields the
source pixels exactly, so the tiling is a pure repackaging and never a second
lossy generation.

### 6.7 The compression story

The whole atlas is **1264 bytes** and decodes to **180 000 bytes** -- about
142:1, because a flat checkerboard is close to the best case for DEFLATE. Real
manga line art lands closer to 5:1 - 15:1, but the structural point holds: the
file on SD is small, and the RAM cost of reading it is one tile regardless of
how large the image is.

---

## 7. Edge cases, failure modes and security

The threat model is not remote code execution from an SD card. It is an EPUB or
the application **crashing or hanging** because a file was malformed, truncated
mid-write by a yanked card, or deliberately hostile. Every case below is a
rejection, not a crash.

| Malformed input | What could go wrong | What actually happens |
|-----------------|---------------------|-----------------------|
| Wrong or missing `JOF1` magic | Misparse as some other format | `k_ra8_err_validation_failed` at the first check |
| Wrong `JOFE` footer magic | Truncation goes undetected | Rejected; the head/tail pair must both match |
| Truncated file | Reads past the end | `total_size` must equal the caller's backing size; the pread seam reports short reads and they fail closed |
| `width`/`height` = 0 | Division by zero computing `tile_cols` | Non-zero is checked before any ceil-division |
| `width`/`height` > 32768 | Loop bound explosion, integer overflow in area | Capped by `ra8_jof_limits_t` |
| `tile_count` != `cols * rows` | Index shorter than the reader assumes | Cross-checked in header *and* footer |
| `tile_count` > 65536 | 512 KiB+ index, unbounded loop | Capped |
| Non-zero reserved bytes | A future field silently reinterpreted | Required to be zero; rejected otherwise |
| `index_off` pointing into the header | Reader parses its own header as index entries | Index window must close the file exactly |
| Index entry pointing into the footer, the index, or past EOF | Out-of-bounds read | Every entry validated against `[32, index_off)` per read |
| Index entry with huge `length` | Unbounded read / scratch overflow | `length` must fit inside the tile-stream region *and* inside `scratch_cap` |
| Overlapping tile windows | Two tiles alias the same bytes | Legal on the wire but flagged by `rabook_imagepack inspect` ("no duplicate tiles"); harmless to the reader since each read is independently bounded |
| **Decompression bomb** | Tiny stream inflating to gigabytes | Two independent limits, below |
| Inflate producing the wrong size | Caller reads uninitialised or foreign pixels | Decoded size must equal `tw*th*bpp` **exactly** |
| `bpp` = 0 or 7 | Payload-size arithmetic nonsense | Must be 1, 3 or 4 |
| `codec` = 2 | Unknown decoder path | Must be 0 or 1 |

### The decompression-bomb caps

Two mechanisms stack here, which is worth understanding because they catch
different attacks:

1. **The format's own bound.** The reader knows the exact expected output size
   (`tw * th * bpp`) *before* it inflates, and `out_cap` must already cover it.
   A bomb cannot write past the output buffer because the buffer was sized from
   validated geometry, not from anything the compressed stream claims.
2. **`ra8_decomp_limits_t`**, the shared cap in `ra8_core`: a hard **64 MiB**
   output ceiling and a **1024:1** expansion-ratio ceiling, enforced inside
   `ra8_io_decompress()`. This is the backstop that protects every DEFLATE
   consumer in the tree, so a bug in any single caller's size arithmetic still
   cannot turn into unbounded memory growth.

For JOF, mechanism 1 is normally what fires -- the geometry cap of 32768 x
32768 x 4 bpp already bounds a single tile far below 64 MiB. Mechanism 2 matters
for `RBKC`, where the expected size is itself read from the file.

### What is deliberately *not* defended

- **Semantic garbage.** A valid atlas whose pixels are noise renders noise.
  That is not a failure mode, it is a content problem.
- **Overlapping tiles.** The wire format permits an index whose windows overlap.
  Each read is independently bounded so this cannot corrupt memory; it can only
  produce a visually wrong image. `rabook_imagepack inspect` reports it so a *producer*
  bug is caught in tooling rather than shipped.
- **Integrity/authenticity.** JOF has no checksum and no signature. It is not
  an authenticated format and must not be treated as one -- if an atlas needs to
  be trusted rather than merely parsed safely, it belongs inside something that
  *is* signed (see @ref md_docs_2formats_2ROT1).

---

## 8. Versioning

JOF uses the **discriminator-byte** scheme described in
@ref md_docs_2formats_2BINARY__FORMATS -- the fourth byte of each magic carries
the revision.

```
   "JOF1"                          "JOFE"
    ^^^ ^                           ^^^ ^
    |   |                           |   |
    |   +-- format revision 1       |   +-- "End": marks the footer, not a
    |                               |          revision
    +-- family: Jump-Offset Format  +-- family
```

A reader `memcmp`s four bytes. There is no partial acceptance and no
"best-effort parse of an unknown version" path. The consequences:

- **Adding an incompatible revision** means emitting `JOF2`. Every existing
  reader rejects it immediately and cleanly, with no code change and no risk of
  misparsing a structure whose meaning changed.
- **Adding a compatible field** means consuming reserved space. There are 14
  reserved bytes in the header (2 at offset 14, 12 at offset 20), all required
  to be zero today. A future reader can distinguish "old file, field absent"
  from "new file, field set" because zero is reserved as the absent value, and
  an old reader rejects any file that sets them -- which is the correct
  behaviour if the new field changes interpretation.
- **The footer magic is a role marker, not a version.** `JOFE` would stay
  `JOFE` across a `JOF2` bump unless the footer layout itself changed.

Because this project has a **zero backward-compatibility policy**, the expected
path for any real change is: bump to `JOF2`, update the producer and reader in
the same commit, regenerate any fixtures, and delete the old handling. There is
no dual-version reader and there should never be one.

### History

| Revision | Status | Notes |
|----------|--------|-------|
| `JOF1` | Current | Initial format. |

---

## See also

- `ra8_jof.h` -- reader API, offset enums, limits
- `ra8_jof_audit.h` -- complete, no-heap audit over an injected positioned
  reader and caller-owned workspace
- `ra8_jof_produce.h` -- import-time transcode producer, and the
  **normative** contract for the writer memory model described in section 5.1
  (`ra8_jof_work_bytes()`, `ra8_jof_webp_work_bytes()`)
- `ra8_epub_img_tiles.h` / `ra8_epub_tile_binder_import()` -- device-side
  import driver
- `ra8_epub_img_tiles.h` -- EPUB tile-cache binder over this format
- @ref md_docs_2formats_2RBKC -- the chunked book container, which solves the
  same seekability problem for a *flat blob* rather than an image grid
