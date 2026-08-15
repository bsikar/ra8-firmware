/**
 * @file ra8_ov5640.c
 * @brief Transport-independent OV5640 SCCB register driver.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * Holds validated OV5640 VGA DVP register settings and the sensor lifecycle.
 * The caller supplies all SCCB and timing operations, so this module owns no
 * RA8 peripheral, board pin, or global transport state. Every register value
 * here is OV5640 device data rather than RA8D2 MMIO.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_ov5640.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/* =============================================================================
 * OV5640 constants (typed enums -- no magic numbers)
 * =============================================================================
 */

/** @brief OV5640 SCCB register addresses + the expected chip-ID value. */
typedef enum : uint16_t {
  k_ov5640_reg_system_reset00     = 0x3000U, /**< MCU reset controls.               */
  k_ov5640_reg_system_reset02     = 0x3002U, /**< JPEG reset controls.              */
  k_ov5640_reg_clock_enable02     = 0x3006U, /**< JPEG clock controls.              */
  k_ov5640_reg_sw_reset           = 0x3008U, /**< System control register.          */
  k_ov5640_reg_chip_id_hi         = 0x300AU, /**< Chip-ID high byte register.       */
  k_ov5640_reg_chip_id_lo         = 0x300BU, /**< Chip-ID low byte register.        */
  k_ov5640_reg_pll_bit_mode       = 0x3034U, /**< PLL bit-mode control.             */
  k_ov5640_reg_pll_sys_div        = 0x3035U, /**< PLL system-clock divider.         */
  k_ov5640_reg_pll_multiplier     = 0x3036U, /**< PLL multiplier.                   */
  k_ov5640_reg_pll_pre_div        = 0x3037U, /**< PLL pre-divider.                  */
  k_ov5640_reg_pll_bypass         = 0x3039U, /**< PLL bypass control.               */
  k_ov5640_reg_clock_select       = 0x3103U, /**< System clock source.              */
  k_ov5640_reg_clock_root         = 0x3108U, /**< PCLK/SCLK root dividers.          */
  k_ov5640_reg_timing_y_start_hi  = 0x3802U, /**< Sensor crop Y start high byte.    */
  k_ov5640_reg_timing_y_start_lo  = 0x3803U, /**< Sensor crop Y start low byte.     */
  k_ov5640_reg_timing_y_end_hi    = 0x3806U, /**< Sensor crop Y end high byte.      */
  k_ov5640_reg_timing_y_end_lo    = 0x3807U, /**< Sensor crop Y end low byte.       */
  k_ov5640_reg_timing_hts_hi      = 0x380CU, /**< Horizontal total high byte.       */
  k_ov5640_reg_timing_hts_lo      = 0x380DU, /**< Horizontal total low byte.        */
  k_ov5640_reg_timing_vts_hi      = 0x380EU, /**< Vertical total high byte.         */
  k_ov5640_reg_timing_vts_lo      = 0x380FU, /**< Vertical total low byte.          */
  k_ov5640_reg_timing_y_offset_hi = 0x3812U, /**< Output Y offset high byte.        */
  k_ov5640_reg_timing_y_offset_lo = 0x3813U, /**< Output Y offset low byte.         */
  k_ov5640_reg_timing_tc_reg20    = 0x3820U, /**< Vertical timing and binning.      */
  k_ov5640_reg_timing_tc_reg21    = 0x3821U, /**< Timing/mirror and JPEG enable.    */
  k_ov5640_reg_pclk_divider       = 0x3824U, /**< DVP pixel-clock divider.          */
  k_ov5640_reg_format             = 0x4300U, /**< DVP output pixel format register. */
  k_ov5640_reg_jpeg_ctrl00        = 0x4400U, /**< JPEG input-format control.        */
  k_ov5640_reg_jpeg_ctrl01        = 0x4401U, /**< JPEG FIFO pacing control.         */
  k_ov5640_reg_jpeg_ctrl04        = 0x4404U, /**< JPEG header-output control.       */
  k_ov5640_reg_jpeg_quality       = 0x4407U, /**< JPEG quantization scale.          */
  k_ov5640_reg_jpeg_length_hi     = 0x4414U, /**< JPEG length bits 23:16.           */
  k_ov5640_reg_jpeg_length_mid    = 0x4415U, /**< JPEG length bits 15:8.            */
  k_ov5640_reg_jpeg_length_lo     = 0x4416U, /**< JPEG length bits 7:0.             */
  k_ov5640_reg_jfifo_overflow     = 0x4417U, /**< JPEG FIFO overflow status.        */
  k_ov5640_reg_jpeg_timing14      = 0x4514U, /**< JPEG binning/orientation timing.  */
  k_ov5640_reg_jpeg_timing20      = 0x4520U, /**< JPEG sampling timing.             */
  k_ov5640_reg_vfifo_ctrl00       = 0x4600U, /**< VFIFO output control.             */
  k_ov5640_reg_compression_w_hi   = 0x4602U, /**< Compressed-output width high.     */
  k_ov5640_reg_compression_w_lo   = 0x4603U, /**< Compressed-output width low.      */
  k_ov5640_reg_compression_h_hi   = 0x4604U, /**< Compressed-output height high.    */
  k_ov5640_reg_compression_h_lo   = 0x4605U, /**< Compressed-output height low.     */
  k_ov5640_reg_vfifo_ctrl0b       = 0x460BU, /**< VFIFO output control.             */
  k_ov5640_reg_vfifo_ctrl0c       = 0x460CU, /**< VFIFO output control.             */
  k_ov5640_reg_jpeg_mode          = 0x4713U, /**< DVP JPEG mode select.             */
  k_ov5640_reg_jpeg_ctrl1c        = 0x471CU, /**< JPEG DVP control.                 */
  k_ov5640_reg_href_minimum       = 0x471FU, /**< JPEG HREF minimum blanking.       */
  k_ov5640_reg_polarity_ctrl00    = 0x4740U, /**< DVP clock and sync polarity.      */
  k_ov5640_reg_isp_ctrl01         = 0x5001U, /**< ISP feature-enable register.      */
  k_ov5640_reg_isp_mux            = 0x501FU, /**< ISP output format-mux register.   */
  k_ov5640_reg_test               = 0x503DU, /**< Pre-output test-pattern register. */
} ra8_ov5640_register_t;

