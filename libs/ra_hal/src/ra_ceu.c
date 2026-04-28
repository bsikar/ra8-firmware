/**
 * @file ra_ceu.c
 * @brief Capture Engine Unit (CEU) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full-coverage HAL driver for the RA8D2 Capture Engine Unit. See
 * `ra_ceu.h` for the per-API doxygen and the high-level state
 * machine; this file implements every public entry point and
 * cites the relevant register sequences out of HUM Ch 60 (p
 * 3626-3682).
 *
 * The driver covers all three CEU operating formats (image-capture
 * 4:2:2 / 4:2:0, data-synchronous fetch, data-enable fetch), both
 * capture modes (single-shot / continuous), interlace + bundle
 * write, plane-B shadow programming with VD-synchronised swap,
 * the TrustZone-style firewall, every CDOCR byte-swap permutation,
 * scale-down + low-pass filter, the CAPCR frame-drop / burst-mode
 * controls, and dispatch + acknowledgement of every CETCR event.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ceu.h"

#include <stdint.h>

#include "ra8d2_ceu_regs.h"
#include "ra_check.h"
#include "ra_dmac.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @var s_tag
 * @brief Component tag used for `ra_log_*` calls from this module.
 */
static const char* s_tag = "CEU";

/**
 * @enum ra_ceu_timing_t
 * @brief Bounded spin budgets for host + target builds.
 */
typedef enum : uint32_t {
  k_ra_ceu_reset_spin = 256U, /**< CSTSR.CPTON / CAPSR.CPKIL clear spin. */
} ra_ceu_timing_t;

/**
 * @enum ra_ceu_align_mask_t
 * @brief Buffer-alignment helper.
 *
 * @details
 * `k_ra_ceu_buffer_align_bytes` is 8; the lowest 3 bits of an
 * 8-byte-aligned address must be zero.
 */
typedef enum : uintptr_t {
  k_ra_ceu_buffer_align_mask = (uintptr_t)k_ra_ceu_buffer_align_bytes - 1U,
} ra_ceu_align_mask_t;

/**
 * @enum ra_ceu_dma_align_t
 * @brief DMAC-pump byte-count alignment.
 *
 * @details
 * `ra_ceu_dma_pump` requires a 4-byte multiple because the DMAC
 * is configured for 32-bit transfers; this mask lets the helper
 * test the lower bits cheaply.
 */
typedef enum : uint32_t {
  k_ra_ceu_dma_byte_align_mask = 3U,
  k_ra_ceu_dma_count_shift     = 2U, /**< divide-by-4 to convert bytes->words. */
} ra_ceu_dma_align_t;

/**
 * @var s_ceu_fn
 * @brief Module-scope event callback registered by `ra_ceu_attach_handler`.
 * @note Read by `ra_ceu_dispatch`; not safe to mutate from an ISR.
 */
static ra_ceu_event_fn_t s_ceu_fn;

/**
 * @var s_ceu_ctx
 * @brief Opaque context forwarded as the first argument to `s_ceu_fn`.
 */
static void* s_ceu_ctx;

/**
 * @var s_ceu_int_enable
 * @brief Cached copy of CEIER -- used to gate the dispatched event mask.
 */
static uint32_t s_ceu_int_enable;

/**
 * @var s_ceu_image_area
 * @brief Cached `image_area_size` from the most recent `ra_ceu_init`.
 *
 * @note Used by `ra_ceu_capture_start` in data-enable-fetch mode to
 *       arm the CFWCR firewall window automatically.
 */
static uint32_t s_ceu_image_area;

/**
 * @var s_ceu_capture_format
 * @brief Cached CAMCR.JPG copy so capture_start can choose its path.
 */
static ra_ceu_capture_format_t s_ceu_capture_format;

/**
 * @brief Build the CAMCR value from a configuration descriptor.
 *
 * @param[in] cfg Non-NULL config (caller already validated).
 * @return Packed CAMCR word.
 */
static uint32_t internal_pack_camcr(const ra_ceu_config_t* cfg)
{
  uint32_t camcr = 0U;
  camcr |= ((uint32_t)cfg->hsync_polarity) << k_ra_ceu_camcr_shift_hdpol;
  camcr |= ((uint32_t)cfg->vsync_polarity) << k_ra_ceu_camcr_shift_vdpol;
  camcr |= ((uint32_t)cfg->capture_format) << k_ra_ceu_camcr_shift_jpg;
  camcr |= ((uint32_t)cfg->input_order) << k_ra_ceu_camcr_shift_dtary;
  camcr |= ((uint32_t)cfg->data_bus) << k_ra_ceu_camcr_shift_dtif;
  camcr |= ((uint32_t)cfg->field_polarity) << k_ra_ceu_camcr_shift_fldpol;
  camcr |= ((uint32_t)cfg->edge.data) << k_ra_ceu_camcr_shift_dsel;
  camcr |= ((uint32_t)cfg->edge.field) << k_ra_ceu_camcr_shift_fldsel;
  camcr |= ((uint32_t)cfg->edge.hsync) << k_ra_ceu_camcr_shift_hdsel;
  camcr |= ((uint32_t)cfg->edge.vsync) << k_ra_ceu_camcr_shift_vdsel;
  return camcr;
}

