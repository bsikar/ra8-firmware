/**
 * @file test_ra8_npu_loader.c
 * @brief Unit tests for ra8_npu_loader.c (`.npub` Vela blob -> ra8_npu_job_t)
 *
 * @details
 * Compiled with `-DRA8_DEVICE_RA8P1` (see tests/CMakeLists.txt) so the
 * device-gated `ra8_npu_loader.c` + `ra8_npu.c` bodies are live and the NPU
 * headers are includable. Two halves:
 *
 *  1. The COMMITTED GOLDEN: includes the generated
 *     `tools/vela/generated/ra8_npu_model_addk_fake.h` (produced by
 *     `tools/vela/vela_gen.py` from `tools/vela/models/npu_addk_fake.json`),
 *     loads it through `ra8_npu_load()`, byte-pins the extracted command stream
 *     and the resolved region bases, then drives the full submit -> run ->
 *     read-output path against a host mirror of the ra8_emulator NPU model and
 *     asserts the output arena holds `input + K` byte-for-byte. This is the
 *     regenerate-and-diff golden the issue asks the host test to pin.
 *
 *  2. NEGATIVE PATHS: a small in-C blob builder (mirroring `ra8_npu_blob.h`)
 *     crafts malformed containers -- bad magic / version, oversized declared
 *     length, too many regions, empty / out-of-range command stream, region
 *     table past the end, a corrupted checksum, a baked region pointing outside
 *     the blob, and a runtime arena too small -- to prove every loader rejection.
 *
 * The NPU register window (`0x40140000`) sits inside the host MMIO backing
 * store, so `ra8_npu_submit()` writes land in RAM and the mirror reads QBASE /
 * BASEPn back exactly as the fake does.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_npu.h"
#include "ra8_npu_blob.h"
#include "ra8_npu_fake_cmd.h"
#include "ra8_npu_loader.h"
#include "ra8_npu_model_addk_fake.h"
#include "ra8_npu_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_blob_bad_t
 * @brief Header words written into an otherwise-valid NPU blob to invalidate it.
 *
 * @details
 * Each value targets one header field so the loader's guards can be driven
 * independently: a wrong magic, an unsupported version, a declared size past
 * the buffer, and a region count above the fixed maximum.
 */
typedef enum : uint32_t {
  k_t_bad_magic        = 0xDEADBEEFU, /**< A magic the loader must not accept.   */
  k_t_bad_version      = 99U,         /**< A blob version it does not implement. */
  k_t_bad_region_count = 9U,          /**< Regions above k_ra8_npu_region_count. */
  k_t_size_overshoot   = 1000U,       /**< Added to the real total so the declared
                                           size runs past the supplied buffer.  */
  k_t_arena_too_small  = 50U,         /**< Arena bytes below what the blob needs. */
} t_blob_bad_t;

/**
 * @enum ra8_loader_test_const_t
 * @brief Fixture sizes and expected constants for the loader tests.
 */
typedef enum : uint32_t {
  k_lt_arena_bytes   = 256U,  /**< Runtime arena length (bytes).          */
  k_lt_scratch_bytes = 512U,  /**< Scratch buffer for hand-built blobs.   */
  k_lt_addr_hi_shift = 32U,   /**< uint64 address -> high 32 bits.        */
  k_lt_addk          = 17U,   /**< Add-constant the golden model applies. */
  k_lt_out_bytes     = 64U,   /**< Golden model output tensor length.     */
  k_lt_byte_mask     = 0xFFU, /**< 8-bit element wrap (matches the op).   */
  k_lt_word_bytes    = 4U,    /**< Bytes in one little-endian word.       */
} ra8_loader_test_const_t;

/** @brief Runtime arena the loader carves output activations from. */
static uint8_t s_arena[k_lt_arena_bytes];
/** @brief Scratch buffer for hand-built negative-case blobs. */
static uint8_t s_scratch[k_lt_scratch_bytes];

/**
 * @brief Reset host MMIO and bring the NPU up cleanly (deinit any prior state).
 */
static void lt_prep(void)
{
  (void)ra8_npu_deinit();
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_init());
}

/** @brief Make a fresh arena descriptor over @p bytes of s_arena. */
static ra8_npu_arena_t lt_arena(uint32_t bytes)
{
  return (ra8_npu_arena_t){.base = s_arena, .bytes = bytes};
}

