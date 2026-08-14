/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/src/cam_ov5640.c
 * @brief OV5640 SCCB driver: reset strap, chip-ID probe, DVP scene config.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements the sensor half of the CEU capture self-test declared in
 * `cam_ov5640.h`. Holds the proven OV5640 VGA DVP scene register table, the
 * SCCB (I2C) read/write helpers, the runtime-discovered 7-bit address, and the
 * reset / probe / configure lifecycle the app drives. Every OV5640 register
 * value here is device data from the OmniVision datasheet, not RA8D2 MMIO, so
 * no Hardware User's Manual citation applies to the table rows.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "cam_ov5640.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/* =============================================================================
 * OV5640 constants (typed enums -- no magic numbers)
 * =============================================================================
 */

/** @brief OV5640 SCCB register addresses + the expected chip-ID value. */
typedef enum : uint16_t {
  k_cam_reg_chip_id_hi = 0x300AU, /**< OV5640 chip-ID high byte register.    */
  k_cam_reg_chip_id_lo = 0x300BU, /**< OV5640 chip-ID low byte register.     */
  k_cam_reg_sw_reset   = 0x3008U, /**< OV5640 system control register.       */
  k_cam_reg_format     = 0x4300U, /**< DVP output pixel format register.     */
  k_cam_reg_isp_mux    = 0x501FU, /**< ISP output format-mux register.       */
  k_cam_reg_test       = 0x503DU, /**< ISP pre-output test-pattern register. */
  k_cam_chip_id_ov5640 = 0x5640U, /**< Expected OV5640 chip-ID value.        */
} cam_ov5640_reg_t;

/** @brief OV5640 SCCB addressing, reset values, and byte-wrangling constants. */
typedef enum : uint8_t {
  k_cam_sccb_addr_7b   = 0x3CU, /**< OV5640 SCCB 7-bit address (SID low).  */
  k_cam_sccb_addr_alt  = 0x3DU, /**< OV5640 SCCB address when SID is high. */
  k_cam_sw_reset_hold  = 0x82U, /**< 0x3008 = software reset (bit7).       */
  k_cam_sw_reset_wake  = 0x02U, /**< 0x3008 = normal operation.            */
  k_cam_reg_addr_bytes = 2U,    /**< OV5640 uses 16-bit register pointers. */
  k_cam_hi_byte_shift  = 8U,    /**< Shift for the high register byte.     */
  k_cam_byte_mask      = 0xFFU, /**< Low-byte mask.                        */
  k_cam_format_yuyv    = 0x30U, /**< YUYV DVP output.                      */
  k_cam_isp_mux_yuv    = 0x00U, /**< Route YUV through the ISP formatter.  */
} cam_ov5640_sccb_t;

/** @brief OV5640 settle delays around reset and register programming (ms). */
typedef enum : uint32_t {
  k_cam_reset_low_ms  = 20U,  /**< Sensor RST held low.                */
  k_cam_reset_high_ms = 20U,  /**< Settle after RST release.           */
  k_cam_swreset_ms    = 10U,  /**< Settle after SCCB software reset.   */
  k_cam_cfg_settle_ms = 500U, /**< Settle after the register sequence. */
} cam_ov5640_delay_t;

/**
 * @struct cam_reg_t
 * @brief One OV5640 SCCB register write (16-bit address, 8-bit value).
 * @details The value plane is device data (OV5640 datasheet), not RA8D2
 *          MMIO, so no HUM citation applies; the table rows are pure data.
 */
typedef struct {
  uint16_t reg; /**< OV5640 register address. */
  uint8_t  val; /**< Value to program.        */
} cam_reg_t;

/**
 * @var s_ov5640_dvp_scene
 * @brief OV5640 DVP init: PLL from 24 MHz XVCLK, VGA YUV422 live output.
 * @details Contains the DVP clock, VGA timing, ISP, auto-exposure,
 *          and colour-processing controls required for a live lens image.
 * @note Each row is device data; see @ref cam_reg_t.
 * @since 0.1.0
 */