/**
 * @brief Build the CAPCR value from a configuration descriptor.
 */
static uint32_t internal_pack_capcr(const ra_ceu_config_t* cfg)
{
  uint32_t capcr = 0U;
  if (cfg->capture_mode == k_ra_ceu_capture_continuous) {
    capcr |= (1UL << k_ra_ceu_capcr_shift_ctncp);
  }
  capcr |= ((uint32_t)cfg->burst_mode) << k_ra_ceu_capcr_shift_mtcm;
  capcr |= ((uint32_t)cfg->frame_drop) << k_ra_ceu_capcr_shift_fdrp;
  return capcr;
}

/**
 * @brief Build the CAIFR value from a configuration descriptor.
 */
static uint32_t internal_pack_caifr(const ra_ceu_config_t* cfg)
{
  uint32_t caifr = 0U;
  caifr |= ((uint32_t)cfg->first_field) << k_ra_ceu_caifr_shift_fci;
  if (cfg->one_field_only) {
    caifr |= (1UL << k_ra_ceu_caifr_shift_cim);
  }
  if (cfg->interlace) {
    caifr |= (1UL << k_ra_ceu_caifr_shift_ifs);
  }
  return caifr;
}

/**
 * @brief Build the CFLCR value from a configuration descriptor.
 */
static uint32_t internal_pack_cflcr(const ra_ceu_config_t* cfg)
{
  uint32_t cflcr = 0U;
  cflcr |= (((uint32_t)cfg->scale.h_fraction) & (uint32_t)k_ra_ceu_cflcr_mask_hfrac)
           << k_ra_ceu_cflcr_shift_hfrac;
  cflcr |= ((((uint32_t)cfg->scale.h_mantissa) << k_ra_ceu_cflcr_shift_hmant) &
            (uint32_t)k_ra_ceu_cflcr_mask_hmant);
  cflcr |= ((((uint32_t)cfg->scale.v_fraction) << k_ra_ceu_cflcr_shift_vfrac) &
            (uint32_t)k_ra_ceu_cflcr_mask_vfrac);
  cflcr |= ((((uint32_t)cfg->scale.v_mantissa) << k_ra_ceu_cflcr_shift_vmant) &
            (uint32_t)k_ra_ceu_cflcr_mask_vmant);
  return cflcr;
}

/**
 * @brief Build the CFSZR value from a configuration descriptor.
 */
static uint32_t internal_pack_cfszr(const ra_ceu_config_t* cfg)
{
  uint32_t cfszr = 0U;
  cfszr |= (((uint32_t)cfg->scale.h_output_clip) << k_ra_ceu_cfszr_shift_hfclp) &
           (uint32_t)k_ra_ceu_cfszr_mask_hfclp;
  cfszr |= (((uint32_t)cfg->scale.v_output_clip) << k_ra_ceu_cfszr_shift_vfclp) &
           (uint32_t)k_ra_ceu_cfszr_mask_vfclp;
  return cfszr;
}

/**
 * @brief Build the CDOCR value from a configuration descriptor.
 */
static uint32_t internal_pack_cdocr(const ra_ceu_config_t* cfg)
{
  uint32_t cdocr = 0U;
  if (cfg->byte_swap.swap_8_bit) {
    cdocr |= (uint32_t)k_ra_ceu_cdocr_mask_cobs;
  }
  if (cfg->byte_swap.swap_16_bit) {
    cdocr |= (uint32_t)k_ra_ceu_cdocr_mask_cows;
  }
  if (cfg->byte_swap.swap_32_bit) {
    cdocr |= (uint32_t)k_ra_ceu_cdocr_mask_cols;
  }
  cdocr |= (((uint32_t)cfg->output_format) << k_ra_ceu_cdocr_shift_cds) &
           (uint32_t)k_ra_ceu_cdocr_mask_cds;
  if (cfg->bundle_write) {
    cdocr |= (uint32_t)k_ra_ceu_cdocr_mask_cbe;
  }
  return cdocr;
}

