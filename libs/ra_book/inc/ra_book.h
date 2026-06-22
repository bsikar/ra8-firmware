/**
 * @file ra_book.h
 * @brief Flat, execute-in-place container for a build-time "compiled" e-book.
 *
 * @details
 * `ra_book` is the on-device representation of a book that has already been
 * unzipped, XML-parsed and image-transcoded on the host by
 * `tools/epub_compile`. The firmware never unzips or parses XHTML at runtime.
 * A `.rabook` file is a small container -- the magic "RBKZ", the inflated size,
 * then a DEFLATE stream of the flat blob -- so it stays compact on flash / SD.
 * `ra_book_open()` inflates it once into a caller-provided SDRAM scratch buffer;
 * thereafter the firmware points a `const ra_book_header_t*` at the inflated
 * blob and walks it with the inline accessors below. Every internal reference
 * is a byte offset relative to the blob base, so the inflated structure is
 * position-independent and walked with no further parsing or copying.
 *
 * @par Fidelity
 * The format is a faithful pre-parsed DOM, NOT a lossy subset chosen to match
 * what the renderer understands today. Every element keeps its real tag name
 * and full attribute list; every stylesheet is preserved verbatim. A tag the
 * renderer cannot lay out yet is still present in the blob intact -- the fix
 * for unsupported markup is to grow the renderer, never to strip the content.
 * The only content that changes form is raster images, which are transcoded to
 * the panel's native 4-bit grayscale (a hardware limit, not a renderer one) and
 * downscaled to panel-class resolution.
 *
 * @par String interning
 * All strings (tag names, attribute names and values, text runs, hrefs,
 * metadata) live in a single string pool and are de-duplicated by the
 * compiler. Identical tag names therefore resolve to the same offset, so a
 * renderer can intern-compare tags by offset instead of `strcmp`.
 *
 * @par Layout
 * @code
 *   [ ra_book_header_t                       ] fixed 100-byte header
 *   [ ra_book_chapter_t   * chapter_count    ] spine / table of contents
 *   [ ra_book_node_t      * node_count       ] DOM nodes (elements + text)
 *   [ ra_book_attr_t      * attr_count       ] element attributes
 *   [ ra_book_stylesheet_t * stylesheet_count] preserved CSS references
 *   [ ra_book_image_t     * image_count      ] image descriptors
 *   [ string pool         : string_size      ] NUL-terminated UTF-8, deduped
 *   [ image pool          : image_pool_size  ] raw 4bpp grayscale / SVG bytes
 * @endcode
 *
 * @note The inline accessors are pure and do no bounds checking; validate a
 *       blob with ra_book_validate() (or open it with ra_book_open()) before
 *       walking it. The blob is immutable; nothing here writes to it.
 *
 * @see tools/epub_compile  Host compiler that emits `.rabook` blobs.
 * @see ra_reflow.h         Renderer that consumes the walked DOM.
 *
 * @since Version 1.0.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_book_version_t
 * @brief Wire-format version stamped into every blob header.
 * @details Bumped on any incompatible change to the on-disk layout. The
 *          loader rejects a blob whose `format_version` it does not recognise.
 * @since Version 1.0.0
 */
typedef enum : uint32_t {
  k_ra_book_format_version = 1U, /**< Current `.rabook` layout revision. */
} ra_book_version_t;

/**
 * @enum ra_book_container_t
 * @brief Constants of the on-disk `.rabook` compression container.
 * @details A file is `["RBKZ"][little-endian uint32 inflated_size][DEFLATE stream]`.
 *          The stream inflates to the flat blob described by @ref ra_book_header_t.
 *          @ref ra_book_open() unwraps it.
 * @since Version 1.0.0
 */
typedef enum : uint8_t {
  k_ra_book_container_magic_len  = 4U, /**< Length of the "RBKZ" magic.                  */
  k_ra_book_container_header_len = 8U, /**< "RBKZ" (4 bytes) + inflated size (uint32 LE). */
} ra_book_container_t;

/**
 * @enum ra_book_sentinel_t
 * @brief Reserved index value meaning "no such element".
 * @details Used for `first_attr`, `first_child`, `next_sibling`,
 *          `cover_image_index` and a stylesheet's `scope_chapter` to mean
 *          "none" / "applies to all", since a real index can never be
 *          `0xFFFFFFFF` (the table count is bounded far below that).
 * @since Version 1.0.0
 */
