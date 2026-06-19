/**
 * @file examples/ek_ra8d2/hw_pending/smbus_demo/main.c
 * @brief SMBus 3.2 protocol-layer demo + HIL over IIC_B (#128).
 *
 * @details
 * `ra_smbus` is the SMBus 3.2 protocol layer on top of the IIC_B (I3C-in-
 * I2C-mode) controller -- it frames Send Byte / Receive Byte / Read Byte
 * Data / ... transactions and delegates the raw byte movement to `ra_i3c`.
 * It had no example and no CI gate. This app is that gate: it drives real
 * SMBus byte transactions against an I2C register-file device and checks the
 * decoded bytes.
 *
 * The target is the ST LSM6DSO 6-DoF IMU (MikroBUS / IMU 12 Click on the
 * Pmod1 I2C side, 7-bit address 0x6B) -- a plain register-indexed I2C device,
 * so it answers the SMBus byte protocols natively (SMBus is electrically
 * I2C). Two independent reads of the WHO_AM_I register (0x0F, fixed 0x6C)
 * exercise three protocol shapes:
 *   - **Read Byte Data**:   write [0x0F], RESTART, read [0x6C].
 *   - **Send Byte** + **Receive Byte**: send [0x0F] (sets the device register
 *     pointer), then a separate read returns [0x6C].
 * PEC is left disabled (the IMU is not an SMBus PEC device), so the frames
 * are plain I2C. The console banner is:
 *
 *   `smbus: whoami=6C sendrecv=6C PASS`
 *
 * Under `board_sim` the modelled LSM6DSO answers on the modelled IIC_B bus,
 * so the banner is deterministic; on the bench it needs the IMU 12 Click
 * fitted (same prerequisite as `imu_lsm6dso_demo`).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / App] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_i3c.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_smbus.h"
#include "ra_time.h"

/** @enum sd_consts_t @brief Console / bus / device knobs (no magic numbers). */
typedef enum : uint32_t {
  k_sd_uart_chan  = 8U,      /**< SCI8 J-Link OB console.             */
  k_sd_uart_baud  = 115200U, /**< Console baud.                       */
  k_sd_bus_hz     = 100000U, /**< IIC_B standard-mode bit rate.       */
  k_sd_iic_chan   = (uint32_t)k_ra_board_mikrobus_iic_b_channel, /**< 0.  */
  k_sd_lsm_addr   = 0x6BU, /**< LSM6DSO 7-bit address (SA0 high).   */
  k_sd_reg_whoami = 0x0FU, /**< WHO_AM_I register index.            */
  k_sd_whoami_val = 0x6CU, /**< Expected WHO_AM_I value.            */
  k_sd_hex_shift  = 4U,    /**< Nibble shift for hex printing.      */
  k_sd_hex_mask   = 0x0FU, /**< Low-nibble mask.                    */
  k_sd_dec_ten    = 10U,   /**< Hex digit / decimal split.          */
} sd_consts_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_sd_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_sd_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);
/** @brief MikroBUS SCL routed through the Pmod1 I2C side (SCL1). */
static const ra_port_pin_t k_sd_pin_scl = (ra_port_pin_t)k_ra_board_mikrobus_i2c_scl;
/** @brief MikroBUS SDA routed through the Pmod1 I2C side (SDA1). */
static const ra_port_pin_t k_sd_pin_sda = (ra_port_pin_t)k_ra_board_mikrobus_i2c_sda;

static const uint8_t k_msg_boot[] = "smbus-demo: boot\r\n";
static const uint8_t k_msg_fail[] = "smbus-demo: FAIL init\r\n";
static const uint8_t k_msg_open[] = "smbus: FAIL open\r\n";
static const uint8_t k_msg_nak[]  = "smbus: NAK (no LSM6DSO)\r\n";
static const uint8_t k_msg_bad[]  = "smbus: mismatch\r\n";
static const uint8_t k_msg_pre[]  = "smbus: whoami=";
static const uint8_t k_msg_mid[]  = " sendrecv=";
static const uint8_t k_msg_ok[]   = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void sd_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_sd_uart_chan, msg, len);
}