/**
 * @brief Bounded spin until CSTSR.CPTON and CAPSR.CPKIL both clear.
 *
 * @return k_ra_ok if both bits cleared in budget, k_ra_err_hw_timeout
 *         on overrun.
 */
static ra_err_t internal_wait_idle(void)
{
#ifdef RA_SIMULATOR_MODE
  /* HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3672 -- on
   * the host there is no live engine; clear the bits ourselves and
   * declare success so callers can proceed. */
  *ra_ceu_reg32(k_ra_ceu_off_cstsr) = 0U;
  *ra_ceu_reg32(k_ra_ceu_off_capsr) = 0U;
  return k_ra_ok;
#else
  for (uint32_t i = 0U; i < (uint32_t)k_ra_ceu_reset_spin; i++) {
    /* HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3672 */
    const uint32_t cstsr = *ra_ceu_reg32(k_ra_ceu_off_cstsr);
    /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
    const uint32_t capsr = *ra_ceu_reg32(k_ra_ceu_off_capsr);
    if (((cstsr & (uint32_t)k_ra_ceu_cstsr_mask_cpton) == 0U) &&
        ((capsr & (uint32_t)k_ra_ceu_capsr_mask_cpkil) == 0U)) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
#endif
}

/**
 * @brief True if `ptr` is non-NULL and 8-byte aligned.
 */
static bool internal_is_aligned(const void* ptr)
{
  if (ptr == nullptr) {
    return true; /* nullptr means "leave register alone" -- caller's choice. */
  }
  return ((uintptr_t)ptr & (uintptr_t)k_ra_ceu_buffer_align_mask) == 0U;
}

/**
 * @brief Validate every non-NULL pointer in a buffer bundle.
 */
static ra_err_t internal_validate_buffers(const ra_ceu_buffers_t* bufs)
{
  if (!internal_is_aligned(bufs->y_top)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->c_top)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->y_bottom)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->c_bottom)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->y_top_2)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->c_top_2)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->y_bottom_2)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->c_bottom_2)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Program the CDAYR / CDACR / CDBYR / CDBCR family on Plane A.
 */
static void internal_program_addresses(const ra_ceu_buffers_t* bufs)
{
  /* HUM Ch 60.2.13 "CDAYR : Capture Data Y Address Register" p 3656 */
  *ra_ceu_reg32(k_ra_ceu_off_cdayr) = (uint32_t)(uintptr_t)bufs->y_top;
  /* HUM Ch 60.2.14 "CDACR : Capture Data C Address Register" p 3657 */
  if (bufs->c_top != nullptr) {
    *ra_ceu_reg32(k_ra_ceu_off_cdacr) = (uint32_t)(uintptr_t)bufs->c_top;
  }
  /* HUM Ch 60.2.15 "CDBYR : Capture Data Y Bottom-Field Register" p 3658 */
  if (bufs->y_bottom != nullptr) {
    *ra_ceu_reg32(k_ra_ceu_off_cdbyr) = (uint32_t)(uintptr_t)bufs->y_bottom;
  }
  /* HUM Ch 60.2.16 "CDBCR : Capture Data C Bottom-Field Register" p 3659 */
  if (bufs->c_bottom != nullptr) {
    *ra_ceu_reg32(k_ra_ceu_off_cdbcr) = (uint32_t)(uintptr_t)bufs->c_bottom;
  }
  /* HUM Ch 60.2 "Register Descriptions" p 3629 */
  /* Bundle-2 group sits at offsets 0x90..0x9C per HUM Table 60.4. */
  if (bufs->y_top_2 != nullptr) {
    *ra_ceu_reg32(k_ra_ceu_off_cdayr2) = (uint32_t)(uintptr_t)bufs->y_top_2;
  }
  if (bufs->c_top_2 != nullptr) {
    *ra_ceu_reg32(k_ra_ceu_off_cdacr2) = (uint32_t)(uintptr_t)bufs->c_top_2;
  }
  if (bufs->y_bottom_2 != nullptr) {
    *ra_ceu_reg32(k_ra_ceu_off_cdbyr2) = (uint32_t)(uintptr_t)bufs->y_bottom_2;
  }
  if (bufs->c_bottom_2 != nullptr) {
    *ra_ceu_reg32(k_ra_ceu_off_cdbcr2) = (uint32_t)(uintptr_t)bufs->c_bottom_2;
  }
  if (bufs->bundle_size_bytes != 0U) {
    /* HUM Ch 60.2.17 "CBDSR : Capture Bundle Destination Size" p 3660 */
    *ra_ceu_reg32(k_ra_ceu_off_cbdsr) =
      bufs->bundle_size_bytes & ~(uint32_t)k_ra_ceu_bundle_align_mask;
  }
}

