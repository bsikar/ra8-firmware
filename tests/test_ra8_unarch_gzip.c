/**
 * @file test_ra8_unarch_gzip.c
 * @brief Tests for the clean-room gzip member decoder (RFC 1952 over tinfl).
 *
 * @details
 * Builds real gzip members in memory (miniz `tdefl` raw-DEFLATE bodies
 * wrapped in hand-assembled RFC 1952 headers/trailers, exercising every
 * optional header field) and decodes them through
 * `ra8_unarch_gzip_unwrap`, proving:
 *
 *   1. honest members decode byte-exactly, with and without the optional
 *      FEXTRA / FNAME / FCOMMENT / FHCRC fields (and an empty payload),
 *   2. every hostile shape is rejected fail-closed with the specific
 *      error: wrong magic / method / reserved flags, corrupt header CRC,
 *      unterminated and over-long strings, truncated or corrupt DEFLATE,
 *      lying trailers (CRC32, ISIZE), truncated trailers, and trailing
 *      bytes after the member,
 *   3. the policy axes each fire through the decoder (ratio, output cap,
 *      iteration budget) and an undersized arena maps to no_mem.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_unarch_gzip.h"
#include "ra8_unarch_io.h"
#include "unity_minimal.h"

/** @brief gzip member layout used by the fixture builder. */
typedef enum : uint8_t {
  k_tg_off_extra  = 10U, /**< First byte after the 10-byte fixed header. */
  k_tg_len_fname  = 9U,  /**< "name.tar" plus its NUL terminator.        */
  k_tg_len_unterm = 6U,  /**< Unterminated FNAME bytes written.          */
} tg_layout_t;

/**
 * @enum t_gz_hdr_t
 * @brief gzip member-header bytes the builder writes and the arms corrupt.
 *
 * @details
 * A gzip member opens with the two-byte magic 0x1F 0x8B, then the compression
 * method and a flag byte. The fixed header is 10 bytes, so offset 10 is the
 * first byte of whatever the flags selected -- the FHCRC, the FEXTRA length,
 * or the DEFLATE stream itself.
 */
typedef enum : uint8_t {
  k_t_magic_b0      = 0x1FU, /**< gzip magic byte 0. */
  k_t_magic_b1      = 0x8BU, /**< gzip magic byte 1. */
  k_t_magic_wrong   = 0x50U, /**< A wrong first byte, ASCII 'P'; must be
                                 refused as not-a-gzip.                       */
  k_t_method_wrong  = 9U,    /**< A compression method that is not DEFLATE;
                                 also the FNAME field width including its NUL. */
  k_t_hdr_fixed     = 10U,   /**< Fixed header length, hence the offset of the
                                 first flag-selected byte.                     */
  k_t_hdr_fixed_p1  = 11U,   /**< The byte after it: the FEXTRA length high byte. */
  k_t_byte_mask     = 0xFFU, /**< Low-byte mask while serialising a field, and
                                 the XOR that corrupts one.                    */
  k_t_trailing_junk = 0xA5U, /**< Byte appended past the member end. */
} t_gz_hdr_t;

/**
 * @enum t_gz_size_t
 * @brief Payload sizes and decompression caps the arms exercise.
 */
typedef enum : uint16_t {
  k_t_payload_small = 64U,   /**< Payload for the header arms, bytes.       */
  k_t_payload_mid   = 256U,  /**< Payload for the body/trailer arms, bytes. */
  k_t_output_cap    = 1024U, /**< Absolute output cap the ratio arm sets.   */
} t_gz_size_t;

/**
 * @enum tg_dim_t
 * @brief Payload / buffer budgets (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_tg_member_cap = 96U * 1024U,  /**< Built gzip member buffer.      */
  k_tg_arena      = 128U * 1024U, /**< Decode destination arena.      */
  k_tg_payload    = 8U * 1024U,   /**< Incompressible payload length. */
  k_tg_zeros      = 64U * 1024U,  /**< Highly compressible payload.   */
  k_tg_longname   = 3000U,        /**< Over-cap FNAME length.         */
  k_tg_lcg_seed   = 0xC0FFEE01U,  /**< Payload LCG seed.              */
  k_tg_lcg_mul    = 1103515245U,  /**< Payload LCG multiplier.        */
  k_tg_lcg_add    = 12345U,       /**< Payload LCG increment.         */
} tg_dim_t;

/** @brief Built gzip member bytes. */
static uint8_t s_member[k_tg_member_cap];
/** @brief Decode destination arena. */
static uint8_t s_arena[k_tg_arena];
/** @brief Source payload scratch. */
static uint8_t s_payload[k_tg_zeros];

