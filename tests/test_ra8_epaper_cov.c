/**
 * @file test_ra8_epaper_cov.c
 * @brief White-box line-coverage tests for the IT8951 e-paper SPI driver
 *        (``ra8_epaper.c``).
 *
 * @details
 * The public happy-path / rejection contract of ``ra8_epaper.c`` is already
 * exercised by ``test_ra8_epaper.c`` through the ``ra8_core_hal`` link. That
 * production copy runs in ``RA8_SIMULATOR_MODE`` where the SPI bus never
 * fails, so every mid-sequence ``if (err != k_ra8_ok) return err;`` leg in
 * the send / receive / command / register helpers stays dark. Those legs are
 * the driver's real defensive logic and this TU proves each one is testable.
 *
 * To reach them the module is compiled a second time here as a private,
 * instrumented copy: this TU ``#include``s the module source and points its
 * wire dependency -- the injected SPI bus seam (``ra8_spi_bus_ops_t``) -- at
 * a deterministic, fault-injecting mock byte shifter. The mock counts every
 * 8-bit transfer and returns ``k_ra8_err_hw_error`` on a chosen 1-based call
 * index, so any single transaction in a longer sequence can be failed
 * precisely, driving the corresponding error leg. The four exported symbols
 * (``ra8_epaper_init`` / ``_load_image`` / ``_display_area`` / ``_sleep``)
 * are renamed to ``*_cov`` so they do not collide with the production copies
 * linked from ``ra8_core_hal``; no CMake edit is needed because the file is
 * glob-discovered by ``ra8_add_test``.
 *
 * The static helpers (``internal_ra8_epaper_*``) become callable in this TU
 * once the source is included, so each helper's error legs are driven
 * directly on a fresh transfer counter. No hardware line is bypassed by an
 * exclusion marker from here; the only exclusions live in the module source
 * and cover redundant timeout re-raises whose underlying timeout leg is
 * already seam-driven. The HRDY wait, the /RESET GPIO pulse, and the
 * display LUT-idle loop all run for real on host (issues #177 / #238):
 * this TU drives the LUT loop's idle-success exit (mock RX 0) and its
 * budget-exhaustion timeout (mock RX always busy), and the HRDY wait's
 * first-poll success through the unarmed ``ra8_sim_mmio`` seam.
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

#include "ra8_check.h"
#include "ra8_epaper.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_port_utils.h"
#include "ra8_sim_mmio.h"
#include "ra8_spi_bus_ops.h"
#include "ra8_time.h"
#include "unity_minimal.h"

/**
 * @enum ra8_epaper_cov_const_t
 * @brief Named fixtures: fault-injection call indices, geometry and byte
 *        stimulus used by the coverage vectors.
 *
 * @details
 * The ``k_fail_*`` values are 1-based ``ra8_spi_xfer8`` call indices computed
 * from the transfer counts of each helper (send16 = 2 transfers, a command /
 * data word = 4, a data read = 6, a register write = 12, a register read =
 * 14). Failing at exactly one index lands the fault on one specific leg.
 */
typedef enum : uint32_t {
  k_cov_panel_w      = 8U,      /**< Test area width.                */
  k_cov_wf_gc16_mode = 2U,      /**< Resolved GC16 LUT mode number.  */
  k_cov_panel_h      = 8U,      /**< Test area height.               */
  k_cov_buf_len      = 64U,     /**< 8x8 = 64-byte pixel buffer.     */
  k_cov_even_len     = 4U,      /**< Two-word even buffer.           */
  k_cov_odd_len      = 1U,      /**< One-byte odd-tail buffer.       */
  k_cov_odd_len3     = 3U,      /**< Three-byte odd-tail buffer.     */
  k_cov_rx_zero      = 0x00U,   /**< Mock rx byte yielding status 0. */
  k_cov_rx_nz        = 0x5AU,   /**< Mock rx byte yielding non-zero. */
  k_cov_rx_busy      = 0xABU,   /**< Mock rx byte for a busy LUT.    */
  k_cov_word_nz      = 0x5A5AU, /**< Word rebuilt from k_cov_rx_nz.  */
  k_cov_reg_addr     = 0x0208U, /**< Arbitrary register address.     */
  k_cov_reg_val      = 0x1234U, /**< Arbitrary register value.       */
  k_cov_fail_1       = 1U,      /**< First transfer of a helper.     */
  k_cov_fail_2       = 2U,      /**< Second transfer (send16 low).   */
  k_cov_fail_3       = 3U,      /**< Third transfer.                 */
  k_cov_fail_5       = 5U,      /**< Fifth transfer.                 */
  k_cov_fail_9       = 9U,      /**< Ninth transfer.                 */
  k_cov_fail_13      = 13U,     /**< Thirteenth transfer.            */
  k_cov_fail_17      = 17U,     /**< Seventeenth transfer.           */
  k_cov_fail_25      = 25U,     /**< Twenty-fifth transfer.          */
  k_cov_fail_29      = 29U,     /**< Twenty-ninth transfer.          */
  k_cov_fail_49      = 49U,     /**< Forty-ninth transfer.           */
  k_cov_fail_never   = 0U,      /**< Disable fault injection.        */
} ra8_epaper_cov_const_t;

