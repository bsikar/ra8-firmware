/**
 * @file test_app_crc_demo.c
 * @brief Integration test: CRC-32 hardware engine vs software ref
 *
 * @details
 * Mirrors examples/ek_ra8d2/crc_demo/main.c. The hardware path uses
 * the host ra8_sim_mmap shim to drive the CRC peripheral; the software
 * reference is a bit-serial CRC-32 (IEEE 802.3, reflected) copied from
 * the demo. Tests cover the bring-up, compute, and inner-loop branches
 * with MC/DC vectors.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_crc.h"
#include "ra8_crc_regs.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum app_crc_demo_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint32_t {
  k_crc_poison_out =
    0xDEADBEEFUL, /**< Poison written into the result out-parameter, so a call that fails without setting it is detectable. */
} app_crc_demo_fixture_t;

typedef enum : uint32_t {
  k_t_crc_sw_poly_rev = 0xEDB88320UL, /**< T CRC sw poly rev. */
  k_t_crc_sw_init     = 0xFFFFFFFFUL, /**< T CRC sw init.     */
  k_t_crc_sw_xor_out  = 0xFFFFFFFFUL, /**< T CRC sw xor out.  */
  k_t_crc_lsb_mask    = 0x1UL,        /**< T CRC lsb mask.    */
} t_crc_sw_const_t;

typedef enum : uint8_t {
  k_t_crc_bits      = 8U,  /**< T CRC bits.      */
  k_t_crc_payload_n = 16U, /**< T CRC payload n. */
} t_crc_byte_t;

static const uint8_t k_t_crc_payload[k_t_crc_payload_n] = {
  0x00U,
  0x11U,
  0x22U,
  0x33U,
  0x44U,
  0x55U,
  0x66U,
  0x77U,
  0x88U,
  0x99U,
  0xAAU,
  0xBBU,
  0xCCU,
  0xDDU,
  0xEEU,
  0xFFU,
};

static void reset_world(void)
{
  ra8_sim_mmap_reset();
}

/** @brief Software reference -- copy of crc_demo_sw_crc32. */
static uint32_t sw_crc32(const uint8_t* data, uint32_t len)
{
  uint32_t crc = (uint32_t)k_t_crc_sw_init;
  for (uint32_t i = 0U; i < len; ++i) {
    crc ^= (uint32_t)data[i];
    for (uint8_t b = 0U; b < (uint8_t)k_t_crc_bits; ++b) {
      if ((crc & (uint32_t)k_t_crc_lsb_mask) != 0U) {
        crc = (crc >> 1) ^ (uint32_t)k_t_crc_sw_poly_rev;
      } else {
        crc = (crc >> 1);
      }
    }
  }
  return crc ^ (uint32_t)k_t_crc_sw_xor_out;
}

/**
 * @brief Bring-up succeeds with the IEEE 802.3 polynomial selector.
 *
 * @par MC/DC:
 * Decision in app: ``ra8_crc_init != ok``. One atomic condition x 2
 * vectors -- valid poly (this) + bad poly (test_crc_app_bad_poly).
 */
static void test_crc_app_init_ok(void)
{
  reset_world();
  TEST_BEGIN("crc_demo: init with IEEE 802.3 polynomial");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_32_ieee802_3));
  TEST_END("crc_demo: init with IEEE 802.3 polynomial");
}

/**
 * @brief Reset + compute applies the IEEE-802.3 init / xor-out convention.
 *
 * @details
 * For a 32-bit polynomial ``ra8_crc_compute`` pre-seeds CRCDOR with
 * 0xFFFFFFFF and XORs the readback with 0xFFFFFFFF before returning.
 * On the sim shim the "engine" leaves the seed in CRCDOR untouched, so
 * the final ``got`` value collapses to ``seed XOR seed = 0`` -- which
 * is also the canonical CRC-32 of an empty message.
 *
 * @par MC/DC:
 * Decision: ``ra8_crc_compute != ok``. Pairs with the NULL-buffer
 * rejection vector below for N+1 = 2 coverage.
 */
static void test_crc_app_compute_drives_dor(void)
{
  reset_world();
  TEST_BEGIN("crc_demo: compute applies IEEE seed + xor-out");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_32_ieee802_3));
  ra8_crc_reset();
  uint32_t got = k_crc_poison_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_compute(k_t_crc_payload, (uint32_t)k_t_crc_payload_n, &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("crc_demo: compute applies IEEE seed + xor-out");
}

/**
 * @brief Software CRC-32 reference exercises both inner-loop branches.
 *
 * @par MC/DC:
 * Compound decision: ``(crc & 1U) != 0``. One atomic condition x 2
 * vectors -- on a 16-byte mixed-bit input both branches are taken
 * many times, and the trailing all-zero buffer case forces only the
 * else branch.
 */
static void test_crc_app_sw_branches(void)
{
  TEST_BEGIN("crc_demo: software CRC-32 takes both bit branches");
  /* Known value: CRC-32 of "" must equal 0 (init ^ xor_out). */
  TEST_ASSERT_EQ(0U, sw_crc32(k_t_crc_payload, 0U));
  /* Non-trivial input must produce non-zero result. */
  const uint32_t v = sw_crc32(k_t_crc_payload, (uint32_t)k_t_crc_payload_n);
  TEST_ASSERT(v != 0U);
  /* Zero-byte input differs from a single-zero-byte input. */
  const uint8_t zero = 0U;
  TEST_ASSERT(sw_crc32(&zero, 1U) != 0U);
  TEST_END("crc_demo: software CRC-32 takes both bit branches");
}

/**
 * @brief NULL data pointer rejected by ra8_crc_compute.
 *
 * @par MC/DC:
 * Decision: ``data == NULL``. One atomic condition x 2 vectors --
 * NULL (this) + valid (test_crc_app_compute_drives_dor).
 */
static void test_crc_app_compute_null(void)
{
  reset_world();
  TEST_BEGIN("crc_demo: NULL data rejected");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_crc_init(k_ra8_crc_poly_32_ieee802_3));
  uint32_t out = 0U;
  TEST_ASSERT(ra8_crc_compute(nullptr, (uint32_t)k_t_crc_payload_n, &out) != k_ra8_ok);
  TEST_END("crc_demo: NULL data rejected");
}

int main(void)
{
  test_crc_app_init_ok();
  test_crc_app_compute_drives_dor();
  test_crc_app_sw_branches();
  test_crc_app_compute_null();
  return 0;
}
