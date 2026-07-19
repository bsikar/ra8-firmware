/**
 * @file test_ra8_vsource.c
 * @brief Unit tests for the ra8_mem object-source registry (Layer 1, #147).
 *
 * @details
 * Exercises paged + XIP registration, the loader adapter (content + zero-padded
 * tail past the object end), the XIP direct-pointer path with bounds checks, the
 * validation guards, and -- the headline -- the Layer 1 + Layer 2 integration: a
 * page cache wired to ra8_vsource_loader pages a file-like object in from a
 * simulated backing and reads back the exact bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/**
 * @enum vsource_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_vsource_i_36 = 36U,
  k_vsource_i_7  = 7U,
  k_vsource_i_a5 = 0xA5U,
} vsource_uint8_const_t;

/**
 * @enum vsource_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_vsource_oid_ffffffff = 0xFFFFFFFFU,
} vsource_uint32_const_t;

/**
 * @enum t_vs_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_store_bytes = 2048U, /**< Simulated backing size.  */
  k_t_objs        = 4U,    /**< Registry capacity.       */
  k_t_frame_bytes = 64U,   /**< Page-cache frame size.   */
  k_t_frames      = 8U,    /**< Page-cache frames.       */
  k_t_buckets     = 16U,   /**< Page-cache hash buckets. */
  k_t_xip_bytes   = 256U,  /**< XIP object size.         */
} t_vs_const_t;

static uint8_t           s_store[(size_t)k_t_store_bytes]; /**< Simulated slow storage. */
static uint8_t           s_xip[(size_t)k_t_xip_bytes];     /**< Simulated XIP region.   */
static ra8_vsource_obj_t s_objs[(size_t)k_t_objs];

/** @brief Backing read callback over the simulated storage buffer. */
static ra8_err_t t_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if ((offset + (uint64_t)len) > (uint64_t)k_t_store_bytes) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf, &s_store[offset], (size_t)len);
  return k_ra8_ok;
}

/** @brief Fill the backing + XIP buffers with deterministic patterns. */
static void t_fill_backing(void)
{
  for (uint32_t i = 0; i < (uint32_t)k_t_store_bytes; ++i) {
    s_store[i] = (uint8_t)((i * k_vsource_i_7) + 1U);
  }
  for (uint32_t i = 0; i < (uint32_t)k_t_xip_bytes; ++i) {
    s_xip[i] = (uint8_t)(i ^ k_vsource_i_a5);
  }
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the loader fills a frame from a paged
 * object and zero-pads the tail that lies past the object end)
 */
static void test_loader_paged(void)
{
  TEST_BEGIN("vsource loader (paged) + zero-pad tail");
  t_fill_backing();
  ra8_vsource_t vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs, k_t_objs));
  uint32_t oid = k_vsource_oid_ffffffff;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(&vs, t_read, nullptr, 0U, 100U, &oid)); /* size 100 */
  TEST_ASSERT_EQ(0U, oid);

  /* A 64-byte frame fully inside the object */
  uint8_t frame[(size_t)k_t_frame_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_loader(&vs, oid, 0U, frame, k_t_frame_bytes));
  TEST_ASSERT_EQ(0, memcmp(frame, &s_store[0], (size_t)k_t_frame_bytes));

  /* A frame at offset 64: object has 100 bytes, so 36 valid + 28 zero-pad */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_loader(&vs, oid, 64U, frame, k_t_frame_bytes));
  TEST_ASSERT_EQ(0, memcmp(frame, &s_store[64], 36U));
  for (uint32_t i = k_vsource_i_36; i < (uint32_t)k_t_frame_bytes; ++i) {
    TEST_ASSERT_EQ(0, frame[i]); /* zero-padded past object end */
  }
  /* Offset at/after the object size -> out_of_range */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_vsource_loader(&vs, oid, 100U, frame, k_t_frame_bytes));
  TEST_END("vsource loader (paged) + zero-pad tail");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- an XIP object yields a direct pointer with
 * bounds checks, and a paged object refuses the XIP path)
 */
static void test_xip(void)
{
  TEST_BEGIN("vsource XIP pointer");
  t_fill_backing();
  ra8_vsource_t vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs, k_t_objs));
  uint32_t paged_id = 0;
  uint32_t xip_id   = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_add_paged(&vs, t_read, nullptr, 0U, 100U, &paged_id));
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
 * @par MC/DC:
 * (no compound decisions under test -- a page cache wired to ra8_vsource_loader
 * pages an object in from the backing and reads back the exact bytes)
 */
static void test_vmem_integration(void)
{
  TEST_BEGIN("vsource + vmem page-in integration");
  t_fill_backing();
  static uint8_t          s_frames[(size_t)k_t_frames * (size_t)k_t_frame_bytes];
  static ra8_vmem_frame_t s_meta[(size_t)k_t_frames];
  static int32_t          s_buckets[(size_t)k_t_buckets];

  ra8_vsource_t vs = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, s_objs, k_t_objs));
  uint32_t oid = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_add_paged(&vs, t_read, nullptr, 0U, k_t_store_bytes, &oid));

  ra8_vmem_cfg_t cfg = {};
  cfg.frame_mem      = s_frames;
  cfg.frame_bytes    = k_t_frame_bytes;
  cfg.frame_count    = k_t_frames;
  cfg.meta           = s_meta;
  cfg.buckets        = s_buckets;
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
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
static void test_validation(void)
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
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_vsource_add_paged(&vs, t_read, nullptr, 0U, 0U, &id));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_add_paged(&vs, t_read, nullptr, 0U, 8U, &id));
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_vsource_add_paged(&vs, t_read, nullptr, 0U, 8U, &id)); /* full */

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vsource_loader(nullptr, 0U, 0U, fr, k_t_frame_bytes));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vsource_loader(&vs, 0U, 0U, nullptr, k_t_frame_bytes));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_vsource_loader(&vs, 9U, 0U, fr, k_t_frame_bytes)); /* bad id */
  TEST_END("vsource validation");
}

int32_t main(void)
{
  test_loader_paged();
  test_xip();
  test_vmem_integration();
  test_validation();
  (void)fprintf(stderr, "[OK  ] test_ra8_vsource.c\n");
  return 0;
}
