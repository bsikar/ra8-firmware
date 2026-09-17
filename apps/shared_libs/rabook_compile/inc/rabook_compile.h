/**
 * @file rabook_compile.h
 * @brief Zero-heap builder that emits a RABOOK1 blob (the #149 compiler back-end).
 * @ingroup grp_ereader
 *
 * @details
 * `rabook_compile` is the serialization back-end of the on-device EPUB ->
 * `.rabook` compiler (issue #149). It takes an in-memory book model -- a DOM of
 * element/text nodes with attributes, spine chapters, a string pool, transcoded
 * images and preserved stylesheets, plus metadata -- and lays it out as the exact
 * binary RABOOK1 blob that the desktop tool `tools/epub_compile/src/epub_compile.py`
 * emits and the on-device reader @ref book parses. This file is ONLY the
 * emitter; the XHTML -> DOM front-end and the raster -> gray4 image transcode are
 * the next stage-(a) pieces that drive this builder.
 *
 * @par Zero allocation
 * NASA Power-of-10 Rule 3: there is no `malloc` anywhere. The caller hands the
 * builder a set of fixed, caller-sized arenas (one per table plus the string and
 * image pools plus the output buffer) via @ref ra8_rabook_buffers_t. Every
 * builder call appends into those arenas; an overflow latches a sticky failure
 * that @ref ra8_rabook_finalize reports, so the caller checks once at the end.
 *
 * @par Byte layout (matches book.h and the desktop tool)
 * @code
 *   [ book_header_t                       ] fixed 100-byte header
 *   [ chapter table                          ] header.chapter_count entries
 *   [ node table                             ] header.node_count entries
 *   [ attr table                             ] header.attr_count entries
 *   [ stylesheet table                       ] header.stylesheet_count entries
 *   [ image table                            ] header.image_count entries
 *   [ string pool                            ] deduped NUL-terminated UTF-8
 *   [ image pool                             ] raw 4bpp grayscale / SVG bytes
 * @endcode
 * The body CRC-32 (reflected 0xEDB88320, init/final 0xFFFFFFFF -- matches Python
 * `zlib.crc32`) covers every byte after the 100-byte header, exactly as
 * @ref book_validate expects.
 *
 * @note Not thread-safe: a builder context is single-owner, mutated in place.
 * @see book.h  The format definition + the reader this emitter targets.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "book.h"
#include "ra8_err.h"

/**
 * @struct ra8_rabook_buffers_t
 * @brief Caller-owned, fixed-capacity arenas the builder appends into (no heap).
 * @details The caller sizes each arena for its worst-case book. The builder
 *          never allocates; it only fills these. `*_cap` are element counts for
 *          the typed tables and byte capacities for the pools and the output.
 * @invariant Every pointer is non-NULL and each arena holds at least its `_cap`.
 * @since Version 0.1.0
 */
typedef struct {
  /* Pointers grouped before the uint32 caps so the 64-bit host unit-test
   * build has zero inter-field padding (clang-analyzer optin.performance). */
  book_chapter_t*    chapters;       /**< Chapter-table arena.           */
  book_node_t*       nodes;          /**< Node-table arena.              */
  book_attr_t*       attrs;          /**< Attribute-table arena.         */
  book_stylesheet_t* stylesheets;    /**< Stylesheet-table arena.        */
  book_image_t*      images;         /**< Image-table arena.             */
  char*              string_pool;    /**< String-pool byte arena.        */
  uint8_t*           image_pool;     /**< Image-pool byte arena.         */
  uint8_t*           out;            /**< Final-blob output buffer.      */
  uint32_t           chapter_cap;    /**< Max chapters.                  */
  uint32_t           node_cap;       /**< Max DOM nodes.                 */
  uint32_t           attr_cap;       /**< Max attribute records.         */
  uint32_t           stylesheet_cap; /**< Max stylesheets.               */
  uint32_t           image_cap;      /**< Max image descriptors.         */
  uint32_t           string_cap;     /**< String-pool capacity in bytes. */
  uint32_t           image_pool_cap; /**< Image-pool capacity in bytes.  */
  uint32_t           out_cap;        /**< Output capacity in bytes.      */
} ra8_rabook_buffers_t;

