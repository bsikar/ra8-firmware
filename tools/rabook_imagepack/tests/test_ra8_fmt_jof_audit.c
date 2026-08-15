/**
 * @file test_ra8_fmt_jof_audit.c
 * @brief Exact-capacity and corruption tests for the portable JOF audit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_fmt_jof_audit.h"

typedef struct {
  const uint8_t* bytes;
  size_t         len;
  bool           fail;
} test_store_t;

static int s_failures;

#define CHECK(expr)                                                                                \
  do {                                                                                             \
    if (!(expr)) {                                                                                 \
      (void)fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expr);                                     \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (false)

static void wr16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
}

static void wr32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
  p[2] = (uint8_t)(v >> 16U);
  p[3] = (uint8_t)(v >> 24U);
}

static void make_atlas(uint8_t atlas[68], const uint8_t top[2], const uint8_t bottom[2])
{
  (void)memset(atlas, 0, 68U);
  (void)memcpy(&atlas[k_ra8_jof_ofs_magic], "JOF1", 4U); /* MAGIC-OK */
  wr16(&atlas[k_ra8_jof_ofs_width], 2U);
  wr16(&atlas[k_ra8_jof_ofs_height], 2U);
  wr16(&atlas[k_ra8_jof_ofs_tile_w], 2U);
  wr16(&atlas[k_ra8_jof_ofs_tile_h], 1U);
  atlas[k_ra8_jof_ofs_bpp]   = 1U;
  atlas[k_ra8_jof_ofs_codec] = (uint8_t)k_ra8_jof_codec_raw;
  wr32(&atlas[k_ra8_jof_ofs_tile_count], 2U);
  (void)memcpy(&atlas[32], top, 2U);
  (void)memcpy(&atlas[34], bottom, 2U);
  wr32(&atlas[36], 32U);
  wr32(&atlas[40], 2U);
  wr32(&atlas[44], 34U);
  wr32(&atlas[48], 2U);
  wr32(&atlas[52 + k_ra8_jof_ftr_index_off], 36U);
  wr32(&atlas[52 + k_ra8_jof_ftr_tile_count], 2U);
  wr32(&atlas[52 + k_ra8_jof_ftr_total_size], 68U);
  (void)memcpy(&atlas[52 + k_ra8_jof_ftr_magic], "JOFE", 4U); /* MAGIC-OK */
}

static ra8_err_t test_pread(void* ctx, uint64_t offset, uint8_t* buf, size_t len, size_t* got)
{
  test_store_t* store = (test_store_t*)ctx;
  *got                = 0U;
  if (store->fail) {
    return k_ra8_fail;
  }
  if (offset >= store->len) {
    return k_ra8_ok;
  }
  size_t n = store->len - (size_t)offset;
  if (n > len) {
    n = len;
  }
  (void)memcpy(buf, &store->bytes[offset], n);
  *got = n;
  return k_ra8_ok;
}

static ra8_err_t run_audit(uint8_t                     atlas[68],
                           ra8_fmt_jof_audit_record_t  records[2],
                           uint32_t                    record_cap,
                           uint8_t                     tile[2],
                           uint32_t                    tile_cap,
                           ra8_fmt_jof_audit_result_t* result)
{
  test_store_t                  store = {.bytes = atlas, .len = 68U, .fail = false};
  ra8_fmt_jof_audit_workspace_t ws    = {.records     = records,
                                         .record_cap  = record_cap,
                                         .tile        = tile,
                                         .tile_cap    = tile_cap,
                                         .scratch     = nullptr,
                                         .scratch_cap = 0U};
  return ra8_fmt_jof_audit(test_pread, &store, store.len, &ws, result);
}

int main(void)
{
  const uint8_t distinct_top[2]    = {1U, 2U};
  const uint8_t distinct_bottom[2] = {3U, 4U};
  uint8_t       atlas[68];
  make_atlas(atlas, distinct_top, distinct_bottom);
  test_store_t store = {.bytes = atlas, .len = sizeof(atlas), .fail = false};

  ra8_fmt_jof_audit_requirements_t need = {};
  CHECK(ra8_fmt_jof_audit_requirements(test_pread, &store, store.len, &need) == k_ra8_ok);
  CHECK(need.record_count == 2U);
  CHECK(need.tile_bytes == 2U);
  CHECK(need.scratch_bytes == 0U);

  ra8_fmt_jof_audit_record_t records[2] = {};
  uint8_t                    tile[2]    = {};
  ra8_fmt_jof_audit_result_t result     = {};
  CHECK(run_audit(atlas, records, 2U, tile, sizeof(tile), &result) == k_ra8_ok);
  CHECK(result.decoded_tiles == 2U);
  CHECK((result.coverage_errors == 0U) && (result.geometry_errors == 0U));
  CHECK(result.duplicate_candidates == 0U);
  CHECK((records[0].payload == 2U) && !records[0].uniform);

  (void)memset(records, 0xA5, sizeof(records));
  (void)memset(tile, 0x5A, sizeof(tile));
  CHECK(run_audit(atlas, records, 1U, tile, sizeof(tile), &result) == k_ra8_err_invalid_size);
  CHECK(records[0].offset == 0xA5A5A5A5U);
  CHECK(run_audit(atlas, records, 2U, tile, 1U, &result) == k_ra8_err_invalid_size);

  const uint8_t duplicate[2] = {9U, 10U};
  make_atlas(atlas, duplicate, duplicate);
  CHECK(run_audit(atlas, records, 2U, tile, sizeof(tile), &result) == k_ra8_ok);
  CHECK(result.duplicate_candidates == 1U);

  const uint8_t uniform[2] = {7U, 7U};
  make_atlas(atlas, uniform, uniform);
  CHECK(run_audit(atlas, records, 2U, tile, sizeof(tile), &result) == k_ra8_ok);
  CHECK(result.duplicate_candidates == 0U);

  make_atlas(atlas, distinct_top, distinct_bottom);
  wr32(&atlas[36], 34U);
  wr32(&atlas[44], 32U);
  CHECK(run_audit(atlas, records, 2U, tile, sizeof(tile), &result) == k_ra8_err_validation_failed);
  CHECK(result.coverage_errors >= 1U);

  store.fail = true;
  CHECK(ra8_fmt_jof_audit_requirements(test_pread, &store, store.len, &need) == k_ra8_fail);
  CHECK(ra8_fmt_jof_audit_requirements(nullptr, &store, store.len, &need) == k_ra8_err_null_ptr);

  if (s_failures != 0) {
    return 1;
  }
  (void)fprintf(stdout, "portable JOF audit: exact buffers, corruption, duplicates passed\n");
  return 0;
}
