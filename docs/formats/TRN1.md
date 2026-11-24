# TRN1 -- Per-Chapter Translation Sidecar Container

**Magic:** `TRN1` (`0x314E5254`) &nbsp;|&nbsp;
**Library:** `libs/ra8_translate` &nbsp;|&nbsp;
**Extension:** `.trn1` &nbsp;|&nbsp;
**Producer / Reader:** `libs/ra8_translate`

---

## 1. Synopsis

`TRN1` is the per-chapter binary sidecar produced by the offline chapter
translation pipeline and read by the e-reader overlay renderer. It stores,
for every page in a downloaded chapter, each detected text bubble's box and
shape, reading order, source OCR text and confidence, translated English
text, and fitted layout parameters (font size in pixels and per-line byte
break offsets), all keyed by the chapter content SHA-256 hash and model
versions.

Because the reader validates a single 96-byte versioned header, fixed-width
page and bubble tables, a compact `u16` line-break table, and one UTF-8
text pool with a whole-file CRC-32 check, opening and rendering a chapter
sidecar takes zero heap allocation and zero text-format parsing.

If you stop reading here: *`TRN1` is a little-endian, CRC-32-checked
sidecar keyed by chapter hash and model versions that lets the e-reader
index every page's bubbles, text, and pre-fitted line wraps in place
with zero dynamic allocation, while rejecting stale caches whenever the
chapter or any translation model changes.*

---

## 2. Design rationale

### Why a bespoke binary container instead of JSON or CBOR

Running JSON, TOML, or dynamically keyed CBOR parsing on every page flip
would either require an allocation-heavy AST or re-scan variable-length
strings repeatedly inside a 1.6 MB SRAM budget. A fixed-width offset table
pairs a single bounded validation pass at chapter open with `O(1)` lookup
for any page (`page_index -> first_bubble .. first_bubble + bubble_count`)
and any bubble (`bubble_index -> shape, box, UTF-8 spans, line breaks`).

### Why content hash and explicit model versions sit in the header

A cached translation becomes wrong whenever either:

1. the source chapter changes (re-downloaded or edited chapter archive),
2. the detector `.npub`, OCR `.npub`, or Marian NMT model is upgraded, or
3. the series glossary revision or source language changes.

Storing `chapter_hash` (32-byte SHA-256), `detector_model_version`,
`ocr_model_version`, `nmt_model_version`, `glossary_revision`, and
`source_lang` directly in the fixed header lets the caller verify cache
freshness before reading any page or bubble table. Any mismatch returns
stale/mismatch immediately so the background job can re-translate the
chapter cleanly.

### Why integer basis points for OCR confidence

Cortex-M85 and host/portable translation layers pass `TRN1` structures
across C and Zig ABI boundaries (and compile for soft-float targets such
as RISC-V). Storing `ocr_confidence_bps` as an unsigned 16-bit integer
in `[0, 10000]` (where `10000` represents `100.00%`) avoids ABI float
variance while preserving two decimal places of precision.

---

## 3. Wire format

All multi-byte integers are **little-endian**. There are no alignment padding
gaps between sections; `total_bytes` must equal the exact sum of the
header and all four section byte lengths.

```
  +----------------------------------------------------+  offset 0x0000
  | Header (96 bytes = 0x60)                           |
  +----------------------------------------------------+  offset 0x0060
  | PageEntry[page_count] (12 bytes each)              |
  +----------------------------------------------------+
  | BubbleRecord[bubble_count] (44 bytes each)         |
  +----------------------------------------------------+
  | line_break_table: u16[line_break_count] (2 B each) |
  +----------------------------------------------------+
  | text_pool: u8[text_pool_bytes] (UTF-8 bytes)       |
  +----------------------------------------------------+  total_bytes
```

### 3.1 Header (96 bytes at offset `0x00`)

