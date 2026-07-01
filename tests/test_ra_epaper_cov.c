/**
 * @file test_ra_epaper_cov.c
 * @brief White-box line-coverage tests for the IT8951 e-paper SPI driver
 *        (``ra_epaper.c``).
 *
 * @details
 * The public happy-path / rejection contract of ``ra_epaper.c`` is already
 * exercised by ``test_ra_epaper.c`` through the ``ra_core_hal`` link. That
 * production copy runs in ``RA_SIMULATOR_MODE`` where the SPI bus never
 * fails, so every mid-sequence ``if (err != k_ra_ok) return err;`` leg in
 * the send / receive / command / register helpers stays dark. Those legs are
 * the driver's real defensive logic and this TU proves each one is testable.
 *
 * To reach them the module is compiled a second time here as a private,
 * instrumented copy: this TU ``#include``s the module source with its two
 * wire dependencies -- ``ra_spi_init`` and ``ra_spi_xfer8`` -- redirected via
 * preprocessor rename to deterministic, fault-injecting mocks. The mock byte
 * shifter counts every 8-bit transfer and returns ``k_ra_err_hw_error`` on a
 * chosen 1-based call index, so any single transaction in a longer sequence
 * can be failed precisely, driving the corresponding error leg. The four
 * exported symbols (``ra_epaper_init`` / ``_load_image`` / ``_display_area``
 * / ``_sleep``) are renamed to ``*_cov`` so they do not collide with the
 * production copies linked from ``ra_core_hal``; no CMake edit is needed
 * because the file is glob-discovered by ``ra_add_test``.
 *
 * The static helpers (``internal_ra_epaper_*``) become callable in this TU
 * once the source is included, so each helper's error legs are driven
 * directly on a fresh transfer counter. No hardware line is bypassed by an
 * exclusion marker from here; the only exclusions live in the module source
 * and cover legs that are provably dead under ``RA_SIMULATOR_MODE`` (the HRDY
 * hardware poll is compiled out, so ``internal_ra_epaper_wait_ready`` cannot
 * fail on the host, and the display LUT loop always returns on iteration 0).
 *
 * @par Tag
 * [Ring 3 / HAL]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_epaper.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_port_utils.h"
#include "ra_spi.h"
#include "ra_time.h"
#include "unity_minimal.h"

/**
 * @enum ra_epaper_cov_const_t
 * @brief Named fixtures: fault-injection call indices, geometry and byte
 *        stimulus used by the coverage vectors.
 *
 * @details
 * The ``k_fail_*`` values are 1-based ``ra_spi_xfer8`` call indices computed
 * from the transfer counts of each helper (send16 = 2 transfers, a command /
 * data word = 4, a data read = 6, a register write = 12, a register read =
 * 14). Failing at exactly one index lands the fault on one specific leg.
 */
typedef enum : uint32_t {
  k_cov_baud_hz    = 12000000U,  /**< 12 MHz SPI clock (valid).       */
  k_cov_pclka_hz   = 100000000U, /**< 100 MHz PCLKA.                  */
  k_cov_baud_huge  = 100000000U, /**< Above the 24 MHz ceiling.       */
  k_cov_panel_w    = 8U,         /**< Test area width.                */
  k_cov_panel_h    = 8U,         /**< Test area height.               */
  k_cov_buf_len    = 64U,        /**< 8x8 = 64-byte pixel buffer.     */
  k_cov_even_len   = 4U,         /**< Two-word even buffer.           */
  k_cov_odd_len    = 1U,         /**< One-byte odd-tail buffer.       */
  k_cov_odd_len3   = 3U,         /**< Three-byte odd-tail buffer.     */
  k_cov_rx_zero    = 0x00U,      /**< Mock rx byte yielding status 0. */
  k_cov_rx_nz      = 0x5AU,      /**< Mock rx byte yielding non-zero. */
  k_cov_rx_busy    = 0xABU,      /**< Mock rx byte for a busy LUT.    */
  k_cov_word_nz    = 0x5A5AU,    /**< Word rebuilt from k_cov_rx_nz.  */
  k_cov_reg_addr   = 0x0208U,    /**< Arbitrary register address.     */
  k_cov_reg_val    = 0x1234U,    /**< Arbitrary register value.       */
  k_cov_fail_1     = 1U,         /**< First transfer of a helper.     */
  k_cov_fail_2     = 2U,         /**< Second transfer (send16 low).   */
  k_cov_fail_3     = 3U,         /**< Third transfer.                 */
  k_cov_fail_5     = 5U,         /**< Fifth transfer.                 */
  k_cov_fail_9     = 9U,         /**< Ninth transfer.                 */
  k_cov_fail_13    = 13U,        /**< Thirteenth transfer.            */
  k_cov_fail_17    = 17U,        /**< Seventeenth transfer.           */
  k_cov_fail_25    = 25U,        /**< Twenty-fifth transfer.          */
  k_cov_fail_29    = 29U,        /**< Twenty-ninth transfer.          */
  k_cov_fail_49    = 49U,        /**< Forty-ninth transfer.           */
  k_cov_fail_never = 0U,         /**< Disable fault injection.        */
} ra_epaper_cov_const_t;

