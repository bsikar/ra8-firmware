/**
 * @file test_ra_iic.c
 * @brief Unit tests for iic.c (polling I2C controller driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <stdint.h>
#include <sys/time.h>

#include "ra8d2_iic_regs.h"
#include "ra_dma.h"
#include "ra_err.h"
#include "ra_iic.h"
#include "ra_mstp.h"
#include "ra_sim_dma.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_iic_test_ch_t
 * @brief Channel numbers used by IIC tests.
 */
typedef enum : uint8_t {
  k_ra_iic_test_ch_zero = 0U,
  k_ra_iic_test_ch_one  = 1U,
  k_ra_iic_test_ch_two  = 2U,
  k_ra_iic_test_ch_oor  = 3U, /**< IIC has only 0..2. */
  k_ra_iic_test_ch_huge = 200U,
} ra_iic_test_ch_t;

/**
 * @enum ra_iic_test_bits_t
 * @brief ICSR2 bit masks used to simulate peripheral status bits.
 */
typedef enum : uint8_t {
  k_ra_iic_test_icsr2_start   = (uint8_t)(1U << 2U),
  k_ra_iic_test_icsr2_stop    = (uint8_t)(1U << 3U),
  k_ra_iic_test_icsr2_rdrf    = (uint8_t)(1U << 5U),
  k_ra_iic_test_icsr2_tdre    = (uint8_t)(1U << 7U),
  k_ra_iic_test_icsr2_all     = (uint8_t)((1U << 2U) | (1U << 3U) | (1U << 5U) | (1U << 7U)),
  k_ra_iic_test_iccr1_enable  = 0x80U,
  k_ra_iic_test_icmr1_default = 0x08U,
} ra_iic_test_bits_t;

/**
 * @enum ra_iic_test_addr_t
 * @brief Test addresses and payloads.
 */
typedef enum : uint8_t {
  k_ra_iic_test_target_addr = 0x50U,
  k_ra_iic_test_byte_a      = 0xA5U,
  k_ra_iic_test_byte_b      = 0x5AU,
  k_ra_iic_test_rx_byte     = 0xC3U,
} ra_iic_test_addr_t;

static const uint8_t s_payload[2] = {
  (uint8_t)k_ra_iic_test_byte_a,
  (uint8_t)k_ra_iic_test_byte_b,
};

/**
 * @enum ra_iic_test_alarm_t
 * @brief Interval-timer delays used to clobber ICSR2 during polling.
 */
typedef enum : uint32_t {
  k_ra_iic_test_alarm_usec = 100U, /**< 100us one-shot clear. */
} ra_iic_test_alarm_t;

/** @brief Which channel the signal handler should clobber. */
static uint8_t s_alarm_channel = 0U;

static void sigalarm_handler_iic(int sig)
{
  (void)sig;
  /* Clear ICSR2 so the polling loop observes bit drop and times out. */
  volatile r_iic_regs_t* reg = ra_iic(s_alarm_channel);
  if (reg != nullptr) {
    reg->ICSR2 = 0U;
  }
}

