/**
 * @file test_ra8_dfu_program.c
 * @brief Host unit tests for the MRAM program/verify half of ra8_dfu.
 *
 * @details
 * Drives the full program -> read-back -> verify round-trip against the
 * simulator-backed code-MRAM window (the `RA8_SIMULATOR_MODE` mock maps
 * `0x02000000`..`0x02100000` to host memory, and `ra8_flash` writes/erases land
 * there). Covers slot addressing, header construction, CRC read-back, and the
 * argument guards. Every compound boolean decision carries its `@par MC/DC:`
 * block.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_dfu.h"
#include "ra8_dfu_internal.h"
#include "ra8_err.h"
#include "ra8_flash_regs.h"
#include "unity_minimal.h"

/**
 * @enum dfu_program_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_dfu_pattern_stride =
    7U, /**< Stride of the image generator, `i * 7 + 3`, so no two bytes of the image agree by position. */
  k_dfu_image_bytes = 64, /**< Size of the fixture image. */
  k_dfu_flip_mask =
    0xFFU, /**< XORed into the first byte to corrupt the image, so the verify step must reject it. */
} dfu_program_uint8_const_t;

/** @brief Test image geometry. */
typedef enum : uint32_t {
  k_test_img_len = 0x00000100U, /**< 256-byte image (8 DFU blocks of 32).   */
  k_test_seq     = 0x00000005U, /**< Sequence number stamped in the header. */
  k_test_off_bad = 0x00000001U, /**< A non-32-aligned offset.               */
} test_prog_const_t;

/** @brief Fill `buf` with a deterministic byte pattern. */
static void fill_pattern(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)((i * k_dfu_pattern_stride) + 3U);
  }
}

/**
 * @brief Mark the simulated MRAM controller "program-ready" (MRCPS.ABUFEMP).
 *
 * @details The host simulator backs MRCPS with plain memory and has no real
 * controller to drive the program handshake, so ra8_flash's commit-wait would
 * spin to a timeout. Priming ABUFEMP (and leaving PRGBSYC/ABUFFULL clear) lets
 * the program path complete -- the same accommodation test_ra8_flash.c uses.
 * `ra8_flash_open` clears MRCPS as part of bring-up, so this must run AFTER
 * ::ra8_dfu_program_prepare and before any program/commit.
 */
static void sim_mark_program_ready(void)
{
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mrcps) = (uint8_t)k_ra8_mrcps_mask_abufemp;
}

/**
 * @brief slot_base / other_slot addressing is correct.
 *
 * @par MC/DC: not applicable -- table lookups, covered by enumeration.
 * @pre None. @pre None. @post No side effects. @post No global state changes.
 * @note Test-only. @since 0.1.0
 */
static void test_slot_addressing(void)
{
  TEST_BEGIN("ra8_dfu: slot addressing");
  TEST_ASSERT_EQ((uintptr_t)k_ra8_dfu_slot_a_base, ra8_dfu_slot_base(k_ra8_dfu_slot_a));
  TEST_ASSERT_EQ((uintptr_t)k_ra8_dfu_slot_b_base, ra8_dfu_slot_base(k_ra8_dfu_slot_b));
  TEST_ASSERT_EQ((uintptr_t)0U, ra8_dfu_slot_base(k_ra8_dfu_slot_none));
  TEST_ASSERT_EQ(k_ra8_dfu_slot_b, ra8_dfu_other_slot(k_ra8_dfu_slot_a));
  TEST_ASSERT_EQ(k_ra8_dfu_slot_a, ra8_dfu_other_slot(k_ra8_dfu_slot_b));
  TEST_END("ra8_dfu: slot addressing");
}

/**
 * @brief Full program -> verify round-trip, then a corruption fails verify.
 *
 * @par MC/DC: not applicable here -- this case is the happy-path data round
 *      trip; the decision guards are exercised in test_program_guards_mcdc.
 * @pre RA8_SIMULATOR_MODE backs the MRAM window. @pre None.
 * @post Slot B holds a valid image. @post No other slot touched.
 * @note Test-only. @since 0.1.0
 */
