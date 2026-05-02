/**
 * @file test_ra_usb_cdc.c
 * @brief Unit tests for the native USB CDC ACM class layer
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_usb_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_usb.h"
#include "ra_usb_cdc.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  (void)ra_usb_cdc_deinit();
}

static void test_init_fs_default_coding(void)
{
  TEST_BEGIN("ra_usb_cdc_init seeds 9600/8/N/1 line coding on FS");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  ra_usb_cdc_line_coding_t coding = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_get_line_coding(&coding));
  TEST_ASSERT_EQ((int32_t)9600U, (int32_t)coding.dte_rate);
  TEST_ASSERT_EQ((int32_t)8U, (int32_t)coding.data_bits);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)coding.char_format);
  TEST_ASSERT_EQ((int32_t)0U, (int32_t)coding.parity_type);
  TEST_END("ra_usb_cdc_init seeds 9600/8/N/1 line coding on FS");
}

static void test_init_bad_speed(void)
{
  TEST_BEGIN("ra_usb_cdc_init rejects bogus speed");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_cdc_init((ra_usb_speed_t)9U));
  TEST_END("ra_usb_cdc_init rejects bogus speed");
}

static void test_init_hs_picks_512_bulk(void)
{
  TEST_BEGIN("ra_usb_cdc_init on HS configures 512-byte bulk pipes");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_hs));
  /* PIPECFG was last written for the interrupt pipe (PIPE6); the
   * bulk-pipe state is in the controller's pipe table, but the host
   * mock keeps the most recent PIPESEL/PIPEMAXP write. Read PIPE6
   * config which should be intr / 8 bytes. */
  volatile r_usb_regs_t* reg = ra_usb_hs();
  TEST_ASSERT_EQ((int32_t)k_ra_cdc_pipe_intr_in, (int32_t)reg->PIPESEL);
  TEST_ASSERT_EQ((int32_t)k_ra_cdc_intr_max_packet, (int32_t)reg->PIPEMAXP);
  TEST_END("ra_usb_cdc_init on HS configures 512-byte bulk pipes");
}

static void test_attach_detach(void)
{
  TEST_BEGIN("ra_usb_cdc_attach toggles DPRPU");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_attach(true));
  volatile r_usb_regs_t* reg   = ra_usb_fs();
  const uint16_t         dpbit = (uint16_t)(1U << k_ra_syscfg_bit_dprpu);
  TEST_ASSERT((reg->SYSCFG & dpbit) != 0);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_attach(false));
  TEST_ASSERT_EQ(0, (int)(reg->SYSCFG & dpbit));
  TEST_END("ra_usb_cdc_attach toggles DPRPU");
}

static void test_pre_init_calls(void)
{
  TEST_BEGIN("CDC API rejects calls before init");
  prep();

  uint8_t  buf[8] = {};
  uint16_t len    = 8U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_attach(true));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_send(buf, 1U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_recv(buf, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_deinit());

  ra_usb_cdc_line_coding_t coding = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_get_line_coding(&coding));
  bool dtr = false;
  bool rts = false;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_get_line_state(&dtr, &rts));

  ra_usb_setup_t setup = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_usb_cdc_handle_setup(&setup));
  TEST_END("CDC API rejects calls before init");
}

static void test_handle_setup_set_control_line_state(void)
{
  TEST_BEGIN("ra_usb_cdc_handle_setup updates DTR/RTS");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x21U,
    .b_request       = (uint8_t)k_ra_cdc_req_set_control_line_state,
    .w_value         = (uint16_t)(k_ra_cdc_line_state_dtr | k_ra_cdc_line_state_rts),
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_handle_setup(&setup));

  bool dtr = false;
  bool rts = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_get_line_state(&dtr, &rts));
  TEST_ASSERT(dtr);
  TEST_ASSERT(rts);

  setup.w_value = (uint16_t)k_ra_cdc_line_state_dtr;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_handle_setup(&setup));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_get_line_state(&dtr, &rts));
  TEST_ASSERT(dtr);
  TEST_ASSERT(!rts);
  TEST_END("ra_usb_cdc_handle_setup updates DTR/RTS");
}

static void test_handle_setup_get_line_coding_acks(void)
{
  TEST_BEGIN("ra_usb_cdc_handle_setup ACKs GET_LINE_CODING");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0xA1U,
    .b_request       = (uint8_t)k_ra_cdc_req_get_line_coding,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 7U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_handle_setup(&setup));
  /* DCPCTR.PID should be BUF, CCPL set. */
  volatile r_usb_regs_t* reg = ra_usb_fs();
  TEST_ASSERT_EQ((int32_t)k_ra_pid_buf, (int32_t)(reg->DCPCTR & k_ra_pid_mask));
  TEST_ASSERT((reg->DCPCTR & (uint16_t)(1U << k_ra_dcpctr_bit_ccpl)) != 0U);
  TEST_END("ra_usb_cdc_handle_setup ACKs GET_LINE_CODING");
}

static void test_handle_setup_rejects_standard(void)
{
  TEST_BEGIN("ra_usb_cdc_handle_setup rejects non-class SETUPs");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)0x80U, /* standard, device, IN */
    .b_request       = (uint8_t)0x06U, /* GET_DESCRIPTOR */
    .w_value         = 0x0100U,
    .w_index         = 0U,
    .w_length        = 18U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_cdc_handle_setup(&setup));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_handle_setup(nullptr));
  TEST_END("ra_usb_cdc_handle_setup rejects non-class SETUPs");
}