/** @brief Fill @p n pseudo-random (incompressible) payload bytes. */
static void tg_fill(uint8_t* dst, size_t n)
{
  uint32_t s = (uint32_t)k_tg_lcg_seed;
  for (size_t i = 0U; i < n; ++i) {
    s      = ((uint32_t)k_tg_lcg_mul * s) + (uint32_t)k_tg_lcg_add;
    dst[i] = (uint8_t)((s >> 16U) & k_t_byte_mask);
  }
}

/**
 * @brief Assemble one gzip member around a payload.
 * @details Header flags selected by @p flg; FEXTRA/FNAME/FCOMMENT get
 *          small fixed contents; FHCRC is computed correctly. The body is
 *          miniz raw DEFLATE, the trailer CRC32 + ISIZE of the payload.
 * @return The member length in bytes (0 on build failure).
 */
static size_t tg_build(const uint8_t* payload, size_t n, uint8_t flg)
{
  size_t off      = 0U;
  s_member[off++] = k_t_magic_b0;
  s_member[off++] = k_t_magic_b1;
  s_member[off++] = 8U; /* CM: DEFLATE */
  s_member[off++] = flg;
  memset(&s_member[off], 0, 6U); /* MTIME, XFL, OS */
  off += 6U;
  if ((flg & 0x04U) != 0U) { /* FEXTRA */
    s_member[off++] = 4U;
    s_member[off++] = 0U;
    memcpy(&s_member[off], "EXTR", 4U);
    off += 4U;
  }
  if ((flg & 0x08U) != 0U) { /* FNAME */
    memcpy(&s_member[off], "name.tar", (size_t)k_tg_len_fname);
    off += k_t_method_wrong;
  }
  if ((flg & 0x10U) != 0U) { /* FCOMMENT */
    memcpy(&s_member[off], "comment", 8U);
    off += 8U;
  }
  if ((flg & 0x02U) != 0U) { /* FHCRC */
    const uint32_t hc = (uint32_t)mz_crc32(MZ_CRC32_INIT, s_member, off) & 0xFFFFU;
    s_member[off++]   = (uint8_t)(hc & k_t_byte_mask);
    s_member[off++]   = (uint8_t)((hc >> 8U) & k_t_byte_mask);
  }
  const size_t comp = tdefl_compress_mem_to_mem(&s_member[off],
                                                sizeof(s_member) - off - 8U,
                                                payload,
                                                n,
                                                (int)TDEFL_DEFAULT_MAX_PROBES);
  TEST_ASSERT(comp > 0U);
  off += comp;
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, payload, n);
  for (uint32_t i = 0U; i < 4U; ++i) {
    s_member[off + i]      = (uint8_t)((crc >> (8U * i)) & k_t_byte_mask);
    s_member[off + 4U + i] = (uint8_t)(((uint32_t)n >> (8U * i)) & k_t_byte_mask);
  }
  return off + 8U;
}

/** @brief Decode a flat member buffer with optional custom limits. */
static ra8_err_t
tg_unwrap(const uint8_t* buf, size_t len, const ra8_decomp_limits_t* lim, size_t* out_len)
{
  ra8_unarch_mem_t mem = {.base = buf, .len = len};
  return ra8_unarch_gzip_unwrap(ra8_unarch_mem_read,
                                &mem,
                                (uint64_t)len,
                                s_arena,
                                sizeof(s_arena),
                                lim,
                                out_len);
}

/**
 * @test test_gzip_magic_and_guards
 * @brief Magic probe + unwrap argument validation.
 *
 * @par MC/DC:
 * (no compound decisions under test -- probe and entry guards are
 * independent single-condition early returns.)
 */
static void test_gzip_magic_and_guards(void)
{
  TEST_BEGIN("gzip: magic probe + argument guards");
  const uint8_t sig[2] = {0x1FU, 0x8BU};
  TEST_ASSERT(!ra8_unarch_gzip_magic(nullptr, 2U));
  TEST_ASSERT(!ra8_unarch_gzip_magic(sig, 1U));
  TEST_ASSERT(ra8_unarch_gzip_magic(sig, 2U));
  const uint8_t bad1[2] = {0x1EU, 0x8BU};
  TEST_ASSERT(!ra8_unarch_gzip_magic(bad1, 2U));
  const uint8_t bad2[2] = {0x1FU, 0x8CU};
  TEST_ASSERT(!ra8_unarch_gzip_magic(bad2, 2U));

  size_t           got = 1U;
  ra8_unarch_mem_t mem = {.base = s_member, .len = 32U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_unarch_gzip_unwrap(nullptr, &mem, 32U, s_arena, 8U, nullptr, &got));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_unarch_gzip_unwrap(ra8_unarch_mem_read, &mem, 32U, nullptr, 8U, nullptr, &got));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_unarch_gzip_unwrap(ra8_unarch_mem_read, &mem, 32U, s_arena, 8U, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_unarch_gzip_unwrap(ra8_unarch_mem_read, &mem, 0U, s_arena, 8U, nullptr, &got));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_unarch_gzip_unwrap(ra8_unarch_mem_read, &mem, 32U, s_arena, 0U, nullptr, &got));
  ra8_decomp_limits_t bad = ra8_decomp_limits_default();
  bad.max_iterations      = 0U;
  const size_t len        = tg_build(s_payload, 16U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, tg_unwrap(s_member, len, &bad, &got));
  TEST_END("gzip: magic probe + argument guards");
}

