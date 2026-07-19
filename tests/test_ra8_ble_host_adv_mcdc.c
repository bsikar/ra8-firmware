/**
 * @file test_ra8_ble_host_adv_mcdc.c
 * @brief MC/DC vector tests for ra8_ble_l2cap.c (L2CAP / init / advertising)
 *
 * @details
 * Split out of test_ra8_ble_host.c to keep each test translation unit under
 * the repository file-size cap. This sibling owns the MC/DC vector tests
 * for the L2CAP send / ACL-in guards, the init role and name-copy-loop
 * decisions, and the advertising argument predicates; the lifecycle / GATT
 * contract tests stay in test_ra8_ble_host.c and the GATT / ATT MC/DC
 * vectors live in test_ra8_ble_host_gatt_mcdc.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_ble.h"
#include "ra8_ble_host.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ble_host_adv_mcdc_fixture_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint8_t {
  k_ble_role_invalid = 0xFFU, /**< A role value outside the enumeration, which init must reject. */
} ble_host_adv_mcdc_fixture_t;

/* Test hooks from libs/ra8_hal/src/ra8_ble.c. */
const uint8_t* ra8_ble_test_tx_capture(uint16_t* out_len);
void           ra8_ble_test_reset_capture(void);

/* Test hooks from libs/ra8_ble_host/src/ra8_ble_l2cap.c. */

/* Internal L2CAP TX entry point declared in ra8_ble_host_internal.h; we
 * re-declare it here so the MC/DC tests do not have to drag in that
 * private header. The signature MUST mirror ra8_ble_l2cap.c verbatim. */
ra8_err_t ra8_ble_host_l2cap_send(uint16_t       conn_handle,
                                  uint16_t       cid,
                                  const uint8_t* payload,
                                  uint16_t       payload_len);

typedef enum : uint16_t {
  k_test_op_le_set_adv_params = 0x2006U, /**< Test op le set adv params. */
  k_test_op_le_set_adv_data   = 0x2008U, /**< Test op le set adv data.   */
  k_test_op_le_set_adv_enable = 0x200AU, /**< Test op le set adv enable. */
  k_test_conn_handle          = 0x0040U, /**< Test conn handle.          */
  k_test_l2cap_cid_att        = 0x0004U, /**< Test L2CAP cid ATT.        */
  k_test_default_appearance   = 0x0040U, /**< Test default appearance.   */
  k_test_adv_interval_ms      = 100U,    /**< Test adv interval ms.      */
  k_test_adv_interval_too_low = 1U,      /**< Test adv interval too low. */
  k_test_value_buf_size       = 32U,     /**< Test value buffer size.    */
} ble_host_test_words_t;

typedef enum : uint8_t {
  k_test_pkt_cmd_byte     = 0x01U, /**< Test pkt cmd byte.     */
  k_test_pkt_acl_byte     = 0x02U, /**< Test pkt acl byte.     */
  k_test_att_op_write_req = 0x12U, /**< Test ATT op write req. */
  k_test_att_op_read_req  = 0x0AU, /**< Test ATT op read req.  */
  k_test_evt_count_zero   = 0U,    /**< Test evt count zero.   */
} ble_host_test_bytes_t;

/* --------------------------------------------------------------------- */

static int32_t                   s_evt_count;
static ra8_ble_host_event_kind_t s_evt_last_kind;
static uint16_t                  s_evt_last_attr;

static void stub_event_cb(void* ctx, const ra8_ble_host_event_t* evt)
{
  (void)ctx;
  s_evt_count++;
  s_evt_last_kind = evt->kind;
  s_evt_last_attr = evt->attr_handle;
}

static void prep_init(ra8_ble_host_role_t role)
{
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_test_reset_capture();
  s_evt_count                     = 0;
  s_evt_last_kind                 = k_ra8_ble_host_event_connected;
  s_evt_last_attr                 = 0U;
  const ra8_ble_host_config_t cfg = {
    .role       = role,
    .name       = "ra8d2",
    .appearance = (uint16_t)k_test_default_appearance,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_attach_event_handler(stub_event_cb, nullptr));
}

/* --------------------------------------------------------------------- */
/* MC/DC vector tests for libs/ra8_ble_host/src/ra8_ble_l2cap.c */
/* --------------------------------------------------------------------- */