/** @brief Store one little-endian 32-bit word into a byte buffer. */
static void lt_put_word(uint8_t* buf, uint32_t off, uint32_t val)
{
  for (uint32_t i = 0U; i < (uint32_t)k_lt_word_bytes; i++) {
    buf[off + i] = (uint8_t)((val >> (i * 8U)) & (uint32_t)k_lt_byte_mask);
  }
}

/** @brief FNV-1a digest over buf[start, end) (matches ra8_npu_blob.h). */
static uint32_t lt_fnv(const uint8_t* buf, uint32_t start, uint32_t end)
{
  uint32_t digest = (uint32_t)k_ra8_npu_blob_fnv_offset;
  for (uint32_t i = start; i < end; i++) {
    digest = (digest ^ (uint32_t)buf[i]) * (uint32_t)k_ra8_npu_blob_fnv_prime;
  }
  return digest;
}

/** @brief One region spec for the in-C blob builder. */
typedef struct {
  uint32_t role;  /**< ra8_npu_blob_role_t value.       */
  bool     baked; /**< True: bytes baked into the blob. */
  uint32_t size;  /**< Region size in bytes.            */
} lt_region_t;

/**
 * @brief Assemble a valid `.npub` blob into @p buf (mirrors vela_gen.py).
 * @param[out] buf   Destination buffer (>= the assembled length).
 * @param[in]  regs  Region specs in descriptor order.
 * @param[in]  nreg  Number of regions.
 * @return The assembled blob length in bytes.
 */
static uint32_t lt_build(uint8_t* buf, const lt_region_t* regs, uint32_t nreg)
{
  const uint32_t hdr    = (uint32_t)k_ra8_npu_blob_header_bytes;
  const uint32_t rdb    = (uint32_t)k_ra8_npu_blob_region_desc_bytes;
  const uint32_t cmd_n  = (uint32_t)k_ra8_npu_fake_word_num;
  const uint32_t cmd_b  = cmd_n * (uint32_t)k_lt_word_bytes;
  const uint32_t coff   = hdr + (nreg * rdb);
  uint32_t       cursor = coff + cmd_b;

  /* SE55 add-constant command stream (contents valid but unused by these tests). */
  lt_put_word(buf,
              coff + ((uint32_t)k_ra8_npu_fake_word_op * (uint32_t)k_lt_word_bytes),
              (uint32_t)k_ra8_npu_fake_magic | (uint32_t)k_ra8_npu_fake_op_addk);
  lt_put_word(buf, coff + ((uint32_t)k_ra8_npu_fake_word_src * (uint32_t)k_lt_word_bytes), 0U);
  lt_put_word(buf, coff + ((uint32_t)k_ra8_npu_fake_word_dst * (uint32_t)k_lt_word_bytes), 0U);
  lt_put_word(buf, coff + ((uint32_t)k_ra8_npu_fake_word_count * (uint32_t)k_lt_word_bytes), 1U);
  lt_put_word(buf, coff + ((uint32_t)k_ra8_npu_fake_word_const * (uint32_t)k_lt_word_bytes), 0U);

  for (uint32_t r = 0U; r < nreg; r++) {
    const uint32_t desc  = hdr + (r * rdb);
    const uint32_t flags = regs[r].baked ? (uint32_t)k_ra8_npu_blob_rflag_baked : 0U;
    const uint32_t doff  = regs[r].baked ? cursor : 0U;
    lt_put_word(buf,
                desc + ((uint32_t)k_ra8_npu_blob_rdesc_role * (uint32_t)k_lt_word_bytes),
                regs[r].role);
    lt_put_word(buf,
                desc + ((uint32_t)k_ra8_npu_blob_rdesc_flags * (uint32_t)k_lt_word_bytes),
                flags);
    lt_put_word(buf,
                desc + ((uint32_t)k_ra8_npu_blob_rdesc_size * (uint32_t)k_lt_word_bytes),
                regs[r].size);
    lt_put_word(buf,
                desc + ((uint32_t)k_ra8_npu_blob_rdesc_data_offset * (uint32_t)k_lt_word_bytes),
                doff);
    if (regs[r].baked) {
      for (uint32_t i = 0U; i < regs[r].size; i++) {
        buf[cursor + i] = (uint8_t)(i & (uint32_t)k_lt_byte_mask);
      }
      cursor += regs[r].size;
    }
  }

  const uint32_t total = cursor;
  lt_put_word(buf,
              (uint32_t)k_ra8_npu_blob_word_magic * (uint32_t)k_lt_word_bytes,
              (uint32_t)k_ra8_npu_blob_magic);
  lt_put_word(buf,
              (uint32_t)k_ra8_npu_blob_word_version * (uint32_t)k_lt_word_bytes,
              (uint32_t)k_ra8_npu_blob_version);
  lt_put_word(buf, (uint32_t)k_ra8_npu_blob_word_total_bytes * (uint32_t)k_lt_word_bytes, total);
  lt_put_word(buf, (uint32_t)k_ra8_npu_blob_word_region_count * (uint32_t)k_lt_word_bytes, nreg);
  lt_put_word(buf, (uint32_t)k_ra8_npu_blob_word_cmd_offset * (uint32_t)k_lt_word_bytes, coff);
  lt_put_word(buf, (uint32_t)k_ra8_npu_blob_word_cmd_bytes * (uint32_t)k_lt_word_bytes, cmd_b);
  lt_put_word(buf,
              (uint32_t)k_ra8_npu_blob_word_accel * (uint32_t)k_lt_word_bytes,
              (uint32_t)k_ra8_npu_blob_accel_ethos_u55_256);
  lt_put_word(buf,
              (uint32_t)k_ra8_npu_blob_word_checksum * (uint32_t)k_lt_word_bytes,
              lt_fnv(buf, (uint32_t)k_ra8_npu_blob_header_bytes, total));
  return total;
}

