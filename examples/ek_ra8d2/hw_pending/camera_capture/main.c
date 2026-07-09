/**
 * @file examples/ek_ra8d2/hw_pending/camera_capture/main.c
 * @brief OV5640 parallel (CEU) camera capture + plausibility self-test
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * End-to-end bring-up of the EK-RA8D2 on-board OV5640 5 MP CMOS sensor
 * over the chip's Capture Engine Unit (CEU) parallel (DVP) path -- the
 * interface the board wires the camera to (connector J35, HUM Ch 60).
 * The app drives the real HAL drivers with no fakes and reports a
 * plausibility verdict over the SCI8 console plus a set of
 * J-Link-probable globals:
 *
 *   1. `ra_cgc_init` / `ra_time_init` / `ra_mstp_init` -- clocks + SysTick.
 *   2. `ra_board_uart_console_init` -- BSP SCI8 console (PD02 / PD03).
 *   3. XCLK: GPT channel 12 saw-PWM on GTIOC12A (P501) generates the
 *      ~24 MHz sensor input clock (XVCLK). The OV5640 needs XVCLK to
 *      answer even on its SCCB port, so this comes up first.
 *   4. I2C ch1 (SCL1 P512 / SDA1 P511) via the board's validated
 *      bring-up, then force SW4-6 = ON through the U15 expander so the
 *      Camera Expansion Board is in parallel (DVP) mode (the board
 *      default is MIPI). The OV5640 SCCB shares ch1 with U15 + the codec.
 *   5. Confirm the XCLK GPT counter advances, route the CEU DVP pins
 *      (VIO_D[7:0], VIO_VD, VIO_HD, VIO_CLK), release the sensor RST
 *      strap (P709), then scan the bus and read the OV5640 chip-ID
 *      registers 0x300A/0x300B -- the VERIFY-FIRST proof the sensor is
 *      present (expected 0x5640), trying SCCB 0x3C then 0x3D.
 *   6. SCCB register sequence: software reset + a compact DVP YUV422
 *      QVGA config with the built-in colour-bar test pattern enabled,
 *      so the captured frame is deterministic and independent of lens
 *      focus or scene light.
 *   7. `ra_ceu_init` + `ra_ceu_capture_arm` capture one frame into an
 *      internal SRAM buffer; the driver polls CETCR.CPE for completion.
 *   8. Plausibility stats over the frame: min / max / mean byte. The
 *      verdict is PASS when the sensor ID is 0x5640, a frame was
 *      captured, and the frame is non-degenerate (max != min) -- a
 *      colour bar spans black..white so a real grab always varies,
 *      while a dead bus reads all-identical bytes.
 *
 * Banner (scraped by the HIL gate once promoted):
 *   `cam: gpt=RUN scan=3C:56.43:00. chipid=5640 xclk=OK rst=OK sccb=OK`
 *   ` ceu=OK frame=OK min=.. max=.. mean=.. verdict=PASS`
 *
 * Hardware: EK-RA8D2 with the OV5640 Camera Expansion Board on the
 * underside FFC port (J35). SW4-6 is driven ON in firmware; no manual
 * jumpers required.
 *
 * @note Bench status (silicon, SWD forensics): SCCB works and the chip
 *       ID reads 0x5640; the OV5640 fully streams over the parallel port
 *       -- VIO_VD, VIO_HD, VIO_CLK and VIO_D[7:0] were all confirmed
 *       toggling on the MCU pins (PORT PIDR sampling). The remaining
 *       blocker is the CEU: every armed frame ends with CETCR IGHS
 *       (bit17, "HD clock-cycle count differs from CMCYR.HCYL") followed
 *       by VBP (bit20, "invalid VD"), and zero bytes are written. No
 *       HCYL value from 2..2560 clears IGHS, so the VIO_HD active-cycle
 *       count the CEU sees is not a stable/matchable width. Resolving
 *       this needs a logic analyzer on VIO_HD / VIO_CLK to measure the
 *       real HREF-active duration and the PCLK/HREF phase; it cannot be
 *       fixed by register config alone. See the README.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_ceu.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_gpt.h"
#include "ra_i2c.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_utils.h"
#include "ra_time.h"

/* =============================================================================
 * Tunable constants (typed enums -- no magic numbers)
 * =============================================================================
 */

/** @brief Scalar app tunables (baud, channels, delays, geometry). */
typedef enum : uint32_t {
  k_cam_baud            = 115200U,   /**< SCI8 console baud.                     */
  k_cam_xclk_hz         = 24000000U, /**< OV5640 XVCLK input-clock target, Hz.   */
  k_cam_frame_bytes     = 153600U,   /**< 320 x 240 x 2 bytes (QVGA YUV422).     */
  k_cam_capture_poll_ms = 5U,        /**< Poll step while awaiting CETCR.CPE.    */
  k_cam_capture_tries   = 400U,      /**< Bounded capture wait (~2 s).           */
  k_cam_reset_low_ms    = 20U,       /**< Sensor RST held low.                   */
  k_cam_reset_high_ms   = 20U,       /**< Settle after RST release.              */
  k_cam_swreset_ms      = 10U,       /**< Settle after SCCB software reset.      */
  k_cam_cfg_settle_ms   = 100U,      /**< Settle after the register sequence.    */
  k_cam_dec_cap         = 99999999U, /**< Max value cam_put_u32 can render.      */
  k_cam_gpt_period_max  = 0xFFFFU,   /**< Max GPT period (16-bit GTPR).          */
  k_cam_gpt_probe_iters = 64U,       /**< GTCNT samples to prove the timer runs. */
} cam_u32_t;

