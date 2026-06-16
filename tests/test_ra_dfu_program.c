/**
 * @file test_ra_dfu_program.c
 * @brief Host unit tests for the MRAM program/verify half of ra_dfu.
 *
 * @details
 * Drives the full program -> read-back -> verify round-trip against the
 * simulator-backed code-MRAM window (the `RA_SIMULATOR_MODE` mock maps
 * `0x02000000`..`0x02100000` to host memory, and `ra_flash` writes/erases land
 * there). Covers slot addressing, header construction, CRC read-back, and the
 * argument guards. Every compound boolean decision carries its `@par MC/DC:`
 * block.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_flash_regs.h"
#include "ra_dfu.h"
#include "ra_err.h"
#include "unity_minimal.h"

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
    buf[i] = (uint8_t)((i * 7U) + 3U);
  }
}

/**
 * @brief Mark the simulated MRAM controller "program-ready" (MRCPS.ABUFEMP).
 *
 * @details The host simulator backs MRCPS with plain memory and has no real
 * controller to drive the program handshake, so ra_flash's commit-wait would
 * spin to a timeout. Priming ABUFEMP (and leaving PRGBSYC/ABUFFULL clear) lets
 * the program path complete -- the same accommodation test_ra_flash.c uses.
 * `ra_flash_open` clears MRCPS as part of bring-up, so this must run AFTER
 * ::ra_dfu_program_prepare and before any program/commit.
 */
static void sim_mark_program_ready(void)
{
  *ra_mram_reg8((uint16_t)k_ra_mram_off_mrcps) = (uint8_t)k_ra_mrcps_mask_abufemp;
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
  TEST_BEGIN("ra_dfu: slot addressing");
  TEST_ASSERT_EQ((uintptr_t)k_ra_dfu_slot_a_base, ra_dfu_slot_base(k_ra_dfu_slot_a));
  TEST_ASSERT_EQ((uintptr_t)k_ra_dfu_slot_b_base, ra_dfu_slot_base(k_ra_dfu_slot_b));
  TEST_ASSERT_EQ((uintptr_t)0U, ra_dfu_slot_base(k_ra_dfu_slot_none));
  TEST_ASSERT_EQ(k_ra_dfu_slot_b, ra_dfu_other_slot(k_ra_dfu_slot_a));
  TEST_ASSERT_EQ(k_ra_dfu_slot_a, ra_dfu_other_slot(k_ra_dfu_slot_b));
  TEST_END("ra_dfu: slot addressing");
}

/**
 * @brief Full program -> verify round-trip, then a corruption fails verify.
 *
 * @par MC/DC: not applicable here -- this case is the happy-path data round
 *      trip; the decision guards are exercised in test_program_guards_mcdc.
 * @pre RA_SIMULATOR_MODE backs the MRAM window. @pre None.
 * @post Slot B holds a valid image. @post No other slot touched.
 * @note Test-only. @since 0.1.0
 */