/** @brief Print the fail banner and trap (board_sim halts on the BKPT). */
static void sd_panic_halt(const uint8_t* msg, uint32_t len)
{
  sd_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print one byte as two upper-case hex digits. */
static void sd_print_hex8(uint8_t value)
{
  uint8_t        buf[2];
  const uint32_t hi = ((uint32_t)value >> (uint32_t)k_sd_hex_shift) & (uint32_t)k_sd_hex_mask;
  const uint32_t lo = (uint32_t)value & (uint32_t)k_sd_hex_mask;
  buf[0] = (uint8_t)((hi < (uint32_t)k_sd_dec_ten) ? ('0' + hi) : ('A' + (hi - k_sd_dec_ten)));
  buf[1] = (uint8_t)((lo < (uint32_t)k_sd_dec_ten) ? ('0' + lo) : ('A' + (lo - k_sd_dec_ten)));
  sd_print(buf, 2U);
}

/** @brief Route SCI8 console pins + the MikroBUS IIC SCL/SDA via PFS. */
[[nodiscard]] static ra_err_t sd_pins_init(void)
{
  if (ra_pfs_route_peripheral(k_sd_pin_txd, k_ra_psel_sci_async, "smbus.txd8") != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }
  if (ra_pfs_route_peripheral(k_sd_pin_rxd, k_ra_psel_sci_async, "smbus.rxd8") != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }
  if (ra_pfs_route_peripheral(k_sd_pin_scl, k_ra_psel_iic, "smbus.scl1") != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }
  return ra_pfs_route_peripheral(k_sd_pin_sda, k_ra_psel_iic, "smbus.sda1");
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void sd_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra_cgc_init() != k_ra_ok) || (ra_mstp_init() != k_ra_ok)) {
    sd_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, out_pclka_hz) != k_ra_ok)) {
    sd_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    sd_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (sd_pins_init() != k_ra_ok) {
    sd_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  const ra_sci_cfg_t cfg = {.baud      = (uint32_t)k_sd_uart_baud,
                            .data_bits = k_ra_sci_data_8,
                            .parity    = k_ra_sci_parity_none,
                            .stop_bits = k_ra_sci_stop_1,
                            .pclk_hz   = *out_pclka_hz};
  if (ra_sci_init((uint8_t)k_sd_uart_chan, &cfg) != k_ra_ok) {
    sd_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief Read WHO_AM_I two ways over SMBus; halt on any failure.
 *
 * @details
 * (1) Read Byte Data(0x0F) -> a, (2) Send Byte(0x0F) then Receive Byte -> b.
 * Both must equal 0x6C. A NAK (no IMU fitted) prints the NAK banner; a wrong
 * value prints the mismatch banner; either halts on a BKPT before the PASS
 * line so the gate is exact.
 *
 * @param[out] out_a Receives the Read-Byte-Data WHO_AM_I.
 * @param[out] out_b Receives the Send-Byte + Receive-Byte WHO_AM_I.
 */
static void sd_read_whoami_or_halt(uint8_t* out_a, uint8_t* out_b)
{
  uint8_t a = 0U;
  if (ra_smbus_read_byte_data((uint8_t)k_sd_lsm_addr, (uint8_t)k_sd_reg_whoami, &a) != k_ra_ok) {
    sd_panic_halt(k_msg_nak, (uint32_t)sizeof(k_msg_nak) - 1U);
  }
  uint8_t b = 0U;
  if (ra_smbus_send_byte((uint8_t)k_sd_lsm_addr, (uint8_t)k_sd_reg_whoami) != k_ra_ok) {
    sd_panic_halt(k_msg_nak, (uint32_t)sizeof(k_msg_nak) - 1U);
  }
  if (ra_smbus_receive_byte((uint8_t)k_sd_lsm_addr, &b) != k_ra_ok) {
    sd_panic_halt(k_msg_nak, (uint32_t)sizeof(k_msg_nak) - 1U);
  }
  if ((a != (uint8_t)k_sd_whoami_val) || (b != (uint8_t)k_sd_whoami_val)) {
    sd_panic_halt(k_msg_bad, (uint32_t)sizeof(k_msg_bad) - 1U);
  }
  *out_a = a;
  *out_b = b;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up SMBus, read WHO_AM_I two ways, print the banner.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The whoami PASS banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  sd_setup_or_halt(&pclka_hz);
  ra_isr_globals_enable();
  sd_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  const ra_smbus_cfg_t cfg = {
    .channel = (uint8_t)k_sd_iic_chan,
    .iic_cfg = {.mode = k_ra_i3c_mode_i2c, .bus_hz = (uint32_t)k_sd_bus_hz, .pclka_hz = pclka_hz},
    .pec_enabled = false};
  if (ra_smbus_init(&cfg) != k_ra_ok) {
    sd_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }

  uint8_t a = 0U;
  uint8_t b = 0U;
  sd_read_whoami_or_halt(&a, &b);

  sd_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  sd_print_hex8(a);
  sd_print(k_msg_mid, (uint32_t)sizeof(k_msg_mid) - 1U);
  sd_print_hex8(b);
  sd_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