typedef enum : uint16_t {
  k_mcdc_payload_len_zero     = 0U,      /**< Mcdc payload length zero.     */
  k_mcdc_payload_len_small    = 4U,      /**< Mcdc payload length small.    */
  k_mcdc_test_cid_att         = 0x0004U, /**< Mcdc test cid ATT.            */
  k_mcdc_test_conn            = 0x0040U, /**< Mcdc test conn.               */
  k_mcdc_acl_len_zero         = 0U,      /**< Mcdc acl length zero.         */
  k_mcdc_acl_len_minimal      = 4U,      /**< Mcdc acl length minimal.      */
  k_mcdc_adv_data_len_zero    = 0U,      /**< Mcdc adv data length zero.    */
  k_mcdc_adv_data_len_small   = 4U,      /**< Mcdc adv data length small.   */
  k_mcdc_adv_data_len_too_big = 64U,     /**< Mcdc adv data length too big. */
  k_mcdc_interval_too_low     = 1U,      /**< Mcdc interval too low.        */
  k_mcdc_interval_ok          = 100U,    /**< Mcdc interval ok.             */
  k_mcdc_interval_too_high    = 20000U,  /**< Mcdc interval too high.       */
} ble_l2cap_mcdc_vals_t;

/**
 * @test test_mcdc_l2cap_send_null_guard
 *
 * @par MC/DC:
 * Decision: `if ((payload == NULL) && (payload_len > 0U))`
 * (2 conditions, ra8_ble_host_l2cap_send in ra8_ble_l2cap.c)
 * - Vector 1: payload=NULL, len=0 -> C1=T, C2=F. Decision F.
 *   (proceeds; len=0 also bypasses copy; returns ok or HCI rc)
 * - Vector 2: payload=non-NULL, len=4 -> C1=F (short-circuits). Decision F.
 * - Vector 3: payload=NULL, len=4 -> C1=T, C2=T. Decision T -> null_ptr.
 * Vectors 1+3 vary C2 with C1 held T (decision flips F->T).
 * Vectors 2+3 vary C1 with C2 held T (decision flips F->T).
 * N+1 = 3 vectors for N=2 conditions.
 *
 * @par DO-178C 6.4.4.3 rationale: All three minimal MC/DC vectors are
 * exercised; no omission required.
 */
