/**
 * @file test_ra_i2c.c
 * @brief Unit tests for the I2C (RIIC) controller driver.
 *
 * @details
 * Drives the polling-mode RIIC controller against the host-side
 * ``ra_sim_mmap`` substrate. ICSR2 status flags are pre-armed where the
 * driver expects them (TDRE, TEND, RDRF) so the bounded wait loops fall
 * through immediately. ICCR2.BBSY is left clear (= bus free) so the
 * bus-busy gate accepts each transaction.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <signal.h>
#include <stdint.h>
#include <sys/time.h>

#include "ra8d2_i2c_regs.h"
#include "ra_err.h"
#include "ra_i2c.h"
#include "ra_i2c_internal.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_i2c_test_const_t
 * @brief Test addresses, channels and payload constants.
 */
typedef enum : uint8_t {
  k_ra_i2c_test_periph  = 0x50U, /**< 7-bit peripheral address under test. */
  k_ra_i2c_test_ch0     = 0U,    /**< Channel 0 (IIC0). */
  k_ra_i2c_test_ch1     = 1U,    /**< Channel 1 (IIC1). */
  k_ra_i2c_test_ch2     = 2U,    /**< Channel 2 (IIC2). */
  k_ra_i2c_test_ch_oor  = 3U,    /**< Out-of-range channel. */
  k_ra_i2c_test_ch_huge = 200U,  /**< Far out-of-range channel. */
  k_ra_i2c_test_byte_a  = 0xA5U, /**< Payload byte A. */
  k_ra_i2c_test_byte_b  = 0x5AU, /**< Payload byte B. */
  k_ra_i2c_test_rx_byte = 0xC3U, /**< Receive payload byte. */
} ra_i2c_test_const_t;

/**
 * @enum ra_i2c_test_clk_t
 * @brief Clock constants used by the bit-rate tests.
 */
typedef enum : uint32_t {
  k_ra_i2c_test_pclkb_hz = 50000000U, /**< 50 MHz PCLKB. */
} ra_i2c_test_clk_t;

/** @brief Standard-mode configuration descriptor. */
static const ra_i2c_cfg_t k_i2c_cfg = {
  .bus_hz   = (uint32_t)k_ra_i2c_speed_standard,
  .pclkb_hz = (uint32_t)k_ra_i2c_test_pclkb_hz,
};

/** @brief Two-byte transmit payload. */
static const uint8_t s_payload[2] = {
  (uint8_t)k_ra_i2c_test_byte_a,
  (uint8_t)k_ra_i2c_test_byte_b,
};

/**
 * @brief Pre-arm ICSR2 so the driver's TDRE / TEND / RDRF wait loops
 *        fall through immediately. ``clear_status`` does not touch these
 *        flags, so a one-shot pre-prime survives the transfer.
 *
 * @param[in] channel Channel to prime.
 */
static void prime_status(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra_i2c_regs(channel);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra_i2c_msk_icsr2_tdre | (uint8_t)k_ra_i2c_msk_icsr2_tend |
                         (uint8_t)k_ra_i2c_msk_icsr2_rdrf);
}

/**
 * @brief Reset the simulator and refresh MSTP state before each case.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/** @brief Channel the SIGALRM injector targets. */
static uint8_t s_alarm_channel = 0U;
/** @brief When true, the SIGALRM injector latches NACKF (and TDRE). */
static volatile bool s_inject_nack = false;
/** @brief When true, the injector also latches TDRE so send_address advances. */
static volatile bool s_inject_tdre = false;

/** @brief Alarm interval in microseconds for the NACK injector. */
typedef enum : uint32_t {
  k_ra_i2c_test_alarm_usec = 100U,
} ra_i2c_test_alarm_t;

/**
 * @brief SIGALRM handler that injects NACKF into ICSR2 while the driver
 *        spins, mirroring how the RIIC hardware latches NACKF
 *        mid-transaction (after the start-of-transfer clear_status).
 *
 * @param[in] sig Unused signal number.
 */