ra_err_t ra_ceu_init(const ra_ceu_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* Continuous capture is only legal in image-capture mode (HUM Ch
   * 60.2.2 "Note" on CTNCP). Reject the impossible combination so
   * callers don't program a doomed sequence. */
  if ((cfg->capture_mode == k_ra_ceu_capture_continuous) &&
      (cfg->capture_format != k_ra_ceu_fmt_image_capture)) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_ceu);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "ceu_init: mstp enable");

  /* HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3672 +
   * HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 -- wait
   * for any in-flight reset to finish before we touch the engine. */
  const ra_err_t idle_err = internal_wait_idle();
  RA_RETURN_ON_ERROR(idle_err, s_tag, "ceu_init: wait idle");

  /* HUM Ch 60.2.10 "CFLCR : Capture Filter Control Register" p 3650 */
  *ra_ceu_reg32(k_ra_ceu_off_cflcr) = internal_pack_cflcr(cfg);

  /* HUM Ch 60.2.7 "CAIFR : Camera Interface Input Format Register" p 3647 */
  *ra_ceu_reg32(k_ra_ceu_off_caifr) = internal_pack_caifr(cfg);

  /* HUM Ch 60.2.2 "CAPCR : Capture Control Register" p 3634 */
  *ra_ceu_reg32(k_ra_ceu_off_capcr) = internal_pack_capcr(cfg);

  /* HUM Ch 60.2.3 "CAMCR : Camera Interface Control Register" p 3636 */
  *ra_ceu_reg32(k_ra_ceu_off_camcr) = internal_pack_camcr(cfg);

  /* HUM Ch 60.2.4 "CMCYR : Camera Interface Cycle Register" p 3641 */
  const uint32_t cmcyr =
    (((uint32_t)cfg->height_px) << k_ra_ceu_cmcyr_shift_vcyl) | (uint32_t)cfg->width_px;
  *ra_ceu_reg32(k_ra_ceu_off_cmcyr) = cmcyr;

  /* HUM Ch 60.2.5 "CAMOR : Camera Interface Offset Register" p 3645 */
  const uint32_t camor =
    (((uint32_t)cfg->y_start_px) << k_ra_ceu_camor_shift_vofst) | (uint32_t)cfg->x_start_px;
  *ra_ceu_reg32(k_ra_ceu_off_camor) = camor;

  /* HUM Ch 60.2.6 "CAPWR : Camera Interface Width Register" p 3646 */
  const uint16_t hwdth = (cfg->x_capture_px != 0U) ? cfg->x_capture_px : cfg->width_px;
  const uint16_t vwdth = (cfg->y_capture_lines != 0U) ? cfg->y_capture_lines : cfg->height_px;
  const uint32_t capwr = (((uint32_t)vwdth) << k_ra_ceu_capwr_shift_vwdth) | (uint32_t)hwdth;
  *ra_ceu_reg32(k_ra_ceu_off_capwr) = capwr;

  /* HUM Ch 60.2.11 "CFSZR : Capture Filter Size Clip Register" p 3651 */
  *ra_ceu_reg32(k_ra_ceu_off_cfszr) = internal_pack_cfszr(cfg);

  /* HUM Ch 60.2.12 "CDWDR : Capture Destination Width Register" p 3654 */
  *ra_ceu_reg32(k_ra_ceu_off_cdwdr) = (uint32_t)cfg->dst_stride;

  /* HUM Ch 60.2.18 "CFWCR : Firewall Operation Control Register" p 3660
   * Disabled at open; capture_start arms the window for data-enable
   * mode using the cached image_area_size. */
  *ra_ceu_reg32(k_ra_ceu_off_cfwcr) = 0U;

  /* HUM Ch 60.2.19 "CLFCR : Capture Low-Pass Filter Control" p 3661 */
  *ra_ceu_reg32(k_ra_ceu_off_clfcr) = cfg->low_pass_filter ? 1U : 0U;

  /* HUM Ch 60.2.20 "CDOCR : Capture Data Output Control Register" p 3662 */
  *ra_ceu_reg32(k_ra_ceu_off_cdocr) = internal_pack_cdocr(cfg);

  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3664 */
  *ra_ceu_reg32(k_ra_ceu_off_cetcr) = 0U;

  s_ceu_int_enable     = (uint32_t)cfg->interrupts;
  s_ceu_image_area     = cfg->image_area_size;
  s_ceu_capture_format = cfg->capture_format;

  /* HUM Ch 60.2.21 "CEIER : Capture Event Interrupt Enable Register" p 3663 */
  *ra_ceu_reg32(k_ra_ceu_off_ceier) = s_ceu_int_enable;

  ra_log_info_val(s_tag, "ceu_init width", (uint32_t)cfg->width_px);
  return k_ra_ok;
}

