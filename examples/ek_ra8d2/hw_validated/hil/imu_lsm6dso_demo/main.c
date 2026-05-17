/**
 * @file examples/ek_ra8d2/hw_validated/hil/imu_lsm6dso_demo/main.c
 * @brief LSM6DSO 6-DoF IMU bring-up demo over IIC_B
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Stand-alone EK-RA8D2 application that exercises the new
 * ``libs/ra_lsm6dso`` driver against an LSM6DSO carrier wired to the
 * Pmod1 I2C pins (P511/SDA1 + P512/SCL1, UM Table 17 p 26). The
 * EK-RA8D2 does not have a dedicated MikroBUS socket -- the MikroE
 * 6DOF IMU 12 Click is expected to be soldered to a MikroE-to-Pmod
 * adapter (or its I2C pads directly hand-wired into J26's I2C side).
 *
 * Flow:
 *   1. ``ra_cgc_init`` -- bring CPUCLK0 and PCLKA up.
 *   2. ``ra_mstp_init`` + PFS routing of SCL1/SDA1 and SCI8 console pins.
 *   3. ``ra_iic_b_init(0, ...)`` at 100 kHz Sm.
 *   4. ``ra_lsm6dso_init`` against the IIC_B-backed transport adapter.
 *   5. Read WHO_AM_I. On success print
 *      ``"lsm6dso: who_am_i=0x6c\r\n"`` (banner line for HIL scrape).
 *      On failure print ``"lsm6dso: I2C NAK\r\n"`` and latch LED2.
 *   6. Configure +-2 g (XL), +-250 dps (G), 104 Hz ODR.
 *   7. Loop: every 250 ms read accel + gyro + temperature and print
 *      ``"lsm6dso: ax=.. ay=.. az=.. gx=.. gy=.. gz=.. temp=..\r\n"``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_iic_b.h"
#include "ra_isr.h"
#include "ra_lsm6dso.h"
#include "ra_mstp.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/* =============================================================================
 * App-wide tunables
 * =============================================================================
 */

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_imu_demo_baud        = 115200U,
  k_imu_demo_period_ms   = 250U,
  k_imu_demo_bus_hz      = 100000U,
  k_imu_demo_sci_channel = 8U,
  k_imu_demo_iic_channel = 0U,
} imu_demo_const_t;

/** @brief Pinout for SCI8 console (PD02 TXD8 / PD03 RXD8), UM Table 13. */
static const ra_port_pin_t k_imu_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_imu_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Pinout for IIC channel 0 (P512 SCL / P511 SDA on EK-RA8D2). */
static const ra_port_pin_t k_imu_demo_pin_scl =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_12);
static const ra_port_pin_t k_imu_demo_pin_sda =
  (ra_port_pin_t)(((uint16_t)k_ra_port_5 << 8) | (uint16_t)k_ra_pin_11);

/* =============================================================================
 * Tiny printf-equivalents for HIL log scraping
 * =============================================================================
 */

/** @brief Banner line scraped by hil.conf. */
static const uint8_t k_imu_demo_banner_ok[]  = "lsm6dso: who_am_i=0x6c\r\n";
static const uint8_t k_imu_demo_banner_bad[] = "lsm6dso: who_am_i mismatch\r\n";
static const uint8_t k_imu_demo_banner_nak[] = "lsm6dso: I2C NAK\r\n";
static const uint8_t k_imu_demo_banner_err[] = "lsm6dso: read error\r\n";

/**
 * @brief Park forever after a fatal error.
 *
 * @pre Called only after a fatal init failure.
 * @post CPU is parked; only a debugger or reset wakes it.
 *
 * @since 0.1.0
 */
static void imu_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * Tiny integer -> decimal serialiser for the periodic log line
 * =============================================================================
 */

/** @brief Result buffer for one decimal int16_t (max "-32768" = 6 bytes
 *         plus the trailing NUL). */
typedef enum : uint32_t {
  k_imu_demo_int_str_cap = 8U,
} imu_demo_str_cap_t;