static void sigalarm_handler_nack(int sig)
{
  (void)sig;
  if (!s_inject_nack) {
    return;
  }
  volatile r_i2c_regs_t* reg = ra_i2c_regs(s_alarm_channel);
  if (reg != nullptr) {
    uint8_t v = (uint8_t)k_ra_i2c_msk_icsr2_nackf;
    if (s_inject_tdre) {
      v |= (uint8_t)k_ra_i2c_msk_icsr2_tdre;
    }
    reg->ICSR2 = (uint8_t)(reg->ICSR2 | v);
  }
}

/**
 * @brief Arm a periodic SIGALRM that latches NACKF on @p channel.
 *
 * @param[in] channel    Channel to inject into.
 * @param[in] also_tdre  Also latch TDRE so a stalled send_address can
 *                       advance and read the NACKF.
 */
static void arm_nack_alarm(uint8_t channel, bool also_tdre)
{
  s_alarm_channel = channel;
  s_inject_nack   = true;
  s_inject_tdre   = also_tdre;
  struct sigaction sa;
  sa.sa_handler = sigalarm_handler_nack;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
  struct itimerval timer;
  timer.it_value.tv_sec     = 0;
  timer.it_value.tv_usec    = (long)k_ra_i2c_test_alarm_usec;
  timer.it_interval.tv_sec  = 0;
  timer.it_interval.tv_usec = (long)k_ra_i2c_test_alarm_usec;
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
}

/**
 * @brief Disarm the SIGALRM injector.
 */
static void disarm_nack_alarm(void)
{
  s_inject_nack          = false;
  s_inject_tdre          = false;
  struct itimerval timer = {};
  (void)setitimer(ITIMER_REAL, &timer, nullptr);
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  (void)sigaction(SIGALRM, &sa, nullptr);
}

/* =============================================================================
 * Init / deinit / clock
 * =============================================================================
 *
 * @par MC/DC:
 * (no compound decisions exercised here -- happy-path / range contract)
 */
static void test_init_configured(void)
{
  TEST_BEGIN("ra_i2c_init: ICE set, bit rate programmed");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch1, &k_i2c_cfg));
  volatile const r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch1);
  TEST_ASSERT((reg->ICCR1 & (uint8_t)k_ra_i2c_msk_iccr1_ice) != 0U);
  /* ICBRL/ICBRH carry the reserved hi bits even at the slowest rate. */
  TEST_ASSERT(reg->ICBRL != 0U);
  TEST_ASSERT(reg->ICBRH != 0U);
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_deinit((uint8_t)k_ra_i2c_test_ch1));
  TEST_END("ra_i2c_init: ICE set, bit rate programmed");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / null contract)
 */
static void test_init_bad_inputs(void)
{
  TEST_BEGIN("ra_i2c_init: bad inputs rejected");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, nullptr));
  ra_i2c_cfg_t bad = k_i2c_cfg;
  bad.bus_hz       = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &bad));
  bad          = k_i2c_cfg;
  bad.pclkb_hz = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &bad));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_i2c_init((uint8_t)k_ra_i2c_test_ch_oor, &k_i2c_cfg));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_i2c_init((uint8_t)k_ra_i2c_test_ch_huge, &k_i2c_cfg));
  TEST_END("ra_i2c_init: bad inputs rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- happy-path / range contract)
 */
static void test_fast_plus_sets_fmpe(void)
{
  TEST_BEGIN("ra_i2c_init: Fm+ sets ICFER.FMPE");
  prep();
  ra_i2c_cfg_t cfg = {.bus_hz   = (uint32_t)k_ra_i2c_speed_fast_plus,
                      .pclkb_hz = (uint32_t)k_ra_i2c_test_pclkb_hz};
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch2, &cfg));
  volatile const r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch2);
  TEST_ASSERT((reg->ICFER & (uint8_t)k_ra_i2c_msk_icfer_fmpe) != 0U);
  TEST_END("ra_i2c_init: Fm+ sets ICFER.FMPE");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / happy-path contract)
 */