ra_err_t ra_ceu_deinit(void)
{
  /* HUM Ch 60.2.21 "CEIER : Capture Event Interrupt Enable Register" p 3663 */
  *ra_ceu_reg32(k_ra_ceu_off_ceier) = 0U;
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
  /* Abort any active capture on the way out. */
  *ra_ceu_reg32(k_ra_ceu_off_capsr) = (uint32_t)k_ra_ceu_capsr_mask_cpkil;

  s_ceu_fn         = nullptr;
  s_ceu_ctx        = nullptr;
  s_ceu_int_enable = 0U;
  s_ceu_image_area = 0U;

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra_mstp_disable(k_ra_mstp_ceu);
}

ra_err_t ra_ceu_reset(void)
{
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
  *ra_ceu_reg32(k_ra_ceu_off_capsr) = (uint32_t)k_ra_ceu_capsr_mask_cpkil;
  return internal_wait_idle();
}

ra_err_t ra_ceu_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3664 */
  *out_mask = *ra_ceu_reg32(k_ra_ceu_off_cetcr);
  return k_ra_ok;
}

ra_err_t ra_ceu_clear_status(uint32_t mask)
{
  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3664
   * CETCR bits clear when 0 is written; preserve bits not in `mask`
   * by writing back a value that is `current & ~mask`. The hardware
   * never resets a CETCR bit on a 1 write, so 1s leave the existing
   * flag asserted. */
  volatile uint32_t* reg     = ra_ceu_reg32(k_ra_ceu_off_cetcr);
  const uint32_t     current = *reg;
  *reg                       = current & ~mask;
  return k_ra_ok;
}

ra_err_t ra_ceu_status_snapshot(ra_ceu_status_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3664 */
  out->events = *ra_ceu_reg32(k_ra_ceu_off_cetcr);
  /* HUM Ch 60.2.24 "CDSSR : Capture Data Size Register" p 3674 */
  out->data_size = *ra_ceu_reg32(k_ra_ceu_off_cdssr);
  /* HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3672 */
  const uint32_t cstsr = *ra_ceu_reg32(k_ra_ceu_off_cstsr);
  out->capturing       = ((cstsr & (uint32_t)k_ra_ceu_cstsr_mask_cpton) != 0U);
  out->top_field       = ((cstsr & (uint32_t)k_ra_ceu_cstsr_mask_cpfld) != 0U);
  out->active_plane =
    ((cstsr & (uint32_t)k_ra_ceu_cstsr_mask_crst) != 0U) ? k_ra_ceu_plane_b : k_ra_ceu_plane_a;
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
  const uint32_t capsr = *ra_ceu_reg32(k_ra_ceu_off_capsr);
  out->reset_in_flight = ((capsr & (uint32_t)k_ra_ceu_capsr_mask_cpkil) != 0U);
  return k_ra_ok;
}

ra_err_t ra_ceu_data_size_get(uint32_t* out_bytes)
{
  RA_CHECK_NULL_PTR(out_bytes, s_tag, "out_bytes must not be nullptr");
  /* HUM Ch 60.2.24 "CDSSR : Capture Data Size Register" p 3674 */
  *out_bytes = *ra_ceu_reg32(k_ra_ceu_off_cdssr);
  return k_ra_ok;
}

ra_err_t ra_ceu_interrupts_set(uint32_t mask)
{
  s_ceu_int_enable = mask;
  /* HUM Ch 60.2.21 "CEIER : Capture Event Interrupt Enable Register" p 3663 */
  *ra_ceu_reg32(k_ra_ceu_off_ceier) = mask;
  return k_ra_ok;
}

ra_err_t ra_ceu_attach_handler(ra_ceu_event_fn_t fn, void* ctx)
{
  s_ceu_fn  = fn;
  s_ceu_ctx = ctx;
  return k_ra_ok;
}

void ra_ceu_dispatch(void)
{
  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3664 */
  volatile uint32_t* reg     = ra_ceu_reg32(k_ra_ceu_off_cetcr);
  const uint32_t     pending = *reg;
  /* Clear every observed bit (write-0-to-clear). Bits we did not see
   * are left alone in case they were asserted between read and write. */
  *reg = ~pending;

  const ra_ceu_event_fn_t fn  = s_ceu_fn;
  void* const             ctx = s_ceu_ctx;
  if (fn != nullptr) {
    fn(ctx, pending & s_ceu_int_enable);
  }
}