/**
 * @brief Convert a signed 16-bit int into a decimal ASCII string.
 *
 * @details
 * Avoids dragging in newlib's printf, which would push the firmware
 * image past the smoke-test footprint budget. The buffer must be at
 * least ``k_imu_demo_int_str_cap`` bytes.
 *
 * @param[in]  value Integer to convert.
 * @param[out] out   Destination buffer (must be >= 8 bytes).
 *
 * @return Number of bytes written (excluding the NUL).
 *
 * @pre ``out`` is non-NULL.
 * @pre Capacity is at least ``k_imu_demo_int_str_cap``.
 * @post ``out`` carries a NUL-terminated decimal representation.
 * @post The returned length is in ``[1, 6]``.
 *
 * @since 0.1.0
 */
static uint32_t imu_demo_i32_to_dec(int32_t value, uint8_t* out)
{
  uint8_t  tmp[k_imu_demo_int_str_cap] = {};
  uint32_t n                            = 0U;
  bool     neg                          = false;
  /* Two's complement edge case: INT16_MIN.  We work in int32_t so we
   * have headroom to negate. */
  int32_t v = value;
  if (v < 0) {
    neg = true;
    v   = -v;
  }
  if (v == 0) {
    tmp[n++] = (uint8_t)'0';
  } else {
    while (v > 0 && n < (uint32_t)k_imu_demo_int_str_cap) {
      tmp[n++] = (uint8_t)('0' + (uint8_t)(v % 10));
      v /= 10;
    }
  }
  if (neg && n < (uint32_t)k_imu_demo_int_str_cap) {
    tmp[n++] = (uint8_t)'-';
  }
  /* Reverse into the caller's buffer. */
  for (uint32_t i = 0U; i < n; ++i) {
    out[i] = tmp[n - 1U - i];
  }
  if (n < (uint32_t)k_imu_demo_int_str_cap) {
    out[n] = 0U;
  }
  return n;
}

/* =============================================================================
 * IIC_B bus adapter for the ra_lsm6dso transport interface
 * =============================================================================
 */

/** @brief Adapter context: holds channel + 7-bit address. */
typedef struct {
  uint8_t channel; /**< IIC_B channel (0 on RA8D2).      */
  uint8_t addr7;   /**< 7-bit slave address.             */
} imu_demo_iic_ctx_t;

/**
 * @brief Transport adapter: read N bytes starting at register ``reg``.
 *
 * @details
 * Uses ``ra_iic_b_transfer`` which does the write-RESTART-read in one
 * bus transaction -- this is the I2C pattern the LSM6DSO needs to
 * read an auto-incremented register block.
 *
 * @param[in]  ctx Adapter context (cast to ``imu_demo_iic_ctx_t*``).
 * @param[in]  reg First register address.
 * @param[out] buf Destination buffer.
 * @param[in]  len Byte count.
 *
 * @return Forwarded ``ra_iic_b_transfer`` return code.
 */
static ra_err_t imu_demo_iic_read(void* ctx, uint8_t reg, uint8_t* buf, uint32_t len)
{
  const imu_demo_iic_ctx_t* c = (const imu_demo_iic_ctx_t*)ctx;
  return ra_iic_b_transfer(c->channel, c->addr7, &reg, 1U, buf, len);
}

/**
 * @brief Transport adapter: write N bytes starting at register ``reg``.
 *
 * @details
 * Stages ``[reg][buf[0..len-1]]`` on a small stack scratch and issues
 * a single ``ra_iic_b_write``. Capped at ``k_imu_demo_iic_tx_cap``
 * bytes payload because the LSM6DSO driver never writes more than
 * eight bytes at once.
 *
 * @param[in] ctx Adapter context.
 * @param[in] reg First register address.
 * @param[in] buf Source buffer.
 * @param[in] len Byte count.
 *
 * @return Forwarded ``ra_iic_b_write`` return code.
 */
typedef enum : uint32_t {
  k_imu_demo_iic_tx_cap = 16U,
} imu_demo_iic_cap_t;

static ra_err_t imu_demo_iic_write(void*          ctx,
                                   uint8_t        reg,
                                   const uint8_t* buf,
                                   uint32_t       len)
{
  if (len > (uint32_t)k_imu_demo_iic_tx_cap - 1U) {
    return k_ra_err_invalid_arg;
  }
  uint8_t scratch[k_imu_demo_iic_tx_cap] = {};
  scratch[0]                              = reg;
  for (uint32_t i = 0U; i < len; ++i) {
    scratch[i + 1U] = buf[i];
  }
  const imu_demo_iic_ctx_t* c = (const imu_demo_iic_ctx_t*)ctx;
  return ra_iic_b_write(c->channel, c->addr7, scratch, len + 1U, false);
}

