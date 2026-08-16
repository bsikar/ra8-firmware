/**
 * @file book_stream_fixture_internal.h
 * @brief Shared strict-RABOOK1 fixture seam for the book-stream test units.
 *
 * @details
 * Declares the fixed flat fixture, the bounded validation and reader
 * workspaces built over it, and the builder seams both strict book-stream test
 * translation units drive. Holding the model here lets the strict-reader
 * vectors and the MC/DC operand vectors each stay one focused translation
 * unit, while every executable that compiles the model keeps its own private,
 * allocation-free copy of the canonical RABOOK1 image.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_book_chunked.h"
#include "ra8_book_stream.h"
#include "ra8_book_stream_internal.h"
#include "ra8_err.h"

/** @brief Fixed fixture and bounded workspace dimensions. */
typedef enum : uint32_t {
  k_stream_strings       = 64U,   /**< Exact string-pool bytes.           */
  k_stream_pool          = 6U,    /**< Two gray4 bytes plus four SVG.     */
  k_stream_chunk         = 64U,   /**< Writer/reader inflated chunk size. */
  k_stream_compressed    = 256U,  /**< One compressed-stream budget.      */
  k_stream_packed        = 2048U, /**< Complete RBKC destination budget.  */
  k_stream_offsets       = 8U,    /**< Chunk-table entry budget.          */
  k_stream_validate_work = 17U,   /**< CRC transfer + node mark budget.   */
} stream_limit_t;

/** @brief Wire-valid flat fixture with exact canonical segment ordering. */
typedef struct {
  ra8_book_header_t     hdr;                       /**< Flat header.      */
  ra8_book_chapter_t    chapters[1];               /**< One chapter.      */
  ra8_book_node_t       nodes[2];                  /**< Element + text.   */
  ra8_book_attr_t       attrs[1];                  /**< One class attr.   */
  ra8_book_stylesheet_t styles[1];                 /**< One global style. */
  ra8_book_image_t      images[2];                 /**< Raster + SVG.     */
  char                  strings[k_stream_strings]; /**< Interned strings. */
  uint8_t               pool[k_stream_pool];       /**< Raw image bytes.  */
} stream_book_t;

/** @brief Memory-backed exact reader with deterministic fault injection. */
typedef struct {
  uint8_t* data;      /**< Backing bytes.                         */
  uint64_t len;       /**< Exact readable/writable byte count.    */
  uint32_t calls;     /**< Read callback invocation count.        */
  uint32_t fail_call; /**< One-based failing call, zero disables. */
} stream_mem_t;

/** @brief One live packed-reader fixture for strict guard fault vectors. */
typedef struct {
  stream_mem_t       file;                    /**< Packed file callback state. */
  uint64_t           table[k_stream_offsets]; /**< Retained chunk table.       */
  ra8_book_chunked_t reader;                  /**< Open reader under test.     */
} stream_guard_fixture_t;

/** @brief The one canonical flat fixture every vector mutates and rebuilds. */
extern stream_book_t g_book;

/** @brief Bounded CRC-transfer and node-ownership scratch for every pass. */
extern uint8_t g_validate_work[k_stream_validate_work];

/** @brief Compressed-chunk staging the packed reader is opened against. */
extern uint8_t g_reader_staging[k_stream_compressed];

/** @brief Inflated-chunk transfer buffer handed to the strict chunk reader. */
extern uint8_t g_reader_chunk[k_stream_chunk];

/**
 * @brief Report the fixture's logical length without trailing struct padding.
 * @details The RABOOK1 total_size field counts the image pool's last byte, not
 * whatever alignment padding the host appends to ::stream_book_t.
 * @return Canonical flat-image byte count.
 * @pre ::g_book has the canonical fixture layout.
 * @pre The host lays ::stream_book_t out in declaration order.
 * @post No fixture or workspace state is modified.
 * @post The result never exceeds `sizeof(stream_book_t)`.
 * @note Test-target-private, pure and synchronous.
 * @since Version 0.1.0
 */
RA8_PRIV uint32_t priv_book_fixture_flat_len(void);

/**
 * @brief Recompute the fixture body CRC after one semantic corruption vector.
 * @details Vectors that corrupt a field the strict validator reaches only
 * after the CRC pass must restore the CRC, or the CRC decision masks them.
 * @return Nothing.
 * @pre ::g_book holds the mutated image to be re-sealed.
 * @pre The mutation left every segment offset canonical.
 * @post `g_book.hdr.crc32` matches the current body bytes.
 * @post No byte outside the header CRC field is modified.
 * @note Test-target-private and synchronous.
 * @since Version 0.1.0
 */
RA8_PRIV void priv_book_fixture_refresh_crc(void);

/**
 * @brief Rebuild one canonical flat blob accepted by every strict pass.
 * @details Zeroes ::g_book, lays out the header, interns the string pool, and
 * writes the chapter, node, attribute, stylesheet and image records before
 * sealing the body CRC.
 * @return Nothing.
 * @pre The assertion process is initialized (interning asserts its bounds).
 * @pre No live reader borrows ::g_book.
 * @post ::g_book validates clean through the strict flat pass.
 * @post Every earlier vector's corruption is gone.
 * @note Test-target-private and synchronous.
 * @since Version 0.1.0
 */
RA8_PRIV void priv_book_fixture_setup(void);

/**
 * @brief Serve one exact bounded read from a memory-backed fixture source.
 * @details Counts every invocation and fails the call whose one-based index
 * matches `fail_call`, which is how the read-status operands are varied.
 * @param[in,out] ctx Memory source descriptor; its call counter advances.
 * @param[in] offset Source byte offset of the requested span.
 * @param[out] dst Destination buffer of at least @p len bytes.
 * @param[in] len Exact byte count to transfer.
 * @return Exact-read status.
 * @retval k_ra8_ok The requested span was copied in full.
 * @retval k_ra8_err_hw_timeout The armed fault injection fired on this call.
 * @retval k_ra8_err_out_of_range The span leaves the described source.
 * @pre @p ctx addresses one live ::stream_mem_t.
 * @pre @p dst holds at least @p len bytes.
 * @post The call counter advanced by exactly one.
 * @post A rejected span leaves @p dst untouched.
 * @note Test-target-private; matches ::ra8_book_stream_read_fn.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_fixture_read(void* ctx, uint64_t offset, uint8_t* dst, uint32_t len);

/**
 * @brief Validate the current flat fixture through the public strict API.
 * @details Wraps ::g_book in a memory source and hands it, with the shared
 * bounded scratch, to ra8_book_validate_stream_strict().
 * @return Strict flat-validation status for the current ::g_book contents.
 * @retval k_ra8_ok The fixture is wire-valid end to end.
 * @pre ::g_book holds the image under test.
 * @pre ::g_validate_work is not borrowed by another live pass.
 * @post No fixture byte is modified.
 * @post The decoded header is discarded, so only the status is observable.
 * @note Test-target-private and synchronous.
 * @since Version 0.1.0
 */
RA8_PRIV ra8_err_t priv_book_fixture_validate(void);

/**
 * @brief Bind direct private-validator calls to the canonical flat fixture.
 * @details Builds the context the module-private validation seams take, so a
 * vector can reach an operand no validated public layout can request.
 * @param[out] mem Receives the exact memory-source descriptor.
 * @return Fully initialized private validation context.
 * @pre priv_book_fixture_setup() initialized ::g_book.
 * @pre @p mem addresses one writable descriptor.
 * @post The result reads from ::g_book and borrows ::g_validate_work.
 * @post No validation pass has run yet.
 * @note Test-target-private; storage stays caller-owned.
 * @since Version 0.1.0
 */
RA8_PRIV stream_validate_t priv_book_fixture_context(stream_mem_t* mem);

/**
 * @brief Pack the canonical fixture into RBKC and open it as a chunk reader.
 * @details Rebuilds ::g_book, writes it through the production container
 * writer into the fixture's packed storage, and opens a chunked reader over
 * the result against ::g_reader_staging.
 * @param[out] fixture Receives the packed file view, chunk table and reader.
 * @return Nothing.
 * @pre @p fixture addresses one writable, zero-initialized fixture.
 * @pre No other live reader borrows ::g_reader_staging.
 * @post The container write and the reader open both succeeded.
 * @post `fixture->reader` is open over `fixture->file` and `fixture->table`.
 * @note Test-target-private and synchronous.
 * @since Version 0.1.0
 */
RA8_PRIV void priv_book_fixture_open_packed(stream_guard_fixture_t* fixture);