static const cam_reg_t s_ov5640_dvp_scene[] = {
  /* ---- clock / PLL: sysclk from PLL fed by the 24 MHz XVCLK ---- */
  {0x3103, 0x11},
  {0x4740, 0x20},
  {0x4050, 0x6E},
  {0x4051, 0x8F},
  {0x3103, 0x02},
  {0x3017, 0x7F},
  {0x3018, 0xFF},
  {0x302C, 0xC2},
  {0x3034, 0x18},
  {0x3035, 0x21},
  {0x3036, 0x46},
  {0x3037, 0x13},
  {0x3108, 0x01},
  {0x3630, 0x2E},
  {0x3631, 0x0E},
  {0x3632, 0xE2},
  {0x3633, 0x23},
  {0x3621, 0xE0},
  {0x3704, 0xA0},
  {0x3703, 0x5A},
  {0x3715, 0x78},
  {0x3717, 0x01},
  {0x370B, 0x60},
  {0x3705, 0x1A},
  {0x3905, 0x02},
  {0x3906, 0x10},
  {0x3901, 0x0A},
  {0x3731, 0x12},
  {0x3600, 0x08},
  {0x3601, 0x33},
  {0x302D, 0x60},
  {0x3620, 0x52},
  {0x371B, 0x20},
  {0x471C, 0x50},
  {0x3A13, 0x43},
  {0x3A18, 0x00},
  {0x3A19, 0xF8},
  {0x3635, 0x1C},
  {0x3636, 0x03},
  {0x3634, 0x40},
  {0x3622, 0x01},
  /* ---- live-scene exposure, anti-flicker, and black-level controls ---- */
  {0x3C01, 0xB4},
  {0x3C04, 0x28},
  {0x3C05, 0x98},
  {0x3C06, 0x00},
  {0x3C07, 0x08},
  {0x3C08, 0x00},
  {0x3C09, 0x1C},
  {0x3C0A, 0x9C},
  {0x3C0B, 0x40},
  {0x3618, 0x00},
  {0x3612, 0x29},
  {0x3708, 0x64},
  {0x3709, 0x52},
  {0x370C, 0x03},
  {0x3A00, 0x3C},
  {0x3A02, 0x05},
  {0x3A03, 0xC4},
  {0x3A08, 0x00},
  {0x3A09, 0x93},
  {0x3A0A, 0x00},
  {0x3A0B, 0x7B},
  {0x3A0D, 0x08},
  {0x3A0E, 0x06},
  {0x3A0F, 0x30},
  {0x3A10, 0x28},
  {0x3A11, 0x60},
  {0x3A14, 0x05},
  {0x3A15, 0xC4},
  {0x3A1B, 0x30},
  {0x3A1E, 0x26},
  {0x3A1F, 0x14},
  {0x3503, 0x00},
  {0x3C00, 0x04},
  {0x4001, 0x02},
  {0x4004, 0x02},
  /* ---- timing: full array windowed to a VGA DVP output ---- */
  {0x3808, 0x02},
  {0x3809, 0x80},
  {0x380A, 0x01},
  {0x380B, 0xE0},
  {0x380C, 0x0C},
  {0x380D, 0x80},
  {0x380E, 0x07},
  {0x380F, 0xD0},
  {0x3800, 0x00},
  {0x3801, 0x00},
  {0x3802, 0x00},
  {0x3803, 0x04},
  {0x3804, 0x0A},
  {0x3805, 0x3F},
  {0x3806, 0x07},
  {0x3807, 0x9B},
  {0x3810, 0x00},
  {0x3811, 0x10},
  {0x3812, 0x00},
  {0x3813, 0x06},
  {0x3814, 0x31},
  {0x3815, 0x31},
  {0x3820, 0x41},
  {0x3821, 0x01},
  /* ---- ISP + DVP output format: YUV422 (YUYV) ---- */
  {0x4300, 0x30},
  {0x501F, 0x00},
  {0x4713, 0x03},
  {0x4407, 0x04},
  {0x460B, 0x35},
  {0x460C, 0x22},
  {0x4837, 0x0A},
  {0x3824, 0x01},
  {0x5000, 0xA7},
  {0x5001, 0xA3},
  /* ---- ISP scene processing: AWB + colour matrix ---- */
  {0x5180, 0xFF},
  {0x5181, 0xF2},
  {0x5182, 0x00},
  {0x5183, 0x14},
  {0x5184, 0x25},
  {0x5185, 0x24},
  {0x5186, 0x09},
  {0x5187, 0x09},
  {0x5188, 0x09},
  {0x5189, 0x88},
  {0x518A, 0x54},
  {0x518B, 0xEE},
  {0x518C, 0xB2},
  {0x518D, 0x50},
  {0x518E, 0x34},
  {0x518F, 0x6B},
  {0x5190, 0x46},
  {0x5191, 0xF8},
  {0x5192, 0x04},
  {0x5193, 0x70},
  {0x5194, 0xF0},
  {0x5195, 0xF0},
  {0x5196, 0x03},
  {0x5197, 0x01},
  {0x5198, 0x04},
  {0x5199, 0x6C},
  {0x519A, 0x04},
  {0x519B, 0x00},
  {0x519C, 0x09},
  {0x519D, 0x2B},
  {0x519E, 0x38},
  {0x5381, 0x1E},
  {0x5382, 0x5B},
  {0x5383, 0x08},
  {0x5384, 0x0A},
  {0x5385, 0x7E},
  {0x5386, 0x88},
  {0x5387, 0x7C},
  {0x5388, 0x6C},
  {0x5389, 0x10},
  {0x538A, 0x01},
  {0x538B, 0x98},
  /* ---- ISP sharpening, gamma, saturation, and lens shading ---- */
  {0x5300, 0x08},
  {0x5301, 0x30},
  {0x5302, 0x10},
  {0x5303, 0x00},
  {0x5304, 0x08},
  {0x5305, 0x30},
  {0x5306, 0x08},
  {0x5307, 0x16},
  {0x5309, 0x08},
  {0x530A, 0x30},
  {0x530B, 0x04},
  {0x530C, 0x06},
  {0x5480, 0x01},
  {0x5481, 0x08},
  {0x5482, 0x14},
  {0x5483, 0x28},
  {0x5484, 0x51},
  {0x5485, 0x65},
  {0x5486, 0x71},
  {0x5487, 0x7D},
  {0x5488, 0x87},
  {0x5489, 0x91},
  {0x548A, 0x9A},
  {0x548B, 0xAA},
  {0x548C, 0xB8},
  {0x548D, 0xCD},
  {0x548E, 0xDD},
  {0x548F, 0xEA},
  {0x5490, 0x1D},
  {0x5580, 0x02},
  {0x5583, 0x40},
  {0x5584, 0x10},
  {0x5589, 0x10},
  {0x558A, 0x00},
  {0x558B, 0xF8},
  {0x5800, 0x23},
  {0x5801, 0x14},
  {0x5802, 0x0F},
  {0x5803, 0x0F},
  {0x5804, 0x12},
  {0x5805, 0x26},
  {0x5806, 0x0C},
  {0x5807, 0x08},
  {0x5808, 0x05},
  {0x5809, 0x05},
  {0x580A, 0x08},
  {0x580B, 0x0D},
  {0x580C, 0x08},
  {0x580D, 0x03},
  {0x580E, 0x00},
  {0x580F, 0x00},
  {0x5810, 0x03},
  {0x5811, 0x09},
  {0x5812, 0x07},
  {0x5813, 0x03},
  {0x5814, 0x00},
  {0x5815, 0x01},
  {0x5816, 0x03},
  {0x5817, 0x08},
  {0x5818, 0x0D},
  {0x5819, 0x08},
  {0x581A, 0x05},
  {0x581B, 0x06},
  {0x581C, 0x08},
  {0x581D, 0x0E},
  {0x581E, 0x29},
  {0x581F, 0x17},
  {0x5820, 0x11},
  {0x5821, 0x11},
  {0x5822, 0x15},
  {0x5823, 0x28},
  {0x5824, 0x46},
  {0x5825, 0x26},
  {0x5826, 0x08},
  {0x5827, 0x26},
  {0x5828, 0x64},
  {0x5829, 0x26},
  {0x582A, 0x24},
  {0x582B, 0x22},
  {0x582C, 0x24},
  {0x582D, 0x24},
  {0x582E, 0x06},
  {0x582F, 0x22},
  {0x5830, 0x40},
  {0x5831, 0x42},
  {0x5832, 0x24},
  {0x5833, 0x26},
  {0x5834, 0x24},
  {0x5835, 0x22},
  {0x5836, 0x22},
  {0x5837, 0x26},
  {0x5838, 0x44},
  {0x5839, 0x24},
  {0x583A, 0x26},
  {0x583B, 0x28},
  {0x583C, 0x42},
  {0x583D, 0xCE},
  {0x5025, 0x00},
  /* ---- DVP pads, interface control, and sync polarity ---- */
  {0x3000, 0x00},
  {0x3002, 0x1C},
  {0x3004, 0xFF},
  {0x3006, 0xC3},
  {0x300E, 0x58},
  {0x302E, 0x00},
  {0x440E, 0x00},
  {0x4709, 0x02},
  {0x470A, 0x00},
  {0x470B, 0x00},
  {0x471D, 0x00},
  {0x4713, 0x03},
  {0x471C, 0x50},
  {0x4740, 0x20},
  {0x4005, 0x1A},
  {0x3406, 0x00},
  {0x3503, 0x00},
  {0x3008, 0x02},
  {0x4745, 0x00},
  {0x4301, 0x01},
  /* ---- ISP test pattern disabled: stream the lens image ---- */
  {0x503D, 0x00},
};