/* =============================================================================
 * Fault-injecting SPI mocks (redirected into the included source below).
 * =============================================================================
 */

/** @brief Running count of ::mock_spi_xfer8 calls since the last ::arm_fault. */
static uint32_t s_xfer_calls;
/** @brief 1-based transfer index to fail at; 0 disables fault injection. */
static uint32_t s_xfer_fail_at;
/** @brief Byte stored into every ``*rx`` on a successful transfer. */
static uint8_t s_xfer_rx;
/** @brief When true, ::mock_spi_init reports an init failure. */
static bool s_spi_init_fail;

/**
 * @brief Deterministic stand-in for ``ra_spi_xfer8``.
 *
 * @details Ignores the channel, stamps @p rx with ::s_xfer_rx, and fails the
 * transfer whose 1-based index equals ::s_xfer_fail_at (when non-zero). This
 * is the whole fault seam: it needs no hardware and no timing.
 *
 * @param[in]  channel Ignored SPI channel selector.
 * @param[in]  tx      Ignored transmit byte.
 * @param[out] rx      Optional receive slot; stamped with ::s_xfer_rx.
 * @return ``k_ra_ok`` normally, ``k_ra_err_hw_error`` on the armed index.
 * @retval k_ra_ok           Transfer accepted.
 * @retval k_ra_err_hw_error Armed fault index reached.
 * @pre  Never called before ::arm_fault primes the counter.
 * @pre  @p rx is either null or writable.
 * @post ::s_xfer_calls advanced by one.
 * @post ``*rx`` holds ::s_xfer_rx when @p rx is non-null.
 */
