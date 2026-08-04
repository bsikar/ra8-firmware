/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_book.h
 * @brief Flat, execute-in-place container for a build-time "compiled" e-book.
 * @ingroup grp_ereader
 *
 * @details
 * `ra8_book` is the on-device representation of a book that has already been
 * unzipped, XML-parsed and image-transcoded on the host by
 * `tools/epub_compile`. The firmware never unzips or parses XHTML at runtime.
 * A `.rabook` file is a chunked container -- the magic "RBKC", a fixed header
 * naming the chunk size / inflated total / chunk count, a flat chunk table,
 * then one independent zlib stream per fixed-size slice of the flat blob --
 * so it stays compact on flash / SD *and* stays seekable: any chunk can be
 * inflated alone, without touching the rest of the file. `ra8_book_open()`
 * inflates every chunk once into a caller-provided SDRAM scratch buffer (the
 * resident fast path); `ra8_book_chunked.h` instead inflates single chunks on
 * demand into ra8_vmem cache frames so a book far larger than RAM is read with
 * a bounded working set. Either way the firmware then points a
 * `const ra8_book_header_t*` at inflated bytes and walks them with the inline
 * accessors below. Every internal reference is a byte offset relative to the
 * blob base, so the inflated structure is position-independent and walked
 * with no further parsing or copying.
 *
 * @par Fidelity
 * The format is a faithful pre-parsed DOM, NOT a lossy subset chosen to match
 * what the renderer understands today. Every element keeps its real tag name
 * and full attribute list; every stylesheet is preserved verbatim. A tag the
 * renderer cannot lay out yet is still present in the blob intact -- the fix
 * for unsupported markup is to grow the renderer, never to strip the content.
 * The only content that changes form is raster images, which are transcoded to
 * grayscale at their source resolution -- no downscale by default, so zoomable
 * content (manga pages) keeps every pixel; a long-edge clamp exists only as an
 * opt-in compile knob. The depth is the `pixel_format` axis: 4bpp panel-native
 * (a hardware limit, not a renderer one) for never-zoomed content, or
 * full-resolution continuous-tone 8bpp kept verbatim for zoomable content (#476),
 * since the zoom loupe and the e-ink dither need the 256-level tones the 4bpp
 * quantise discards. See @ref md_docs_2formats_2RBKC section 3.5.
 *
 * @par String interning
 * All strings (tag names, attribute names and values, text runs, hrefs,
 * metadata) live in a single string pool and are de-duplicated by the
 * compiler. Identical tag names therefore resolve to the same offset, so a
 * renderer can intern-compare tags by offset instead of `strcmp`.
 *
 * @par Layout
 * @code
 *   [ ra8_book_header_t                       ] fixed 100-byte header
 *   [ ra8_book_chapter_t   * chapter_count    ] spine / table of contents
 *   [ ra8_book_node_t      * node_count       ] DOM nodes (elements + text)
 *   [ ra8_book_attr_t      * attr_count       ] element attributes
 *   [ ra8_book_stylesheet_t * stylesheet_count] preserved CSS references
 *   [ ra8_book_image_t     * image_count      ] image descriptors
 *   [ string pool         : string_size      ] NUL-terminated UTF-8, deduped
 *   [ image pool          : image_pool_size  ] raw 4bpp grayscale / SVG bytes
 * @endcode
 *
 * @note The inline accessors are pure and do no bounds checking; validate a
 *       blob with ra8_book_validate() (or open it with ra8_book_open()) before
 *       walking it. The blob is immutable; nothing here writes to it.
 *
 * @see @ref md_docs_2formats_2RBKC -- the full RBKC wire-format specification
 *      (rationale, algorithms, worked example, failure modes).
 * @see tools/epub_compile  Host compiler that emits `.rabook` blobs.
 * @see ra8_reflow.h         Renderer that consumes the walked DOM.
 *
 * @since Version 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_book_version_t
 * @brief Wire-format version stamped into every blob header.
 * @details Bumped on any incompatible change to the on-disk layout. The
 *          loader rejects a blob whose `format_version` it does not recognise.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_book_format_version = 1U, /**< Current `.rabook` layout revision. */
} ra8_book_version_t;