/**
 * @test test_gzip_honest_members
 * @brief Members with every header-field combination decode byte-exactly.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each optional-field flag is an
 * independent single-condition branch, driven set and clear across these
 * vectors.)
 */
static void test_gzip_honest_members(void)
{
  TEST_BEGIN("gzip: honest members decode");
  tg_fill(s_payload, (size_t)k_tg_payload);
  size_t got = 0U;

  /* Plain member, multi-pass input (8 KiB incompressible > 512 window). */
  size_t len = tg_build(s_payload, (size_t)k_tg_payload, 0U);
  TEST_ASSERT_EQ(k_ra8_ok, tg_unwrap(s_member, len, nullptr, &got));
  TEST_ASSERT_EQ(k_tg_payload, got);
  TEST_ASSERT_EQ(0, memcmp(s_arena, s_payload, got));

  /* Every optional field at once: FTEXT+FHCRC+FEXTRA+FNAME+FCOMMENT. */
  len = tg_build(s_payload, (size_t)k_tg_payload, k_t_magic_b0);
  TEST_ASSERT_EQ(k_ra8_ok, tg_unwrap(s_member, len, nullptr, &got));
  TEST_ASSERT_EQ(k_tg_payload, got);
  TEST_ASSERT_EQ(0, memcmp(s_arena, s_payload, got));

  /* Empty payload: a valid gzip of a zero-byte file. */
  len = tg_build(s_payload, 0U, 0U);
  TEST_ASSERT_EQ(k_ra8_ok, tg_unwrap(s_member, len, nullptr, &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("gzip: honest members decode");
}

/**
 * @test test_gzip_hostile_headers
 * @brief Every malformed header shape is rejected with the specific error.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each rejection drives one
 * single-condition guard in the header parser.)
 */
static void test_gzip_hostile_headers(void)
{
  TEST_BEGIN("gzip: hostile headers rejected");
  size_t got = 1U;
  size_t len = tg_build(s_payload, k_t_payload_small, 0x02U); /* with FHCRC */

  /* Wrong magic / method / reserved bits. */
  uint8_t save = s_member[0];
  s_member[0]  = k_t_magic_wrong;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, tg_unwrap(s_member, len, nullptr, &got));
  TEST_ASSERT_EQ(0U, got);
  s_member[0] = save;
  save        = s_member[2];
  s_member[2] = k_t_method_wrong; /* not DEFLATE */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, tg_unwrap(s_member, len, nullptr, &got));
  s_member[2] = save;
  save        = s_member[3];
  s_member[3] |= 0x20U; /* reserved flag bit */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, tg_unwrap(s_member, len, nullptr, &got));
  s_member[3] = save;

  /* Corrupt FHCRC. */
  s_member[k_t_hdr_fixed] ^= k_t_byte_mask; /* first FHCRC byte (no optional fields before it) */
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch, tg_unwrap(s_member, len, nullptr, &got));
  s_member[k_t_hdr_fixed] ^= k_t_byte_mask;

  /* Header truncated inside the fixed fields. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, tg_unwrap(s_member, 6U, nullptr, &got));

  /* Unterminated FNAME: flag set, no NUL before EOF. */
  uint8_t tiny[16] = {k_t_magic_b0, k_t_magic_b1, 8U, 0x08U};
  memset(&tiny[k_tg_off_extra], (int)'n', (size_t)k_tg_len_unterm);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, tg_unwrap(tiny, sizeof(tiny), nullptr, &got));

  /* Over-long FNAME: longer than the fail-closed cap, NUL far away. */
  uint8_t* big = s_member;
  memset(big, 0, sizeof(s_member));
  big[0] = k_t_magic_b0;
  big[1] = k_t_magic_b1;
  big[2] = 8U;
  big[3] = 0x08U;
  memset(&big[k_tg_off_extra], (int)'n', (size_t)k_tg_longname);
  big[k_t_hdr_fixed + (size_t)k_tg_longname] = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 tg_unwrap(big, 10U + (size_t)k_tg_longname + 1U, nullptr, &got));

  /* FEXTRA whose declared length runs past the member. */
  uint8_t fx[16]       = {k_t_magic_b0, k_t_magic_b1, 8U, 0x04U};
  fx[k_t_hdr_fixed]    = k_t_byte_mask;
  fx[k_t_hdr_fixed_p1] = 0x00U; /* XLEN=255 with only 4 bytes left */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, tg_unwrap(fx, sizeof(fx), nullptr, &got));
  TEST_END("gzip: hostile headers rejected");
}

