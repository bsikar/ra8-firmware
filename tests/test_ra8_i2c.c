/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_i2c.c
 * @brief Unit tests for the I2C (RIIC) controller driver.
 *
 * @details
 * Drives the polling-mode RIIC controller against the host-side
 * ``ra8_fake_mmap`` substrate. ICSR2 status flags are pre-armed where the
 * driver expects them (TDRE, TEND, RDRF) so the bounded wait loops fall
 * through immediately. ICCR2.BBSY is left clear (= bus free) so the
 * bus-busy gate accepts each transaction.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_i2c.h"
#include "ra8_i2c_internal.h"
#include "ra8_i2c_regs.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/**
 * @enum i2c_fixture_t
 * @brief The recognizable values moved through the code under test.
 */
typedef enum : uint8_t {
  k_i2c_payload_byte = 0xA5U, /**< A recognizable single-byte payload; neither 0x00 nor 0xFF. */
  k_i2c_payload_byte_alt =
    0x12U, /**< A second payload byte, so two writes on one bus cannot be confused. */
} i2c_fixture_t;

/**
 * @enum ra8_i2c_test_const_t
 * @brief Test addresses, channels and payload constants.
 */
typedef enum : uint8_t {
  k_ra8_i2c_test_periph  = 0x50U, /**< 7-bit peripheral address under test. */
  k_ra8_i2c_test_ch0     = 0U,    /**< Channel 0 (IIC0).                    */
  k_ra8_i2c_test_ch1     = 1U,    /**< Channel 1 (IIC1).                    */
  k_ra8_i2c_test_ch2     = 2U,    /**< Channel 2 (IIC2).                    */
  k_ra8_i2c_test_ch_oor  = 3U,    /**< Out-of-range channel.                */
  k_ra8_i2c_test_ch_huge = 200U,  /**< Far out-of-range channel.            */
  k_ra8_i2c_test_byte_a  = 0xA5U, /**< Payload byte A.                      */
  k_ra8_i2c_test_byte_b  = 0x5AU, /**< Payload byte B.                      */
  k_ra8_i2c_test_rx_byte = 0xC3U, /**< Receive payload byte.                */
} ra8_i2c_test_const_t;

/**
 * @enum ra8_i2c_test_clk_t
 * @brief Clock constants used by the bit-rate tests.
 */
typedef enum : uint32_t {
  k_ra8_i2c_test_pclkb_hz = 50000000U, /**< 50 MHz PCLKB. */
} ra8_i2c_test_clk_t;

/** @brief Standard-mode configuration descriptor. */
static const ra8_i2c_cfg_t k_i2c_cfg = {
  .bus_hz   = (uint32_t)k_ra8_i2c_speed_standard,
  .pclkb_hz = (uint32_t)k_ra8_i2c_test_pclkb_hz,
};

/** @brief Two-byte transmit payload. */
static const uint8_t s_payload[2] = {
  (uint8_t)k_ra8_i2c_test_byte_a,
  (uint8_t)k_ra8_i2c_test_byte_b,
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
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  reg->ICSR2 = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_tend |
                         (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
}

/**
 * @brief Reset the fake and refresh MSTP state before each case.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/* Deterministic NACK injection via the ra8_fake_mmio poll-hook -- it runs inline on
 * the driver's OWN poll thread, so there is no wall-clock timer and no concurrent
 * servicer thread to race or starve. The RIIC driver clears ICSR2's condition /
 * fault flags at the start of every transaction, so a pre-armed NACKF would be
 * wiped; the hook instead re-asserts TDRE|NACKF on every internal_i2c_wait_icsr2
 * poll. ra8_i2c_scan waits for TEND|NACKF in that bounded poll, so it observes the
 * injected NACKF and reports acked=false, deterministically on any host. */

/** @brief Channel the NACK poll-hook injects into. */
static uint8_t s_nack_ch;

/**
 * @brief Poll-hook body: latch TDRE|NACKF into the target channel's ICSR2.
 */
static void i2c_nack_hook(void)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(s_nack_ch);
  if (reg != nullptr) {
    const uint8_t inject =
      (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tdre | (uint8_t)k_ra8_i2c_msk_icsr2_nackf);
    reg->ICSR2 = (uint8_t)(reg->ICSR2 | inject);
  }
}

/**
 * @brief Install the NACK poll-hook for @p channel.
 */