/**
 * @enum ra8_book_container_t
 * @brief Constants of the on-disk `.rabook` chunked compression container.
 * @details A file is (all integers little-endian):
 * @code
 *   [0]  "RBKC" magic (4 bytes)
 *   [4]  uint32 chunk_bytes      inflated bytes per chunk (last chunk short)
 *   [8]  uint64 inflated_total   flat-blob length in bytes
 *   [16] uint32 chunk_count      == ceil(inflated_total / chunk_bytes)
 *   [20] uint32 reserved         must be 0
 *   [24] uint64 offset[chunk_count + 1]   chunk-table: each chunk's zlib
 *        stream starts at payload + offset[i] and ends at payload + offset[i+1];
 *        offset[0] == 0 and offset[chunk_count] == payload length
 *   [..] payload: chunk_count concatenated zlib (RFC 1950) streams
 * @endcode
 *          Chunk `i` inflates to exactly
 *          `min(chunk_bytes, inflated_total - i * chunk_bytes)` bytes of the
 *          flat blob described by @ref ra8_book_header_t, so any chunk can be
 *          inflated independently -- the property the ra8_vmem paged read path
 *          (ra8_book_chunked.h) relies on. Producers size `chunk_bytes` equal
 *          to the reader's ra8_vmem frame size so one chunk fills exactly one
 *          cache frame. @ref ra8_book_open() inflates all chunks (resident).
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_book_container_magic_len  = 4U,  /**< Length of the "RBKC" magic.                  */
  k_ra8_book_container_header_len = 24U, /**< Fixed header bytes ahead of the chunk table. */
  k_ra8_book_container_entry_len  = 8U,  /**< One chunk-table entry (uint64 LE offset).    */
} ra8_book_container_t;

/**
 * @enum ra8_book_sentinel_t
 * @brief Reserved index value meaning "no such element".
 * @details Used for `first_attr`, `first_child`, `next_sibling`,
 *          `cover_image_index` and a stylesheet's `scope_chapter` to mean
 *          "none" / "applies to all", since a real index can never be
 *          `0xFFFFFFFF` (the table count is bounded far below that).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_book_nil = 0xFFFFFFFFU, /**< Absent index / "applies to all chapters". */
} ra8_book_sentinel_t;

/**
 * @enum ra8_book_flag_t
 * @brief Feature-flag bits carried in `ra8_book_header_t.flags`.
 * @details Flags extend the v1 layout without a format-version bump: a bit may
 *          only change how content is *presented*, never where any table or
 *          pool lives. ra8_book_validate() rejects a blob whose `flags` sets any
 *          bit outside ::k_ra8_book_flag_mask_known, so an old firmware never
 *          silently mis-renders a book that depends on a semantic it does not
 *          implement. Kept in lockstep with `FLAG_RTL` in
 *          `tools/epub_compile/epub_compile.py` (emitted by the CBZ arm,
 *          `tools/epub_compile/cbz_compile.py`, under `--rtl`).
 * @invariant ::k_ra8_book_flag_mask_known is the OR of every other enumerator.
 * @see ra8_book_is_rtl()
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_book_flag_rtl        = 0x00000001U, /**< Right-to-left reading order (manga):
                                            *   spine pages advance leaf-first, and the
                                            *   reader mirrors its page-turn zones.     */
  k_ra8_book_flag_mask_known = 0x00000001U, /**< Every bit this firmware understands; a
                                            *   set bit outside this mask fails
                                            *   ra8_book_validate().                     */
} ra8_book_flag_t;

/**
 * @enum ra8_book_node_kind_t
 * @brief Discriminates the two kinds of DOM node.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_book_node_element = 0U, /**< An element: has a tag name and attributes. */
  k_ra8_book_node_text    = 1U, /**< A text run: carries a string, no children. */
} ra8_book_node_kind_t;

/**
 * @enum ra8_book_image_format_t
 * @brief Pixel encoding of an entry in the image pool.
 * @details Raster images use panel-native 4-bit grayscale; SVG keeps its vector
 *          source for on-device rasterization. An enum so future encodings can
 *          be added without a flag-day.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_book_image_gray4 =
    0U,                      /**< 4bpp gray, 2px/byte; pixel (x,y) is at flat index y*width + x. */
  k_ra8_book_image_svg = 1U, /**< Verbatim UTF-8 SVG source (vector; on-device rasterized).      */
} ra8_book_image_format_t;

