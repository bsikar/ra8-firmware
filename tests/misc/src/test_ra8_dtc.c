/**
 * @file test_ra8_dtc.c
 * @brief Unit tests for ra8_dtc.c (Data Transfer Controller)
 * @details Covers DTC descriptor setup, vector-table activation, transfer validation, and lifecycle state using bounded fixture storage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dtc.h"
#include "ra8_dtc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum dtc_fixture_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_dtc_regs_bytes =
    0x30U, /**< Required DTC register-block size; static_assert catches hardware-layout drift. */
} dtc_fixture_t;

/**
 * @enum dtc_fixture2_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint16_t {
  k_dtc_probe_sts_c =
    0xBEADU, /**< A third value, one nibble off the second, so a partial-width read is visible. */
  k_dtc_probe_sts_b =
    0xBEEFU, /**< A second, different value, so the read cannot be a cached first result. */
  /** Planted in DTCSTS to prove the read reaches the register. */
  k_dtc_probe_sts_a = 0xCAFEU,
} dtc_fixture2_t;

typedef enum : uintptr_t {
  k_ra8_dtc_test_vector_addr  = 0x22000400UL, /**< Arbitrary SRAM-region pointer. */
  k_ra8_dtc_test_vector_addr2 = 0x22000800UL, /**< Secondary reconfig target.     */
} ra8_dtc_test_addr_t;

/* Compile-time check: the regs block matches FSP R_DTC_Type
 * (size = 0x30 / 48 bytes per RA8D2 CMSIS R_DTC_Type). */
static_assert(sizeof(r_dtc_regs_t) == k_dtc_regs_bytes,
              "r_dtc_regs_t must be 48 bytes (FSP R_DTC_Type)");
static_assert(sizeof(r_dtc_xfer_info_t) == 16U, "r_dtc_xfer_info_t must be 16 bytes (HUM 18.2)");

static uint32_t s_dtc_cb_count;
static uint16_t s_dtc_cb_last_mask;

static void stub_dtc_cb(void* ctx, uint16_t mask)
{
  (void)ctx;
  ++s_dtc_cb_count;
  s_dtc_cb_last_mask = mask;
}

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_dtc_cb_count     = 0U;
  s_dtc_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_vector(void)
{
  TEST_BEGIN("dtc init null vector");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_init(nullptr));
  TEST_END("dtc init null vector");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("dtc init happy");
  prep();

  void*           vec = (void*)(uintptr_t)k_ra8_dtc_test_vector_addr;
  const ra8_err_t err = ra8_dtc_init(vec);
  TEST_ASSERT_EQ(k_ra8_ok, err);

  volatile r_dtc_regs_t* reg = ra8_dtc();
  TEST_ASSERT_EQ(0, reg->DTCCR);
  TEST_ASSERT_EQ(0, reg->DTCST);
  TEST_ASSERT_EQ(k_ra8_dtc_test_vector_addr, reg->DTCVBR);
  TEST_END("dtc init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enable_then_disable(void)
{
  TEST_BEGIN("dtc enable then disable");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_enable());
  volatile r_dtc_regs_t* reg = ra8_dtc();
  TEST_ASSERT_EQ(k_ra8_dtcst_dtcst_msk, reg->DTCST);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_disable());
  TEST_ASSERT_EQ(0, reg->DTCST);
  TEST_END("dtc enable then disable");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("dtc deinit");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_init((void*)(uintptr_t)k_ra8_dtc_test_vector_addr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_deinit());
  volatile r_dtc_regs_t* reg = ra8_dtc();
  TEST_ASSERT_EQ(0, reg->DTCVBR);
  TEST_END("dtc deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_reconfigure(void)
{
  TEST_BEGIN("dtc reconfigure");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_init((void*)(uintptr_t)k_ra8_dtc_test_vector_addr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_reconfigure(nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_reconfigure((void*)(uintptr_t)k_ra8_dtc_test_vector_addr2));
  volatile r_dtc_regs_t* reg = ra8_dtc();
  TEST_ASSERT_EQ(k_ra8_dtc_test_vector_addr2, reg->DTCVBR);
  TEST_ASSERT_EQ(0, reg->DTCST);
  /* FSP-aligned: reconfigure leaves DTCCR with RRS enabled (0x18). */
  TEST_ASSERT_EQ(k_ra8_dtccr_rrs_enable, reg->DTCCR);
  TEST_END("dtc reconfigure");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("dtc status read + clear");
  prep();

  ra8_dtc()->DTCSTS = k_dtc_probe_sts_c;
  uint16_t mask     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_get_status(&mask));
  TEST_ASSERT_EQ(0xBEADU, mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_clear_status(0x00FFU));
  TEST_ASSERT_EQ((0xBEADU & ~0x00FFU), ra8_dtc()->DTCSTS);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_dtc_get_status(nullptr));
  TEST_END("dtc status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("dtc attach + dispatch");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_attach_handler(stub_dtc_cb, (void*)(uintptr_t)0xD0U));
  ra8_dtc()->DTCSTS = k_dtc_probe_sts_a;
  ra8_dtc_dispatch();
  TEST_ASSERT_EQ(1, s_dtc_cb_count);
  TEST_ASSERT_EQ(0xCAFEU, s_dtc_cb_last_mask);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_attach_handler(nullptr, nullptr));
  ra8_dtc()->DTCSTS = k_dtc_probe_sts_b;
  ra8_dtc_dispatch();
  TEST_ASSERT_EQ(1, s_dtc_cb_count);
  TEST_END("dtc attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("dtc power transition");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_init((void*)(uintptr_t)k_ra8_dtc_test_vector_addr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_enter_stop());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_dtc_exit_stop());
  TEST_END("dtc power transition");
}

int main(void)
{
  test_init_null_vector();
  test_init_happy();
  test_enable_then_disable();
  test_deinit();
  test_reconfigure();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  return 0;
}