static void test_mcdc_l2cap_send_null_guard(void)
{
  TEST_BEGIN("mcdc l2cap_send (payload==NULL && len>0)");
  prep_init(k_ra8_ble_host_role_peripheral);
  static const uint8_t k_buf[k_mcdc_payload_len_small] = {0x01U, 0x02U, 0x03U, 0x04U};

  /* Vector 1: NULL, len=0. C1=T, C2=F. Falls through to send (len=0 ok). */
  (void)ra8_ble_host_l2cap_send((uint16_t)k_mcdc_test_conn,
                                (uint16_t)k_mcdc_test_cid_att,
                                nullptr,
                                (uint16_t)k_mcdc_payload_len_zero);

  /* Vector 2: non-NULL, len>0. C1=F short-circuits. Falls through. */
  (void)ra8_ble_host_l2cap_send((uint16_t)k_mcdc_test_conn,
                                (uint16_t)k_mcdc_test_cid_att,
                                k_buf,
                                (uint16_t)k_mcdc_payload_len_small);

  /* Vector 3: NULL, len>0. Decision T -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_l2cap_send((uint16_t)k_mcdc_test_conn,
                                         (uint16_t)k_mcdc_test_cid_att,
                                         nullptr,
                                         (uint16_t)k_mcdc_payload_len_small));
  TEST_END("mcdc l2cap_send (payload==NULL && len>0)");
}

/**
 * @test test_mcdc_acl_in_null_or_zero
 *
 * @par MC/DC:
 * Decision: `if ((payload == NULL) || (len == 0U))`
 * (2 conditions, ra8_ble_host_acl_in in ra8_ble_l2cap.c)
 * Routes via ra8_ble_host_test_inject_acl which calls ra8_ble_host_acl_in.
 * - Vector 1: payload=non-NULL, len=4 -> C1=F, C2=F. Decision F (proceeds).
 * - Vector 2: payload=NULL, len=4 -> C1=T short-circuits. Decision T (returns).
 * - Vector 3: payload=non-NULL, len=0 -> C1=F, C2=T. Decision T (returns).
 * Vectors 1+2 vary C1 with C2 held F (decision flips F->T).
 * Vectors 1+3 vary C2 with C1 held F (decision flips F->T).
 * N+1 = 3 vectors for N=2 conditions.
 */
static void test_mcdc_acl_in_null_or_zero(void)
{
  TEST_BEGIN("mcdc acl_in (payload==NULL || len==0)");
  prep_init(k_ra8_ble_host_role_peripheral);
  static const uint8_t k_frame[k_mcdc_acl_len_minimal] = {0U, 0U, 0x04U, 0U};

  /* Vector 1: non-NULL, len=4. Both F: function proceeds (no crash). */
  ra8_ble_host_test_inject_acl((uint16_t)k_mcdc_test_conn,
                               k_frame,
                               (uint16_t)k_mcdc_acl_len_minimal);

  /* Vector 2: NULL, len=4. C1 short-circuits T -> early return. */
  ra8_ble_host_test_inject_acl((uint16_t)k_mcdc_test_conn,
                               nullptr,
                               (uint16_t)k_mcdc_acl_len_minimal);

  /* Vector 3: non-NULL, len=0. C2=T -> early return. */
  ra8_ble_host_test_inject_acl((uint16_t)k_mcdc_test_conn, k_frame, (uint16_t)k_mcdc_acl_len_zero);
  TEST_END("mcdc acl_in (payload==NULL || len==0)");
}

/**
 * @test test_mcdc_init_role_4cond
 *
 * @par MC/DC:
 * Decision: `(role != peripheral) && (role != central) &&
 *           (role != observer) && (role != broadcaster)`
 * (4 conditions, ra8_ble_host_init in ra8_ble_l2cap.c)
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Full MC/DC for N=4 of identical short-circuit form requires N+1=5
 * vectors. Each valid role short-circuits at exactly its own position
 * (peripheral at C1, central at C2, observer at C3, broadcaster at C4),
 * driving the decision F. A bogus role drives all four to T. The five
 * vectors below independently vary each condition while the others are
 * held at their masking value, achieving full MC/DC with no omission.
 *
 * - V1 role=peripheral    -> C1=F (short-circuit). Decision F (ok).
 * - V2 role=central       -> C1=T,C2=F. Decision F (ok).
 * - V3 role=observer      -> C1=T,C2=T,C3=F. Decision F (ok).
 * - V4 role=broadcaster   -> C1=T,C2=T,C3=T,C4=F. Decision F (ok).
 * - V5 role=0xFF (bogus)  -> all T. Decision T -> invalid_arg.
 */
static void test_mcdc_init_role_4cond(void)
{
  TEST_BEGIN("mcdc init role (4-cond AND chain)");
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_host_config_t cfg = {.role       = k_ra8_ble_host_role_peripheral,
                               .name       = "x",
                               .appearance = 0U};

  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  /* V2 */
  cfg.role = k_ra8_ble_host_role_central;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  /* V3 */
  cfg.role = k_ra8_ble_host_role_observer;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  /* V4 */
  cfg.role = k_ra8_ble_host_role_broadcaster;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&cfg));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  /* V5: bogus role */
  cfg.role = (ra8_ble_host_role_t)k_ble_role_invalid;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ble_host_init(&cfg));
  TEST_END("mcdc init role (4-cond AND chain)");
}

/**
 * @test test_mcdc_advertise_role_guard
 *
 * @par MC/DC:
 * Decision: `(role != peripheral) && (role != broadcaster)`
 * (2 conditions, internal_advertise_validate in
 * ra8_ble_l2cap_advertise.c)
 * - V1 role=peripheral  -> C1=F short-circuits. F (proceeds).
 * - V2 role=broadcaster -> C1=T,C2=F. F (proceeds).
 * - V3 role=central     -> C1=T,C2=T. T -> invalid_arg.
 * V1 vs V3 vary C1 (decision F->T). V2 vs V3 vary C2 (F->T). N+1=3.
 */