ra_err_t ra_ceu_enter_stop(void)
{
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
  *ra_ceu_reg32(k_ra_ceu_off_capsr) = (uint32_t)k_ra_ceu_capsr_mask_cpkil;
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra_mstp_disable(k_ra_mstp_ceu);
}

ra_err_t ra_ceu_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra_mstp_enable(k_ra_mstp_ceu);
}

/**
 * @brief Common arming sequence for capture_start / capture_start_ex.
 */
static ra_err_t internal_arm_capture(const ra_ceu_buffers_t* bufs)
{
  /* HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3672 -- a
   * non-zero CPTON means a capture is already running. */
  const uint32_t cstsr = *ra_ceu_reg32(k_ra_ceu_off_cstsr);
  if ((cstsr & (uint32_t)k_ra_ceu_cstsr_mask_cpton) != 0U) {
    return k_ra_err_busy;
  }
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 -- if a
   * software reset is in flight the engine is not ready. */
  const uint32_t capsr_now = *ra_ceu_reg32(k_ra_ceu_off_capsr);
  if ((capsr_now & (uint32_t)k_ra_ceu_capsr_mask_cpkil) != 0U) {
    return k_ra_err_busy;
  }

  internal_program_addresses(bufs);

  /* HUM Ch 60.2.18 "CFWCR : Firewall Operation Control Register" p 3661
   * In data-enable-fetch mode arm the firewall to the cached image
   * area; in image / data-sync modes leave the firewall disabled. */
  if (s_ceu_capture_format == k_ra_ceu_fmt_data_enable) {
    if ((s_ceu_image_area != 0U) && (bufs->y_top != nullptr)) {
      const uint32_t upper = (uint32_t)(uintptr_t)bufs->y_top + s_ceu_image_area - 1U;
      *ra_ceu_reg32(k_ra_ceu_off_cfwcr) =
        (uint32_t)k_ra_ceu_cfwcr_mask_fwe | (upper & (uint32_t)k_ra_ceu_cfwcr_mask_fwv);
    }
  } else {
    *ra_ceu_reg32(k_ra_ceu_off_cfwcr) = 0U;
  }

  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3664 */
  *ra_ceu_reg32(k_ra_ceu_off_cetcr) = 0U;

  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 -- arm the
   * CE bit; capture begins on the next VD edge. */
  *ra_ceu_reg32(k_ra_ceu_off_capsr) = (uint32_t)k_ra_ceu_capsr_mask_ce;
  return k_ra_ok;
}

ra_err_t ra_ceu_capture_start(uint8_t* buffer)
{
  RA_CHECK_NULL_PTR(buffer, s_tag, "buffer must not be nullptr");

  /* HUM Ch 60.2.13 "CDAYR : Capture Data Y Address Register" p 3656 --
   * lower 3 bits must be zero (8-byte alignment). */
  if (((uintptr_t)buffer & (uintptr_t)k_ra_ceu_buffer_align_mask) != 0U) {
    return k_ra_err_invalid_arg;
  }

  const ra_ceu_buffers_t bufs = {
    .y_top             = buffer,
    .c_top             = buffer, /* default to same buffer; image-mode caller can override later. */
    .y_bottom          = nullptr,
    .c_bottom          = nullptr,
    .y_top_2           = nullptr,
    .c_top_2           = nullptr,
    .y_bottom_2        = nullptr,
    .c_bottom_2        = nullptr,
    .bundle_size_bytes = 0U,
  };

  const ra_err_t arm_err = internal_arm_capture(&bufs);
  if (arm_err != k_ra_ok) {
    return arm_err;
  }
  ra_log_info_val(s_tag, "ceu_capture_start buf", (uint32_t)(uintptr_t)buffer);
  return k_ra_ok;
}

ra_err_t ra_ceu_capture_start_ex(const ra_ceu_buffers_t* bufs)
{
  RA_CHECK_NULL_PTR(bufs, s_tag, "bufs must not be nullptr");
  RA_CHECK_NULL_PTR(bufs->y_top, s_tag, "bufs->y_top must not be nullptr");

  const ra_err_t align_err = internal_validate_buffers(bufs);
  if (align_err != k_ra_ok) {
    return align_err;
  }

  return internal_arm_capture(bufs);
}

ra_err_t ra_ceu_capture_stop(void)
{
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 -- clear
   * CE so the next VD does not start a new frame; in-flight frame
   * runs to completion. */
  volatile uint32_t* reg = ra_ceu_reg32(k_ra_ceu_off_capsr);
  const uint32_t     val = *reg;
  *reg                   = val & ~(uint32_t)k_ra_ceu_capsr_mask_ce;
  return k_ra_ok;
}