static void test_set_clock(void)
{
  TEST_BEGIN("ra_i2c_set_clock: reprograms divider");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_i2c_set_clock((uint8_t)k_ra_i2c_test_ch0,
                                  (uint32_t)k_ra_i2c_speed_fast,
                                  (uint32_t)k_ra_i2c_test_pclkb_hz));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_i2c_set_clock((uint8_t)k_ra_i2c_test_ch_oor,
                                  (uint32_t)k_ra_i2c_speed_fast,
                                  (uint32_t)k_ra_i2c_test_pclkb_hz));
  TEST_ASSERT_EQ(
    k_ra_err_invalid_arg,
    ra_i2c_set_clock((uint8_t)k_ra_i2c_test_ch0, 0U, (uint32_t)k_ra_i2c_test_pclkb_hz));
  TEST_END("ra_i2c_set_clock: reprograms divider");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / null contract)
 */
static void test_deinit_range(void)
{
  TEST_BEGIN("ra_i2c_deinit: range check");
  prep();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_i2c_deinit((uint8_t)k_ra_i2c_test_ch_oor));
  TEST_END("ra_i2c_deinit: range check");
}

/* =============================================================================
 * Polling write
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- happy-path write+STOP)
 */
static void test_write_happy(void)
{
  TEST_BEGIN("ra_i2c_write: write + STOP success");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra_i2c_test_ch0);
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_i2c_write((uint8_t)k_ra_i2c_test_ch0,
                              (uint8_t)k_ra_i2c_test_periph,
                              s_payload,
                              sizeof(s_payload),
                              /*send_stop=*/true));
  volatile const r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch0);
  /* STOP requested via ICCR2.SP and the wire byte equals the last data. */
  TEST_ASSERT((reg->ICCR2 & (uint8_t)k_ra_i2c_msk_iccr2_sp) != 0U);
  TEST_END("ra_i2c_write: write + STOP success");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range contract)
 */
static void test_write_bad_inputs(void)
{
  TEST_BEGIN("ra_i2c_write: null / range rejected");
  prep();
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_i2c_write((uint8_t)k_ra_i2c_test_ch_oor,
                              (uint8_t)k_ra_i2c_test_periph,
                              s_payload,
                              1U,
                              true));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_i2c_write((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, nullptr, 1U, true));
  TEST_END("ra_i2c_write: null / range rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- bus-busy rejection)
 */
static void test_write_bus_busy(void)
{
  TEST_BEGIN("ra_i2c_write: BBSY set rejects transfer");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch0);
  reg->ICCR2                 = (uint8_t)k_ra_i2c_msk_iccr2_bbsy;
  TEST_ASSERT_EQ(
    k_ra_err_busy,
    ra_i2c_write((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, s_payload, 1U, true));
  TEST_END("ra_i2c_write: BBSY set rejects transfer");
}

/* =============================================================================
 * Polling read
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- happy-path read+STOP)
 */
static void test_read_happy(void)
{
  TEST_BEGIN("ra_i2c_read: multi-byte read + STOP");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch0);
  reg->ICDRR                 = (uint8_t)k_ra_i2c_test_rx_byte;
  uint8_t buf[3]             = {0U, 0U, 0U};
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_i2c_read((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra_i2c_test_rx_byte, buf[0]);
  TEST_ASSERT((reg->ICCR2 & (uint8_t)k_ra_i2c_msk_iccr2_sp) != 0U);
  TEST_END("ra_i2c_read: multi-byte read + STOP");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- single-byte read)
 */
static void test_read_single_byte(void)
{
  TEST_BEGIN("ra_i2c_read: single-byte read");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch0);
  reg->ICDRR                 = (uint8_t)k_ra_i2c_test_rx_byte;
  uint8_t b                  = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_i2c_read((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, &b, 1U));
  TEST_ASSERT_EQ(k_ra_i2c_test_rx_byte, b);
  TEST_END("ra_i2c_read: single-byte read");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range / len)
 */
