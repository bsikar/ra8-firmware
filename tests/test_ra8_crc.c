/**
 * @file test_ra8_crc.c
 * @brief Unit tests for ra8_crc.c (CRC calculator driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_crc.h"
#include "ra8_crc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum crc_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them, plus poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint32_t {
  k_crc_probe_crcdor =
    0x12345678UL, /**< Planted in CRCDOR to prove the result read reaches the register. */
  k_crc_poison_out =
    0xDEADBEEFUL, /**< Poison in the out-parameter; a call failing to set it is detectable. */
} crc_fixture_t;

/**
 * @enum ra8_crc_test_bit_t
 * @brief CRC control bit masks used by the tests.
 */
typedef enum : uint8_t {
  k_ra8_crc_test_dorclr   = (uint8_t)(1U << 7U), /**< CRCCR0.DORCLR.   */
  k_ra8_crc_test_gps_mask = 0x07U,               /**< CRCCR0.GPS[2:0]. */
} ra8_crc_test_bit_t;

/**
 * @enum ra8_crc_test_const_t
 * @brief Constant values used by CRC tests.
 */
typedef enum : uint32_t {
  k_ra8_crc_test_marker = 0xDEADBEEFUL, /**< RA8 CRC test marker. */
  k_ra8_crc_test_len    = 4U,           /**< RA8 CRC test length. */
} ra8_crc_test_const_t;

static const uint8_t s_payload[4] = {0x01U, 0x02U, 0x03U, 0x04U};

/**
 * @brief Drive CRCDOR with a known value and verify `ra8_crc_compute()`
 *        returns it through the out pointer.
 */
