/**
 * @file examples/ek_ra8d2/hw_pending/camera_capture/src/cam_ceu.c
 * @brief CEU parallel (DVP) capture: DVP pin routing, open, arm/poll, buffer.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements the Capture Engine Unit half of the camera self-test declared in
 * `cam_ceu.h`. Owns the 11-entry DVP pin map, the QVGA YUV422 open-time
 * descriptor, the 8-byte-aligned single-frame capture buffer, and the arm +
 * bounded-poll capture wrapper. The raw `ra8_ceu_*` driver is wrapped so the
 * app never touches the ::ra8_ceu_config_t descriptor.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "cam_ceu.h"

#include <stdint.h>

#include "ra8_ceu.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/* =============================================================================
 * CEU capture constants (typed enums -- no magic numbers)
 * =============================================================================
 */

/** @brief QVGA capture geometry the CEU descriptor is built from. */
typedef enum : uint16_t {
  k_cam_width_px   = 320U, /**< Captured frame width (QVGA).  */
  k_cam_height_px  = 240U, /**< Captured frame height (QVGA). */
  k_cam_line_bytes = 640U, /**< Bytes per line (width x 2).   */
} cam_ceu_geom_t;

/** @brief Bytes-per-pixel for the packed YUV422 / RGB565 output. */
typedef enum : uint8_t {
  k_cam_bytes_per_px = 2U, /**< YUV422 / RGB565 -> 2 bytes/pixel. */
} cam_ceu_px_t;

/** @brief Bounded capture-completion poll (step + attempt cap). */
typedef enum : uint32_t {
  k_cam_capture_poll_ms = 5U,   /**< Poll step while awaiting CETCR.CPE. */
  k_cam_capture_tries   = 400U, /**< Bounded capture wait (~2 s).        */
} cam_ceu_poll_t;

/** @brief One CEU DVP pin: MCU port/pin routed to the CEU peripheral. */
typedef struct {
  ra8_port_t port; /**< MCU port.                */
  ra8_pin_t  pin;  /**< MCU pin within the port. */
} cam_ceu_pin_t;

/**
 * @var s_ceu_pins
 * @brief The 11 EK-RA8D2 J35 parallel-camera pins that feed the CEU:
 *        VIO_D[7:0], VIO_VD, VIO_HD, VIO_CLK (EK-RA8D2 UM Table 35 p 48).
 * @since 0.1.0
 */
static const cam_ceu_pin_t s_ceu_pins[] = {
  {k_ra8_port_4, k_ra8_pin_0},  /* D0    P400 (VIO_D0)  */
  {k_ra8_port_9, k_ra8_pin_2},  /* D1    P902 (VIO_D1)  */
  {k_ra8_port_4, k_ra8_pin_5},  /* D2    P405 (VIO_D2)  */
  {k_ra8_port_4, k_ra8_pin_6},  /* D3    P406 (VIO_D3)  */
  {k_ra8_port_7, k_ra8_pin_0},  /* D4    P700 (VIO_D4)  */
  {k_ra8_port_7, k_ra8_pin_1},  /* D5    P701 (VIO_D5)  */
  {k_ra8_port_7, k_ra8_pin_2},  /* D6    P702 (VIO_D6)  */
  {k_ra8_port_7, k_ra8_pin_3},  /* D7    P703 (VIO_D7)  */
  {k_ra8_port_11, k_ra8_pin_2}, /* VSYNC PB02 (VIO_VD)  */
  {k_ra8_port_11, k_ra8_pin_3}, /* HSYNC PB03 (VIO_HD)  */
  {k_ra8_port_11, k_ra8_pin_4}, /* PCLK  PB04 (VIO_CLK) */
};

/**
 * @var s_frame
 * @brief Capture target for one QVGA YUV422 frame (internal SRAM).
 * @details 8-byte aligned per HUM Ch 60.2.13 p 3656 (CDAYR alignment).
 * @warning Written by the CEU bus initiator; do not touch mid-capture.
 * @since 0.1.0
 */