static void test_read_bad_inputs(void)
{
  TEST_BEGIN("ra_i2c_read: null / range / zero len rejected");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  uint8_t b = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_i2c_read((uint8_t)k_ra_i2c_test_ch_oor, (uint8_t)k_ra_i2c_test_periph, &b, 1U));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_i2c_read((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_i2c_read((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, &b, 0U));
  TEST_END("ra_i2c_read: null / range / zero len rejected");
}

/* =============================================================================
 * Scan / status
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- ACK probe happy path)
 */
static void test_scan_ack(void)
{
  TEST_BEGIN("ra_i2c_scan: ACK reported");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra_i2c_test_ch0);
  bool acked = false;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_i2c_scan((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, &acked));
  TEST_ASSERT(acked);
  TEST_END("ra_i2c_scan: ACK reported");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- NACK probe path)
 */
static void test_scan_nack(void)
{
  TEST_BEGIN("ra_i2c_scan: NACK reported");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch0);
  /* TDRE so the address byte writes; the SIGALRM injector latches NACKF
   * mid-spin (the start-of-transfer clear_status wipes any pre-armed
   * NACKF, so it must be injected after the driver clears it). */
  reg->ICSR2 = (uint8_t)k_ra_i2c_msk_icsr2_tdre;
  arm_nack_alarm((uint8_t)k_ra_i2c_test_ch0, /*also_tdre=*/false);
  bool acked = true;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_i2c_scan((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, &acked));
  disarm_nack_alarm();
  TEST_ASSERT(!acked);
  TEST_END("ra_i2c_scan: NACK reported");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range contract)
 */
static void test_scan_bad_inputs(void)
{
  TEST_BEGIN("ra_i2c_scan: null / range rejected");
  prep();
  bool acked = false;
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_i2c_scan((uint8_t)k_ra_i2c_test_ch_oor, (uint8_t)k_ra_i2c_test_periph, &acked));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_i2c_scan((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, nullptr));
  TEST_END("ra_i2c_scan: null / range rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- error decode / clear)
 */
static void test_errors_get_clear(void)
{
  TEST_BEGIN("ra_i2c_get_errors / clear_errors");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch0);
  reg->ICSR2   = (uint8_t)((uint8_t)k_ra_i2c_msk_icsr2_al | (uint8_t)k_ra_i2c_msk_icsr2_nackf);
  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_get_errors((uint8_t)k_ra_i2c_test_ch0, &mask));
  TEST_ASSERT((mask & (uint8_t)k_ra_i2c_err_arb_lost) != 0U);
  TEST_ASSERT((mask & (uint8_t)k_ra_i2c_err_nack) != 0U);
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_clear_errors((uint8_t)k_ra_i2c_test_ch0));
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_get_errors((uint8_t)k_ra_i2c_test_ch0, &mask));
  TEST_ASSERT_EQ(k_ra_i2c_err_none, mask);
  /* Null / range guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_i2c_get_errors((uint8_t)k_ra_i2c_test_ch0, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_i2c_get_errors((uint8_t)k_ra_i2c_test_ch_oor, &mask));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_i2c_clear_errors((uint8_t)k_ra_i2c_test_ch_oor));
  TEST_END("ra_i2c_get_errors / clear_errors");
}

/* =============================================================================
 * MC/DC vector sets
 * =============================================================================
 */

/**
 * @test test_mcdc_clk_invalid
 *
 * @par MC/DC:
 * Decision in ``ra_i2c_internal_clk_invalid``, libs/ra_hal/src/ra_i2c.c:105 (CITES-OK: MC/DC anchor)
 *   ``(bus_hz == 0) || (pclkb_hz == 0)`` (2 conditions, OR).
 * - V1: bus!=0, pclkb!=0 -> C1=F,C2=F -> dec F
 * - V2: bus=0,  pclkb!=0 -> C1=T (short-circuits) -> dec T (varies left)
 * - V3: bus!=0, pclkb=0  -> C1=F,C2=T -> dec T (varies right)
 * Pairs (V1,V2) flip C1; (V1,V3) flip C2. N+1 = 3 vectors.
 */