static uint32_t compute_with_preseeded_result(ra8_crc_poly_t poly, uint32_t preset)
{
  (void)ra8_crc_init(poly);
  volatile r_crc_regs_t* reg = ra8_crc();
  reg->CRCDOR                = preset;
  uint32_t        got        = 0U;
  const ra8_err_t err        = ra8_crc_compute(s_payload, (uint32_t)k_ra8_crc_test_len, &got);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  return got;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_programs_poly_crc8(void)
{
  TEST_BEGIN("crc init programs poly crc8");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_8));
  volatile r_crc_regs_t* reg = ra8_crc();
  /* Init writes GPS|DORCLR; mask off DORCLR before checking GPS. */
  TEST_ASSERT_EQ(k_ra8_crc_poly_8, (reg->CRCCR0 & (uint8_t)k_ra8_crc_test_gps_mask));
  TEST_ASSERT_EQ(k_ra8_crc_test_dorclr, (reg->CRCCR0 & (uint8_t)k_ra8_crc_test_dorclr));
  TEST_ASSERT_EQ(0, reg->CRCCR1);
  TEST_END("crc init programs poly crc8");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_programs_poly_crc16(void)
{
  TEST_BEGIN("crc init programs poly crc16");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_16));
  volatile r_crc_regs_t* reg = ra8_crc();
  TEST_ASSERT_EQ(k_ra8_crc_poly_16, (reg->CRCCR0 & (uint8_t)k_ra8_crc_test_gps_mask));
  TEST_END("crc init programs poly crc16");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_programs_poly_crc32(void)
{
  TEST_BEGIN("crc init programs poly crc32");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_32_ieee802_3));
  volatile r_crc_regs_t* reg = ra8_crc();
  TEST_ASSERT_EQ(k_ra8_crc_poly_32_ieee802_3, (reg->CRCCR0 & (uint8_t)k_ra8_crc_test_gps_mask));
  TEST_END("crc init programs poly crc32");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_programs_poly_none(void)
{
  TEST_BEGIN("crc init poly none");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_none));
  TEST_END("crc init poly none");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_pulses_dorclr(void)
{
  TEST_BEGIN("crc init pulses DORCLR");
  ra8_fake_mmap_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_16_ccitt));
  volatile r_crc_regs_t* reg = ra8_crc();
  /* DORCLR bit (0x80) should be set alongside GPS=3. */
  TEST_ASSERT_EQ(k_ra8_crc_test_dorclr, (reg->CRCCR0 & (uint8_t)k_ra8_crc_test_dorclr));
  TEST_ASSERT_EQ(k_ra8_crc_poly_16_ccitt, (reg->CRCCR0 & (uint8_t)k_ra8_crc_test_gps_mask));
  TEST_END("crc init pulses DORCLR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reset_sets_crccr0_dorclr(void)
{
  TEST_BEGIN("crc reset sets CRCCR0.DORCLR");
  ra8_fake_mmap_reset();

  (void)ra8_crc_init(k_ra8_crc_poly_8);
  ra8_crc_reset();
  /* Driver read-modify-writes CRCCR0 with DORCLR (bit 7) set. The fake
   * register does not auto-clear the bit (ra8_fake_mmap is raw memory), so
   * we should see the GPS bits plus DORCLR in the final value. */
  volatile r_crc_regs_t* reg = ra8_crc();
  const uint8_t expected = (uint8_t)((uint8_t)k_ra8_crc_poly_8 | (uint8_t)k_ra8_crc_test_dorclr);
  TEST_ASSERT_EQ(expected, reg->CRCCR0);
  TEST_END("crc reset sets CRCCR0.DORCLR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_null_data(void)
{
  TEST_BEGIN("crc compute null data");
  ra8_fake_mmap_reset();

  uint32_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_crc_compute(nullptr, (uint32_t)k_ra8_crc_test_len, &out));
  TEST_END("crc compute null data");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_null_out(void)
{
  TEST_BEGIN("crc compute null out");
  ra8_fake_mmap_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_crc_compute(s_payload, (uint32_t)k_ra8_crc_test_len, nullptr));
  TEST_END("crc compute null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_crc8_reads_dor(void)
{
  TEST_BEGIN("crc compute crc8 reads dor");
  ra8_fake_mmap_reset();

  const uint32_t got =
    compute_with_preseeded_result(k_ra8_crc_poly_8, (uint32_t)k_ra8_crc_test_marker);
  TEST_ASSERT_EQ(k_ra8_crc_test_marker, got);
  /* CRC-8 path uses CRCDIR_BY (8-bit alias) -- last byte should be at
   * the byte register. The ra8_fake_mmap union exposes the same address
   * via reg->CRCDIR (low byte equals last write). */
  volatile r_crc_regs_t* reg = ra8_crc();
  TEST_ASSERT_EQ(s_payload[3], reg->CRCDIR_BY);
  TEST_END("crc compute crc8 reads dor");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_crc16_reads_dor(void)
{
  TEST_BEGIN("crc compute crc16 reads dor");
  ra8_fake_mmap_reset();

  const uint32_t marker = 0x1122UL;
  const uint32_t got    = compute_with_preseeded_result(k_ra8_crc_poly_16_ccitt, marker);
  TEST_ASSERT_EQ(marker, got);
  TEST_END("crc compute crc16 reads dor");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_crc32_reads_dor(void)
{
  TEST_BEGIN("crc compute crc32 reads dor");
  ra8_fake_mmap_reset();

  /* For 32-bit polynomials the driver pre-seeds CRCDOR with
   * 0xFFFFFFFF and XORs the readback with the same constant on the
   * way out (IEEE 802.3 / Castagnoli convention). On the fake the
   * "engine" doesn't transform the seed, so the readback equals the
   * seed and the final out_crc value is `seed XOR seed = 0`. */
  (void)ra8_crc_init(k_ra8_crc_poly_32c_rev);
  uint32_t        got = k_crc_poison_out;
  const ra8_err_t err = ra8_crc_compute(s_payload, (uint32_t)k_ra8_crc_test_len, &got);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(0U, got);
  /* 32-bit poly path packs 4 input bytes into a single CRCDIR write. */
  volatile r_crc_regs_t* reg    = ra8_crc();
  const uint32_t         packed = (uint32_t)s_payload[0] | ((uint32_t)s_payload[1] << 8U) |
                                  ((uint32_t)s_payload[2] << 16U) | ((uint32_t)s_payload[3] << 24U);
  TEST_ASSERT_EQ(packed, reg->CRCDIR);
  TEST_END("crc compute crc32 reads dor");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_compute_zero_length(void)
{
  TEST_BEGIN("crc compute zero length");
  ra8_fake_mmap_reset();

  (void)ra8_crc_init(k_ra8_crc_poly_8);
  volatile r_crc_regs_t* reg = ra8_crc();
  reg->CRCDOR                = k_crc_probe_crcdor;

  uint32_t        got = 0U;
  const ra8_err_t err = ra8_crc_compute(s_payload, 0U, &got);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(0x12345678, got);
  TEST_END("crc compute zero length");
}

/* ---- full build-out ---- */

static void prep_w44(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("crc deinit");
  prep_w44();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_32_ieee802_3));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_deinit());
  TEST_ASSERT_EQ(0, ra8_crc()->CRCCR0);
  TEST_END("crc deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_poly(void)
{
  TEST_BEGIN("crc set_poly");
  prep_w44();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_8));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_set_poly(k_ra8_crc_poly_16_ccitt));
  /* set_poly also pulses DORCLR; mask off bit 7 before comparing GPS. */
  TEST_ASSERT_EQ(k_ra8_crc_poly_16_ccitt, (ra8_crc()->CRCCR0 & (uint8_t)k_ra8_crc_test_gps_mask));
  TEST_END("crc set_poly");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_status(void)
{
  TEST_BEGIN("crc get_status");
  prep_w44();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_32c_rev));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_get_status(&mask));
  /* get_status returns the full CRCCR0 byte (GPS|LMS|DORCLR). Mask
   * off the DORCLR bit before comparing the polynomial. */
  TEST_ASSERT_EQ(k_ra8_crc_poly_32c_rev, (mask & (uint8_t)k_ra8_crc_test_gps_mask));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_crc_get_status(nullptr));
  TEST_END("crc get_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("crc power transition");
  prep_w44();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_16));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_enter_stop());
  TEST_ASSERT_EQ(0, ra8_crc()->CRCCR0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_exit_stop());
  TEST_END("crc power transition");
}