/** @brief OV5640 SCCB addressing, reset values, and byte-wrangling constants. */
typedef enum : uint8_t {
  k_ov5640_sw_reset_hold           = 0x82U, /**< Software reset (0x3008 bit 7).  */
  k_ov5640_sw_standby              = 0x42U, /**< Software standby.               */
  k_ov5640_sw_reset_wake           = 0x02U, /**< Normal operation.               */
  k_ov5640_mcu_reset_hold          = 0x20U, /**< Hold the internal MCU reset.    */
  k_ov5640_hi_byte_shift           = 8U,    /**< Chip-ID high-byte shift.        */
  k_ov5640_byte_mask               = 0xFFU, /**< Full-register readback mask.    */
  k_ov5640_format_yuyv             = 0x30U, /**< YUYV/JPEG ISP input format.     */
  k_ov5640_isp_mux_yuv             = 0x00U, /**< Route YUV through ISP.          */
  k_ov5640_jpeg_mode_dvp_2         = 0x02U, /**< DVP compressed-frame mode 2.    */
  k_ov5640_jpeg_mode_mask          = 0x07U, /**< JPEG mode-select field.         */
  k_ov5640_jpeg_sync_polarity_mask = 0x03U, /**< JPEG VSYNC/HREF polarity bits.  */
  k_ov5640_jpeg_sync_polarity      = 0x01U, /**< Align HREF pulses with high VD. */
  k_ov5640_jpeg_ctrl00             = 0x81U, /**< YUV422 input and FIFO enable.   */
  k_ov5640_jpeg_ctrl01             = 0x01U, /**< JPEG FIFO pacing control.       */
  k_ov5640_jpeg_ctrl04             = 0x24U, /**< Header and output control.      */
  k_ov5640_jpeg_ctrl1c             = 0x50U, /**< JPEG DVP control value.         */
  k_ov5640_jpeg_timing14_vga       = 0xAAU, /**< VGA binning/orientation timing. */
  k_ov5640_jpeg_timing20_vga       = 0x0BU, /**< VGA sampling timing.            */
  k_ov5640_timing_tc_reg20_jpeg    = 0x01U, /**< VGA JPEG vertical binning.      */
  k_ov5640_jpeg_y_start_hi         = 0x00U, /**< VGA crop Y start bits 15:8.     */
  k_ov5640_jpeg_y_start_lo         = 0x00U, /**< VGA crop Y start bits 7:0.      */
  k_ov5640_jpeg_y_end_hi           = 0x07U, /**< VGA crop Y end bits 15:8.       */
  k_ov5640_jpeg_y_end_lo           = 0x9FU, /**< VGA crop Y end bits 7:0.        */
  k_ov5640_jpeg_hts_hi             = 0x08U, /**< VGA horizontal total bits 15:8. */
  k_ov5640_jpeg_hts_lo             = 0x0CU, /**< VGA horizontal total bits 7:0.  */
  k_ov5640_jpeg_vts_hi             = 0x03U, /**< VGA vertical total bits 15:8.   */
  k_ov5640_jpeg_vts_lo             = 0xD8U, /**< VGA vertical total bits 7:0.    */
  k_ov5640_jpeg_y_offset_hi        = 0x00U, /**< VGA Y offset bits 15:8.         */
  k_ov5640_jpeg_y_offset_lo        = 0x08U, /**< VGA Y offset bits 7:0.          */
  k_ov5640_jpeg_clock_mask         = 0x28U, /**< JPEG and JPEG2x clock bits.     */
  k_ov5640_vfifo_ctrl0b_jpeg       = 0x35U, /**< JPEG VFIFO setting.             */
  k_ov5640_vfifo_ctrl0c_jpeg       = 0x22U, /**< JPEG VFIFO setting.             */
  k_ov5640_pll_bit_mode_raw        = 0x18U, /**< Qualified DVP PLL 8-bit mode.   */
  k_ov5640_pll_sys_div_raw         = 0x21U, /**< Qualified system divider.       */
  k_ov5640_pll_multiplier_raw      = 0x46U, /**< Qualified PLL multiplier.       */
  k_ov5640_pll_pre_div_raw         = 0x13U, /**< Qualified PLL pre-divider.      */
  k_ov5640_pll_bypass_disabled     = 0x00U, /**< Use the programmed PLL.         */
  k_ov5640_clock_root_raw          = 0x01U, /**< Qualified clock-root dividers.  */
  k_ov5640_clock_select_raw        = 0x02U, /**< Qualified PLL clock selection.  */
  k_ov5640_pclk_divider_raw        = 0x01U, /**< Qualified DVP PCLK divider.     */
  k_ov5640_jpeg_enable_mask        = 0x20U, /**< 0x3821 JPEG-enable bit.         */
  k_ov5640_jpeg_reset_mask         = 0x1CU, /**< JFIFO/SFIFO/JPEG reset bits.    */
  k_ov5640_jpeg_quant_scale_mask   = 0x3FU, /**< JPEG CTRL07 QS field.           */
  k_ov5640_jpeg_input_yuv422_mask  = 0x80U, /**< JPEG CTRL00 input-format bit.   */
  k_ov5640_jpeg_header_mask        = 0x20U, /**< JPEG CTRL04 header-output bit.  */
  k_ov5640_jfifo_overflow_mask     = 0x01U, /**< JPEG FIFO overflow bit.         */
  k_ov5640_isp_scale_enable_mask   = 0x20U, /**< ISP scaling-enable bit.         */
  k_ov5640_jpeg_vga_width_hi       = 0x02U, /**< VGA width bits 15:8.            */
  k_ov5640_jpeg_vga_width_lo       = 0x80U, /**< VGA width bits 7:0.             */
  k_ov5640_jpeg_vga_height_hi      = 0x01U, /**< VGA height bits 15:8.           */
  k_ov5640_jpeg_vga_height_lo      = 0xE0U, /**< VGA height bits 7:0.            */
} ra8_ov5640_value_t;