/**
 * @enum ra8_book_image_pixfmt_t
 * @brief Pixel depth of a @ref k_ra8_book_image_gray4 raster in the image pool.
 * @details A second axis, orthogonal to @ref ra8_book_image_format_t: that field
 *          only says whether an entry is a packed-grayscale raster or verbatim
 *          SVG, while this one -- modelled on JOF's `bpp` header field -- records
 *          how DEEP that raster is. A grayscale e-reader can then rasterize at
 *          4bpp (half the storage, and exactly right for an even 16-level e-ink
 *          panel) while a device with more headroom carries the lossless 8bpp
 *          source; the compiler picks the depth by device profile instead of
 *          baking 4bpp in for every reader.
 *
 *          It lives in the descriptor's former padding byte
 *          (@ref ra8_book_image_t::pixel_format), so it costs no format growth and
 *          needs no @ref ra8_book_version_t bump: every `.rabook` written before
 *          this field existed zero-filled that byte, and 0 is
 *          @ref k_ra8_book_pixfmt_gray4 -- exactly the 4bpp packing those blobs
 *          already carried. New firmware therefore reads every pre-existing blob
 *          unchanged (backward-read); an unknown depth is rejected by
 *          ra8_book_validate() rather than silently mis-rendered.
 * @invariant An @ref k_ra8_book_image_svg entry stores 0 here (the field is
 *            meaningful only for a raster; callers branch on `format` first).
 * @code
 *   const ra8_book_image_t* img = &ra8_book_images(base)[i];
 *   if (img->format == k_ra8_book_image_gray4 &&
 *       ra8_book_image_pixfmt(img) == k_ra8_book_pixfmt_gray8) {
 *     ra8_gfx_blit_gray8(ra8_book_image_data(base, img), img->width, img->height, x, y);
 *   }
 * @endcode
 * @see ra8_book_image_pixfmt()
 * @see ra8_book_image_t
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_book_pixfmt_gray4 =
    0U, /**< 4bpp packed grayscale, 2px/byte (default; every pre-field blob). */
  /** 8bpp grayscale, 1px/byte (lossless against any grey panel). */
  k_ra8_book_pixfmt_gray8 = 1U,
} ra8_book_image_pixfmt_t;

/**
 * @enum ra8_book_struct_size_t
 * @brief Pinned on-disk sizes of the blob's fixed-layout records.
 * @details The format is a binary wire layout shared with the host compiler, so
 *          each record's byte size is part of the contract. These named
 *          constants drive the `static_assert`s that guard against accidental
 *          padding or a silent field change.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_book_sizeof_header     = 100U, /**< Bytes in @ref ra8_book_header_t.     */
  k_ra8_book_sizeof_chapter    = 12U,  /**< Bytes in @ref ra8_book_chapter_t.    */
  k_ra8_book_sizeof_node       = 24U,  /**< Bytes in @ref ra8_book_node_t.       */
  k_ra8_book_sizeof_attr       = 8U,   /**< Bytes in @ref ra8_book_attr_t.       */
  k_ra8_book_sizeof_stylesheet = 8U,   /**< Bytes in @ref ra8_book_stylesheet_t. */
  k_ra8_book_sizeof_image      = 24U,  /**< Bytes in @ref ra8_book_image_t.      */
} ra8_book_struct_size_t;

/**
 * @struct ra8_book_header_t
 * @brief Fixed 100-byte prologue describing every table and pool in the blob.
 * @details All `_off` fields are byte offsets from the blob base; all `_count`
 *          / `_size` fields are element counts / byte lengths. Metadata string
 *          offsets index the string pool.
 * @invariant `total_size` equals the exact byte length of the blob.
 * @invariant Every `_off` plus its table extent lies within `[0, total_size]`.
 * @since Version 0.1.0
 */
typedef struct {
  char     magic[8];          /**< Always "RABOOK1" (7 chars + NUL).                          */
  uint32_t format_version;    /**< @ref ra8_book_version_t of this blob.                      */
  uint32_t total_size;        /**< Total blob length in bytes.                                */
  uint32_t flags;             /**< @ref ra8_book_flag_t bits; unknown bits are rejected.      */
  uint32_t title_off;         /**< String-pool offset of the book title.                      */
  uint32_t author_off;        /**< String-pool offset of the author.                          */
  uint32_t language_off;      /**< String-pool offset of the BCP-47 language.                 */
  uint32_t identifier_off;    /**< String-pool offset of the unique book id.                  */
  uint32_t cover_image_index; /**< Image-table index of the cover, or nil.                    */
  uint32_t chapter_count;     /**< Number of spine chapters.                                  */
  uint32_t chapter_off;       /**< Offset to the chapter table.                               */
  uint32_t node_count;        /**< Number of DOM nodes.                                       */
  uint32_t node_off;          /**< Offset to the node table.                                  */
  uint32_t attr_count;        /**< Number of attribute records.                               */
  uint32_t attr_off;          /**< Offset to the attribute table.                             */
  uint32_t stylesheet_count;  /**< Number of preserved stylesheets.                           */
  uint32_t stylesheet_off;    /**< Offset to the stylesheet table.                            */
  uint32_t image_count;       /**< Number of image descriptors.                               */
  uint32_t image_off;         /**< Offset to the image table.                                 */
  uint32_t string_off;        /**< Offset to the string pool.                                 */
  uint32_t string_size;       /**< String-pool length in bytes.                               */
  uint32_t image_pool_off;    /**< Offset to the image pool.                                  */
  uint32_t image_pool_size;   /**< Image-pool length in bytes.                                */
  uint32_t crc32;             /**< CRC-32/ISO-HDLC of the body (all bytes after this header). */
} ra8_book_header_t;