/** @brief Recompute + store the payload checksum after a mutation. */
static void lt_patch_checksum(uint8_t* buf, uint32_t total)
{
  lt_put_word(buf,
              (uint32_t)k_ra8_npu_blob_word_checksum * (uint32_t)k_lt_word_bytes,
              lt_fnv(buf, (uint32_t)k_ra8_npu_blob_header_bytes, total));
}

/** @brief Read a BASEPn register back as a host arena pointer. */
static uint8_t* lt_region_ptr(ra8_npu_region_idx_t idx)
{
  const uint32_t lo_off =
    (uint32_t)k_ra8_npu_off_basep0_lo + ((uint32_t)idx * (uint32_t)k_ra8_npu_basep_stride_bytes);
  const uint32_t hi_off = lo_off + (uint32_t)k_ra8_npu_reg_hi_offset;
  const uint64_t base   = (uint64_t)*ra8_npu_reg(lo_off) |
                          ((uint64_t)*ra8_npu_reg(hi_off) << (uint32_t)k_lt_addr_hi_shift);
  return (uint8_t*)(uintptr_t)base;
}

/** @brief Mirror the ra8_emulator NPU model: decode QBASE, apply the op, latch STATUS. */
static void lt_exec(void)
{
  const uint64_t qbase =
    (uint64_t)*ra8_npu_reg(k_ra8_npu_off_qbase_lo) |
    ((uint64_t)*ra8_npu_reg(k_ra8_npu_off_qbase_hi) << (uint32_t)k_lt_addr_hi_shift);
  const uint32_t* w = (const uint32_t*)(uintptr_t)qbase;
  if ((w[k_ra8_npu_fake_word_op] & (uint32_t)k_ra8_npu_fake_magic_mask) !=
      (uint32_t)k_ra8_npu_fake_magic) {
    *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_parse_bit);
    return;
  }
  const uint32_t src   = w[k_ra8_npu_fake_word_src];
  const uint32_t dst   = w[k_ra8_npu_fake_word_dst];
  const uint32_t count = w[k_ra8_npu_fake_word_count];
  const uint32_t konst = w[k_ra8_npu_fake_word_const];
  const uint8_t* s     = lt_region_ptr((ra8_npu_region_idx_t)src);
  uint8_t*       d     = lt_region_ptr((ra8_npu_region_idx_t)dst);
  for (uint32_t i = 0U; i < count; i++) {
    d[i] = (uint8_t)((s[i] + konst) & (uint32_t)k_ra8_npu_fake_byte_mask);
  }
  *ra8_npu_reg(k_ra8_npu_off_status) = ((uint32_t)1U << k_ra8_npu_status_cmd_end_bit) |
                                       ((uint32_t)1U << k_ra8_npu_status_irq_raised_bit);
}

/** @brief Read region @p r 's descriptor field @p field from the golden blob. */
static uint32_t lt_golden_rfield(uint32_t r, ra8_npu_blob_rdesc_t field)
{
  const uint32_t desc =
    (uint32_t)k_ra8_npu_blob_header_bytes + (r * (uint32_t)k_ra8_npu_blob_region_desc_bytes);
  return ra8_npu_blob_read_word(ra8_npu_model_addk_fake_blob(),
                                desc + ((uint32_t)field * (uint32_t)k_lt_word_bytes));
}