static void i2c_nack_hook_arm(uint8_t channel)
{
  s_nack_ch = channel;
  ra8_fake_mmio_set_poll_hook(i2c_nack_hook);
}

/**
 * @brief Remove the NACK poll-hook.
 */
static void i2c_nack_hook_disarm(void)
{
  ra8_fake_mmio_set_poll_hook(nullptr);
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
  TEST_BEGIN("ra8_i2c_init: ICE set, bit rate programmed");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch1, &k_i2c_cfg));
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch1);
  TEST_ASSERT((reg->ICCR1 & (uint8_t)k_ra8_i2c_msk_iccr1_ice) != 0U);
  /* ICBRL/ICBRH carry the reserved hi bits even at the slowest rate. */
  TEST_ASSERT(reg->ICBRL != 0U);
  TEST_ASSERT(reg->ICBRH != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_deinit((uint8_t)k_ra8_i2c_test_ch1));
  TEST_END("ra8_i2c_init: ICE set, bit rate programmed");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / null contract)
 */
static void test_init_bad_inputs(void)
{
  TEST_BEGIN("ra8_i2c_init: bad inputs rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, nullptr));
  ra8_i2c_cfg_t bad = k_i2c_cfg;
  bad.bus_hz        = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &bad));
  bad          = k_i2c_cfg;
  bad.pclkb_hz = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch_oor, &k_i2c_cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch_huge, &k_i2c_cfg));
  TEST_END("ra8_i2c_init: bad inputs rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- happy-path / range contract)
 */
static void test_fast_plus_sets_fmpe(void)
{
  TEST_BEGIN("ra8_i2c_init: Fm+ sets ICFER.FMPE");
  prep();
  ra8_i2c_cfg_t cfg = {.bus_hz   = (uint32_t)k_ra8_i2c_speed_fast_plus,
                       .pclkb_hz = (uint32_t)k_ra8_i2c_test_pclkb_hz};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch2, &cfg));
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch2);
  TEST_ASSERT((reg->ICFER & (uint8_t)k_ra8_i2c_msk_icfer_fmpe) != 0U);
  TEST_END("ra8_i2c_init: Fm+ sets ICFER.FMPE");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / happy-path contract)
 */
static void test_set_clock(void)
{
  TEST_BEGIN("ra8_i2c_set_clock: reprograms divider");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_set_clock((uint8_t)k_ra8_i2c_test_ch0,
                                   (uint32_t)k_ra8_i2c_speed_fast,
                                   (uint32_t)k_ra8_i2c_test_pclkb_hz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i2c_set_clock((uint8_t)k_ra8_i2c_test_ch_oor,
                                   (uint32_t)k_ra8_i2c_speed_fast,
                                   (uint32_t)k_ra8_i2c_test_pclkb_hz));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_i2c_set_clock((uint8_t)k_ra8_i2c_test_ch0, 0U, (uint32_t)k_ra8_i2c_test_pclkb_hz));
  TEST_END("ra8_i2c_set_clock: reprograms divider");
}

/**
 * @par MC/DC:
 * (no compound decisions exercised here -- range / null contract)
 */
static void test_deinit_range(void)
{
  TEST_BEGIN("ra8_i2c_deinit: range check");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_deinit((uint8_t)k_ra8_i2c_test_ch_oor));
  TEST_END("ra8_i2c_deinit: range check");
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
  TEST_BEGIN("ra8_i2c_write: write + STOP success");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra8_i2c_test_ch0);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               sizeof(s_payload),
                               /*send_stop=*/true));
  volatile const r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  /* STOP requested via ICCR2.SP and the wire byte equals the last data. */
  TEST_ASSERT((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_sp) != 0U);
  TEST_END("ra8_i2c_write: write + STOP success");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range contract)
 */