static void arm_icsr2_clear_alarm(uint8_t channel)
{
  s_alarm_channel = channel;
  struct sigaction sa;
  sa.sa_handler = sigalarm_handler_iic;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  /* Repeat every k_ra_iic_test_alarm_usec us so we keep clobbering
   * ICSR2 during the driver's busy-wait loops even if the first
   * fire lands before the loop is reached. */
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = (long)k_ra_iic_test_alarm_usec;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = (long)k_ra_iic_test_alarm_usec;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

static void disarm_alarm(void)
{
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

static void test_controller_init_happy(void)
{
  TEST_BEGIN("iic controller_init happy");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_controller_init((uint8_t)k_ra_iic_test_ch_zero));

  volatile r_iic_regs_t* reg = ra_iic((uint8_t)k_ra_iic_test_ch_zero);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT_EQ((int)k_ra_iic_test_iccr1_enable, (int)reg->ICCR1);
  TEST_ASSERT_EQ((int)k_ra_iic_test_icmr1_default, (int)reg->ICMR1);
  /* Wave 3.2 changes: controller_init now uses 100 kHz default + PCLKB
   * 60 MHz which produces a non-zero ICBRL. The legacy "should be 0"
   * assertion was tied to the Wave 0 stub that wrote zeros. */
  TEST_ASSERT(reg->ICBRL != 0U);
  TEST_ASSERT_EQ(0, (int)reg->ICBRH);
  TEST_END("iic controller_init happy");
}

static void test_controller_init_ch1_and_ch2(void)
{
  TEST_BEGIN("iic controller_init ch1+ch2");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_controller_init((uint8_t)k_ra_iic_test_ch_one));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_iic_controller_init((uint8_t)k_ra_iic_test_ch_two));
  TEST_END("iic controller_init ch1+ch2");
}

static void test_controller_init_bad_channel(void)
{
  TEST_BEGIN("iic controller_init bad channel");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();

  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_iic_controller_init((uint8_t)k_ra_iic_test_ch_oor));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_iic_controller_init((uint8_t)k_ra_iic_test_ch_huge));
  TEST_END("iic controller_init bad channel");
}

/**
 * @brief Pre-arm ICSR2 so start / tdre / stop flags all read as set,
 *        letting the polling helpers fall through immediately.
 */
static void prime_icsr2_all(uint8_t channel)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  reg->ICSR2                 = (uint8_t)k_ra_iic_test_icsr2_all;
}

static void test_write_happy(void)
{
  TEST_BEGIN("iic write happy");
  ra_sim_mmap_reset();

  prime_icsr2_all((uint8_t)k_ra_iic_test_ch_zero);
  const ra_err_t err =
    ra_iic_write((uint8_t)k_ra_iic_test_ch_zero, (uint8_t)k_ra_iic_test_target_addr, s_payload, 2U);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_END("iic write happy");
}

static void test_write_zero_length(void)
{
  TEST_BEGIN("iic write zero length");
  ra_sim_mmap_reset();

  prime_icsr2_all((uint8_t)k_ra_iic_test_ch_zero);
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_iic_write((uint8_t)k_ra_iic_test_ch_zero,
                                   (uint8_t)k_ra_iic_test_target_addr,
                                   s_payload,
                                   0U));
  TEST_END("iic write zero length");
}

static void test_write_bad_channel(void)
{
  TEST_BEGIN("iic write bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_write((uint8_t)k_ra_iic_test_ch_oor,
                                   (uint8_t)k_ra_iic_test_target_addr,
                                   s_payload,
                                   1U));
  TEST_END("iic write bad channel");
}

static void test_write_null_data(void)
{
  TEST_BEGIN("iic write null data");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_write((uint8_t)k_ra_iic_test_ch_zero,
                                   (uint8_t)k_ra_iic_test_target_addr,
                                   nullptr,
                                   1U));
  TEST_END("iic write null data");
}

static void test_write_timeout_on_start(void)
{
  TEST_BEGIN("iic write timeout on start");
  ra_sim_mmap_reset();

  /* ICSR2 is zero -> START flag never sets -> start-detect times out. */
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout,
                 (int)ra_iic_write((uint8_t)k_ra_iic_test_ch_zero,
                                   (uint8_t)k_ra_iic_test_target_addr,
                                   s_payload,
                                   1U));
  TEST_END("iic write timeout on start");
}