/**
 * @struct ra8_rabook_ctx_t
 * @brief Builder state: the arenas plus running counts and a sticky-fail flag.
 * @details Initialise with @ref rabook_compile_init; treat the fields as
 *          private. `failed` latches on the first arena overflow so the caller
 *          can build optimistically and check once at @ref ra8_rabook_finalize.
 * @invariant Each table count and the string size never exceeds its matching cap.
 * @invariant An internal image pool never exceeds `image_pool_cap`; an external
 *            pool is instead bounded by the uint32 wire-format size.
 * @since Version 0.1.0
 */
typedef struct {
  ra8_rabook_buffers_t buf;               /**< Caller-provided arenas.             */
  uint32_t             chapter_count;     /**< Chapters appended so far.           */
  uint32_t             node_count;        /**< Nodes appended so far.              */
  uint32_t             attr_count;        /**< Attributes appended so far.         */
  uint32_t             stylesheet_count;  /**< Stylesheets appended so far.        */
  uint32_t             image_count;       /**< Images appended so far.             */
  uint32_t             string_size;       /**< String-pool bytes used.             */
  uint32_t             image_pool_size;   /**< Image-pool bytes used.              */
  uint32_t             title_off;         /**< Metadata: title string offset.      */
  uint32_t             author_off;        /**< Metadata: author string offset.     */
  uint32_t             language_off;      /**< Metadata: language string offset.   */
  uint32_t             identifier_off;    /**< Metadata: identifier string offset. */
  uint32_t             cover_image_index; /**< Cover image index, or nil.          */
  uint32_t             flags;             /**< Reserved header flags (0 in v1).    */
  uint8_t              image_pool_mode;   /**< Internal builder storage mode.      */
  bool                 failed;            /**< Sticky: an arena overflowed.        */
} ra8_rabook_ctx_t;

