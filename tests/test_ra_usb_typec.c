/**
 * @file test_ra_usb_typec.c
 * @brief Unit tests for the USB Type-C / Power Delivery driver
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_usb_typec_regs.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_usb_typec.h"
#include "unity_minimal.h"

/**
 * @brief Reset MMIO + driver between cases.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_usb_typec_close();
}

static ra_usb_typec_cfg_t default_cfg(void)
{
  return (ra_usb_typec_cfg_t){
    .role           = k_ra_usb_typec_role_sink,
    .pmode          = k_ra_usb_typec_pmode_default,
    .cc_debounce_ms = 100U,
    .pd_debounce_ms = 20U,
  };
}

static void test_init_close_happy(void)
{
  TEST_BEGIN("ra_usb_typec_init/close round-trips and clears PDOWN");
  prep();
  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));
  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();
  /* PDOWN should be cleared after init. */
  TEST_ASSERT_EQ(0, (int)(reg->CCC & ((uint32_t)1U << k_ra_typec_ccc_bit_pdown)));
  /* MEC.GUS should be set so SM is in Unattached.SNK. */
  TEST_ASSERT(0 != (reg->MEC & ((uint32_t)1U << k_ra_typec_mec_bit_gus)));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_close());
  /* After close PDOWN should be set. */
  TEST_ASSERT(0 != (reg->CCC & ((uint32_t)1U << k_ra_typec_ccc_bit_pdown)));
  TEST_END("ra_usb_typec_init/close round-trips and clears PDOWN");
}

static void test_init_null_and_bad_args(void)
{
  TEST_BEGIN("ra_usb_typec_init rejects NULL + invalid role/pmode");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_typec_init(nullptr));

  ra_usb_typec_cfg_t bad = default_cfg();
  bad.role               = (ra_usb_typec_role_t)9U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_typec_init(&bad));

  bad       = default_cfg();
  bad.pmode = (ra_usb_typec_pmode_t)9U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_typec_init(&bad));
  TEST_END("ra_usb_typec_init rejects NULL + invalid role/pmode");
}

static void test_double_init_fails(void)
{
  TEST_BEGIN("ra_usb_typec_init twice without close fails");
  prep();
  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_typec_init(&cfg));
  TEST_END("ra_usb_typec_init twice without close fails");
}

static void test_set_role_transitions(void)
{
  TEST_BEGIN("ra_usb_typec_set_role flips MEC.MODE between sink and source");
  prep();
  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));
  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();
  /* Sink -> MODE bit set. */
  TEST_ASSERT(0 != (reg->MEC & ((uint32_t)1U << k_ra_typec_mec_bit_mode)));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_set_role(k_ra_usb_typec_role_source));
  TEST_ASSERT_EQ(0, (int)(reg->MEC & ((uint32_t)1U << k_ra_typec_mec_bit_mode)));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_set_role(k_ra_usb_typec_role_drp));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_typec_set_role((ra_usb_typec_role_t)42U));
  TEST_END("ra_usb_typec_set_role flips MEC.MODE between sink and source");
}

static void test_get_orientation_reads_cc(void)
{
  TEST_BEGIN("ra_usb_typec_get_orientation decodes CC1S/CC2S/PLUG");
  prep();
  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));

  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();

  /* No attachment: CC1S = CC2S = 0. */
  reg->TCS                          = 0U;
  ra_usb_typec_orientation_t orient = k_ra_usb_typec_orientation_cc2_attached;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_get_orientation(&orient));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_typec_orientation_unattached, (int32_t)orient);

  /* CC1 attached: CC1S = SNK.Default (1), PLUG=0. */
  reg->TCS = (uint32_t)1U << k_ra_typec_tcs_shift_cc1s;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_get_orientation(&orient));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_typec_orientation_cc1_attached, (int32_t)orient);

  /* CC2 attached: CC2S = SNK.Default (1), PLUG=1. */
  reg->TCS =
    ((uint32_t)1U << k_ra_typec_tcs_shift_cc2s) | ((uint32_t)1U << k_ra_typec_tcs_bit_plug);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_get_orientation(&orient));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_typec_orientation_cc2_attached, (int32_t)orient);
  TEST_END("ra_usb_typec_get_orientation decodes CC1S/CC2S/PLUG");
}