static void test_mcdc_clk_invalid(void)
{
  TEST_BEGIN("i2c MC/DC: clk_invalid OR");
  TEST_ASSERT(!ra_i2c_internal_clk_invalid(100000U, 50000000U));
  TEST_ASSERT(ra_i2c_internal_clk_invalid(0U, 50000000U));
  TEST_ASSERT(ra_i2c_internal_clk_invalid(100000U, 0U));
  TEST_END("i2c MC/DC: clk_invalid OR");
}

/**
 * @test test_mcdc_is_wait_byte
 *
 * @par MC/DC:
 * Decision in ``ra_i2c_internal_is_wait_byte``, libs/ra_hal/src/ra_i2c.c:127 (CITES-OK: MC/DC anchor)
 *   ``(len >= 2) && (index == len - 2)`` (2 conditions, AND).
 * - V1: len=3, index=1 -> C1=T,C2=T -> dec T
 * - V2: len=1, index=0 -> C1=F (short-circuits) -> dec F (varies left)
 * - V3: len=3, index=0 -> C1=T,C2=F -> dec F (varies right)
 * Pairs (V1,V2) flip C1; (V1,V3) flip C2. N+1 = 3 vectors.
 */
static void test_mcdc_is_wait_byte(void)
{
  TEST_BEGIN("i2c MC/DC: is_wait_byte AND");
  TEST_ASSERT(ra_i2c_internal_is_wait_byte(1U, 3U));
  TEST_ASSERT(!ra_i2c_internal_is_wait_byte(0U, 1U));
  TEST_ASSERT(!ra_i2c_internal_is_wait_byte(0U, 3U));
  TEST_END("i2c MC/DC: is_wait_byte AND");
}

/**
 * @test test_mcdc_transfer
 *
 * @par MC/DC:
 * Three 2-condition decisions in ``ra_i2c_transfer``:
 * Decision A libs/ra_hal/src/ra_i2c.c:845 (CITES-OK: MC/DC anchor): ``(wr_len == 0) && (rd_len == 0)``
 * - V1: wr=0, rd=0       -> C1=T,C2=T -> dec T (-> invalid_arg)
 * - V2: wr!=0            -> C1=F (short-circuits) -> dec F
 * - V3: wr=0, rd!=0      -> C1=T,C2=F -> dec F
 * Decision B libs/ra_hal/src/ra_i2c.c:848 (CITES-OK: MC/DC anchor): ``(wr_len != 0) && (wr == nullptr)``
 * - V1: wr_len=0          -> C1=F (short-circuits) -> dec F
 * - V2: wr_len!=0, wr!=0  -> C1=T,C2=F -> dec F
 * - V3: wr_len!=0, wr=NULL-> C1=T,C2=T -> dec T (null_ptr)
 * Decision C libs/ra_hal/src/ra_i2c.c:851 (CITES-OK: MC/DC anchor): ``(rd_len != 0) && (rd == nullptr)``
 * mirrors B with rd_len/rd; same N+1 = 3 vectors.
 */
