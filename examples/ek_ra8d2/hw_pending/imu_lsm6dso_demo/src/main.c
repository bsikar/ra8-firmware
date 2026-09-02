/**
 * @file examples/ek_ra8d2/hw_pending/imu_lsm6dso_demo/src/main.c
 * @brief LSM6DSO 6-DoF IMU bring-up demo over IIC_B
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Stand-alone EK-RA8D2 application that exercises the
 * ``libs/ra8_lsm6dso`` driver against a MikroE LSM6DSO 6-DoF IMU 12 Click
 * sitting in the MikroBUS slot. Project wiring (see BSP comments at
 * ``k_ra8_board_mikrobus_i2c_*``):
 *
 *   - Pmod1 (J26) is free (Wi-Fi/BLE/OTA is on an ESP32 companion IC).
 *   - Pmod2 (J25) is occupied by the Digilent PMOD MicroSD.
 *   - The LSM6DSO Click is wired to a MikroBUS socket adapted onto the
 *     EK-RA8D2 via a MikroE Click-Shield breakout on the Arduino
 *     headers; the shield routes MikroBUS SDA/SCL to Arduino D14/D15
 *     which the EK-RA8D2 v1 UM Table 20 p 28 identifies as
 *     SDA1 = P511 / SCL1 = P512.
 *
 * Pins resolve through the BSP symbols ``k_ra8_board_mikrobus_i2c_sda``
 * / ``_scl``; the underlying IIC_B controller is channel
 * ``k_ra8_board_mikrobus_iic_b_channel`` (= 0 -- the RA8D2 group exposes
 * only one IIC_B controller).
 *
 * Flow:
 *   1. ``ra8_cgc_init`` -- bring CPUCLK0 and PCLKA up.
 *   2. PFS routing of MikroBUS SDA/SCL and the SCI8 console pins.
 *   3. ``ra8_i3c_init`` on the MikroBUS IIC_B channel at 100 kHz Sm.
 *   4. ``ra8_lsm6dso_init`` against the IIC_B-backed transport adapter.
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

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_i3c.h"
#include "ra8_isr.h"
#include "ra8_lsm6dso.h"
#include "ra8_mstp.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/* =============================================================================
 * App-wide tunables
 * =============================================================================
 */

/** @brief App-wide tunables. */
typedef enum : uint32_t {
  k_imu_decimal_base   = 10U,     /**< Radix for integer-to-ASCII. */
  k_imu_demo_baud      = 115200U, /**< Imu demo baud.              */
  k_imu_demo_period_ms = 250U,    /**< Imu demo period ms.         */
  k_imu_demo_bus_hz    = 100000U, /**< Imu demo bus Hz.            */
  k_imu_demo_iic_channel =
    (uint32_t)k_ra8_board_mikrobus_iic_b_channel, /**< Imu demo iic channel. */
} imu_demo_const_t;

/** @brief MikroBUS SDA/SCL routed through Arduino D14/D15 to SDA1/SCL1. */
static const ra8_port_pin_t k_imu_demo_pin_scl = (ra8_port_pin_t)k_ra8_board_mikrobus_i2c_scl;
static const ra8_port_pin_t k_imu_demo_pin_sda = (ra8_port_pin_t)k_ra8_board_mikrobus_i2c_sda;

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
  k_imu_demo_int_str_cap = 8U, /**< Imu demo int str cap. */
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
  uint32_t n                           = 0U;
  bool     neg                         = false;
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
      tmp[n++] = (uint8_t)('0' + (uint8_t)((uint32_t)v % k_imu_decimal_base));
      v        = (int32_t)((uint32_t)v / k_imu_decimal_base);
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
 * IIC_B bus adapter for the ra8_lsm6dso transport interface
 * =============================================================================
 */

/** @brief Adapter context: holds channel + 7-bit address. */
typedef struct {
  uint8_t channel; /**< IIC_B channel (0 on RA8D2). */
  uint8_t addr7;   /**< 7-bit peripheral address.   */
} imu_demo_iic_ctx_t;

