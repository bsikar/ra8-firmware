/**
 * @file ns_pal_shims.c
 * @brief Non-Secure RTOS timebase and NSC adapter shims for ``ra8_display_pal``.
 * @ingroup grp_ereader
 *
 * @details
 * Provides ThreadX-backed ``ra8_delay_ms()`` / ``ra8_time_ms()`` (avoiding any
 * SysTick conflict with ThreadX) and forwards the board/GLCDC/cache hooks
 * imported by ``libra8_display_pal.a`` across the TrustZone NSC boundary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_cache.h"
#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_imgdec.h"
#include "ra8_jpeg_imgdec.h"
#include "ra8_nsc.h"
#include "ra8_nsc_io.h"
#include "ra8_time.h"
#include "ra8_webp_imgdec.h"
#include "tx_api.h"

/**
 * @brief Non-Secure millisecond sleep backed by ThreadX 1 ms tick.
 *
 * @param[in] ms Milliseconds to sleep (0 sleeps 1 tick).
 * @since 0.1.0
 */
void ra8_delay_ms(uint32_t ms)
{
  (void)tx_thread_sleep((ms == 0U) ? 1UL : (ULONG)ms);
}

/**
 * @brief Non-Secure monotonic millisecond timestamp backed by ThreadX.
 *
 * @return Kernel ticks elapsed since scheduler start.
 * @since 0.1.0
 */
uint32_t ra8_time_ms(void)
{
  return (uint32_t)tx_time_get();
}

/**
 * @brief NS adapter driving J1 panel reset pulse + BLEN via short NSC GPIO calls.
 *
 * @details Sleeps between GPIO edges in Non-Secure ThreadX context instead of
 *          blocking inside Secure state for 100 ms across RTOS ticks.
 */
ra8_err_t ra8_board_lcd_panel_power_on(void)
{
  constexpr uint32_t k_reset_pulse_ms = 50U;
  ra8_err_t          err              = ra8_nsc_lcd_panel_reset_init(false);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_reset_pulse_ms);

  err = ra8_nsc_lcd_panel_reset_write(true);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_reset_pulse_ms);

  return ra8_nsc_backlight_init(true);
}

/** @brief NS adapter forwarding J1 GLCDC pin routing across the NSC boundary. */
ra8_err_t ra8_board_glcdc_init(ra8_board_glcdc_fmt_t fmt)
{
  return ra8_nsc_board_glcdc_init((uint8_t)fmt);
}

/** @brief NS adapter forwarding GLCDC config across the NSC boundary. */
ra8_err_t ra8_glcdc_init(const ra8_glcdc_config_t* cfg)
{
  return ra8_nsc_glcdc_init(cfg);
}

/** @brief NS adapter forwarding GLCDC background colour across the NSC boundary. */
ra8_err_t ra8_glcdc_set_background_color(uint32_t argb)
{
  return ra8_nsc_glcdc_set_background_color(argb);
}

/** @brief NS adapter forwarding GLCDC scan start/stop across the NSC boundary. */
ra8_err_t ra8_glcdc_start(bool enable)
{
  return ra8_nsc_glcdc_start(enable);
}

/** @brief NS adapter forwarding GLCDC layer 1 address across the NSC boundary. */
ra8_err_t ra8_glcdc_layer1_show(uintptr_t fb_addr)
{
  return ra8_nsc_glcdc_layer1_show(fb_addr);
}

/** @brief NS adapter forwarding GLCDC deinit across the NSC boundary. */
ra8_err_t ra8_glcdc_deinit(void)
{
  return ra8_nsc_glcdc_deinit();
}

/** @brief NS adapter forwarding D-cache clean across the NSC boundary. */
ra8_err_t ra8_cache_dcache_clean_by_addr(const void* addr, uint32_t size)
{
  if (addr == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return ra8_nsc_dcache_clean_by_addr(addr, size);
}

/** @brief NS adapter for SDHI pin routing (Secure boot owns PFS). */
ra8_err_t ra8_board_sdhi_pins_init(void)
{
  return k_ra8_ok;
}

/**
 * @brief Stub WebP decoder binding for freestanding Non-Secure e-reader app.
 *
 * @details The standalone e-reader app decodes JPEG/PNG figures via ra8_img_imgdec.
 *          WebP is not supported in the freestanding ARM build to stay within
 *          flash budget (RA8FW-1399).
 *
 * @param[out] out     Handle to fill.
 * @param[in]  scratch Scratch arena.
 * @return ra8_err_t   k_ra8_err_not_supported.
 */
ra8_err_t ra8_webp_imgdec_bind(ra8_imgdec_t* out, ra8_webp_arena_t* scratch)
{
  (void)out;
  (void)scratch;
  return k_ra8_err_not_supported;
}

/**
 * @brief Stub JPEG decoder binding for freestanding Non-Secure e-reader app.
 *
 * @details The standalone e-reader app decodes JPEG figures via stb_image in
 *          ra8_img_imgdec. Freestanding first-party ra8_jpeg is omitted to stay
 *          within flash budget (RA8FW-1399).
 *
 * @param[out] out Handle to fill.
 * @return ra8_err_t k_ra8_err_not_supported.
 */
ra8_err_t ra8_jpeg_imgdec_bind(ra8_imgdec_t* out)
{
  (void)out;
  return k_ra8_err_not_supported;
}