static_assert(sizeof(ra8_book_header_t) == k_ra8_book_sizeof_header,
              "ra8_book_header_t size pinned");

/**
 * @struct ra8_book_chapter_t
 * @brief One spine document (a renderable chapter) plus its TOC label.
 * @since Version 0.1.0
 */
typedef struct {
  uint32_t title_off; /**< String-pool offset of the TOC label ("" if none). */
  uint32_t href_off;  /**< String-pool offset of the source spine href.      */
  uint32_t root_node; /**< Node-table index of this chapter's root element.  */
} ra8_book_chapter_t;

static_assert(sizeof(ra8_book_chapter_t) == k_ra8_book_sizeof_chapter,
              "ra8_book_chapter_t size pinned");

/**
 * @struct ra8_book_node_t
 * @brief One DOM node. Children form a singly-linked sibling chain.
 * @details Walk an element's children by reading `first_child` then following
 *          each node's `next_sibling` until @ref k_ra8_book_nil. Element nodes
 *          set `name_off` (tag name) and may have attributes; text nodes set
 *          `text_off` and have neither attributes nor children.
 * @invariant `kind == k_ra8_book_node_text` implies `attr_count == 0`,
 *            `first_attr == nil` and `first_child == nil`.
 * @since Version 0.1.0
 */
typedef struct {
  uint8_t  kind;         /**< @ref ra8_book_node_kind_t.                         */
  uint8_t  reserved;     /**< Padding; 0.                                        */
  uint16_t attr_count;   /**< Element: number of attributes (text: 0).           */
  uint32_t name_off;     /**< Element: string-pool offset of tag name (text: 0). */
  uint32_t text_off;     /**< Text: string-pool offset of the run (element: 0).  */
  uint32_t first_attr;   /**< Index of first attribute, or nil.                  */
  uint32_t first_child;  /**< Index of first child node, or nil.                 */
  uint32_t next_sibling; /**< Index of next sibling node, or nil.                */
} ra8_book_node_t;

static_assert(sizeof(ra8_book_node_t) == k_ra8_book_sizeof_node, "ra8_book_node_t size pinned");

/**
 * @struct ra8_book_attr_t
 * @brief One `name="value"` attribute on an element.
 * @since Version 0.1.0
 */
typedef struct {
  uint32_t name_off;  /**< String-pool offset of the attribute name.  */
  uint32_t value_off; /**< String-pool offset of the attribute value. */
} ra8_book_attr_t;

static_assert(sizeof(ra8_book_attr_t) == k_ra8_book_sizeof_attr, "ra8_book_attr_t size pinned");

/**
 * @struct ra8_book_stylesheet_t
 * @brief A preserved CSS stylesheet and the chapter it scopes to.
 * @details `source_off` points at the verbatim stylesheet text so the CSS
 *          cascade engine receives the real rules. `scope_chapter` is a chapter
 *          index, or @ref k_ra8_book_nil for a book-wide stylesheet.
 * @since Version 0.1.0
 */
typedef struct {
  uint32_t source_off;    /**< String-pool offset of the verbatim CSS text.   */
  uint32_t scope_chapter; /**< Chapter index this applies to, or nil (= all). */
} ra8_book_stylesheet_t;

static_assert(sizeof(ra8_book_stylesheet_t) == k_ra8_book_sizeof_stylesheet,
              "ra8_book_stylesheet_t size pinned");

/**
 * @struct ra8_book_image_t
 * @brief Descriptor for one transcoded image in the image pool.
 * @details `id_off` is the original manifest href so an `<img src>` attribute
 *          value resolves to this entry. Raster images are packed grayscale at
 *          source resolution (panel-ready, no decode; an opt-in compile knob can
 *          clamp the long edge) -- 4bpp (2px/byte) or 8bpp (1px/byte) per
 *          `pixel_format`; SVG is kept as verbatim vector source. Pool bytes are
 *          raw -- the blob is chunk-DEFLATE-wrapped on disk (see
 *          @ref ra8_book_container_t) and inflated on open, so per-image
 *          compression would not help -- thus `data_size == raw_size` and
 *          `data_off` indexes the image pool.
 * @since Version 0.1.0
 */
