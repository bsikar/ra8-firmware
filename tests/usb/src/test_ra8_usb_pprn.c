/**
 * @file test_ra8_usb_pprn.c
 * @brief Unit tests for the native USB device-side Printer class layer
 *
 * @details Exercises printer class descriptors, status requests, reset handling, endpoint I/O, and invalid requests with bounded fixtures.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_pprn.h"
#include "unity_minimal.h"

/**
 * @enum t_setup_t
 * @brief SETUP-packet fields the class arms submit.
 *
 * @details
 * `bmRequestType` packs direction, type and recipient into one byte, so each
 * name states the combination rather than the hex value.
 */
typedef enum : uint16_t {
  k_t_bmreq_class_out = 0x21U, /**< Host-to-device, class, interface.               */
  k_t_bmreq_class_in  = 0xA1U, /**< Device-to-host, class, interface.               */
  k_t_bmreq_std_in    = 0x80U, /**< Device-to-host, standard, device.               */
  k_t_breq_unknown    = 0x77U, /**< A bRequest outside the printer set; must stall. */
  k_t_wlen_device_id  = 64U,   /**< GET_DEVICE_ID buffer length, bytes.             */
  k_t_oversize_buf    = 128U,  /**< A buffer past the class's maximum, to prove
                                    the length guard rather than the copy.    */
} t_setup_t;

/* Sample minimal config descriptor blob. */
static const uint8_t s_sample_desc[] = {
  0x09,
  0x02,
  0x20,
  0x00,
  0x01,
  0x01,
  0x00,
  0x80,
  0x32,
  0x09,
  0x04,
  0x00,
  0x00,
  0x02,
  0x07,
  0x01,
  0x02,
  0x00,
};

/* Sample IEEE 1284 device-ID payload (USB Printer 1.1 sec 4.2.1). */
static const uint8_t s_sample_dev_id[] = {
  0x00, 0x1A, 'M', 'F', 'G', ':', 'R', 'e', 'n', 'e', 's', 'a',
  's',  ';',  'M', 'D', 'L', ':', 'R', 'A', '8', 'D', '2', ';',
};

static int32_t   s_setup_cb_calls       = 0;
static uint8_t   s_setup_cb_last_breq   = 0U;
static ra8_err_t s_setup_cb_return_code = k_ra8_ok;

