/**
 * @file test_ra8_ble_gatt.c
 * @brief Standalone MC/DC unit tests for libs/ra8_ble_host/src/ra8_ble_gatt.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_ble_gatt_internal.h"
#include "ra8_ble_host.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ble_gatt_uuid_t
 * @brief UUID markers for the fixture's service and characteristic, on separate pages so the two cannot alias.
 */
typedef enum : uint8_t {
  k_uuid_gatt_svc = 0x70U, /**< Service of this file's registration fixture. */
  k_uuid_gatt_chr =
    0x80U, /**< Its characteristic; a separate marker page so the two UUIDs cannot alias. */
} ble_gatt_uuid_t;

typedef enum : uint16_t {
  k_test_gatt_handle_unknown = 0xBEEFU, /**< Test GATT handle unknown. */
} test_gatt_t;

typedef enum : uint16_t {
  k_test_value_buf_size = 32U, /**< Test value buffer size. */
} test_gatt_buf_t;

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
}

static void make_uuid_g(uint8_t out[16], uint8_t seed)
{
  for (uint8_t i = 0U; i < 16U; ++i) {
    out[i] = (uint8_t)(seed + i);
  }
}

/**
 * @test test_mcdc_gatt_notify_init_and_lookup
 *
 * @par MC/DC:
 * Decision A: ``if (st->initialized == 0U)``
 *   (1 condition, ra8_ble_host_gatt_notify in ra8_ble_gatt.c)
 * Decision B: ``if ((a == NULL) || (a->kind != k_attr_kind_char_value))``
 *   (2 conditions, ra8_ble_host_gatt_notify in ra8_ble_gatt.c)
 *
 * Per DO-178C 6.4.4.3 the standalone fixture covers V_A1 (uninit) +
 * V_A2 (init) + V_B2 (a==NULL via unknown handle). The remaining
 * V_B1 (happy) and V_B3 (a!=NULL but kind!=char_value) are exercised
 * end-to-end from test_ra8_ble_host.c, recorded here per DO-178C
 * 6.4.4.3 representative-subset clause (multi-fixture coverage).
 */
static void test_mcdc_gatt_notify_init_and_lookup(void)
{
  TEST_BEGIN("ra8_ble_host_gatt_notify MC/DC: init + lookup guards");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_ble_host_gatt_notify((uint16_t)k_test_gatt_handle_unknown));
  prep();
  const ra8_ble_host_config_t cfg = {.role       = k_ra8_ble_host_role_peripheral,
                                     .appearance = 0U,
                                     .name       = "g"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 ra8_ble_host_gatt_notify((uint16_t)k_test_gatt_handle_unknown));
  TEST_END("ra8_ble_host_gatt_notify MC/DC: init + lookup guards");
}

/**
 * @test test_mcdc_gatt_set_value_zero_len
 *
 * @par MC/DC:
 * Decision (libs/ra8_ble_host/src/ra8_ble_gatt.c):
 *   ``if ((len > 0U) && (a->value != NULL))``  (2 conditions)
 * - Vector 1 (T,T): set_value(chr, payload, 4) -- copies (covered elsewhere
 *   by tests/test_ra8_ble_host.c::test_set_value_and_notify_paths).
 * - Vector 2 (F,T): set_value(chr, payload, 0) -- skips memcpy, varies len.
 * Vectors 1+2 prove ``len > 0`` independently affects outcome.
 * (a->value NULL is unreachable through the public API because
 * register_char rejects value_buf==NULL when value_max>0; recorded as
 * a defensive guard per DO-178C 6.4.4.3 deactivated-condition note.)
 */