static void test_program_roundtrip(void)
{
  TEST_BEGIN("ra_dfu: program round-trip + verify");

  uint8_t img[k_test_img_len];
  fill_pattern(img, (uint32_t)k_test_img_len);

  TEST_ASSERT_EQ(k_ra_ok, ra_dfu_program_prepare(k_ra_dfu_slot_b));
  sim_mark_program_ready();
  TEST_ASSERT_EQ(k_ra_ok, ra_dfu_program_image(k_ra_dfu_slot_b, 0U, img, (uint32_t)k_test_img_len));
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_dfu_program_commit(k_ra_dfu_slot_b, (uint32_t)k_test_img_len, (uint32_t)k_test_seq));

  TEST_ASSERT(ra_dfu_slot_valid(k_ra_dfu_slot_b));
  TEST_ASSERT_EQ(k_ra_ok, ra_dfu_program_verify(k_ra_dfu_slot_b));

  /* Image bytes landed at slot_base (header-last layout: app vectors at base). */
  const uint8_t* body = (const uint8_t*)((uintptr_t)k_ra_dfu_slot_b_base);
  TEST_ASSERT_EQ(0, memcmp(body, img, (size_t)k_test_img_len));

  /* Header is the slot's last page; entry points at the aligned slot base. */
  ra_dfu_img_hdr_t h = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_dfu_read_header(k_ra_dfu_slot_b, &h));
  TEST_ASSERT_EQ((uint32_t)k_ra_dfu_hdr_magic, h.magic);
  TEST_ASSERT_EQ((uint32_t)k_test_seq, h.seq);
  TEST_ASSERT_EQ((uint32_t)k_test_img_len, h.img_len);
  TEST_ASSERT_EQ((uint32_t)((uintptr_t)k_ra_dfu_slot_b_base), h.entry);

  uint32_t seq = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_dfu_slot_seq(k_ra_dfu_slot_b, &seq));
  TEST_ASSERT_EQ((uint32_t)k_test_seq, seq);

  /* Corrupt one body byte -> verify must report a CRC mismatch. */
  uint8_t* mut = (uint8_t*)body;
  mut[0]       = (uint8_t)(mut[0] ^ 0xFFU);
  TEST_ASSERT_EQ(k_ra_err_crc_mismatch, ra_dfu_program_verify(k_ra_dfu_slot_b));
  TEST_ASSERT(!ra_dfu_slot_valid(k_ra_dfu_slot_b));

  TEST_END("ra_dfu: program round-trip + verify");
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
 * @pre RA_SIMULATOR_MODE backs the MRAM window. @pre None.
 * @post No global state changes beyond Slot A scratch. @post No other slot touched.
 * @note Test-only. @since 0.1.0
 */
static void test_program_guards_mcdc(void)
{
  TEST_BEGIN("ra_dfu: program guards (MC/DC)");

  uint8_t buf[64];
  fill_pattern(buf, (uint32_t)sizeof(buf));
  TEST_ASSERT_EQ(k_ra_ok, ra_dfu_program_prepare(k_ra_dfu_slot_a));
  sim_mark_program_ready();

  /* program_image OR-guard MC/DC. */
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_dfu_program_image(k_ra_dfu_slot_a, 0U, buf, (uint32_t)k_ra_dfu_page_size)); /* F F F */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_dfu_program_image(k_ra_dfu_slot_a, 0U, buf, 0U)); /* len 0 */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_dfu_program_image(k_ra_dfu_slot_a,
                                      0U,
                                      buf,
                                      (uint32_t)k_ra_dfu_page_size + 1U)); /* len%page */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_dfu_program_image(k_ra_dfu_slot_a,
                                      (uint32_t)k_test_off_bad,
                                      buf,
                                      (uint32_t)k_ra_dfu_page_size)); /* off%page */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_dfu_program_image(k_ra_dfu_slot_a, 0U, nullptr, (uint32_t)k_ra_dfu_page_size));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_dfu_program_image(k_ra_dfu_slot_none, 0U, buf, (uint32_t)k_ra_dfu_page_size));
  /* Overflow: offset + len past img_max. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_dfu_program_image(k_ra_dfu_slot_a,
                                      (uint32_t)k_ra_dfu_img_max,
                                      buf,
                                      (uint32_t)k_ra_dfu_page_size));

  /* program_commit img_len-guard MC/DC. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_dfu_program_commit(k_ra_dfu_slot_a, 0U, 1U)); /* len 0 */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_dfu_program_commit(k_ra_dfu_slot_a,
                                       (uint32_t)k_ra_dfu_img_max + (uint32_t)k_ra_dfu_page_size,
                                       1U)); /* len > max */
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_dfu_program_commit(k_ra_dfu_slot_a, (uint32_t)k_ra_dfu_page_size + 1U, 1U)); /* len%page */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_dfu_program_commit(k_ra_dfu_slot_none, 32U, 1U));

  /* NULL-out guards on the readers. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_dfu_read_header(k_ra_dfu_slot_a, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_dfu_program_verify(k_ra_dfu_slot_none));

  TEST_END("ra_dfu: program guards (MC/DC)");
}

int main(void)
{
  test_slot_addressing();
  test_program_roundtrip();
  test_program_guards_mcdc();
  return 0;
}