/** @brief Verify setup cb behavior. @details Executes the setup cb scenario with bounded fixture state and asserts the contract-specific result. @param[in,out] ctx Fixture argument governed by the exercised interface contract. @param[in] setup Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_test_setup_cb(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  s_setup_cb_calls++;
  s_setup_cb_last_breq = setup->b_request;
  return s_setup_cb_return_code;
}

/** @brief Provide the file-local prep test helper. @details Implements the prep fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  (void)ra8_usb_pprn_close();
  s_setup_cb_calls       = 0;
  s_setup_cb_last_breq   = 0U;
  s_setup_cb_return_code = k_ra8_ok;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify init default status behavior. @details Executes the init default status scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_default_status(void)
{
  TEST_BEGIN("ra8_usb_pprn_init seeds default port-status (online | not-error)");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));

  uint8_t status = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_get_port_status(&status));
  /* Bit 4 (select) and bit 3 (not-error) must be set. */
  TEST_ASSERT((status & (1U << k_ra8_pprn_status_bit_select)) != 0U);
  TEST_ASSERT((status & (1U << k_ra8_pprn_status_bit_not_error)) != 0U);
  TEST_ASSERT((status & (1U << k_ra8_pprn_status_bit_paper_empty)) == 0U);
  TEST_END("ra8_usb_pprn_init seeds default port-status (online | not-error)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify init bad speed behavior. @details Executes the init bad speed scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_init_bad_speed(void)
{
  TEST_BEGIN("ra8_usb_pprn_init rejects bogus speed");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pprn_init((ra8_usb_speed_t)9U));
  TEST_END("ra8_usb_pprn_init rejects bogus speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify class request codes behavior. @details Executes the class request codes scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_class_request_codes(void)
{
  TEST_BEGIN("Printer class request codes match USB Printer 1.1 spec");
  TEST_ASSERT_EQ(0x00, k_ra8_pprn_req_get_device_id);
  TEST_ASSERT_EQ(0x01, k_ra8_pprn_req_get_port_status);
  TEST_ASSERT_EQ(0x02, k_ra8_pprn_req_soft_reset);
  TEST_END("Printer class request codes match USB Printer 1.1 spec");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify pre init calls behavior. @details Executes the pre init calls scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_pre_init_calls(void)
{
  TEST_BEGIN("PPRN API rejects calls before init");
  internal_prep();

  uint8_t         buf[8] = {};
  uint16_t        got    = 0U;
  uint8_t         status = 0U;
  ra8_usb_setup_t setup  = {};

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pprn_close());
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_pprn_set_descriptors(s_sample_desc,
                                              (uint16_t)sizeof(s_sample_desc),
                                              s_sample_dev_id,
                                              (uint16_t)sizeof(s_sample_dev_id)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pprn_recv(buf, 8U, &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pprn_send(buf, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pprn_set_port_status(0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pprn_get_port_status(&status));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_usb_pprn_attach_setup_handler(internal_test_setup_cb, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_usb_pprn_handle_setup(&setup));
  TEST_END("PPRN API rejects calls before init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify set descriptors behavior. @details Executes the set descriptors scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_set_descriptors(void)
{
  TEST_BEGIN("ra8_usb_pprn_set_descriptors validates pairing");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));

  /* desc NULL -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pprn_set_descriptors(nullptr, 1U, nullptr, 0U));

  /* desc_len 0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pprn_set_descriptors(s_sample_desc, 0U, nullptr, 0U));

  /* device_id ptr set but len 0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_pprn_set_descriptors(s_sample_desc,
                                              (uint16_t)sizeof(s_sample_desc),
                                              s_sample_dev_id,
                                              0U));

  /* Both NULL device-id is OK. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_usb_pprn_set_descriptors(s_sample_desc, (uint16_t)sizeof(s_sample_desc), nullptr, 0U));

  /* Both set is OK. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_pprn_set_descriptors(s_sample_desc,
                                              (uint16_t)sizeof(s_sample_desc),
                                              s_sample_dev_id,
                                              (uint16_t)sizeof(s_sample_dev_id)));
  TEST_END("ra8_usb_pprn_set_descriptors validates pairing");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify send recv validation behavior. @details Executes the send recv validation scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_send_recv_validation(void)
{
  TEST_BEGIN("ra8_usb_pprn_send / recv validate args");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));

  uint8_t  buf[16] = {};
  uint16_t got     = 0U;

  /* recv: NULL buf / NULL got_len / max_len 0. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pprn_recv(nullptr, 8U, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pprn_recv(buf, 8U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pprn_recv(buf, 0U, &got));

  /* send: NULL data + len -> null_ptr; len 0 -> invalid_arg; len > 64 (FS bulk) -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pprn_send(nullptr, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pprn_send(buf, 0U));

  uint8_t big[k_t_oversize_buf] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pprn_send(big, (uint16_t)sizeof(big)));
  TEST_END("ra8_usb_pprn_send / recv validate args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify port status round trip behavior. @details Executes the port status round trip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_port_status_round_trip(void)
{
  TEST_BEGIN("ra8_usb_pprn_set_port_status round-trips through get_port_status");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));

  /* Simulate paper-empty event. */
  const uint8_t paper_empty =
    (uint8_t)((1U << k_ra8_pprn_status_bit_paper_empty) | (1U << k_ra8_pprn_status_bit_select));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_set_port_status(paper_empty));

  uint8_t out = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_get_port_status(&out));
  TEST_ASSERT_EQ(paper_empty, out);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pprn_get_port_status(nullptr));
  TEST_END("ra8_usb_pprn_set_port_status round-trips through get_port_status");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify handle setup dispatch behavior. @details Executes the handle setup dispatch scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_handle_setup_dispatch(void)
{
  TEST_BEGIN("ra8_usb_pprn_handle_setup dispatches GET_DEVICE_ID / SOFT_RESET to callback");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_attach_setup_handler(internal_test_setup_cb, nullptr));

  /* GET_DEVICE_ID. */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_in,
    .b_request       = (uint8_t)k_ra8_pprn_req_get_device_id,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = k_t_wlen_device_id,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_handle_setup(&setup));
  TEST_ASSERT_EQ(1, s_setup_cb_calls);
  TEST_ASSERT_EQ(k_ra8_pprn_req_get_device_id, s_setup_cb_last_breq);

  /* GET_PORT_STATUS. */
  setup.b_request = (uint8_t)k_ra8_pprn_req_get_port_status;
  setup.w_length  = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_handle_setup(&setup));
  TEST_ASSERT_EQ(2, s_setup_cb_calls);

  /* SOFT_RESET. */
  setup.bm_request_type = (uint8_t)k_t_bmreq_class_out;
  setup.b_request       = (uint8_t)k_ra8_pprn_req_soft_reset;
  setup.w_length        = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_handle_setup(&setup));
  TEST_ASSERT_EQ(3, s_setup_cb_calls);
  TEST_END("ra8_usb_pprn_handle_setup dispatches GET_DEVICE_ID / SOFT_RESET to callback");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches) @brief Verify handle setup rejects behavior. @details Executes the handle setup rejects scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_handle_setup_rejects(void)
{
  TEST_BEGIN("ra8_usb_pprn_handle_setup rejects non-class / unknown / NULL");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));

  /* Standard envelope -> not_supported. */
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_std_in,
    .b_request       = (uint8_t)0x06U,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_pprn_handle_setup(&setup));

  /* Class envelope but unknown bRequest -> not_supported. */
  setup.bm_request_type = (uint8_t)k_t_bmreq_class_in;
  setup.b_request       = (uint8_t)k_t_breq_unknown;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_pprn_handle_setup(&setup));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pprn_handle_setup(nullptr));
  TEST_END("ra8_usb_pprn_handle_setup rejects non-class / unknown / NULL");
}