typedef enum : uint32_t {
  k_ra_book_nil = 0xFFFFFFFFU, /**< Absent index / "applies to all chapters". */
} ra_book_sentinel_t;

/**
 * @enum ra_book_node_kind_t
 * @brief Discriminates the two kinds of DOM node.
 * @since Version 1.0.0
 */
typedef enum : uint8_t {
  k_ra_book_node_element = 0U, /**< An element: has a tag name and attributes. */
  k_ra_book_node_text    = 1U, /**< A text run: carries a string, no children. */
} ra_book_node_kind_t;

/**
 * @enum ra_book_image_format_t
 * @brief Pixel encoding of an entry in the image pool.
 * @details Raster images use panel-native 4-bit grayscale; SVG keeps its vector
 *          source for on-device rasterization. An enum so future encodings can
 *          be added without a flag-day.
 * @since Version 1.0.0
 */
typedef enum : uint8_t {
  k_ra_book_image_gray4 = 0U, /**< 4bpp gray, 2px/byte; pixel (x,y) is at flat index y*width + x. */
  k_ra_book_image_svg   = 1U, /**< Verbatim UTF-8 SVG source (vector; on-device rasterized). */
} ra_book_image_format_t;

/**
 * @enum ra_book_struct_size_t
 * @brief Pinned on-disk sizes of the blob's fixed-layout records.
 * @details The format is a binary wire layout shared with the host compiler, so
 *          each record's byte size is part of the contract. These named
 *          constants drive the `static_assert`s that guard against accidental
 *          padding or a silent field change.
 * @since Version 1.0.0
 */
typedef enum : uint16_t {
  k_ra_book_sizeof_header     = 100U, /**< Bytes in @ref ra_book_header_t.     */
  k_ra_book_sizeof_chapter    = 12U,  /**< Bytes in @ref ra_book_chapter_t.    */
  k_ra_book_sizeof_node       = 24U,  /**< Bytes in @ref ra_book_node_t.       */
  k_ra_book_sizeof_attr       = 8U,   /**< Bytes in @ref ra_book_attr_t.       */
  k_ra_book_sizeof_stylesheet = 8U,   /**< Bytes in @ref ra_book_stylesheet_t. */
  k_ra_book_sizeof_image      = 24U,  /**< Bytes in @ref ra_book_image_t.      */
} ra_book_struct_size_t;

/**
 * @struct ra_book_header_t
 * @brief Fixed 100-byte prologue describing every table and pool in the blob.
 * @details All `_off` fields are byte offsets from the blob base; all `_count`
 *          / `_size` fields are element counts / byte lengths. Metadata string
 *          offsets index the string pool.
 * @invariant `total_size` equals the exact byte length of the blob.
 * @invariant Every `_off` plus its table extent lies within `[0, total_size]`.
 * @since Version 1.0.0
 */
typedef struct {
  char     magic[8];          /**< Always "RABOOK1" (7 chars + NUL).            */
  uint32_t format_version;    /**< @ref ra_book_version_t of this blob.         */
  uint32_t total_size;        /**< Total blob length in bytes.                  */
  uint32_t flags;             /**< Reserved feature flags; 0 in v1.             */
  uint32_t title_off;         /**< String-pool offset of the book title.        */
  uint32_t author_off;        /**< String-pool offset of the author.            */
  uint32_t language_off;      /**< String-pool offset of the BCP-47 language.   */
  uint32_t identifier_off;    /**< String-pool offset of the unique book id.    */
  uint32_t cover_image_index; /**< Image-table index of the cover, or nil.      */
  uint32_t chapter_count;     /**< Number of spine chapters.                    */
  uint32_t chapter_off;       /**< Offset to the chapter table.                 */
  uint32_t node_count;        /**< Number of DOM nodes.                         */
  uint32_t node_off;          /**< Offset to the node table.                    */
  uint32_t attr_count;        /**< Number of attribute records.                 */
  uint32_t attr_off;          /**< Offset to the attribute table.               */
  uint32_t stylesheet_count;  /**< Number of preserved stylesheets.             */
  uint32_t stylesheet_off;    /**< Offset to the stylesheet table.              */
  uint32_t image_count;       /**< Number of image descriptors.                 */
  uint32_t image_off;         /**< Offset to the image table.                   */
  uint32_t string_off;        /**< Offset to the string pool.                   */
  uint32_t string_size;       /**< String-pool length in bytes.                 */
  uint32_t image_pool_off;    /**< Offset to the image pool.                    */
  uint32_t image_pool_size;   /**< Image-pool length in bytes.                  */
  uint32_t crc32;             /**< CRC-32/ISO-HDLC of the body (all bytes after this header). */
} ra_book_header_t;