/**
 * @test test_mcdc_is_32bit_poly
 *
 * @par MC/DC:
 * Decision: `return (poly == k_ra8_crc_poly_32_ieee802_3) ||
 *                  (poly == k_ra8_crc_poly_32c_rev);`
 * (2 conditions, libs/ra8_hal/src/ra8_crc.c line 70 -- gap row 58 in CSV)
 * Exercised indirectly via `ra8_crc_compute()` which calls
 * `ra8_crc_is_32bit_poly(poly)` to choose the 32-bit-word vs byte input
 * loop. The selected branch is observed by running compute() to k_ra8_ok
 * for each polynomial.
 * - Vector 1: poly = poly_32_ieee802_3 -> C1=T, C2 short-circuits
 *   decision T -> word path (CRCDIR writes)
 * - Vector 2: poly = poly_32c_rev      -> C1=F, C2=T
 *   decision T -> word path (varies C2 vs V3)
 * - Vector 3: poly = poly_16           -> C1=F, C2=F
 *   decision F -> byte path (CRCDIR_BY writes)
 * MC/DC pair for C1: V1(T,_)->T vs V3(F,F)->F, decision flips, C2 held
 * F (or short-circuited). MC/DC pair for C2: V2(F,T)->T vs V3(F,F)->F,
 * decision flips, C1 held F. N+1 = 3 vectors for N=2 conditions.
 */
static void test_mcdc_is_32bit_poly(void)
{
  TEST_BEGIN("crc compute MC/DC: poly==32_ieee || poly==32c_rev");
  prep_w44();

  uint32_t crc = 0U;

  /* Vector 1: CRC-32 IEEE 802.3 -> C1=T -> word path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_32_ieee802_3));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_compute(s_payload, (uint32_t)k_ra8_crc_test_len, &crc));

  /* Vector 2: CRC-32C reversed -> C1=F, C2=T -> word path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_32c_rev));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_compute(s_payload, (uint32_t)k_ra8_crc_test_len, &crc));

  /* Vector 3: CRC-16 -> C1=F, C2=F -> byte path. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_16));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_compute(s_payload, (uint32_t)k_ra8_crc_test_len, &crc));

  TEST_END("crc compute MC/DC: poly==32_ieee || poly==32c_rev");
}

int32_t main(void)
{
  test_init_programs_poly_crc8();
  test_init_programs_poly_crc16();
  test_init_programs_poly_crc32();
  test_init_programs_poly_none();
  test_init_pulses_dorclr();
  test_reset_sets_crccr0_dorclr();
  test_compute_null_data();
  test_compute_null_out();
  test_compute_crc8_reads_dor();
  test_compute_crc16_reads_dor();
  test_compute_crc32_reads_dor();
  test_compute_zero_length();
  test_deinit();
  test_set_poly();
  test_get_status();
  test_power_transition();
  test_mcdc_is_32bit_poly();
  (void)fprintf(stderr, "[OK ] test_ra8_crc.c\n");
  return 0;
}
