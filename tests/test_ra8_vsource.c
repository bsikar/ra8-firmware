/**
 * @file test_ra8_vsource.c
 * @brief Unit tests for the ra8_mem object-source registry (Layer 1, #147).
 *
 * @details
 * Exercises paged + XIP registration, the loader adapter (content + zero-padded
 * tail past the object end), the XIP direct-pointer path with bounds checks,
 * the validation guards, and -- the headline -- the Layer 1 + Layer 2
 * integration: a page cache wired to ra8_vsource_loader pages a file-like
 * object in from a fake backing and reads back the exact bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/**
 * @enum t_vsrc_t
 * @brief Backing-store fill patterns and the object-id seed.
 */
typedef enum : uint32_t {
  k_t_store_stride  = 7U,          /**< Multiplier of the store byte pattern. */
  k_t_xip_mask      = 0xA5U,       /**< XOR mask of the XIP pattern; different
                                   from the store pattern so a source mix-up
                                   is visible.                              */
  k_t_frame_hdr_len = 36U,         /**< Header bytes before the frame payload,
                                   where the comparison starts.             */
  k_t_oid_unset     = 0xFFFFFFFFU, /**< Pre-set object id; a lookup that fails
                                   must leave it.                           */
} t_vsrc_t;

/**
 * @enum t_vs_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_store_bytes = 2048U, /**< Fake backing size.       */
  k_t_objs        = 4U,    /**< Registry capacity.       */
  k_t_frame_bytes = 64U,   /**< Page-cache frame size.   */
  k_t_frames      = 8U,    /**< Page-cache frames.       */
  k_t_buckets     = 16U,   /**< Page-cache hash buckets. */
  k_t_xip_bytes   = 256U,  /**< XIP object size.         */
} t_vs_const_t;

static uint8_t           s_store[(size_t)k_t_store_bytes]; /**< Fake slow storage. */
static uint8_t           s_xip[(size_t)k_t_xip_bytes];     /**< Fake XIP region.   */
static ra8_vsource_obj_t s_objs[(size_t)k_t_objs];

/**
 * @brief Read an exact range from the deterministic fake backing store.
 * @details Rejects ranges beyond the fixture capacity and otherwise copies the
 * requested bytes.
 * @param[in] ctx Unused injected source context.
 * @param[in] offset Byte offset within the fake backing store.
 * @param[out] buf Destination receiving `len` bytes on success.
 * @param[in] len Number of bytes requested by the source adapter.
 * @return A repository error code describing the read result.
 * @retval k_ra8_ok The requested range was copied exactly.
 * @retval k_ra8_err_out_of_range The requested end exceeds the backing store.
 * @pre `buf` addresses at least `len` writable bytes.
 * @pre Offset addition is representable for these bounded fixture values.
 * @post Successful output equals the corresponding backing-store range.
 * @post An out-of-range request performs no copy.
 * @note The callback deliberately ignores context to exercise a context-free
 * binding.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_t_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if ((offset + (uint64_t)len) > (uint64_t)k_t_store_bytes) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf, &s_store[offset], (size_t)len);
  return k_ra8_ok;
}

/**
 * @brief Fill paged and XIP backing arrays with distinct deterministic
 * patterns.
 * @details Generates one affine byte sequence for slow storage and one XOR
 * sequence for XIP.
 * @pre Both file-scope backing arrays are writable.
 * @pre Their enum capacities match their declared array extents.
 * @post Every paged byte follows the stride-plus-one fixture formula.
 * @post Every XIP byte follows the index-XOR-mask formula.
 * @note Distinct formulas expose accidental source-kind substitution.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_t_fill_backing(void)
{
  for (uint32_t i = 0; i < (uint32_t)k_t_store_bytes; ++i) {
    s_store[i] = (uint8_t)((i * k_t_store_stride) + 1U);
  }
  for (uint32_t i = 0; i < (uint32_t)k_t_xip_bytes; ++i) {
    s_xip[i] = (uint8_t)(i ^ k_t_xip_mask);
  }
}

/**
 * @brief Verify paged loading copies content and zero-pads a short tail.
 * @details Registers a 100-byte object, reads a full first frame and a partial
 * final frame, then probes EOF.
 * @pre The object table and deterministic backing store are available.
 * @pre Frame storage is large enough for the configured 64-byte page.
 * @post In-range bytes match the fake backing and tail bytes are zero.
 * @post A request starting at object end reports out of range.
 * @note The final-frame vector distinguishes source bytes from padding byte by
 * byte.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- the loader fills a frame from a paged
 * object and zero-pads the tail that lies past the object end)
 */