ra_err_t ra_ceu_plane_b_program(const ra_ceu_buffers_t* bufs)
{
  if (bufs != nullptr) {
    const ra_err_t align_err = internal_validate_buffers(bufs);
    if (align_err != k_ra_ok) {
      return align_err;
    }
  }

  /* HUM Ch 60.2 Table 60.4 "Register configuration of CEU" p 3629 --
   * mirror every 3-plane register from Plane A to Plane B so the
   * shadow window is consistent before we install per-bundle
   * overrides. */
  static const ra_ceu_off_t s_plane_offsets[] = {
    k_ra_ceu_off_camor,
    k_ra_ceu_off_capwr,
    k_ra_ceu_off_cflcr,
    k_ra_ceu_off_cfszr,
    k_ra_ceu_off_cdwdr,
    k_ra_ceu_off_cdayr,
    k_ra_ceu_off_cdacr,
    k_ra_ceu_off_cdbyr,
    k_ra_ceu_off_cdbcr,
    k_ra_ceu_off_cbdsr,
    k_ra_ceu_off_clfcr,
    k_ra_ceu_off_cdocr,
    k_ra_ceu_off_cdayr2,
    k_ra_ceu_off_cdacr2,
    k_ra_ceu_off_cdbyr2,
    k_ra_ceu_off_cdbcr2,
  };
  for (uint32_t i = 0U; i < (sizeof(s_plane_offsets) / sizeof(s_plane_offsets[0])); i++) {
    const uint32_t a = *ra_ceu_reg32_plane(s_plane_offsets[i], k_ra_ceu_plane_a_off);
    *ra_ceu_reg32_plane(s_plane_offsets[i], k_ra_ceu_plane_b_off) = a;
  }

  /* Apply caller overrides on top of the mirror. Plane B holds the
   * pending geometry/address set; the CEU swaps planes on the next
   * VD edge once we set CRCNTR.RVS below. */
  if (bufs != nullptr) {
    if (bufs->y_top != nullptr) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cdayr, k_ra_ceu_plane_b_off) =
        (uint32_t)(uintptr_t)bufs->y_top;
    }
    if (bufs->c_top != nullptr) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cdacr, k_ra_ceu_plane_b_off) =
        (uint32_t)(uintptr_t)bufs->c_top;
    }
    if (bufs->y_bottom != nullptr) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cdbyr, k_ra_ceu_plane_b_off) =
        (uint32_t)(uintptr_t)bufs->y_bottom;
    }
    if (bufs->c_bottom != nullptr) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cdbcr, k_ra_ceu_plane_b_off) =
        (uint32_t)(uintptr_t)bufs->c_bottom;
    }
    if (bufs->y_top_2 != nullptr) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cdayr2, k_ra_ceu_plane_b_off) =
        (uint32_t)(uintptr_t)bufs->y_top_2;
    }
    if (bufs->c_top_2 != nullptr) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cdacr2, k_ra_ceu_plane_b_off) =
        (uint32_t)(uintptr_t)bufs->c_top_2;
    }
    if (bufs->y_bottom_2 != nullptr) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cdbyr2, k_ra_ceu_plane_b_off) =
        (uint32_t)(uintptr_t)bufs->y_bottom_2;
    }
    if (bufs->c_bottom_2 != nullptr) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cdbcr2, k_ra_ceu_plane_b_off) =
        (uint32_t)(uintptr_t)bufs->c_bottom_2;
    }
    if (bufs->bundle_size_bytes != 0U) {
      *ra_ceu_reg32_plane(k_ra_ceu_off_cbdsr, k_ra_ceu_plane_b_off) =
        bufs->bundle_size_bytes & ~(uint32_t)k_ra_ceu_bundle_align_mask;
    }
  }

  /* HUM Ch 60.2.8 "CRCNTR : CEU Register Control Register" p 3649 --
   * enable both planes and arm the VD-synchronised swap. */
  *ra_ceu_reg32(k_ra_ceu_off_crcntr) = (uint32_t)k_ra_ceu_crcntr_mask_rc |
                                       (uint32_t)k_ra_ceu_crcntr_mask_rs |
                                       (uint32_t)k_ra_ceu_crcntr_mask_rvs;
  return k_ra_ok;
}

ra_err_t ra_ceu_plane_swap_force(void)
{
  /* HUM Ch 60.2.9 "CRCMPR : CEU Register Forcible Control Register" p 3649 */
  *ra_ceu_reg32(k_ra_ceu_off_crcmpr) = (uint32_t)k_ra_ceu_crcmpr_mask_ra;
  return k_ra_ok;
}