/** @brief 16-bit constants: SCCB register addresses + expected chip ID. */
typedef enum : uint16_t {
  k_cam_reg_chip_id_hi = 0x300AU, /**< OV5640 chip-ID high byte register. */
  k_cam_reg_chip_id_lo = 0x300BU, /**< OV5640 chip-ID low byte register.  */
  k_cam_reg_sw_reset   = 0x3008U, /**< OV5640 system control register.    */
  k_cam_chip_id_ov5640 = 0x5640U, /**< Expected OV5640 chip-ID value.     */
  k_cam_width_px       = 320U,    /**< Captured frame width (QVGA).       */
  k_cam_height_px      = 240U,    /**< Captured frame height (QVGA).      */
  k_cam_line_bytes     = 640U,    /**< Bytes per line (width x 2).        */
} cam_u16_t;

/** @brief 8-bit constants: SCCB address, geometry byte-count, bit work. */
typedef enum : uint8_t {
  k_cam_sccb_addr_7b   = 0x3CU, /**< OV5640 SCCB 7-bit address (SID low).    */
  k_cam_sw_reset_hold  = 0x82U, /**< 0x3008 = software reset (bit7).         */
  k_cam_sw_reset_wake  = 0x02U, /**< 0x3008 = normal operation.              */
  k_cam_bytes_per_px   = 2U,    /**< YUV422 / RGB565 -> 2 bytes/pixel.       */
  k_cam_reg_addr_bytes = 2U,    /**< OV5640 uses 16-bit register pointers.   */
  k_cam_hi_byte_shift  = 8U,    /**< Shift for the high register byte.       */
  k_cam_byte_mask      = 0xFFU, /**< Low-byte mask.                          */
  k_cam_nibble_mask    = 0x0FU, /**< Low-nibble mask for hex printing.       */
  k_cam_dec_base       = 10U,   /**< Base for decimal printing.              */
  k_cam_scan_lo        = 0x08U, /**< First 7-bit address in the bus scan.    */
  k_cam_scan_hi        = 0x77U, /**< Last 7-bit address in the bus scan.     */
  k_cam_sccb_addr_alt  = 0x3DU, /**< OV5640 SCCB address when SID is high.   */
  k_cam_u15_addr       = 0x43U, /**< U15 PI4IOE5V6408 SW4-override address.  */
  k_cam_u15_reg_output = 0x05U, /**< U15 output register (1 = SW4 OFF).      */
  k_cam_sw46_bit       = 5U,    /**< U15 output bit for SW4-6 (camera mode). */
} cam_u8_t;

/** @brief GPT channel that generates XCLK on GTIOC12A (P501). */
typedef enum : uint8_t {
  k_cam_xclk_gpt_ch = 12U, /**< P501 = GTIOC12A per RA8D2 datasheet pinout. */
} cam_gpt_t;

/** @brief RIIC channel carrying the OV5640 SCCB bus. */
typedef enum : uint8_t {
  k_cam_iic_ch = 1U, /**< SCL1 P512 / SDA1 P511 (shared with U15). */
} cam_iic_t;

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
 * @var s_ov5640_dvp_colorbar
 * @brief Compact OV5640 DVP init: PLL from 24 MHz XVCLK, YUV422 output,
 *        QVGA (320x240) window, colour-bar test pattern enabled.
 * @details Trimmed from the OmniVision OV5640 datasheet DVP programming
 *          guide to the registers a deterministic test-pattern grab
 *          needs (clock, pad-enable, format, output window, colour bar).
 *          Autofocus / AWB / lens-correction firmware is intentionally
 *          omitted -- the colour bar is synthesised in the ISP and needs
 *          none of it.
 * @note Each row is device data; see @ref cam_reg_t.
 * @since 0.1.0
 */
static const cam_reg_t s_ov5640_dvp_colorbar[] = {
  /* ---- clock / PLL: sysclk from PLL fed by the 24 MHz XVCLK ---- */
  {0x3103, 0x11},
  {0x3103, 0x03},
  {0x3017, 0x7F},
  {0x3018, 0xFC},
  {0x3034, 0x1A},
  {0x3035, 0x21},
  {0x3036, 0x69},
  {0x3037, 0x13},
  {0x3108, 0x01},
  {0x3630, 0x36},
  {0x3631, 0x0E},
  {0x3632, 0xE2},
  {0x3633, 0x12},
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
  {0x3635, 0x13},
  {0x3636, 0x03},
  {0x3634, 0x40},
  {0x3622, 0x01},
  /* ---- timing: full array windowed to a QVGA DVP output ---- */
  {0x3808, 0x01},
  {0x3809, 0x40},
  {0x380A, 0x00},
  {0x380B, 0xF0},
  {0x380C, 0x07},
  {0x380D, 0x68},
  {0x380E, 0x03},
  {0x380F, 0xD8},
  {0x3800, 0x00},
  {0x3801, 0x00},
  {0x3802, 0x00},
  {0x3803, 0x00},
  {0x3804, 0x0A},
  {0x3805, 0x3F},
  {0x3806, 0x07},
  {0x3807, 0x9F},
  {0x3810, 0x00},
  {0x3811, 0x10},
  {0x3812, 0x00},
  {0x3813, 0x06},
  {0x3814, 0x31},
  {0x3815, 0x31},
  {0x3820, 0x41},
  {0x3821, 0x07},
  /* ---- ISP + DVP output format: YUV422 (YUYV) ---- */
  {0x4300, 0x30},
  {0x501F, 0x00},
  {0x4713, 0x02},
  {0x4407, 0x04},
  {0x460B, 0x35},
  {0x460C, 0x22},
  {0x4837, 0x0A},
  {0x3824, 0x02},
  {0x5000, 0xA7},
  {0x5001, 0xA3},
  /* ---- DVP pad clock enable + HREF/VSYNC/PCLK polarity ----
     0x4740=0x00: VSYNC + HREF active-high, matching the CEU CAMCR
     (HDPOL=VDPOL=0). Do NOT clear 0x300E bit6 here: on the EK-RA8D2
     that bit gates VIO_VD to the DVP port -- clearing it (e.g. the
     Linux 0x300E=0x18 MIPI-power-down) makes the CEU report NVD. */
  {0x3000, 0x00},
  {0x3002, 0x00},
  {0x3004, 0xFF},
  {0x3006, 0xFF},
  {0x4740, 0x00},
  /* ---- colour-bar test pattern (deterministic frame) ---- */
  {0x503D, 0x80},
};

