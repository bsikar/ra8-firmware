/**
 * @file examples/ek_ra8d2/hw_pending/battery_monitor_demo/main.c
 * @brief Battery state-of-charge monitor over a MAX17048-class fuel gauge.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Reads battery **state-of-charge** + **charge direction** from a MAX17048-class
 * I2C fuel gauge (7-bit address 0x36) on the MikroBUS / Pmod1 IIC_B bus and
 * reports them on the SCI8 console once a second:
 *
 *   - **SOC** (register 0x04): the high byte is the integer percent, so a single
 *     SMBus Read-Byte-Data of 0x04 yields the battery % directly.
 *   - **CRATE** (register 0x16): a signed charge rate. Its high byte's sign bit
 *     is 0 while charging (positive rate) and 1 while discharging (negative), so
 *     reading the high byte tells the charge direction.
 *
 * Each second: ``battery: soc=NN% chg=Y/N PASS``. Each reading is also folded
 * into the ``ra8_batt`` nag policy, which prints a one-shot ``battery: NAG LOW``
 * (SOC <=20%) or ``battery: NAG CRITICAL`` (SOC <=10%) on the descent into each
 * band -- edge-triggered with hysteresis, so a steady low battery does not spam.
 * ``g_bat_soc`` / ``g_bat_chg`` / ``g_bat_nag`` / ``g_bat_heartbeat`` mirror the
 * result for headless probing.
 *
 * @note **Headless-emulator status.** ``tools/ra8_emulator`` models the MAX17048
 * fuel gauge (``board_periph_i2c.c``): its SOC + CRATE registers are driven by
 * the fake battery state (settable via ``--battery <pct>`` / ``--charge``),
 * so this app reads a real percent over the modelled I2C bus and reaches its
 * banner headlessly. On a stock EK-RA8D2 the fuel gauge is not fitted (a NAK
 * banner prints), so the app lives in ``hw_pending/`` until a real fuel-gauge
 * board is on the MikroBUS. See ``README.md`` for the bench plan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_batt.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_smbus.h"
#include "ra8_time.h"

/** @enum bm_consts_t @brief Console / bus / fuel-gauge knobs (no magic numbers). */
typedef enum : uint32_t {
  /** Console baud. */
  k_bm_uart_baud = 115200U,
  /** IIC_B bit rate. */
  k_bm_bus_hz = 100000U,
  /** IIC_B channel (0). */
  k_bm_iic_chan = (uint32_t)k_ra8_board_mikrobus_iic_b_channel,
  /** Poll period. */
  k_bm_period_ms  = 1000U,
  k_bm_fg_addr    = 0x36U, /**< MAX17048 7-bit I2C address.                 */
  k_bm_reg_soc    = 0x04U, /**< SOC register (high byte = integer percent). */
  k_bm_reg_crate  = 0x16U, /**< CRATE register (signed charge rate).        */
  k_bm_sign_mask  = 0x80U, /**< High-byte sign bit (1 = negative).          */
  k_bm_soc_max    = 100U,  /**< SOC clamp for display.                      */
  k_bm_dec_ten    = 10U,   /**< Decimal base.                               */
  k_bm_dec_digits = 3U,    /**< Max decimal digits for a 0..100 value.      */
} bm_consts_t;

/** @brief MikroBUS SCL routed through the Pmod1 I2C side (SCL1). */
static const ra8_port_pin_t k_bm_pin_scl = (ra8_port_pin_t)k_ra8_board_mikrobus_i2c_scl;
/** @brief MikroBUS SDA routed through the Pmod1 I2C side (SDA1). */
static const ra8_port_pin_t k_bm_pin_sda = (ra8_port_pin_t)k_ra8_board_mikrobus_i2c_sda;