/**
 * @test test_gzip_hostile_body_and_trailer
 * @brief Corrupt / truncated bodies and lying trailers are rejected.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each rejection drives one
 * single-condition guard in the inflate loop or trailer verifier.)
 */
static void test_gzip_hostile_body_and_trailer(void)
{
  TEST_BEGIN("gzip: hostile bodies + trailers rejected");
  tg_fill(s_payload, k_t_payload_mid);
  size_t got = 1U;
  size_t len = tg_build(s_payload, k_t_payload_mid, 0U);

  /* Corrupt DEFLATE: reserved block type at the stream start. */
  const uint8_t save      = s_member[10];
  s_member[k_t_hdr_fixed] = 0x06U; /* BTYPE=11 (reserved) */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, tg_unwrap(s_member, len, nullptr, &got));
  TEST_ASSERT_EQ(0U, got);
  s_member[k_t_hdr_fixed] = save;

  /* Truncated DEFLATE: cut mid-body (before the trailer). */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, tg_unwrap(s_member, len - 20U, nullptr, &got));

  /* Truncated trailer: member ends inside the 8 trailer bytes. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, tg_unwrap(s_member, len - 4U, nullptr, &got));

  /* Lying trailer CRC32. */
  s_member[len - 8U] ^= k_t_byte_mask;
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch, tg_unwrap(s_member, len, nullptr, &got));
  s_member[len - 8U] ^= k_t_byte_mask;

  /* Lying trailer ISIZE. */
  s_member[len - 1U] ^= k_t_byte_mask;
  TEST_ASSERT_EQ(k_ra8_err_checksum_mismatch, tg_unwrap(s_member, len, nullptr, &got));
  s_member[len - 1U] ^= k_t_byte_mask;

  /* Trailing bytes after the member: no concatenation. */
  s_member[len] = k_t_trailing_junk;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, tg_unwrap(s_member, len + 1U, nullptr, &got));

  /* Unmodified member still decodes (the mutations were reverted). */
  TEST_ASSERT_EQ(k_ra8_ok, tg_unwrap(s_member, len, nullptr, &got));
  TEST_ASSERT_EQ(256U, got);
  TEST_END("gzip: hostile bodies + trailers rejected");
}

/**
 * @test test_gzip_policy_bounds
 * @brief Ratio, output cap, iteration budget, and arena exhaustion fire.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each breach drives one
 * single-condition budget charge or the arena-full guard.)
 */
static void test_gzip_policy_bounds(void)
{
  TEST_BEGIN("gzip: policy bounds fire");
  size_t got = 1U;

  /* Ratio bomb: 64 KiB of zeros in a few hundred bytes, tightened ratio. */
  memset(s_payload, 0, (size_t)k_tg_zeros);
  size_t              len = tg_build(s_payload, (size_t)k_tg_zeros, 0U);
  ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  lim.max_ratio           = 2U;
  lim.ratio_grace_bytes   = 16U;
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, tg_unwrap(s_member, len, &lim, &got));
  TEST_ASSERT_EQ(0U, got);

  /* Output cap: the same member against a small absolute cap. */
  lim                  = ra8_decomp_limits_default();
  lim.max_output_bytes = k_t_output_cap;
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, tg_unwrap(s_member, len, &lim, &got));

  /* Iteration budget: an incompressible body needs many input refills. */
  tg_fill(s_payload, (size_t)k_tg_payload);
  len                = tg_build(s_payload, (size_t)k_tg_payload, 0U);
  lim                = ra8_decomp_limits_default();
  lim.max_iterations = 2U;
  TEST_ASSERT_EQ(k_ra8_err_decomp_iterations, tg_unwrap(s_member, len, &lim, &got));

  /* Arena smaller than the payload: no_mem, not a crash. */
  ra8_unarch_mem_t mem = {.base = s_member, .len = len};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_unarch_gzip_unwrap(ra8_unarch_mem_read,
                                        &mem,
                                        (uint64_t)len,
                                        s_arena,
                                        1024U,
                                        nullptr,
                                        &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("gzip: policy bounds fire");
}

/**
 * @brief Test entry point -- runs the gzip decoder suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int32_t main(void)
{
  test_gzip_magic_and_guards();
  test_gzip_honest_members();
  test_gzip_hostile_headers();
  test_gzip_hostile_body_and_trailer();
  test_gzip_policy_bounds();
  (void)fprintf(stderr, "[OK  ] test_ra8_unarch_gzip.c\n");
  return 0;
}