| Offset | Width | Field | Meaning and valid range |
|--------|-------|-------|-------------------------|
| `0x00` | `u32` | `magic` | `0x314E5254` (bytes `"TRN1"`). |
| `0x04` | `u16` | `format_version` | Must be `1`. |
| `0x06` | `u16` | `flags` | Bitmask; reserved, must be `0` in v1. |
| `0x08` | `u32` | `total_bytes` | Exact byte length of the full sidecar; `>= 96`. |
| `0x0C` | `u32` | `file_crc32` | IEEE 802.3 CRC-32 over `[0..total_bytes)` with bytes `[0x0C..0x10)` zeroed. |
| `0x10` | `u8[32]` | `chapter_hash` | SHA-256 digest of the source chapter content. |
| `0x30` | `u32` | `detector_model_version` | Non-zero detector `.npub` model version. |
| `0x34` | `u32` | `ocr_model_version` | Non-zero OCR `.npub` model version. |
| `0x38` | `u32` | `nmt_model_version` | Non-zero NMT model + tokenizer version. |
| `0x3C` | `u32` | `glossary_revision` | Per-series glossary revision (`0` if unused). |
| `0x40` | `u8` | `source_lang` | `0=unknown`, `1=korean`, `2=japanese`, `3=chinese`. |
| `0x41` | `u8[3]` | `reserved0` | Must be zero (`{0, 0, 0}`). |
| `0x44` | `u16` | `page_count` | Number of `PageEntry` records (`0..65535`). |
| `0x46` | `u16` | `reserved1` | Must be `0`. |
| `0x48` | `u32` | `bubble_count` | Total `BubbleRecord` entries across all pages. |
| `0x4C` | `u32` | `line_break_count` | Total `u16` entries in `line_break_table`. |
| `0x50` | `u32` | `text_pool_bytes` | Exact byte length of `text_pool`. |
| `0x54` | `u8[12]` | `reserved2` | Must be all zeros in v1. |

### 3.2 `PageEntry` (12 bytes per page)

Pages appear in strictly ascending `page_index` order matching `0 .. page_count - 1`
or non-decreasing explicit chapter page numbers, and own contiguous
half-open intervals of `bubble_table`.

| Offset | Width | Field | Meaning and valid range |
|--------|-------|-------|-------------------------|
| `0x00` | `u16` | `page_index` | 0-based page index (`page[i].page_index == i`). |
| `0x02` | `u16` | `bubble_count` | Number of bubbles on this page (`<= 1024`). |
| `0x04` | `u32` | `first_bubble` | Start index into `bubble_table` (`page[0].first_bubble == 0`; contiguous). |
| `0x08` | `u16` | `page_width` | Page width in pixels (`> 0`). |
| `0x0A` | `u16` | `page_height` | Page height in pixels (`> 0`). |

### 3.3 `BubbleRecord` (44 bytes per bubble)

Within each page's slice `[first_bubble .. first_bubble + bubble_count)`,
`reading_order` values partition `0 .. page.bubble_count - 1` cleanly without
duplicates.

| Offset | Width | Field | Meaning and valid range |
|--------|-------|-------|-------------------------|
| `0x00` | `u16` | `x` | Bounding-box X coordinate in page pixels. |
| `0x02` | `u16` | `y` | Bounding-box Y coordinate in page pixels. |
| `0x04` | `u16` | `w` | Bounding-box width in pixels (`> 0`, `x + w <= page_width`). |
| `0x06` | `u16` | `h` | Bounding-box height in pixels (`> 0`, `y + h <= page_height`). |
| `0x08` | `u8` | `shape` | `0=rect`, `1=ellipse`, `2=rounded_rect`, `3=cloud`. |
| `0x09` | `u8` | `text_orientation` | `0=horizontal`, `1=vertical_rl`, `2=vertical_lr`. |
| `0x0A` | `u16` | `corner_radius` | Corner radius in pixels for `rounded_rect` (`0` otherwise). |
| `0x0C` | `u16` | `reading_order` | 0-based reading sequence index within the page (`< page.bubble_count`). |
| `0x0E` | `u16` | `ocr_confidence_bps` | OCR confidence in basis points (`0..10000`). |
| `0x10` | `u16` | `font_size_px` | Fitted English font height in pixels (`> 0` when `english_text_len > 0`). |
| `0x12` | `u16` | `line_count` | Number of fitted line-end offsets in `line_break_table`. |
| `0x14` | `u32` | `first_line_break` | Start index in `line_break_table` (contiguous across bubbles). |
| `0x18` | `u32` | `source_text_off` | Byte offset into `text_pool` for UTF-8 source text. |
| `0x1C` | `u32` | `source_text_len` | Byte length of UTF-8 source text. |
| `0x20` | `u32` | `english_text_off` | Byte offset into `text_pool` for UTF-8 English text. |
| `0x24` | `u32` | `english_text_len` | Byte length of UTF-8 English text (`<= 65535` when `line_count > 0`). |
| `0x28` | `u8` | `bubble_flags` | Bit `0`: glossary-matched term flag; bits `1..7` must be `0`. |
| `0x29` | `u8[3]` | `reserved` | Must be zero (`{0, 0, 0}`). |