static void test_mcdc_gatt_set_value_zero_len(void)
{
  TEST_BEGIN("ra8_ble_host_gatt_set_value MC/DC: len==0 path");
  prep();
  const ra8_ble_host_config_t cfg = {.role       = k_ra8_ble_host_role_peripheral,
                                     .appearance = 0U,
                                     .name       = "g"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));

  uint8_t svc_uuid[16];
  uint8_t chr_uuid[16];
  make_uuid_g(svc_uuid, k_uuid_gatt_svc);
  make_uuid_g(chr_uuid, k_uuid_gatt_chr);
  uint16_t svc = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_register_service(svc_uuid, &svc));
  uint8_t  buf[k_test_value_buf_size];
  uint16_t chr = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_gatt_register_char(
                   svc,
                   chr_uuid,
                   (uint8_t)(k_ra8_ble_host_char_prop_read | k_ra8_ble_host_char_prop_notify),
                   buf,
                   (uint16_t)k_test_value_buf_size,
                   &chr));

  /* Vector 2: F,T -- len=0 with non-NULL value buf. */
  const uint8_t payload[1] = {0xAAU};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_set_value(chr, payload, 0U));

  /* Notify with no connection still returns ok and exercises the
   * value_len==0 branch in ra8_ble_host_gatt_notify (same shape
   * decision). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_gatt_notify(chr));
  TEST_END("ra8_ble_host_gatt_set_value MC/DC: len==0 path");
}

/**
 * @test test_mcdc_gatt_internal_should_copy
 *
 * @par MC/DC:
 * Decision at the ra8_ble_host_gatt_set_value and
 * ra8_ble_host_gatt_notify call sites -> helper
 * ra8_ble_gatt_internal_should_copy in ra8_ble_gatt.c:
 *   ``len > 0 && value != NULL`` (2 conditions, AND).
 * - V1: len=0,  val!=NULL -> false (left varies vs V2)
 * - V2: len>0,  val!=NULL -> true
 * - V3: len>0,  val=NULL  -> false (right varies vs V2)
 * N+1 = 3.
 */
static void test_mcdc_gatt_internal_should_copy(void)
{
  TEST_BEGIN("ra8_ble_gatt MC/DC: should_copy AND");
  uint8_t scratch = 0U;
  TEST_ASSERT(!ra8_ble_gatt_internal_should_copy(0U, &scratch));
  TEST_ASSERT(ra8_ble_gatt_internal_should_copy(4U, &scratch));
  TEST_ASSERT(!ra8_ble_gatt_internal_should_copy(4U, nullptr));
  TEST_END("ra8_ble_gatt MC/DC: should_copy AND");
}

/**
 * @test test_mcdc_gatt_internal_notify_invalid
 *
 * @par MC/DC:
 * Decision at libs/ra8_ble_host/src/ra8_ble_gatt.c (call site) -> helper at
 * libs/ra8_ble_host/src/ra8_ble_gatt.c:
 *   ``decl_present == 0 || (props & notify_mask) == 0``
 *   (2 conditions, OR).
 * - V1: present=1, notify-bit-set     -> false
 * - V2: present=0, notify-bit-set     -> true (varies left)
 * - V3: present=1, notify-bit-clear   -> true (varies right)
 * N+1 = 3.
 */
static void test_mcdc_gatt_internal_notify_invalid(void)
{
  TEST_BEGIN("ra8_ble_gatt MC/DC: notify_invalid OR");
  TEST_ASSERT(!ra8_ble_gatt_internal_notify_invalid(1U,
                                                    (uint8_t)k_ra8_ble_host_char_prop_notify,
                                                    (uint8_t)k_ra8_ble_host_char_prop_notify));
  TEST_ASSERT(ra8_ble_gatt_internal_notify_invalid(0U,
                                                   (uint8_t)k_ra8_ble_host_char_prop_notify,
                                                   (uint8_t)k_ra8_ble_host_char_prop_notify));
  TEST_ASSERT(
    ra8_ble_gatt_internal_notify_invalid(1U, 0U, (uint8_t)k_ra8_ble_host_char_prop_notify));
  TEST_END("ra8_ble_gatt MC/DC: notify_invalid OR");
}

int32_t main(void)
{
  test_mcdc_gatt_notify_init_and_lookup();
  test_mcdc_gatt_set_value_zero_len();
  test_mcdc_gatt_internal_should_copy();
  test_mcdc_gatt_internal_notify_invalid();
  (void)fprintf(stderr, "[OK ] test_ra8_ble_gatt.c\n");
  return 0;
}