/**
 * @var s_sccb_addr
 * @brief Active OV5640 SCCB 7-bit address, discovered at runtime.
 * @details Defaults to 0x3C (SID low); ::cam_probe_sensor flips it to
 *          0x3D if the chip ID only answers there.
 * @warning Set once during probe; not thread-safe.
 * @since 0.1.0
 */
static uint8_t s_sccb_addr = (uint8_t)k_cam_sccb_addr_7b;

/* =============================================================================
 * SCCB (I2C) helpers
 * =============================================================================
 */

/**
 * @brief Write one OV5640 register (16-bit address, 8-bit value) over SCCB.
 *
 * @param[in] reg OV5640 register address.
 * @param[in] val Value to program.
 * @return ra8_err_t from the RIIC write.
 * @retval k_ra8_ok Register written and ACKed.
 * @retval k_ra8_err_nack Sensor NACKed.
 *
 * @pre RIIC channel 1 is initialized.
 * @pre XVCLK is running (the OV5640 core clocks SCCB from it).
 * @post The addressed register holds `val` (on ACK).
 * @post The bus is released (STOP issued).
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t cam_sccb_write(uint16_t reg, uint8_t val)
{
  if (reg == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint8_t payload[3] = {
    (uint8_t)((reg >> (uint16_t)k_cam_hi_byte_shift) & (uint16_t)k_cam_byte_mask),
    (uint8_t)(reg & (uint16_t)k_cam_byte_mask),
    val,
  };
  return ra8_i2c_write((uint8_t)k_cam_iic_ch,
                       s_sccb_addr,
                       payload,
                       (uint32_t)sizeof(payload),
                       true);
}

/**
 * @brief Read one OV5640 register (16-bit address, 8-bit value) over SCCB.
 *
 * @param[in]  reg     OV5640 register address.
 * @param[out] out_val Receives the register value.
 * @return ra8_err_t from the RIIC combined transfer.
 * @retval k_ra8_ok Register read.
 * @retval k_ra8_err_null_ptr `out_val` was NULL.
 * @retval k_ra8_err_nack Sensor NACKed.
 *
 * @pre RIIC channel 1 is initialized and XVCLK is running.
 * @pre `out_val` is non-NULL.
 * @post `*out_val` holds the register value on success.
 * @post The bus is released.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t cam_sccb_read(uint16_t reg, uint8_t* out_val)
{
  RA8_CHECK_NULL_PTR(out_val, "cam", "sccb_read");
  if (reg == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint8_t addr[2] = {
    (uint8_t)((reg >> (uint16_t)k_cam_hi_byte_shift) & (uint16_t)k_cam_byte_mask),
    (uint8_t)(reg & (uint16_t)k_cam_byte_mask),
  };
  return ra8_i2c_transfer((uint8_t)k_cam_iic_ch,
                          s_sccb_addr,
                          addr,
                          (uint32_t)k_cam_reg_addr_bytes,
                          out_val,
                          1U);
}

/**
 * @brief Read the OV5640 16-bit chip ID (0x300A:0x300B).
 *
 * @param[out] out_id Receives the composed chip ID.
 * @return ra8_err_t; ok only when both bytes read.
 * @retval k_ra8_ok Chip ID composed into `*out_id`.
 * @retval k_ra8_err_null_ptr `out_id` was NULL.
 *
 * @pre RIIC ch1 up, XVCLK running.
 * @pre `out_id` is non-NULL.
 * @post `*out_id` holds the 16-bit ID on success.
 * @post The bus is released.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t cam_read_chip_id(uint16_t* out_id)
{
  RA8_CHECK_NULL_PTR(out_id, "cam", "chip_id");
  uint8_t   hi  = 0U;
  uint8_t   lo  = 0U;
  ra8_err_t err = cam_sccb_read((uint16_t)k_cam_reg_chip_id_hi, &hi);
  if (err != k_ra8_ok) {
    return err;
  }
  err = cam_sccb_read((uint16_t)k_cam_reg_chip_id_lo, &lo);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_id = (uint16_t)(((uint16_t)hi << (uint16_t)k_cam_hi_byte_shift) | (uint16_t)lo);
  return k_ra8_ok;
}

/* =============================================================================
 * Sensor lifecycle (public -- contracts in cam_ov5640.h)
 * =============================================================================
 */