/* =============================================================================
 * J-Link-probable result globals
 * =============================================================================
 */

/** @var g_cam_chipid
 *  @brief Chip ID read over SCCB (0x300A<<8 | 0x300B). 0x5640 = OV5640.
 *  @warning Written once by main(); read via SWD.  @since 0.1.0 */
volatile uint32_t g_cam_chipid = 0U;
/** @var g_cam_frame_ok
 *  @brief 1 when the CEU signalled a completed capture, else 0.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_frame_ok = 0U;
/** @var g_cam_min
 *  @brief Minimum byte value across the captured frame.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_min = 0U;
/** @var g_cam_max
 *  @brief Maximum byte value across the captured frame.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_max = 0U;
/** @var g_cam_mean
 *  @brief Mean byte value across the captured frame.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_mean = 0U;
/** @var g_cam_verdict
 *  @brief 1 = plausible camera frame (PASS), 0 = FAIL.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_verdict = 0U;

/**
 * @var s_frame
 * @brief Capture target for one QVGA YUV422 frame (internal SRAM).
 * @details 8-byte aligned per HUM Ch 60.2.13 p 3656 (CDAYR alignment).
 * @warning Written by the CEU bus initiator; do not touch mid-capture.
 * @since 0.1.0
 */
alignas(8) static uint8_t s_frame[k_cam_frame_bytes];

/** @brief Console tag prefix for every banner line. */
static const uint8_t k_cam_tag[] = "cam: ";

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
 * Console helpers
 * =============================================================================
 */

/**
 * @brief Bounded length of a NUL-terminated C string.
 *
 * @param[in] s Non-NULL C string.
 * @return Length in bytes, capped at ::k_cam_frame_bytes.
 *
 * @pre `s` is non-NULL.
 * @pre The string is NUL-terminated within the cap.
 * @post Return value never exceeds the cap.
 * @post `s` is not modified.
 * @note Thread safety: pure function.
 * @since 0.1.0
 */
static uint32_t cam_strlen(const char* s)
{
  RA_CHECK_NULL_PTR(s, "cam", "strlen");
  uint32_t n = 0U;
  while ((n < (uint32_t)k_cam_frame_bytes) && (s[n] != '\0')) {
    n += 1U;
  }
  return n;
}

/**
 * @brief Write a NUL-terminated string to the SCI8 console.
 *
 * @param[in] s Non-NULL C string.
 * @return ra_err_t from the BSP console write.
 * @retval k_ra_ok String queued.
 * @retval k_ra_err_null_ptr `s` was NULL.
 *
 * @pre `s` is non-NULL.
 * @pre `ra_board_uart_console_init` has run.
 * @post The bytes of `s` are handed to the console.
 * @post `s` is not modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t cam_puts(const char* s)
{
  RA_CHECK_NULL_PTR(s, "cam", "puts");
  const uint32_t len = cam_strlen(s);
  return ra_board_uart_console_write((const uint8_t*)s, (size_t)len);
}

/**
 * @brief Write `width` hex nibbles of `value` (big-endian) to the console.
 *
 * @param[in] value Value to print.
 * @param[in] width Number of nibbles (1..8).
 * @return ra_err_t from the console write.
 * @retval k_ra_ok Digits queued.
 * @retval k_ra_err_invalid_arg `width` out of 1..8.
 *
 * @pre `width` is 1..8.
 * @pre `ra_board_uart_console_init` has run.
 * @post Exactly `width` hex characters are emitted.
 * @post No other console state changes.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t cam_put_hex(uint32_t value, uint8_t width)
{
  static const char k_hex[] = "0123456789ABCDEF";
  RA_CHECK_RANGE(width, 1U, 8U, k_ra_err_invalid_arg);
  char     buf[8];
  uint8_t  i = width;
  uint32_t v = value;
  while (i > 0U) {
    i -= 1U;
    buf[i] = k_hex[v & (uint32_t)k_cam_nibble_mask];
    v >>= 4U;
  }
  return ra_board_uart_console_write((const uint8_t*)buf, (size_t)width);
}

/**
 * @brief Write an unsigned decimal integer to the console.
 *
 * @param[in] value Value to print (0..99999999).
 * @return ra_err_t from the console write.
 * @retval k_ra_ok Digits queued.
 * @retval k_ra_err_invalid_arg `value` exceeds 8 decimal digits.
 *
 * @pre `value` fits in 8 decimal digits.
 * @pre `ra_board_uart_console_init` has run.
 * @post The decimal text of `value` is emitted.
 * @post No other console state changes.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t cam_put_u32(uint32_t value)
{
  if (value > (uint32_t)k_cam_dec_cap) {
    return k_ra_err_invalid_arg;
  }
  char     buf[8];
  uint8_t  i = (uint8_t)sizeof(buf);
  uint32_t v = value;
  do {
    i -= 1U;
    buf[i] = (char)('0' + (v % (uint32_t)k_cam_dec_base));
    v /= (uint32_t)k_cam_dec_base;
  } while ((v != 0U) && (i > 0U));
  const uint32_t len = (uint32_t)((uint8_t)sizeof(buf) - i);
  return ra_board_uart_console_write((const uint8_t*)&buf[i], (size_t)len);
}

/* =============================================================================
 * SCCB (I2C) helpers
 * =============================================================================
 */