/* =============================================================================
 * Fault-injecting SPI mocks (redirected into the included source below).
 * =============================================================================
 */

/** @brief Running count of ::mock_bus_xfer8 calls since the last ::arm_fault. */
static uint32_t s_xfer_calls;
/** @brief 1-based transfer index to fail at; 0 disables fault injection. */
static uint32_t s_xfer_fail_at;
/** @brief Byte stored into every ``*rx`` on a successful transfer. */
static uint8_t s_xfer_rx;
/** @brief Stable non-register object whose address keys the LUT-poll seam. */
static uint8_t s_bus_probe;

/**
 * @brief Deterministic ``xfer8`` row for the driver's injected bus seam.
 *
 * @details Ignores the seam context, stamps @p rx with ::s_xfer_rx, and fails
 * the transfer whose 1-based index equals ::s_xfer_fail_at (when non-zero).
 * This is the whole fault seam: it needs no hardware and no timing.
 *
 * @param[in]  ctx Ignored seam cookie (::s_bus_probe in the fixture cfg).
 * @param[in]  tx  Ignored transmit byte.
 * @param[out] rx  Optional receive slot; stamped with ::s_xfer_rx.
 * @return ``k_ra8_ok`` normally, ``k_ra8_err_hw_error`` on the armed index.
 * @retval k_ra8_ok           Transfer accepted.
 * @retval k_ra8_err_hw_error Armed fault index reached.
 * @pre  Never called before ::arm_fault primes the counter.
 * @pre  @p rx is either null or writable.
 * @post ::s_xfer_calls advanced by one.
 * @post ``*rx`` holds ::s_xfer_rx when @p rx is non-null.
 */