static void test_mcdc_advertise_role_guard(void)
{
  TEST_BEGIN("mcdc advertise role guard");
  static const uint8_t k_adv[k_mcdc_adv_data_len_small] = {0U, 0U, 0U, 0U};
  /* V1: peripheral -> ok */
  prep_init(k_ra8_ble_host_role_peripheral);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V2: broadcaster -> ok */
  prep_init(k_ra8_ble_host_role_broadcaster);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V3: central -> invalid_arg */
  prep_init(k_ra8_ble_host_role_central);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_END("mcdc advertise role guard");
}

/**
 * @test test_mcdc_advertise_adv_data_null
 *
 * @par MC/DC:
 * Decision: `(adv_data == NULL) && (adv_data_len > 0)`
 * (2 conditions, internal_advertise_validate in
 * ra8_ble_l2cap_advertise.c)
 * - V1 ptr=NULL, len=0 -> C1=T, C2=F. F (proceeds).
 * - V2 ptr=non-NULL, len=4 -> C1=F short-circuits. F (proceeds).
 * - V3 ptr=NULL, len=4 -> both T. T -> null_ptr.
 * V1+V3 vary C2 (decision flips). V2+V3 vary C1 (decision flips). N+1=3.
 */
static void test_mcdc_advertise_adv_data_null(void)
{
  TEST_BEGIN("mcdc advertise adv_data null guard");
  static const uint8_t k_adv[k_mcdc_adv_data_len_small] = {0U, 0U, 0U, 0U};
  prep_init(k_ra8_ble_host_role_peripheral);
  /* V1 */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_ble_host_advertise_start(nullptr, 0U, nullptr, 0U, (uint16_t)k_mcdc_interval_ok));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_advertise_start(nullptr,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_END("mcdc advertise adv_data null guard");
}

/**
 * @test test_mcdc_advertise_lengths_too_big
 *
 * @par MC/DC:
 * Decision: `(adv_data_len > MAX) || (scan_resp_len > MAX)`
 * (2 conditions, internal_advertise_validate in
 * ra8_ble_l2cap_advertise.c)
 * - V1 adv=4, sr=0 -> both F. F (proceeds).
 * - V2 adv=64, sr=0 -> C1=T short-circuits. T -> invalid_arg.
 * - V3 adv=4, sr=64 -> C1=F, C2=T. T -> invalid_arg.
 * V1+V2 vary C1 (decision flips). V1+V3 vary C2 (decision flips). N+1=3.
 */
static void test_mcdc_advertise_lengths_too_big(void)
{
  TEST_BEGIN("mcdc advertise length cap (OR)");
  static const uint8_t k_buf_big[k_mcdc_adv_data_len_too_big] = {};
  prep_init(k_ra8_ble_host_role_peripheral);
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_buf_big,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V2: adv too big */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_buf_big,
                                              (uint8_t)k_mcdc_adv_data_len_too_big,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  /* V3: scan_resp too big */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_buf_big,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              k_buf_big,
                                              (uint8_t)k_mcdc_adv_data_len_too_big,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_END("mcdc advertise length cap (OR)");
}

/**
 * @test test_mcdc_advertise_interval_range
 *
 * @par MC/DC:
 * Decision: `(interval_ms < MIN) || (interval_ms > MAX)`
 * (2 conditions, internal_advertise_validate in
 * ra8_ble_l2cap_advertise.c)
 * - V1 interval=100 -> both F. F (proceeds, ok).
 * - V2 interval=1   -> C1=T short-circuits. T -> invalid_arg.
 * - V3 interval=20000 -> C1=F, C2=T. T -> invalid_arg.
 * V1+V2 vary C1; V1+V3 vary C2. N+1=3.
 */
static void test_mcdc_advertise_interval_range(void)
{
  TEST_BEGIN("mcdc advertise interval range (OR)");
  static const uint8_t k_adv[k_mcdc_adv_data_len_small] = {};
  prep_init(k_ra8_ble_host_role_peripheral);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_too_low));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_too_high));
  TEST_END("mcdc advertise interval range (OR)");
}

/* --------------------------------------------------------------------- */
/* MC/DC vector tests for ra8_ble_l2cap.c remaining decisions */
/* --------------------------------------------------------------------- */

/* Internal ATT entry point declared in ra8_ble_host_internal.h; mirror the
 * signature here so MC/DC tests need not pull in that private header. */