static void test_program_roundtrip(void)
{
  TEST_BEGIN("ra8_dfu: program round-trip + verify");

  uint8_t img[k_test_img_len];
  fill_pattern(img, (uint32_t)k_test_img_len);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_program_prepare(k_ra8_dfu_slot_b));
  sim_mark_program_ready();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dfu_program_image(k_ra8_dfu_slot_b, 0U, img, (uint32_t)k_test_img_len));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_dfu_program_commit(k_ra8_dfu_slot_b, (uint32_t)k_test_img_len, (uint32_t)k_test_seq));

  TEST_ASSERT(ra8_dfu_slot_valid(k_ra8_dfu_slot_b));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_program_verify(k_ra8_dfu_slot_b));

  /* Image bytes landed at slot_base (header-last layout: app vectors at base). */
  const uint8_t* body = (const uint8_t*)((uintptr_t)k_ra8_dfu_slot_b_base);
  TEST_ASSERT_EQ(0, memcmp(body, img, (size_t)k_test_img_len));

  /* Header is the slot's last page; entry is the SRAM run base (copy-to-run). */
  ra8_dfu_img_hdr_t h = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_read_header(k_ra8_dfu_slot_b, &h));
  TEST_ASSERT_EQ(k_ra8_dfu_hdr_magic, h.magic);
  TEST_ASSERT_EQ(k_test_seq, h.seq);
  TEST_ASSERT_EQ(k_test_img_len, h.img_len);
  TEST_ASSERT_EQ(k_ra8_dfu_run_base, h.entry);

  uint32_t seq = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_slot_seq(k_ra8_dfu_slot_b, &seq));
  TEST_ASSERT_EQ(k_test_seq, seq);

  /* Corrupt one body byte -> verify must report a CRC mismatch. */
  uint8_t* mut = (uint8_t*)body;
  mut[0]       = (uint8_t)(mut[0] ^ k_dfu_flip_mask);
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_dfu_program_verify(k_ra8_dfu_slot_b));
  TEST_ASSERT(!ra8_dfu_slot_valid(k_ra8_dfu_slot_b));

  TEST_END("ra8_dfu: program round-trip + verify");
}

/**
 * @brief Argument guards on program_image / commit / prepare, with MC/DC.
 *
 * @par MC/DC:
 * program_image length/alignment guard:
 * `(len==0) || (len%page!=0) || (img_offset%page!=0)` (3 conditions, OR).
 * - V1: len=32, off=0      -> F F F -> ok    (control: all false)
 * - V2: len=0,  off=0      -> T . . -> reject (varies len==0)
 * - V3: len=33, off=0      -> F T . -> reject (varies len%page!=0)
 * - V4: len=32, off=1      -> F F T -> reject (varies off%page!=0)
 * program_commit img_len guard:
 * `(img_len==0) || (img_len>max) || (img_len%page!=0)` (3 conditions, OR),
 * mirrored vectors (0, max+page, 33 reject; a 32-multiple in range ok).
 * Plus NULL data -> null_ptr and a none-slot -> invalid_arg.
 *
 * @pre RA8_SIMULATOR_MODE backs the MRAM window. @pre None.
 * @post No global state changes beyond Slot A scratch. @post No other slot touched.
 * @note Test-only. @since 0.1.0
 */
static void test_program_guards_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: program guards (MC/DC)");

  uint8_t buf[k_dfu_image_bytes];
  fill_pattern(buf, (uint32_t)sizeof(buf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_program_prepare(k_ra8_dfu_slot_a));
  sim_mark_program_ready();

  /* program_image OR-guard MC/DC. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_dfu_program_image(k_ra8_dfu_slot_a, 0U, buf, (uint32_t)k_ra8_dfu_page_size)); /* F F F */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_program_image(k_ra8_dfu_slot_a, 0U, buf, 0U)); /* len 0 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_program_image(k_ra8_dfu_slot_a,
                                       0U,
                                       buf,
                                       (uint32_t)k_ra8_dfu_page_size + 1U)); /* len%page */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_program_image(k_ra8_dfu_slot_a,
                                       (uint32_t)k_test_off_bad,
                                       buf,
                                       (uint32_t)k_ra8_dfu_page_size)); /* off%page */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_dfu_program_image(k_ra8_dfu_slot_a, 0U, nullptr, (uint32_t)k_ra8_dfu_page_size));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_dfu_program_image(k_ra8_dfu_slot_none, 0U, buf, (uint32_t)k_ra8_dfu_page_size));
  /* Overflow: offset + len past img_max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_program_image(k_ra8_dfu_slot_a,
                                       (uint32_t)k_ra8_dfu_img_max,
                                       buf,
                                       (uint32_t)k_ra8_dfu_page_size));

  /* program_commit img_len-guard MC/DC. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_program_commit(k_ra8_dfu_slot_a, 0U, 1U)); /* len 0 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_program_commit(k_ra8_dfu_slot_a,
                                        (uint32_t)k_ra8_dfu_img_max + (uint32_t)k_ra8_dfu_page_size,
                                        1U)); /* len > max */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_program_commit(k_ra8_dfu_slot_a,
                                        (uint32_t)k_ra8_dfu_page_size + 1U,
                                        1U)); /* len%page */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dfu_program_commit(k_ra8_dfu_slot_none, 32U, 1U));

  /* NULL-out guards on the readers. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dfu_read_header(k_ra8_dfu_slot_a, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dfu_program_verify(k_ra8_dfu_slot_none));

  TEST_END("ra8_dfu: program guards (MC/DC)");
}

/**
 * @brief `ra8_dfu_internal_write_secure` argument-guard, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision: `(src == nullptr) || (len == 0U)` (2 conditions, OR) -- the secure
 * MRAM writer's argument guard, promoted to TU-external linkage so its true
 * arms are reachable (both production callers validate src/len upstream, so
 * neither condition is presentable through the public program API).
 * - V1: src!=NULL, len!=0 -> F,F -> false (control; supplied by the program
 *   round-trip, which dispatches here with a live buffer and a page length).
 * - V2: src==NULL         -> T,. -> true  (returns invalid_arg; varies left).
 * - V3: src!=NULL, len==0 -> F,T -> true  (returns invalid_arg; varies right).
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @pre RA8_SIMULATOR_MODE backs the MRAM window. @pre None.
 * @post No slot is programmed on either guard-true vector. @post No other slot touched.
 * @note Test-only. @since 0.1.0
 */