/**
 * @brief Write one OV5640 register (16-bit address, 8-bit value) over SCCB.
 *
 * @param[in] reg OV5640 register address.
 * @param[in] val Value to program.
 * @return ra_err_t from the RIIC write.
 * @retval k_ra_ok Register written and ACKed.
 * @retval k_ra_err_nack Sensor NACKed.
 *
 * @pre RIIC channel 1 is initialized.
 * @pre XVCLK is running (the OV5640 core clocks SCCB from it).
 * @post The addressed register holds `val` (on ACK).
 * @post The bus is released (STOP issued).
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t cam_sccb_write(uint16_t reg, uint8_t val)
{
  if (reg == 0U) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t payload[3] = {
    (uint8_t)((reg >> (uint16_t)k_cam_hi_byte_shift) & (uint16_t)k_cam_byte_mask),
    (uint8_t)(reg & (uint16_t)k_cam_byte_mask),
    val,
  };
  return ra_i2c_write((uint8_t)k_cam_iic_ch, s_sccb_addr, payload, (uint32_t)sizeof(payload), true);
}

/**
 * @brief Read one OV5640 register (16-bit address, 8-bit value) over SCCB.
 *
 * @param[in]  reg     OV5640 register address.
 * @param[out] out_val Receives the register value.
 * @return ra_err_t from the RIIC combined transfer.
 * @retval k_ra_ok Register read.
 * @retval k_ra_err_null_ptr `out_val` was NULL.
 * @retval k_ra_err_nack Sensor NACKed.
 *
 * @pre RIIC channel 1 is initialized and XVCLK is running.
 * @pre `out_val` is non-NULL.
 * @post `*out_val` holds the register value on success.
 * @post The bus is released.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t cam_sccb_read(uint16_t reg, uint8_t* out_val)
{
  RA_CHECK_NULL_PTR(out_val, "cam", "sccb_read");
  if (reg == 0U) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t addr[2] = {
    (uint8_t)((reg >> (uint16_t)k_cam_hi_byte_shift) & (uint16_t)k_cam_byte_mask),
    (uint8_t)(reg & (uint16_t)k_cam_byte_mask),
  };
  return ra_i2c_transfer((uint8_t)k_cam_iic_ch,
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
 * @return ra_err_t; ok only when both bytes read.
 * @retval k_ra_ok Chip ID composed into `*out_id`.
 * @retval k_ra_err_null_ptr `out_id` was NULL.
 *
 * @pre RIIC ch1 up, XVCLK running.
 * @pre `out_id` is non-NULL.
 * @post `*out_id` holds the 16-bit ID on success.
 * @post The bus is released.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t cam_read_chip_id(uint16_t* out_id)
{
  RA_CHECK_NULL_PTR(out_id, "cam", "chip_id");
  uint8_t  hi  = 0U;
  uint8_t  lo  = 0U;
  ra_err_t err = cam_sccb_read((uint16_t)k_cam_reg_chip_id_hi, &hi);
  if (err != k_ra_ok) {
    return err;
  }
  err = cam_sccb_read((uint16_t)k_cam_reg_chip_id_lo, &lo);
  if (err != k_ra_ok) {
    return err;
  }
  *out_id = (uint16_t)(((uint16_t)hi << (uint16_t)k_cam_hi_byte_shift) | (uint16_t)lo);
  return k_ra_ok;
}

/* =============================================================================
 * Board / pin bring-up
 * =============================================================================
 */

/** @brief One CEU DVP pin: MCU port/pin routed to the CEU peripheral. */
typedef struct {
  ra_port_t port; /**< MCU port.                */
  ra_pin_t  pin;  /**< MCU pin within the port. */
} cam_ceu_pin_t;

/**
 * @var s_ceu_pins
 * @brief The 11 EK-RA8D2 J35 parallel-camera pins that feed the CEU:
 *        VIO_D[7:0], VIO_VD, VIO_HD, VIO_CLK (EK-RA8D2 UM Table 35 p 48).
 * @since 0.1.0
 */
static const cam_ceu_pin_t s_ceu_pins[] = {
  {k_ra_port_4, k_ra_pin_0},  /* D0    P400 (VIO_D0)  */
  {k_ra_port_9, k_ra_pin_2},  /* D1    P902 (VIO_D1)  */
  {k_ra_port_4, k_ra_pin_5},  /* D2    P405 (VIO_D2)  */
  {k_ra_port_4, k_ra_pin_6},  /* D3    P406 (VIO_D3)  */
  {k_ra_port_7, k_ra_pin_0},  /* D4    P700 (VIO_D4)  */
  {k_ra_port_7, k_ra_pin_1},  /* D5    P701 (VIO_D5)  */
  {k_ra_port_7, k_ra_pin_2},  /* D6    P702 (VIO_D6)  */
  {k_ra_port_7, k_ra_pin_3},  /* D7    P703 (VIO_D7)  */
  {k_ra_port_11, k_ra_pin_2}, /* VSYNC PB02 (VIO_VD)  */
  {k_ra_port_11, k_ra_pin_3}, /* HSYNC PB03 (VIO_HD)  */
  {k_ra_port_11, k_ra_pin_4}, /* PCLK  PB04 (VIO_CLK) */
};