RA8_INTERNAL static void internal_test_loader_paged(void)
{
  TEST_BEGIN("vsource loader (paged) + zero-pad tail");
  internal_t_fill_backing();
  ra8_vsource_t vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs, k_t_objs));
  uint32_t oid = k_t_oid_unset;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_vsource_add_paged(&vs, internal_t_read, nullptr, 0U, 100U, &oid)); /* size 100 */
  TEST_ASSERT_EQ(0U, oid);

  /* A 64-byte frame fully inside the object */
  uint8_t frame[(size_t)k_t_frame_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_loader(&vs, oid, 0U, frame, k_t_frame_bytes));
  TEST_ASSERT_EQ(0, memcmp(frame, &s_store[0], (size_t)k_t_frame_bytes));

  /* A frame at offset 64: object has 100 bytes, so 36 valid + 28 zero-pad */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_loader(&vs, oid, 64U, frame, k_t_frame_bytes));
  TEST_ASSERT_EQ(0, memcmp(frame, &s_store[64], 36U));
  for (uint32_t i = k_t_frame_hdr_len; i < (uint32_t)k_t_frame_bytes; ++i) {
    TEST_ASSERT_EQ(0, frame[i]); /* zero-padded past object end */
  }
  /* Offset at/after the object size -> out_of_range */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_vsource_loader(&vs, oid, 100U, frame, k_t_frame_bytes));
  TEST_END("vsource loader (paged) + zero-pad tail");
}

/**
 * @brief Verify direct XIP views and source-kind bounds enforcement.
 * @details Registers paged and XIP objects, checks a valid direct span, an
 * overflowing span, and a paged-object request.
 * @pre Deterministic paged and XIP buffers have been filled.
 * @pre The registry capacity accommodates both objects.
 * @post A valid XIP request returns the exact backing-array address and bytes.
 * @post Overflow and paged-kind requests return their distinct documented
 * errors.
 * @note Direct-pointer identity is checked in addition to byte equality.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- an XIP object yields a direct pointer
 * with bounds checks, and a paged object refuses the XIP path)
 */
RA8_INTERNAL static void internal_test_xip(void)
{
  TEST_BEGIN("vsource XIP pointer");
  internal_t_fill_backing();
  ra8_vsource_t vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs, k_t_objs));
  uint32_t paged_id = 0;
  uint32_t xip_id   = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(&vs, internal_t_read, nullptr, 0U, 100U, &paged_id));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_add_xip(&vs, s_xip, k_t_xip_bytes, &xip_id));

  const uint8_t* p = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_xip_ptr(&vs, xip_id, 32U, 16U, &p));
  TEST_ASSERT(p == &s_xip[32]);
  TEST_ASSERT_EQ(0, memcmp(p, &s_xip[32], 16U));
  /* out-of-range length, and the paged object refuses XIP */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_vsource_xip_ptr(&vs, xip_id, 250U, 16U, &p));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_vsource_xip_ptr(&vs, paged_id, 0U, 4U, &p));
  TEST_END("vsource XIP pointer");
}

/**
 * @brief Verify a vsource paged object feeds the vmem cache exactly.
 * @details Binds caller-owned cache storage to the source loader and compares
 * three fetched pages with independent backing ranges.
 * @pre Cache arrays and the fake source store satisfy their configured
 * capacities.
 * @pre The registry contains one paged object spanning the backing store.
 * @post Every fetched page matches the corresponding source bytes.
 * @post Each borrowed page is returned to the cache before the next iteration.
 * @note Block-local cache arrays avoid hidden cross-test residency state.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a page cache wired to ra8_vsource_loader
 * pages an object in from the backing and reads back the exact bytes)
 */