static ra8_err_t mock_bus_xfer8(void* ctx, uint8_t tx, uint8_t* rx)
{
  (void)ctx;
  (void)tx;
  s_xfer_calls = s_xfer_calls + 1U;
  if (rx != nullptr) {
    *rx = s_xfer_rx;
  }
  if ((s_xfer_fail_at != 0U) && (s_xfer_calls == s_xfer_fail_at)) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/* Rename the four exported symbols so the instrumented copy does not clash
 * with the production driver linked from ra8_core_hal. The wire itself needs
 * no preprocessor redirection any more: the driver reaches SPI only through
 * the injected seam, which the fixture cfg points at ::mock_bus_xfer8. */
ra8_err_t ra8_epaper_init_cov(const ra8_epaper_cfg_t* cfg);
ra8_err_t ra8_epaper_load_image_cov(const ra8_epaper_area_t*  area,
                                    const uint8_t*            buf,
                                    size_t                    buf_len,
                                    ra8_epaper_pixel_format_t pf,
                                    ra8_epaper_endian_t       endian);
ra8_err_t ra8_epaper_display_area_cov(const ra8_epaper_area_t* area,
                                      ra8_epaper_waveform_t    waveform);
ra8_err_t ra8_epaper_sleep_cov(void);
ra8_err_t ra8_epaper_dev_info_cov(ra8_epaper_dev_info_t* out_info);
/* The pure geometry / pixel-format / waveform-map helpers live in
 * ra8_epaper_geom.c, not in the white-box copy included below, so they are
 * NOT renamed: they hold no driver state, so the cov TU calls the same
 * production symbols the rest of the tree links. */
ra8_err_t ra8_epaper_get_vcom_cov(uint16_t* out_mv);
ra8_err_t ra8_epaper_set_vcom_cov(uint16_t mv);
bool      ra8_epaper_vcom_verified_cov(void);

/** @brief RA8 epaper init. */
#define ra8_epaper_init ra8_epaper_init_cov
/** @brief RA8 epaper load image. */
#define ra8_epaper_load_image ra8_epaper_load_image_cov
/** @brief RA8 epaper display area. */
#define ra8_epaper_display_area ra8_epaper_display_area_cov
/** @brief RA8 epaper sleep. */
#define ra8_epaper_sleep ra8_epaper_sleep_cov
/** @brief RA8 epaper dev info. */
#define ra8_epaper_dev_info ra8_epaper_dev_info_cov
/** @brief RA8 epaper VCOM read. */
#define ra8_epaper_get_vcom ra8_epaper_get_vcom_cov
/** @brief RA8 epaper VCOM write. */
#define ra8_epaper_set_vcom ra8_epaper_set_vcom_cov
/** @brief RA8 epaper INV-VCOM-1 permit query. */
#define ra8_epaper_vcom_verified ra8_epaper_vcom_verified_cov

#include "ra8_epaper.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/* =============================================================================
 * Fixture helpers -- all deterministic; no timers, no sleeps.
 * =============================================================================
 */

/**
 * @brief Arm the transfer counter to fail at @p n (0 => never fail).
 * @param[in] n 1-based ``bus.xfer8`` call index to fault; 0 disables.
 */
static void arm_fault(uint32_t n)
{
  s_xfer_calls   = 0U;
  s_xfer_fail_at = n;
}

/** @brief Build an otherwise-valid config for the cov init path. */
static ra8_epaper_cfg_t cov_cfg(void)
{
  const ra8_epaper_cfg_t cfg = {
    .bus          = {.xfer8 = mock_bus_xfer8, .ctx = (void*)&s_bus_probe},
    .waveform     = {.init = 0U, .du = 1U, .gc16 = 2U, .a2 = 4U},
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
  /* Keep the mock seam installed so direct helper calls stay legal. */
  s_panel.cfg   = cov_cfg();
  s_panel.state = k_ra8_epaper_state_uninit;
}

/** @brief Force the file-static panel to the ready state with a valid cfg. */
static void set_ready(void)
{
  s_panel.cfg = cov_cfg();
  /* Pre-grant the INV-VCOM-1 permit by writing the latch directly. Every
   * other test in this TU is about a different leg -- the DPY_AREA command
   * fault, the LUT poll, the argument block -- and would otherwise stop at
   * the VCOM refusal before reaching the leg it exists to cover. The gate
   * itself, and both outcomes of the readback compare that grants it, are
   * driven properly through the public API in ``test_vcom_permit_legs``. */
  s_panel.state         = k_ra8_epaper_state_ready;
  s_panel.vcom_verified = true;
  s_panel.vcom_mv       = (uint16_t)k_cov_word_nz;
}

/* =============================================================================
 * Direct helper tests -- each drives one helper's error legs on the mock.
 * =============================================================================
 */

/**
 * @test test_send16_recv16_legs
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- ``internal_ra8_epaper_send16``
 * and ``internal_ra8_epaper_recv16`` chain single-condition ``err != k_ra8_ok``
 * guards; no ``&&`` / ``||``.)
 */
static void test_send16_recv16_legs(void)
{
  TEST_BEGIN("send16/recv16: high-byte fault, low-byte fault, and clean word");
  set_uninit(); /* installs the mock seam into s_panel.cfg.bus */
  s_xfer_rx = (uint8_t)k_cov_rx_nz;

  /* send16: fail the high byte -> early return leg. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_send16((uint16_t)k_cov_reg_val));
  /* send16: fail the low byte -> tail-return propagates the error. */
  arm_fault((uint32_t)k_cov_fail_2);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_send16((uint16_t)k_cov_reg_val));
  /* send16: both bytes clock out -> ok. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_send16((uint16_t)k_cov_reg_val));

  /* recv16: fail the high byte, then the low byte, then a clean word. */
  uint16_t word = 0U;
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_recv16(&word));
  arm_fault((uint32_t)k_cov_fail_2);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_recv16(&word));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_recv16(&word));
  TEST_ASSERT_EQ(k_cov_word_nz, word);

  TEST_END("send16/recv16: high-byte fault, low-byte fault, and clean word");
}

/**
 * @test test_wait_ready_sim_leg
 *
 * @par MC/DC:
 * (no compound decisions -- the real HRDY poll loop runs and its loop-exit is
 * a single-condition seam consult; the unarmed ``ra8_sim_mmio`` seam models a
 * panel whose HRDY is already high, so the first poll succeeds. The timeout
 * leg is driven in test_ra8_epaper.c by arming the busy pin's PCNTR2.)
 */
static void test_wait_ready_sim_leg(void)
{
  TEST_BEGIN("wait_ready: unarmed seam reports ready on the first poll");
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_wait_ready());
  TEST_END("wait_ready: unarmed seam reports ready on the first poll");
}

/**
 * @test test_write_cmd_data_read_legs
 *
 * @par MC/DC:
 * (no compound decisions -- the command / data-write / data-read helpers are
 * straight-line sequences of single-condition ``err != k_ra8_ok`` guards.)
 */