alignas(8) static uint8_t s_frame[k_cam_frame_bytes];

/* =============================================================================
 * CEU descriptor
 * =============================================================================
 */

/**
 * @brief Fill the CEU open-time descriptor for a QVGA YUV422 raw grab.
 *
 * @param[out] cfg Descriptor to populate.
 * @return ra8_err_t; ok on a valid fill.
 * @retval k_ra8_ok Descriptor populated.
 * @retval k_ra8_err_null_ptr `cfg` was NULL.
 *
 * @pre `cfg` is non-NULL.
 * @pre The sensor DVP timing matches QVGA / 2 bytes-per-pixel.
 * @post `cfg` requests a single-shot data-synchronous 8-bit capture.
 * @post `cfg->dst_stride` equals the byte width of one line.
 * @note Thread safety: pure population of `*cfg`.
 * @since 0.1.0
 */
static ra8_err_t cam_fill_ceu_config(ra8_ceu_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, "cam", "ceu_cfg");
  const ra8_ceu_config_t seed = {
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
    .capture_format  = k_ra8_ceu_fmt_data_synchronous,
    .capture_mode    = k_ra8_ceu_capture_single,
    .data_bus        = k_ra8_ceu_bus_8_bit,
    .hsync_polarity  = k_ra8_ceu_pol_high_active,
    .vsync_polarity  = k_ra8_ceu_pol_high_active,
    .field_polarity  = k_ra8_ceu_pol_high_active,
    .input_order     = k_ra8_ceu_input_y0_cb0_y1_cr0,
    .output_format   = k_ra8_ceu_output_ycbcr_422,
    .burst_mode      = k_ra8_ceu_burst_32,
    .first_field     = k_ra8_ceu_field_immediate,
    .edge            = {k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising},
    .byte_swap       = {false, false, false},
    .scale           = {0U, 0U, 0U, 0U, 0U, 0U},
    .interlace       = false,
    .one_field_only  = false,
    .bundle_write    = false,
    .low_pass_filter = false,
    .image_area_size = (uint32_t)k_cam_frame_bytes,
  };
  *cfg = seed;
  return k_ra8_ok;
}

/* =============================================================================
 * Capture pipeline (public -- contracts in cam_ceu.h)
 * =============================================================================
 */

ra8_err_t cam_route_ceu_pins(void)
{
  const uint32_t count = (uint32_t)(sizeof(s_ceu_pins) / sizeof(s_ceu_pins[0]));
  RA8_CHECK_RANGE(count, 1U, 32U, k_ra8_err_invalid_arg);
  for (uint32_t i = 0U; i < count; i += 1U) {
    const ra8_port_pin_t pin = RA8_PIN(s_ceu_pins[i].port, s_ceu_pins[i].pin);
    const ra8_err_t      err = ra8_pfs_route_peripheral(pin, k_ra8_psel_ceu, "cam.ceu");
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

ra8_err_t cam_ceu_setup(void)
{
  ra8_ceu_config_t cfg = {};
  ra8_err_t        err = cam_fill_ceu_config(&cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_ceu_init(&cfg);
}

ra8_err_t cam_capture_one(void)
{
  ra8_err_t err = ra8_ceu_capture_arm(s_frame);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_cam_capture_tries; i += 1U) {
    uint32_t evt = 0U;
    err          = ra8_ceu_get_status(&evt);
    if (err != k_ra8_ok) {
      return err;
    }
    if ((evt & (uint32_t)k_ra8_ceu_evt_cpe) != 0U) {
      (void)ra8_ceu_clear_status((uint32_t)k_ra8_ceu_evt_cpe);
      return k_ra8_ok;
    }
    ra8_delay_ms((uint32_t)k_cam_capture_poll_ms);
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t cam_ceu_get_status(uint32_t* out_evt)
{
  RA8_CHECK_NULL_PTR(out_evt, "cam", "ceu_status");
  return ra8_ceu_get_status(out_evt);
}

const uint8_t* cam_ceu_frame(void)
{
  return s_frame;
}