/**
 * @brief Byte-pin the extracted SE55 add-constant command stream.
 * @param[in] job The loaded NPU job whose cmd_stream is inspected.
 * @pre @p job was produced by ra8_npu_load over the golden blob.
 * @post Every command-stream word matched its expected value.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void lt_check_cmd_stream(const ra8_npu_job_t* job)
{
  const uint8_t* cs      = (const uint8_t*)job->cmd_stream;
  const uint32_t exp_op  = (uint32_t)k_ra8_npu_fake_magic | (uint32_t)k_ra8_npu_fake_op_addk;
  const uint32_t exp_src = (uint32_t)k_ra8_npu_region_1;
  const uint32_t exp_dst = (uint32_t)k_ra8_npu_region_2;
  const uint32_t exp_cnt = (uint32_t)k_lt_out_bytes;
  const uint32_t exp_k   = (uint32_t)k_lt_addk;
  TEST_ASSERT_EQ(
    exp_op,
    ra8_npu_blob_read_word(cs, (uint32_t)k_ra8_npu_fake_word_op * (uint32_t)k_lt_word_bytes));
  TEST_ASSERT_EQ(
    exp_src,
    ra8_npu_blob_read_word(cs, (uint32_t)k_ra8_npu_fake_word_src * (uint32_t)k_lt_word_bytes));
  TEST_ASSERT_EQ(
    exp_dst,
    ra8_npu_blob_read_word(cs, (uint32_t)k_ra8_npu_fake_word_dst * (uint32_t)k_lt_word_bytes));
  TEST_ASSERT_EQ(
    exp_cnt,
    ra8_npu_blob_read_word(cs, (uint32_t)k_ra8_npu_fake_word_count * (uint32_t)k_lt_word_bytes));
  TEST_ASSERT_EQ(
    exp_k,
    ra8_npu_blob_read_word(cs, (uint32_t)k_ra8_npu_fake_word_const * (uint32_t)k_lt_word_bytes));
}

/**
 * @brief Assert the output arena holds input + K byte-for-byte.
 * @param[in] blob  The golden model blob.
 * @param[in] i_off Byte offset of the baked input region within @p blob.
 * @pre The job has run and mirrored its output into s_arena.
 * @post Every output byte equals (input + K) & 0xFF.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void lt_check_output(const uint8_t* blob, uint32_t i_off)
{
  const uint8_t* input = blob + i_off;
  for (uint32_t i = 0U; i < (uint32_t)k_lt_out_bytes; i++) {
    const uint8_t expected = (uint8_t)((input[i] + (uint32_t)k_lt_addk) & (uint32_t)k_lt_byte_mask);
    TEST_ASSERT_EQ(expected, s_arena[i]);
  }
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- ra8_npu_load's guards are all single
 * condition; the golden's command stream and region layout are byte-pinned and
 * then run end-to-end through the single-condition submit / run / poll paths)
 */