void ra8_ble_host_att_handle_pdu(uint16_t conn_handle, const uint8_t* pdu, uint16_t pdu_len);

typedef enum : uint16_t {
  k_mcdc_att_pdu_len_zero  = 0U, /**< Mcdc ATT pdu length zero.  */
  k_mcdc_att_pdu_len_small = 5U, /**< Mcdc ATT pdu length small. */
} ble_att_mcdc_vals_t;

/**
 * @test test_mcdc_advertise_scan_resp_null
 *
 * @par MC/DC:
 * Decision: `(scan_resp == NULL) && (scan_resp_len > 0)`
 * (2 conditions, internal_advertise_validate in
 * ra8_ble_l2cap_advertise.c)
 * - V1 ptr=NULL, len=0 -> C1=T, C2=F. Decision F (proceeds).
 * - V2 ptr=non-NULL, len=4 -> C1=F short-circuits. Decision F (proceeds).
 * - V3 ptr=NULL, len=4 -> both T. Decision T -> null_ptr.
 * V1+V3 vary C2 with C1 held T (decision flips F->T).
 * V2+V3 vary C1 with C2 held T (decision flips F->T).
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved; no omission.
 */
static void test_mcdc_advertise_scan_resp_null(void)
{
  TEST_BEGIN("mcdc advertise scan_resp null guard");
  static const uint8_t k_adv[k_mcdc_adv_data_len_small] = {0U, 0U, 0U, 0U};
  static const uint8_t k_sr[k_mcdc_adv_data_len_small]  = {0U, 0U, 0U, 0U};
  prep_init(k_ra8_ble_host_role_peripheral);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              0U,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              k_sr,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_ble_host_advertise_start(k_adv,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              nullptr,
                                              (uint8_t)k_mcdc_adv_data_len_small,
                                              (uint16_t)k_mcdc_interval_ok));
  TEST_END("mcdc advertise scan_resp null guard");
}

/**
 * @test test_mcdc_init_name_copy_loop
 *
 * @par MC/DC:
 * Decision: `while ((i < k_name_copy_max) && (cfg->name[i] != '\\0'))`
 * (2 conditions, the name-copy loop of ra8_ble_host_init in
 * ra8_ble_l2cap.c)
 * Loop continuation predicate.
 * - V1 name="x"            -> iter0: C1=T,C2=T (enter). iter1: C2=F (exit
 *   via C2). Independent flip of C2 with C1 held T (T-T -> T-F).
 * - V2 name="" (empty)     -> iter0: C1=T,C2=F (immediate exit on C2).
 * - V3 name=36-char string -> iter0..30: C1=T,C2=T. iter31: C1=F (i==max).
 *   Independent flip of C1 with C2 held T (T-T -> F-T).
 * N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale: Full minimal MC/DC achieved.
 */
static void test_mcdc_init_name_copy_loop(void)
{
  TEST_BEGIN("mcdc init name-copy loop (i<max && name[i]!=0)");
  ra8_sim_mmap_reset();
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_host_config_t c1 = {.role       = k_ra8_ble_host_role_peripheral,
                              .name       = "x",
                              .appearance = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&c1));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  ra8_ble_host_config_t c2 = {.role = k_ra8_ble_host_role_peripheral, .name = "", .appearance = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&c2));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  static const char     k_long[] = "0123456789012345678901234567890ABCDE";
  ra8_ble_host_config_t c3       = {.role       = k_ra8_ble_host_role_peripheral,
                                    .name       = k_long,
                                    .appearance = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ble_host_init(&c3));
  (void)ra8_ble_host_close();
  (void)ra8_ble_close();
  TEST_END("mcdc init name-copy loop (i<max && name[i]!=0)");
}

int main(void)
{
  test_mcdc_l2cap_send_null_guard();
  test_mcdc_acl_in_null_or_zero();
  test_mcdc_init_role_4cond();
  test_mcdc_advertise_role_guard();
  test_mcdc_advertise_adv_data_null();
  test_mcdc_advertise_lengths_too_big();
  test_mcdc_advertise_interval_range();
  test_mcdc_advertise_scan_resp_null();
  test_mcdc_init_name_copy_loop();
  return 0;
}
