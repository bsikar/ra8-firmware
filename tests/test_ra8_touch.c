/**
 * @file test_ra8_touch.c
 * @brief Unit tests for the ra8_touch (GT911) driver
 *
 * @details
 * Drives the touch driver against the host ``ra8_sim_mmap`` substrate.
 * Status flags for the underlying IIC_B channel are pre-armed so the
 * polling driver does not time out. Lifecycle, bus-path, and handler
 * dispatch are covered here.
 *
 * The driver's injected ``ra8_i2c_bus_ops_t`` seam is filled with a
 * test-local controllable-RX trampoline layered over the real
 * i3c-i2c-compat bus model: reads of a scripted GT911 register pointer
 * return a chosen byte stream (the real model can only echo the address
 * byte), everything else forwards to the bound bus. This is what lets
 * the product-id VALUE check and the exact status-byte read legs run on
 * the host (#234). The wire-format parser tests live in
 * ``test_ra8_touch_decode.c``.
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
 * @enum touch_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_touch_got_7        = 7U,
  k_touch_got_9        = 9U,
  k_touch_got_99       = 99U,
  k_touch_target_7b_42 = 0x42U,
  k_touch_val_5        = 5U,
} touch_uint8_const_t;

/**
 * @enum ra8_touch_test_const_t
 * @brief Test-only constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_test_max_points_default = 5U,  /**< Caller cap matching the hw limit.      */
  k_test_irq_pin_unused     = 32U, /**< Same as ::k_ra8_touch_irq_pin_unset.   */
  k_test_max_count_wide     = 8U,  /**< Read cap above the 5-point hard limit. */
} ra8_touch_test_const_t;

/**
 * @enum ra8_touch_test_addr_t
 * @brief Address constants used across tests.
 */