/**
 * @brief Transport adapter: read N bytes starting at register ``reg``.
 *
 * @details
 * Uses ``ra8_i3c_transfer`` which does the write-RESTART-read in one
 * bus transaction -- this is the I2C pattern the LSM6DSO needs to
 * read an auto-incremented register block.
 *
 * @param[in]  ctx Adapter context (cast to ``imu_demo_iic_ctx_t*``).
 * @param[in]  reg First register address.
 * @param[out] buf Destination buffer.
 * @param[in]  len Byte count.
 *
 * @return Forwarded ``ra8_i3c_transfer`` return code.
 */
static ra8_err_t imu_demo_iic_read(void* ctx, uint8_t reg, uint8_t* buf, uint32_t len)
{
  const imu_demo_iic_ctx_t* c = (const imu_demo_iic_ctx_t*)ctx;
  return ra8_i3c_transfer(c->channel, c->addr7, &reg, 1U, buf, len);
}

/**
 * @brief Transport adapter: write N bytes starting at register ``reg``.
 *
 * @details
 * Stages ``[reg][buf[0..len-1]]`` on a small stack scratch and issues
 * a single ``ra8_i3c_write``. Capped at ``k_imu_demo_iic_tx_cap``
 * bytes payload because the LSM6DSO driver never writes more than
 * eight bytes at once.
 *
 * @param[in] ctx Adapter context.
 * @param[in] reg First register address.
 * @param[in] buf Source buffer.
 * @param[in] len Byte count.
 *
 * @return Forwarded ``ra8_i3c_write`` return code.
 */
typedef enum : uint32_t {
  k_imu_demo_iic_tx_cap = 16U, /**< Imu demo iic TX cap. */
} imu_demo_iic_cap_t;

static ra8_err_t imu_demo_iic_write(void* ctx, uint8_t reg, const uint8_t* buf, uint32_t len)
{
  if (len > (uint32_t)k_imu_demo_iic_tx_cap - 1U) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t scratch[k_imu_demo_iic_tx_cap] = {};
  scratch[0]                             = reg;
  for (uint32_t i = 0U; i < len; ++i) {
    scratch[i + 1U] = buf[i];
  }
  const imu_demo_iic_ctx_t* c = (const imu_demo_iic_ctx_t*)ctx;
  return ra8_i3c_write(c->channel, c->addr7, scratch, len + 1U, false);
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
  if (ra8_cgc_init() != k_ra8_ok) {
    imu_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    imu_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, out_pclka_hz) != k_ra8_ok) {
    imu_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    imu_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    imu_demo_panic_halt();
  }
}

/**
 * @brief Route IIC channel-0 SCL/SDA via PFS.
 *
 * @pre ``ra8_mstp_init`` already ran so PFS writes land.
 * @post Both IIC pins are in their peripheral PSEL on success.
 *
 * @since 0.1.0
 */
static void imu_demo_pfs_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_imu_demo_pin_scl, k_ra8_psel_iic, "imu_demo.scl1") != k_ra8_ok) {
    imu_demo_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_imu_demo_pin_sda, k_ra8_psel_iic, "imu_demo.sda1") != k_ra8_ok) {
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

  if (ra8_board_uart_console_init((uint32_t)k_imu_demo_baud) != k_ra8_ok) {
    imu_demo_panic_halt();
  }
  const ra8_i3c_cfg_t iic_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = k_imu_demo_bus_hz,
    .pclka_hz = pclka_hz,
  };
  if (ra8_i3c_init((uint8_t)k_imu_demo_iic_channel, &iic_cfg) != k_ra8_ok) {
    imu_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    imu_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    imu_demo_panic_halt();
  }
}

/**
 * @brief Emit one line of bytes via the BSP UART console (SCI8).
 *
 * @param[in] bytes Null-terminated byte array.
 * @param[in] len   Byte count.
 *
 * @since 0.1.0
 */