/** @brief Bit positions used to combine the three JPEG length registers. */
typedef enum : uint32_t {
  k_ov5640_jpeg_length_hi_shift  = 16U, /**< JPEG length high-byte shift.   */
  k_ov5640_jpeg_length_mid_shift = 8U,  /**< JPEG length middle-byte shift. */
  k_ov5640_dimension_hi_shift    = 8U,  /**< Compression dimension shift.   */
} ra8_ov5640_jpeg_length_shift_t;

/** @brief OV5640 settle delays around reset and register programming (ms). */
typedef enum : uint32_t {
  k_ov5640_reset_guard_ms   = 100U, /**< Guard before and after reset.       */
  k_ov5640_mcu_reset_ms     = 10U,  /**< Settle after holding the MCU reset. */
  k_ov5640_stream_settle_ms = 5U,   /**< Settle after standby/wake change.   */
  k_ov5640_cfg_settle_ms    = 500U, /**< Settle after the register sequence. */
} ra8_ov5640_delay_t;

/**
 * @struct ra8_ov5640_reg_write_t
 * @brief One OV5640 SCCB register write (16-bit address, 8-bit value).
 * @details The value plane is device data (OV5640 datasheet), not RA8D2
 *          MMIO, so no HUM citation applies; the table rows are pure data.
 */
typedef struct {
  uint16_t reg; /**< OV5640 register address. */
  uint8_t  val; /**< Value to program.        */
} ra8_ov5640_reg_write_t;

/** @brief One masked OV5640 register readback expectation. */
typedef struct {
  uint16_t reg;   /**< OV5640 register address.    */
  uint8_t  mask;  /**< Bits relevant to this mode. */
  uint8_t  value; /**< Expected masked value.      */
} ra8_ov5640_reg_expect_t;