/**
 * @brief Route the 11 J35 parallel-camera data/sync pins to the CEU.
 *
 * @return ra_err_t; first failing route or ok.
 * @retval k_ra_ok All pins routed to the CEU peripheral.
 * @retval k_ra_err_gpio_conflict A pin was already claimed.
 *
 * @pre `ra_pfs_init` context (IOPORT reachable).
 * @pre DIP SW4-6 is ON (board in parallel-camera mode).
 * @post All 11 pins carry the CEU (VIO_*) function (PMR=1, PSEL=CEU).
 * @post No CEU pin is left as GPIO.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra_err_t cam_route_ceu_pins(void)
{
  const uint32_t count = (uint32_t)(sizeof(s_ceu_pins) / sizeof(s_ceu_pins[0]));
  RA_CHECK_RANGE(count, 1U, 32U, k_ra_err_invalid_arg);
  for (uint32_t i = 0U; i < count; i += 1U) {
    const ra_port_pin_t pin = RA_PIN(s_ceu_pins[i].port, s_ceu_pins[i].pin);
    const ra_err_t      err = ra_pfs_route_peripheral(pin, k_ra_psel_ceu, "cam.ceu");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Start the ~24 MHz XVCLK on GTIOC12A (P501) via GPT channel 12.
 *
 * @details Programs a saw-wave PWM whose period divides PCLKD down to the
 *          XVCLK target, enables the GTIOC12A output, and routes P501 to
 *          the GPT peripheral. The OV5640 needs this clock before any
 *          SCCB access.
 *
 * @return ra_err_t; ok when the clock is toggling on P501.
 * @retval k_ra_ok XVCLK is running.
 * @retval k_ra_err_invalid_arg PCLKD readback gave an unusable divisor.
 *
 * @pre `ra_cgc_init` + `ra_mstp_init` have run.
 * @pre P501 is free (not muxed to the audio codec).
 * @post GPT12 counts and drives a ~24 MHz square wave on P501.
 * @post P501 carries the GTIOC12A function.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra_err_t cam_start_xclk(void)
{
  uint32_t pclkd_hz = 0U;
  ra_err_t err      = ra_cgc_get_clock_hz(k_ra_clock_id_pclkd, &pclkd_hz);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t period_counts = pclkd_hz / (uint32_t)k_cam_xclk_hz;
  RA_CHECK_RANGE(period_counts, 2U, (uint32_t)k_cam_gpt_period_max, k_ra_err_invalid_arg);
  const ra_gpt_cfg_t cfg = {
    .mode       = k_ra_gpt_mode_saw_pwm,
    .prescaler  = k_ra_gpt_ps_div_1,
    .period     = period_counts - 1U,
    .duty_a     = period_counts / 2U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  err = ra_gpt_init((uint8_t)k_cam_xclk_gpt_ch, &cfg);
  if (err != k_ra_ok) {
    return err;
  }
  const ra_gpt_pwm_pin_cfg_t pin = {
    .output_enable    = true,
    .polarity         = k_ra_gpt_pol_active_high,
    .stop_level       = k_ra_gpt_stop_low,
    .disable_on_fault = k_ra_gpt_disable_none,
  };
  err = ra_gpt_pwm_pin_configure((uint8_t)k_cam_xclk_gpt_ch, k_ra_gpt_pin_a, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(RA_PIN(k_ra_port_5, k_ra_pin_1), k_ra_psel_gpt0, "cam.xclk");
}

/**
 * @brief Pulse the OV5640 hardware reset strap (RST, P709, active-low).
 *
 * @return ra_err_t; ok when RST has been driven low then high.
 * @retval k_ra_ok Reset pulsed, sensor released.
 * @retval k_ra_err_gpio_conflict P709 was already claimed.
 *
 * @pre XVCLK is running (reset is sampled against it).
 * @pre P709 is free.
 * @post The sensor has seen a low->high reset edge.
 * @post P709 is driven high (sensor out of reset).
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra_err_t cam_reset_sensor(void)
{
  const ra_port_pin_t rst = RA_PIN(k_ra_port_7, k_ra_pin_9);
  ra_err_t            err = ra_gpio_output_init(rst, k_ra_level_low);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_cam_reset_low_ms);
  err = ra_gpio_write(rst, k_ra_level_high);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_cam_reset_high_ms);
  return k_ra_ok;
}

/* =============================================================================
 * Sensor configuration + capture
 * =============================================================================
 */

/**
 * @brief Software-reset the OV5640 and apply the DVP colour-bar sequence.
 *
 * @return ra_err_t; ok when every register write ACKed.
 * @retval k_ra_ok Sensor configured and woken.
 * @retval k_ra_err_nack A register write was NACKed.
 *
 * @pre RIIC ch1 up, XVCLK running, sensor out of hardware reset.
 * @pre The chip ID has been confirmed as 0x5640.
 * @post The sensor streams a QVGA YUV422 colour bar on the DVP bus.
 * @post The sensor is in normal (awake) mode.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t cam_configure_sensor(void)
{
  ra_err_t err = cam_sccb_write((uint16_t)k_cam_reg_sw_reset, (uint8_t)k_cam_sw_reset_hold);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_cam_swreset_ms);
  const uint32_t count = (uint32_t)(sizeof(s_ov5640_dvp_colorbar) / sizeof(cam_reg_t));
  for (uint32_t i = 0U; i < count; i += 1U) {
    err = cam_sccb_write(s_ov5640_dvp_colorbar[i].reg, s_ov5640_dvp_colorbar[i].val);
    if (err != k_ra_ok) {
      return err;
    }
  }
  err = cam_sccb_write((uint16_t)k_cam_reg_sw_reset, (uint8_t)k_cam_sw_reset_wake);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_cam_cfg_settle_ms);
  return k_ra_ok;
}

/**
 * @brief Fill the CEU open-time descriptor for a QVGA YUV422 raw grab.
 *
 * @param[out] cfg Descriptor to populate.
 * @return ra_err_t; ok on a valid fill.
 * @retval k_ra_ok Descriptor populated.
 * @retval k_ra_err_null_ptr `cfg` was NULL.
 *
 * @pre `cfg` is non-NULL.
 * @pre The sensor DVP timing matches QVGA / 2 bytes-per-pixel.
 * @post `cfg` requests a single-shot data-synchronous 8-bit capture.
 * @post `cfg->dst_stride` equals the byte width of one line.
 * @note Thread safety: pure population of `*cfg`.
 * @since 0.1.0
 */