static void test_write_bad_inputs(void)
{
  TEST_BEGIN("ra8_i2c_write: null / range rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch_oor,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               1U,
                               true));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, nullptr, 1U, true));
  TEST_END("ra8_i2c_write: null / range rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- bus-busy rejection)
 */
static void test_write_bus_busy(void)
{
  TEST_BEGIN("ra8_i2c_write: BBSY set rejects transfer");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICCR2                 = (uint8_t)k_ra8_i2c_msk_iccr2_bbsy;
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               1U,
                               true));
  TEST_END("ra8_i2c_write: BBSY set rejects transfer");
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
  TEST_BEGIN("ra8_i2c_read: multi-byte read + STOP");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra8_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICDRR                 = (uint8_t)k_ra8_i2c_test_rx_byte;
  uint8_t buf[3]             = {0U, 0U, 0U};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, buf, sizeof(buf)));
  TEST_ASSERT_EQ(k_ra8_i2c_test_rx_byte, buf[0]);
  TEST_ASSERT((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_sp) != 0U);
  TEST_END("ra8_i2c_read: multi-byte read + STOP");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- single-byte read)
 */
static void test_read_single_byte(void)
{
  TEST_BEGIN("ra8_i2c_read: single-byte read");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra8_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICDRR                 = (uint8_t)k_ra8_i2c_test_rx_byte;
  uint8_t b                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &b, 1U));
  TEST_ASSERT_EQ(k_ra8_i2c_test_rx_byte, b);
  TEST_END("ra8_i2c_read: single-byte read");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range / len)
 */
static void test_read_bad_inputs(void)
{
  TEST_BEGIN("ra8_i2c_read: null / range / zero len rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  uint8_t b = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch_oor, (uint8_t)k_ra8_i2c_test_periph, &b, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &b, 0U));
  TEST_END("ra8_i2c_read: null / range / zero len rejected");
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
  TEST_BEGIN("ra8_i2c_scan: ACK reported");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra8_i2c_test_ch0);
  bool acked = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &acked));
  TEST_ASSERT(acked);
  TEST_END("ra8_i2c_scan: ACK reported");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- NACK probe path)
 */
static void test_scan_nack(void)
{
  TEST_BEGIN("ra8_i2c_scan: NACK reported");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  /* TDRE so the address byte writes; the poll-hook re-asserts NACKF on each
   * scan poll (the start-of-transfer clear_status wipes any pre-armed NACKF,
   * so it must be injected after the driver clears it). */
  reg->ICSR2 = (uint8_t)k_ra8_i2c_msk_icsr2_tdre;
  i2c_nack_hook_arm((uint8_t)k_ra8_i2c_test_ch0);
  bool acked = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &acked));
  i2c_nack_hook_disarm();
  TEST_ASSERT(!acked);
  TEST_END("ra8_i2c_scan: NACK reported");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- null / range contract)
 */