ra8_err_t cam_reset_sensor(void)
{
  const ra8_port_pin_t rst = RA8_PIN(k_ra8_port_7, k_ra8_pin_9);
  ra8_err_t            err = ra8_gpio_output_init(rst, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_cam_reset_low_ms);
  err = ra8_gpio_write(rst, k_ra8_level_high);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_cam_reset_high_ms);
  return k_ra8_ok;
}

ra8_err_t cam_configure_sensor(void)
{
  ra8_err_t err = cam_sccb_write((uint16_t)k_cam_reg_sw_reset, (uint8_t)k_cam_sw_reset_hold);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_cam_swreset_ms);
  const uint32_t count = (uint32_t)(sizeof(s_ov5640_dvp_scene) / sizeof(cam_reg_t));
  for (uint32_t i = 0U; i < count; i += 1U) {
    err = cam_sccb_write(s_ov5640_dvp_scene[i].reg, s_ov5640_dvp_scene[i].val);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  err = cam_sccb_write((uint16_t)k_cam_reg_sw_reset, (uint8_t)k_cam_sw_reset_wake);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_cam_cfg_settle_ms);
  uint8_t format  = 0U;
  uint8_t isp_mux = 0U;
  uint8_t test    = 0U;
  err             = cam_sccb_read((uint16_t)k_cam_reg_format, &format);
  if (err != k_ra8_ok) {
    return err;
  }
  err = cam_sccb_read((uint16_t)k_cam_reg_isp_mux, &isp_mux);
  if (err != k_ra8_ok) {
    return err;
  }
  err = cam_sccb_read((uint16_t)k_cam_reg_test, &test);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((format != (uint8_t)k_cam_format_yuyv) || (isp_mux != (uint8_t)k_cam_isp_mux_yuv) ||
      (test != 0U)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

bool cam_probe_sensor(uint16_t* out_id)
{
  if (out_id == nullptr) {
    return false;
  }
  *out_id                = 0U;
  const uint8_t addrs[2] = {(uint8_t)k_cam_sccb_addr_7b, (uint8_t)k_cam_sccb_addr_alt};
  for (uint32_t i = 0U; i < (uint32_t)sizeof(addrs); i += 1U) {
    s_sccb_addr    = addrs[i];
    uint16_t  id   = 0U;
    ra8_err_t rerr = cam_read_chip_id(&id);
    if (rerr == k_ra8_ok) {
      *out_id = id;
      if (id == (uint16_t)k_cam_chip_id_ov5640) {
        return true;
      }
    }
  }
  s_sccb_addr = (uint8_t)k_cam_sccb_addr_7b;
  return false;
}