static ra_err_t cam_fill_ceu_config(ra_ceu_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, "cam", "ceu_cfg");
  const ra_ceu_config_t seed = {
    /* Data-synchronous 8-bit fetch counts raw bus cycles, not pixels:
       CMCYR.HCYL must equal the byte-cycles per active line (pixels x 2
       for YUV422) or the CEU flags an HD line-count mismatch (IGHS) and
       writes zero bytes. So width == line bytes, matching x_capture_px. */
    .width_px        = (uint16_t)k_cam_line_bytes,
    .height_px       = (uint16_t)k_cam_height_px,
    .x_start_px      = 0U,
    .y_start_px      = 0U,
    .x_capture_px    = (uint16_t)k_cam_line_bytes,
    .y_capture_lines = (uint16_t)k_cam_height_px,
    .dst_stride      = (uint16_t)k_cam_line_bytes,
    .frame_drop      = 0U,
    .bytes_per_pixel = (uint8_t)k_cam_bytes_per_px,
    .interrupts      = 0U,
    .capture_format  = k_ra_ceu_fmt_data_synchronous,
    .capture_mode    = k_ra_ceu_capture_single,
    .data_bus        = k_ra_ceu_bus_8_bit,
    .hsync_polarity  = k_ra_ceu_pol_high_active,
    .vsync_polarity  = k_ra_ceu_pol_high_active,
    .field_polarity  = k_ra_ceu_pol_high_active,
    .input_order     = k_ra_ceu_input_y0_cb0_y1_cr0,
    .output_format   = k_ra_ceu_output_ycbcr_422,
    .burst_mode      = k_ra_ceu_burst_32,
    .first_field     = k_ra_ceu_field_immediate,
    .edge            = {k_ra_ceu_edge_rising,
                        k_ra_ceu_edge_rising,
                        k_ra_ceu_edge_rising,
                        k_ra_ceu_edge_rising},
    .byte_swap       = {false, false, false},
    .scale           = {0U, 0U, 0U, 0U, 0U, 0U},
    .interlace       = false,
    .one_field_only  = false,
    .bundle_write    = false,
    .low_pass_filter = false,
    .image_area_size = (uint32_t)k_cam_frame_bytes,
  };
  *cfg = seed;
  return k_ra_ok;
}

/**
 * @brief Capture one CEU frame into ::s_frame, polling CETCR.CPE.
 *
 * @return ra_err_t; ok when the CEU reports one-frame-end.
 * @retval k_ra_ok Frame captured.
 * @retval k_ra_err_hw_timeout No CPE within the bounded wait.
 *
 * @pre `ra_ceu_init` completed and the sensor is streaming.
 * @pre CEU DVP pins are routed.
 * @post ::s_frame holds one frame of DVP bytes on success.
 * @post The capture engine is idle (CE cleared).
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t cam_capture_one(void)
{
  ra_err_t err = ra_ceu_capture_arm(s_frame);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_cam_capture_tries; i += 1U) {
    uint32_t evt = 0U;
    err          = ra_ceu_get_status(&evt);
    if (err != k_ra_ok) {
      return err;
    }
    if ((evt & (uint32_t)k_ra_ceu_evt_cpe) != 0U) {
      (void)ra_ceu_clear_status((uint32_t)k_ra_ceu_evt_cpe);
      return k_ra_ok;
    }
    ra_delay_ms((uint32_t)k_cam_capture_poll_ms);
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Compute min / max / mean over ::s_frame and store to globals.
 *
 * @return true when the frame is non-degenerate (max != min).
 *
 * @pre ::s_frame holds a captured frame.
 * @pre ::k_cam_frame_bytes is non-zero.
 * @post `g_cam_min` / `g_cam_max` / `g_cam_mean` reflect the frame.
 * @post Return distinguishes a live grab from an all-identical buffer.
 * @note Thread safety: reads shared ::s_frame; call after capture only.
 * @since 0.1.0
 */
static bool cam_frame_is_plausible(void)
{
  const uint32_t n = (uint32_t)k_cam_frame_bytes;
  if (n == 0U) {
    return false;
  }
  uint32_t lo  = (uint32_t)k_cam_byte_mask;
  uint32_t hi  = 0U;
  uint32_t sum = 0U;
  for (uint32_t i = 0U; i < n; i += 1U) {
    const uint32_t b = (uint32_t)s_frame[i];
    if (b < lo) {
      lo = b;
    }
    if (b > hi) {
      hi = b;
    }
    sum += b;
  }
  g_cam_min  = lo;
  g_cam_max  = hi;
  g_cam_mean = sum / n;
  return hi != lo;
}

/* =============================================================================
 * Discovery
 * =============================================================================
 */