/**
 * @var s_bm_bus
 * @brief Bound I2C bus handle the SMBus layer's injected seam points at.
 *
 * @details
 * File-scope because the seam's `ctx` references it for the whole run.
 *
 * @note Written once during bring-up, then read-only.
 * @warning Do not rebind while the SMBus layer is initialised.
 * @since 0.1.0
 */
static ra8_io_i2c_bus_t s_bm_bus;

static const uint8_t k_msg_boot[]     = "battery-monitor: boot\r\n";
static const uint8_t k_msg_fail[]     = "battery-monitor: FAIL init\r\n";
static const uint8_t k_msg_open[]     = "battery: FAIL open\r\n";
static const uint8_t k_msg_nak[]      = "battery: NAK (no fuel gauge)\r\n";
static const uint8_t k_msg_pre[]      = "battery: soc=";
static const uint8_t k_msg_pct[]      = "% chg=";
static const uint8_t k_msg_ok[]       = " PASS\r\n";
static const uint8_t k_msg_nag_low[]  = "battery: NAG LOW soc=";
static const uint8_t k_msg_nag_crit[] = "battery: NAG CRITICAL soc=";
static const uint8_t k_msg_nag_tail[] = "%\r\n";

/**
 * @var g_bat_soc
 * @brief Last read state-of-charge percent (0..100).
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_bat_soc = 0U;

/**
 * @var g_bat_chg
 * @brief 1 when the fuel gauge reports a positive (charging) rate.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_bat_chg = 0U;

/**
 * @var g_bat_nag
 * @brief Last raised nag level (::ra8_batt_nag_t), 0 when none this poll.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_bat_nag = 0U;

/**
 * @var g_bat_heartbeat
 * @brief Bumps once per poll -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_bat_heartbeat = 0U;

/** @brief Emit a byte run on the SCI8 console. */
static void bm_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void bm_panic_halt(const uint8_t* msg, uint32_t len)
{
  bm_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a small unsigned integer (0..100) in decimal. */
static void bm_print_uint(uint32_t value)
{
  uint8_t  buf[k_bm_dec_digits];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_bm_dec_digits)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_bm_dec_ten));
    n++;
    value /= (uint32_t)k_bm_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    bm_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Emit the low/critical battery nag line for @p nag at @p soc. */
static void bm_print_nag(ra8_batt_nag_t nag, uint8_t soc)
{
  if (nag == k_ra8_batt_nag_low) {
    bm_print(k_msg_nag_low, (uint32_t)sizeof(k_msg_nag_low) - 1U);
  } else if (nag == k_ra8_batt_nag_critical) {
    bm_print(k_msg_nag_crit, (uint32_t)sizeof(k_msg_nag_crit) - 1U);
  } else {
    return;
  }
  bm_print_uint((uint32_t)soc);
  bm_print(k_msg_nag_tail, (uint32_t)sizeof(k_msg_nag_tail) - 1U);
}