static void test_write_cmd_data_read_legs(void)
{
  TEST_BEGIN("write_cmd/write_data16/read_data16: preamble and payload faults");
  s_xfer_rx = (uint8_t)k_cov_rx_nz;

  /* write_cmd: fault the preamble send16, then the command send16, then ok. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_write_cmd((uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_3);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_write_cmd((uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_write_cmd((uint16_t)k_cov_reg_val));

  /* write_data16: same shape -- preamble fault, payload fault, then ok. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_write_data16((uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_3);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_write_data16((uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_write_data16((uint16_t)k_cov_reg_val));

  /* read_data16: preamble fault, dummy-word fault, output-word fault, then ok. */
  uint16_t word = 0U;
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_read_data16(&word));
  arm_fault((uint32_t)k_cov_fail_3);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_read_data16(&word));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_read_data16(&word));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_read_data16(&word));
  TEST_ASSERT_EQ(k_cov_word_nz, word);

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
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_reg_write((uint16_t)k_cov_reg_addr, (uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_reg_write((uint16_t)k_cov_reg_addr, (uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_9);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_reg_write((uint16_t)k_cov_reg_addr, (uint16_t)k_cov_reg_val));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_ra8_epaper_reg_write((uint16_t)k_cov_reg_addr, (uint16_t)k_cov_reg_val));

  /* reg_read: fault write_cmd, then addr write_data16, then read_data16. */
  uint16_t val = 0U;
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_reg_read((uint16_t)k_cov_reg_addr, &val));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_reg_read((uint16_t)k_cov_reg_addr, &val));
  arm_fault((uint32_t)k_cov_fail_9);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_reg_read((uint16_t)k_cov_reg_addr, &val));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_reg_read((uint16_t)k_cov_reg_addr, &val));
  TEST_ASSERT_EQ(k_cov_word_nz, val);

  TEST_END("reg_write/reg_read: fault each of the three sub-transactions");
}

/**
 * @test test_read_dev_info_legs
 *
 * @par MC/DC:
 * (no compound decisions -- the GET_DEV_INFO read is a bounded loop of
 * single-condition read guards.)
 *
 * @details
 * The block is now decoded rather than discarded, so the clean leg also
 * pins the decode: every response byte is ::k_cov_rx_zero, which is
 * outside printable ASCII, so both version strings must come back empty
 * (the decoder drops non-printable bytes to NUL rather than letting a
 * garbled read reach a log line).
 */
static void test_read_dev_info_legs(void)
{
  TEST_BEGIN("read_dev_info: command fault, in-loop read fault, clean decode");
  s_xfer_rx                  = (uint8_t)k_cov_rx_zero;
  ra8_epaper_dev_info_t info = {};

  /* Fault the GET_DEV_INFO command. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_read_dev_info(&info));
  /* Fault the first read of the 20-word block (command spent 4 transfers). */
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, internal_ra8_epaper_read_dev_info(&info));
  /* Clean read + decode of all 20 words. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_read_dev_info(&info));
  TEST_ASSERT_EQ('\0', info.fw_version[0]);
  TEST_ASSERT_EQ('\0', info.lut_version[0]);

  /* A printable response decodes into the version strings, which is what
   * ra8_epaper_waveform_cfg_for_lut consumes. */
  s_xfer_rx = (uint8_t)'A';
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_read_dev_info(&info));
  TEST_ASSERT_EQ('A', info.lut_version[0]);
  TEST_ASSERT_EQ('\0', info.lut_version[k_ra8_epaper_ver_chars]);
  s_xfer_rx = (uint8_t)k_cov_rx_zero;

  TEST_END("read_dev_info: command fault, in-loop read fault, clean decode");
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
  const ra8_epaper_area_t area = {.x = 0U, .y = 0U, .width = 1U, .height = 1U};
  s_xfer_rx                    = (uint8_t)k_cov_rx_zero;

  /* send_load_args: fault arg0, x, y, width, height, then a clean block. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    internal_ra8_epaper_send_load_args(&area, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    internal_ra8_epaper_send_load_args(&area, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_9);
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    internal_ra8_epaper_send_load_args(&area, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_13);
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    internal_ra8_epaper_send_load_args(&area, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_17);
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    internal_ra8_epaper_send_load_args(&area, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    internal_ra8_epaper_send_load_args(&area, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));

  /* send_display_args: fault x, y, width, height, waveform, then a clean block. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_send_display_args(&area, k_cov_wf_gc16_mode));
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_send_display_args(&area, k_cov_wf_gc16_mode));
  arm_fault((uint32_t)k_cov_fail_9);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_send_display_args(&area, k_cov_wf_gc16_mode));
  arm_fault((uint32_t)k_cov_fail_13);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_send_display_args(&area, k_cov_wf_gc16_mode));
  arm_fault((uint32_t)k_cov_fail_17);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_send_display_args(&area, k_cov_wf_gc16_mode));
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_send_display_args(&area, k_cov_wf_gc16_mode));

  TEST_END("send_load_args/send_display_args: fault every argument word");
}

/**
 * @test test_stream_pixels_even_odd
 *
 * @par MC/DC:
 * Decision: ``if ((buf_len & 1U) != 0U)`` in ``internal_ra8_epaper_stream_pixels``
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
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_stream_pixels(buf, (size_t)k_cov_even_len));
  /* Even length, fault the first word write -> loop-body error leg. */
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 internal_ra8_epaper_stream_pixels(buf, (size_t)k_cov_even_len));
  /* Odd length emits the padded tail word -> tail predicate true, ok. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_stream_pixels(buf, (size_t)k_cov_odd_len));
  /* Odd length that also walks the loop once before the tail. */
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, internal_ra8_epaper_stream_pixels(buf, (size_t)k_cov_odd_len3));

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
 * (no compound decisions on the exercised legs -- ``ra8_epaper_init`` guards
 * SYS_RUN and drain with single-condition ``err`` checks; its only compound
 * decision, ``validate_cfg``, is covered by ::test_validate_and_size_mcdc.)
 */
