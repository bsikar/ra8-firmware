# JOF -- The Jump-Offset Band-Tile Atlas

**Magic:** `JOF1` (header), `JOFE` (footer) &nbsp;|&nbsp;
**Library:** `libs/ra8_tileatlas` &nbsp;|&nbsp;
**Extension:** `.jof` &nbsp;|&nbsp;
**Issues:** #231 (full-resolution pages), #289 (longstrip scroll), #290 (codec policy), #319 (rename from RTA1)

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

JOF fixes this once, at **import time**, on the host. The source image is
transcoded into a grid of **independently decodable tiles**, plus a **per-tile
byte index**. A reader parses a fixed 16-byte footer, learns where the index
is, reads one 8-byte index entry, and jumps straight to the tile it wants. One
bounded read, one bounded inflate, and the resident cost is *one tile* -- not
one image.

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

## 2. Design rationale -- why not just use PNG?

This is the section worth reading even if you never touch the parser, because
the reasoning generalises to every format in this section.

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
with no special case. `ra8_fmt inspect` reports this as `longstrip: YES`.

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

The offsets above are mirrored by the `k_ra8_tileatlas_ofs_*` enumerators in
`ra8_tileatlas.h`, which are what the code actually indexes with. The limits
are `ra8_tileatlas_limits_t`.

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
`ra8_fmt verify` proves when it reports "coverage exact, no duplicate tiles".

By codec:

- **Codec 0 (raw)**: the stored stream *is* the payload, verbatim. Stored
  length equals `tw * th * bpp`.
- **Codec 1 (deflate)**: the stored stream is one standalone **raw DEFLATE**
  stream (RFC 1951 -- *no* zlib or gzip wrapper, no Adler-32) that inflates to
  exactly the payload size. Note the contrast with `RBKC` and `RCBZ`, which use
  zlib (RFC 1950) and therefore begin with a `78 xx` header; a JOF tile does
  not.

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

### 4.1 Producing (host side, `ra8_tileatlas_produce()` / `ra8_fmt convert`)

@dot
digraph produce {
  bgcolor="transparent"; rankdir=TB;
  node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
        fillcolor="#e8eef7", color="#5a7ca6"];
  edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
  a [label="1. Decode source (PNG/JPEG)\nand pick geometry"];
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
pure append-only sink (`ra8_tileatlas_memstore_sink`, or a ZIP store entry) can
do this by keeping those 32 bytes addressable; everything else is strictly
forward.

### 4.2 Parsing (`ra8_tileatlas_parse()`)

Parsing is cheap and touches only 48 bytes of the file. It reads the 32-byte
header at offset 0 and the 16-byte footer at `total_size - 16`, then
cross-checks, in order:

1. Both magics (`JOF1` at the head, `JOFE` at the tail).
2. `total_size` is large enough to hold header + footer, and within the
   `uint32` cap.
3. Geometry is non-zero and within `ra8_tileatlas_limits_t`.
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
**per read**, by `ra8_tileatlas_read_tile()`, at the moment they are used. This
is a deliberate split -- validate structure eagerly, validate contents lazily
but always before use.

### 4.3 Reading one tile (`ra8_tileatlas_read_tile()`)

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

`ra8_tileatlas_stored_bound()` sizes the scratch buffer. For deflate it returns
`raw + raw/8 + 256`, the safe over-estimate of miniz's worst-case expansion on
incompressible input -- because DEFLATE on random data is slightly *larger*
than the input, and a scratch buffer sized at exactly `raw` would fail on a
noise tile.

---

## 5. Memory behaviour

This is the section that justifies the format's existence, so it is worth being
concrete.

