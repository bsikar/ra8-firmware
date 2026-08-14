/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/src/cam_ceu.c
 * @brief CEU parallel (DVP) capture: DVP pin routing, open, arm/poll, buffer.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements the Capture Engine Unit half of the camera self-test declared in
 * `cam_ceu.h`. Owns the 11-entry DVP pin map, the VGA YUV422 open-time
 * descriptor, the cache-line-aligned packed-frame capture buffer, and the arm +
 * bounded-poll capture wrapper. The raw `ra8_ceu_*` driver is wrapped so the
 * app never touches the ::ra8_ceu_config_t descriptor.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "cam_ceu.h"

#include <stdint.h>

#include "ra8_cache.h"
#include "ra8_ceu.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_port_regs.h"
#include "ra8_port_utils.h"
#include "ra8_systick.h"
#include "ra8_time.h"

/* =============================================================================
 * CEU capture constants (typed enums -- no magic numbers)
 * =============================================================================
 */

/** @brief VGA capture geometry the CEU descriptor is built from. */
typedef enum : uint16_t {
  k_cam_width_px   = 640U,  /**< Captured frame width (VGA).  */
  k_cam_height_px  = 480U,  /**< Captured frame height (VGA). */
  k_cam_line_bytes = 1280U, /**< Bytes per packed input line. */
} cam_ceu_geom_t;

/** @brief Cache-line alignment for the CEU frame buffer. */
typedef enum : uint32_t {
  k_cam_cache_line_bytes = 32U,
} cam_ceu_buffer_t;

/** @brief Bytes-per-pixel for the packed YUV422 / RGB565 output. */
typedef enum : uint8_t {
  k_cam_bytes_per_px = 2U, /**< YUV422 / RGB565 -> 2 bytes/pixel. */
} cam_ceu_px_t;