ra_err_t ra_ceu_firewall_set(bool enable, uint32_t upper_bound)
{
  uint32_t cfwcr = 0U;
  if (enable) {
    cfwcr |= (uint32_t)k_ra_ceu_cfwcr_mask_fwe;
    cfwcr |= (upper_bound & (uint32_t)k_ra_ceu_cfwcr_mask_fwv);
  }
  /* HUM Ch 60.2.18 "CFWCR : Firewall Operation Control Register" p 3661 */
  *ra_ceu_reg32(k_ra_ceu_off_cfwcr) = cfwcr;
  return k_ra_ok;
}

ra_err_t ra_ceu_byte_swap_set(const ra_ceu_byte_swap_t* swap)
{
  RA_CHECK_NULL_PTR(swap, s_tag, "swap must not be nullptr");
  /* HUM Ch 60.2.20 "CDOCR : Capture Data Output Control Register" p 3662 */
  volatile uint32_t* reg     = ra_ceu_reg32(k_ra_ceu_off_cdocr);
  uint32_t           current = *reg;
  current &= ~((uint32_t)k_ra_ceu_cdocr_mask_cobs | (uint32_t)k_ra_ceu_cdocr_mask_cows |
               (uint32_t)k_ra_ceu_cdocr_mask_cols);
  if (swap->swap_8_bit) {
    current |= (uint32_t)k_ra_ceu_cdocr_mask_cobs;
  }
  if (swap->swap_16_bit) {
    current |= (uint32_t)k_ra_ceu_cdocr_mask_cows;
  }
  if (swap->swap_32_bit) {
    current |= (uint32_t)k_ra_ceu_cdocr_mask_cols;
  }
  *reg = current;
  return k_ra_ok;
}

ra_err_t ra_ceu_bundle_size_set(uint32_t size_bytes)
{
  /* HUM Ch 60.2.17 "CBDSR : Capture Bundle Destination Size" p 3660 */
  *ra_ceu_reg32(k_ra_ceu_off_cbdsr) = size_bytes & ~(uint32_t)k_ra_ceu_bundle_align_mask;
  return k_ra_ok;
}

ra_err_t ra_ceu_low_pass_set(bool enable)
{
  /* HUM Ch 60.2.19 "CLFCR : Capture Low-Pass Filter Control" p 3661 */
  *ra_ceu_reg32(k_ra_ceu_off_clfcr) = enable ? (uint32_t)k_ra_ceu_clfcr_mask_lpf : 0U;
  return k_ra_ok;
}

ra_err_t ra_ceu_capture_mode_set(ra_ceu_capture_mode_t mode)
{
  /* HUM Ch 60.2.2 "CAPCR : Capture Control Register" p 3634 */
  volatile uint32_t* reg     = ra_ceu_reg32(k_ra_ceu_off_capcr);
  uint32_t           current = *reg;
  current &= ~(uint32_t)k_ra_ceu_capcr_mask_ctncp;
  if (mode == k_ra_ceu_capture_continuous) {
    current |= (uint32_t)k_ra_ceu_capcr_mask_ctncp;
  }
  *reg = current;
  return k_ra_ok;
}

ra_err_t ra_ceu_frame_drop_set(uint8_t count)
{
  /* HUM Ch 60.2.2 "CAPCR : Capture Control Register" p 3634 */
  volatile uint32_t* reg     = ra_ceu_reg32(k_ra_ceu_off_capcr);
  uint32_t           current = *reg;
  current &= ~(uint32_t)k_ra_ceu_capcr_mask_fdrp;
  current |= ((uint32_t)count) << k_ra_ceu_capcr_shift_fdrp;
  *reg = current;
  return k_ra_ok;
}

ra_err_t ra_ceu_dma_pump(uint8_t channel, const uint8_t* src, uint8_t* dst, uint32_t bytes)
{
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA_CHECK_NULL_PTR(dst, s_tag, "dst must not be nullptr");
  if (bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((bytes & (uint32_t)k_ra_ceu_dma_byte_align_mask) != 0U) {
    return k_ra_err_invalid_arg;
  }

  /* The CEU writes pixels with its own bus master; the DMAC is only
   * used by the application-level pump that ferries finished
   * frames. Configure for word-wide block transfer with both
   * pointers incrementing. */
  const ra_dmac_config_t cfg = {
    .src     = (uint32_t)(uintptr_t)src,
    .dst     = (uint32_t)(uintptr_t)dst,
    .count   = (uint16_t)(bytes >> k_ra_ceu_dma_count_shift),
    .width   = k_ra_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  return ra_dmac_start(channel, &cfg);
}