/** @brief Raw register bytes used to assemble one JPEG status snapshot. */
typedef struct {
  uint8_t length_hi;     /**< Encoded length bits 23:16. */
  uint8_t length_mid;    /**< Encoded length bits 15:8.  */
  uint8_t length_lo;     /**< Encoded length bits 7:0.   */
  uint8_t overflow;      /**< JPEG FIFO status.          */
  uint8_t jpeg_input;    /**< JPEG input format.         */
  uint8_t jpeg_ctrl01;   /**< JPEG FIFO pacing.          */
  uint8_t jpeg_header;   /**< JPEG header control.       */
  uint8_t vfifo_ctrl00;  /**< VFIFO operating mode.      */
  uint8_t width_hi;      /**< Compression width high.    */
  uint8_t width_lo;      /**< Compression width low.     */
  uint8_t height_hi;     /**< Compression height high.   */
  uint8_t height_lo;     /**< Compression height low.    */
  uint8_t href_minimum;  /**< Minimum HREF blanking.     */
  uint8_t timing_ctrl21; /**< JPEG enable state.         */
} ra8_ov5640_jpeg_status_raw_t;

/**
 * @var s_ov5640_vga_uyvy
 * @brief OV5640 DVP init: PLL from 24 MHz XVCLK, VGA YUV422 live output.
 * @details Contains the DVP clock, VGA timing, ISP, auto-exposure,
 *          and colour-processing controls required for a live lens image.
 * @note Each row is device data; see @ref ra8_ov5640_reg_write_t.
 * @since 0.1.0
 */