/** @brief Bounded capture-completion poll (step + attempt cap). */
typedef enum : uint32_t {
  k_cam_capture_poll_ms = 5U,       /**< Poll step while awaiting CETCR.CPE.       */
  k_cam_capture_tries   = 800U,     /**< Bounded capture wait (~4 s at slow PCLK). */
  k_cam_sync_samples    = 2000000U, /**< GPIO samples for live-sync diagnostic.    */
  k_cam_data_d2_pin_bit = 5U,       /**< P405 bit position for camera D2.          */
  k_cam_data_d5_out_bit = 5U,       /**< Reconstructed byte bit for camera D5.     */
  k_cam_data_d7_out_bit = 7U,       /**< Reconstructed byte bit for camera D7.     */
  k_cam_long_line_pclk  = 1024U,    /**< Diagnostic threshold for long HREF lines. */
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
 * @brief Capture target for one VGA packed YCbCr 4:2:2 frame (internal SRAM).
 * @details Cache-line aligned so CEU writes can be invalidated without
 *          dropping unrelated dirty bytes from adjacent objects. This also
 *          exceeds the 8-byte CDAYR alignment required by HUM Ch 60.2.13.
 * @warning Written by the CEU bus initiator; do not touch mid-capture.
 * @since 0.1.0
 */
alignas(k_cam_cache_line_bytes) static uint8_t s_frame[k_cam_frame_bytes];
static uint32_t s_capture_bytes;

/* =============================================================================
 * CEU descriptor
 * =============================================================================
 */

/**
 * @brief Fill the CEU open-time descriptor for a VGA YUV422 raw grab.
 *
 * @param[out] cfg Descriptor to populate.
 * @return ra8_err_t; ok on a valid fill.
 * @retval k_ra8_ok Descriptor populated.
 * @retval k_ra8_err_null_ptr `cfg` was NULL.
 *
 * @pre `cfg` is non-NULL.
 * @pre The sensor DVP timing matches VGA / 2 bytes-per-pixel.
 * @post `cfg` requests a single-shot data-synchronous 8-bit capture.
 * @post `cfg->dst_stride` equals the packed byte width of one output line.
 * @note Thread safety: pure population of `*cfg`.
 * @since 0.1.0
 */
static ra8_err_t cam_fill_ceu_config(ra8_ceu_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, "cam", "ceu_cfg");
  const ra8_ceu_config_t seed = {
    .width_px        = (uint16_t)k_cam_width_px,
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
    .input_order     = k_ra8_ceu_input_cb0_y0_cr0_y1,
    .output_format   = k_ra8_ceu_output_ycbcr_422,
    .burst_mode      = k_ra8_ceu_burst_32,
    .first_field     = k_ra8_ceu_field_immediate,
    .edge            = {k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising,
                        k_ra8_ceu_edge_rising},
    .byte_swap       = {false, true, true},
    .scale           = {0U, 0U, 0U, 0U, (uint16_t)k_cam_width_px, (uint16_t)k_cam_height_px},
    .interlace       = false,
    .one_field_only  = false,
    .bundle_write    = false,
    .low_pass_filter = false,
    .image_area_size = 0U,
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

ra8_err_t cam_probe_sync_activity(cam_ceu_sync_probe_t* out_probe)
{
  RA8_CHECK_NULL_PTR(out_probe, "cam", "sync_probe");
  const uint32_t pin_count = (uint32_t)(sizeof(s_ceu_pins) / sizeof(s_ceu_pins[0]));
  uint32_t       claimed   = 0U;
  for (; claimed < pin_count; claimed += 1U) {
    const ra8_port_pin_t pin = RA8_PIN(s_ceu_pins[claimed].port, s_ceu_pins[claimed].pin);
    const ra8_err_t      err = ra8_gpio_input_init(pin, k_ra8_pull_none);
    if (err != k_ra8_ok) {
      break;
    }
  }
  if (claimed != pin_count) {
    while (claimed > 0U) {
      claimed -= 1U;
      (void)ra8_gpio_release(RA8_PIN(s_ceu_pins[claimed].port, s_ceu_pins[claimed].pin));
    }
    return k_ra8_err_gpio_conflict;
  }
  volatile r_port_regs_t* const port4  = ra8_port(k_ra8_port_4);
  volatile r_port_regs_t* const port7  = ra8_port(k_ra8_port_7);
  volatile r_port_regs_t* const port9  = ra8_port(k_ra8_port_9);
  volatile r_port_regs_t* const port11 = ra8_port(k_ra8_port_11);
  if ((port4 == nullptr) || (port7 == nullptr) || (port9 == nullptr) || (port11 == nullptr)) {
    for (uint32_t i = 0U; i < pin_count; i += 1U) {
      (void)ra8_gpio_release(RA8_PIN(s_ceu_pins[i].port, s_ceu_pins[i].pin));
    }
    return k_ra8_err_hw_error;
  }
  const uint32_t sync_mask       = (1UL << (uint32_t)k_ra8_pin_2) | (1UL << (uint32_t)k_ra8_pin_3) |
                                   (1UL << (uint32_t)k_ra8_pin_4);
  uint32_t       prior           = port11->PCNTR2 & sync_mask;
  uint32_t       vsync_edges     = 0U;
  uint32_t       hsync_edges     = 0U;
  uint32_t       high_min        = UINT32_MAX;
  uint32_t       high_max        = 0U;
  uint32_t       low_min         = UINT32_MAX;
  uint32_t       low_max         = 0U;
  uint32_t       high_cycles_min = UINT32_MAX;
  uint32_t       high_cycles_max = 0U;
  uint32_t       pclk_edges      = 0U;
  uint32_t       pclk_half_cycles_min = UINT32_MAX;
  uint32_t       run_samples          = 1U;
  uint32_t       run_start_cycles     = ra8_dwt_cyccnt_read();
  uint32_t       prior_pclk_cycles    = run_start_cycles;
  uint32_t       data_samples         = 0U;
  uint32_t       data_changes         = 0U;
  uint8_t        data_min             = UINT8_MAX;
  uint8_t        data_max             = 0U;
  uint8_t        data_and             = UINT8_MAX;
  uint8_t        data_or              = 0U;
  uint8_t        prior_data           = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_cam_sync_samples; i += 1U) {
    const uint32_t current = port11->PCNTR2 & sync_mask;
    vsync_edges += (((current ^ prior) & (1UL << (uint32_t)k_ra8_pin_2)) != 0U) ? 1U : 0U;
    hsync_edges += (((current ^ prior) & (1UL << (uint32_t)k_ra8_pin_3)) != 0U) ? 1U : 0U;
    if (((current ^ prior) & (1UL << (uint32_t)k_ra8_pin_4)) != 0U) {
      const uint32_t current_cycles = ra8_dwt_cyccnt_read();
      const uint32_t half_period    = current_cycles - prior_pclk_cycles;
      pclk_edges += 1U;
      pclk_half_cycles_min =
        (half_period < pclk_half_cycles_min) ? half_period : pclk_half_cycles_min;
      prior_pclk_cycles = current_cycles;
      if (((current & (1UL << (uint32_t)k_ra8_pin_4)) != 0U) &&
          ((current & (1UL << (uint32_t)k_ra8_pin_3)) != 0U)) {
        const uint32_t p4   = port4->PCNTR2;
        const uint32_t p7   = port7->PCNTR2;
        const uint32_t p9   = port9->PCNTR2;
        const uint8_t  data = (uint8_t)(((p4 >> 0U) & 1U) | (((p9 >> 2U) & 1U) << 1U) |
                                        (((p4 >> (uint32_t)k_cam_data_d2_pin_bit) & 1U) << 2U) |
                                        (((p4 >> 6U) & 1U) << 3U) | (((p7 >> 0U) & 1U) << 4U) |
                                        (((p7 >> 1U) & 1U) << (uint32_t)k_cam_data_d5_out_bit) |
                                        (((p7 >> 2U) & 1U) << 6U) |
                                        (((p7 >> 3U) & 1U) << (uint32_t)k_cam_data_d7_out_bit));
        if ((data_samples != 0U) && (data != prior_data)) {
          data_changes += 1U;
        }
        data_min   = (data < data_min) ? data : data_min;
        data_max   = (data > data_max) ? data : data_max;
        data_and   = (uint8_t)(data_and & data);
        data_or    = (uint8_t)(data_or | data);
        prior_data = data;
        data_samples += 1U;
      }
    }
    if ((current & (1UL << (uint32_t)k_ra8_pin_3)) == (prior & (1UL << (uint32_t)k_ra8_pin_3))) {
      run_samples += 1U;
    } else if ((prior & (1UL << (uint32_t)k_ra8_pin_3)) != 0U) {
      const uint32_t run_cycles = ra8_dwt_cyccnt_read() - run_start_cycles;
      high_min                  = (run_samples < high_min) ? run_samples : high_min;
      high_max                  = (run_samples > high_max) ? run_samples : high_max;
      high_cycles_min           = (run_cycles < high_cycles_min) ? run_cycles : high_cycles_min;
      high_cycles_max           = (run_cycles > high_cycles_max) ? run_cycles : high_cycles_max;
      run_samples               = 1U;
      run_start_cycles          = ra8_dwt_cyccnt_read();
    } else {
      low_min          = (run_samples < low_min) ? run_samples : low_min;
      low_max          = (run_samples > low_max) ? run_samples : low_max;
      run_samples      = 1U;
      run_start_cycles = ra8_dwt_cyccnt_read();
    }
    prior = current;
  }
  uint32_t measured_lines = 0U;
  uint32_t line_pclk_min  = UINT32_MAX;
  uint32_t line_pclk_max  = 0U;
  uint32_t line_pclk_sum  = 0U;
  uint32_t line_pclk_long = 0U;
  uint32_t line_pclk      = 0U;
  prior                   = port11->PCNTR2 & sync_mask;
  for (uint32_t i = 0U;
       (i < (uint32_t)k_cam_sync_samples) && ((prior & (1UL << (uint32_t)k_ra8_pin_3)) != 0U);
       i += 1U) {
    prior = port11->PCNTR2 & sync_mask;
  }
  for (uint32_t i = 0U;
       (i < (uint32_t)k_cam_sync_samples) && ((prior & (1UL << (uint32_t)k_ra8_pin_3)) == 0U);
       i += 1U) {
    prior = port11->PCNTR2 & sync_mask;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_cam_sync_samples; i += 1U) {
    const uint32_t current = port11->PCNTR2 & sync_mask;
    const uint32_t changed = current ^ prior;
    if (((changed & (1UL << (uint32_t)k_ra8_pin_4)) != 0U) &&
        ((current & (1UL << (uint32_t)k_ra8_pin_4)) != 0U) &&
        ((current & (1UL << (uint32_t)k_ra8_pin_3)) != 0U)) {
      line_pclk += 1U;
    }
    if (((changed & (1UL << (uint32_t)k_ra8_pin_3)) != 0U) &&
        ((current & (1UL << (uint32_t)k_ra8_pin_3)) == 0U)) {
      line_pclk_min = (line_pclk < line_pclk_min) ? line_pclk : line_pclk_min;
      line_pclk_max = (line_pclk > line_pclk_max) ? line_pclk : line_pclk_max;
      line_pclk_sum += line_pclk;
      line_pclk_long += (line_pclk > (uint32_t)k_cam_long_line_pclk) ? 1U : 0U;
      measured_lines += 1U;
      line_pclk = 0U;
    }
    prior = current;
  }
  for (uint32_t i = 0U; i < pin_count; i += 1U) {
    (void)ra8_gpio_release(RA8_PIN(s_ceu_pins[i].port, s_ceu_pins[i].pin));
  }
  out_probe->vsync_edges           = vsync_edges;
  out_probe->hsync_edges           = hsync_edges;
  out_probe->hsync_high_min        = (high_min == UINT32_MAX) ? 0U : high_min;
  out_probe->hsync_high_max        = high_max;
  out_probe->hsync_low_min         = (low_min == UINT32_MAX) ? 0U : low_min;
  out_probe->hsync_low_max         = low_max;
  out_probe->hsync_high_cycles_min = (high_cycles_min == UINT32_MAX) ? 0U : high_cycles_min;
  out_probe->hsync_high_cycles_max = high_cycles_max;
  out_probe->pclk_edges            = pclk_edges;
  out_probe->pclk_half_cycles_min =
    (pclk_half_cycles_min == UINT32_MAX) ? 0U : pclk_half_cycles_min;
  out_probe->data_samples   = data_samples;
  out_probe->data_changes   = data_changes;
  out_probe->measured_lines = measured_lines;
  out_probe->line_pclk_min  = (line_pclk_min == UINT32_MAX) ? 0U : line_pclk_min;
  out_probe->line_pclk_max  = line_pclk_max;
  out_probe->line_pclk_mean = (measured_lines == 0U) ? 0U : (line_pclk_sum / measured_lines);
  out_probe->line_pclk_long = line_pclk_long;
  out_probe->data_min       = (data_min == UINT8_MAX) ? 0U : data_min;
  out_probe->data_max       = data_max;
  out_probe->data_and       = (data_samples == 0U) ? 0U : data_and;
  out_probe->data_or        = data_or;
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
  s_capture_bytes = 0U;
  ra8_err_t err   = ra8_cache_dcache_clean_invalidate_by_addr(s_frame, (uint32_t)k_cam_frame_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_ceu_buffers_t buffers = {
    .y_top             = s_frame,
    .c_top             = nullptr,
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = nullptr,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };
  err = ra8_ceu_capture_start_ex(&buffers);
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
      err = ra8_cache_dcache_invalidate_by_addr(s_frame, (uint32_t)k_cam_frame_bytes);
      if (err != k_ra8_ok) {
        return err;
      }
      s_capture_bytes = (uint32_t)k_cam_frame_bytes;
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

uint32_t cam_ceu_capture_bytes(void)
{
  return s_capture_bytes;
}