static void test_init_ladder_legs(void)
{
  TEST_BEGIN("init: SYS_RUN fault, drain fault, clean bring-up");
  const ra8_epaper_cfg_t cfg = cov_cfg();
  s_xfer_rx                  = (uint8_t)k_cov_rx_zero;

  /* SYS_RUN command faults (its first transfer) -> propagated. */
  set_uninit();
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epaper_init_cov(&cfg));

  /* drain_dev_info faults on its command (transfer 5, after SYS_RUN's 4). */
  set_uninit();
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epaper_init_cov(&cfg));

  /* Clean bring-up -> ready. */
  set_uninit();
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init_cov(&cfg));
  TEST_ASSERT_EQ(k_ra8_epaper_state_ready, s_panel.state);

  TEST_END("init: SYS_RUN fault, drain fault, clean bring-up");
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
/**
 * @brief Run one 8 bpp ``load_image`` with the transfer counter armed.
 *
 * @details
 * Collapses the repeated set_ready / arm_fault / call triple so each leg
 * of ::test_load_image_api_legs is one readable line naming the transfer
 * index it faults.
 *
 * @param[in] fault 1-based ``bus.xfer8`` index to fail; 0 disables.
 * @param[in] area  Rectangle to load.
 * @param[in] buf   Source pixels, ::k_cov_buf_len bytes.
 * @return The driver's return code for that leg.
 * @pre  The fixture cfg is installed in the file-static panel.
 * @pre  ``buf`` holds ::k_cov_buf_len readable bytes.
 * @post The panel is left in the ready state.
 * @post ::s_xfer_calls reflects this call only.
 */
static ra8_err_t
cov_load_at_fault(uint32_t fault, const ra8_epaper_area_t* area, const uint8_t* buf)
{
  set_ready();
  arm_fault(fault);
  return ra8_epaper_load_image_cov(area,
                                   buf,
                                   (size_t)k_cov_buf_len,
                                   k_ra8_epaper_pf_8bpp,
                                   k_ra8_epaper_endian_little);
}

static void test_load_image_api_legs(void)
{
  TEST_BEGIN("load_image: fault LISAR lo/hi, cmd, args, and pixel stream");
  const uint8_t           buf[k_cov_buf_len] = {};
  const ra8_epaper_area_t area               = {.x      = 0U,
                                                .y      = 0U,
                                                .width  = (uint16_t)k_cov_panel_w,
                                                .height = (uint16_t)k_cov_panel_h};
  s_xfer_rx                                  = (uint8_t)k_cov_rx_zero;

  /* LISAR-low reg_write fault (transfer 1). */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, cov_load_at_fault((uint32_t)k_cov_fail_1, &area, buf));
  /* LISAR-high reg_write fault (transfer 13, after LISAR-low's 12). */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, cov_load_at_fault((uint32_t)k_cov_fail_13, &area, buf));
  /* LD_IMG_AREA command fault (transfer 25, after two reg writes = 24). */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, cov_load_at_fault((uint32_t)k_cov_fail_25, &area, buf));
  /* send_load_args fault (transfer 29, after cmd's 4 = 28). */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, cov_load_at_fault((uint32_t)k_cov_fail_29, &area, buf));
  /* stream_pixels fault (transfer 49, after args' 20 = 48). */
  TEST_ASSERT_EQ(k_ra8_err_hw_error, cov_load_at_fault((uint32_t)k_cov_fail_49, &area, buf));
  /* Clean load through LD_IMG_END. */
  TEST_ASSERT_EQ(k_ra8_ok, cov_load_at_fault((uint32_t)k_cov_fail_never, &area, buf));

  TEST_END("load_image: fault LISAR lo/hi, cmd, args, and pixel stream");
}

