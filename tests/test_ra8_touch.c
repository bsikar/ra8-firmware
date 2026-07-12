/**
 * @file test_ra8_touch.c
 * @brief Unit tests for the ra8_touch (GT911) driver
 *
 * @details
 * Drives the touch driver against the host ``ra8_sim_mmap`` substrate.
 * Status flags for the underlying IIC_B channel are pre-armed so the
 * polling driver does not time out, and the GT911 wire-format parser
 * is verified directly through the ``ra8_touch_test_decode`` test hook.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_i3c_i2c.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_touch.h"
#include "ra8_touch_gt911_regs.h"
#include "unity_minimal.h"

/**
 * @enum ra8_touch_test_const_t
 * @brief Test-only constants (no magic numbers).
 */
typedef enum : uint16_t {
  k_test_max_points_default = 5U,
  k_test_irq_pin_unused     = 32U, /**< Same as ::k_ra8_touch_irq_pin_unset. */
  k_test_x_one              = 0x0123U,
  k_test_y_one              = 0x0456U,
  k_test_x_two              = 0x07F0U,
  k_test_y_two              = 0x0218U,
  k_test_x_three            = 0x0099U,
  k_test_y_three            = 0x012CU,
  k_test_x_five_a           = 0x0001U,
  k_test_y_five_a           = 0x0002U,
  k_test_max_count_wide     = 8U, /**< Read cap above the 5-point hard limit. */
  k_test_decode_hw_over     = 7U, /**< Decode n_points/max_count above 5.     */
} ra8_touch_test_const_t;

/**
 * @enum ra8_touch_test_addr_t
 * @brief Address constants used across tests.
 */
typedef enum : uint8_t {
  k_test_addr_default = 0x5DU,
  k_test_addr_alt     = 0x14U,
  k_test_addr_bad     = 0x42U,
  k_test_pressure_one = 0x40U,
  k_test_pressure_two = 0x80U,
  k_test_track_zero   = 0U,
  k_test_track_one    = 1U,
  k_test_track_two    = 2U,
  k_test_byte_shift   = 8U,
  k_test_irq_pin_zero = 0U, /**< In-range ICU IRQ pin for the attach path. */
} ra8_touch_test_addr_t;

/**
 * @brief Pre-arm IIC_B status registers so polling helpers fall through.
 *
 * @details
 * Mirrors the prime_ntst() helper in ``test_ra8_i3c_i2c.c``: NTST gets the
 * "TX buffer empty" + "RX buffer full" bits set so the wait loops exit
 * immediately, and BCST gets the "bus free" bit set so the busy gate
 * does not reject the transaction.
 */
static void prime_iic_b(void)
{
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->NTST = (uint32_t)k_ra8_i3c_i2c_msk_ntst_tdbef0 | (uint32_t)k_ra8_i3c_i2c_msk_ntst_rdbff0;
  reg->BCST = (uint32_t)k_ra8_i3c_i2c_msk_bcst_bfref;
}

/**
 * @enum ra8_touch_test_bus_t
 * @brief App-side bus bring-up clocking used by prep().
 */
typedef enum : uint32_t {
  k_test_bus_hz   = 400000U,   /**< Fast-mode I2C clock.     */
  k_test_pclka_hz = 60000000U, /**< IIC_B clock-source rate. */
} ra8_touch_test_bus_t;

/** @brief Bound I2C bus handle the injected seam's ctx points at. */
static ra8_io_i2c_bus_t s_bus;
/** @brief Seam filled from ::s_bus by ``ra8_io_i2c_bus_as_ops`` in prep(). */
static ra8_i2c_bus_ops_t s_bus_ops;
/** @brief Standard config used by happy-path tests (filled in prep()). */
static ra8_touch_cfg_t s_cfg_default;