static const ra8_ov5640_reg_write_t s_ov5640_vga_uyvy[] = {
  /* ---- clock / PLL: sysclk from PLL fed by the 24 MHz XVCLK ---- */
  {0x3103, 0x11},
  {0x4740, 0x20},
  {0x4050, 0x6E},
  {0x4051, 0x8F},
  {0x3103, 0x02},
  {0x3017, 0x7F},
  {0x3018, 0xFF},
  {0x302C, 0xC2},
  {(uint16_t)k_ov5640_reg_pll_bit_mode, (uint8_t)k_ov5640_pll_bit_mode_raw},
  {(uint16_t)k_ov5640_reg_pll_sys_div, (uint8_t)k_ov5640_pll_sys_div_raw},
  {(uint16_t)k_ov5640_reg_pll_multiplier, (uint8_t)k_ov5640_pll_multiplier_raw},
  {(uint16_t)k_ov5640_reg_pll_pre_div, (uint8_t)k_ov5640_pll_pre_div_raw},
  {(uint16_t)k_ov5640_reg_clock_root, (uint8_t)k_ov5640_clock_root_raw},
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
  {(uint16_t)k_ov5640_reg_pclk_divider, (uint8_t)k_ov5640_pclk_divider_raw},
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
  {0x3000, 0x20},
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

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_init(ra8_ov5640_t* dev, const ra8_ov5640_bus_t* bus)
{
  RA8_CHECK_NULL_PTR(dev, "ov5640", "init");
  RA8_CHECK_NULL_PTR(bus, "ov5640", "init");
  RA8_CHECK_NULL_PTR(bus->read_reg, "ov5640", "init");
  RA8_CHECK_NULL_PTR(bus->write_reg, "ov5640", "init");
  RA8_CHECK_NULL_PTR(bus->delay_ms, "ov5640", "init");
  *dev = (ra8_ov5640_t){
    .bus         = *bus,
    .address     = (uint8_t)k_ra8_ov5640_addr_primary,
    .initialized = true,
  };
  return k_ra8_ok;
}

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_read_reg(ra8_ov5640_t* dev, uint16_t reg, uint8_t* out_value)
{
  RA8_CHECK_NULL_PTR(dev, "ov5640", "read");
  RA8_CHECK_NULL_PTR(out_value, "ov5640", "read");
  if (!dev->initialized) {
    return k_ra8_err_not_initialized;
  }
  return dev->bus.read_reg(dev->bus.ctx, dev->address, reg, out_value);
}

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_write_reg(ra8_ov5640_t* dev, uint16_t reg, uint8_t value)
{
  RA8_CHECK_NULL_PTR(dev, "ov5640", "write");
  if (!dev->initialized) {
    return k_ra8_err_not_initialized;
  }
  return dev->bus.write_reg(dev->bus.ctx, dev->address, reg, value);
}

/**
 * @brief Update selected bits in one sensor register.
 * @details Reads the current byte, merges @p value under @p mask, and writes it.
 * @param[in,out] dev Initialized sensor instance.
 * @param[in] reg Sensor register address.
 * @param[in] mask Bits eligible for replacement.
 * @param[in] value Replacement bits.
 * @return Error code.
 * @retval k_ra8_ok The merged byte was written.
 * @retval other Propagated register read or write error.
 * @pre @p dev is initialized and addresses a responding sensor.
 * @pre The bound transport is idle.
 * @post On success masked bits equal @p value masked by @p mask.
 * @post On success unmasked bits retain their prior values.
 * @note Not thread-safe with respect to the same sensor instance.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_ov5640_update_bits(ra8_ov5640_t* dev, uint16_t reg, uint8_t mask, uint8_t value)
{
  uint8_t   current = 0U;
  ra8_err_t err     = ra8_ov5640_read_reg(dev, reg, &current);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint8_t updated = (uint8_t)((current & (uint8_t)~mask) | (value & mask));
  return ra8_ov5640_write_reg(dev, reg, updated);
}

/**
 * @brief Verify a bounded set of masked sensor register expectations.
 * @details Reads each entry and compares only the bits selected by its mask.
 * @param[in,out] dev Initialized sensor instance.
 * @param[in] expected Readback table containing @p count entries.
 * @param[in] count Bounded table-entry count.
 * @return Error code.
 * @retval k_ra8_ok Every masked readback matched.
 * @retval k_ra8_err_invalid_arg A masked readback differed.
 * @retval other Propagated register-read error.
 * @pre @p expected addresses at least @p count readable entries.
 * @pre @p dev is initialized and the transport is idle.
 * @post Sensor state and the expectation table are unchanged.
 * @post Success proves every requested masked equality.
 * @note Not thread-safe with respect to the same sensor instance.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_ov5640_verify(ra8_ov5640_t* dev, const ra8_ov5640_reg_expect_t* expected, uint32_t count)
{
  for (uint32_t i = 0U; i < count; i += 1U) {
    uint8_t   actual = 0U;
    ra8_err_t err    = ra8_ov5640_read_reg(dev, expected[i].reg, &actual);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((actual & expected[i].mask) != expected[i].value) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Read and combine the OV5640 high and low chip-ID bytes.
 * @details Performs two SCCB reads at the instance's currently selected address.
 * @param[in,out] dev Initialized sensor instance.
 * @param[out] out_id Combined big-endian chip identifier on success.
 * @return Error code.
 * @retval k_ra8_ok Both bytes were read and combined.
 * @retval other Propagated register-read error.
 * @pre @p out_id points to writable storage.
 * @pre @p dev is initialized and the sensor is clocked.
 * @post On success @p out_id contains the combined identifier.
 * @post The selected device address is unchanged.
 * @note Not thread-safe with respect to the same sensor instance.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ov5640_read_chip_id(ra8_ov5640_t* dev, uint16_t* out_id)
{
  uint8_t   hi  = 0U;
  uint8_t   lo  = 0U;
  ra8_err_t err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_chip_id_hi, &hi);
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_chip_id_lo, &lo);
  }
  if (err == k_ra8_ok) {
    *out_id = (uint16_t)(((uint16_t)hi << (uint16_t)k_ov5640_hi_byte_shift) | (uint16_t)lo);
  }
  return err;
}

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_probe(ra8_ov5640_t* dev, uint16_t* out_id)
{
  RA8_CHECK_NULL_PTR(dev, "ov5640", "probe");
  RA8_CHECK_NULL_PTR(out_id, "ov5640", "probe");
  if (!dev->initialized) {
    return k_ra8_err_not_initialized;
  }
  *out_id                = 0U;
  const uint8_t addrs[2] = {(uint8_t)k_ra8_ov5640_addr_primary,
                            (uint8_t)k_ra8_ov5640_addr_secondary};
  for (uint32_t i = 0U; i < (uint32_t)sizeof(addrs); i++) {
    dev->address = addrs[i];
    uint16_t id  = 0U;
    if ((internal_ov5640_read_chip_id(dev, &id) == k_ra8_ok) &&
        (id == (uint16_t)k_ra8_ov5640_chip_id)) {
      *out_id = id;
      return k_ra8_ok;
    }
    *out_id = id;
  }
  dev->address = (uint8_t)k_ra8_ov5640_addr_primary;
  return k_ra8_err_not_found;
}

/**
 * @brief Program the board-qualified VGA base table.
 * @details Writes every retained Renesas DVP scene row in order and applies
 *          the documented delay when the table holds the sensor MCU reset.
 * @param[in,out] dev Initialized sensor instance.
 * @return Error code.
 * @retval k_ra8_ok Every base-table row was written.
 * @retval other Propagated register-write error.
 * @pre The sensor is held in software reset.
 * @pre The bound SCCB transport is idle.
 * @post On success the VGA UYVY base scene is programmed.
 * @post On success the MCU-reset settling interval has elapsed.
 * @note Applies the required MCU-reset delay at the matching table row.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ov5640_write_vga_base(ra8_ov5640_t* dev)
{
  const uint32_t count = (uint32_t)(sizeof(s_ov5640_vga_uyvy) / sizeof(ra8_ov5640_reg_write_t));
  for (uint32_t i = 0U; i < count; i += 1U) {
    const ra8_err_t err =
      ra8_ov5640_write_reg(dev, s_ov5640_vga_uyvy[i].reg, s_ov5640_vga_uyvy[i].val);
    if (err != k_ra8_ok) {
      return err;
    }
    if (s_ov5640_vga_uyvy[i].reg == (uint16_t)k_ov5640_reg_system_reset00) {
      dev->bus.delay_ms(dev->bus.ctx, (uint32_t)k_ov5640_mcu_reset_ms);
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Apply the JPEG-specific overlay to the VGA base scene.
 * @details Programs JPEG quality and mode, then updates shared format,
 *          polarity, reset, and clock controls without disturbing other bits.
 * @param[in,out] dev Initialized sensor instance.
 * @return Error code.
 * @retval k_ra8_ok The JPEG path was configured and the MCU held reset.
 * @retval other Propagated register-access error.
 * @pre The VGA base scene is programmed while the sensor is stopped.
 * @pre The bound SCCB transport is idle.
 * @post On success JPEG format, clocks, reset, and polarity are configured.
 * @post On success the sensor MCU remains held reset until the final wake.
 * @note Preserves unrelated bits in every shared control register.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ov5640_configure_jpeg(ra8_ov5640_t* dev)
{
  static const ra8_ov5640_reg_write_t jpeg_writes[] = {
    {(uint16_t)k_ov5640_reg_jpeg_quality, (uint8_t)k_ra8_ov5640_jpeg_quant_scale_default},
    {(uint16_t)k_ov5640_reg_jpeg_mode, (uint8_t)k_ov5640_jpeg_mode_dvp_2},
    {(uint16_t)k_ov5640_reg_format, (uint8_t)k_ov5640_format_yuyv},
    {(uint16_t)k_ov5640_reg_isp_mux, (uint8_t)k_ov5640_isp_mux_yuv},
  };
  const uint32_t count = (uint32_t)(sizeof(jpeg_writes) / sizeof(ra8_ov5640_reg_write_t));
  for (uint32_t i = 0U; i < count; i += 1U) {
    const ra8_err_t err = ra8_ov5640_write_reg(dev, jpeg_writes[i].reg, jpeg_writes[i].val);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  ra8_err_t err = internal_ov5640_update_bits(dev,
                                              (uint16_t)k_ov5640_reg_polarity_ctrl00,
                                              (uint8_t)k_ov5640_jpeg_sync_polarity_mask,
                                              (uint8_t)k_ov5640_jpeg_sync_polarity);
  if (err == k_ra8_ok) {
    err = internal_ov5640_update_bits(dev,
                                      (uint16_t)k_ov5640_reg_timing_tc_reg21,
                                      (uint8_t)k_ov5640_jpeg_enable_mask,
                                      (uint8_t)k_ov5640_jpeg_enable_mask);
  }
  if (err == k_ra8_ok) {
    err = internal_ov5640_update_bits(dev,
                                      (uint16_t)k_ov5640_reg_system_reset02,
                                      (uint8_t)k_ov5640_jpeg_reset_mask,
                                      0U);
  }
  if (err == k_ra8_ok) {
    err = internal_ov5640_update_bits(dev,
                                      (uint16_t)k_ov5640_reg_clock_enable02,
                                      (uint8_t)k_ov5640_jpeg_clock_mask,
                                      (uint8_t)k_ov5640_jpeg_clock_mask);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_write_reg(dev,
                               (uint16_t)k_ov5640_reg_system_reset00,
                               (uint8_t)k_ov5640_mcu_reset_hold);
  }
  return err;
}

/**
 * @brief Verify the common UYVY register scene.
 * @details Reads the format, ISP mux, and test-pattern controls through the
 *          masked table verifier.
 * @param[in,out] dev Initialized sensor instance.
 * @return Error code from masked register verification.
 * @retval k_ra8_ok Every common UYVY field matches.
 * @retval other Propagated read or mismatch error.
 * @pre Configuration writes and their settle delay are complete.
 * @pre The bound SCCB transport is idle.
 * @post Sensor state is unchanged.
 * @post Success proves all common UYVY expectations matched.
 * @note Not thread-safe with respect to the same sensor instance.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ov5640_verify_uyvy(ra8_ov5640_t* dev)
{
  static const ra8_ov5640_reg_expect_t expected[] = {
    {(uint16_t)k_ov5640_reg_format, (uint8_t)k_ov5640_byte_mask, (uint8_t)k_ov5640_format_yuyv},
    {(uint16_t)k_ov5640_reg_isp_mux, (uint8_t)k_ov5640_byte_mask, (uint8_t)k_ov5640_isp_mux_yuv},
    {(uint16_t)k_ov5640_reg_test, (uint8_t)k_ov5640_byte_mask, 0x00U},
  };
  return internal_ov5640_verify(dev,
                                expected,
                                (uint32_t)(sizeof(expected) / sizeof(ra8_ov5640_reg_expect_t)));
}

/**
 * @brief Verify the JPEG-specific register scene.
 * @details Reads the JPEG reset, clock, enable, quality, mode, polarity, and
 *          qualified DVP clock fields through the masked table verifier.
 * @param[in,out] dev Initialized sensor instance.
 * @return Error code from masked register verification.
 * @retval k_ra8_ok Every JPEG-specific field matches.
 * @retval other Propagated read or mismatch error.
 * @pre JPEG overlay writes and their settle delay are complete.
 * @pre The bound SCCB transport is idle.
 * @post Sensor state is unchanged.
 * @post Success proves all JPEG-specific expectations matched.
 * @note Not thread-safe with respect to the same sensor instance.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ov5640_verify_jpeg(ra8_ov5640_t* dev)
{
  static const ra8_ov5640_reg_expect_t expected[] = {
    {(uint16_t)k_ov5640_reg_system_reset02, (uint8_t)k_ov5640_jpeg_reset_mask, 0x00U},
    {(uint16_t)k_ov5640_reg_clock_enable02,
     (uint8_t)k_ov5640_jpeg_clock_mask,
     (uint8_t)k_ov5640_jpeg_clock_mask},
    {(uint16_t)k_ov5640_reg_timing_tc_reg21,
     (uint8_t)k_ov5640_jpeg_enable_mask,
     (uint8_t)k_ov5640_jpeg_enable_mask},
    {(uint16_t)k_ov5640_reg_jpeg_quality,
     (uint8_t)k_ov5640_jpeg_quant_scale_mask,
     (uint8_t)k_ra8_ov5640_jpeg_quant_scale_default},
    {(uint16_t)k_ov5640_reg_jpeg_mode,
     (uint8_t)k_ov5640_jpeg_mode_mask,
     (uint8_t)k_ov5640_jpeg_mode_dvp_2},
    {(uint16_t)k_ov5640_reg_polarity_ctrl00,
     (uint8_t)k_ov5640_jpeg_sync_polarity_mask,
     (uint8_t)k_ov5640_jpeg_sync_polarity},
    {(uint16_t)k_ov5640_reg_pll_bypass,
     (uint8_t)k_ov5640_byte_mask,
     (uint8_t)k_ov5640_pll_bypass_disabled},
    {(uint16_t)k_ov5640_reg_pclk_divider,
     (uint8_t)k_ov5640_byte_mask,
     (uint8_t)k_ov5640_pclk_divider_raw},
  };
  return internal_ov5640_verify(dev,
                                expected,
                                (uint32_t)(sizeof(expected) / sizeof(ra8_ov5640_reg_expect_t)));
}

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_configure(ra8_ov5640_t* dev, ra8_ov5640_mode_t mode)
{
  RA8_CHECK_NULL_PTR(dev, "ov5640", "configure");
  if (!dev->initialized) {
    return k_ra8_err_not_initialized;
  }
  if ((mode != k_ra8_ov5640_mode_vga_uyvy) && (mode != k_ra8_ov5640_mode_vga_jpeg)) {
    return k_ra8_err_not_supported;
  }
  dev->bus.delay_ms(dev->bus.ctx, (uint32_t)k_ov5640_reset_guard_ms);
  ra8_err_t err =
    ra8_ov5640_write_reg(dev, (uint16_t)k_ov5640_reg_sw_reset, (uint8_t)k_ov5640_sw_reset_hold);
  if (err != k_ra8_ok) {
    return err;
  }
  dev->bus.delay_ms(dev->bus.ctx, (uint32_t)k_ov5640_reset_guard_ms);
  err = internal_ov5640_write_vga_base(dev);
  if (mode == k_ra8_ov5640_mode_vga_jpeg) {
    err = internal_ov5640_configure_jpeg(dev);
  }
  if (err != k_ra8_ok) {
    return err;
  }
  dev->bus.delay_ms(dev->bus.ctx, (uint32_t)k_ov5640_cfg_settle_ms);
  err = internal_ov5640_verify_uyvy(dev);
  if ((err != k_ra8_ok) || (mode != k_ra8_ov5640_mode_vga_jpeg)) {
    return err;
  }
  return internal_ov5640_verify_jpeg(dev);
}

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_set_jpeg_quantization_scale(ra8_ov5640_t* dev, uint8_t quant_scale)
{
  RA8_CHECK_NULL_PTR(dev, "ov5640", "jpeg_quality");
  if (!dev->initialized) {
    return k_ra8_err_not_initialized;
  }
  if (quant_scale > (uint8_t)k_ra8_ov5640_jpeg_quant_scale_max) {
    return k_ra8_err_invalid_arg;
  }
  return internal_ov5640_update_bits(dev,
                                     (uint16_t)k_ov5640_reg_jpeg_quality,
                                     (uint8_t)k_ov5640_jpeg_quant_scale_mask,
                                     quant_scale);
}

/**
 * @brief Read the register bytes comprising one JPEG status snapshot.
 * @details Reads length, FIFO, format, header, geometry, blanking, and enable
 *          registers in a fixed order into caller-owned raw storage.
 * @param[in,out] dev Initialized sensor instance.
 * @param[out] raw Destination for raw register bytes.
 * @return Error code.
 * @retval k_ra8_ok Every status register was read.
 * @retval other First propagated register-read error.
 * @pre The sensor stream is stopped so the multi-register snapshot is stable.
 * @pre The bound SCCB transport is idle.
 * @post On success every field in @p raw is initialized.
 * @post Sensor register state is unchanged.
 * @note The helper stops at the first transport error.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ov5640_read_jpeg_status(ra8_ov5640_t*                 dev,
                                                               ra8_ov5640_jpeg_status_raw_t* raw)
{
  ra8_err_t err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_jpeg_length_hi, &raw->length_hi);
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_jpeg_length_mid, &raw->length_mid);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_jpeg_length_lo, &raw->length_lo);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_jfifo_overflow, &raw->overflow);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_jpeg_ctrl00, &raw->jpeg_input);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_jpeg_ctrl01, &raw->jpeg_ctrl01);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_jpeg_ctrl04, &raw->jpeg_header);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_vfifo_ctrl00, &raw->vfifo_ctrl00);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_compression_w_hi, &raw->width_hi);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_compression_w_lo, &raw->width_lo);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_compression_h_hi, &raw->height_hi);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_compression_h_lo, &raw->height_lo);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_href_minimum, &raw->href_minimum);
  }
  if (err == k_ra8_ok) {
    err = ra8_ov5640_read_reg(dev, (uint16_t)k_ov5640_reg_timing_tc_reg21, &raw->timing_ctrl21);
  }
  return err;
}

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_jpeg_status_get(ra8_ov5640_t* dev, ra8_ov5640_jpeg_status_t* out_status)
{
  RA8_CHECK_NULL_PTR(dev, "ov5640", "jpeg_status");
  RA8_CHECK_NULL_PTR(out_status, "ov5640", "jpeg_status");
  *out_status = (ra8_ov5640_jpeg_status_t){};
  if (!dev->initialized) {
    return k_ra8_err_not_initialized;
  }
  ra8_ov5640_jpeg_status_raw_t raw = {};
  const ra8_err_t              err = internal_ov5640_read_jpeg_status(dev, &raw);
  if (err != k_ra8_ok) {
    return err;
  }

  *out_status = (ra8_ov5640_jpeg_status_t){
    .encoded_bytes = ((uint32_t)raw.length_hi << (uint32_t)k_ov5640_jpeg_length_hi_shift) |
                     ((uint32_t)raw.length_mid << (uint32_t)k_ov5640_jpeg_length_mid_shift) |
                     (uint32_t)raw.length_lo,
    .compression_width =
      (uint16_t)(((uint16_t)raw.width_hi << (uint16_t)k_ov5640_dimension_hi_shift) | raw.width_lo),
    .compression_height =
      (uint16_t)(((uint16_t)raw.height_hi << (uint16_t)k_ov5640_dimension_hi_shift) |
                 raw.height_lo),
    .jpeg_ctrl01           = raw.jpeg_ctrl01,
    .vfifo_ctrl00          = raw.vfifo_ctrl00,
    .href_minimum_blanking = raw.href_minimum,
    .fifo_overflow         = (raw.overflow & (uint8_t)k_ov5640_jfifo_overflow_mask) != 0U,
    .input_is_yuv422       = (raw.jpeg_input & (uint8_t)k_ov5640_jpeg_input_yuv422_mask) != 0U,
    .header_output         = (raw.jpeg_header & (uint8_t)k_ov5640_jpeg_header_mask) != 0U,
    .compression_enabled   = (raw.timing_ctrl21 & (uint8_t)k_ov5640_jpeg_enable_mask) != 0U,
  };
  return k_ra8_ok;
}

/* See the public header for the documented contract. */
ra8_err_t ra8_ov5640_stream_set(ra8_ov5640_t* dev, bool enabled)
{
  if (dev == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!dev->initialized) {
    return k_ra8_err_not_initialized;
  }
  const uint8_t   value = enabled ? (uint8_t)k_ov5640_sw_reset_wake : (uint8_t)k_ov5640_sw_standby;
  const ra8_err_t err   = ra8_ov5640_write_reg(dev, (uint16_t)k_ov5640_reg_sw_reset, value);
  if (err == k_ra8_ok) {
    dev->bus.delay_ms(dev->bus.ctx, (uint32_t)k_ov5640_stream_settle_ms);
  }
  return err;
}