static void imu_demo_tx(const uint8_t* bytes, uint32_t len)
{
  (void)ra8_board_uart_console_write(bytes, (size_t)len);
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
 * Main-loop helpers (kept out of main() to satisfy NASA P10 Rule 4).
 * =============================================================================
 */

/**
 * @brief Verify the LSM6DSO is alive and is the expected silicon.
 *
 * @details
 * Reads WHO_AM_I and compares against ``k_lsm6dso_who_am_i_value``.
 * On any failure the function emits a banner, latches LED2, and
 * parks via ``imu_demo_panic_halt`` -- it never returns to the caller
 * in that case.
 *
 * @param[in,out] dev Driver instance bound by ``ra8_lsm6dso_init``.
 *
 * @pre ``dev`` was bound by a successful ``ra8_lsm6dso_init``.
 * @pre SCI8 is initialized for banner emission.
 * @post On success the OK banner has been printed.
 * @post On failure the function never returns; LED2 is latched ON.
 *
 * @note Halts the CPU on failure -- not callable from ISR context.
 *
 * @since 0.1.0
 */
static void imu_demo_check_who_am_i(ra8_lsm6dso_t* dev)
{
  uint8_t         who = 0U;
  const ra8_err_t r   = ra8_lsm6dso_who_am_i(dev, &who);
  if (r != k_ra8_ok) {
    imu_demo_tx(k_imu_demo_banner_nak, (uint32_t)(sizeof(k_imu_demo_banner_nak) - 1U));
    (void)ra8_board_led_on(k_ra8_board_led2);
    imu_demo_panic_halt();
  }
  if (who != (uint8_t)k_lsm6dso_who_am_i_value) {
    imu_demo_tx(k_imu_demo_banner_bad, (uint32_t)(sizeof(k_imu_demo_banner_bad) - 1U));
    (void)ra8_board_led_on(k_ra8_board_led2);
    imu_demo_panic_halt();
  }
  imu_demo_tx(k_imu_demo_banner_ok, (uint32_t)(sizeof(k_imu_demo_banner_ok) - 1U));
}

/**
 * @brief Apply the demo's fixed sensor configuration (+-2 g, +-250 dps, 104 Hz).
 *
 * @details
 * Wraps the three configuration setters into a single panic-on-error
 * gate. Halts the CPU and latches LED2 if any setter returns non-OK.
 *
 * @param[in,out] dev Driver instance bound by ``ra8_lsm6dso_init``.
 *
 * @pre ``dev->initialized`` is true.
 * @pre The IIC bus is up and the LSM6DSO is ACKing.
 * @post On success the sensor is configured and ready for reads.
 * @post On failure the function never returns; LED2 is latched ON.
 *
 * @note Halts the CPU on failure -- not callable from ISR context.
 *
 * @since 0.1.0
 */
static void imu_demo_configure(ra8_lsm6dso_t* dev)
{
  if (ra8_lsm6dso_set_accel_range(dev, k_lsm6dso_xl_fs_2g) != k_ra8_ok) {
    (void)ra8_board_led_on(k_ra8_board_led2);
    imu_demo_panic_halt();
  }
  if (ra8_lsm6dso_set_gyro_range(dev, k_lsm6dso_g_fs_250dps) != k_ra8_ok) {
    (void)ra8_board_led_on(k_ra8_board_led2);
    imu_demo_panic_halt();
  }
  if (ra8_lsm6dso_set_odr(dev, k_lsm6dso_odr_104hz) != k_ra8_ok) {
    (void)ra8_board_led_on(k_ra8_board_led2);
    imu_demo_panic_halt();
  }
}

/**
 * @brief Read one cycle of accel + gyro + temperature and emit the log line.
 *
 * @details
 * Performs three back-to-back driver reads and, on full success,
 * prints the ``"lsm6dso: ax=.. ay=.. ..."`` log line scraped by the
 * HIL harness. On any single read failure the error banner is
 * printed and LED2 is latched ON for the rest of the run.
 *
 * @param[in,out] dev Driver instance bound by ``ra8_lsm6dso_init``.
 *
 * @pre ``dev->initialized`` is true.
 * @pre SCI8 has been initialized for banner emission.
 * @post Exactly one ``"lsm6dso: ..."`` line has been emitted on success.
 * @post LED2 is ON if any read returned non-OK; otherwise unchanged.
 *
 * @note Not thread-safe -- single-threaded background loop only.
 *
 * @since 0.1.0
 */
static void imu_demo_sample_and_emit(ra8_lsm6dso_t* dev)
{
  static const uint8_t k_label_prefix[]  = "lsm6dso: ax=";
  static const uint8_t k_label_ay[]      = " ay=";
  static const uint8_t k_label_az[]      = " az=";
  static const uint8_t k_label_gx[]      = " gx=";
  static const uint8_t k_label_gy[]      = " gy=";
  static const uint8_t k_label_gz[]      = " gz=";
  static const uint8_t k_label_temp[]    = " temp=";
  static const uint8_t k_label_newline[] = "\r\n";

  ra8_lsm6dso_xyz_t a      = {};
  ra8_lsm6dso_xyz_t g      = {};
  int32_t           tcenti = 0;
  const ra8_err_t   ra     = ra8_lsm6dso_read_accel(dev, &a);
  const ra8_err_t   rg     = ra8_lsm6dso_read_gyro(dev, &g);
  const ra8_err_t   rt     = ra8_lsm6dso_read_temp(dev, &tcenti);
  if (ra != k_ra8_ok || rg != k_ra8_ok || rt != k_ra8_ok) {
    imu_demo_tx(k_imu_demo_banner_err, (uint32_t)(sizeof(k_imu_demo_banner_err) - 1U));
    (void)ra8_board_led_on(k_ra8_board_led2);
    return;
  }
  imu_demo_emit_kv(k_label_prefix, (uint32_t)(sizeof(k_label_prefix) - 1U), a.x);
  imu_demo_emit_kv(k_label_ay, (uint32_t)(sizeof(k_label_ay) - 1U), a.y);
  imu_demo_emit_kv(k_label_az, (uint32_t)(sizeof(k_label_az) - 1U), a.z);
  imu_demo_emit_kv(k_label_gx, (uint32_t)(sizeof(k_label_gx) - 1U), g.x);
  imu_demo_emit_kv(k_label_gy, (uint32_t)(sizeof(k_label_gy) - 1U), g.y);
  imu_demo_emit_kv(k_label_gz, (uint32_t)(sizeof(k_label_gz) - 1U), g.z);
  imu_demo_emit_kv(k_label_temp, (uint32_t)(sizeof(k_label_temp) - 1U), tcenti);
  imu_demo_tx(k_label_newline, (uint32_t)(sizeof(k_label_newline) - 1U));
}

/* =============================================================================
 * Main
 * =============================================================================
 */

/**
 * @brief Application entry: bring up CGC + IIC_B + LSM6DSO, print samples.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the read + print loop forever.
 * @post On any HAL hard error LED2 latches ON and the loop exits.
 *
 * @since 0.1.0
 */
void main(void)
{
  imu_demo_setup_or_halt();
  ra8_isr_globals_enable();

  imu_demo_iic_ctx_t ctx = {
    .channel = (uint8_t)k_imu_demo_iic_channel,
    .addr7   = (uint8_t)k_lsm6dso_i2c_addr_sa0_high,
  };
  const ra8_lsm6dso_bus_t bus = {
    .read_regs  = imu_demo_iic_read,
    .write_regs = imu_demo_iic_write,
    .ctx        = &ctx,
  };
  ra8_lsm6dso_t dev = {};
  if (ra8_lsm6dso_init(&dev, &bus) != k_ra8_ok) {
    imu_demo_tx(k_imu_demo_banner_err, (uint32_t)(sizeof(k_imu_demo_banner_err) - 1U));
    (void)ra8_board_led_on(k_ra8_board_led2);
    imu_demo_panic_halt();
  }

  imu_demo_check_who_am_i(&dev);
  /* Configure: +-2 g, +-250 dps, 104 Hz. */
  imu_demo_configure(&dev);

  while (1) {
    imu_demo_sample_and_emit(&dev);
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms(k_imu_demo_period_ms);
  }

  imu_demo_panic_halt();
}