/* =============================================================================
 * Init helpers
 * =============================================================================
 */

/**
 * @brief Bring CGC + SysTick + MSTP up; return PCLKA via @p out_pclka_hz.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post On success ``*out_pclka_hz`` is non-zero and CGC is live.
 *
 * @since 0.1.0
 */
static void imu_demo_clocks_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz) != k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    imu_demo_panic_halt();
  }
}

/**
 * @brief Route SCI8 console pins + IIC channel-0 SCL/SDA via PFS.
 *
 * @pre ``ra_mstp_init`` already ran so PFS writes land.
 * @post All four pins are in their peripheral PSEL on success.
 *
 * @since 0.1.0
 */
static void imu_demo_pfs_or_halt(void)
{
  if (ra_pfs_route_peripheral(k_imu_demo_pin_txd, k_ra_psel_sci_async, "imu_demo.txd8") !=
      k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_imu_demo_pin_rxd, k_ra_psel_sci_async, "imu_demo.rxd8") !=
      k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_imu_demo_pin_scl, k_ra_psel_iic, "imu_demo.scl1") != k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_pfs_route_peripheral(k_imu_demo_pin_sda, k_ra_psel_iic, "imu_demo.sda1") != k_ra_ok) {
    imu_demo_panic_halt();
  }
}

/**
 * @brief Bring CGC + SysTick + console + IIC_B up. Panic-halts on any fail.
 *
 * @since 0.1.0
 */
static void imu_demo_setup_or_halt(void)
{
  uint32_t pclka_hz = 0U;
  imu_demo_clocks_or_halt(&pclka_hz);
  imu_demo_pfs_or_halt();

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_imu_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_imu_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    imu_demo_panic_halt();
  }
  const ra_iic_b_cfg_t iic_cfg = {
    .bus_hz   = k_imu_demo_bus_hz,
    .pclka_hz = pclka_hz,
  };
  if (ra_iic_b_init((uint8_t)k_imu_demo_iic_channel, &iic_cfg) != k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    imu_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    imu_demo_panic_halt();
  }
}

/**
 * @brief Emit one line of bytes via SCI8.
 *
 * @param[in] bytes Null-terminated byte array.
 * @param[in] len   Byte count.
 *
 * @since 0.1.0
 */
static void imu_demo_tx(const uint8_t* bytes, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_imu_demo_sci_channel, bytes, len);
}

/**
 * @brief Emit a labelled signed integer: ``"<label>=<n>"``.
 *
 * @param[in] label    NUL-terminated label.
 * @param[in] label_len Length of the label.
 * @param[in] value    Signed integer.
 *
 * @since 0.1.0
 */
static void imu_demo_emit_kv(const uint8_t* label, uint32_t label_len, int32_t value)
{
  uint8_t buf[k_imu_demo_int_str_cap] = {};
  imu_demo_tx(label, label_len);
  const uint32_t n = imu_demo_i32_to_dec(value, buf);
  imu_demo_tx(buf, n);
}

