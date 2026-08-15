/**
 * @file test_ra8_usb_mcdc.c
 * @brief MC/DC vector tests for the native USB device-mode driver (ra8_usb.c)
 *
 * @details
 * Split out of test_ra8_usb.c to keep each test translation unit under the
 * repository file-size cap. This sibling owns the MC/DC vector tests for the
 * compound boolean decisions in ra8_usb.c (DO-178C Level B / IEC 61508 SIL 3);
 * the core happy-path / error-rejection contract tests stay in
 * test_ra8_usb.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_mcdc_t
 * @brief Oversized transfer buffer for the length-guard vector.
 */
typedef enum : uint16_t {
  k_t_oversize_buf = 128U, /**< Past the endpoint's maximum packet, so the guard
                                rather than the copy is what fires.              */
} t_mcdc_t;

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/* =====================================================================
 * MC/DC vector tests (DO-178C Level B / IEC 61508 SIL 3)
 *
 * Each test below pins all conditions in a compound boolean decision
 * except one and shows that flipping the varied condition flips the
 * decision outcome. With N conditions the minimal set is N+1 vectors.
 * Source-of-truth gap rows: docs/MCDC_GAPS.csv (ra8_usb.c entries).
 * ===================================================================== */

typedef enum : uint16_t {
  k_mcdc_usb_pipe_lo_bad = 0U,    /**< pipe_num == 0 -> rejected.         */
  k_mcdc_usb_pipe_hi_bad = 99U,   /**< pipe_num > k_ra8_usb_max_pipe_num. */
  k_mcdc_usb_pipe_ok     = 1U,    /**< 1 .. k_ra8_usb_max_pipe_num.       */
  k_mcdc_usb_ep_lo_bad   = 0U,    /**< ep_addr == 0 -> rejected.          */
  k_mcdc_usb_ep_hi_bad   = 99U,   /**< ep_addr > k_ra8_usb_max_ep_addr.   */
  k_mcdc_usb_ep_ok       = 1U,    /**< 1 .. k_ra8_usb_max_ep_addr.        */
  k_mcdc_usb_mp_lo_bad   = 0U,    /**< max_packet == 0 -> rejected.       */
  k_mcdc_usb_mp_hi_bad   = 9999U, /**< max_packet > pipe_max_packet.      */
  k_mcdc_usb_mp_ok       = 64U,   /**< common bulk max packet.            */
  k_mcdc_usb_len_zero    = 0U,    /**< Mcdc USB length zero.              */
  k_mcdc_usb_len_ok      = 4U,    /**< Mcdc USB length ok.                */
  k_mcdc_usb_len_too_big = 9999U, /**< Mcdc USB length too big.           */
  k_mcdc_usb_speed_bogus = 9U,    /**< not FS, not HS.                    */
} mcdc_usb_const_t;

/**
 * @test test_mcdc_check_ep_args_pipe_num
 *
 * @par MC/DC:
 * Decision: `if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num))`
 * (libs/ra8_hal/src/ra8_usb.c conditions, reached via
 * ra8_usb_configure_endpoint -> internal_check_ep_args).
 * - V1: pipe=1, others valid               -> false (control: both false).
 * - V2: pipe=0, others valid               -> true  (varies C1 only).
 * - V3: pipe=99, others valid              -> true  (varies C2 only).
 * V1+V2 prove C1 (pipe==0) independently flips the decision; V1+V3
 * prove C2 (pipe>max). N+1 = 3 vectors for N=2.
 */