static void test_get_attach_status(void)
{
  TEST_BEGIN("ra_usb_typec_get_attach_status decodes TCS.CNS");
  prep();
  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));

  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();

  reg->TCS                        = 0U;
  ra_usb_typec_attach_status_t st = k_ra_usb_typec_attach_present_attached;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_get_attach_status(&st));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_typec_attach_nothing, (int32_t)st);

  /* CNS = Attached.SNK Default (4) -> bits b6..b4 set as 4 << 4 = 0x40. */
  reg->TCS = (uint32_t)4U << k_ra_typec_tcs_shift_cns;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_get_attach_status(&st));
  TEST_ASSERT_EQ((int32_t)k_ra_usb_typec_attach_present_attached, (int32_t)st);
  TEST_END("ra_usb_typec_get_attach_status decodes TCS.CNS");
}

static void test_pd_build_header_byte_layout(void)
{
  TEST_BEGIN("ra_usb_typec_pd_build_header lays out PD 3.0 sec 6.2.1.1 bits");
  /* Build an Accept (msg-type=3), DFP, PD3.0, source, msg_id=5, 0 obj. */
  const uint16_t hdr = ra_usb_typec_pd_build_header(k_ra_usb_typec_pd_msg_accept,
                                                    k_ra_usb_typec_pd_role_source_or_dfp,
                                                    k_ra_usb_typec_pd_spec_rev_3_0,
                                                    k_ra_usb_typec_pd_role_source_or_dfp,
                                                    (uint8_t)5U,
                                                    (uint8_t)0U,
                                                    (uint8_t)0U);
  /* msg-type 3 in low 5 bits, port-data-role bit5=1, spec-rev=2 in b7..6,
   * port-power-role bit8=1, msg-id 5 in b11..9. */
  const uint16_t expected = (uint16_t)0x3U          /* msg type */
                            | (uint16_t)(1U << 5U)  /* PDR */
                            | (uint16_t)(2U << 6U)  /* spec rev 3.0 */
                            | (uint16_t)(1U << 8U)  /* PPR */
                            | (uint16_t)(5U << 9U); /* msg id */
  TEST_ASSERT_EQ((int32_t)expected, (int32_t)hdr);
  TEST_END("ra_usb_typec_pd_build_header lays out PD 3.0 sec 6.2.1.1 bits");
}

static void test_pd_send_message_kicks_tx(void)
{
  TEST_BEGIN("ra_usb_typec_pd_send_message stages header + sets tx_start");
  prep();
  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));

  ra_usb_typec_pd_msg_t msg = {};
  msg.header                = ra_usb_typec_pd_build_header(k_ra_usb_typec_pd_msg_ps_rdy,
                                                           k_ra_usb_typec_pd_role_source_or_dfp,
                                                           k_ra_usb_typec_pd_spec_rev_3_0,
                                                           k_ra_usb_typec_pd_role_source_or_dfp,
                                                           (uint8_t)0U,
                                                           (uint8_t)0U,
                                                           (uint8_t)0U);
  msg.num_objects           = 0U;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_pd_send_message(&msg));
  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();
  TEST_ASSERT(0 != (reg->PDFC & ((uint32_t)1U << k_ra_typec_pdfc_bit_tx_start)));
  /* Header LSBs visible in PDFD shadow. */
  TEST_ASSERT_EQ((int32_t)msg.header, (int32_t)(reg->PDFD & 0xFFFFU));
  TEST_END("ra_usb_typec_pd_send_message stages header + sets tx_start");
}