static ra_err_t mock_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx)
{
  (void)channel;
  (void)tx;
  s_xfer_calls = s_xfer_calls + 1U;
  if (rx != nullptr) {
    *rx = s_xfer_rx;
  }
  if ((s_xfer_fail_at != 0U) && (s_xfer_calls == s_xfer_fail_at)) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/**
 * @brief Deterministic stand-in for ``ra_spi_init``.
 *
 * @param[in] channel Ignored SPI channel selector.
 * @param[in] cfg     Ignored SPI configuration descriptor.
 * @return ``k_ra_err_hw_init_failed`` when ::s_spi_init_fail, else ``k_ra_ok``.
 * @retval k_ra_ok               Init accepted.
 * @retval k_ra_err_hw_init_failed Init rejected on request.
 * @pre  @p cfg is a valid pointer supplied by the driver.
 * @pre  ::s_spi_init_fail reflects the intended outcome.
 * @post No hardware state is touched.
 * @post Return value mirrors ::s_spi_init_fail.
 */
static ra_err_t mock_spi_init(uint8_t channel, const ra_spi_cfg_t* cfg)
{
  (void)channel;
  (void)cfg;
  return s_spi_init_fail ? k_ra_err_hw_init_failed : k_ra_ok;
}

/* Rename the four exported symbols so the instrumented copy does not clash
 * with the production driver linked from ra_core_hal, and redirect the two
 * SPI wire calls to the deterministic mocks above. */
ra_err_t ra_epaper_init_cov(const ra_epaper_cfg_t* cfg);
ra_err_t ra_epaper_load_image_cov(const ra_epaper_area_t* area,
                                  const uint8_t*          buf,
                                  size_t                  buf_len,
                                  ra_epaper_endian_t      endian);
ra_err_t ra_epaper_display_area_cov(const ra_epaper_area_t* area, ra_epaper_waveform_t waveform);
ra_err_t ra_epaper_sleep_cov(void);

#define ra_epaper_init         ra_epaper_init_cov
#define ra_epaper_load_image   ra_epaper_load_image_cov
#define ra_epaper_display_area ra_epaper_display_area_cov
#define ra_epaper_sleep        ra_epaper_sleep_cov
#define ra_spi_init            mock_spi_init
#define ra_spi_xfer8           mock_spi_xfer8

#include "ra_epaper.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/* =============================================================================
 * Fixture helpers -- all deterministic; no timers, no sleeps.
 * =============================================================================
 */

/**
 * @brief Arm the transfer counter to fail at @p n (0 => never fail).
 * @param[in] n 1-based ``ra_spi_xfer8`` call index to fault; 0 disables.
 */
static void arm_fault(uint32_t n)
{
  s_xfer_calls   = 0U;
  s_xfer_fail_at = n;
}

/** @brief Build an otherwise-valid config for the cov init path. */
static ra_epaper_cfg_t cov_cfg(void)
{
  const ra_epaper_cfg_t cfg = {
    .spi_channel  = 0U,
    .spi_baud_hz  = (uint32_t)k_cov_baud_hz,
    .pclka_hz     = (uint32_t)k_cov_pclka_hz,
    .reset_pin    = 0U,
    .busy_pin     = 0U,
    .panel_width  = (uint16_t)k_cov_panel_w,
    .panel_height = (uint16_t)k_cov_panel_h,
  };
  return cfg;
}

/** @brief Force the file-static panel back to the uninitialized state. */
static void set_uninit(void)
{
  s_panel.state = k_ra_epaper_state_uninit;
}

/** @brief Force the file-static panel to the ready state with a valid cfg. */
static void set_ready(void)
{
  s_panel.cfg   = cov_cfg();
  s_panel.state = k_ra_epaper_state_ready;
}

/* =============================================================================
 * Direct helper tests -- each drives one helper's error legs on the mock.
 * =============================================================================
 */

/**
 * @test test_send16_recv16_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- ``internal_ra_epaper_send16``
 * and ``internal_ra_epaper_recv16`` chain single-condition ``err != k_ra_ok``
 * guards; no ``&&`` / ``||``.)
 */
static void test_send16_recv16_legs(void)
{
  TEST_BEGIN("send16/recv16: high-byte fault, low-byte fault, and clean word");
  s_xfer_rx = (uint8_t)k_cov_rx_nz;

  /* send16: fail the high byte -> early return leg. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_send16((uint16_t)k_cov_reg_val));
  /* send16: fail the low byte -> tail-return propagates the error. */
  arm_fault((uint32_t)k_cov_fail_2);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_send16((uint16_t)k_cov_reg_val));
  /* send16: both bytes clock out -> ok. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_send16((uint16_t)k_cov_reg_val));

  /* recv16: fail the high byte, then the low byte, then a clean word. */
  uint16_t word = 0U;
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_recv16(&word));
  arm_fault((uint32_t)k_cov_fail_2);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_recv16(&word));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_recv16(&word));
  TEST_ASSERT_EQ((uint16_t)k_cov_word_nz, word);

  TEST_END("send16/recv16: high-byte fault, low-byte fault, and clean word");
}

/**
 * @test test_wait_ready_sim_leg
 *
 * @par MC/DC:
 * (no compound decisions -- under ``RA_SIMULATOR_MODE`` the helper is a single
 * unconditional ``return k_ra_ok``; the HRDY poll loop is compiled out.)
 */
static void test_wait_ready_sim_leg(void)
{
  TEST_BEGIN("wait_ready: host build short-circuits to k_ra_ok");
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_wait_ready());
  TEST_END("wait_ready: host build short-circuits to k_ra_ok");
}

/**
 * @test test_write_cmd_data_read_legs
 *
 * @par MC/DC:
 * (no compound decisions -- the command / data-write / data-read helpers are
 * straight-line sequences of single-condition ``err != k_ra_ok`` guards.)
 */