typedef struct {
  uint32_t id_off;       /**< String-pool offset of the source href / manifest id. */
  uint16_t width;        /**< Pixel width.                                         */
  uint16_t height;       /**< Pixel height.                                        */
  uint8_t  format;       /**< @ref ra8_book_image_format_t.                        */
  uint8_t  pixel_format; /**< @ref ra8_book_image_pixfmt_t (0 == gray4; SVG: 0).   */
  uint16_t reserved2;    /**< Padding; 0.                                          */
  uint32_t data_off;     /**< Image-pool offset of the compressed pixel data.      */
  uint32_t data_size;    /**< Compressed length in bytes.                          */
  uint32_t raw_size;     /**< Inflated length in bytes.                            */
} ra8_book_image_t;

static_assert(sizeof(ra8_book_image_t) == k_ra8_book_sizeof_image, "ra8_book_image_t size pinned");

/* -------------------------------------------------------------------------- */
/* Inline accessors. All take a validated blob base; see ra8_book_validate(). */
/* -------------------------------------------------------------------------- */

/**
 * @brief View the blob base as its header.
 * @param[in] base Pointer to the first byte of a `.rabook` blob (non-NULL).
 * @return The header view.
 * @pre `base` points at a blob already accepted by ra8_book_validate().
 * @pre `base` is at least `alignof(uint32_t)`-aligned.
 * @post Returned pointer aliases `base`; never NULL when `base` is non-NULL.
 * @note Thread-safe: read-only over immutable data.
 * @since Version 0.1.0
 */
static inline const ra8_book_header_t* ra8_book_header(const void* base)
{
  return (const ra8_book_header_t*)base;
}

/**
 * @brief Whether the book declares right-to-left reading order (manga).
 *
 * @details
 * Reads ::k_ra8_book_flag_rtl out of the header's `flags` word. An RTL book's
 * spine is still stored first-page-first; the flag tells the *reader* to
 * mirror its presentation (page-turn zones, spread direction). Consuming the
 * flag in the page-turn UI is the reader's job; this accessor only decodes it.
 *
 * @param[in] base Pointer to the first byte of a `.rabook` blob (non-NULL).
 *
 * @return Whether the RTL flag bit is set.
 * @retval true  The book reads right-to-left.
 * @retval false The book reads left-to-right (every v1 blob before the flag).
 *
 * @pre `base` points at a blob already accepted by ra8_book_validate().
 * @pre `base` is at least `alignof(uint32_t)`-aligned.
 * @post The blob is not modified (pure read).
 * @post The result is stable for the lifetime of the immutable blob.
 *
 * @note Thread-safe: read-only over immutable data.
 * @see ra8_book_flag_t
 * @since Version 0.1.0
 */
static inline bool ra8_book_is_rtl(const void* base)
{
  return (ra8_book_header(base)->flags & (uint32_t)k_ra8_book_flag_rtl) != 0U;
}

/**
 * @brief Resolve a blob-relative byte offset to a pointer.
 * @param[in] base Blob base (non-NULL).
 * @param[in] off  Byte offset within the blob (validated `< total_size`).
 * @return `base + off`.
 * @pre `base` non-NULL and validated.
 * @pre `off` lies inside the blob.
 * @post Result points within `[base, base + total_size)`.
 * @note Thread-safe: pure pointer arithmetic.
 * @since Version 0.1.0
 */
static inline const void* ra8_book_at(const void* base, uint32_t off)
{
  return (const void*)((const uint8_t*)base + off);
}