/**
 * @brief Read bytes from an externally stored image pool.
 * @param[in]  ctx       Caller context supplied to @ref ra8_rabook_finalize_stream.
 * @param[in]  offset    Byte offset in the logical image pool.
 * @param[out] dst       Destination for @p requested bytes.
 * @param[in]  requested Exact number of bytes requested.
 * @param[out] out_read  Receives the number of bytes actually copied.
 * @return Read status; callback-specific failures are propagated unchanged.
 * @pre Pointers are non-NULL and @p dst spans @p requested writable bytes.
 * @post On success `*out_read <= requested`; the finalizer rejects a short read.
 * @note Not thread-safe unless the callback context is independently synchronized.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_rabook_image_read_fn)(void*     ctx,
                                              uint32_t  offset,
                                              uint8_t*  dst,
                                              uint32_t  requested,
                                              uint32_t* out_read);

/**
 * @brief Append bytes to a streaming RABOOK1 destination.
 * @param[in,out] ctx         Caller context supplied to @ref ra8_rabook_finalize_stream.
 * @param[in]     src         Source bytes to append.
 * @param[in]     requested   Exact number of bytes requested.
 * @param[out]    out_written Receives the number of bytes actually appended.
 * @return Write status; callback-specific failures are propagated unchanged.
 * @pre Pointers are non-NULL and @p src spans @p requested readable bytes.
 * @post On success `*out_written <= requested`; the finalizer rejects a short write.
 * @note Not thread-safe unless the callback context is independently synchronized.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_rabook_write_fn)(void*          ctx,
                                         const uint8_t* src,
                                         uint32_t       requested,
                                         uint32_t*      out_written);

/**
 * @brief Bind a builder context to its caller-provided arenas.
 * @details Zero-inits the running counts and clears the sticky-fail flag. All
 *          metadata offsets default to 0 and the cover index to @ref k_book_nil
 *          until @ref ra8_rabook_set_metadata overrides them. String-pool offset 0
 *          is reserved for the empty string -- init interns `""` first so offset 0
 *          is the empty-string sentinel that text/element nodes store, matching
 *          the desktop `StringPool.__init__` in `tools/epub_compile/src/epub_compile.py`.
 * @param[out] ctx Builder context to initialise (non-NULL).
 * @param[in]  buf Caller-owned arenas (non-NULL; all member pointers non-NULL).
 * @return Error code.
 * @retval k_ra8_ok           Context bound and ready.
 * @retval k_ra8_err_null_ptr @p ctx or @p buf (or a member pointer) is NULL.
 * @pre @p buf's arenas each hold at least their declared `_cap`.
 * @pre @p ctx is not aliased by another live builder.
 * @post On success every running count is 0 and `failed` is false.
 * @post On success the metadata offsets are 0 and the cover index is nil.
 * @post On success string-pool offset 0 holds the empty string (`string_size == 1`).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra8_err_t rabook_compile_init(ra8_rabook_ctx_t* ctx, const ra8_rabook_buffers_t* buf);

/**
 * @brief Intern a NUL-terminated UTF-8 string into the pool, de-duplicated.
 * @details Scans the existing pool string-by-string for an exact match; on a hit
 *          returns that offset, otherwise appends `str` plus its NUL and returns
 *          the new offset. Identical strings therefore share one offset (the
 *          first-interned occurrence), matching the desktop StringPool so a
 *          renderer can intern-compare tags by offset.
 * @param[in,out] ctx Builder context (non-NULL, initialised).
 * @param[in]     str NUL-terminated string to intern (non-NULL).
 * @return The string-pool byte offset, or @ref k_book_nil on overflow / error.
 * @retval k_book_nil @p ctx or @p str is NULL, or the pool is full.
 * @pre @p ctx was initialised by @ref rabook_compile_init.
 * @pre @p str is NUL-terminated; its length is measured with `strlen`. (Pool
 *      overflow is handled gracefully -- see the @retval / @post below -- so it
 *      is not a caller precondition.)
 * @post On success the returned offset addresses a NUL-terminated copy of @p str.
 * @post On overflow the sticky `failed` flag is set and the pool is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint32_t ra8_rabook_intern(ra8_rabook_ctx_t* ctx, const char* str);

/**
 * @brief Append an element node with its attributes; return its node index.
 * @details Writes a @ref k_book_node_element node carrying @p name_off and
 *          appends @p attr_count attribute records contiguously, linking the
 *          node's `first_attr` / `attr_count`. Child / sibling links default to
 *          nil; set them with @ref ra8_rabook_link_child / @ref ra8_rabook_link_sibling.
 * @param[in,out] ctx        Builder context (non-NULL, initialised).
 * @param[in]     name_off   String-pool offset of the tag name.
 * @param[in]     attrs      Attribute records (may be NULL iff @p attr_count is 0).
 * @param[in]     attr_count Number of attributes for this element.
 * @return The new node index, or @ref k_book_nil on overflow / error.
 * @retval k_book_nil @p ctx is NULL, the node/attr arena is full, or @p attrs
 *                       is NULL with a non-zero @p attr_count.
 * @pre @p ctx was initialised.
 * @pre Adding one node and @p attr_count attrs fits the node / attr arenas.
 * @post On success node `name_off`, `attr_count` and `first_attr` are set.
 * @post On overflow `failed` is set and no node / attr is appended.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint32_t ra8_rabook_add_element(ra8_rabook_ctx_t*  ctx,
                                uint32_t           name_off,
                                const book_attr_t* attrs,
                                uint16_t           attr_count);

/**
 * @brief Append a text node carrying @p text_off; return its node index.
 * @details Writes a @ref k_book_node_text node whose `text_off` is @p text_off;
 *          a text node has no attributes and no children. Sibling links default
 *          to nil; set them with @ref ra8_rabook_link_sibling.
 * @param[in,out] ctx      Builder context (non-NULL, initialised).
 * @param[in]     text_off String-pool offset of the text run.
 * @return The new node index, or @ref k_book_nil on overflow / error.
 * @retval k_book_nil @p ctx is NULL or the node arena is full.
 * @pre @p ctx was initialised.
 * @pre One more node fits the node arena.
 * @post On success the node is a text node with `text_off` set.
 * @post On overflow `failed` is set and no node is appended.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint32_t ra8_rabook_add_text(ra8_rabook_ctx_t* ctx, uint32_t text_off);

/**
 * @brief Set @p parent's first child to @p child.
 * @details Records the head of @p parent's child sibling-chain. Call once per
 *          parent with the first child; chain the rest via @ref ra8_rabook_link_sibling.
 * @param[in,out] ctx    Builder context (non-NULL, initialised).
 * @param[in]     parent Node index of the parent element (valid, an element).
 * @param[in]     child  Node index of the first child (valid).
 * @return Error code.
 * @retval k_ra8_ok              Link recorded.
 * @retval k_ra8_err_null_ptr    @p ctx is NULL.
 * @retval k_ra8_err_invalid_arg @p parent or @p child is out of range.
 * @pre @p ctx was initialised.
 * @pre @p parent and @p child are indices of nodes already appended.
 * @post On success `nodes[parent].first_child == child`.
 * @post On error the node table is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra8_err_t ra8_rabook_link_child(ra8_rabook_ctx_t* ctx, uint32_t parent, uint32_t child);

/**
 * @brief Set @p previous_node's next sibling to @p next_sibling.
 * @details Extends a child sibling-chain by one. Walk a parent's children by
 *          reading `first_child` then each node's `next_sibling`.
 * @param[in,out] ctx           Builder context (non-NULL, initialised).
 * @param[in]     previous_node Node index whose sibling is set (valid).
 * @param[in]     next_sibling  Node index of the next sibling (valid).
 * @return Error code.
 * @retval k_ra8_ok              Link recorded.
 * @retval k_ra8_err_null_ptr    @p ctx is NULL.
 * @retval k_ra8_err_invalid_arg @p previous_node or @p next_sibling is out of range.
 * @pre @p ctx was initialised.
 * @pre @p previous_node and @p next_sibling are indices of nodes already appended.
 * @post On success `nodes[previous_node].next_sibling == next_sibling`.
 * @post On error the node table is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra8_err_t
ra8_rabook_link_sibling(ra8_rabook_ctx_t* ctx, uint32_t previous_node, uint32_t next_sibling);

/**
 * @brief Append a spine chapter; return its chapter index.
 * @details Records one chapter-table entry pointing at its root DOM node plus the
 *          interned TOC label and spine href. Chapters are appended in spine
 *          (reading) order, so the returned index doubles as the chapter's
 *          position in the book.
 * @param[in,out] ctx       Builder context (non-NULL, initialised).
 * @param[in]     title_off String-pool offset of the TOC label ("" if none).
 * @param[in]     href_off  String-pool offset of the spine href.
 * @param[in]     root_node Node index of this chapter's root element.
 * @return The new chapter index, or @ref k_book_nil on overflow / error.
 * @retval k_book_nil @p ctx is NULL or the chapter arena is full.
 * @pre @p ctx was initialised.
 * @pre One more chapter fits the chapter arena.
 * @post On success the chapter table gains one entry.
 * @post On overflow `failed` is set and no chapter is appended.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint32_t ra8_rabook_add_chapter(ra8_rabook_ctx_t* ctx,
                                uint32_t          title_off,
                                uint32_t          href_off,
                                uint32_t          root_node);

/**
 * @brief Append an image descriptor and copy its pixel/SVG bytes into the pool.
 * @details Copies @p data_size bytes into the image pool and records a descriptor
 *          with `data_off` set to the bytes' pool offset and `raw_size == data_size`
 *          (the pool is uncompressed; the whole blob is DEFLATE-wrapped on disk).
 * @param[in,out] ctx       Builder context (non-NULL, initialised).
 * @param[in]     id_off    String-pool offset of the source href / manifest id.
 * @param[in]     width     Pixel width (0 for SVG).
 * @param[in]     height    Pixel height (0 for SVG).
 * @param[in]     format    @ref book_image_format_t (gray4 or svg).
 * @param[in]     pixel_format @ref book_image_pixfmt_t depth of a gray4-format
 *                          raster (gray4 = 4bpp packed, gray8 = 8bpp); pass
 *                          @ref k_book_pixfmt_gray4 (0) for an SVG entry.
 * @param[in]     data      Pixel / SVG bytes to copy (non-NULL iff @p data_size > 0).
 * @param[in]     data_size Byte length of @p data.
 * @return The new image index, or @ref k_book_nil on overflow / error.
 * @retval k_book_nil @p ctx is NULL, the image table or pool is full, or
 *                       @p data is NULL with a non-zero @p data_size.
 * @pre @p ctx was initialised.
 * @pre @p data_size bytes fit the remaining image-pool capacity.
 * @post On success the descriptor's `data_off` addresses the copied bytes and its
 *       `pixel_format` records @p pixel_format.
 * @post On overflow `failed` is set and no descriptor / bytes are appended.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint32_t ra8_rabook_add_image(ra8_rabook_ctx_t* ctx,
                              uint32_t          id_off,
                              uint16_t          width,
                              uint16_t          height,
                              uint8_t           format,
                              uint8_t           pixel_format,
                              const uint8_t*    data,
                              uint32_t          data_size);

/**
 * @brief Append an image descriptor whose bytes live in a caller-owned spool.
 * @details Reserves the next contiguous span of the logical image pool without
 *          copying bytes into @ref ra8_rabook_buffers_t::image_pool. The caller
 *          stores exactly @p data_size bytes at the returned @p out_data_off and
 *          later supplies a matching read callback to
 *          @ref ra8_rabook_finalize_stream. Internal and external non-empty
 *          images cannot be mixed in one context.
 * @param[in,out] ctx          Builder context.
 * @param[in]     id_off       Interned image identifier offset.
 * @param[in]     width        Image width in pixels.
 * @param[in]     height       Image height in pixels.
 * @param[in]     format       @ref book_image_format_t value.
 * @param[in]     pixel_format @ref book_image_pixfmt_t value.
 * @param[in]     data_size    Reserved external byte count.
 * @param[out]    out_data_off Receives the logical image-pool offset.
 * @return The new image index, or @ref k_book_nil on error.
 * @retval k_book_nil NULL arguments, image-table exhaustion, mixed storage
 *                       modes, or a logical image-pool size overflow.
 * @pre @p ctx was initialized and @p out_data_off is writable.
 * @pre @p data_size bytes are available in the caller's spool before finalize.
 * @post On success the descriptor references `[out_data_off, out_data_off + data_size)`.
 * @post On failure the sticky builder failure is latched and no descriptor is appended.
 * @note The builder does not own or write the external bytes.
 * @since 0.1.0
 */