static void test_write_cmd_data_read_legs(void)
{
  TEST_BEGIN("write_cmd/write_data16/read_data16: preamble and payload faults");
  s_xfer_rx = (uint8_t)k_cov_rx_nz;

  /* write_cmd: fault the preamble send16, then the command send16, then ok. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_write_cmd((uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_3);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_write_cmd((uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_write_cmd((uint16_t)k_cov_reg_val));

  /* write_data16: same shape -- preamble fault, payload fault, then ok. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_write_data16((uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_3);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_write_data16((uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_write_data16((uint16_t)k_cov_reg_val));

  /* read_data16: preamble fault, dummy-word fault, output-word fault, then ok. */
  uint16_t word = 0U;
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_read_data16(&word));
  arm_fault((uint32_t)k_cov_fail_3);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_read_data16(&word));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_read_data16(&word));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_read_data16(&word));
  TEST_ASSERT_EQ((uint16_t)k_cov_word_nz, word);

  TEST_END("write_cmd/write_data16/read_data16: preamble and payload faults");
}

/**
 * @test test_reg_write_read_legs
 *
 * @par MC/DC:
 * (no compound decisions -- register write / read chain three single-condition
 * guards over their sub-transactions.)
 */
static void test_reg_write_read_legs(void)
{
  TEST_BEGIN("reg_write/reg_read: fault each of the three sub-transactions");
  s_xfer_rx = (uint8_t)k_cov_rx_nz;

  /* reg_write: fault write_cmd, then addr write_data16, then value write. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_reg_write((uint16_t)k_cov_reg_addr, (uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_reg_write((uint16_t)k_cov_reg_addr, (uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_9);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_reg_write((uint16_t)k_cov_reg_addr, (uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok,
                 internal_ra_epaper_reg_write((uint16_t)k_cov_reg_addr, (uint16_t)k_cov_reg_val));

  /* reg_read: fault write_cmd, then addr write_data16, then read_data16. */
  uint16_t val = 0U;
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_reg_read((uint16_t)k_cov_reg_addr, &val));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_reg_read((uint16_t)k_cov_reg_addr, &val));
  arm_fault((uint32_t)k_cov_fail_9);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_reg_read((uint16_t)k_cov_reg_addr, &val));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_reg_read((uint16_t)k_cov_reg_addr, &val));
  TEST_ASSERT_EQ((uint16_t)k_cov_word_nz, val);

  TEST_END("reg_write/reg_read: fault each of the three sub-transactions");
}

/**
 * @test test_drain_dev_info_legs
 *
 * @par MC/DC:
 * (no compound decisions -- the GET_DEV_INFO drain is a bounded loop of
 * single-condition read guards.)
 */
static void test_drain_dev_info_legs(void)
{
  TEST_BEGIN("drain_dev_info: command fault, in-loop read fault, clean drain");
  s_xfer_rx = (uint8_t)k_cov_rx_zero;

  /* Fault the GET_DEV_INFO command. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_drain_dev_info());
  /* Fault the first read of the 20-word drain (command spent 4 transfers). */
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_drain_dev_info());
  /* Clean drain of all 20 words. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_drain_dev_info());

  TEST_END("drain_dev_info: command fault, in-loop read fault, clean drain");
}

/**
 * @test test_load_display_args_legs
 *
 * @par MC/DC:
 * (no compound decisions -- the argument blocks are five sequential
 * single-condition write guards each.)
 */