/**
 * @brief Resolve a string-pool offset to a NUL-terminated UTF-8 string.
 * @param[in] base Blob base (non-NULL, validated).
 * @param[in] off  String-pool offset.
 * @return Pointer to the interned string.
 * @pre `base` validated.
 * @pre `off` is within the string pool.
 * @post Result is a NUL-terminated string inside the blob.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const char* ra8_book_string(const void* base, uint32_t off)
{
  return (const char*)ra8_book_at(base, ra8_book_header(base)->string_off + off);
}

/**
 * @brief Base of the chapter table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to chapter[0]; read `ra8_book_header(base)->chapter_count` entries.
 * @pre `base` validated.
 * @pre `ra8_book_header(base)->chapter_count > 0` for the result to be dereferenceable.
 * @post Result indexes a contiguous array of `chapter_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const ra8_book_chapter_t* ra8_book_chapters(const void* base)
{
  return (const ra8_book_chapter_t*)ra8_book_at(base, ra8_book_header(base)->chapter_off);
}

/**
 * @brief Base of the DOM node table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to node[0]; indices `first_child`/`next_sibling`/`root_node` index here.
 * @pre `base` validated.
 * @pre `ra8_book_header(base)->node_count > 0`.
 * @post Result indexes a contiguous array of `node_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const ra8_book_node_t* ra8_book_nodes(const void* base)
{
  return (const ra8_book_node_t*)ra8_book_at(base, ra8_book_header(base)->node_off);
}

/**
 * @brief Base of the attribute table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to attr[0]; a node's `first_attr` indexes here.
 * @pre `base` validated.
 * @pre `ra8_book_header(base)->attr_count > 0`.
 * @post Result indexes a contiguous array of `attr_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const ra8_book_attr_t* ra8_book_attrs(const void* base)
{
  return (const ra8_book_attr_t*)ra8_book_at(base, ra8_book_header(base)->attr_off);
}

/**
 * @brief Base of the stylesheet table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to stylesheet[0].
 * @pre `base` validated.
 * @pre `ra8_book_header(base)->stylesheet_count > 0`.
 * @post Result indexes a contiguous array of `stylesheet_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const ra8_book_stylesheet_t* ra8_book_stylesheets(const void* base)
{
  return (const ra8_book_stylesheet_t*)ra8_book_at(base, ra8_book_header(base)->stylesheet_off);
}

/**
 * @brief Base of the image table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to image[0]; `cover_image_index` indexes here.
 * @pre `base` validated.
 * @pre `ra8_book_header(base)->image_count > 0`.
 * @post Result indexes a contiguous array of `image_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const ra8_book_image_t* ra8_book_images(const void* base)
{
  return (const ra8_book_image_t*)ra8_book_at(base, ra8_book_header(base)->image_off);
}

/**
 * @brief Tag name of an element node.
 * @param[in] base Blob base (non-NULL, validated).
 * @param[in] node Element node (non-NULL, `kind == k_ra8_book_node_element`).
 * @return The interned, NUL-terminated tag name.
 * @pre `node` belongs to `base` and is an element.
 * @pre `base` validated.
 * @post Result is a string inside the blob.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const char* ra8_book_node_name(const void* base, const ra8_book_node_t* node)
{
  return ra8_book_string(base, node->name_off);
}

/**
 * @brief Text of a text node.
 * @param[in] base Blob base (non-NULL, validated).
 * @param[in] node Text node (non-NULL, `kind == k_ra8_book_node_text`).
 * @return The NUL-terminated UTF-8 run.
 * @pre `node` belongs to `base` and is a text node.
 * @pre `base` validated.
 * @post Result is a string inside the blob.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const char* ra8_book_node_text(const void* base, const ra8_book_node_t* node)
{
  return ra8_book_string(base, node->text_off);
}

/**
 * @brief Image-pool pointer to one image's compressed pixel data.
 * @param[in] base Blob base (non-NULL, validated).
 * @param[in] img  Image descriptor (non-NULL).
 * @return Pointer to `img->data_size` DEFLATE bytes.
 * @pre `img` belongs to `base`.
 * @pre `base` validated.
 * @post Result points within the image pool.
 * @note Thread-safe: read-only.
 * @since Version 0.1.0
 */
static inline const uint8_t* ra8_book_image_data(const void* base, const ra8_book_image_t* img)
{
  return (const uint8_t*)ra8_book_at(base, ra8_book_header(base)->image_pool_off) + img->data_off;
}

/**
 * @brief Declared pixel depth of one image descriptor.
 *
 * @details
 * Decodes @ref ra8_book_image_t::pixel_format into its @ref ra8_book_image_pixfmt_t.
 * Meaningful only for a @ref k_ra8_book_image_gray4 raster (4bpp vs 8bpp packing);
 * an SVG entry stores 0 and reports @ref k_ra8_book_pixfmt_gray4, so a renderer
 * must branch on @ref ra8_book_image_t::format first and only consult this for a
 * raster. Every blob written before the field existed zero-filled the byte, so
 * an old gray4 blob reports @ref k_ra8_book_pixfmt_gray4 unchanged (backward-read).
 *
 * @param[in] img Image descriptor (non-NULL) obtained from ra8_book_images().
 *
 * @return The declared pixel depth.
 * @retval k_ra8_book_pixfmt_gray4 4bpp packed grayscale (2px/byte); also every
 *                                 pre-field blob and every SVG entry.
 * @retval k_ra8_book_pixfmt_gray8 8bpp grayscale (1px/byte).
 *
 * @pre `img` belongs to a blob accepted by ra8_book_validate().
 * @pre `img` is non-NULL.
 * @post The blob is not modified (pure read).
 * @post The result is a depth ra8_book_validate() already accepted as known.
 *
 * @note Thread-safe: read-only over immutable data.
 * @see ra8_book_image_pixfmt_t
 * @see ra8_book_image_data()
 * @since Version 0.1.0
 */
static inline ra8_book_image_pixfmt_t ra8_book_image_pixfmt(const ra8_book_image_t* img)
{
  return (ra8_book_image_pixfmt_t)img->pixel_format;
}