### 3.4 `line_break_table` and `text_pool`

- **`line_break_table`:** `line_break_count` little-endian `u16` entries.
  A bubble's slice `line_break_table[first_line_break .. +line_count]`
  stores cumulative byte end offsets into `english_text`. When
  `english_text_len == 0`, `line_count` must be `0`. When `english_text_len > 0`,
  `line_count >= 1`, entries are strictly ascending (`0 < breaks[0] < ...`),
  each break falls on a UTF-8 codepoint boundary, and the final entry equals
  `english_text_len`. Line `k` covers `english_text[start..end]` where
  `start = (k == 0) ? 0 : breaks[k - 1]`.
- **`text_pool`:** Concatenated UTF-8 spans referenced by `(off, len)` in
  `BubbleRecord`. Both source and English spans must fall cleanly within
  `text_pool_bytes` and contain valid UTF-8.

---

## 4. Algorithms

### Writing (`writeSidecar`)

1. Validate header metadata (`detector_model_version`, `ocr_model_version`,
   `nmt_model_version` non-zero, valid `source_lang`).
2. Compute exact section offsets using checked additions to obtain `total_bytes`,
   verifying `out_buf.len >= total_bytes`.
3. Write zeroed `file_crc32` placeholder at `0x0C..0x10`, serialize `PageEntry`
   records, `BubbleRecord` records, `line_break_table` `u16` entries, and
   UTF-8 text bytes into `text_pool`, validating every invariant (`page_index`,
   bounding box fit, `ocr_confidence_bps <= 10000`, monotonic line breaks,
   `<= 65535` breakable length, valid UTF-8, and non-duplicate reading order).
4. Compute IEEE CRC-32 over `out_buf[0..total_bytes]` (with `0x0C..0x10` zero)
   and write `file_crc32` little-endian at `0x0C..0x10`.

### Opening and invalidation (`SidecarView.open`, `matchesKey`)

1. Check `bytes.len >= 96`, `magic == "TRN1"`, `format_version == 1`, `flags == 0`,
   reserved bytes all zero, non-zero model versions, valid `source_lang`, and
   `total_bytes == bytes.len`.
2. Recompute exact `96 + page_count*12 + bubble_count*44 + line_break_count*2 + text_pool_bytes`
   with overflow checks and verify equality with `total_bytes`.
3. Verify IEEE CRC-32 with `[0x0C..0x10)` treated as `0`.
4. Walk every `PageEntry` and `BubbleRecord` (bounded by `page_count` and
   `bubble_count`) to validate contiguous partitions, page bounds, unique
   `0 .. page.bubble_count - 1` `reading_order` values (bitmask/count check),
   UTF-8 validity, and monotonic line breaks.
5. `matchesKey(key)` checks exact equality of `chapter_hash`,
   `detector_model_version`, `ocr_model_version`, `nmt_model_version`,
   `glossary_revision`, and `source_lang`.

---

## 5. Memory behaviour