**Resident cost of reading a tile is `scratch_cap + out_cap`, and nothing
else.** It does not depend on the image dimensions, the tile count, or the file
size. The index is *not* held resident -- each read fetches its own 8-byte
entry. The parsed `ra8_tileatlas_info_t` is 24 bytes.

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
| JOF, one 800x64 RGB band | 245 760 payload + 287 kB scratch (~520 KiB) | Yes |

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
       label="{SRAM working set|{<s>scratch\\n(stored tile)|<o>out_px\\n(decoded tile)|<i>info\\n24 B}}"];
  cache [shape=box, style="rounded,filled", fillcolor="#e8eef7", color="#5a7ca6",
         label="ra8_tile_cache\n(N decoded tiles,\ncaller-sized)"];

  store -> ram [label="pread: 8 B index entry,\nthen length B tile"];
  ram -> cache [label="decoded tile"];
}
@enddot

Zero heap throughout (NASA P10 Rule 3): the caller owns `scratch` and `out_px`,
sizes them from `ra8_tileatlas_stored_bound()`, and the reader never allocates.

---

## 6. Worked example -- real bytes

Everything below was produced by running the in-tree tools. To reproduce:

```
cmake -S tools/ra8_fmt -B build/ra8_fmt && cmake --build build/ra8_fmt
./build/ra8_fmt/ra8_fmt convert --format jof --in sample.png --out sample.jof
./build/ra8_fmt/ra8_fmt inspect sample.jof
```

The source is a 200 x 300 RGB PNG. The converter chose a band geometry
(`tile_w == width`), giving one column and two rows.

```
convert: 200x300 bpp=3 band=256 tiles=2 -> sample.jof (1873 bytes)
```

```
JOF atlas: 1873 bytes
  image      : 200 x 300 px
  tile       : 200 x 256 px
  grid       : 1 cols x 2 rows
  bpp        : 3
  codec      : 1 (deflate)
  tile_count : 2
  index_off  : 1841
  total_size : 1873
  longstrip  : YES (tile_w == width)
verdict: VALID (coverage exact, no duplicate tiles)
```

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

Located at `total_size - 16` = `1873 - 16` = **1857**:

```
00000741  31 07 00 00 02 00 00 00 51 07 00 00 4a 4f 46 45  |1.......Q...JOFE|
```

| Bytes | Hex | Field | Decoded |
|-------|-----|-------|---------|
| 0-3 | `31 07 00 00` | `index_offset` | `0x0731` = **1841** |
| 4-7 | `02 00 00 00` | `tile_count` | **2** -- matches the header |
| 8-11 | `51 07 00 00` | `total_size` | `0x0751` = **1873** -- matches the real file length |
| 12-15 | `4a 4f 46 45` | `magic` | `"JOFE"` |

The closure check: `index_off + 8 * tile_count + 16` =
`1841 + 16 + 16` = `1873` = `total_size`. Exact. The file has no slack bytes.

### 6.3 The index

Two entries, 8 bytes each, at offset 1841:

```
00000731  20 00 00 00 22 05 00 00 42 05 00 00 ef 01 00 00  | ..."...B.......|
```

| Entry | Offset field | Length field | Decoded |
|-------|--------------|--------------|---------|
| `index[0]` | `20 00 00 00` = `0x20` = **32** | `22 05 00 00` = `0x0522` = **1314** | tile 0 at byte 32, 1314 bytes stored |
| `index[1]` | `42 05 00 00` = `0x0542` = **1346** | `ef 01 00 00` = `0x01ef` = **495** | tile 1 at byte 1346, 495 bytes stored |

Tile 0 starts at 32 -- immediately after the header, as expected. Tile 1 starts
at `32 + 1314 = 1346`, immediately after tile 0. The last tile ends at
`1346 + 495 = 1841`, which is exactly `index_off`. The tile-stream region is
contiguous and closes precisely where the index begins.

### 6.4 The edge clamp, demonstrated

This is where the `min()` from section 3.2 earns its keep:

| Tile | `tile_y` | Nominal | Clamp calculation | Actual | Payload bytes | Stored | Ratio |
|------|----------|---------|-------------------|--------|---------------|--------|-------|
| 0 | 0 | 200 x 256 | `min(256, 300 - 0*256)` = 256 | **200 x 256** | 153 600 | 1314 | 116.9:1 |
| 1 | 1 | 200 x 256 | `min(256, 300 - 1*256)` = **44** | **200 x 44** | 26 400 | 495 | 53.3:1 |