static void test_load_display_args_legs(void)
{
  TEST_BEGIN("send_load_args/send_display_args: fault every argument word");
  const ra_epaper_area_t area = {.x = 0U, .y = 0U, .width = 1U, .height = 1U};
  s_xfer_rx                   = (uint8_t)k_cov_rx_zero;

  /* send_load_args: fault arg0, x, y, width, height, then a clean block. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_load_args(&area, k_ra_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_load_args(&area, k_ra_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_9);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_load_args(&area, k_ra_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_13);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_load_args(&area, k_ra_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_17);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_load_args(&area, k_ra_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_send_load_args(&area, k_ra_epaper_endian_little));

  /* send_display_args: fault x, y, width, height, waveform, then a clean block. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_display_args(&area, k_ra_epaper_wf_gc16));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_display_args(&area, k_ra_epaper_wf_gc16));
  arm_fault((uint32_t)k_cov_fail_9);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_display_args(&area, k_ra_epaper_wf_gc16));
  arm_fault((uint32_t)k_cov_fail_13);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_display_args(&area, k_ra_epaper_wf_gc16));
  arm_fault((uint32_t)k_cov_fail_17);
  TEST_ASSERT_EQ(k_ra_err_hw_error,
                 internal_ra_epaper_send_display_args(&area, k_ra_epaper_wf_gc16));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_send_display_args(&area, k_ra_epaper_wf_gc16));

  TEST_END("send_load_args/send_display_args: fault every argument word");
}

/**
 * @test test_stream_pixels_even_odd
 *
 * @par MC/DC:
 * Decision: ``if ((buf_len & 1U) != 0U)`` in ``internal_ra_epaper_stream_pixels``
 * (1 condition -- odd-tail predicate). N+1 = 2 vectors:
 * - V1: buf_len even (4) -> predicate false, no tail word emitted.
 * - V2: buf_len odd (1)  -> predicate true, padded tail word emitted.
 * The even-buffer mid-loop fault also exercises the loop-body error guard.
 */
static void test_stream_pixels_even_odd(void)
{
  TEST_BEGIN("stream_pixels: even buffer, mid-loop fault, and odd-tail pad");
  const uint8_t buf[k_cov_buf_len] = {};
  s_xfer_rx                        = (uint8_t)k_cov_rx_zero;

  /* Even length streams whole words only -> tail predicate false, ok. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_stream_pixels(buf, (size_t)k_cov_even_len));
  /* Even length, fault the first word write -> loop-body error leg. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, internal_ra_epaper_stream_pixels(buf, (size_t)k_cov_even_len));
  /* Odd length emits the padded tail word -> tail predicate true, ok. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_stream_pixels(buf, (size_t)k_cov_odd_len));
  /* Odd length that also walks the loop once before the tail. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, internal_ra_epaper_stream_pixels(buf, (size_t)k_cov_odd_len3));

  TEST_END("stream_pixels: even buffer, mid-loop fault, and odd-tail pad");
}

/* =============================================================================
 * Public-API tests -- drive each API's own error short-circuits.
 * =============================================================================
 */

/**
 * @test test_init_ladder_legs
 *
 * @par MC/DC:
 * (no compound decisions on the exercised legs -- ``ra_epaper_init`` guards
 * spi_init, SYS_RUN and drain with single-condition ``err`` checks; its only
 * compound decision, ``validate_cfg``, is covered by ::test_validate_and_size_mcdc.)
 */
static void test_init_ladder_legs(void)
{
  TEST_BEGIN("init: spi_init fault, SYS_RUN fault, drain fault, clean bring-up");
  const ra_epaper_cfg_t cfg = cov_cfg();
  s_xfer_rx                 = (uint8_t)k_cov_rx_zero;

  /* spi_init reports failure -> hw_init_failed leg. */
  set_uninit();
  s_spi_init_fail = true;
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_err_hw_init_failed, ra_epaper_init_cov(&cfg));
  s_spi_init_fail = false;

  /* SYS_RUN command faults (its first transfer) -> propagated. */
  set_uninit();
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_epaper_init_cov(&cfg));

  /* drain_dev_info faults on its command (transfer 5, after SYS_RUN's 4). */
  set_uninit();
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_epaper_init_cov(&cfg));

  /* Clean bring-up -> ready. */
  set_uninit();
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_init_cov(&cfg));
  TEST_ASSERT_EQ((int)k_ra_epaper_state_ready, (int)s_panel.state);

  TEST_END("init: spi_init fault, SYS_RUN fault, drain fault, clean bring-up");
}

/**
 * @test test_load_image_api_legs
 *
 * @par MC/DC:
 * (no compound decisions on the exercised legs -- each LISAR write, the
 * LD_IMG_AREA command, the argument block and the pixel stream are guarded by
 * a single-condition ``err`` check; the size decision is covered by
 * ::test_validate_and_size_mcdc.)
 */