static void test_write_secure_guard_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: internal_write_secure arg guard (MC/DC)");
  uint8_t buf[k_ra8_dfu_page_size];
  fill_pattern(buf, (uint32_t)k_ra8_dfu_page_size);
  const uintptr_t addr = (uintptr_t)k_ra8_dfu_slot_a_base;

  /* V2 (C1 true): NULL source -> invalid_arg, no write performed. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_dfu_internal_write_secure(addr, nullptr, (uint32_t)k_ra8_dfu_page_size));
  /* V3 (C2 true): zero length -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_dfu_internal_write_secure(addr, buf, 0U));

  TEST_END("ra8_dfu: internal_write_secure arg guard (MC/DC)");
}

/**
 * @brief `ra8_dfu_slot_valid` length-bound decision, with MC/DC vectors.
 *
 * @par MC/DC:
 * Decision: `(img_len != 0) && (img_len <= k_ra8_dfu_img_max) &&
 * ((img_len % k_ra8_dfu_page_size) == 0)` (3 conditions, AND) -- gates the CRC
 * fold so a corrupt header cannot drive an out-of-slot MRAM over-read. The
 * header is crafted directly in the sim-backed slot page (the same technique
 * the round-trip uses to corrupt a body byte).
 * - V1: img_len valid+aligned -> T,T,T -> true  (control; the round-trip's
 *   valid image folds the CRC and slot_valid == true).
 * - V2: img_len == 0          -> F,.,. -> false (varies C1).
 * - V3: img_len > img_max      -> T,F,. -> false (varies C2).
 * - V4: img_len not page-mult  -> T,T,F -> false (varies C3).
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC. Each false-arm header
 * also fails `ra8_dfu_hdr_valid`, so slot_valid returns false.
 *
 * @pre RA8_SIMULATOR_MODE backs the MRAM window. @pre None.
 * @post Slot A holds a crafted (invalid) header on exit. @post No other slot touched.
 * @note Test-only. @since 0.1.0
 */
static void test_slot_valid_len_bound_mcdc(void)
{
  TEST_BEGIN("ra8_dfu: slot_valid length-bound (MC/DC)");
  const uintptr_t base = (uintptr_t)k_ra8_dfu_slot_a_base;

  /* V1 control: a genuinely valid image -> the AND decision is T,T,T and
   * slot_valid == true. */
  uint8_t img[k_test_img_len];
  fill_pattern(img, (uint32_t)k_test_img_len);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_program_prepare(k_ra8_dfu_slot_a));
  sim_mark_program_ready();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_dfu_program_image(k_ra8_dfu_slot_a, 0U, img, (uint32_t)k_test_img_len));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_dfu_program_commit(k_ra8_dfu_slot_a, (uint32_t)k_test_img_len, (uint32_t)k_test_seq));
  TEST_ASSERT(ra8_dfu_slot_valid(k_ra8_dfu_slot_a));

  /* Read the just-written header; rewrite only img_len for each false arm and
   * store it straight back into the slot's header page. */
  ra8_dfu_img_hdr_t hdr = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dfu_read_header(k_ra8_dfu_slot_a, &hdr));
  uint8_t* const hdr_dst = (uint8_t*)(base + (uintptr_t)k_ra8_dfu_hdr_offset);

  /* V2 (C1 false): img_len == 0. */
  ra8_dfu_img_hdr_t h0 = hdr;
  h0.img_len           = 0U;
  (void)memcpy(hdr_dst, &h0, sizeof(h0));
  TEST_ASSERT(!ra8_dfu_slot_valid(k_ra8_dfu_slot_a));

  /* V3 (C2 false): img_len > k_ra8_dfu_img_max. */
  ra8_dfu_img_hdr_t hbig = hdr;
  hbig.img_len           = (uint32_t)k_ra8_dfu_img_max + (uint32_t)k_ra8_dfu_page_size;
  (void)memcpy(hdr_dst, &hbig, sizeof(hbig));
  TEST_ASSERT(!ra8_dfu_slot_valid(k_ra8_dfu_slot_a));

  /* V4 (C3 false): img_len not a multiple of the page size. */
  ra8_dfu_img_hdr_t hunal = hdr;
  hunal.img_len           = (uint32_t)k_ra8_dfu_page_size + 1U;
  (void)memcpy(hdr_dst, &hunal, sizeof(hunal));
  TEST_ASSERT(!ra8_dfu_slot_valid(k_ra8_dfu_slot_a));

  TEST_END("ra8_dfu: slot_valid length-bound (MC/DC)");
}

int main(void)
{
  test_slot_addressing();
  test_program_roundtrip();
  test_program_guards_mcdc();
  test_write_secure_guard_mcdc();
  test_slot_valid_len_bound_mcdc();
  return 0;
}