Tile 1 is *not* 256 rows tall. The image is 300 rows, tile 0 consumed 256, so
44 remain. The reader computes this independently and requires the inflate to
produce exactly 26 400 bytes -- if a corrupt stream inflated to 153 600 (a full
tile's worth), the read would fail closed rather than hand back 127 200 bytes of
whatever followed.

Coverage check: `153600 + 26400 = 180000 = 200 * 300 * 3`. Exactly the image,
no padding, no overlap. This is the invariant `ra8_fmt verify` asserts.

### 6.5 Start of a tile stream

```
00000020  ed d0 43 02 00 06 02 00 b1 ee d6 b6 6d db b6 6d  |..C.........m..m|
```

Note there is **no `78 xx` zlib header** -- the stream starts immediately with
DEFLATE-compressed data. This is the RFC 1951 raw stream promised by codec 1,
and it is the visible difference from `RBKC`/`RCBZ` payloads, which do begin
`78 da`.

### 6.6 The compression story

The whole atlas is **1873 bytes** and decodes to **180 000 bytes** -- about
96:1, because the sample is synthetic and highly compressible. Real manga line
art lands closer to 5:1 - 15:1, but the structural point holds: the file on SD
is small, and the RAM cost of reading it is one tile regardless.

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
| `width`/`height` > 32768 | Loop bound explosion, integer overflow in area | Capped by `ra8_tileatlas_limits_t` |
| `tile_count` != `cols * rows` | Index shorter than the reader assumes | Cross-checked in header *and* footer |
| `tile_count` > 65536 | 512 KiB+ index, unbounded loop | Capped |
| Non-zero reserved bytes | A future field silently reinterpreted | Required to be zero; rejected otherwise |
| `index_off` pointing into the header | Reader parses its own header as index entries | Index window must close the file exactly |
| Index entry pointing into the footer, the index, or past EOF | Out-of-bounds read | Every entry validated against `[32, index_off)` per read |
| Index entry with huge `length` | Unbounded read / scratch overflow | `length` must fit inside the tile-stream region *and* inside `scratch_cap` |
| Overlapping tile windows | Two tiles alias the same bytes | Legal on the wire but flagged by `ra8_fmt verify` ("no duplicate tiles"); harmless to the reader since each read is independently bounded |
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
for the shared paths (`RBKC`, `RCBZ`) where the expected size is itself read
from the file.

### What is deliberately *not* defended

- **Semantic garbage.** A valid atlas whose pixels are noise renders noise.
  That is not a failure mode, it is a content problem.
- **Overlapping tiles.** The wire format permits an index whose windows overlap.
  Each read is independently bounded so this cannot corrupt memory; it can only
  produce a visually wrong image. `ra8_fmt verify` reports it so a *producer*
  bug is caught in tooling rather than shipped.
- **Integrity/authenticity.** JOF has no checksum and no signature. It is not
  an authenticated format and must not be treated as one -- if an atlas needs to
  be trusted rather than merely parsed safely, it belongs inside something that
  *is* signed (see @ref md_docs_2formats_2ROT1).

---

## 8. Versioning

JOF uses the **discriminator-byte** scheme described in
@ref md_docs_2formats_2BINARY__FORMATS: the fourth byte of each magic carries
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
| `JOF1` | Current | Initial format. Previously named **RTA1** (`ra8_tileatlas`, "RA8 Tile Atlas"); renamed to JOF under issue #319 to name the *access pattern* rather than the library. The byte layout did not change in the rename -- only the magic and the library identifier. |

---

## See also

- `ra8_tileatlas.h` -- reader API, offset enums, limits
- `ra8_tileatlas_produce.h` -- import-time transcode producer
- `ra8_epub_img_tiles.h` -- EPUB tile-cache binder over this format
- @ref md_docs_2formats_2RBKC -- the chunked book container, which solves the
  same seekability problem for a *flat blob* rather than an image grid
- @ref md_docs_2formats_2RCBZ -- the per-page comic container, the same idea
  keyed by page instead of by tile