/**
 * @brief Probe every 7-bit I2C address on ch1 and print those that ACK.
 *
 * @details A discovery aid: emits ` scan=3C.43.` so the bench log shows
 *          exactly which devices answer on the SCCB bus (the OV5640 at
 *          0x3C/0x3D, the U15 expander at 0x43, ...). Address-only probes,
 *          no register access.
 *
 * @pre RIIC ch1 is initialized.
 * @pre The console is up.
 * @post One ` scan=...` field has been written for the run.
 * @post The bus is left idle.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_bus_scan(void)
{
  (void)cam_puts(" scan=");
  for (uint8_t a = (uint8_t)k_cam_scan_lo; a <= (uint8_t)k_cam_scan_hi; a += 1U) {
    bool acked = false;
    if (ra_i2c_scan((uint8_t)k_cam_iic_ch, a, &acked) != k_ra_ok) {
      continue;
    }
    if (!acked) {
      continue;
    }
    (void)cam_put_hex((uint32_t)a, 2U);
    /* Read register 0 (8-bit pointer) as a coarse device-ID fingerprint. */
    const uint8_t reg = 0U;
    uint8_t       v   = 0U;
    if (ra_i2c_transfer((uint8_t)k_cam_iic_ch, a, &reg, 1U, &v, 1U) == k_ra_ok) {
      (void)cam_puts(":");
      (void)cam_put_hex((uint32_t)v, 2U);
    }
    (void)cam_puts(".");
  }
}

/**
 * @brief Read the OV5640 chip ID, trying SCCB address 0x3C then 0x3D.
 *
 * @param[out] out_id Receives the last chip ID actually read (0 if none).
 * @return true when 0x5640 was read (and ::s_sccb_addr left at that address).
 *
 * @pre `out_id` is non-NULL.
 * @pre XVCLK is running and the sensor is out of hardware reset.
 * @post On true, ::s_sccb_addr is the address the OV5640 answered on.
 * @post On false, ::s_sccb_addr is restored to 0x3C.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static bool cam_probe_sensor(uint16_t* out_id)
{
  if (out_id == nullptr) {
    return false;
  }
  *out_id                = 0U;
  const uint8_t addrs[2] = {(uint8_t)k_cam_sccb_addr_7b, (uint8_t)k_cam_sccb_addr_alt};
  for (uint32_t i = 0U; i < (uint32_t)sizeof(addrs); i += 1U) {
    s_sccb_addr   = addrs[i];
    uint16_t id   = 0U;
    ra_err_t rerr = cam_read_chip_id(&id);
    if (rerr == k_ra_ok) {
      *out_id = id;
      if (id == (uint16_t)k_cam_chip_id_ov5640) {
        return true;
      }
    }
  }
  s_sccb_addr = (uint8_t)k_cam_sccb_addr_7b;
  return false;
}

/* =============================================================================
 * Orchestration
 * =============================================================================
 */

/** @brief Park forever after the self-test has reported its verdict.
 *  @pre The verdict banner has been emitted.
 *  @post The CPU idles; only reset or a debugger wakes it.
 *  @since 0.1.0 */
static void cam_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Force the Camera Expansion Board into parallel (DVP) mode.
 *
 * @details The board multiplexes the camera FFC (J35) between MIPI-CSI
 *          (SW4-6 OFF, the board default) and parallel DVP (SW4-6 ON) via
 *          the U15 PI4IOE5V6408 SW4-override expander. In MIPI mode the
 *          P501 XCLK is not routed to the sensor, so the OV5640 stays
 *          unclocked and never answers SCCB. This clears U15 output bit 5
 *          (SW4-6 -> ON) so the DVP path -- and the P501 XVCLK -- reach the
 *          sensor, without disturbing the other SW4 overrides.
 *
 * @return ra_err_t; ok when U15 latched SW4-6 = ON.
 * @retval k_ra_ok Parallel-camera routing selected.
 * @retval k_ra_err_nack U15 did not ACK.
 *
 * @pre RIIC ch1 up and `ra_board_io_expander_apply_project_sw4_defaults`
 *      has configured U15 as all-outputs.
 * @pre The Camera Expansion Board is on J35.
 * @post SW4-6 reads ON; the DVP data/clock path is live.
 * @post U15's other SW4 overrides are unchanged.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra_err_t cam_select_parallel_camera(void)
{
  const uint8_t reg = (uint8_t)k_cam_u15_reg_output;
  uint8_t       cur = 0U;
  ra_err_t      err =
    ra_i2c_transfer((uint8_t)k_cam_iic_ch, (uint8_t)k_cam_u15_addr, &reg, 1U, &cur, 1U);
  if (err != k_ra_ok) {
    return err;
  }
  const uint8_t want       = (uint8_t)(cur & (uint8_t)(~(1U << (uint8_t)k_cam_sw46_bit)));
  const uint8_t payload[2] = {reg, want};
  return ra_i2c_write((uint8_t)k_cam_iic_ch,
                      (uint8_t)k_cam_u15_addr,
                      payload,
                      (uint32_t)sizeof(payload),
                      true);
}

/**
 * @brief Bring up clocks, console, XCLK, SCCB and select parallel camera.
 *
 * @return ra_err_t from the first failing bring-up step, or ok.
 * @retval k_ra_ok Console, XCLK and I2C ch1 are live; SW4-6 = ON.
 *
 * @pre Reset_Handler + SystemInit ran.
 * @pre Single-threaded init context.
 * @post On ok the console prints and the OV5640 has XVCLK + SCCB.
 * @post On ok the Camera Expansion Board is in parallel (DVP) mode.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra_err_t cam_bringup(void)
{
  uint32_t cpuclk0_hz = 0U;
  ra_err_t err        = ra_cgc_init();
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_mstp_init();
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_time_init(cpuclk0_hz);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_board_uart_console_init((uint32_t)k_cam_baud);
  if (err != k_ra_ok) {
    return err;
  }
  err = cam_start_xclk();
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_board_io_expander_apply_project_sw4_defaults();
  if (err != k_ra_ok) {
    return err;
  }
  /* The board default is SW4-6 OFF (MIPI camera); flip it ON so the DVP
     path + the P501 XVCLK reach the sensor. */
  err = cam_select_parallel_camera();
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_cam_cfg_settle_ms);
  return k_ra_ok;
}