uint32_t ra8_rabook_add_image_external(ra8_rabook_ctx_t* ctx,
                                       uint32_t          id_off,
                                       uint16_t          width,
                                       uint16_t          height,
                                       uint8_t           format,
                                       uint8_t           pixel_format,
                                       uint32_t          data_size,
                                       uint32_t*         out_data_off);

/**
 * @brief Append a preserved stylesheet; return its stylesheet index.
 * @details Records one verbatim-CSS stylesheet entry in the stylesheet table.
 *          @p scope_chapter limits it to a single chapter, or @ref k_book_nil
 *          applies it book-wide; the CSS text itself is interned in the string
 *          pool and referenced by @p source_off.
 * @param[in,out] ctx           Builder context (non-NULL, initialised).
 * @param[in]     source_off    String-pool offset of the verbatim CSS text.
 * @param[in]     scope_chapter Chapter index it scopes to, or @ref k_book_nil.
 * @return The new stylesheet index, or @ref k_book_nil on overflow / error.
 * @retval k_book_nil @p ctx is NULL or the stylesheet arena is full.
 * @pre @p ctx was initialised.
 * @pre One more stylesheet fits the stylesheet arena.
 * @post On success the stylesheet table gains one entry.
 * @post On overflow `failed` is set and no stylesheet is appended.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
uint32_t
ra8_rabook_add_stylesheet(ra8_rabook_ctx_t* ctx, uint32_t source_off, uint32_t scope_chapter);

/**
 * @brief Record the book metadata offsets that land in the blob header.
 * @details Stashes the title / author / language / identifier string-pool offsets
 *          plus the cover image index; @ref ra8_rabook_finalize copies them into
 *          the fixed 100-byte RABOOK1 header. The strings must already be interned
 *          in this builder so the offsets stay valid through finalize. If the
 *          builder has already latched its sticky `failed` flag the call is a
 *          no-op that reports @ref k_ra8_err_no_mem (the same way finalize does).
 * @param[in,out] ctx               Builder context (non-NULL, initialised).
 * @param[in]     title_off         String-pool offset of the title.
 * @param[in]     author_off        String-pool offset of the author.
 * @param[in]     language_off      String-pool offset of the BCP-47 language.
 * @param[in]     identifier_off    String-pool offset of the unique id.
 * @param[in]     cover_image_index Image index of the cover, or @ref k_book_nil.
 * @return Error code.
 * @retval k_ra8_ok           Metadata recorded.
 * @retval k_ra8_err_null_ptr @p ctx is NULL.
 * @retval k_ra8_err_no_mem   A prior builder call overflowed an arena (sticky
 *                           `failed` latched); the metadata is not recorded.
 * @pre @p ctx was initialised.
 * @pre The offsets address strings already interned in this builder.
 * @post On success @ref ra8_rabook_finalize writes these into the header.
 * @post On error the recorded metadata is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra8_err_t ra8_rabook_set_metadata(ra8_rabook_ctx_t* ctx,
                                  uint32_t          title_off,
                                  uint32_t          author_off,
                                  uint32_t          language_off,
                                  uint32_t          identifier_off,
                                  uint32_t          cover_image_index);

/**
 * @brief Lay out the tables and pools, fill the header, CRC, and emit the blob.
 *
 * @details
 * Copies the chapter, node, attr, stylesheet and image tables, then the string
 * and image pools, into the output buffer at the contract offsets (see the file
 * header), fills the 100-byte @ref book_header_t (magic, version, all offsets
 * / counts / sizes, metadata, `total_size`), and computes the body CRC-32 over
 * every byte after the header. The result passes @ref book_validate.
 *
 * @param[in,out] ctx      Builder context (non-NULL, initialised, no overflow).
 * @param[out]    out_blob Receives the blob base (= the output arena) (non-NULL).
 * @param[out]    out_len  Receives the blob length in bytes (non-NULL).
 *
 * @return Error code.
 * @retval k_ra8_ok               Blob emitted; @p out_blob / @p out_len set.
 * @retval k_ra8_err_null_ptr     A required pointer argument is NULL.
 * @retval k_ra8_err_no_mem       A prior builder call overflowed an arena.
 * @retval k_ra8_err_invalid_size The laid-out blob exceeds the output capacity.
 *
 * @pre @p ctx was initialised and no builder call set the sticky `failed` flag.
 * @pre The output arena holds the full blob (header + tables + pools).
 * @post On k_ra8_ok, `out_blob[0..*out_len)` is a valid RABOOK1 blob.
 * @post On error @p out_blob / @p out_len are untouched.
 *
 * @note Not thread-safe.
 * @see book_validate()
 * @since Version 0.1.0
 */