static void test_load_golden_maps_job(void)
{
  TEST_BEGIN("loader maps the golden blob into a runnable job");
  lt_prep();

  const uint8_t*  blob  = ra8_npu_model_addk_fake_blob();
  const uint32_t  bytes = ra8_npu_model_addk_fake_bytes();
  ra8_npu_arena_t arena = lt_arena((uint32_t)sizeof(s_arena));
  ra8_npu_job_t   job   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_load(blob, bytes, &arena, &job));

  /* Command stream: the loader points it at blob + cmd_offset, length cmd_bytes. */
  const uint32_t coff =
    ra8_npu_blob_read_word(blob,
                           (uint32_t)k_ra8_npu_blob_word_cmd_offset * (uint32_t)k_lt_word_bytes);
  const uint32_t cbytes =
    ra8_npu_blob_read_word(blob,
                           (uint32_t)k_ra8_npu_blob_word_cmd_bytes * (uint32_t)k_lt_word_bytes);
  const uint64_t exp_cmd = (uint64_t)(uintptr_t)(blob + coff);
  const uint64_t got_cmd = (uint64_t)(uintptr_t)job.cmd_stream;
  TEST_ASSERT_EQ(exp_cmd, got_cmd);
  TEST_ASSERT_EQ(cbytes, job.cmd_stream_bytes);

  /* Byte-pin the extracted SE55 add-constant command stream. */
  lt_check_cmd_stream(&job);

  /* Regions: baked weights/input resolve into the blob; output into the arena. */
  TEST_ASSERT_EQ(3U, job.region_count);
  const uint32_t w_off  = lt_golden_rfield(0U, k_ra8_npu_blob_rdesc_data_offset);
  const uint32_t i_off  = lt_golden_rfield(1U, k_ra8_npu_blob_rdesc_data_offset);
  const uint64_t exp_r0 = (uint64_t)(uintptr_t)(blob + w_off);
  const uint64_t exp_r1 = (uint64_t)(uintptr_t)(blob + i_off);
  const uint64_t exp_r2 = (uint64_t)(uintptr_t)s_arena;
  TEST_ASSERT_EQ(exp_r0, job.region_base[k_ra8_npu_region_0]);
  TEST_ASSERT_EQ(exp_r1, job.region_base[k_ra8_npu_region_1]);
  TEST_ASSERT_EQ(exp_r2, job.region_base[k_ra8_npu_region_2]);

  /* Run it: submit -> run -> emulator-mirror exec -> poll -> read output. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_submit(&job));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_run());
  lt_exec();
  bool done = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_poll(&done));
  TEST_ASSERT(done);

  /* Output arena holds input + K byte-for-byte (baked input read from the blob). */
  lt_check_output(blob, i_off);
  TEST_END("loader maps the golden blob into a runnable job");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- each null argument trips a separate
 * single-condition RA8_CHECK_NULL_PTR guard, exercised one at a time)
 */
static void test_load_rejects_null(void)
{
  TEST_BEGIN("loader rejects null arguments");
  const uint8_t*  blob  = ra8_npu_model_addk_fake_blob();
  const uint32_t  bytes = ra8_npu_model_addk_fake_bytes();
  ra8_npu_arena_t arena = lt_arena((uint32_t)sizeof(s_arena));
  ra8_npu_job_t   job   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_npu_load(nullptr, bytes, &arena, &job));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_npu_load(blob, bytes, nullptr, &job));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_npu_load(blob, bytes, &arena, nullptr));
  TEST_END("loader rejects null arguments");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- every malformed-container rejection is
 * a separate single-condition header guard in the loader, tripped one per case)
 */
static void test_load_rejects_bad_container(void)
{
  TEST_BEGIN("loader rejects malformed containers");
  ra8_npu_arena_t   arena  = lt_arena((uint32_t)sizeof(s_arena));
  ra8_npu_job_t     job    = {};
  const lt_region_t regs[] = {
    {.role = (uint32_t)k_ra8_npu_blob_role_weights, .baked = true, .size = 32U},
  };
  const uint32_t total = lt_build(s_scratch, regs, 1U);

  /* Buffer shorter than the fixed header. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_npu_load(s_scratch, 4U, &arena, &job));

  /* Bad magic. */
  lt_put_word(s_scratch,
              (uint32_t)k_ra8_npu_blob_word_magic * (uint32_t)k_lt_word_bytes,
              k_t_bad_magic);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_npu_load(s_scratch, total, &arena, &job));

  /* Unsupported version. */
  (void)lt_build(s_scratch, regs, 1U);
  lt_put_word(s_scratch,
              (uint32_t)k_ra8_npu_blob_word_version * (uint32_t)k_lt_word_bytes,
              k_t_bad_version);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_npu_load(s_scratch, total, &arena, &job));

  /* Declared total larger than the buffer. */
  (void)lt_build(s_scratch, regs, 1U);
  lt_put_word(s_scratch,
              (uint32_t)k_ra8_npu_blob_word_total_bytes * (uint32_t)k_lt_word_bytes,
              total + k_t_size_overshoot);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_npu_load(s_scratch, total, &arena, &job));

  /* Region count above k_ra8_npu_region_count. */
  (void)lt_build(s_scratch, regs, 1U);
  lt_put_word(s_scratch,
              (uint32_t)k_ra8_npu_blob_word_region_count * (uint32_t)k_lt_word_bytes,
              k_t_bad_region_count);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_npu_load(s_scratch, total, &arena, &job));

  /* Empty command stream. */
  (void)lt_build(s_scratch, regs, 1U);
  lt_put_word(s_scratch, (uint32_t)k_ra8_npu_blob_word_cmd_bytes * (uint32_t)k_lt_word_bytes, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_npu_load(s_scratch, total, &arena, &job));

  /* Command stream offset past the blob. */
  (void)lt_build(s_scratch, regs, 1U);
  lt_put_word(s_scratch,
              (uint32_t)k_ra8_npu_blob_word_cmd_offset * (uint32_t)k_lt_word_bytes,
              total);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_npu_load(s_scratch, total, &arena, &job));

  /* Region table declared past the blob: the max region count (8) needs a
   * 128-byte table, which overruns this small one-region blob. */
  (void)lt_build(s_scratch, regs, 1U);
  lt_put_word(s_scratch,
              (uint32_t)k_ra8_npu_blob_word_region_count * (uint32_t)k_lt_word_bytes,
              (uint32_t)k_ra8_npu_region_count);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_npu_load(s_scratch, total, &arena, &job));
  TEST_END("loader rejects malformed containers");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the loader's checksum test is a single
 * `digest != stored` comparison; a flipped payload byte trips it)
 */