/**
 * @brief Configure the sensor, capture one frame, and print the verdict.
 *
 * @details Called only after the OV5640 chip ID is confirmed. Emits the
 *          ` ceu= frame= min= max= mean= verdict=` banner fields and sets
 *          the result globals. PASS requires a captured, non-degenerate
 *          frame.
 *
 * @pre `cam_probe_sensor` returned true (SCCB reaches the OV5640).
 * @pre The CEU DVP pins are routed and the sensor is out of reset.
 * @post One verdict field has been written to SCI8.
 * @post `g_cam_frame_ok` / `g_cam_verdict` reflect the outcome.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_capture_and_verdict(void)
{
  ra_err_t        err = cam_configure_sensor();
  ra_ceu_config_t ceu = {};
  if (err == k_ra_ok) {
    err = cam_fill_ceu_config(&ceu);
  }
  if (err == k_ra_ok) {
    err = ra_ceu_init(&ceu);
  }
  (void)cam_puts(" ceu=");
  (void)cam_puts((err == k_ra_ok) ? "OK" : "ERR");

  bool frame_ok = false;
  if (err == k_ra_ok) {
    frame_ok = (cam_capture_one() == k_ra_ok);
  }
  g_cam_frame_ok = frame_ok ? 1U : 0U;
  (void)cam_puts(" frame=");
  (void)cam_puts(frame_ok ? "OK" : "TIMEOUT");

  /* Emit the raw CEU event register (CETCR) so a timeout is diagnosable
     from the banner alone: bit17=IGHS (HD cycle-count mismatch), bit20=VBP
     (invalid VD), bit24/25=NHD/NVD (sync missing), bit0=CPE (frame done). */
  uint32_t cetcr = 0U;
  (void)ra_ceu_get_status(&cetcr);
  (void)cam_puts(" cetcr=");
  (void)cam_put_hex(cetcr, 8U);

  bool plausible = false;
  if (frame_ok) {
    plausible = cam_frame_is_plausible();
  }
  (void)cam_puts(" min=");
  (void)cam_put_u32(g_cam_min);
  (void)cam_puts(" max=");
  (void)cam_put_u32(g_cam_max);
  (void)cam_puts(" mean=");
  (void)cam_put_u32(g_cam_mean);

  g_cam_verdict = plausible ? 1U : 0U;
  (void)cam_puts(plausible ? " verdict=PASS\r\n" : " verdict=FAIL\r\n");
}

/**
 * @brief Run the capture pipeline and emit the plausibility banner.
 *
 * @details Reports progressively so a single flash reveals exactly which
 *          stage the hardware reached. The verdict is PASS only when the
 *          sensor ID is 0x5640, a frame was captured, and it is
 *          non-degenerate.
 *
 * @pre `cam_bringup` returned ok (console + XCLK + SCCB live).
 * @post A `cam: ...verdict=...` line has been written to SCI8.
 * @post The result globals reflect the run.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_run(void)
{
  (void)cam_puts((const char*)k_cam_tag);

  /* Confirm the XCLK GPT is actually counting (a proxy that the P501
     XVCLK square wave is live). The GTCNT wraps every few counts at the
     ~24 MHz output rate, so sample it in a tight loop and call it live as
     soon as any read differs from the first; a stuck GTCNT (every sample
     identical) means the sensor never gets a clock. */
  uint32_t cnt_ref     = 0U;
  bool     gpt_running = false;
  (void)ra_gpt_read((uint8_t)k_cam_xclk_gpt_ch, &cnt_ref);
  for (uint32_t i = 0U; i < (uint32_t)k_cam_gpt_probe_iters; i += 1U) {
    uint32_t cnt = 0U;
    (void)ra_gpt_read((uint8_t)k_cam_xclk_gpt_ch, &cnt);
    if (cnt != cnt_ref) {
      gpt_running = true;
      break;
    }
  }
  (void)cam_puts(" gpt=");
  (void)cam_puts(gpt_running ? "RUN" : "DEAD");

  /* The OV5640 needs XVCLK (up in cam_bringup) and must be out of
     hardware reset before it answers on SCCB, so route the DVP pins and
     release RST (P709) FIRST, then probe. */
  (void)cam_route_ceu_pins();
  const ra_err_t rst_err = cam_reset_sensor();

  cam_bus_scan();

  uint16_t   chip  = 0U;
  const bool id_ok = cam_probe_sensor(&chip);
  g_cam_chipid     = (uint32_t)chip;
  (void)cam_puts(" chipid=");
  (void)cam_put_hex((uint32_t)chip, 4U);
  (void)cam_puts(" xclk=OK rst=");
  (void)cam_puts((rst_err == k_ra_ok) ? "OK" : "ERR");
  (void)cam_puts(" sccb=");
  (void)cam_puts(id_ok ? "OK" : "ERR");

  if (!id_ok) {
    (void)cam_puts(" verdict=FAIL\r\n");
    return;
  }

  cam_capture_and_verdict();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: OV5640 CEU capture plausibility self-test.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU and priority grouping.
 * @post The verdict banner is on SCI8 and the CPU parks.
 * @post The result globals hold the outcome for SWD probing.
 * @since 0.1.0
 */
int32_t main(void)
{
  if (cam_bringup() != k_ra_ok) {
    (void)cam_puts("cam: bringup ERROR verdict=FAIL\r\n");
    cam_park();
    return 0;
  }
  ra_isr_globals_enable();
  cam_run();
  cam_park();
  return 0;
}
#pragma GCC diagnostic pop