static void test_load_image_api_legs(void)
{
  TEST_BEGIN("load_image: fault LISAR lo/hi, cmd, args, and pixel stream");
  const uint8_t          buf[k_cov_buf_len] = {};
  const ra_epaper_area_t area               = {.x      = 0U,
                                               .y      = 0U,
                                               .width  = (uint16_t)k_cov_panel_w,
                                               .height = (uint16_t)k_cov_panel_h};
  s_xfer_rx                                 = (uint8_t)k_cov_rx_zero;

  /* LISAR-low reg_write fault (transfer 1). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(
    k_ra_err_hw_error,
    ra_epaper_load_image_cov(&area, buf, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));
  /* LISAR-high reg_write fault (transfer 13, after LISAR-low's 12). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_13);
  TEST_ASSERT_EQ(
    k_ra_err_hw_error,
    ra_epaper_load_image_cov(&area, buf, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));
  /* LD_IMG_AREA command fault (transfer 25, after two reg writes = 24). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_25);
  TEST_ASSERT_EQ(
    k_ra_err_hw_error,
    ra_epaper_load_image_cov(&area, buf, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));
  /* send_load_args fault (transfer 29, after cmd's 4 = 28). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_29);
  TEST_ASSERT_EQ(
    k_ra_err_hw_error,
    ra_epaper_load_image_cov(&area, buf, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));
  /* stream_pixels fault (transfer 49, after args' 20 = 48). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_49);
  TEST_ASSERT_EQ(
    k_ra_err_hw_error,
    ra_epaper_load_image_cov(&area, buf, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));
  /* Clean load through LD_IMG_END. */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_epaper_load_image_cov(&area, buf, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));

  TEST_END("load_image: fault LISAR lo/hi, cmd, args, and pixel stream");
}

/**
 * @test test_display_area_api_legs
 *
 * @par MC/DC:
 * (no compound decisions on the exercised legs -- the DPY_AREA command, the
 * argument block and the in-loop LUT read are single-condition guards; the
 * ``status == 0`` early-exit and the ``RA_SIMULATOR_MODE`` bail are both
 * single-condition too.)
 */
static void test_display_area_api_legs(void)
{
  TEST_BEGIN("display_area: cmd fault, args fault, LUT read fault, both exits");
  const ra_epaper_area_t area = {.x      = 0U,
                                 .y      = 0U,
                                 .width  = (uint16_t)k_cov_panel_w,
                                 .height = (uint16_t)k_cov_panel_h};

  /* DPY_AREA command fault (transfer 1). */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_zero;
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_epaper_display_area_cov(&area, k_ra_epaper_wf_gc16));
  /* send_display_args fault (transfer 5, after cmd's 4). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_epaper_display_area_cov(&area, k_ra_epaper_wf_gc16));
  /* LUT reg_read fault (transfer 25, after cmd 4 + args 20 = 24). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_25);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_epaper_display_area_cov(&area, k_ra_epaper_wf_gc16));

  /* Clean refresh, LUT reads back zero -> status==0 early exit. */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_zero;
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_display_area_cov(&area, k_ra_epaper_wf_gc16));
  /* Clean refresh, LUT reads back busy -> RA_SIMULATOR_MODE single-iteration bail. */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_busy;
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_display_area_cov(&area, k_ra_epaper_wf_gc16));

  TEST_END("display_area: cmd fault, args fault, LUT read fault, both exits");
}

/**
 * @test test_sleep_legs
 *
 * @par MC/DC:
 * (no compound decisions -- ``ra_epaper_sleep`` is a single-condition state
 * guard followed by a straight-line SLEEP command whose result is returned
 * verbatim after the state is reset.)
 */
static void test_sleep_legs(void)
{
  TEST_BEGIN("sleep: rejected pre-ready, clean sleep, and SLEEP-command fault");

  /* Not ready -> invalid_state. */
  set_uninit();
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_sleep_cov());

  /* Ready + clean SLEEP command -> ok, state falls back to uninit. */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_zero;
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_sleep_cov());
  TEST_ASSERT_EQ((int)k_ra_epaper_state_uninit, (int)s_panel.state);

  /* Ready + faulted SLEEP command -> error surfaced, state still reset. */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_epaper_sleep_cov());
  TEST_ASSERT_EQ((int)k_ra_epaper_state_uninit, (int)s_panel.state);

  TEST_END("sleep: rejected pre-ready, clean sleep, and SLEEP-command fault");
}