- **Writer:** Streams directly into caller-owned `out_buf[0..total_bytes]`
  using `O(1)` fixed stack scratch (small fixed reading-order check buffer
  per page or bounded stack check) and zero heap allocations.
- **Reader:** `SidecarView` holds only slice/header offsets over the caller's
  byte buffer (`bytes: []const u8`). Zero copies are made for strings or
  tables on read; text spans return slices directly into `text_pool`.

---

## 6. Worked example

One chapter with `1` page (`800 x 1200`), `1` rounded-rect bubble (`20, 40, 200, 80`),
OCR text `"hello_ko"` (8 bytes, confidence `9450` bps), English `"Hi there"`
wrapped across `2` lines (`"Hi "` and `"there"`, end offsets `[3, 8]` at font size `20` px):

```
0x0000: 54 52 4e 31  01 00 00 00   ; magic="TRN1", version=1, flags=0
0x0008: ac 00 00 00  <crc32 LE>    ; total_bytes = 96 + 12 + 44 + 4 + 16 = 172 (0xAC)
0x0010: <32-byte chapter_hash>     ; SHA-256 of source chapter
0x0030: 01 00 00 00  02 00 00 00   ; detector_ver=1, ocr_ver=2
0x0038: 03 00 00 00  04 00 00 00   ; nmt_ver=3, glossary_rev=4
0x0040: 01 00 00 00  01 00 00 00   ; source_lang=1(ko), res, page_count=1, res
0x0048: 01 00 00 00  02 00 00 00   ; bubble_count=1, line_break_count=2
0x0050: 10 00 00 00  00..00        ; text_pool_bytes=16, reserved2=0 (12 bytes)
0x0060: 00 00 01 00  00 00 00 00   ; Page 0: page_idx=0, bubble_cnt=1, first_bubble=0
0x0068: 20 03 b0 04                ;         w=800 (0x0320), h=1200 (0x04B0)
0x006C: 14 00 28 00  c8 00 50 00   ; Bubble 0: x=20, y=40, w=200, h=80
0x0074: 02 00 0c 00  00 00 ea 24   ;           shape=2(rounded), orient=0, r=12, order=0, conf=9450
0x007C: 14 00 02 00  00 00 00 00   ;           font=20, line_cnt=2, first_lb=0
0x0084: 00 00 00 00  08 00 00 00   ;           src_off=0, src_len=8
0x008C: 08 00 00 00  08 00 00 00   ;           en_off=8, en_len=8
0x0094: 00 00 00 00                ;           bubble_flags=0, reserved=0
0x0098: 03 00 08 00                ; line_break_table: [3, 8]
0x009C: <8B source> <8B "Hi there"> ; text_pool (16 bytes)
```

---

## 7. Failure modes

| Condition | Result | Why |
|-----------|--------|-----|
| Wrong magic, unsupported `format_version`, non-zero `flags`/reserved | Rejected on `open` | Prevents silent misinterpretation of future or corrupt containers. |
| Truncated buffer or `total_bytes != bytes.len` or section size overflow | Rejected on `open` | Bounds every table walk before indexing memory. |
| CRC-32 mismatch | Rejected on `open` | Detects storage truncation or bit flips before rendering overlays. |
| Bubble outside page box, duplicate `reading_order`, invalid enum/UTF-8, bad line break | Rejected on `open` | Keeps overlay renderer free of unchecked coordinates and malformed UTF-8 slices. |
| `chapter_hash`, model version, `glossary_revision`, or `source_lang` mismatch | `matchesKey` returns `false` | Forces re-translation when chapter bytes or any model/glossary changes. |

---

## 8. Versioning

- Minor backward-incompatible layout changes increment `format_version`.
  v1 readers accept only `format_version == 1` and `flags == 0`.
- Model upgrades do not require changing `format_version`: updating
  `detector_model_version`, `ocr_model_version`, `nmt_model_version`, or
  `glossary_revision` automatically invalidates prior `TRN1` files via
  `matchesKey()`.