static void test_mcdc_transfer(void)
{
  TEST_BEGIN("i2c MC/DC: transfer arg-validation 2-cond decisions");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  uint8_t wr_buf[1] = {0xA5U};
  uint8_t rd_buf[1] = {0U};

  /* Decision A V1: both lens zero -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_i2c_transfer((uint8_t)k_ra_i2c_test_ch0,
                                 (uint8_t)k_ra_i2c_test_periph,
                                 wr_buf,
                                 0U,
                                 rd_buf,
                                 0U));
  /* Decision B V3: wr_len!=0 with NULL wr -> null_ptr. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_i2c_transfer((uint8_t)k_ra_i2c_test_ch0,
                                 (uint8_t)k_ra_i2c_test_periph,
                                 nullptr,
                                 1U,
                                 rd_buf,
                                 0U));
  /* Decision C V3: rd_len!=0 with NULL rd (wr_len=0 keeps A=F, B=F). */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_i2c_transfer((uint8_t)k_ra_i2c_test_ch0,
                                 (uint8_t)k_ra_i2c_test_periph,
                                 wr_buf,
                                 0U,
                                 nullptr,
                                 1U));
  /* Range guard: out-of-range channel -> null_ptr. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_i2c_transfer((uint8_t)k_ra_i2c_test_ch_oor,
                                 (uint8_t)k_ra_i2c_test_periph,
                                 wr_buf,
                                 1U,
                                 rd_buf,
                                 1U));
  TEST_END("i2c MC/DC: transfer arg-validation 2-cond decisions");
}

/**
 * @test test_mcdc_transfer_combined
 *
 * @par MC/DC:
 * Happy-path masking pairs (decision F vectors) for the three
 * ``ra_i2c_transfer`` decisions and the write-finish OR at
 * libs/ra_hal/src/ra_i2c.c:669 (CITES-OK: MC/DC anchor):
 *   ``(err != k_ra_ok) || send_stop`` (2 conditions, OR).
 * - V1 (write+read combined, send_stop=false on write phase): C1=F,C2=F
 *   -> dec F (bus held for the RESTART read phase)
 * - V2 (plain write, send_stop=true): C1=F,C2=T -> dec T (varies right)
 * - The error-path C1=T arm is covered by the timeout cases in
 *   test_write_timeout / test_read_timeout.
 * Combined transfer also drives decision A=F (wr!=0), B=F (wr!=NULL),
 * C=F (rd!=NULL) -- the masking-pair complements of test_mcdc_transfer.
 */
static void test_mcdc_transfer_combined(void)
{
  TEST_BEGIN("i2c MC/DC: transfer combined write+read happy path");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch0);
  reg->ICDRR                 = (uint8_t)k_ra_i2c_test_rx_byte;
  uint8_t wr_buf[1]          = {0x12U};
  uint8_t rd_buf[2]          = {0U, 0U};
  /* Decision F V1: write phase holds the bus (send_stop=false). */
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_i2c_transfer((uint8_t)k_ra_i2c_test_ch0,
                                 (uint8_t)k_ra_i2c_test_periph,
                                 wr_buf,
                                 sizeof(wr_buf),
                                 rd_buf,
                                 sizeof(rd_buf)));
  /* Decision F V2: plain write issues STOP (send_stop=true). */
  prime_status((uint8_t)k_ra_i2c_test_ch0);
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_i2c_transfer((uint8_t)k_ra_i2c_test_ch0,
                                 (uint8_t)k_ra_i2c_test_periph,
                                 wr_buf,
                                 sizeof(wr_buf),
                                 nullptr,
                                 0U));
  TEST_END("i2c MC/DC: transfer combined write+read happy path");
}

/**
 * @test test_write_timeout
 *
 * @par MC/DC:
 * Error-path arm (C1=T) of the write-finish OR at
 * libs/ra_hal/src/ra_i2c.c:669 (CITES-OK: MC/DC anchor)
 *   ``(err != k_ra_ok) || send_stop``: with TDRE never pre-armed the
 *   address send times out, so ``err != k_ra_ok`` (C1=T) forces STOP
 *   regardless of send_stop. Complements the C1=F vectors in
 *   test_mcdc_transfer_combined.
 */
static void test_write_timeout(void)
{
  TEST_BEGIN("ra_i2c_write: address timeout -> STOP");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  /* No prime_status: TDRE stays clear so the address wait times out. */
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_i2c_write((uint8_t)k_ra_i2c_test_ch0,
                              (uint8_t)k_ra_i2c_test_periph,
                              s_payload,
                              1U,
                              /*send_stop=*/false));
  TEST_END("ra_i2c_write: address timeout -> STOP");
}

/**
 * @test test_read_timeout
 *
 * @par MC/DC:
 * Error-path arm of the receive-drain loop guard at
 * libs/ra_hal/src/ra_i2c.c:776 (CITES-OK: MC/DC anchor)
 *   ``(err == k_ra_ok) && (i < len)``: with RDRF never pre-armed the
 *   dummy read times out, so ``err != k_ra_ok`` (C1=F) halts the loop on
 *   its first evaluation. The C1=T,C2=T (drain) and C1=T,C2=F (loop
 *   exit) vectors are covered by test_read_happy.
 */
