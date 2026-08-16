/**
 * @file test_ra8_rabook_comic_mcdc.c
 * @brief MC/DC vectors for the streaming comic-to-RABOOK facade.
 * @details Drives every new compound guard through the public comic API with
 *          real BMP normalization and caller-owned builder/spool workspaces.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_rabook_comic.h"
#include "unity_minimal.h"

/** @brief Bounded capacities for one-page guard fixtures. */
typedef enum : uint32_t {
  k_mcdc_chapters         = 2U,       /**< Chapter rows.            */
  k_mcdc_nodes            = 4U,       /**< DOM nodes.               */
  k_mcdc_attrs            = 2U,       /**< DOM attributes.          */
  k_mcdc_images           = 2U,       /**< Image descriptors.       */
  k_mcdc_strings          = 2048U,    /**< Interned strings.        */
  k_mcdc_codec_bytes      = 1U << 20, /**< Decoder arena bytes.     */
  k_mcdc_pixel_bytes      = 256U,     /**< RGBA scratch bytes.      */
  k_mcdc_gray_bytes       = 64U,      /**< Gray scratch bytes.      */
  k_mcdc_normalized_bytes = 64U,      /**< Normalized page bytes.   */
  k_mcdc_spool_bytes      = 256U,     /**< External image spool.    */
  k_mcdc_flat_bytes       = 4096U,    /**< Flat RABOOK destination. */
} comic_mcdc_limit_t;

/** @brief One bounded in-memory source or destination. */
typedef struct {
  uint8_t* bytes;    /**< Mutable caller backing.            */
  uint32_t capacity; /**< Accessible byte capacity.          */
  uint32_t length;   /**< Highest initialized byte plus one. */
} comic_mcdc_memory_t;

/** @brief Complete caller state retained through one comic operation. */
typedef struct {
  ra8_rabook_comic_t           comic;     /**< Production facade state. */
  ra8_rabook_comic_workspace_t workspace; /**< Bound caller workspace.  */
  ra8_img_arena_t              stb;       /**< stb decoder descriptor.  */
  ra8_webp_arena_t             webp;      /**< WebP decoder descriptor. */
  comic_mcdc_memory_t          image;     /**< Normalized image spool.  */
  comic_mcdc_memory_t          flat;      /**< Flat RABOOK destination. */
} comic_mcdc_fixture_t;