static void test_load_rejects_checksum(void)
{
  TEST_BEGIN("loader rejects a corrupted payload");
  ra8_npu_arena_t   arena  = lt_arena((uint32_t)sizeof(s_arena));
  ra8_npu_job_t     job    = {};
  const lt_region_t regs[] = {
    {.role = (uint32_t)k_ra8_npu_blob_role_weights, .baked = true, .size = 32U},
  };
  const uint32_t total = lt_build(s_scratch, regs, 1U);
  s_scratch[total - 1U] =
    (uint8_t)(s_scratch[total - 1U] ^ (uint32_t)k_lt_byte_mask); /* no re-checksum */
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch, ra8_npu_load(s_scratch, total, &arena, &job));
  TEST_END("loader rejects a corrupted payload");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the baked-region bound is one
 * internal_npu_span_ok() call; an out-of-range data_offset trips it)
 */
static void test_load_rejects_baked_oob(void)
{
  TEST_BEGIN("loader rejects a baked region outside the blob");
  ra8_npu_arena_t   arena  = lt_arena((uint32_t)sizeof(s_arena));
  ra8_npu_job_t     job    = {};
  const lt_region_t regs[] = {
    {.role = (uint32_t)k_ra8_npu_blob_role_weights, .baked = true, .size = 32U},
  };
  const uint32_t total = lt_build(s_scratch, regs, 1U);
  /* Point region 0's baked data past the end, then fix the checksum so the
   * loader reaches region placement rather than stopping at the checksum. */
  lt_put_word(s_scratch,
              (uint32_t)k_ra8_npu_blob_header_bytes +
                ((uint32_t)k_ra8_npu_blob_rdesc_data_offset * (uint32_t)k_lt_word_bytes),
              total);
  lt_patch_checksum(s_scratch, total);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_npu_load(s_scratch, total, &arena, &job));
  TEST_END("loader rejects a baked region outside the blob");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- the two runtime-fit guards are separate
 * single conditions; one case trips the aligned-overflow guard, one the
 * remaining-bytes guard)
 */
static void test_load_runtime_arena_limits(void)
{
  TEST_BEGIN("loader rejects when runtime regions do not fit");
  ra8_npu_job_t job = {};

  /* One 64-byte runtime region, arena of 0 bytes -> remaining-bytes guard. */
  const lt_region_t one[] = {
    {.role = (uint32_t)k_ra8_npu_blob_role_output, .baked = false, .size = 64U},
  };
  const uint32_t  t1    = lt_build(s_scratch, one, 1U);
  ra8_npu_arena_t empty = lt_arena(0U);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_npu_load(s_scratch, t1, &empty, &job));

  /* Two 50-byte runtime regions, arena of 50 bytes -> the second region's
   * 16-byte-aligned base (64) overflows the arena (aligned-overflow guard). */
  const lt_region_t two[] = {
    {.role = (uint32_t)k_ra8_npu_blob_role_scratch, .baked = false, .size = 50U},
    {.role = (uint32_t)k_ra8_npu_blob_role_output, .baked = false, .size = 50U},
  };
  const uint32_t  t2    = lt_build(s_scratch, two, 2U);
  ra8_npu_arena_t small = lt_arena(k_t_arena_too_small);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_npu_load(s_scratch, t2, &small, &job));
  TEST_END("loader rejects when runtime regions do not fit");
}

int32_t main(void)
{
  test_load_golden_maps_job();
  test_load_rejects_null();
  test_load_rejects_bad_container();
  test_load_rejects_checksum();
  test_load_rejects_baked_oob();
  test_load_runtime_arena_limits();
  return 0;
}