RA8_INTERNAL static void internal_test_vmem_integration(void)
{
  TEST_BEGIN("vsource + vmem page-in integration");
  internal_t_fill_backing();
  static uint8_t          frames[(size_t)k_t_frames * (size_t)k_t_frame_bytes];
  static ra8_vmem_frame_t meta[(size_t)k_t_frames];
  static ra8_vmem_key_t   keys[(size_t)k_t_frames];
  static int32_t          buckets[(size_t)k_t_buckets];

  ra8_vsource_t vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs, k_t_objs));
  uint32_t oid = 0;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(&vs, internal_t_read, nullptr, 0U, k_t_store_bytes, &oid));

  ra8_vmem_cfg_t cfg = {};
  cfg.frame_mem      = frames;
  cfg.frame_bytes    = k_t_frame_bytes;
  cfg.frame_count    = k_t_frames;
  cfg.meta           = meta;
  cfg.keys           = keys;
  cfg.buckets        = buckets;
  cfg.bucket_count   = k_t_buckets;
  cfg.loader         = ra8_vsource_loader;
  cfg.loader_ctx     = &vs;
  ra8_vmem_t vm      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  /* Page in three different offsets and verify against the backing. */
  const uint32_t offs[3] = {0U, 128U, 1024U};
  for (uint32_t k = 0; k < 3U; ++k) {
    void* page = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_get(&vm, oid, (uint64_t)offs[k], &page));
    TEST_ASSERT_EQ(0, memcmp(page, &s_store[offs[k]], (size_t)k_t_frame_bytes));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, page));
  }
  TEST_END("vsource + vmem page-in integration");
}

/**
 * @brief Verify vsource construction, registration, loader, and capacity
 * guards.
 * @details Exercises null pointers, zero capacities and sizes, registry
 * exhaustion, bad object ids, and null loader output.
 * @pre The one-entry object table and frame buffer are writable.
 * @pre The fake read callback is valid for the successful registration.
 * @post Each malformed call returns the documented error family.
 * @post The second registration reports no memory without replacing the first
 * object.
 * @note The vector intentionally separates invalid-size, null-pointer, range,
 * and capacity failures.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
RA8_INTERNAL static void internal_test_validation(void)
{
  TEST_BEGIN("vsource validation");
  ra8_vsource_t vs                          = {};
  uint32_t      id                          = 0;
  uint8_t       fr[(size_t)k_t_frame_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vsource_init(nullptr, s_objs, k_t_objs));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vsource_init(&vs, nullptr, k_t_objs));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_vsource_init(&vs, s_objs, 0U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs, 1U)); /* capacity 1 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vsource_add_paged(&vs, nullptr, nullptr, 0U, 8U, &id));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_vsource_add_paged(&vs, internal_t_read, nullptr, 0U, 0U, &id));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_add_paged(&vs, internal_t_read, nullptr, 0U, 8U, &id));
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_vsource_add_paged(&vs, internal_t_read, nullptr, 0U, 8U, &id)); /* full */

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vsource_loader(nullptr, 0U, 0U, fr, k_t_frame_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vsource_loader(&vs, 0U, 0U, nullptr, k_t_frame_bytes));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_vsource_loader(&vs, 9U, 0U, fr, k_t_frame_bytes)); /* bad id */
  TEST_END("vsource validation");
}

/**
 * @brief Consume one host-test log byte without touching target ITM MMIO.
 * @details Implements the injected logger sink as an intentional no-op for expected-error vectors.
 * @param[in] context Unused sink context.
 * @param[in] byte Unused diagnostic byte emitted by the production path.
 * @pre The test process owns the logger sink for the suite lifetime.
 * @pre No vector depends on observing diagnostic text.
 * @post No memory, descriptor, or hardware state is modified.
 * @post Control returns to the production logger immediately.
 * @note Installing this sink keeps sanitizer runs away from the target-only ITM address window.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_log_sink(void* context, uint8_t byte)
{
  (void)context;
  (void)byte;
}

int main(void)
{
  ra8_log_set_byte_sink(internal_host_log_sink, nullptr);
  internal_test_loader_paged();
  internal_test_xip();
  internal_test_vmem_integration();
  internal_test_validation();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