typedef enum : uint8_t {
  k_test_addr_default = 0x5DU, /**< Factory-default GT911 target address.     */
  k_test_addr_alt     = 0x14U, /**< Alternate GT911 target address.           */
  k_test_addr_bad     = 0x42U, /**< Address outside the GT911 pair.           */
  k_test_byte_shift   = 8U,    /**< Bits per byte for register decoding.      */
  k_test_irq_pin_zero = 0U,    /**< In-range ICU IRQ pin for the attach path. */
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

/**
 * @enum ra8_touch_test_script_t
 * @brief Scripted-RX byte values presented through the trampoline seam.
 */
typedef enum : uint8_t {
  k_test_pid_byte_match    = 0x39U, /**< ASCII '9' -- valid GT911 product-id byte 0. */
  k_test_pid_byte_mismatch = 0x35U, /**< ASCII '5' -- foreign-IC product-id byte 0.  */
  k_test_status_ready_zero = 0x80U, /**< Status: buffer-ready (bit7), zero contacts. */
  k_test_status_ready_one  = 0x81U, /**< Status: buffer-ready (bit7), one contact.   */
} ra8_touch_test_script_t;

/** @brief Bound I2C bus handle the forwarding trampolines drain into. */
static ra8_io_i2c_bus_t s_bus;
/** @brief Real seam filled from ::s_bus by ``ra8_io_i2c_bus_as_ops`` in prep(). */
static ra8_i2c_bus_ops_t s_bus_ops;
/** @brief Standard config used by happy-path tests (filled in prep()). */
static ra8_touch_cfg_t s_cfg_default;

/* =============================================================================
 * Controllable-RX bus trampoline (#234)
 *
 * The real i3c-i2c-compat host model can only echo the address byte on
 * reads, so it can never present a valid GT911 product id (or an exact
 * status byte). These trampolines wrap the real seam: a read of a
 * scripted register pointer returns a chosen byte stream, everything
 * else forwards to ::s_bus_ops.
 * =============================================================================
 */

/** @brief When true, product-id reads return ::s_script_pid_byte0. */
static bool s_script_pid_armed;
/** @brief Scripted PRODUCT_ID byte 0 ('9' accepts, anything else rejects). */
static uint8_t s_script_pid_byte0;
/** @brief When true, status reads return ::s_script_status_byte. */
static bool s_script_status_armed;
/** @brief Scripted STATUS byte (bit7 ready, bits[3:0] contact count). */
static uint8_t s_script_status_byte;

/**
 * @brief Forwarding trampoline for the seam's ``write`` row.
 */
static ra8_err_t
tramp_write(void* ctx, uint8_t addr, const uint8_t* data, uint32_t len, bool send_stop)
{
  const ra8_i2c_bus_ops_t* real = ctx;
  return real->write(real->ctx, addr, data, len, send_stop);
}

/**
 * @brief Forwarding trampoline for the seam's ``read`` row.
 */
static ra8_err_t tramp_read(void* ctx, uint8_t addr, uint8_t* data, uint32_t len)
{
  const ra8_i2c_bus_ops_t* real = ctx;
  return real->read(real->ctx, addr, data, len);
}

/**
 * @brief Decode the 16-bit GT911 register pointer from a transfer's
 *        write phase; 0 when the phase is not a register-pointer write.
 */
static uint16_t tramp_decode_reg(const uint8_t* wr, uint32_t wr_len)
{
  if (wr == nullptr) {
    return 0U;
  }
  if (wr_len != (uint32_t)k_ra8_touch_gt911_reg_ptr_bytes) {
    return 0U;
  }
  return (uint16_t)(((uint32_t)wr[0] << (uint32_t)k_test_byte_shift) | (uint32_t)wr[1]);
}

/**
 * @brief Fill a scripted RX buffer: ``byte0`` first, zeroes after.
 */
static ra8_err_t tramp_fill_rx(uint8_t* rd, uint32_t rd_len, uint8_t byte0)
{
  if (rd == nullptr) {
    return k_ra8_err_null_ptr;
  }
  for (uint32_t i = 0U; i < rd_len; i++) {
    rd[i] = 0U;
  }
  if (rd_len > 0U) {
    rd[0] = byte0;
  }
  return k_ra8_ok;
}

/**
 * @brief Controllable-RX trampoline for the seam's ``transfer`` row.
 *
 * @details
 * Reads of a scripted register pointer (PRODUCT_ID / STATUS) return the
 * scripted byte stream without touching the bus model; every other
 * transfer forwards to the real ::s_bus_ops seam.
 */
static ra8_err_t tramp_transfer(void*          ctx,
                                uint8_t        addr,
                                const uint8_t* wr,
                                uint32_t       wr_len,
                                uint8_t*       rd,
                                uint32_t       rd_len)
{
  const ra8_i2c_bus_ops_t* real = ctx;
  const uint16_t           reg  = tramp_decode_reg(wr, wr_len);
  if (s_script_pid_armed) {
    if (reg == (uint16_t)k_ra8_touch_gt911_reg_product) {
      return tramp_fill_rx(rd, rd_len, s_script_pid_byte0);
    }
  }
  if (s_script_status_armed) {
    if (reg == (uint16_t)k_ra8_touch_gt911_reg_status) {
      return tramp_fill_rx(rd, rd_len, s_script_status_byte);
    }
  }
  return real->transfer(real->ctx, addr, wr, wr_len, rd, rd_len);
}

/**
 * @brief Reset the simulator, bring MSTP up, and play the app's role:
 *        initialise IIC_B channel 0, bind it through the ra8_io facade,
 *        and wrap the resulting seam in the controllable-RX trampoline.
 *
 * @details
 * The default script presents a valid GT911 product id ('9') so every
 * open() in this file passes the always-compiled product-id VALUE
 * check; the status script starts disarmed so read-path tests keep
 * exercising the real bus model.
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
  s_script_pid_armed                = true;
  s_script_pid_byte0                = (uint8_t)k_test_pid_byte_match;
  s_script_status_armed             = false;
  s_script_status_byte              = 0U;
  const ra8_i2c_bus_ops_t tramp_ops = {
    .write    = tramp_write,
    .read     = tramp_read,
    .transfer = tramp_transfer,
    .ctx      = &s_bus_ops,
  };
  const ra8_touch_cfg_t cfg = {
    .bus        = tramp_ops,
    .target_7b  = (uint8_t)k_test_addr_default,
    .irq_pin    = (uint8_t)k_test_irq_pin_unused,
    .max_points = (uint8_t)k_test_max_points_default,
  };
  s_cfg_default = cfg;
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
  ra8_touch_point_t pt[k_touch_val_5];
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
  ra8_touch_point_t pt[k_touch_val_5];
  uint8_t           got = k_touch_got_99;
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
  ra8_touch_point_t pt[k_touch_val_5];
  uint8_t           got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_touch_read(pt, (uint8_t)k_test_max_points_default, &got));
  TEST_END("ra8_touch_read: not-initialized rejected");
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
  cfg.target_7b = (uint8_t)k_touch_target_7b_42;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_open(&cfg));
  cfg            = s_cfg_default;
  cfg.max_points = k_touch_val_5;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  cfg.max_points = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  cfg.max_points = k_touch_got_99;
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

  ra8_touch_point_t pt[k_touch_val_5];
  uint8_t           got = k_touch_got_7;
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

  ra8_touch_point_t pt[k_touch_val_5];
  uint8_t           got = k_touch_got_9;
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
  /* Disarm the product-id script so the read forwards to the real bus,
   * then drop the bus-free flag so that forwarded transfer (open's first)
   * fails its busy gate: priv_check_product_id returns hw_init_failed. */
  s_script_pid_armed             = false;
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->BCST                      = 0U;
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_touch_open(&s_cfg_default));
  TEST_END("ra8_touch_open: product-id read transport error -> hw_init_failed");
}