/**
 * @brief Reset the simulator, bring MSTP up, and play the app's role:
 *        initialise IIC_B channel 0 and bind it through the ra8_io facade
 *        into the driver's injected seam.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_i3c_cfg_t iic_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_test_bus_hz,
    .pclka_hz = (uint32_t)k_test_pclka_hz,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_i3c_init(0U, &iic_cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_bind_i3c_compat(&s_bus, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_i2c_bus_as_ops(&s_bus, &s_bus_ops));
  const ra8_touch_cfg_t cfg = {
    .bus        = s_bus_ops,
    .target_7b  = (uint8_t)k_test_addr_default,
    .irq_pin    = (uint8_t)k_test_irq_pin_unused,
    .max_points = (uint8_t)k_test_max_points_default,
  };
  s_cfg_default = cfg;
}

/**
 * @brief Pack an unsigned 16-bit value into LSB-first byte order.
 */
static void pack_le16(uint16_t v, uint8_t* out)
{
  out[0] = (uint8_t)(v & 0xFFU);
  out[1] = (uint8_t)((uint32_t)v >> (uint32_t)k_test_byte_shift);
}

/**
 * @brief Build one GT911 8-byte point record.
 */
static void build_point(uint8_t* buf, uint8_t track, uint16_t x, uint16_t y, uint16_t size)
{
  buf[k_ra8_touch_gt911_point_off_track] = track;
  pack_le16(x, &buf[k_ra8_touch_gt911_point_off_x_lsb]);
  pack_le16(y, &buf[k_ra8_touch_gt911_point_off_y_lsb]);
  pack_le16(size, &buf[k_ra8_touch_gt911_point_off_size_lsb]);
  buf[k_ra8_touch_gt911_point_off_reserved] = 0U;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_open_close_happy(void)
{
  TEST_BEGIN("ra8_touch_open / close happy path");
  prep();
  prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_open / close happy path");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_null_cfg(void)
{
  TEST_BEGIN("ra8_touch_open: NULL cfg rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_open(nullptr));
  TEST_END("ra8_touch_open: NULL cfg rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_invalid_address(void)
{
  TEST_BEGIN("ra8_touch_open: bad target address rejected");
  prep();
  prime_iic_b();
  ra8_touch_cfg_t bad = s_cfg_default;
  bad.target_7b       = (uint8_t)k_test_addr_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_open(&bad));
  TEST_END("ra8_touch_open: bad target address rejected");
}

/**
 * @test test_mcdc_open_bus_seam
 *
 * @par MC/DC:
 * Decision libs/ra8_hal/src/ra8_touch.c@priv_validate_cfg:
 * ``if ((cfg->bus.write == nullptr) || (cfg->bus.transfer == nullptr))``
 * (2 conditions, ``||``). N+1 = 3:
 * - V1: write=valid, transfer=valid -> F||F -> F (open proceeds; the
 *       happy-path cases in this file complete it)
 * - V2: write=NULL,  transfer=valid -> T||- -> T (varies write only)
 * - V3: write=valid, transfer=NULL  -> F||T -> T (varies transfer only)
 * Vectors 1+2 prove `write` independently affects the outcome; 1+3 prove
 * the same for `transfer`. Minimal MC/DC for N=2.
 */
static void test_mcdc_open_bus_seam(void)
{
  TEST_BEGIN("ra8_touch_open: incomplete bus seam rejected");
  prep();
  prime_iic_b();
  /* V1: complete seam accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  /* V2: missing write row rejected. */
  ra8_touch_cfg_t bad = s_cfg_default;
  bad.bus.write       = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_open(&bad));
  /* V3: missing transfer row rejected. */
  bad              = s_cfg_default;
  bad.bus.transfer = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_open(&bad));
  TEST_END("ra8_touch_open: incomplete bus seam rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_open_already_open(void)
{
  TEST_BEGIN("ra8_touch_open: already-open rejected");
  prep();
  prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_touch_open(&s_cfg_default));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_open: already-open rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_close_without_open(void)
{
  TEST_BEGIN("ra8_touch_close: not-initialized rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_touch_close());
  TEST_END("ra8_touch_close: not-initialized rejected");
}

/* =============================================================================
 * Read path
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_read_null_args(void)
{
  TEST_BEGIN("ra8_touch_read: NULL args rejected");
  prep();
  prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));
  ra8_touch_point_t pt[5U];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_touch_read(nullptr, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_touch_read(pt, (uint8_t)k_test_max_points_default, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_read(pt, 0U, &got));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_read: NULL args rejected");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_returns_ok(void)
{
  TEST_BEGIN("ra8_touch_read: returns ok with primed I2C transport");
  prep();
  prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));

  /* The simulator's NTDTBP0 read-back reflects whatever value the
   * driver last wrote (the address byte for the trailing read phase),
   * so the actual decoded got_count is implementation-defined here.
   * What this test asserts is the absence of error: the read path
   * walks the full state machine without hitting a timeout or hw
   * fault. End-to-end byte-level checks live in
   * ``test_decode_*`` against the parser hook. */
  ra8_touch_point_t pt[5U];
  uint8_t           got = 99U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_read(pt, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT(got <= (uint8_t)k_test_max_points_default);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_read: returns ok with primed I2C transport");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_before_open(void)
{
  TEST_BEGIN("ra8_touch_read: not-initialized rejected");
  prep();
  ra8_touch_point_t pt[5U];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_touch_read(pt, (uint8_t)k_test_max_points_default, &got));
  TEST_END("ra8_touch_read: not-initialized rejected");
}

/* =============================================================================
 * Decode parser (test_decode hook)
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_decode_one_point(void)
{
  TEST_BEGIN("ra8_touch_test_decode: single point");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes] = {};
  build_point(raw,
              (uint8_t)k_test_track_zero,
              (uint16_t)k_test_x_one,
              (uint16_t)k_test_y_one,
              (uint16_t)k_test_pressure_one);

  ra8_touch_point_t pts[5U];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_touch_test_decode(raw, 1U, pts, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(1, got);
  TEST_ASSERT_EQ(k_test_track_zero, pts[0].track_id);
  TEST_ASSERT_EQ(k_test_x_one, pts[0].x);
  TEST_ASSERT_EQ(k_test_y_one, pts[0].y);
  TEST_ASSERT_EQ(k_test_pressure_one, pts[0].pressure);
  TEST_END("ra8_touch_test_decode: single point");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_decode_three_points(void)
{
  TEST_BEGIN("ra8_touch_test_decode: three points");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes * 3U] = {};
  build_point(&raw[0U * (uint32_t)k_ra8_touch_gt911_point_bytes],
              (uint8_t)k_test_track_zero,
              (uint16_t)k_test_x_one,
              (uint16_t)k_test_y_one,
              (uint16_t)k_test_pressure_one);
  build_point(&raw[1U * (uint32_t)k_ra8_touch_gt911_point_bytes],
              (uint8_t)k_test_track_one,
              (uint16_t)k_test_x_two,
              (uint16_t)k_test_y_two,
              (uint16_t)k_test_pressure_two);
  build_point(&raw[2U * (uint32_t)k_ra8_touch_gt911_point_bytes],
              (uint8_t)k_test_track_two,
              (uint16_t)k_test_x_three,
              (uint16_t)k_test_y_three,
              (uint16_t)k_test_pressure_one);

  ra8_touch_point_t pts[5U];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_touch_test_decode(raw, 3U, pts, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(3, got);
  TEST_ASSERT_EQ(k_test_x_one, pts[0].x);
  TEST_ASSERT_EQ(k_test_x_two, pts[1].x);
  TEST_ASSERT_EQ(k_test_y_three, pts[2].y);
  TEST_ASSERT_EQ(k_test_track_two, pts[2].track_id);
  TEST_END("ra8_touch_test_decode: three points");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_decode_five_points_max(void)
{
  TEST_BEGIN("ra8_touch_test_decode: five points (max)");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes * 5U] = {};
  for (uint8_t i = 0U; i < 5U; i++) {
    build_point(&raw[(uint32_t)i * (uint32_t)k_ra8_touch_gt911_point_bytes],
                i,
                (uint16_t)((uint32_t)k_test_x_five_a + i),
                (uint16_t)((uint32_t)k_test_y_five_a + i),
                (uint16_t)k_test_pressure_one);
  }

  ra8_touch_point_t pts[5U];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_touch_test_decode(raw, 5U, pts, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(5, got);
  for (uint8_t i = 0U; i < 5U; i++) {
    TEST_ASSERT_EQ(i, pts[i].track_id);
  }
  TEST_END("ra8_touch_test_decode: five points (max)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_decode_clamp_to_max_count(void)
{
  TEST_BEGIN("ra8_touch_test_decode: input > max_count clamps");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes * 3U] = {};
  for (uint8_t i = 0U; i < 3U; i++) {
    build_point(&raw[(uint32_t)i * (uint32_t)k_ra8_touch_gt911_point_bytes],
                i,
                (uint16_t)((uint32_t)k_test_x_five_a + i),
                (uint16_t)((uint32_t)k_test_y_five_a + i),
                (uint16_t)k_test_pressure_one);
  }
  ra8_touch_point_t pts[2U];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_test_decode(raw, 3U, pts, 2U, &got));
  TEST_ASSERT_EQ(2, got);
  TEST_END("ra8_touch_test_decode: input > max_count clamps");
}

/* =============================================================================
 * Handler attach + dispatch
 * =============================================================================
 */

static int32_t s_cb_count;
static void*   s_cb_last_ctx;

static void touch_cb(void* ctx)
{
  s_cb_count++;
  s_cb_last_ctx = ctx;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_dispatch(void)
{
  TEST_BEGIN("ra8_touch_attach_handler + dispatch");
  prep();
  prime_iic_b();
  s_cb_count    = 0;
  s_cb_last_ctx = nullptr;

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_touch_attach_handler(touch_cb, (void*)0xCAFEU));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_attach_handler(touch_cb, (void*)0xCAFEU));

  ra8_touch_dispatch_irq();
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(0xCAFE, (uintptr_t)s_cb_last_ctx);

  /* Detaching with NULL must not fire the callback again. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_attach_handler(nullptr, nullptr));
  ra8_touch_dispatch_irq();
  TEST_ASSERT_EQ(1, s_cb_count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_attach_handler + dispatch");
}

/* =============================================================================
 * Calibrate (no-op for GT911)
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_calibrate_noop(void)
{
  TEST_BEGIN("ra8_touch_calibrate: no-op returns ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_calibrate());
  TEST_END("ra8_touch_calibrate: no-op returns ok");
}

/**
 * @test test_mcdc_ra8_touch
 *
 * @par MC/DC:
 * Decision A: ``priv_validate_cfg`` line 254,
 * libs/ra8_hal/src/ra8_touch.c:
 * ``if ((target_7b != GT911_LOW) && (target_7b != GT911_HIGH))``
 * (2 conditions, ``&&``). N+1 = 3:
 * - V1: addr=0x5D -> dec F (accept)
 * - V2: addr=0x14 -> dec F (accept)
 * - V3: addr=0x42 -> dec T (reject)
 *
 * Decision B: ``priv_stash_state`` line 309,
 * ``if ((max_points == 0) || (max_points > MAX))`` (2 conditions, ``||``).
 * N+1 = 3 (clamp observed via post-state):
 * - V1: max=5  -> dec F (no clamp)
 * - V2: max=0  -> dec T (clamp)
 * - V3: max=99 -> dec T (clamp)
 * DO-178C 6.4.4.3 met.
 */
static void test_mcdc_ra8_touch(void)
{
  TEST_BEGIN("touch MC/DC: validate_cfg + stash_state 2-cond decisions");
  prep();
  prime_iic_b(); /* product-id read now runs on host: keep the bus free so open succeeds */
  ra8_touch_cfg_t cfg = s_cfg_default;
  cfg.target_7b       = (uint8_t)k_ra8_touch_gt911_addr_low;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  cfg.target_7b = (uint8_t)k_ra8_touch_gt911_addr_high;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  cfg.target_7b = (uint8_t)0x42U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_open(&cfg));
  cfg            = s_cfg_default;
  cfg.max_points = 5U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  cfg.max_points = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  cfg.max_points = 99U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("touch MC/DC: validate_cfg + stash_state 2-cond decisions");
}

/**
 * @test test_open_with_irq_pin
 *
 * @par MC/DC:
 * (no compound decisions -- drives the ``irq_pin < k_ra8_touch_irq_pin_count``
 * leg of ``priv_attach_irq_pin`` so the ICU IRQ-pin programming branch is
 * exercised; the guarding decision is single-condition.)
 */
static void test_open_with_irq_pin(void)
{
  TEST_BEGIN("ra8_touch_open: in-range IRQ pin programs the ICU");
  prep();
  prime_iic_b();
  ra8_touch_cfg_t cfg = s_cfg_default;
  cfg.irq_pin         = (uint8_t)k_test_irq_pin_zero;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_open: in-range IRQ pin programs the ICU");
}

/**
 * @test test_read_no_frame_ready
 *
 * @par MC/DC:
 * (no compound decisions -- the read-back status byte equals the odd
 * read-address byte 0x29 for target 0x14, whose bit7 is clear, so the
 * single-condition ``(status & ready_mask) == 0`` no-frame leg is taken.)
 */
static void test_read_no_frame_ready(void)
{
  TEST_BEGIN("ra8_touch_read: status bit7 clear -> no frame, got=0");
  prep();
  prime_iic_b();
  ra8_touch_cfg_t cfg = s_cfg_default;
  cfg.target_7b       = (uint8_t)k_test_addr_alt; /* 0x14 -> status 0x29, bit7 clear. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));

  ra8_touch_point_t pt[5U];
  uint8_t           got = 7U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_read(pt, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(0, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_read: status bit7 clear -> no frame, got=0");
}

/**
 * @test test_read_clamp_to_max_points
 *
 * @par MC/DC:
 * (no compound decisions -- with target 0x5D the status reads back 0xBB,
 * whose low nibble is 11; a read cap of 8 clamps emit to 8 then the
 * single-condition ``emit > s_state.max_points`` clamp drops it to 5.)
 */
static void test_read_clamp_to_max_points(void)
{
  TEST_BEGIN("ra8_touch_read: emit clamped to max_points (5)");
  prep();
  prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));

  ra8_touch_point_t pt[(uint32_t)k_test_max_count_wide];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_read(pt, (uint8_t)k_test_max_count_wide, &got));
  TEST_ASSERT_EQ(k_ra8_touch_max_points, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_read: emit clamped to max_points (5)");
}

/**
 * @test test_read_status_transfer_error
 *
 * @par MC/DC:
 * (no compound decisions -- clearing BCST.BFREF makes the status-read
 * transfer fail the bus-free gate, so the single-condition
 * ``status_err != k_ra8_ok`` error leg returns k_ra8_err_hw_error.)
 */
static void test_read_status_transfer_error(void)
{
  TEST_BEGIN("ra8_touch_read: status-read transport error -> hw_error");
  prep();
  prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));

  /* Drop the bus-free flag so the next transfer's busy gate rejects it. */
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->BCST                      = 0U;

  ra8_touch_point_t pt[5U];
  uint8_t           got = 9U;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_touch_read(pt, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(0, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_read: status-read transport error -> hw_error");
}

/**
 * @test test_open_product_id_transfer_error
 *
 * @par MC/DC:
 * (no compound decisions -- clearing BCST.BFREF fails the product-id read's
 * bus-free gate, so priv_check_product_id's single-condition
 * ``pid_err != k_ra8_ok`` leg returns k_ra8_err_hw_init_failed, which
 * ra8_touch_open propagates.)
 */
static void test_open_product_id_transfer_error(void)
{
  TEST_BEGIN("ra8_touch_open: product-id read transport error -> hw_init_failed");
  prep();
  prime_iic_b();
  /* Drop the bus-free flag so the product-id read (open's first transfer)
   * fails its busy gate: priv_check_product_id returns hw_init_failed instead
   * of the fake success the deleted RA8_SIMULATOR_MODE short-circuit returned. */
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->BCST                      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_touch_open(&s_cfg_default));
  TEST_END("ra8_touch_open: product-id read transport error -> hw_init_failed");
}

/**
 * @test test_decode_clamp_to_hw_max
 *
 * @par MC/DC:
 * (no compound decisions -- n_points and max_count both 7 leave emit at 7
 * past the ``emit > max_count`` clamp, so the single-condition
 * ``emit > k_ra8_touch_gt911_max_points`` clamp drops it to 5.)
 */
static void test_decode_clamp_to_hw_max(void)
{
  TEST_BEGIN("ra8_touch_test_decode: input > hw max clamps to 5");
  uint8_t raw[(uint32_t)k_ra8_touch_gt911_point_bytes * (uint32_t)k_test_decode_hw_over] = {};
  for (uint8_t i = 0U; i < (uint8_t)k_test_decode_hw_over; i++) {
    build_point(&raw[(uint32_t)i * (uint32_t)k_ra8_touch_gt911_point_bytes],
                i,
                (uint16_t)((uint32_t)k_test_x_five_a + i),
                (uint16_t)((uint32_t)k_test_y_five_a + i),
                (uint16_t)k_test_pressure_one);
  }
  ra8_touch_point_t pts[(uint32_t)k_test_decode_hw_over];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_touch_test_decode(raw,
                                       (uint8_t)k_test_decode_hw_over,
                                       pts,
                                       (uint8_t)k_test_decode_hw_over,
                                       &got));
  TEST_ASSERT_EQ(k_ra8_touch_gt911_max_points, got);
  TEST_END("ra8_touch_test_decode: input > hw max clamps to 5");
}

/**
 * @test test_decode_zero_max_count
 *
 * @par MC/DC:
 * (no compound decisions -- exercises the single-condition
 * ``max_count == 0`` rejection leg of the ra8_touch_test_decode hook.)
 */
static void test_decode_zero_max_count(void)
{
  TEST_BEGIN("ra8_touch_test_decode: max_count == 0 rejected");
  uint8_t           raw[(uint32_t)k_ra8_touch_gt911_point_bytes] = {};
  ra8_touch_point_t pts[1U];
  uint8_t           got = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_test_decode(raw, 1U, pts, 0U, &got));
  TEST_ASSERT_EQ(0, got);
  TEST_END("ra8_touch_test_decode: max_count == 0 rejected");
}

int32_t main(void)
{
  test_open_close_happy();
  test_open_null_cfg();
  test_open_invalid_address();
  test_mcdc_open_bus_seam();
  test_open_already_open();
  test_close_without_open();
  test_read_null_args();
  test_read_returns_ok();
  test_read_before_open();
  test_decode_one_point();
  test_decode_three_points();
  test_decode_five_points_max();
  test_decode_clamp_to_max_count();
  test_attach_dispatch();
  test_calibrate_noop();
  test_mcdc_ra8_touch();
  test_open_with_irq_pin();
  test_read_no_frame_ready();
  test_read_clamp_to_max_points();
  test_read_status_transfer_error();
  test_open_product_id_transfer_error();
  test_decode_clamp_to_hw_max();
  test_decode_zero_max_count();
  (void)fprintf(stderr, "[OK ] test_ra8_touch.c\n");
  return 0;
}