/**
 * @test test_vcom_permit_legs
 *
 * @details
 * INV-VCOM-1 end to end on the controllable mock, which is the only place
 * both outcomes of the readback compare can be driven: the mock stamps
 * every received byte with ::s_xfer_rx, so a 16-bit readback is always
 * ``(s_xfer_rx << 8) | s_xfer_rx`` and the test chooses whether the value
 * written matches it.
 *
 * The asymmetry being protected is the reason this test exists at all: a
 * blank screen costs one confusing boot, a panel biased at an unverified
 * VCOM is destroyed slowly and unrecoverably. So the refusal path is
 * asserted first, and the permit is proven to be revoked -- not merely
 * absent -- after a failed write and after a sleep.
 *
 * @par MC/DC:
 * Decision A: `if (readback != mv)` in ``ra8_epaper_set_vcom``
 * (1 condition -- branch coverage, 2 vectors both necessary):
 * - Vector 1: readback == mv -> false (permit granted, display permitted)
 * - Vector 2: readback != mv -> true  (refused, permit stays revoked)
 * Decision B: `if (!s_panel.vcom_verified)` in ``ra8_epaper_display_area``
 * (1 condition -- branch coverage, 2 vectors both necessary):
 * - Vector 1: permit absent -> true  (refresh refused)
 * - Vector 2: permit held   -> false (refresh proceeds)
 */
static void test_vcom_permit_legs(void)
{
  TEST_BEGIN("vcom: readback grants or revokes the display permit");
  const ra8_epaper_area_t area = {.x      = 0U,
                                  .y      = 0U,
                                  .width  = (uint16_t)k_cov_panel_w,
                                  .height = (uint16_t)k_cov_panel_h};

  /* Start ready but explicitly WITHOUT the permit: this is the state a
   * freshly initialised driver is really in. */
  set_ready();
  s_panel.vcom_verified = false;
  s_xfer_rx             = (uint8_t)k_cov_rx_nz;
  arm_fault((uint32_t)k_cov_fail_never);

  /* Decision B vector 1 -- no permit, so no refresh, whatever else is fine. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epaper_display_area_cov(&area, k_ra8_epaper_wf_gc16));

  /* Decision A vector 2 -- the controller echoes k_cov_word_nz, so asking
   * for anything else is a mismatch. The write is refused and, critically,
   * the panel is still not drivable. */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epaper_set_vcom_cov((uint16_t)k_cov_reg_val));
  TEST_ASSERT_EQ(false, ra8_epaper_vcom_verified_cov());
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epaper_display_area_cov(&area, k_ra8_epaper_wf_gc16));

  /* Decision A vector 1 -- ask for exactly what the mock echoes back. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_set_vcom_cov((uint16_t)k_cov_word_nz));
  TEST_ASSERT_EQ(true, ra8_epaper_vcom_verified_cov());

  /* Decision B vector 2 -- with the permit held the refresh runs for real.
   * LUT reads back zero on the first poll, so this is the idle-success exit. */
  s_xfer_rx = (uint8_t)k_cov_rx_zero;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_display_area_cov(&area, k_ra8_epaper_wf_gc16));

  /* A failed write after a good one revokes rather than leaving the earlier
   * permit standing -- otherwise a panel could be refreshed on the strength
   * of a calibration that has since been contradicted. */
  s_xfer_rx = (uint8_t)k_cov_rx_nz;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_set_vcom_cov((uint16_t)k_cov_word_nz));
  TEST_ASSERT_EQ(true, ra8_epaper_vcom_verified_cov());
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_epaper_set_vcom_cov((uint16_t)k_cov_reg_val));
  TEST_ASSERT_EQ(false, ra8_epaper_vcom_verified_cov());

  /* Sleep leaves the controller in an undefined state, so the permit must
   * not survive it into the next init. */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_nz;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_set_vcom_cov((uint16_t)k_cov_word_nz));
  TEST_ASSERT_EQ(true, ra8_epaper_vcom_verified_cov());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep_cov());
  TEST_ASSERT_EQ(false, ra8_epaper_vcom_verified_cov());

  TEST_END("vcom: readback grants or revokes the display permit");
}