static void test_write_after_start_clear(void)
{
  TEST_BEGIN("iic write after start clear");
  ra_sim_mmap_reset();

  /* Arm only START so start passes, then the driver clears it with a
   * full-register write (~(1<<2) = 0xFB) that happens to leave TDRE
   * and the stop bit set as well. Verifies the entire write sequence
   * still returns k_ra_ok on the mock. */
  volatile r_iic_regs_t* reg = ra_iic((uint8_t)k_ra_iic_test_ch_zero);
  reg->ICSR2                 = (uint8_t)k_ra_iic_test_icsr2_start;

  const ra_err_t err =
    ra_iic_write((uint8_t)k_ra_iic_test_ch_zero, (uint8_t)k_ra_iic_test_target_addr, s_payload, 1U);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_END("iic write after start clear");
}

static void test_read_happy(void)
{
  TEST_BEGIN("iic read happy");
  ra_sim_mmap_reset();

  volatile r_iic_regs_t* reg = ra_iic((uint8_t)k_ra_iic_test_ch_zero);
  reg->ICSR2                 = (uint8_t)k_ra_iic_test_icsr2_all;
  reg->ICDRR                 = (uint8_t)k_ra_iic_test_rx_byte;

  uint8_t        buf[2] = {0U, 0U};
  const ra_err_t err =
    ra_iic_read((uint8_t)k_ra_iic_test_ch_zero, (uint8_t)k_ra_iic_test_target_addr, buf, 2U);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)err);
  TEST_ASSERT_EQ((int)k_ra_iic_test_rx_byte, (int)buf[0]);
  TEST_END("iic read happy");
}

static void test_read_zero_length(void)
{
  TEST_BEGIN("iic read zero length");
  ra_sim_mmap_reset();

  prime_icsr2_all((uint8_t)k_ra_iic_test_ch_zero);
  uint8_t buf = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_iic_read((uint8_t)k_ra_iic_test_ch_zero, (uint8_t)k_ra_iic_test_target_addr, &buf, 0U));
  TEST_END("iic read zero length");
}

static void test_read_bad_channel(void)
{
  TEST_BEGIN("iic read bad channel");
  ra_sim_mmap_reset();

  uint8_t buf = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_err_null_ptr,
    (int)ra_iic_read((uint8_t)k_ra_iic_test_ch_oor, (uint8_t)k_ra_iic_test_target_addr, &buf, 1U));
  TEST_END("iic read bad channel");
}

static void test_read_null_out(void)
{
  TEST_BEGIN("iic read null out");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_iic_read((uint8_t)k_ra_iic_test_ch_zero,
                                  (uint8_t)k_ra_iic_test_target_addr,
                                  nullptr,
                                  1U));
  TEST_END("iic read null out");
}

static void test_read_timeout_on_start(void)
{
  TEST_BEGIN("iic read timeout on start");
  ra_sim_mmap_reset();

  uint8_t buf = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_err_hw_timeout,
    (int)ra_iic_read((uint8_t)k_ra_iic_test_ch_zero, (uint8_t)k_ra_iic_test_target_addr, &buf, 1U));
  TEST_END("iic read timeout on start");
}

/**
 * @enum ra_iic_test_long_t
 * @brief Length used for the long-data timeout tests.
 */
typedef enum : uint32_t {
  k_ra_iic_test_long_len = 1000000U, /**< 1M-byte payload to out-race the alarm. */
} ra_iic_test_long_t;

/** @brief Big scratch buffer so the big-len tests have a real source. */
static uint8_t s_long_buffer[1000000];

static void test_write_tdre_break(void)
{
  TEST_BEGIN("iic write tdre break");
  ra_sim_mmap_reset();

  /* Pre-arm START so start wait passes instantly. A repeating SIGALRM
   * fires every 100us and zeroes ICSR2. The data loop is big enough
   * (1M bytes) that it spans several alarm periods, so at some point
   * the signal handler runs between the driver's ICDRT write and the
   * next wait_icsr2 read -- that TDRE wait then times out, the loop
   * breaks, and the function returns hw_timeout. */
  volatile r_iic_regs_t* reg = ra_iic((uint8_t)k_ra_iic_test_ch_zero);
  reg->ICSR2                 = (uint8_t)k_ra_iic_test_icsr2_start;
  arm_icsr2_clear_alarm((uint8_t)k_ra_iic_test_ch_zero);

  const ra_err_t err = ra_iic_write((uint8_t)k_ra_iic_test_ch_zero,
                                    (uint8_t)k_ra_iic_test_target_addr,
                                    s_long_buffer,
                                    (uint32_t)k_ra_iic_test_long_len);
  disarm_alarm();
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout, (int)err);
  TEST_END("iic write tdre break");
}