static void test_pd_send_rejects_too_many_objects(void)
{
  TEST_BEGIN("ra_usb_typec_pd_send_message rejects num_objects > 7");
  prep();
  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));

  ra_usb_typec_pd_msg_t msg = {};
  msg.num_objects           = 8U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_typec_pd_send_message(&msg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_typec_pd_send_message(nullptr));
  TEST_END("ra_usb_typec_pd_send_message rejects num_objects > 7");
}

static void test_pd_recv_no_data_then_happy(void)
{
  TEST_BEGIN("ra_usb_typec_pd_recv_message: empty FIFO + happy path");
  prep();
  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));

  ra_usb_typec_pd_msg_t out = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_no_data, (int32_t)ra_usb_typec_pd_recv_message(&out));

  /* Stage a control message: msg_type=Accept, num_objects=0. Set rx_avail. */
  volatile r_usb_typec_regs_t* reg = ra_usb_typec_regs();
  const uint16_t hdr = ra_usb_typec_pd_build_header(k_ra_usb_typec_pd_msg_accept,
                                                    k_ra_usb_typec_pd_role_sink_or_ufp,
                                                    k_ra_usb_typec_pd_spec_rev_3_0,
                                                    k_ra_usb_typec_pd_role_sink_or_ufp,
                                                    (uint8_t)0U,
                                                    (uint8_t)0U,
                                                    (uint8_t)0U);
  reg->PDFD          = (uint32_t)hdr;
  reg->PDFS |= ((uint32_t)1U << k_ra_typec_pdfs_bit_rx_avail);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_pd_recv_message(&out));
  TEST_ASSERT_EQ((int32_t)hdr, (int32_t)out.header);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)out.num_objects);
  /* rx_avail should be cleared on drain. */
  TEST_ASSERT_EQ(0, (int)(reg->PDFS & ((uint32_t)1U << k_ra_typec_pdfs_bit_rx_avail)));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_typec_pd_recv_message(nullptr));
  TEST_END("ra_usb_typec_pd_recv_message: empty FIFO + happy path");
}

static void s_dummy_cb(const ra_usb_typec_event_t* ev)
{
  (void)ev;
}

static void test_set_callback(void)
{
  TEST_BEGIN("ra_usb_typec_set_callback installs / clears");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_typec_set_callback(s_dummy_cb, nullptr));

  const ra_usb_typec_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_init(&cfg));
  uint32_t marker = 0xCAFEBABEU;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_set_callback(s_dummy_cb, &marker));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_typec_set_callback(nullptr, nullptr));
  TEST_END("ra_usb_typec_set_callback installs / clears");
}

static void test_pre_init_guards(void)
{
  TEST_BEGIN("Type-C API rejects calls before init");
  prep();
  ra_usb_typec_orientation_t orient = k_ra_usb_typec_orientation_unattached;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_typec_get_orientation(&orient));

  ra_usb_typec_attach_status_t at = k_ra_usb_typec_attach_nothing;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_typec_get_attach_status(&at));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_usb_typec_set_role(k_ra_usb_typec_role_sink));

  ra_usb_typec_pd_msg_t msg = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_typec_pd_send_message(&msg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_typec_pd_recv_message(&msg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_typec_close());

  /* NULL-arg checks before init still hit the NULL guard first. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_typec_get_orientation(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_typec_get_attach_status(nullptr));
  TEST_END("Type-C API rejects calls before init");
}

int32_t main(void)
{
  test_init_close_happy();
  test_init_null_and_bad_args();
  test_double_init_fails();
  test_set_role_transitions();
  test_get_orientation_reads_cc();
  test_get_attach_status();
  test_pd_build_header_byte_layout();
  test_pd_send_message_kicks_tx();
  test_pd_send_rejects_too_many_objects();
  test_pd_recv_no_data_then_happy();
  test_set_callback();
  test_pre_init_guards();
  (void)fprintf(stderr, "[OK ] test_ra_usb_typec.c\n");
  return 0;
}