/** @brief 2x2 24-bit BMP used by every reachable page vector. */
static const uint8_t s_bmp[] = {
  0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
  0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
  0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
  0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

static ra8_book_chapter_t    s_chapters[k_mcdc_chapters];
static ra8_book_node_t       s_nodes[k_mcdc_nodes];
static ra8_book_attr_t       s_attrs[k_mcdc_attrs];
static ra8_book_stylesheet_t s_styles[1];
static ra8_book_image_t      s_images[k_mcdc_images];
static char                  s_strings[k_mcdc_strings];
static uint8_t               s_dummy_image_pool[1];
static uint8_t               s_dummy_output[1];
alignas(16) static uint8_t s_stb_arena[k_mcdc_codec_bytes];
alignas(16) static uint8_t s_webp_arena[k_mcdc_codec_bytes];
static uint8_t s_rgba[k_mcdc_pixel_bytes];
static uint8_t s_gray[k_mcdc_gray_bytes];
static uint8_t s_normalized[k_mcdc_normalized_bytes];
static uint8_t s_stream_scratch[k_mcdc_normalized_bytes];
static uint8_t s_image_bytes[k_mcdc_spool_bytes];
static uint8_t s_flat_bytes[k_mcdc_flat_bytes];

/**
 * @brief Write one exact span into a bounded memory object.
 * @details Checks the complete range before copying and advances the logical
 *          extent only after all requested bytes fit.
 * @param[in,out] memory Destination object.
 * @param[in] offset Destination offset.
 * @param[in] source Exact source bytes.
 * @param[in] requested Requested byte count.
 * @param[out] out_written Accepted byte count.
 * @return Memory status.
 * @retval k_ra8_ok Every byte was stored.
 * @retval k_ra8_err_invalid_size The complete range does not fit.
 * @pre Pointers are non-NULL and source spans requested bytes.
 * @pre Memory backing remains exclusively mutable.
 * @post Success initializes the complete destination range.
 * @post Failure reports zero without changing the logical extent.
 * @note Test-only callback; not thread-safe through the fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_memory_write(comic_mcdc_memory_t* memory,
                                                    uint32_t             offset,
                                                    const uint8_t*       source,
                                                    uint32_t             requested,
                                                    uint32_t*            out_written)
{
  *out_written = 0U;
  if ((offset > memory->capacity) || (requested > (memory->capacity - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&memory->bytes[offset], source, requested);
  const uint32_t end = offset + requested;
  if (end > memory->length) {
    memory->length = end;
  }
  *out_written = requested;
  return k_ra8_ok;
}

/**
 * @brief Adapt the bounded writer to the comic image-spool contract.
 * @details Delegates the exact offset/span operation to the common bounded
 *          memory writer without changing callback status or progress.
 * @param[in,out] ctx Bound ::comic_mcdc_memory_t.
 * @param[in] offset Image-pool offset.
 * @param[in] source Normalized image bytes.
 * @param[in] requested Requested byte count.
 * @param[out] out_written Accepted byte count.
 * @return ::internal_memory_write status.
 * @retval k_ra8_ok Every requested byte was stored.
 * @retval k_ra8_err_invalid_size The complete range does not fit.
 * @pre Callback arguments satisfy ::ra8_rabook_comic_write_at_fn.
 * @pre Context remains exclusively mutable.
 * @post Results match ::internal_memory_write.
 * @post No hidden state or allocation is used.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_image_write(void*          ctx,
                                                   uint32_t       offset,
                                                   const uint8_t* source,
                                                   uint32_t       requested,
                                                   uint32_t*      out_written)
{
  return internal_memory_write(ctx, offset, source, requested, out_written);
}

/**
 * @brief Read one exact initialized image-spool span.
 * @details Rejects the complete range before copying so the finalizer cannot
 *          observe uninitialized spool bytes or partial progress.
 * @param[in] ctx Bound ::comic_mcdc_memory_t source.
 * @param[in] offset Source offset.
 * @param[out] destination Exact destination span.
 * @param[in] requested Requested byte count.
 * @param[out] out_read Copied byte count.
 * @return Read status.
 * @retval k_ra8_ok Every byte was copied.
 * @retval k_ra8_err_invalid_size The requested range is not initialized.
 * @pre Pointers are non-NULL and destination spans requested bytes.
 * @pre Source remains immutable.
 * @post Success reports requested bytes; failure reports zero.
 * @post No source byte is modified.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_image_read(void*     ctx,
                                                  uint32_t  offset,
                                                  uint8_t*  destination,
                                                  uint32_t  requested,
                                                  uint32_t* out_read)
{
  const comic_mcdc_memory_t* memory = ctx;
  *out_read                         = 0U;
  if ((offset > memory->length) || (requested > (memory->length - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(destination, &memory->bytes[offset], requested);
  *out_read = requested;
  return k_ra8_ok;
}

/**
 * @brief Append one exact flat-book span to bounded memory.
 * @details Uses the current initialized length as the next append offset and
 *          otherwise preserves the common writer's exact semantics.
 * @param[in,out] ctx Bound ::comic_mcdc_memory_t destination.
 * @param[in] source Flat-book bytes.
 * @param[in] requested Requested byte count.
 * @param[out] out_written Accepted byte count.
 * @return ::internal_memory_write status.
 * @retval k_ra8_ok Every requested byte was appended.
 * @retval k_ra8_err_invalid_size The complete append does not fit.
 * @pre Callback arguments satisfy ::ra8_rabook_write_fn.
 * @pre Current length is the append offset.
 * @post Success extends the object by requested bytes.
 * @post Failure preserves the initialized prefix.
 * @note Test-only callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_flat_append(void* ctx, const uint8_t* source, uint32_t requested, uint32_t* out_written)
{
  comic_mcdc_memory_t* memory = ctx;
  return internal_memory_write(memory, memory->length, source, requested, out_written);
}

/**
 * @brief Bind every caller-owned arena for one isolated vector.
 * @details Resets the public fixture state and permits the node capacity to be
 *          reduced to the exact body/image failure boundary.
 * @param[out] fixture Complete fixture state.
 * @param[in] node_cap DOM node capacity for this vector.
 * @pre Static backing arrays are exclusively owned.
 * @pre Fixture is non-NULL and writable.
 * @post Every workspace pointer and capacity is initialized.
 * @post Image and flat logical extents are zero.
 * @note Not thread-safe because backing arrays are shared.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind_fixture(comic_mcdc_fixture_t* fixture, uint32_t node_cap)
{
  *fixture       = (comic_mcdc_fixture_t){};
  fixture->image = (comic_mcdc_memory_t){.bytes = s_image_bytes, .capacity = sizeof s_image_bytes};
  fixture->flat  = (comic_mcdc_memory_t){.bytes = s_flat_bytes, .capacity = sizeof s_flat_bytes};
  fixture->stb   = (ra8_img_arena_t){.base = s_stb_arena, .cap = sizeof s_stb_arena};
  fixture->webp  = (ra8_webp_arena_t){.base = s_webp_arena, .cap = sizeof s_webp_arena};
  fixture->workspace.builder = (ra8_rabook_buffers_t){.chapters       = s_chapters,
                                                      .nodes          = s_nodes,
                                                      .attrs          = s_attrs,
                                                      .stylesheets    = s_styles,
                                                      .images         = s_images,
                                                      .string_pool    = s_strings,
                                                      .image_pool     = s_dummy_image_pool,
                                                      .out            = s_dummy_output,
                                                      .chapter_cap    = k_mcdc_chapters,
                                                      .node_cap       = node_cap,
                                                      .attr_cap       = k_mcdc_attrs,
                                                      .stylesheet_cap = 1U,
                                                      .image_cap      = k_mcdc_images,
                                                      .string_cap     = k_mcdc_strings,
                                                      .image_pool_cap = sizeof s_dummy_image_pool,
                                                      .out_cap        = sizeof s_dummy_output};
  fixture->workspace.raster  = (ra8_rabook_raster_workspace_t){.stb_arena  = &fixture->stb,
                                                               .webp_arena = &fixture->webp,
                                                               .rgba       = s_rgba,
                                                               .rgba_cap   = sizeof s_rgba,
                                                               .gray       = s_gray,
                                                               .gray_cap   = sizeof s_gray};
  fixture->workspace.normalized         = s_normalized;
  fixture->workspace.normalized_cap     = sizeof s_normalized;
  fixture->workspace.stream_scratch     = s_stream_scratch;
  fixture->workspace.stream_scratch_cap = sizeof s_stream_scratch;
}

/**
 * @brief Initialize one valid fixture with a selected node capacity.
 * @details Rebinds all caller storage, then passes the production facade its
 *          real raster callbacks, image context, and gray4 output policy.
 * @param[out] fixture Bound and initialized comic fixture.
 * @param[in] node_cap DOM node capacity.
 * @return Production initialization status.
 * @retval k_ra8_ok The empty fixture is active.
 * @return Constructor validation or builder errors propagate unchanged.
 * @pre Static fixture storage is exclusively owned.
 * @pre Fixture is non-NULL and writable.
 * @post Success leaves an active zero-page comic.
 * @post Failure leaves the facade inactive.
 * @note Test-only orchestration helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_begin_fixture(comic_mcdc_fixture_t* fixture,
                                                     uint32_t              node_cap)
{
  internal_bind_fixture(fixture, node_cap);
  return ra8_rabook_comic_init(&fixture->comic,
                               &fixture->workspace,
                               internal_image_read,
                               internal_image_write,
                               &fixture->image,
                               0U,
                               (uint8_t)k_ra8_book_pixfmt_gray4);
}

/**
 * @brief Run one add-page vector against a fresh production fixture.
 * @details Rebuilds the facade for every observation so terminal add failures
 *          cannot contaminate a later vector's lifecycle state.
 * @param[in] node_cap DOM node capacity.
 * @param[in] page_id Candidate page identifier.
 * @param[in] source Candidate encoded source.
 * @param[in] source_size Candidate source extent.
 * @param[in] expected Expected production status.
 * @pre Non-NULL source arguments span their declared lengths.
 * @pre Static fixture storage is exclusively owned.
 * @post The observed add-page result equals @p expected.
 * @post No fixture state escapes the helper.
 * @note Test-only orchestration helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_add(uint32_t       node_cap,
                                             const char*    page_id,
                                             const uint8_t* source,
                                             size_t         source_size,
                                             ra8_err_t      expected)
{
  comic_mcdc_fixture_t fixture = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_begin_fixture(&fixture, node_cap));
  TEST_ASSERT_EQ(expected, ra8_rabook_comic_add_page(&fixture.comic, page_id, source, source_size));
}

/**
 * @brief Run one finish vector against a fresh one-page comic.
 * @details Normalizes a real BMP page before selecting valid or null output
 *          arguments and comparing the production finish status exactly.
 * @param[in] omit_write Replace the output callback with NULL.
 * @param[in] omit_ctx Replace the output context with NULL.
 * @param[in] omit_length Replace the length output with NULL.
 * @param[in] metadata Candidate final metadata.
 * @param[in] expected Expected production status.
 * @pre Metadata strings are readable through the bounded comic limit.
 * @pre Static fixture storage is exclusively owned.
 * @post The observed finish result equals @p expected.
 * @post No fixture state escapes the helper.
 * @note Test-only orchestration helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_finish(bool                               omit_write,
                                                bool                               omit_ctx,
                                                bool                               omit_length,
                                                const ra8_rabook_comic_metadata_t* metadata,
                                                ra8_err_t                          expected)
{
  comic_mcdc_fixture_t fixture = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_begin_fixture(&fixture, k_mcdc_nodes));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_comic_add_page(&fixture.comic, "page.bmp", s_bmp, sizeof s_bmp));
  uint32_t            flat_length = 0U;
  ra8_rabook_write_fn write       = omit_write ? nullptr : internal_flat_append;
  void*               write_ctx   = omit_ctx ? nullptr : &fixture.flat;
  uint32_t*           length      = omit_length ? nullptr : &flat_length;
  TEST_ASSERT_EQ(expected,
                 ra8_rabook_comic_finish(&fixture.comic, metadata, write, write_ctx, length));
}

/**
 * @brief Initialization pointer terms vary independently.
 * @test A valid control and four one-true vectors exercise the first guard.
 * @details Reuses one fully bound workspace while nulling each required input
 *          independently; a fresh output comic is zeroed on every invocation.
 * @par MC/DC:
 * `libs/ra8_rabook_compile/src/ra8_rabook_comic.c@ra8_rabook_comic_init`
 * receives `(F,F,F,F)->F`, `(T,-,-,-)->T`, `(F,T,-,-)->T`,
 * `(F,F,T,-)->T`, and `(F,F,F,T)->T` for its input-pointer OR.
 * Companion capacity/format vectors below cover its other compound guards.
 * @pre Static fixture storage is exclusively owned.
 * @pre The control workspace is complete and writable.
 * @post The valid control succeeds and each null input is rejected.
 * @post No failed vector reports an active comic.
 * @note Null comic itself is a separate single-condition guard.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_init_inputs(void)
{
  TEST_BEGIN("rabook comic: init input MC/DC");
  comic_mcdc_fixture_t fixture = {};
  internal_bind_fixture(&fixture, k_mcdc_nodes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_comic_init(&fixture.comic,
                                       nullptr,
                                       internal_image_read,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       nullptr,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       nullptr,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       internal_image_write,
                                       nullptr,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  TEST_END("rabook comic: init input MC/DC");
}

/**
 * @brief Workspace pointer and capacity terms vary independently.
 * @test Pointer OR and capacity OR each receive their N+1 vectors.
 * @details Nulls and zeroes one workspace field at a time while all remaining
 *          required fields retain the valid control values.
 * @pre Static fixture storage is exclusively owned.
 * @pre Every restored workspace field retains its original exact capacity.
 * @post Each malformed workspace returns its canonical status.
 * @post The untouched terms remain valid in every one-true vector.
 * @note Completes the MC/DC table cited by ::internal_test_mcdc_init_inputs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_init_workspace(void)
{
  TEST_BEGIN("rabook comic: init workspace MC/DC");
  comic_mcdc_fixture_t fixture = {};
  internal_bind_fixture(&fixture, k_mcdc_nodes);
  fixture.workspace.normalized = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  internal_bind_fixture(&fixture, k_mcdc_nodes);
  fixture.workspace.stream_scratch = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  internal_bind_fixture(&fixture, k_mcdc_nodes);
  fixture.workspace.normalized_cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  internal_bind_fixture(&fixture, k_mcdc_nodes);
  fixture.workspace.stream_scratch_cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray4));
  TEST_END("rabook comic: init workspace MC/DC");
}

/**
 * @brief Pixel-format terms vary independently with a complete workspace.
 * @test Gray4, gray8, and an unsupported value exercise the two-term AND.
 * @details The companion input test supplies the gray4 false-first control;
 *          this test supplies `(T,F)->F` and `(T,T)->T`.
 * @pre Static fixture storage is exclusively owned.
 * @pre The workspace is complete and every capacity is nonzero.
 * @post Gray8 initializes successfully and the unsupported value is rejected.
 * @post No failed vector reports an active comic.
 * @note Completes the MC/DC table cited by ::internal_test_mcdc_init_inputs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_init_format(void)
{
  TEST_BEGIN("rabook comic: init format MC/DC");
  comic_mcdc_fixture_t fixture = {};
  internal_bind_fixture(&fixture, k_mcdc_nodes);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       (uint8_t)k_ra8_book_pixfmt_gray8));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_comic_init(&fixture.comic,
                                       &fixture.workspace,
                                       internal_image_read,
                                       internal_image_write,
                                       &fixture.image,
                                       0U,
                                       UINT8_MAX));
  TEST_END("rabook comic: init format MC/DC");
}

/**
 * @brief Page-input and body/image allocation terms vary independently.
 * @test Fresh fixtures isolate each input OR term and each node-allocation term.
 * @details Node capacities two, zero, and one produce `(F,F)`, `(T,-)`, and
 *          `(F,T)` for the body/image failure OR after real BMP normalization.
 * @par MC/DC:
 * `libs/ra8_rabook_compile/src/ra8_rabook_comic.c@ra8_rabook_comic_add_page`
 * receives `(F,F,F)->F` and each one-true input vector.
 * `libs/ra8_rabook_compile/src/ra8_rabook_comic.c@internal_append_chapter`
 * receives `(F,F)->F`, `(T,-)->T`, and `(F,T)->T`.
 * @pre Static fixture storage is exclusively owned.
 * @pre The BMP fixture remains immutable.
 * @post Valid capacity appends one page; malformed inputs are rejected.
 * @post Zero/one-node fixtures fail specifically at body/image allocation.
 * @note Each terminal failure is isolated in a fresh private build.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_add_page(void)
{
  TEST_BEGIN("rabook comic: add-page MC/DC");
  internal_expect_add(2U, "page.bmp", s_bmp, sizeof s_bmp, k_ra8_ok);
  internal_expect_add(2U, nullptr, s_bmp, sizeof s_bmp, k_ra8_err_invalid_arg);
  internal_expect_add(2U, "page.bmp", nullptr, sizeof s_bmp, k_ra8_err_invalid_arg);
  internal_expect_add(2U, "page.bmp", s_bmp, 0U, k_ra8_err_invalid_arg);
  internal_expect_add(0U, "page.bmp", s_bmp, sizeof s_bmp, k_ra8_err_no_mem);
  internal_expect_add(1U, "page.bmp", s_bmp, sizeof s_bmp, k_ra8_err_no_mem);
  TEST_END("rabook comic: add-page MC/DC");
}

/**
 * @brief Finish output terms vary independently after one valid page.
 * @test A valid control and three one-true vectors exercise the output OR.
 * @details Every vector rebuilds the page so finish's terminal state transition
 *          cannot influence a later observation.
 * @par MC/DC:
 * `libs/ra8_rabook_compile/src/ra8_rabook_comic.c@ra8_rabook_comic_finish`
 * receives `(F,F,F)->F`, `(T,-,-)->T`, `(F,T,-)->T`, `(F,F,T)->T`.
 * @pre Static fixture storage is exclusively owned.
 * @pre Valid metadata remains immutable.
 * @post The control emits a flat book and each null output is rejected.
 * @post Every build is inactive after finish returns.
 * @note Metadata behavior is isolated in the companion test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_finish_outputs(void)
{
  TEST_BEGIN("rabook comic: finish output MC/DC");
  const ra8_rabook_comic_metadata_t metadata = {.title      = "Comic",
                                                .author     = "RA8",
                                                .language   = "en",
                                                .identifier = "urn:ra8:mcdc"};
  internal_expect_finish(false, false, false, &metadata, k_ra8_ok);
  internal_expect_finish(true, false, false, &metadata, k_ra8_err_null_ptr);
  internal_expect_finish(false, true, false, &metadata, k_ra8_err_null_ptr);
  internal_expect_finish(false, false, true, &metadata, k_ra8_err_null_ptr);
  TEST_END("rabook comic: finish output MC/DC");
}

/**
 * @brief Metadata bounded-string terms vary independently.
 * @test Four one-true vectors compare with a fully bounded control.
 * @details A 512-byte non-NUL span occupies each field in turn, after all
 *          preceding fields remain valid and therefore evaluate false.
 * @par MC/DC:
 * `libs/ra8_rabook_compile/src/ra8_rabook_comic.c@internal_set_metadata`
 * receives `(F,F,F,F)->F`, `(T,-,-,-)->T`, `(F,T,-,-)->T`,
 * `(F,F,T,-)->T`, and `(F,F,F,T)->T`.
 * @pre Static fixture storage is exclusively owned.
 * @pre The local hostile span is readable through the exact public bound.
 * @post The bounded control succeeds and each hostile field is rejected.
 * @post No invalid metadata reaches the streaming finalizer.
 * @note Each vector owns a fresh populated comic.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_metadata(void)
{
  TEST_BEGIN("rabook comic: metadata MC/DC");
  char unterminated[k_ra8_rabook_comic_string_max];
  (void)memset(unterminated, 'x', sizeof unterminated);
  const ra8_rabook_comic_metadata_t good      = {.title      = "Comic",
                                                 .author     = "RA8",
                                                 .language   = "en",
                                                 .identifier = "urn:ra8:mcdc"};
  ra8_rabook_comic_metadata_t       candidate = good;
  internal_expect_finish(false, false, false, &candidate, k_ra8_ok);
  candidate.title = unterminated;
  internal_expect_finish(false, false, false, &candidate, k_ra8_err_invalid_size);
  candidate        = good;
  candidate.author = unterminated;
  internal_expect_finish(false, false, false, &candidate, k_ra8_err_invalid_size);
  candidate          = good;
  candidate.language = unterminated;
  internal_expect_finish(false, false, false, &candidate, k_ra8_err_invalid_size);
  candidate            = good;
  candidate.identifier = unterminated;
  internal_expect_finish(false, false, false, &candidate, k_ra8_err_invalid_size);
  TEST_END("rabook comic: metadata MC/DC");
}

int main(void)
{
  internal_test_mcdc_init_inputs();
  internal_test_mcdc_init_workspace();
  internal_test_mcdc_init_format();
  internal_test_mcdc_add_page();
  internal_test_mcdc_finish_outputs();
  internal_test_mcdc_metadata();
  return 0;
}