static void test_read_rdrf_break(void)
{
  TEST_BEGIN("iic read rdrf break");
  ra_sim_mmap_reset();

  volatile r_iic_regs_t* reg = ra_iic((uint8_t)k_ra_iic_test_ch_zero);
  reg->ICSR2                 = (uint8_t)k_ra_iic_test_icsr2_start;
  arm_icsr2_clear_alarm((uint8_t)k_ra_iic_test_ch_zero);

  const ra_err_t err = ra_iic_read((uint8_t)k_ra_iic_test_ch_zero,
                                   (uint8_t)k_ra_iic_test_target_addr,
                                   s_long_buffer,
                                   (uint32_t)k_ra_iic_test_long_len);
  disarm_alarm();
  TEST_ASSERT_EQ((int)k_ra_err_hw_timeout, (int)err);
  TEST_END("iic read rdrf break");
}

static void test_read_after_start_clear(void)
{
  TEST_BEGIN("iic read after start clear");
  ra_sim_mmap_reset();

  /* Same mechanic as the write test: pre-arm start only. The driver
   * clears start with a full-register write (0xFB) that leaves RDRF,
   * TDRE and STOP bits set, so the rest of the read loop completes
   * successfully on the mock. */
  volatile r_iic_regs_t* reg = ra_iic((uint8_t)k_ra_iic_test_ch_zero);
  reg->ICSR2                 = (uint8_t)k_ra_iic_test_icsr2_start;
  uint8_t buf                = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_iic_read((uint8_t)k_ra_iic_test_ch_zero, (uint8_t)k_ra_iic_test_target_addr, &buf, 1U));
  TEST_END("iic read after start clear");
}

/* =============================================================================
 * Wave 3.2: new API tests
 * =============================================================================
 */

static int32_t s_iic_cb_count = 0;
static int32_t s_iic_cb_err   = 0;
static void    stub_iic_cb(void* ctx, uint8_t err_mask)
{
  (void)ctx;
  ++s_iic_cb_count;
  s_iic_cb_err = (int32_t)err_mask;
}

static void prep_w32(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_iic_cb_count = 0;
  s_iic_cb_err   = 0;
}

static const ra_iic_cfg_t k_iic_cfg = {
  .bus_hz   = (uint32_t)k_ra_iic_speed_fast,
  .pclkb_hz = 60000000U,
};

static void test_iic_init_configured(void)
{
  TEST_BEGIN("ra_iic_init: ICCR1.ICE set, ICBRL programmed");
  prep_w32();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_cfg));
  volatile const r_iic_regs_t* reg = ra_iic(0U);
  TEST_ASSERT(reg->ICCR1 != 0U);
  TEST_ASSERT(reg->ICBRL != 0U);
  TEST_END("ra_iic_init: ICCR1.ICE set, ICBRL programmed");
}

static void test_iic_init_bad(void)
{
  TEST_BEGIN("ra_iic_init: bad inputs rejected");
  prep_w32();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iic_init(0U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_init(9U, &k_iic_cfg));
  TEST_END("ra_iic_init: bad inputs rejected");
}

static void test_iic_deinit(void)
{
  TEST_BEGIN("ra_iic_deinit: ICCR1 zeroed, MSTP released");
  prep_w32();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(1U, &k_iic_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_deinit(1U));
  bool stopped = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_iic1, &stopped));
  TEST_ASSERT(stopped);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_deinit(9U));
  TEST_END("ra_iic_deinit: ICCR1 zeroed, MSTP released");
}