static void test_read_timeout(void)
{
  TEST_BEGIN("ra_i2c_read: RDRF timeout halts drain loop");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  /* TDRE so the address send succeeds, but no RDRF for the data phase. */
  volatile r_i2c_regs_t* reg = ra_i2c_regs((uint8_t)k_ra_i2c_test_ch0);
  reg->ICSR2                 = (uint8_t)k_ra_i2c_msk_icsr2_tdre;
  uint8_t b                  = 0U;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_i2c_read((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, &b, 1U));
  TEST_END("ra_i2c_read: RDRF timeout halts drain loop");
}

/**
 * @test test_mcdc_scan_addr_err
 *
 * @par MC/DC:
 * Decision in ``ra_i2c_scan``, libs/ra_hal/src/ra_i2c.c:892 (CITES-OK: MC/DC anchor)
 *   ``(err != k_ra_ok) && (err != k_ra_err_nack)`` (2 conditions, AND).
 * - V1 (address timeout): err=hw_timeout -> C1=T,C2=T -> dec T (hard
 *   error returned). TDRE never armed.
 * - V2 (ACK happy path, test_scan_ack): err=k_ra_ok -> C1=F
 *   (short-circuits) -> dec F.
 * - V3 (NACK probe, test_scan_nack): err=k_ra_err_nack -> C1=T,C2=F ->
 *   dec F (probe continues, reports acked=false).
 * Pairs (V1,V2) flip C1; (V1,V3) flip C2. N+1 = 3 vectors.
 */
static void test_mcdc_scan_addr_err(void)
{
  TEST_BEGIN("i2c MC/DC: scan address-error AND");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));

  /* V1: TDRE clear and no injection -> address wait times out, so
   * err=hw_timeout: C1=T (err != ok), C2=T (err != nack) -> dec T. */
  bool acked = false;
  TEST_ASSERT_EQ(k_ra_err_hw_timeout,
                 ra_i2c_scan((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, &acked));

  /* V3: inject TDRE + NACKF so send_address reads NACKF and returns
   * k_ra_err_nack: C1=T (err != ok), C2=F (err == nack) -> dec F; the
   * probe then continues and reports acked=false. */
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_i2c_init((uint8_t)k_ra_i2c_test_ch0, &k_i2c_cfg));
  arm_nack_alarm((uint8_t)k_ra_i2c_test_ch0, /*also_tdre=*/true);
  acked = true;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_i2c_scan((uint8_t)k_ra_i2c_test_ch0, (uint8_t)k_ra_i2c_test_periph, &acked));
  disarm_nack_alarm();
  TEST_ASSERT(!acked);
  TEST_END("i2c MC/DC: scan address-error AND");
}

/**
 * @brief Run every test case in this translation unit.
 *
 * @return 0 when all cases pass (Unity aborts the process on failure).
 *
 * @pre The ``ra_sim_mmap`` constructor has installed the MMIO windows.
 * @post Every registered case has executed.
 * @note Thread safety: single-threaded test harness.
 * @since 0.1.0
 */
int main(void)
{
  test_init_configured();
  test_init_bad_inputs();
  test_fast_plus_sets_fmpe();
  test_set_clock();
  test_deinit_range();
  test_write_happy();
  test_write_bad_inputs();
  test_write_bus_busy();
  test_read_happy();
  test_read_single_byte();
  test_read_bad_inputs();
  test_scan_ack();
  test_scan_nack();
  test_scan_bad_inputs();
  test_errors_get_clear();
  test_mcdc_clk_invalid();
  test_mcdc_is_wait_byte();
  test_mcdc_transfer();
  test_mcdc_transfer_combined();
  test_write_timeout();
  test_read_timeout();
  test_mcdc_scan_addr_err();
  return 0;
}