/** @brief Route the MikroBUS IIC SCL/SDA via PFS. */
[[nodiscard]] static ra8_err_t bm_pins_init(void)
{
  if (ra8_pfs_route_peripheral(k_bm_pin_scl, k_ra8_psel_iic, "battery.scl1") != k_ra8_ok) {
    return k_ra8_err_hw_init_failed;
  }
  return ra8_pfs_route_peripheral(k_bm_pin_sda, k_ra8_psel_iic, "battery.sda1");
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void bm_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    bm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, out_pclka_hz) != k_ra8_ok)) {
    bm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    bm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (bm_pins_init() != k_ra8_ok) {
    bm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_bm_uart_baud) != k_ra8_ok) {
    bm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief Read SOC + the CRATE-sign from the fuel gauge; halt on a bus NAK.
 *
 * @param[out] out_soc 1-byte SOC high byte = integer percent (clamped to 100).
 * @param[out] out_chg 1 when the CRATE high byte's sign bit is clear (charging).
 *
 * @note Charge decode is a single condition: CRATE high-byte bit 7 clear means a
 * positive (charging) rate. Not a compound boolean decision, so no MC/DC vector
 * set applies; the path is exercised by the ra8_emulator gate, not a host test.
 *
 * @pre ::ra8_smbus_init succeeded.
 * @post ``*out_soc`` / ``*out_chg`` reflect the fuel gauge; halts on NAK.
 * @since 0.1.0
 */
static void bm_read_or_halt(uint8_t* out_soc, uint8_t* out_chg)
{
  uint8_t soc = 0U;
  if (ra8_smbus_read_byte_data((uint8_t)k_bm_fg_addr, (uint8_t)k_bm_reg_soc, &soc) != k_ra8_ok) {
    bm_panic_halt(k_msg_nak, (uint32_t)sizeof(k_msg_nak) - 1U);
  }
  uint8_t crate_hi = 0U;
  if (ra8_smbus_read_byte_data((uint8_t)k_bm_fg_addr, (uint8_t)k_bm_reg_crate, &crate_hi) !=
      k_ra8_ok) {
    bm_panic_halt(k_msg_nak, (uint32_t)sizeof(k_msg_nak) - 1U);
  }
  *out_soc = (soc > (uint8_t)k_bm_soc_max) ? (uint8_t)k_bm_soc_max : soc;
  *out_chg = ((crate_hi & (uint8_t)k_bm_sign_mask) == 0U) ? 1U : 0U;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  bm_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  bm_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  /* App-owned bus bring-up: IIC_B in I2C-compat mode, bound through the
   * ra8_io facade into the SMBus layer's injected seam. A future board
   * revision that moves the bus onto a RIIC channel only swaps the bind. */
  const ra8_i3c_cfg_t iic_cfg = {.mode     = k_ra8_i3c_mode_i2c,
                                 .bus_hz   = (uint32_t)k_bm_bus_hz,
                                 .pclka_hz = pclka_hz};
  ra8_i2c_bus_ops_t   bus_ops = {};
  if (ra8_i3c_init((uint8_t)k_bm_iic_chan, &iic_cfg) != k_ra8_ok) {
    bm_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }
  if (ra8_io_i2c_bus_bind_i3c_compat(&s_bm_bus, (uint8_t)k_bm_iic_chan) != k_ra8_ok) {
    bm_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }
  if (ra8_io_i2c_bus_as_ops(&s_bm_bus, &bus_ops) != k_ra8_ok) {
    bm_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }

  const ra8_smbus_cfg_t cfg = {.bus = bus_ops, .pec_enabled = false};
  if (ra8_smbus_init(&cfg) != k_ra8_ok) {
    bm_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }

  ra8_batt_monitor_t nag_mon;
  if (ra8_batt_monitor_init(&nag_mon) != k_ra8_ok) {
    bm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  while (1) {
    uint8_t soc = 0U;
    uint8_t chg = 0U;
    bm_read_or_halt(&soc, &chg);
    g_bat_soc = (uint32_t)soc;
    g_bat_chg = (uint32_t)chg;

    bm_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
    bm_print_uint((uint32_t)soc);
    bm_print(k_msg_pct, (uint32_t)sizeof(k_msg_pct) - 1U);
    bm_print((chg != 0U) ? (const uint8_t*)"Y" : (const uint8_t*)"N", 1U);
    bm_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

    /* Fold the reading into the nag policy: a one-shot LOW / CRITICAL line
     * prints on the descent into each band (edge-triggered, see ra8_batt). */
    ra8_batt_nag_t nag = k_ra8_batt_nag_none;
    if (ra8_batt_update(&nag_mon, soc, (chg != 0U), &nag) == k_ra8_ok) {
      g_bat_nag = (uint32_t)nag;
      bm_print_nag(nag, soc);
    }

    ++g_bat_heartbeat;
    ra8_delay_ms((uint32_t)k_bm_period_ms);
  }
  bm_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  return 0;
}
#pragma GCC diagnostic pop