/* =============================================================================
 * Main
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring up CGC + IIC_B + LSM6DSO, print samples.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the read + print loop forever.
 * @post On any HAL hard error LED2 latches ON and the loop exits.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  imu_demo_setup_or_halt();
  ra_isr_globals_enable();

  imu_demo_iic_ctx_t ctx = {
    .channel = (uint8_t)k_imu_demo_iic_channel,
    .addr7   = (uint8_t)k_lsm6dso_i2c_addr_sa0_high,
  };
  const ra_lsm6dso_bus_t bus = {
    .read_regs  = imu_demo_iic_read,
    .write_regs = imu_demo_iic_write,
    .ctx        = &ctx,
  };
  ra_lsm6dso_t dev = {};
  if (ra_lsm6dso_init(&dev, &bus) != k_ra_ok) {
    imu_demo_tx(k_imu_demo_banner_err, (uint32_t)(sizeof(k_imu_demo_banner_err) - 1U));
    (void)ra_board_led_on(k_ra_board_led2);
    imu_demo_panic_halt();
  }

  uint8_t        who = 0U;
  const ra_err_t r   = ra_lsm6dso_who_am_i(&dev, &who);
  if (r != k_ra_ok) {
    imu_demo_tx(k_imu_demo_banner_nak, (uint32_t)(sizeof(k_imu_demo_banner_nak) - 1U));
    (void)ra_board_led_on(k_ra_board_led2);
    imu_demo_panic_halt();
  }
  if (who != (uint8_t)k_lsm6dso_who_am_i_value) {
    imu_demo_tx(k_imu_demo_banner_bad, (uint32_t)(sizeof(k_imu_demo_banner_bad) - 1U));
    (void)ra_board_led_on(k_ra_board_led2);
    imu_demo_panic_halt();
  }
  imu_demo_tx(k_imu_demo_banner_ok, (uint32_t)(sizeof(k_imu_demo_banner_ok) - 1U));

  /* Configure: +-2 g, +-250 dps, 104 Hz. */
  if (ra_lsm6dso_set_accel_range(&dev, k_lsm6dso_xl_fs_2g) != k_ra_ok) {
    (void)ra_board_led_on(k_ra_board_led2);
    imu_demo_panic_halt();
  }
  if (ra_lsm6dso_set_gyro_range(&dev, k_lsm6dso_g_fs_250dps) != k_ra_ok) {
    (void)ra_board_led_on(k_ra_board_led2);
    imu_demo_panic_halt();
  }
  if (ra_lsm6dso_set_odr(&dev, k_lsm6dso_odr_104hz) != k_ra_ok) {
    (void)ra_board_led_on(k_ra_board_led2);
    imu_demo_panic_halt();
  }

  static const uint8_t k_label_prefix[]  = "lsm6dso: ax=";
  static const uint8_t k_label_ay[]      = " ay=";
  static const uint8_t k_label_az[]      = " az=";
  static const uint8_t k_label_gx[]      = " gx=";
  static const uint8_t k_label_gy[]      = " gy=";
  static const uint8_t k_label_gz[]      = " gz=";
  static const uint8_t k_label_temp[]    = " temp=";
  static const uint8_t k_label_newline[] = "\r\n";

  while (1) {
    ra_lsm6dso_xyz_t a       = {};
    ra_lsm6dso_xyz_t g       = {};
    int32_t          tcenti  = 0;
    const ra_err_t   ra      = ra_lsm6dso_read_accel(&dev, &a);
    const ra_err_t   rg      = ra_lsm6dso_read_gyro(&dev, &g);
    const ra_err_t   rt      = ra_lsm6dso_read_temp(&dev, &tcenti);
    if (ra != k_ra_ok || rg != k_ra_ok || rt != k_ra_ok) {
      imu_demo_tx(k_imu_demo_banner_err,
                  (uint32_t)(sizeof(k_imu_demo_banner_err) - 1U));
      (void)ra_board_led_on(k_ra_board_led2);
    } else {
      imu_demo_emit_kv(k_label_prefix, (uint32_t)(sizeof(k_label_prefix) - 1U), a.x);
      imu_demo_emit_kv(k_label_ay,     (uint32_t)(sizeof(k_label_ay) - 1U),     a.y);
      imu_demo_emit_kv(k_label_az,     (uint32_t)(sizeof(k_label_az) - 1U),     a.z);
      imu_demo_emit_kv(k_label_gx,     (uint32_t)(sizeof(k_label_gx) - 1U),     g.x);
      imu_demo_emit_kv(k_label_gy,     (uint32_t)(sizeof(k_label_gy) - 1U),     g.y);
      imu_demo_emit_kv(k_label_gz,     (uint32_t)(sizeof(k_label_gz) - 1U),     g.z);
      imu_demo_emit_kv(k_label_temp,   (uint32_t)(sizeof(k_label_temp) - 1U),   tcenti);
      imu_demo_tx(k_label_newline, (uint32_t)(sizeof(k_label_newline) - 1U));
    }
    (void)ra_board_led_toggle(k_ra_board_led1);
    ra_delay_ms(k_imu_demo_period_ms);
  }

  imu_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