static_assert(sizeof(ra_book_header_t) == k_ra_book_sizeof_header, "ra_book_header_t size pinned");

/**
 * @struct ra_book_chapter_t
 * @brief One spine document (a renderable chapter) plus its TOC label.
 * @since Version 1.0.0
 */
typedef struct {
  uint32_t title_off; /**< String-pool offset of the TOC label ("" if none).   */
  uint32_t href_off;  /**< String-pool offset of the source spine href.        */
  uint32_t root_node; /**< Node-table index of this chapter's root element.    */
} ra_book_chapter_t;

static_assert(sizeof(ra_book_chapter_t) == k_ra_book_sizeof_chapter,
              "ra_book_chapter_t size pinned");

/**
 * @struct ra_book_node_t
 * @brief One DOM node. Children form a singly-linked sibling chain.
 * @details Walk an element's children by reading `first_child` then following
 *          each node's `next_sibling` until @ref k_ra_book_nil. Element nodes
 *          set `name_off` (tag name) and may have attributes; text nodes set
 *          `text_off` and have neither attributes nor children.
 * @invariant `kind == k_ra_book_node_text` implies `attr_count == 0`,
 *            `first_attr == nil` and `first_child == nil`.
 * @since Version 1.0.0
 */
typedef struct {
  uint8_t  kind;         /**< @ref ra_book_node_kind_t.                          */
  uint8_t  reserved;     /**< Padding; 0.                                        */
  uint16_t attr_count;   /**< Element: number of attributes (text: 0).           */
  uint32_t name_off;     /**< Element: string-pool offset of tag name (text: 0). */
  uint32_t text_off;     /**< Text: string-pool offset of the run (element: 0).  */
  uint32_t first_attr;   /**< Index of first attribute, or nil.                  */
  uint32_t first_child;  /**< Index of first child node, or nil.                 */
  uint32_t next_sibling; /**< Index of next sibling node, or nil.                */
} ra_book_node_t;

static_assert(sizeof(ra_book_node_t) == k_ra_book_sizeof_node, "ra_book_node_t size pinned");

/**
 * @struct ra_book_attr_t
 * @brief One `name="value"` attribute on an element.
 * @since Version 1.0.0
 */
typedef struct {
  uint32_t name_off;  /**< String-pool offset of the attribute name.  */
  uint32_t value_off; /**< String-pool offset of the attribute value. */
} ra_book_attr_t;

static_assert(sizeof(ra_book_attr_t) == k_ra_book_sizeof_attr, "ra_book_attr_t size pinned");

/**
 * @struct ra_book_stylesheet_t
 * @brief A preserved CSS stylesheet and the chapter it scopes to.
 * @details `source_off` points at the verbatim stylesheet text so the CSS
 *          cascade engine receives the real rules. `scope_chapter` is a chapter
 *          index, or @ref k_ra_book_nil for a book-wide stylesheet.
 * @since Version 1.0.0
 */
typedef struct {
  uint32_t source_off;    /**< String-pool offset of the verbatim CSS text.     */
  uint32_t scope_chapter; /**< Chapter index this applies to, or nil (= all).   */
} ra_book_stylesheet_t;

static_assert(sizeof(ra_book_stylesheet_t) == k_ra_book_sizeof_stylesheet,
              "ra_book_stylesheet_t size pinned");