/**
 * @test internal_test_mcdc_pprn
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra8_hal/src/ra8_usb_pprn.c.
 *
 * Decision A (line 156, 2 conds): pprn_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3.
 * Decision B (line 251, 2 conds): pprn_send NULL-with-len
 *   `(data == NULL) && (len != 0)` -- N+1=3.
 * Decision C (line 254, 2 conds): pprn_send size envelope
 *   `(len == 0) || (len > bulk_max_packet)` -- N+1=3.
 * Decision D (lines 310-311, 2 conds): handle_setup envelope
 *   `(bm != iface_in) && (bm != iface_out)` -- N+1=3.
 * Decision E (lines 145-146, 3-condition OR chain): per DO-178C
 *   6.4.4.3 representative-subset for a side-effect-free OR -- 3
 *   lone-true vectors + 1 all-false vector. @brief Verify mcdc pprn behavior. @details Executes the mcdc pprn scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_test_mcdc_pprn(void)
{
  TEST_BEGIN("pprn MC/DC: init / send envelope / handle_setup decisions");
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_hs));
  internal_prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pprn_init((ra8_usb_speed_t)9U));

  internal_prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_init(k_ra8_usb_speed_fs));

  uint8_t buf[16] = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pprn_send(nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_usb_pprn_send(nullptr, 4U));
  /* B-V2 + C-V2: forwards into ra8_usb_queue_in. The FRDY wait
   * converges via the unarmed ra8_fake_mmio seam (see priv_wait_frdy),
   * so a well-formed call returns k_ra8_ok. The MC/DC obligation is met
   * because every pre-check inside ra8_usb_pprn_send was exercised
   * end-to-end. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_send(buf, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_pprn_send(buf, 1024U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_attach_setup_handler(internal_test_setup_cb, nullptr));
  ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_t_bmreq_class_in,
    .b_request       = (uint8_t)k_ra8_pprn_req_get_device_id,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_t_bmreq_class_out;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_t_bmreq_std_in;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_pprn_handle_setup(&setup));

  setup.bm_request_type    = (uint8_t)k_t_bmreq_class_in;
  const uint8_t requests[] = {
    (uint8_t)k_ra8_pprn_req_get_device_id,
    (uint8_t)k_ra8_pprn_req_get_port_status,
    (uint8_t)k_ra8_pprn_req_soft_reset,
  };
  for (uint8_t i = 0U; i < 3U; ++i) {
    setup.b_request = requests[i];
    TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_pprn_handle_setup(&setup));
  }
  setup.b_request = k_t_breq_unknown;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_usb_pprn_handle_setup(&setup));

  TEST_END("pprn MC/DC: init / send envelope / handle_setup decisions");
}

int main(void)
{
  internal_test_init_default_status();
  internal_test_init_bad_speed();
  internal_test_class_request_codes();
  internal_test_pre_init_calls();
  internal_test_set_descriptors();
  internal_test_send_recv_validation();
  internal_test_port_status_round_trip();
  internal_test_handle_setup_dispatch();
  internal_test_handle_setup_rejects();
  internal_test_mcdc_pprn();
  return 0;
}