/**
 * @brief Validate that a byte buffer is a well-formed, intact `.rabook` blob.
 *
 * @details
 * Checks, in order: the magic and `format_version`; that `flags` sets no bit
 * outside ::k_ra8_book_flag_mask_known (a blob relying on a presentation
 * semantic this firmware does not implement must be rejected, not mis-read);
 * that `total_size` fits in `size`; that every table offset plus its extent
 * and every pool lie within `total_size`; that every image descriptor names a
 * @ref ra8_book_image_pixfmt_t this build can unpack (an unknown depth is refused
 * rather than mis-blitted); and finally the CRC-32 of the body. Must be called
 * once before any accessor is used on `base`; the accessors assume a validated
 * blob and do no bounds checking themselves (they are pure offset arithmetic for
 * XIP).
 *
 * @param[in] base Pointer to the candidate blob (may be NULL).
 * @param[in] size Number of readable bytes at `base`.
 *
 * @return Error code.
 * @retval k_ra8_ok             Blob is well-formed and CRC matches.
 * @retval k_ra8_err_null_ptr   `base` is NULL.
 * @retval k_ra8_err_invalid_arg  Magic is wrong, the format version is unknown,
 *                               `flags` carries an unknown feature bit, or an
 *                               image declares an unknown pixel format.
 * @retval k_ra8_err_invalid_size `size` is too small or a table/pool runs past `total_size`.
 * @retval k_ra8_err_range_check_failed CRC-32 of the body does not match the header.
 *
 * @pre `size` is the true readable length at `base` (no over-read).
 * @pre `base` is `alignof(uint32_t)`-aligned when non-NULL.
 * @post On `k_ra8_ok`, every accessor on `base` stays within `[base, base + total_size)`.
 * @post On any error, `base` must not be passed to other accessors.
 *
 * @note Thread-safe: reads only the immutable candidate buffer.
 * @see ra8_book_header()
 * @since Version 0.1.0
 */
ra8_err_t ra8_book_validate(const void* base, size_t size);

/**
 * @typedef ra8_book_inflate_fn
 * @brief Caller-supplied DEFLATE inflater used by @ref ra8_book_open().
 * @details Keeps `ra8_book` decoupled from any one decompressor: the firmware
 *          passes a miniz-backed inflater, host tests pass a zlib one.
 * @param[in]  src     Start of the raw DEFLATE stream.
 * @param[in]  src_len DEFLATE stream length in bytes.
 * @param[out] dst     Destination buffer to inflate into.
 * @param[in]  dst_cap Capacity of `dst` in bytes.
 * @param[out] out_len Receives the number of bytes written to `dst`.
 * @return `k_ra8_ok` on success, any non-zero @ref ra8_err_t on failure.
 * @since Version 0.1.0
 */
typedef ra8_err_t (*ra8_book_inflate_fn)(const void* src,
                                         size_t      src_len,
                                         void*       dst,
                                         size_t      dst_cap,
                                         size_t*     out_len);

/**
 * @brief Open a `.rabook` file: check the container, inflate, validate the blob.
 *
 * @details
 * Parses the "RBKC" container header and chunk table (see
 * @ref ra8_book_container_t for the layout), inflates every chunk's zlib stream
 * in order into the caller-owned `scratch` buffer (expected to live in SDRAM),
 * then runs ra8_book_validate() over the reassembled flat blob. On success
 * `*out_base` is the validated blob base (equal to `scratch`) ready for the
 * inline accessors. This is the *resident* open -- the whole inflated blob must
 * fit `scratch_cap`; a book larger than the resident budget is instead read
 * chunk-by-chunk through ra8_book_chunked.h + ra8_book_src_paged().
 *
 * @param[in]  file        Pointer to the `.rabook` file bytes (non-NULL).
 * @param[in]  file_len    Length of `file` in bytes.
 * @param[in]  inflate     Decompressor callback (see @ref ra8_book_inflate_fn).
 * @param[out] scratch     Buffer that receives the inflated blob (non-NULL).
 * @param[in]  scratch_cap Capacity of `scratch`; must be >= the inflated total.
 * @param[out] out_base    Receives the validated blob base on success.
 * @param[out] out_size    Receives the inflated blob length on success.
 *
 * @return Error code.
 * @retval k_ra8_ok               Container valid, inflated, and blob validated.
 * @retval k_ra8_err_null_ptr     A required pointer argument is NULL.
 * @retval k_ra8_err_invalid_arg  Container magic / header geometry / chunk table
 *                               is malformed (bad magic, zero chunk size, count
 *                               disagreeing with the total, non-monotonic table,
 *                               table end disagreeing with the payload length).
 * @retval k_ra8_err_invalid_size File too short, `scratch_cap` too small, or a
 *                               chunk inflated to a length other than its span.
 * @retval k_ra8_err_range_check_failed Blob CRC mismatch (from ra8_book_validate()).
 *
 * @pre `file_len` is the true readable length at `file`.
 * @pre `scratch` is `alignof(uint32_t)`-aligned.
 * @post On `k_ra8_ok`, `*out_base == scratch` and accessors stay within it.
 * @post On any error, `scratch` contents are unspecified and must not be walked.
 *
 * @note Thread-safe if `inflate` is and `scratch` is not shared concurrently.
 * @see ra8_book_validate()
 * @since Version 0.1.0
 */