/**
 * @test test_validate_and_size_mcdc
 *
 * @par MC/DC:
 * Decision A: ``internal_ra_epaper_validate_cfg``
 * ``if ((spi_channel > 1) || (panel_w == 0) || (panel_h == 0) ||
 *      (panel_w > MAX) || (panel_h > MAX) || (baud == 0) || (baud > BAUD_MAX))``
 * (7 conditions, ``||`` short-circuit chain). N+1 = 8 vectors: an all-false
 * baseline that accepts, plus one vector per condition that flips exactly
 * that Ck true while every other Ci stays false, proving independent
 * influence pair-wise against the baseline.
 * - V0: all Ci=F                 -> accept (init reaches ready)
 * - V1: spi_channel=99           -> reject
 * - V2: panel_width=0            -> reject
 * - V3: panel_height=0           -> reject
 * - V4: panel_width=0xFFFF>MAX   -> reject
 * - V5: panel_height=0xFFFF>MAX  -> reject
 * - V6: spi_baud_hz=0            -> reject
 * - V7: spi_baud_hz>BAUD_MAX     -> reject
 *
 * Decision B: ``ra_epaper_load_image`` ``if ((buf_len != expect) ||
 * (expect == 0U))`` (2 conditions, ``||``). N+1 = 3 vectors:
 * - V1: buf_len==expect, expect>0 -> accept
 * - V2: buf_len!=expect           -> reject (varies buf_len only)
 * - V3: expect==0 (0x0 area)      -> reject (varies expect only)
 */
static void test_validate_and_size_mcdc(void)
{
  TEST_BEGIN("validate_cfg 7-cond + load_image size 2-cond MC/DC");
  s_xfer_rx       = (uint8_t)k_cov_rx_zero;
  s_spi_init_fail = false;
  arm_fault((uint32_t)k_cov_fail_never);

  /* V0: baseline accept. */
  set_uninit();
  ra_epaper_cfg_t cfg = cov_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_epaper_init_cov(&cfg));

  /* V1..V7: flip exactly one condition true, expect rejection. */
  set_uninit();
  cfg             = cov_cfg();
  cfg.spi_channel = 99U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init_cov(&cfg));
  set_uninit();
  cfg             = cov_cfg();
  cfg.panel_width = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init_cov(&cfg));
  set_uninit();
  cfg              = cov_cfg();
  cfg.panel_height = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init_cov(&cfg));
  set_uninit();
  cfg             = cov_cfg();
  cfg.panel_width = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init_cov(&cfg));
  set_uninit();
  cfg              = cov_cfg();
  cfg.panel_height = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init_cov(&cfg));
  set_uninit();
  cfg             = cov_cfg();
  cfg.spi_baud_hz = 0U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init_cov(&cfg));
  set_uninit();
  cfg             = cov_cfg();
  cfg.spi_baud_hz = (uint32_t)k_cov_baud_huge;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_epaper_init_cov(&cfg));

  /* Decision B: size check on a ready panel. */
  set_ready();
  const uint8_t          buf[k_cov_buf_len] = {};
  const ra_epaper_area_t area               = {.x      = 0U,
                                               .y      = 0U,
                                               .width  = (uint16_t)k_cov_panel_w,
                                               .height = (uint16_t)k_cov_panel_h};
  /* V1: exact size accepts. */
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_epaper_load_image_cov(&area, buf, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));
  /* V2: wrong size rejects. */
  set_ready();
  TEST_ASSERT_EQ(
    k_ra_err_invalid_size,
    ra_epaper_load_image_cov(&area, buf, (size_t)k_cov_even_len, k_ra_epaper_endian_little));
  /* V3: zero-area rejects. */
  set_ready();
  const ra_epaper_area_t area0 = {.x = 0U, .y = 0U, .width = 0U, .height = 0U};
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_epaper_load_image_cov(&area0, buf, 0U, k_ra_epaper_endian_little));

  /* Null-argument guards on both entry points. */
  set_ready();
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_epaper_load_image_cov(nullptr, buf, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_epaper_load_image_cov(&area, nullptr, (size_t)k_cov_buf_len, k_ra_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epaper_display_area_cov(nullptr, k_ra_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_epaper_init_cov(nullptr));

  /* Double-init rejection: a second init while ready. */
  set_ready();
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_epaper_init_cov(&cfg));

  set_uninit();
  TEST_END("validate_cfg 7-cond + load_image size 2-cond MC/DC");
}

int32_t main(void)
{
  test_send16_recv16_legs();
  test_wait_ready_sim_leg();
  test_write_cmd_data_read_legs();
  test_reg_write_read_legs();
  test_drain_dev_info_legs();
  test_load_display_args_legs();
  test_stream_pixels_even_odd();
  test_init_ladder_legs();
  test_load_image_api_legs();
  test_display_area_api_legs();
  test_sleep_legs();
  test_validate_and_size_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra_epaper_cov.c\n");
  return 0;
}