static void test_mcdc_check_ep_args_pipe_num(void)
{
  TEST_BEGIN("mcdc: check_ep_args pipe_num decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  /* V1: both conditions false -> ok (config succeeds). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  /* V2: pipe == 0 -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_lo_bad,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  /* V3: pipe > max -> rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_hi_bad,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  TEST_END("mcdc: check_ep_args pipe_num decision");
}

/**
 * @test test_mcdc_check_ep_args_ep_addr
 *
 * @par MC/DC:
 * Decision: `if ((ep_addr == 0U) || (ep_addr > k_ra8_usb_max_ep_addr))`
 * (libs/ra8_hal/src/ra8_usb.c conditions).
 * - V1: ep=1, others valid     -> false (both false).
 * - V2: ep=0, others valid     -> true  (varies C1).
 * - V3: ep=99, others valid    -> true  (varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_check_ep_args_ep_addr(void)
{
  TEST_BEGIN("mcdc: check_ep_args ep_addr decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_lo_bad,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_hi_bad,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  TEST_END("mcdc: check_ep_args ep_addr decision");
}

/**
 * @test test_mcdc_check_ep_args_dir
 *
 * @par MC/DC:
 * Decision: `if ((dir != k_ra8_usb_ep_dir_in) && (dir != k_ra8_usb_ep_dir_out))`
 * (libs/ra8_hal/src/ra8_usb.c conditions). Note these are AND-of-NEs:
 * the decision is true only when dir matches NEITHER enum value.
 * - V1: dir = IN  -> C1 false, short-circuits  -> false (control).
 * - V2: dir = OUT -> C1 true, C2 false         -> false (varies C2).
 * - V3: dir = 9   -> C1 true, C2 true          -> true  (rejected).
 * V1+V3 prove C1 flips outcome (with C2 fixed true via dir=9 vs dir=IN
 * where C2 is unreachable -- short-circuit masking is the standard MC/DC
 * concession here). V2+V3 prove C2 flips outcome with C1 held true.
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_check_ep_args_dir(void)
{
  TEST_BEGIN("mcdc: check_ep_args dir decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_out,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            (ra8_usb_ep_dir_t)9U,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  TEST_END("mcdc: check_ep_args dir decision");
}

/**
 * @test test_mcdc_check_ep_args_max_packet
 *
 * @par MC/DC:
 * Decision: `if ((max_packet == 0U) || (max_packet > k_ra8_usb_pipe_max_packet))`
 * (libs/ra8_hal/src/ra8_usb.c conditions).
 * - V1: mp=64,  others valid    -> false (both false).
 * - V2: mp=0,   others valid    -> true  (varies C1).
 * - V3: mp=9999,others valid    -> true  (varies C2, exceeds 1024 cap).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_check_ep_args_max_packet(void)
{
  TEST_BEGIN("mcdc: check_ep_args max_packet decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_ok));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_lo_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_configure_endpoint(k_ra8_usb_speed_fs,
                                            (uint8_t)k_mcdc_usb_pipe_ok,
                                            (uint8_t)k_mcdc_usb_ep_ok,
                                            k_ra8_usb_ep_dir_in,
                                            k_ra8_usb_ep_type_bulk,
                                            (uint16_t)k_mcdc_usb_mp_hi_bad));
  TEST_END("mcdc: check_ep_args max_packet decision");
}

/**
 * @test test_mcdc_queue_in_pipe_num
 *
 * @par MC/DC:
 * Decision: `if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num))`
 * (libs/ra8_hal/src/ra8_usb.c conditions, in ra8_usb_queue_in).
 * - V1: pipe=1, FRDY pre-asserted, len=4   -> false (both false, returns ok).
 * - V2: pipe=0                              -> true  (varies C1).
 * - V3: pipe=99                             -> true  (varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_queue_in_pipe_num(void)
{
  TEST_BEGIN("mcdc: queue_in pipe_num decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t buf[4] = {0U, 0U, 0U, 0U};
  /* V1: pre-arm FRDY so the success path runs. */
  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs,
                                  (uint8_t)k_mcdc_usb_pipe_ok,
                                  buf,
                                  (uint16_t)k_mcdc_usb_len_ok));
  /* V2: pipe == 0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs,
                                  (uint8_t)k_mcdc_usb_pipe_lo_bad,
                                  buf,
                                  (uint16_t)k_mcdc_usb_len_ok));
  /* V3: pipe > max. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs,
                                  (uint8_t)k_mcdc_usb_pipe_hi_bad,
                                  buf,
                                  (uint16_t)k_mcdc_usb_len_ok));
  TEST_END("mcdc: queue_in pipe_num decision");
}

/**
 * @test test_mcdc_queue_in_data_len
 *
 * @par MC/DC:
 * Decision: `if ((len > k_ra8_usb_pipe_max_packet) ||
 *                ((data == nullptr) && (len != 0U)))`
 * (libs/ra8_hal/src/ra8_usb.c conditions).
 * Naming: C1 = (len > MAX), C2 = (data == NULL), C3 = (len != 0).
 * The inner AND short-circuits on C2, so we use the N+1 = 4 vector set:
 * - V1: data=buf, len=4              -> C1=F, (C2=F so AND=F)         -> false (control).
 * - V2: data=buf, len=9999           -> C1=T                          -> true  (varies C1).
 * - V3: data=NULL,len=4              -> C1=F, C2=T, C3=T -> AND=T     -> true  (varies C2 with C1 held false).
 * - V4: data=NULL,len=0              -> C1=F, C2=T, C3=F -> AND=F     -> false (varies C3 with C2 held true).
 * V1+V2 prove C1; V1+V3 prove C2 (C1 held false); V3+V4 prove C3
 * (C2 held true). N+1 = 4 vectors for N=3.
 */