static void test_iic_set_clock(void)
{
  TEST_BEGIN("ra_iic_set_clock: updates ICBRL");
  prep_w32();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_cfg));
  const uint8_t before = ra_iic(0U)->ICBRL;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_set_clock(0U, (uint32_t)k_ra_iic_speed_standard, 60000000U));
  TEST_ASSERT(ra_iic(0U)->ICBRL != before);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_set_clock(0U, 0U, 60000000U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_set_clock(9U, 100000U, 60000000U));
  TEST_END("ra_iic_set_clock: updates ICBRL");
}

static void test_iic_errors_mask_and_clear(void)
{
  TEST_BEGIN("ra_iic_get/clear_errors: AL/NACK/TMO");
  prep_w32();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_cfg));

  volatile r_iic_regs_t* reg = ra_iic(0U);
  reg->ICSR2                 = (uint8_t)((1U << 0U) | (1U << 1U) | (1U << 4U));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_get_errors(0U, &mask));
  TEST_ASSERT_EQ((int32_t)((uint8_t)k_ra_iic_err_timeout | (uint8_t)k_ra_iic_err_arb_lost |
                           (uint8_t)k_ra_iic_err_nack),
                 (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_clear_errors(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_get_errors(0U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_iic_err_none, (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iic_get_errors(0U, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_get_errors(9U, &mask));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_clear_errors(9U));
  TEST_END("ra_iic_get/clear_errors: AL/NACK/TMO");
}

static void test_iic_attach_handler(void)
{
  TEST_BEGIN("ra_iic_attach_transfer_handler: ICIER toggled");
  prep_w32();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_cfg));

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_attach_transfer_handler(0U, stub_iic_cb, nullptr));
  TEST_ASSERT(ra_iic(0U)->ICIER != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_attach_transfer_handler(0U, nullptr, nullptr));
  TEST_ASSERT_EQ(0, (int32_t)ra_iic(0U)->ICIER);

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_attach_transfer_handler(9U, stub_iic_cb, nullptr));
  TEST_END("ra_iic_attach_transfer_handler: ICIER toggled");
}

static void test_iic_dispatch_eri(void)
{
  TEST_BEGIN("ra_iic_dispatch_eri: fires callback with mask");
  prep_w32();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iic_attach_transfer_handler(0U, stub_iic_cb, nullptr));

  volatile r_iic_regs_t* reg = ra_iic(0U);
  reg->ICSR2                 = (uint8_t)(1U << 4U); /* NACKF */

  ra_iic_dispatch_eri(0U);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_iic_cb_count);
  TEST_ASSERT_EQ((int32_t)(uint8_t)k_ra_iic_err_nack, (int32_t)s_iic_cb_err);

  /* Zero-mask (no real error) does not fire the callback. */
  reg->ICSR2     = 0U;
  s_iic_cb_count = 0;
  ra_iic_dispatch_eri(0U);
  TEST_ASSERT_EQ((int32_t)0, (int32_t)s_iic_cb_count);

  /* Out-of-range is a no-op. */
  ra_iic_dispatch_eri(9U);
  ra_iic_dispatch_txi(0U);
  ra_iic_dispatch_txi(9U);
  ra_iic_dispatch_rxi(0U);
  ra_iic_dispatch_rxi(9U);
  TEST_END("ra_iic_dispatch_eri: fires callback with mask");
}

static void test_iic_power_transition(void)
{
  TEST_BEGIN("ra_iic_enter_stop / exit_stop");
  prep_w32();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(2U, &k_iic_cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_enter_stop(2U));
  bool stopped = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_iic2, &stopped));
  TEST_ASSERT(stopped);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_exit_stop(2U));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_mstp_is_stopped(k_ra_mstp_iic2, &stopped));
  TEST_ASSERT(!stopped);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_enter_stop(9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iic_exit_stop(9U));
  TEST_END("ra_iic_enter_stop / exit_stop");
}