/**
 * @test test_display_area_api_legs
 *
 * @par MC/DC:
 * (no compound decisions on the exercised legs -- the DPY_AREA command, the
 * argument block and the in-loop LUT read are single-condition guards; the
 * ``status == 0`` idle-success early-exit and the loop-budget-exhaustion
 * timeout are both single-condition too.)
 */
static void test_display_area_api_legs(void)
{
  TEST_BEGIN("display_area: cmd fault, args fault, LUT read fault, both exits");
  const ra8_epaper_area_t area = {.x      = 0U,
                                  .y      = 0U,
                                  .width  = (uint16_t)k_cov_panel_w,
                                  .height = (uint16_t)k_cov_panel_h};

  /* DPY_AREA command fault (transfer 1). */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_zero;
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epaper_display_area_cov(&area, k_ra8_epaper_wf_gc16));
  /* send_display_args fault (transfer 5, after cmd's 4). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_5);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epaper_display_area_cov(&area, k_ra8_epaper_wf_gc16));
  /* LUT reg_read fault (transfer 25, after cmd 4 + args 20 = 24). */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_25);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epaper_display_area_cov(&area, k_ra8_epaper_wf_gc16));

  /* Clean refresh, LUT reads back zero -> status==0 satisfies the real poll on
   * its first iteration (the ra8_sim_mmio seam is unarmed here, so it honours the
   * driver's real status comparison) -> k_ra8_ok early exit. */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_zero;
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_display_area_cov(&area, k_ra8_epaper_wf_gc16));
  /* Clean refresh, LUT never reports idle -> the real bounded poll exhausts its
   * budget and returns k_ra8_err_hw_timeout. The LUT-idle poll consults the
   * ra8_sim_mmio seam keyed on the injected seam's ctx cookie (ra8_epaper.c
   * lut_probe == cfg.bus.ctx == &s_bus_probe here); with the unarmed=succeed
   * contract we arm fail_wait on that key so the loop runs to exhaustion
   * deterministically. Covers the loop-exhaustion leg. */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_busy;
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((volatile const void*)&s_bus_probe));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_epaper_display_area_cov(&area, k_ra8_epaper_wf_gc16));
  ra8_sim_mmio_reset();

  TEST_END("display_area: cmd fault, args fault, LUT read fault, both exits");
}

/**
 * @test test_sleep_legs
 *
 * @par MC/DC:
 * (no compound decisions -- ``ra8_epaper_sleep`` is a single-condition state
 * guard followed by a straight-line SLEEP command whose result is returned
 * verbatim after the state is reset.)
 */
static void test_sleep_legs(void)
{
  TEST_BEGIN("sleep: rejected pre-ready, clean sleep, and SLEEP-command fault");

  /* Not ready -> invalid_state. */
  set_uninit();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_sleep_cov());

  /* Ready + clean SLEEP command -> ok, state falls back to uninit. */
  set_ready();
  s_xfer_rx = (uint8_t)k_cov_rx_zero;
  arm_fault((uint32_t)k_cov_fail_never);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_sleep_cov());
  TEST_ASSERT_EQ(k_ra8_epaper_state_uninit, s_panel.state);

  /* Ready + faulted SLEEP command -> error surfaced, state still reset. */
  set_ready();
  arm_fault((uint32_t)k_cov_fail_1);
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_epaper_sleep_cov());
  TEST_ASSERT_EQ(k_ra8_epaper_state_uninit, s_panel.state);

  TEST_END("sleep: rejected pre-ready, clean sleep, and SLEEP-command fault");
}

/**
 * @brief Decision A: validate_cfg 5-condition MC/DC (baseline + lone-true flips).
 * @return None.
 * @pre The transfer/fault stubs are armed for success.
 * @post The baseline accepted and each single bad field was rejected.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void epaper_check_validate_cfg(void)
{
  /* V0: baseline accept. */
  set_uninit();
  ra8_epaper_cfg_t cfg = cov_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epaper_init_cov(&cfg));

  /* V1..V5: flip exactly one condition true, expect rejection. */
  set_uninit();
  cfg           = cov_cfg();
  cfg.bus.xfer8 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init_cov(&cfg));
  set_uninit();
  cfg             = cov_cfg();
  cfg.panel_width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init_cov(&cfg));
  set_uninit();
  cfg              = cov_cfg();
  cfg.panel_height = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init_cov(&cfg));
  set_uninit();
  cfg             = cov_cfg();
  cfg.panel_width = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init_cov(&cfg));
  set_uninit();
  cfg              = cov_cfg();
  cfg.panel_height = (uint16_t)0xFFFFU;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_epaper_init_cov(&cfg));
}