ra8_err_t ra8_book_open(const void*         file,
                        size_t              file_len,
                        ra8_book_inflate_fn inflate,
                        void*               scratch,
                        size_t              scratch_cap,
                        const void**        out_base,
                        size_t*             out_size);

/**
 * @brief Serialize one chapter's DOM subtree back to XHTML for the renderer.
 *
 * @details
 * A bridge for feeding a compiled book into `ra8_reflow_layout_chapter()`, which
 * consumes XHTML. Walks the chapter's element/text tree iteratively (no
 * recursion, bounded stack) and writes well-formed XHTML -- every tag,
 * attribute and text run, faithfully -- into @p out. Void elements self-close;
 * text and attribute values are entity-escaped. The output is NOT
 * NUL-terminated.
 *
 * @param[in]  base        Validated `ra8_book` blob base (non-NULL).
 * @param[in]  chapter_idx Spine chapter index (`< header chapter_count`).
 * @param[out] out         Destination XHTML buffer (non-NULL).
 * @param[in]  cap         Capacity of @p out in bytes.
 * @param[out] out_len     Receives the XHTML byte length written.
 *
 * @return Error code.
 * @retval k_ra8_ok               Chapter serialized; @p out_len bytes written.
 * @retval k_ra8_err_null_ptr     A required pointer argument is NULL.
 * @retval k_ra8_err_invalid_arg  @p chapter_idx is out of range.
 * @retval k_ra8_err_invalid_size Output did not fit @p cap, or DOM nesting
 *                               exceeded the bounded walk stack.
 *
 * @pre @p base was accepted by ra8_book_validate() / ra8_book_open().
 * @pre @p cap is large enough for the chapter's serialized XHTML.
 * @post On k_ra8_ok, `out[0..*out_len)` is well-formed (not NUL-terminated).
 * @post On error, @p out contents are unspecified.
 *
 * @note Thread-safe: reads only the immutable blob, writes only @p out.
 * @see ra8_reflow_layout_chapter()
 * @since Version 0.1.0
 */
ra8_err_t ra8_book_chapter_to_xhtml(const void* base,
                                    uint32_t    chapter_idx,
                                    char*       out,
                                    size_t      cap,
                                    size_t*     out_len);

/**
 * @brief Extract one chapter's readable plain text from the DOM.
 *
 * @details
 * Walks the chapter's element/text tree iteratively (no recursion, bounded
 * stack) and writes its text runs into @p out, inserting a newline at each
 * block-level element so paragraphs stay separated. Markup, attributes and
 * inline structure are dropped -- this is for a simple word-wrap reader, not
 * rich layout. The output is NOT NUL-terminated.
 *
 * @param[in]  base        Validated `ra8_book` blob base (non-NULL).
 * @param[in]  chapter_idx Spine chapter index (`< header chapter_count`).
 * @param[out] out         Destination text buffer (non-NULL).
 * @param[in]  cap         Capacity of @p out in bytes.
 * @param[out] out_len     Receives the text byte length written.
 *
 * @return Error code.
 * @retval k_ra8_ok               Chapter text extracted.
 * @retval k_ra8_err_null_ptr     A required pointer argument is NULL.
 * @retval k_ra8_err_invalid_arg  @p chapter_idx is out of range.
 * @retval k_ra8_err_invalid_size Output did not fit @p cap, or DOM nesting
 *                               exceeded the bounded walk stack.
 *
 * @pre @p base was accepted by ra8_book_validate() / ra8_book_open().
 * @pre @p chapter_idx is less than `ra8_book_header(base)->chapter_count`.
 * @post On k_ra8_ok, `out[0..*out_len)` contains the plain-text run (not NUL-terminated).
 * @post On error, @p out contents are unspecified.
 *
 * @note Thread-safe: reads only the immutable blob, writes only @p out.
 * @since Version 0.1.0
 */
ra8_err_t ra8_book_chapter_text(const void* base,
                                uint32_t    chapter_idx,
                                char*       out,
                                size_t      cap,
                                size_t*     out_len);