static int32_t s_iic_dma_done = 0;

static void stub_iic_dma_done(void* ctx)
{
  (void)ctx;
  ++s_iic_dma_done;
}

static void test_iic_write_dma_streams_to_icdrt(void)
{
  TEST_BEGIN("ra_iic_write_dma: buffer streams into ICDRT");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_cfg));

  const uint8_t src[] = {0x11U, 0x22U, 0x33U};
  uint8_t       dch   = 0xFFU;
  s_iic_dma_done      = 0;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_iic_write_dma(0U, src, (uint16_t)sizeof(src), stub_iic_dma_done, nullptr, &dch));
  TEST_ASSERT(dch < 8U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(dch));
  volatile const r_iic_regs_t* reg = ra_iic(0U);
  TEST_ASSERT_EQ((int32_t)0x33U, (int32_t)reg->ICDRT);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(dch));
  TEST_ASSERT_EQ(1, s_iic_dma_done);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dch));
  TEST_END("ra_iic_write_dma: buffer streams into ICDRT");
}

static void test_iic_read_dma_streams_from_icdrr(void)
{
  TEST_BEGIN("ra_iic_read_dma: ICDRR streams into buffer");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_cfg));

  volatile r_iic_regs_t* reg = ra_iic(0U);
  reg->ICDRR                 = 0x55U;

  uint8_t out[2] = {0U, 0U};
  uint8_t dch    = 0xFFU;
  s_iic_dma_done = 0;
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_iic_read_dma(0U, out, (uint16_t)sizeof(out), stub_iic_dma_done, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_memcpy(dch));
  TEST_ASSERT_EQ((int32_t)0x55U, (int32_t)out[0]);
  TEST_ASSERT_EQ((int32_t)0x55U, (int32_t)out[1]);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sim_dma_complete(dch));
  TEST_ASSERT_EQ(1, s_iic_dma_done);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_release(dch));
  TEST_END("ra_iic_read_dma: ICDRR streams into buffer");
}

static void test_iic_dma_arg_validation(void)
{
  TEST_BEGIN("ra_iic_{write,read}_dma: arg validation");
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dma_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iic_init(0U, &k_iic_cfg));

  uint8_t       dch    = 0U;
  const uint8_t src[]  = {0xAAU};
  uint8_t       dst[1] = {0U};

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_write_dma(0U, nullptr, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_write_dma(0U, src, 1U, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_read_dma(0U, nullptr, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_iic_read_dma(0U, dst, 1U, nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_write_dma(99U, src, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_read_dma(99U, dst, 1U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_write_dma(0U, src, 0U, nullptr, nullptr, &dch));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_iic_read_dma(0U, dst, 0U, nullptr, nullptr, &dch));
  TEST_END("ra_iic_{write,read}_dma: arg validation");
}

int32_t main(void)
{
  test_controller_init_happy();
  test_controller_init_ch1_and_ch2();
  test_controller_init_bad_channel();
  test_write_happy();
  test_write_zero_length();
  test_write_bad_channel();
  test_write_null_data();
  test_write_timeout_on_start();
  test_write_after_start_clear();
  test_write_tdre_break();
  test_read_happy();
  test_read_zero_length();
  test_read_bad_channel();
  test_read_null_out();
  test_read_timeout_on_start();
  test_read_after_start_clear();
  test_read_rdrf_break();
  test_iic_init_configured();
  test_iic_init_bad();
  test_iic_deinit();
  test_iic_set_clock();
  test_iic_errors_mask_and_clear();
  test_iic_attach_handler();
  test_iic_dispatch_eri();
  test_iic_power_transition();
  test_iic_write_dma_streams_to_icdrt();
  test_iic_read_dma_streams_from_icdrr();
  test_iic_dma_arg_validation();
  (void)fprintf(stderr, "[OK  ] test_ra_iic.c\n");
  return 0;
}