/**
 * @struct ra_book_image_t
 * @brief Descriptor for one transcoded image in the image pool.
 * @details `id_off` is the original manifest href so an `<img src>` attribute
 *          value resolves to this entry. Raster images are downscaled to panel
 *          class and packed as 4bpp grayscale (panel-ready, no decode); SVG is
 *          kept as verbatim vector source. Pool bytes are raw -- the whole blob
 *          is DEFLATE-wrapped on disk and inflated once on open, so per-image
 *          compression would not help -- thus `data_size == raw_size` and
 *          `data_off` indexes the image pool.
 * @since Version 1.0.0
 */
typedef struct {
  uint32_t id_off;    /**< String-pool offset of the source href / manifest id. */
  uint16_t width;     /**< Pixel width.                                         */
  uint16_t height;    /**< Pixel height.                                        */
  uint8_t  format;    /**< @ref ra_book_image_format_t.                         */
  uint8_t  reserved;  /**< Padding; 0.                                          */
  uint16_t reserved2; /**< Padding; 0.                                          */
  uint32_t data_off;  /**< Image-pool offset of the compressed pixel data.      */
  uint32_t data_size; /**< Compressed length in bytes.                          */
  uint32_t raw_size;  /**< Inflated length in bytes.                            */
} ra_book_image_t;

static_assert(sizeof(ra_book_image_t) == k_ra_book_sizeof_image, "ra_book_image_t size pinned");

/* -------------------------------------------------------------------------- */
/* Inline accessors. All take a validated blob base; see ra_book_validate().  */
/* -------------------------------------------------------------------------- */

/**
 * @brief View the blob base as its header.
 * @param[in] base Pointer to the first byte of a `.rabook` blob (non-NULL).
 * @return The header view.
 * @pre `base` points at a blob already accepted by ra_book_validate().
 * @pre `base` is at least `alignof(uint32_t)`-aligned.
 * @post Returned pointer aliases `base`; never NULL when `base` is non-NULL.
 * @note Thread-safe: read-only over immutable data.
 * @since Version 1.0.0
 */