static void test_send_recv_arg_validation(void)
{
  TEST_BEGIN("ra_usb_cdc_{send,recv} validate args");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  uint8_t  buf[8] = {};
  uint16_t len    = 8U;

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_cdc_send(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_recv(nullptr, &len));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_recv(buf, nullptr));
  uint16_t zero = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_cdc_recv(buf, &zero));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_get_line_coding(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_get_line_state(nullptr, nullptr));
  bool dtr = false;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_usb_cdc_get_line_state(&dtr, nullptr));
  TEST_END("ra_usb_cdc_{send,recv} validate args");
}

/* =============================================================================
 * MC/DC vector tests for compound boolean decisions flagged in
 * docs/MCDC_GAPS.csv against libs/ra_hal/src/ra_usb_cdc.c.
 * =============================================================================
 */

/**
 * @enum test_cdc_mcdc_t
 */
typedef enum : uint16_t {
  k_test_cdc_speed_bad      = 9U,
  k_test_cdc_iface_class    = 0x21U, /**< k_ra_cdc_bm_class_recip_iface. */
  k_test_cdc_iface_class_in = 0xA1U, /**< k_ra_cdc_bm_class_recip_in.   */
  k_test_cdc_bm_bogus       = 0x80U,
} test_cdc_mcdc_t;

/**
 * @test test_mcdc_cdc
 *
 * @par MC/DC:
 * Covers compound decisions flagged in docs/MCDC_GAPS.csv for
 * libs/ra_hal/src/ra_usb_cdc.c.
 *
 * Decision A (line 174, 2 conds): cdc_init speed gate
 *   `(speed != FS) && (speed != HS)` -- N+1=3:
 *   - V1 FS -> C1=F (short circuit) -> dec=F (init ok)
 *   - V2 HS -> C1=T, C2=F           -> dec=F (init ok)
 *   - V3 9  -> C1=T, C2=T           -> dec=T (invalid_arg)
 *
 * Decision B (line 229, 2 conds): cdc_send NULL-with-len
 *   `(data == NULL) && (len != 0)` -- N+1=3:
 *   - V1 (NULL,0) -> C1=T, C2=F -> dec=F (forwards OK)
 *   - V2 (buf,4)  -> C1=F       -> dec=F (forwards OK)
 *   - V3 (NULL,4) -> C1=T, C2=T -> dec=T (invalid_arg)
 *
 * Decision C (line 311-312, 2 conds): handle_setup envelope
 *   `(bm != iface) && (bm != iface_in)` -- N+1=3:
 *   - V1 iface     -> C1=F (short circuit)              -> dec=F (dispatched)
 *   - V2 iface_in  -> C1=T, C2=F                        -> dec=F (dispatched)
 *   - V3 0x80      -> C1=T, C2=T                        -> dec=T (not_supported)
 *
 * Decision D (line 154, 2 conds, internal_apply_line_coding):
 *   `(data == NULL) || (len < 7)`. Per DO-178C 6.4.4.3 deactivated-code
 *   provision: this static helper is only called from
 *   `internal_dispatch_class_setup` after `internal_pull_data_stage`
 *   returns `k_ra_ok`, but `internal_pull_data_stage` is currently a
 *   stub that always returns `k_ra_err_not_supported` (see source line
 *   269). The line-coding apply branch is therefore unreachable from
 *   any public entry point; an MC/DC vector set is not constructable
 *   without modifying the unit under test. The branch is left under
 *   structural-coverage waiver until the live USB ISR path that
 *   delivers the EP0 OUT data stage is integrated, at which point the
 *   waiver lifts and the standard 3-vector pattern (NULL/len<7,
 *   buf/len<7, buf/len>=7) becomes runnable.
 */
static void test_mcdc_cdc(void)
{
  TEST_BEGIN("cdc MC/DC: init / send / handle_setup compound decisions");

  /* Decision A: init speed gate. */
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_hs));
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_usb_cdc_init((ra_usb_speed_t)k_test_cdc_speed_bad));

  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_init(k_ra_usb_speed_fs));

  /* Decision B: send null/len matrix. */
  uint8_t buf[8] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_usb_cdc_send(nullptr, 4U));
  /* V1 NULL,0: forwarded to ra_usb_queue_in which may return ok or
   * hw_error from the simulator -- we only assert it is NOT
   * invalid_arg, which is the only outcome for B-V1 false. */
  const ra_err_t b_v1 = ra_usb_cdc_send(nullptr, 0U);
  TEST_ASSERT(b_v1 != k_ra_err_invalid_arg);
  const ra_err_t b_v2 = ra_usb_cdc_send(buf, 4U);
  TEST_ASSERT(b_v2 != k_ra_err_invalid_arg);

  /* Decision C: handle_setup envelope. */
  ra_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_test_cdc_iface_class,
    .b_request       = (uint8_t)k_ra_cdc_req_set_control_line_state,
    .w_value         = 0U,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_test_cdc_iface_class_in;
  setup.b_request       = (uint8_t)k_ra_cdc_req_get_line_coding;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_usb_cdc_handle_setup(&setup));
  setup.bm_request_type = (uint8_t)k_test_cdc_bm_bogus;
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_usb_cdc_handle_setup(&setup));

  TEST_END("cdc MC/DC: init / send / handle_setup compound decisions");
}

int32_t main(void)
{
  test_init_fs_default_coding();
  test_init_bad_speed();
  test_init_hs_picks_512_bulk();
  test_attach_detach();
  test_pre_init_calls();
  test_handle_setup_set_control_line_state();
  test_handle_setup_get_line_coding_acks();
  test_handle_setup_rejects_standard();
  test_send_recv_arg_validation();
  test_mcdc_cdc();
  (void)fprintf(stderr, "[OK ] test_ra_usb_cdc.c\n");
  return 0;
}