static void test_scan_bad_inputs(void)
{
  TEST_BEGIN("ra8_i2c_scan: null / range rejected");
  prep();
  bool acked = false;
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch_oor, (uint8_t)k_ra8_i2c_test_periph, &acked));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, nullptr));
  TEST_END("ra8_i2c_scan: null / range rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions newly exercised here -- error decode / clear)
 */
static void test_errors_get_clear(void)
{
  TEST_BEGIN("ra8_i2c_get_errors / clear_errors");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICSR2   = (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_al | (uint8_t)k_ra8_i2c_msk_icsr2_nackf);
  uint8_t mask = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_get_errors((uint8_t)k_ra8_i2c_test_ch0, &mask));
  TEST_ASSERT((mask & (uint8_t)k_ra8_i2c_err_arb_lost) != 0U);
  TEST_ASSERT((mask & (uint8_t)k_ra8_i2c_err_nack) != 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_clear_errors((uint8_t)k_ra8_i2c_test_ch0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_get_errors((uint8_t)k_ra8_i2c_test_ch0, &mask));
  TEST_ASSERT_EQ(k_ra8_i2c_err_none, mask);
  /* Null / range guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_i2c_get_errors((uint8_t)k_ra8_i2c_test_ch0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_get_errors((uint8_t)k_ra8_i2c_test_ch_oor, &mask));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_i2c_clear_errors((uint8_t)k_ra8_i2c_test_ch_oor));
  TEST_END("ra8_i2c_get_errors / clear_errors");
}

/* =============================================================================
 * MC/DC vector sets
 * =============================================================================
 */

/**
 * @test test_mcdc_clk_invalid
 *
 * @par MC/DC:
 * Decision in ``ra8_i2c_internal_clk_invalid``, libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_internal_clk_invalid
 *   ``(bus_hz == 0) || (pclkb_hz == 0)`` (2 conditions, OR).
 * - V1: bus!=0, pclkb!=0 -> C1=F,C2=F -> dec F
 * - V2: bus=0,  pclkb!=0 -> C1=T (short-circuits) -> dec T (varies left)
 * - V3: bus!=0, pclkb=0  -> C1=F,C2=T -> dec T (varies right)
 * Pairs (V1,V2) flip C1; (V1,V3) flip C2. N+1 = 3 vectors.
 */
static void test_mcdc_clk_invalid(void)
{
  TEST_BEGIN("i2c MC/DC: clk_invalid OR");
  TEST_ASSERT(!ra8_i2c_internal_clk_invalid(100000U, 50000000U));
  TEST_ASSERT(ra8_i2c_internal_clk_invalid(0U, 50000000U));
  TEST_ASSERT(ra8_i2c_internal_clk_invalid(100000U, 0U));
  TEST_END("i2c MC/DC: clk_invalid OR");
}

/**
 * @test test_mcdc_transfer
 *
 * @par MC/DC:
 * Three 2-condition decisions in ``ra8_i2c_transfer``:
 * Decision A libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_transfer: ``(wr_len == 0) && (rd_len == 0)``
 * - V1: wr=0, rd=0       -> C1=T,C2=T -> dec T (-> invalid_arg)
 * - V2: wr!=0            -> C1=F (short-circuits) -> dec F
 * - V3: wr=0, rd!=0      -> C1=T,C2=F -> dec F
 * Decision B libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_transfer: ``(wr_len != 0) && (wr == nullptr)``
 * - V1: wr_len=0          -> C1=F (short-circuits) -> dec F
 * - V2: wr_len!=0, wr!=0  -> C1=T,C2=F -> dec F
 * - V3: wr_len!=0, wr=NULL-> C1=T,C2=T -> dec T (null_ptr)
 * Decision C libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_transfer: ``(rd_len != 0) && (rd == nullptr)``
 * mirrors B with rd_len/rd; same N+1 = 3 vectors.
 */
static void test_mcdc_transfer(void)
{
  TEST_BEGIN("i2c MC/DC: transfer arg-validation 2-cond decisions");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  uint8_t wr_buf[1] = {k_i2c_payload_byte};
  uint8_t rd_buf[1] = {0U};

  /* Decision A V1: both lens zero -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  wr_buf,
                                  0U,
                                  rd_buf,
                                  0U));
  /* Decision B V3: wr_len!=0 with NULL wr -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  nullptr,
                                  1U,
                                  rd_buf,
                                  0U));
  /* Decision C V3: rd_len!=0 with NULL rd (wr_len=0 keeps A=F, B=F). */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  wr_buf,
                                  0U,
                                  nullptr,
                                  1U));
  /* Range guard: out-of-range channel -> null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch_oor,
                                  (uint8_t)k_ra8_i2c_test_periph,
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
 * ``ra8_i2c_transfer`` decisions and the write-finish OR at
 * libs/ra8_hal/src/ra8_i2c.c@internal_i2c_finish_tx:
 *   ``(err != k_ra8_ok) || send_stop`` (2 conditions, OR).
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  prime_status((uint8_t)k_ra8_i2c_test_ch0);
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICDRR                 = (uint8_t)k_ra8_i2c_test_rx_byte;
  uint8_t wr_buf[1]          = {k_i2c_payload_byte_alt};
  uint8_t rd_buf[2]          = {0U, 0U};
  /* Decision F V1: write phase holds the bus (send_stop=false). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
                                  wr_buf,
                                  sizeof(wr_buf),
                                  rd_buf,
                                  sizeof(rd_buf)));
  /* Decision F V2: plain write issues STOP (send_stop=true). */
  prime_status((uint8_t)k_ra8_i2c_test_ch0);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_transfer((uint8_t)k_ra8_i2c_test_ch0,
                                  (uint8_t)k_ra8_i2c_test_periph,
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
 * libs/ra8_hal/src/ra8_i2c.c@internal_i2c_finish_tx
 *   ``(err != k_ra8_ok) || send_stop``: with TDRE never pre-armed the
 *   address send times out, so ``err != k_ra8_ok`` (C1=T) forces STOP
 *   regardless of send_stop. Complements the C1=F vectors in
 *   test_mcdc_transfer_combined.
 */
static void test_write_timeout(void)
{
  TEST_BEGIN("ra8_i2c_write: address timeout -> STOP");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  /* No prime_status: TDRE stays clear so the address wait times out. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_write((uint8_t)k_ra8_i2c_test_ch0,
                               (uint8_t)k_ra8_i2c_test_periph,
                               s_payload,
                               1U,
                               /*send_stop=*/false));
  TEST_END("ra8_i2c_write: address timeout -> STOP");
}