static inline const ra_book_header_t* ra_book_header(const void* base)
{
  return (const ra_book_header_t*)base;
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
 * @since Version 1.0.0
 */
static inline const void* ra_book_at(const void* base, uint32_t off)
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
 * @since Version 1.0.0
 */
static inline const char* ra_book_string(const void* base, uint32_t off)
{
  return (const char*)ra_book_at(base, ra_book_header(base)->string_off + off);
}

/**
 * @brief Base of the chapter table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to chapter[0]; read `ra_book_header(base)->chapter_count` entries.
 * @pre `base` validated.
 * @pre `ra_book_header(base)->chapter_count > 0` for the result to be dereferenceable.
 * @post Result indexes a contiguous array of `chapter_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 1.0.0
 */
static inline const ra_book_chapter_t* ra_book_chapters(const void* base)
{
  return (const ra_book_chapter_t*)ra_book_at(base, ra_book_header(base)->chapter_off);
}

/**
 * @brief Base of the DOM node table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to node[0]; indices `first_child`/`next_sibling`/`root_node` index here.
 * @pre `base` validated.
 * @pre `ra_book_header(base)->node_count > 0`.
 * @post Result indexes a contiguous array of `node_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 1.0.0
 */
static inline const ra_book_node_t* ra_book_nodes(const void* base)
{
  return (const ra_book_node_t*)ra_book_at(base, ra_book_header(base)->node_off);
}

/**
 * @brief Base of the attribute table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to attr[0]; a node's `first_attr` indexes here.
 * @pre `base` validated.
 * @pre `ra_book_header(base)->attr_count > 0`.
 * @post Result indexes a contiguous array of `attr_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 1.0.0
 */
static inline const ra_book_attr_t* ra_book_attrs(const void* base)
{
  return (const ra_book_attr_t*)ra_book_at(base, ra_book_header(base)->attr_off);
}

/**
 * @brief Base of the stylesheet table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to stylesheet[0].
 * @pre `base` validated.
 * @pre `ra_book_header(base)->stylesheet_count > 0`.
 * @post Result indexes a contiguous array of `stylesheet_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 1.0.0
 */
static inline const ra_book_stylesheet_t* ra_book_stylesheets(const void* base)
{
  return (const ra_book_stylesheet_t*)ra_book_at(base, ra_book_header(base)->stylesheet_off);
}

/**
 * @brief Base of the image table.
 * @param[in] base Blob base (non-NULL, validated).
 * @return Pointer to image[0]; `cover_image_index` indexes here.
 * @pre `base` validated.
 * @pre `ra_book_header(base)->image_count > 0`.
 * @post Result indexes a contiguous array of `image_count` entries.
 * @note Thread-safe: read-only.
 * @since Version 1.0.0
 */
static inline const ra_book_image_t* ra_book_images(const void* base)
{
  return (const ra_book_image_t*)ra_book_at(base, ra_book_header(base)->image_off);
}

/**
 * @brief Tag name of an element node.
 * @param[in] base Blob base (non-NULL, validated).
 * @param[in] node Element node (non-NULL, `kind == k_ra_book_node_element`).
 * @return The interned, NUL-terminated tag name.
 * @pre `node` belongs to `base` and is an element.
 * @pre `base` validated.
 * @post Result is a string inside the blob.
 * @note Thread-safe: read-only.
 * @since Version 1.0.0
 */
static inline const char* ra_book_node_name(const void* base, const ra_book_node_t* node)
{
  return ra_book_string(base, node->name_off);
}

/**
 * @brief Text of a text node.
 * @param[in] base Blob base (non-NULL, validated).
 * @param[in] node Text node (non-NULL, `kind == k_ra_book_node_text`).
 * @return The NUL-terminated UTF-8 run.
 * @pre `node` belongs to `base` and is a text node.
 * @pre `base` validated.
 * @post Result is a string inside the blob.
 * @note Thread-safe: read-only.
 * @since Version 1.0.0
 */
static inline const char* ra_book_node_text(const void* base, const ra_book_node_t* node)
{
  return ra_book_string(base, node->text_off);
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
 * @since Version 1.0.0
 */
static inline const uint8_t* ra_book_image_data(const void* base, const ra_book_image_t* img)
{
  return (const uint8_t*)ra_book_at(base, ra_book_header(base)->image_pool_off) + img->data_off;
}

/**
 * @brief Validate that a byte buffer is a well-formed, intact `.rabook` blob.
 *
 * @details
 * Checks, in order: the magic and `format_version`; that `total_size` fits in
 * `size`; that every table offset plus its extent and every pool lie within
 * `total_size`; and finally the CRC-32 of the body. Must be called once before
 * any accessor is used on `base`; the accessors assume a validated blob and do
 * no bounds checking themselves (they are pure offset arithmetic for XIP).
 *
 * @param[in] base Pointer to the candidate blob (may be NULL).
 * @param[in] size Number of readable bytes at `base`.
 *
 * @return Error code.
 * @retval k_ra_ok             Blob is well-formed and CRC matches.
 * @retval k_ra_err_null_ptr   `base` is NULL.
 * @retval k_ra_err_invalid_arg  Magic is wrong or the format version is unknown.
 * @retval k_ra_err_invalid_size `size` is too small or a table/pool runs past `total_size`.
 * @retval k_ra_err_range_check_failed CRC-32 of the body does not match the header.
 *
 * @pre `size` is the true readable length at `base` (no over-read).
 * @pre `base` is `alignof(uint32_t)`-aligned when non-NULL.
 * @post On `k_ra_ok`, every accessor on `base` stays within `[base, base + total_size)`.
 * @post On any error, `base` must not be passed to other accessors.
 *
 * @note Thread-safe: reads only the immutable candidate buffer.
 * @see ra_book_header()
 * @since Version 1.0.0
 */
ra_err_t ra_book_validate(const void* base, size_t size);

/**
 * @typedef ra_book_inflate_fn
 * @brief Caller-supplied DEFLATE inflater used by @ref ra_book_open().
 * @details Keeps `ra_book` decoupled from any one decompressor: the firmware
 *          passes a miniz-backed inflater, host tests pass a zlib one.
 * @param[in]  src     Start of the raw DEFLATE stream.
 * @param[in]  src_len DEFLATE stream length in bytes.
 * @param[out] dst     Destination buffer to inflate into.
 * @param[in]  dst_cap Capacity of `dst` in bytes.
 * @param[out] out_len Receives the number of bytes written to `dst`.
 * @return `k_ra_ok` on success, any non-zero @ref ra_err_t on failure.
 * @since Version 1.0.0
 */
typedef ra_err_t (
  *ra_book_inflate_fn)(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len);

/**
 * @brief Open a `.rabook` file: check the container, inflate, validate the blob.
 *
 * @details
 * Reads the "RBKZ" container header, inflates the DEFLATE payload into the
 * caller-owned `scratch` buffer (expected to live in SDRAM), then runs
 * ra_book_validate() over the inflated flat blob. On success `*out_base` is the
 * validated blob base (equal to `scratch`) ready for the inline accessors.
 *
 * @param[in]  file        Pointer to the `.rabook` file bytes (non-NULL).
 * @param[in]  file_len    Length of `file` in bytes.
 * @param[in]  inflate     Decompressor callback (see @ref ra_book_inflate_fn).
 * @param[out] scratch     Buffer that receives the inflated blob (non-NULL).
 * @param[in]  scratch_cap Capacity of `scratch`; must be >= the inflated size.
 * @param[out] out_base    Receives the validated blob base on success.
 * @param[out] out_size    Receives the inflated blob length on success.
 *
 * @return Error code.
 * @retval k_ra_ok               Container valid, inflated, and blob validated.
 * @retval k_ra_err_null_ptr     A required pointer argument is NULL.
 * @retval k_ra_err_invalid_arg  Container magic is wrong.
 * @retval k_ra_err_invalid_size File too short, `scratch_cap` too small, or the
 *                               inflated length disagrees with the header.
 * @retval k_ra_err_range_check_failed Blob CRC mismatch (from ra_book_validate()).
 *
 * @pre `file_len` is the true readable length at `file`.
 * @pre `scratch` is `alignof(uint32_t)`-aligned.
 * @post On `k_ra_ok`, `*out_base == scratch` and accessors stay within it.
 * @post On any error, `scratch` contents are unspecified and must not be walked.
 *
 * @note Thread-safe if `inflate` is and `scratch` is not shared concurrently.
 * @see ra_book_validate()
 * @since Version 1.0.0
 */
ra_err_t ra_book_open(const void*        file,
                      size_t             file_len,
                      ra_book_inflate_fn inflate,
                      void*              scratch,
                      size_t             scratch_cap,
                      const void**       out_base,
                      size_t*            out_size);

/**
 * @brief Serialize one chapter's DOM subtree back to XHTML for the renderer.
 *
 * @details
 * A bridge for feeding a compiled book into `ra_reflow_layout_chapter()`, which
 * consumes XHTML. Walks the chapter's element/text tree iteratively (no
 * recursion, bounded stack) and writes well-formed XHTML -- every tag,
 * attribute and text run, faithfully -- into @p out. Void elements self-close;
 * text and attribute values are entity-escaped. The output is NOT
 * NUL-terminated.
 *
 * @param[in]  base        Validated `ra_book` blob base (non-NULL).
 * @param[in]  chapter_idx Spine chapter index (`< header chapter_count`).
 * @param[out] out         Destination XHTML buffer (non-NULL).
 * @param[in]  cap         Capacity of @p out in bytes.
 * @param[out] out_len     Receives the XHTML byte length written.
 *
 * @return Error code.
 * @retval k_ra_ok               Chapter serialized; @p out_len bytes written.
 * @retval k_ra_err_null_ptr     A required pointer argument is NULL.
 * @retval k_ra_err_invalid_arg  @p chapter_idx is out of range.
 * @retval k_ra_err_invalid_size Output did not fit @p cap, or DOM nesting
 *                               exceeded the bounded walk stack.
 *
 * @pre @p base was accepted by ra_book_validate() / ra_book_open().
 * @pre @p cap is large enough for the chapter's serialized XHTML.
 * @post On k_ra_ok, `out[0..*out_len)` is well-formed (not NUL-terminated).
 * @post On error, @p out contents are unspecified.
 *
 * @note Thread-safe: reads only the immutable blob, writes only @p out.
 * @see ra_reflow_layout_chapter()
 * @since Version 1.0.0
 */
ra_err_t ra_book_chapter_to_xhtml(const void* base, uint32_t chapter_idx, char* out, size_t cap,
                                  size_t* out_len);