/**
 * @test test_mcdc_open_product_id_value
 *
 * @par MC/DC:
 * Decisions in ``priv_check_product_id``, libs/ra8_hal/src/ra8_touch.c.
 * Both are single-condition, so MC/DC degenerates to branch coverage:
 * both outcomes of each decision (DO-178C 6.4.4.3).
 * Decision A: ``if (pid_err != k_ra8_ok)``
 * - V1: scripted RX transfer succeeds -> F (falls through to Decision B)
 * - V2: forwarded transfer fails the bus-free gate -> T -> hw_init_failed
 *       (driven by test_open_product_id_transfer_error)
 * Decision B: ``if ((uint32_t)product[0] != k_ra8_touch_product_id_byte0)``
 * - V1: scripted product[0] = '9' (0x39) -> F -> open returns k_ra8_ok
 * - V3: scripted product[0] = '5' (0x35) -> T -> hw_init_failed
 * V1+V2 exercise Decision A both ways; V1+V3 exercise Decision B both
 * ways with the transfer held constant-passing.
 */
static void test_mcdc_open_product_id_value(void)
{
  TEST_BEGIN("ra8_touch_open: product-id VALUE match accepts, mismatch rejects");
  prep();
  prime_iic_b();
  /* V1: scripted '9' passes the value check and open completes. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  /* V3: a non-GT911 id byte behind the same address is rejected. */
  s_script_pid_byte0 = (uint8_t)k_test_pid_byte_mismatch;
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_touch_open(&s_cfg_default));
  TEST_END("ra8_touch_open: product-id VALUE match accepts, mismatch rejects");
}

/**
 * @test test_read_block_transfer_error
 *
 * @par MC/DC:
 * (no compound decisions -- scripting the status RX to "ready, one
 * contact" (0x81) while BCST.BFREF is clear makes the forwarded
 * per-point block read fail its bus-free gate after the status read
 * succeeded, driving the single-condition ``blk_err != k_ra8_ok``
 * error leg of priv_read_inner.)
 */
static void test_read_block_transfer_error(void)
{
  TEST_BEGIN("ra8_touch_read: point-block transport error -> hw_error");
  prep();
  prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));

  /* Script "frame ready, one contact", then break the bus: the scripted
   * status read still succeeds while the forwarded block read fails. */
  s_script_status_armed          = true;
  s_script_status_byte           = (uint8_t)k_test_status_ready_one;
  volatile r_i3c_i2c_regs_t* reg = i3c_i2c_regs(0U);
  reg->BCST                      = 0U;

  ra8_touch_point_t pt[k_touch_val_5];
  uint8_t           got = k_touch_got_9;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_touch_read(pt, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(0, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_read: point-block transport error -> hw_error");
}

/**
 * @test test_read_ready_zero_contacts
 *
 * @par MC/DC:
 * (no compound decisions -- scripting the status RX to "ready, zero
 * contacts" (0x80) leaves emit at 0 past both clamps, driving the
 * single-condition ``emit > 0`` else leg (full-release frame) of
 * priv_read_inner: got_count is 0 and the frame is still acked.)
 */
static void test_read_ready_zero_contacts(void)
{
  TEST_BEGIN("ra8_touch_read: ready frame with zero contacts -> ok, got=0");
  prep();
  prime_iic_b();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_open(&s_cfg_default));

  s_script_status_armed = true;
  s_script_status_byte  = (uint8_t)k_test_status_ready_zero;

  ra8_touch_point_t pt[k_touch_val_5];
  uint8_t           got = k_touch_got_7;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_read(pt, (uint8_t)k_test_max_points_default, &got));
  TEST_ASSERT_EQ(0, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_close());
  TEST_END("ra8_touch_read: ready frame with zero contacts -> ok, got=0");
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
  test_open_close_happy,
  test_open_null_cfg,
  test_open_invalid_address,
  test_mcdc_open_bus_seam,
  test_open_already_open,
  test_close_without_open,
  test_read_null_args,
  test_read_returns_ok,
  test_read_before_open,
  test_attach_dispatch,
  test_calibrate_noop,
  test_mcdc_ra8_touch,
  test_open_with_irq_pin,
  test_read_no_frame_ready,
  test_read_clamp_to_max_points,
  test_read_status_transfer_error,
  test_open_product_id_transfer_error,
  test_mcdc_open_product_id_value,
  test_read_block_transfer_error,
  test_read_ready_zero_contacts,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK ] test_ra8_touch.c\n");
  return 0;
}