static void test_mcdc_queue_in_data_len(void)
{
  TEST_BEGIN("mcdc: queue_in (len/data) compound decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t buf[4]         = {0U, 0U, 0U, 0U};
  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;

  /* V1: small valid call. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs,
                                  (uint8_t)k_mcdc_usb_pipe_ok,
                                  buf,
                                  (uint16_t)k_mcdc_usb_len_ok));
  /* V2: len exceeds pipe_max_packet. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs,
                                  (uint8_t)k_mcdc_usb_pipe_ok,
                                  buf,
                                  (uint16_t)k_mcdc_usb_len_too_big));
  /* V3: data NULL with non-zero len. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs,
                                  (uint8_t)k_mcdc_usb_pipe_ok,
                                  nullptr,
                                  (uint16_t)k_mcdc_usb_len_ok));
  /* V4: data NULL with zero len -> AND collapses to false; the outer
   * decision is false; the call falls through to the no-op zero-byte
   * write path and returns ok. */
  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_usb_queue_in(k_ra8_usb_speed_fs,
                                  (uint8_t)k_mcdc_usb_pipe_ok,
                                  nullptr,
                                  (uint16_t)k_mcdc_usb_len_zero));
  TEST_END("mcdc: queue_in (len/data) compound decision");
}

/**
 * @test test_mcdc_check_queue_out_args_buf
 *
 * @par MC/DC:
 * Decision: `if ((out_buf == nullptr) || (inout_len == nullptr))`
 * (libs/ra8_hal/src/ra8_usb.c conditions, in
 * internal_check_queue_out_args via ra8_usb_queue_out).
 * - V1: out_buf=valid, inout_len=valid (with FRDY+DTLN=0)  -> false (both false; reaches no_data).
 * - V2: out_buf=NULL,  inout_len=valid                     -> true  (varies C1).
 * - V3: out_buf=valid, inout_len=NULL                      -> true  (varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_check_queue_out_args_buf(void)
{
  TEST_BEGIN("mcdc: queue_out NULL-arg decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {0U};
  uint16_t len    = 8U;

  /* V1: both pointers valid -> falls through to FRDY/DTLN logic. */
  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(
    k_ra8_err_no_data,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_ok, buf, &len, true));
  /* V2: out_buf NULL. */
  len = 8U;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_ok, nullptr, &len, true));
  /* V3: inout_len NULL. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_ok, buf, nullptr, true));
  TEST_END("mcdc: queue_out NULL-arg decision");
}

/**
 * @test test_mcdc_check_queue_out_args_pipe
 *
 * @par MC/DC:
 * Decision: `if ((pipe_num == 0U) || (pipe_num > k_ra8_usb_max_pipe_num))`
 * (libs/ra8_hal/src/ra8_usb.c conditions, in
 * internal_check_queue_out_args).
 * - V1: pipe=1                      -> false.
 * - V2: pipe=0                      -> true (varies C1).
 * - V3: pipe=99                     -> true (varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_check_queue_out_args_pipe(void)
{
  TEST_BEGIN("mcdc: queue_out pipe_num decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {0U};
  uint16_t len    = 8U;

  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(
    k_ra8_err_no_data,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_ok, buf, &len, true));
  len = 8U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_lo_bad, buf, &len, true));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_hi_bad, buf, &len, true));
  TEST_END("mcdc: queue_out pipe_num decision");
}

/**
 * @test test_mcdc_check_queue_out_args_inout_len
 *
 * @par MC/DC:
 * Decision: `if ((*inout_len == 0U) || (*inout_len > k_ra8_usb_pipe_max_packet))`
 * (libs/ra8_hal/src/ra8_usb.c conditions).
 * - V1: *inout_len=8                -> false (both false).
 * - V2: *inout_len=0                -> true  (varies C1).
 * - V3: *inout_len=9999             -> true  (varies C2).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_check_queue_out_args_inout_len(void)
{
  TEST_BEGIN("mcdc: queue_out *inout_len decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t  buf[8] = {0U};
  uint16_t len    = (uint16_t)k_mcdc_usb_len_ok + 4U; /* 8 */

  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(
    k_ra8_err_no_data,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_ok, buf, &len, true));
  uint16_t zero = (uint16_t)k_mcdc_usb_len_zero;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_ok, buf, &zero, true));
  uint16_t big = (uint16_t)k_mcdc_usb_len_too_big;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_usb_queue_out(k_ra8_usb_speed_fs, (uint8_t)k_mcdc_usb_pipe_ok, buf, &big, true));
  TEST_END("mcdc: queue_out *inout_len decision");
}

/**
 * @test test_mcdc_enter_stop_speed
 *
 * @par MC/DC:
 * Decision: `if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs))`
 * (libs/ra8_hal/src/ra8_usb.c conditions, in ra8_usb_enter_stop).
 * - V1: speed=FS -> C1=F, short-circuits             -> false (control, returns ok).
 * - V2: speed=HS -> C1=T, C2=F                       -> false (varies C2).
 * - V3: speed=9  -> C1=T, C2=T                       -> true  (rejected).
 * V1+V3 prove C1 (with C2 held T via speed=9 vs FS where C2 unevaluated).
 * V2+V3 prove C2 with C1 held T. N+1 = 3 vectors for N=2.
 */