ra8_err_t ra8_rabook_finalize(ra8_rabook_ctx_t* ctx, const void** out_blob, uint32_t* out_len);

/**
 * @brief Stream a canonical flat RABOOK1 blob without a full output arena.
 * @details Computes the same canonical layout and CRC as @ref ra8_rabook_finalize,
 *          then appends the header, tables, string pool, and image pool through
 *          @p write. Internal image pools are read directly. An external image
 *          pool is read twice through @p image_read: once for CRC and once for
 *          emission, in chunks no larger than @p scratch_cap.
 * @param[in]     ctx         Completed builder context.
 * @param[in]     image_read  External image-pool reader; required only for an
 *                            external non-empty pool.
 * @param[in,out] image_ctx   Context passed to @p image_read.
 * @param[in]     write       Exact append callback.
 * @param[in,out] write_ctx   Context passed to @p write.
 * @param[in,out] scratch     External-pool transfer scratch; required only for
 *                            an external non-empty pool.
 * @param[in]     scratch_cap Writable bytes at @p scratch.
 * @param[out]    out_len     Receives the complete flat blob length.
 * @return Finalization status.
 * @retval k_ra8_ok Blob streamed completely and @p out_len set.
 * @retval k_ra8_err_null_ptr A required pointer or callback is NULL.
 * @retval k_ra8_err_no_mem A prior builder operation overflowed an arena.
 * @retval k_ra8_err_invalid_size Layout overflow, zero external scratch, or a
 *                                short callback transfer.
 * @return Other callback failures are propagated unchanged.
 * @pre The external reader is repeatable and exposes the logical pool recorded
 *      by @ref ra8_rabook_add_image_external.
 * @pre @p write appends atomically or reports the exact short transfer.
 * @post On success exactly @p *out_len bytes were appended in canonical order.
 * @post On failure @p out_len is untouched; the destination may hold a prefix.
 * @note Uses only caller storage and fixed stack state; performs no allocation.
 * @since 0.1.0
 */
ra8_err_t ra8_rabook_finalize_stream(const ra8_rabook_ctx_t*  ctx,
                                     ra8_rabook_image_read_fn image_read,
                                     void*                    image_ctx,
                                     ra8_rabook_write_fn      write,
                                     void*                    write_ctx,
                                     uint8_t*                 scratch,
                                     uint32_t                 scratch_cap,
                                     uint32_t*                out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif
