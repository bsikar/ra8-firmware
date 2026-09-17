/**
 * @file rabook_compile_test_fixture.h
 * @brief Caller-owned fixture for RABOOK1 builder and finalizer tests.
 *
 * @details
 * Defines the bounded arenas and shared book-population helpers used by the
 * resident-builder and external-stream test executables. Every byte of fixture
 * storage belongs to the caller, so separate executables receive independent
 * state and the support layer never allocates memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "book.h"
#include "ra8_attributes.h"
#include "rabook_compile.h"

/**
 * @enum ra8_test_rabook_cap_t
 * @brief Fixed capacities of the shared compiler fixture.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_test_rabook_chapter_cap = 8U,    /**< Maximum fixture chapters.      */
  k_ra8_test_rabook_node_cap    = 64U,   /**< Maximum fixture DOM nodes.     */
  k_ra8_test_rabook_attr_cap    = 32U,   /**< Maximum fixture attributes.    */
  k_ra8_test_rabook_style_cap   = 4U,    /**< Maximum fixture stylesheets.   */
  k_ra8_test_rabook_image_cap   = 8U,    /**< Maximum fixture images.        */
  k_ra8_test_rabook_string_cap  = 1024U, /**< String-pool capacity in bytes. */
  k_ra8_test_rabook_imgpool_cap = 256U,  /**< Image-pool capacity in bytes.  */
  k_ra8_test_rabook_out_cap     = 4096U, /**< Output capacity in bytes.      */
} ra8_test_rabook_cap_t;

/**
 * @enum ra8_test_rabook_image_t
 * @brief Geometry of the two-byte packed gray4 cover fixture.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_test_rabook_img_w = 2U, /**< Cover width in pixels.  */
  k_ra8_test_rabook_img_h = 2U, /**< Cover height in pixels. */
} ra8_test_rabook_image_t;

/**
 * @struct ra8_test_rabook_fixture_t
 * @brief Complete caller-owned storage for one compiler test executable.
 * @details Holds builder arenas, the memory-finalizer destination, an independent
 *          stream destination, expected bytes, and an external image pool.
 * @invariant Each capacity enumerator matches its corresponding array extent.
 * @code
 * static ra8_test_rabook_fixture_t s_fixture;
 * @endcode
 * @see ra8_test_rabook_buffers
 */
typedef struct {
  book_chapter_t    chapters[k_ra8_test_rabook_chapter_cap]; /**< Chapter arena.        */
  book_node_t       nodes[k_ra8_test_rabook_node_cap];       /**< DOM-node arena.       */
  book_attr_t       attrs[k_ra8_test_rabook_attr_cap];       /**< Attribute arena.      */
  book_stylesheet_t styles[k_ra8_test_rabook_style_cap];     /**< Stylesheet arena.     */
  book_image_t      images[k_ra8_test_rabook_image_cap];     /**< Image descriptors.    */
  char              strpool[k_ra8_test_rabook_string_cap];   /**< Interned strings.     */
  uint8_t           imgpool[k_ra8_test_rabook_imgpool_cap];  /**< Resident image bytes. */
  uint8_t           out[k_ra8_test_rabook_out_cap];          /**< Memory output.        */
  uint8_t           stream_out[k_ra8_test_rabook_out_cap];   /**< Stream output.        */
  uint8_t           expected[k_ra8_test_rabook_out_cap];     /**< Expected bytes.       */
  /** @brief External image bytes. */
  uint8_t external_pool[k_ra8_test_rabook_imgpool_cap];
} ra8_test_rabook_fixture_t;

/**
 * @struct ra8_test_rabook_roundtrip_t
 * @brief Handles carried from fixture population into read-back verification.
 * @invariant Node and image indices are values returned by the builder API.
 * @see ra8_test_rabook_populate
 */
typedef struct {
  const void* blob;      /**< Finalized book blob.                 */
  uint32_t    blob_len;  /**< Blob length in bytes.                */
  uint32_t    body_idx;  /**< Node index of the body element.      */
  uint32_t    p_idx;     /**< Node index of the paragraph element. */
  uint32_t    text_idx;  /**< Node index of the text run.          */
  uint32_t    img_idx;   /**< Cover-image index.                   */
  uint32_t    body_name; /**< Interned body-tag offset.            */
} ra8_test_rabook_roundtrip_t;

/**
 * @brief Form full-capacity compiler buffers over one fixture.
 * @details Maps every builder arena to its matching fixed array without clearing
 *          or otherwise mutating fixture storage.
 * @param[in,out] fixture Caller-owned fixture storage.
 * @return Fully populated builder-buffer descriptor.
 * @retval ra8_rabook_buffers_t Descriptor whose members reference @p fixture.
 * @pre @p fixture is non-NULL.
 * @pre @p fixture remains alive while the returned descriptor is in use.
 * @post Every returned pointer is non-NULL.
 * @post Every returned capacity equals the corresponding array extent.
 * @note Test-only; the returned descriptor borrows caller storage.
 * @par MC/DC:
 * No compound decisions; this helper only constructs a bounded descriptor.
 * @since 0.1.0
 */
RA8_TEST_HELPER
ra8_rabook_buffers_t ra8_test_rabook_buffers(ra8_test_rabook_fixture_t* fixture);

/**
 * @brief Initialize a compiler context over one fixture.
 * @details Builds the bounded descriptor and requires production initialization
 *          to accept it before returning to the test.
 * @param[in,out] fixture Caller-owned fixture storage.
 * @param[out] ctx Compiler context to initialize.
 * @return Nothing; a failed production initialization fails the active test.
 * @pre @p fixture and @p ctx are non-NULL.
 * @pre The minimal Unity test harness is active.
 * @post @p ctx is initialized over @p fixture.
 * @post The empty-string sentinel occupies string-pool offset zero.
 * @note Test-only and not thread-safe with shared fixture storage.
 * @par MC/DC:
 * No compound decisions; the production return value is asserted directly.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_test_rabook_init(ra8_test_rabook_fixture_t* fixture,
                                          ra8_rabook_ctx_t*          ctx);

/**
 * @brief Populate the canonical fixture book through either image-pool API.
 * @details Adds metadata, a cover, stylesheet, two-deep DOM, and one chapter.
 * @param[in,out] fixture Caller-owned resident and external storage.
 * @param[in,out] ctx Initialized compiler context.
 * @param[out] roundtrip Receives the builder indices used by verification.
 * @param[in] external_image_pool Select external image reservation when true.
 * @return Nothing; unexpected production results fail the active test.
 * @pre All pointer arguments are non-NULL.
 * @pre @p ctx was initialized over @p fixture.
 * @post The builder is ready for memory or stream finalization.
 * @post @p roundtrip contains every index needed for read-back verification.
 * @note Test-only and not thread-safe with shared fixture storage.
 * @par MC/DC:
 * The pool selector receives both boolean values across the two test executables.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_test_rabook_populate(ra8_test_rabook_fixture_t*   fixture,
                                              ra8_rabook_ctx_t*            ctx,
                                              ra8_test_rabook_roundtrip_t* roundtrip,
                                              bool                         external_image_pool);

/**
 * @brief Verify every field of a finalized canonical fixture book.
 * @details Reads the blob only through the production reader accessors and checks
 *          metadata, chapter, DOM, attribute, image, and stylesheet records.
 * @param[in] roundtrip Finalized blob and saved builder indices.
 * @return Nothing; any mismatch fails the active test.
 * @pre @p roundtrip is non-NULL and its blob passed validation.
 * @pre Saved indices came from @ref ra8_test_rabook_populate.
 * @post Every serialized fixture field matched its builder input.
 * @post The blob remains unmodified.
 * @note Test-only and read-only after finalization.
 * @par MC/DC:
 * No compound decisions; every production accessor result is asserted directly.
 * @since 0.1.0
 */
RA8_TEST_HELPER void ra8_test_rabook_verify(const ra8_test_rabook_roundtrip_t* roundtrip);