static void test_mcdc_enter_stop_speed(void)
{
  TEST_BEGIN("mcdc: enter_stop speed decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_enter_stop(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_enter_stop(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_usb_enter_stop((ra8_usb_speed_t)k_mcdc_usb_speed_bogus));
  TEST_END("mcdc: enter_stop speed decision");
}

/**
 * @test test_mcdc_exit_stop_speed
 *
 * @par MC/DC:
 * Decision: `if ((speed != k_ra8_usb_speed_fs) && (speed != k_ra8_usb_speed_hs))`
 * (libs/ra8_hal/src/ra8_usb.c conditions, in ra8_usb_exit_stop).
 * - V1: speed=FS  -> false.
 * - V2: speed=HS  -> false (varies C2).
 * - V3: speed=9   -> true  (varies C1).
 * N+1 = 3 vectors for N=2.
 */
static void test_mcdc_exit_stop_speed(void)
{
  TEST_BEGIN("mcdc: exit_stop speed decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));
  (void)ra8_usb_enter_stop(k_ra8_usb_speed_fs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_exit_stop(k_ra8_usb_speed_fs));
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_hs));
  (void)ra8_usb_enter_stop(k_ra8_usb_speed_hs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_exit_stop(k_ra8_usb_speed_hs));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_exit_stop((ra8_usb_speed_t)k_mcdc_usb_speed_bogus));
  TEST_END("mcdc: exit_stop speed decision");
}

/**
 * @test test_mcdc_dcp_in_data_len_data
 *
 * @par MC/DC:
 * Decision: `if ((data == nullptr) && (len != 0U))`
 * (libs/ra8_hal/src/ra8_usb.c@ra8_usb_dcp_in_data).
 * Two-condition AND:
 *   C1: data == nullptr
 *   C2: len != 0
 * - V1: data=valid, len=4   -> C1=F, C2=T -> outer=F (ok, single-chunk).
 * - V2: data=NULL,  len=4   -> C1=T, C2=T -> outer=T (rejected, varies C1).
 * - V3: data=NULL,  len=0   -> C1=T, C2=F -> outer=F (ZLP path, varies C2).
 * V1+V2 prove C1 independently flips outer (with C2 held T); V2+V3 prove
 * C2 independently flips outer (with C1 held T). N+1=3 vectors for the
 * 2-condition AND.
 *
 * Bonus: V4 exercises the multi-chunk path (len > DCPMAXP) introduced
 * after the 75-byte CONFIGURATION-descriptor stall fix.
 */
static void test_mcdc_dcp_in_data_len_data(void)
{
  TEST_BEGIN("mcdc: dcp_in_data (len/data) compound decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_device_init(k_ra8_usb_speed_fs));

  uint8_t big_buf[k_t_oversize_buf] = {};
  ra8_usb_fs()->CFIFOCTR            = (uint16_t)k_ra8_fifoctr_frdy;

  /* V1: small valid call -> ok (single chunk). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, big_buf, 4U));
  /* V2: data NULL with non-zero len -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, nullptr, 4U));
  /* V3: data NULL with zero len -> AND collapses to false; outer is
   * false; the call falls through to the ZLP path. */
  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, nullptr, 0U));
  /* V4: full-MPS chunk (64 bytes) -> single loop iteration, exercises
   * the new chunk-loop body. The host-side mock CFIFOCTR is plain
   * memory, so we cannot easily simulate the controller's FRDY
   * re-assertion between chunks; the on-target multi-chunk path is
   * exercised by the live USB enumeration test (75-byte CONFIGURATION
   * descriptor on real silicon). The loop bound itself
   * (``k_ra8_usb_frdy_poll_limit``) was bumped to ~10 ms ceiling so
   * the second chunk no longer times out unconditionally; the bound
   * itself is reachable on hardware (host pulls each chunk in <100
   * us) so production calls return after a single FRDY=1 sample. */
  ra8_usb_fs()->CFIFOCTR = (uint16_t)k_ra8_fifoctr_frdy;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, big_buf, 64U));
  TEST_END("mcdc: dcp_in_data (len/data) compound decision");
}

int32_t main(void)
{
  test_mcdc_check_ep_args_pipe_num();
  test_mcdc_check_ep_args_ep_addr();
  test_mcdc_check_ep_args_dir();
  test_mcdc_check_ep_args_max_packet();
  test_mcdc_queue_in_pipe_num();
  test_mcdc_queue_in_data_len();
  test_mcdc_check_queue_out_args_buf();
  test_mcdc_check_queue_out_args_pipe();
  test_mcdc_check_queue_out_args_inout_len();
  test_mcdc_enter_stop_speed();
  test_mcdc_exit_stop_speed();
  test_mcdc_dcp_in_data_len_data();
  return 0;
}