/**
 * @brief Decision B: load_image size MC/DC, null-argument guards and double-init.
 * @return None.
 * @pre validate_cfg vectors already ran.
 * @post Each size vector, null guard and the double-init rejection held.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void epaper_check_size_and_guards(void)
{
  /* Decision B: size check on a ready panel. */
  set_ready();
  const uint8_t           buf[k_cov_buf_len] = {};
  const ra8_epaper_area_t area               = {.x      = 0U,
                                                .y      = 0U,
                                                .width  = (uint16_t)k_cov_panel_w,
                                                .height = (uint16_t)k_cov_panel_h};
  /* V1: exact size accepts. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epaper_load_image_cov(&area,
                                           buf,
                                           (size_t)k_cov_buf_len,
                                           k_ra8_epaper_pf_8bpp,
                                           k_ra8_epaper_endian_little));
  /* V2: wrong size rejects. */
  set_ready();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_epaper_load_image_cov(&area,
                                           buf,
                                           (size_t)k_cov_even_len,
                                           k_ra8_epaper_pf_8bpp,
                                           k_ra8_epaper_endian_little));
  /* V3: zero-area rejects. */
  set_ready();
  const ra8_epaper_area_t area0 = {.x = 0U, .y = 0U, .width = 0U, .height = 0U};
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_epaper_load_image_cov(&area0, buf, 0U, k_ra8_epaper_pf_8bpp, k_ra8_epaper_endian_little));

  /* Null-argument guards on both entry points. */
  set_ready();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epaper_load_image_cov(nullptr,
                                           buf,
                                           (size_t)k_cov_buf_len,
                                           k_ra8_epaper_pf_8bpp,
                                           k_ra8_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_epaper_load_image_cov(&area,
                                           nullptr,
                                           (size_t)k_cov_buf_len,
                                           k_ra8_epaper_pf_8bpp,
                                           k_ra8_epaper_endian_little));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_display_area_cov(nullptr, k_ra8_epaper_wf_gc16));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_epaper_init_cov(nullptr));

  /* Double-init rejection: a second init while ready. */
  set_ready();
  ra8_epaper_cfg_t cfg = cov_cfg();
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_epaper_init_cov(&cfg));
}

/**
 * @test test_validate_and_size_mcdc
 *
 * @par MC/DC:
 * Decision A libs/ra8_hal/src/ra8_epaper.c@internal_ra8_epaper_validate_cfg:
 * ``if ((bus.xfer8 == NULL) || (panel_w == 0) || (panel_h == 0) ||
 *      (panel_w > MAX) || (panel_h > MAX))``
 * (5 conditions, ``||`` short-circuit chain). N+1 = 6 vectors: an all-false
 * baseline that accepts, plus one vector per condition that flips exactly
 * that Ck true while every other Ci stays false, proving independent
 * influence pair-wise against the baseline.
 * - V0: all Ci=F                 -> accept (init reaches ready)
 * - V1: bus.xfer8=NULL           -> reject
 * - V2: panel_width=0            -> reject
 * - V3: panel_height=0           -> reject
 * - V4: panel_width=0xFFFF>MAX   -> reject
 * - V5: panel_height=0xFFFF>MAX  -> reject
 *
 * Decision B: ``ra8_epaper_load_image`` ``if ((buf_len != expect) ||
 * (expect == 0U))`` (2 conditions, ``||``). N+1 = 3 vectors:
 * - V1: buf_len==expect, expect>0 -> accept
 * - V2: buf_len!=expect           -> reject (varies buf_len only)
 * - V3: expect==0 (0x0 area)      -> reject (varies expect only)
 */
static void test_validate_and_size_mcdc(void)
{
  TEST_BEGIN("validate_cfg 5-cond + load_image size 2-cond MC/DC");
  s_xfer_rx = (uint8_t)k_cov_rx_zero;
  arm_fault((uint32_t)k_cov_fail_never);

  epaper_check_validate_cfg();
  epaper_check_size_and_guards();

  set_uninit();
  TEST_END("validate_cfg 5-cond + load_image size 2-cond MC/DC");
}

int32_t main(void)
{
  test_send16_recv16_legs();
  test_wait_ready_sim_leg();
  test_write_cmd_data_read_legs();
  test_reg_write_read_legs();
  test_read_dev_info_legs();
  test_load_display_args_legs();
  test_stream_pixels_even_odd();
  test_init_ladder_legs();
  test_load_image_api_legs();
  test_vcom_permit_legs();
  test_display_area_api_legs();
  test_sleep_legs();
  test_validate_and_size_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra8_epaper_cov.c\n");
  return 0;
}