/**
 * @test test_read_timeout
 *
 * @par MC/DC:
 * (no compound decision under test -- the receive-drain loop guard this
 * case once anchored has been refactored in ra8_i2c_read to a
 * single-condition form, so MC/DC no longer applies. This still
 * exercises the RDRF-timeout error path: with RDRF never pre-armed the
 * dummy read times out and ra8_i2c_read returns k_ra8_err_hw_timeout.)
 */
static void test_read_timeout(void)
{
  TEST_BEGIN("ra8_i2c_read: RDRF timeout halts drain loop");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  /* TDRE so the address send succeeds, but no RDRF for the data phase. */
  volatile r_i2c_regs_t* reg = ra8_i2c_regs((uint8_t)k_ra8_i2c_test_ch0);
  reg->ICSR2                 = (uint8_t)k_ra8_i2c_msk_icsr2_tdre;
  uint8_t b                  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_read((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &b, 1U));
  TEST_END("ra8_i2c_read: RDRF timeout halts drain loop");
}

/**
 * @test test_mcdc_scan_addr_err
 *
 * @par MC/DC:
 * Decision in ``ra8_i2c_scan``, libs/ra8_hal/src/ra8_i2c.c@ra8_i2c_scan
 *   ``(err != k_ra8_ok) && (err != k_ra8_err_nack)`` (2 conditions, AND).
 * - V1 (address timeout): err=hw_timeout -> C1=T,C2=T -> dec T (hard
 *   error returned). TDRE never armed.
 * - V2 (ACK happy path, test_scan_ack): err=k_ra8_ok -> C1=F
 *   (short-circuits) -> dec F.
 * - V3 (NACK probe, test_scan_nack): err=k_ra8_err_nack -> C1=T,C2=F ->
 *   dec F (probe continues, reports acked=false).
 * Pairs (V1,V2) flip C1; (V1,V3) flip C2. N+1 = 3 vectors.
 */
static void test_mcdc_scan_addr_err(void)
{
  TEST_BEGIN("i2c MC/DC: scan address-error AND");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));

  /* V1: TDRE clear and no injection -> address wait times out, so
   * err=hw_timeout: C1=T (err != ok), C2=T (err != nack) -> dec T. */
  bool acked = false;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &acked));

  /* V3: inject TDRE + NACKF so send_address reads NACKF and returns
   * k_ra8_err_nack: C1=T (err != ok), C2=F (err == nack) -> dec F; the
   * probe then continues and reports acked=false. */
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i2c_init((uint8_t)k_ra8_i2c_test_ch0, &k_i2c_cfg));
  i2c_nack_hook_arm((uint8_t)k_ra8_i2c_test_ch0);
  acked = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_i2c_scan((uint8_t)k_ra8_i2c_test_ch0, (uint8_t)k_ra8_i2c_test_periph, &acked));
  i2c_nack_hook_disarm();
  TEST_ASSERT(!acked);
  TEST_END("i2c MC/DC: scan address-error AND");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_init_configured,  test_init_bad_inputs, test_fast_plus_sets_fmpe,
  test_set_clock,        test_deinit_range,    test_write_happy,
  test_write_bad_inputs, test_write_bus_busy,  test_read_happy,
  test_read_single_byte, test_read_bad_inputs, test_scan_ack,
  test_scan_nack,        test_scan_bad_inputs, test_errors_get_clear,
  test_mcdc_clk_invalid, test_mcdc_transfer,   test_mcdc_transfer_combined,
  test_write_timeout,    test_read_timeout,    test_mcdc_scan_addr_err,
};

/**
 * @brief Run every test case in this translation unit.
 *
 * @return 0 when all cases pass (Unity aborts the process on failure).
 *
 * @pre The ``ra8_fake_mmap`` constructor has installed the MMIO windows.
 * @post Every registered case has executed.
 * @note Thread safety: single-threaded test harness.
 * @since 0.1.0
 */
int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
