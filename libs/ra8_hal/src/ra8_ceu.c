/**
 * @file ra8_ceu.c
 * @brief Capture Engine Unit (CEU) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full-coverage HAL driver for the RA8D2 Capture Engine Unit. See
 * `ra8_ceu.h` for the per-API doxygen and the high-level state
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

#include "ra8_ceu.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_ceu_internal.h"
#include "ra8_ceu_regs.h"
#include "ra8_check.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/**
 * @var s_tag
 * @brief Component tag used for `ra8_log_*` calls from this module.
 */
static const char* s_tag = "CEU";

/**
 * @enum ra8_ceu_timing_t
 * @brief Bounded spin budgets for host + target builds.
 */
typedef enum : uint32_t {
  k_ra8_ceu_reset_spin = 256U, /**< CSTSR.CPTON / CAPSR.CPKIL clear spin. */
} ra8_ceu_timing_t;

/**
 * @enum ra8_ceu_align_mask_t
 * @brief Buffer-alignment helper.
 *
 * @details
 * `k_ra8_ceu_buffer_align_bytes` is 8; the lowest 3 bits of an
 * 8-byte-aligned address must be zero.
 */
typedef enum : uintptr_t {
  k_ra8_ceu_buffer_align_mask =
    (uintptr_t)k_ra8_ceu_buffer_align_bytes - 1U, /**< RA8 CEU buffer align mask. */
} ra8_ceu_align_mask_t;

/**
 * @enum ra8_ceu_dma_align_t
 * @brief DMAC-pump byte-count alignment.
 *
 * @details
 * `ra8_ceu_dma_pump` requires a 4-byte multiple because the DMAC
 * is configured for 32-bit transfers; this mask lets the helper
 * test the lower bits cheaply.
 */
typedef enum : uint32_t {
  k_ra8_ceu_dma_byte_align_mask = 3U, /**< RA8 CEU DMA byte align mask.         */
  k_ra8_ceu_dma_count_shift     = 2U, /**< divide-by-4 to convert bytes->words. */
} ra8_ceu_dma_align_t;

/**
 * @var s_ceu_fn
 * @brief Module-scope event callback registered by `ra8_ceu_attach_handler`.
 * @note Read by `ra8_ceu_dispatch`; not safe to mutate from an ISR.
 */
static ra8_ceu_event_fn_t s_ceu_fn;

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
 * @brief Cached `image_area_size` from the most recent `ra8_ceu_init`.
 *
 * @note Used by `ra8_ceu_capture_start` in data-enable-fetch mode to
 *       arm the CFWCR firewall window automatically.
 */
static uint32_t s_ceu_image_area;

/**
 * @var s_ceu_capture_format
 * @brief Cached CAMCR.JPG copy so capture_start can choose its path.
 */
static ra8_ceu_capture_format_t s_ceu_capture_format;

/**
 * @var s_ceu_dma_buf
 * @brief Cached DMAC-target buffer set via `ra8_ceu_set_dma_buffer`.
 *
 * @note Read by `ra8_ceu_capture_start` to arm CDAYR.
 * @since 0.1.0
 */
static uint8_t* s_ceu_dma_buf;

/**
 * @var s_ceu_dma_len
 * @brief Cached buffer length (bytes) for the next capture.
 *
 * @since 0.1.0
 */
static uint32_t s_ceu_dma_len;

/**
 * @brief Bounded spin until CSTSR.CPTON and CAPSR.CPKIL both clear.
 *
 * @return k_ra8_ok if both bits cleared in budget, k_ra8_err_hw_timeout
 *         on overrun.
 *
 * @details Polls the real two-register idle condition on every build.
 * On host tests the loop-exit decision comes from the ra8_fake_mmio seam
 * keyed on CSTSR (first-poll success unless a test arms a fault), so
 * both the retry and the timeout legs are testable.
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_hw_timeout CPTON or CPKIL stuck past the budget.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre The CEU register window is accessible (MSTP released).
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_wait_idle(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_ceu_reset_spin; i++) {
    /* HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3673 */
    const uint32_t cstsr = *ra8_ceu_reg32(k_ra8_ceu_off_cstsr);
    /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
    const uint32_t capsr = *ra8_ceu_reg32(k_ra8_ceu_off_capsr);
    const bool     idle  = ((cstsr & (uint32_t)k_ra8_ceu_cstsr_mask_cpton) == 0U) &&
                           ((capsr & (uint32_t)k_ra8_ceu_capsr_mask_cpkil) == 0U);
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* Host MMIO fault seam, keyed on CSTSR (the primary status reg). */
    if (ra8_fake_mmio_wait_eval(ra8_ceu_reg32(k_ra8_ceu_off_cstsr), i, idle)) {
      return k_ra8_ok;
    }
#else
    if (idle) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief True if `ptr` is non-NULL and 8-byte aligned.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] ptr See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_is_aligned(const void* ptr)
{
  if (ptr == nullptr) {
    return true; /* nullptr means "leave register alone" -- caller's choice. */
  }
  return ((uintptr_t)ptr & k_ra8_ceu_buffer_align_mask) == 0U;
}

/**
 * @brief Validate every non-NULL pointer in a buffer bundle.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] bufs See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_buffers(const ra8_ceu_buffers_t* bufs)
{
  if (!internal_is_aligned(bufs->y_top)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->c_top)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->y_bottom)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->c_bottom)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->y_top_2)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->c_top_2)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->y_bottom_2)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_is_aligned(bufs->c_bottom_2)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Program the CDAYR / CDACR / CDBYR / CDBCR family on Plane A.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] bufs See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_addresses(const ra8_ceu_buffers_t* bufs)
{
  /* HUM Ch 60.2.13 "CDAYR : Capture Data Y Address Register" p 3656 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cdayr) = (uint32_t)(uintptr_t)bufs->y_top;
  /* HUM Ch 60.2.14 "CDACR : Capture Data C Address Register" p 3657 */
  if (bufs->c_top != nullptr) {
    *ra8_ceu_reg32(k_ra8_ceu_off_cdacr) = (uint32_t)(uintptr_t)bufs->c_top;
  }
  /* HUM Ch 60.2.15 "CDBYR : Capture Data Y Bottom-Field Register" p 3658 */
  if (bufs->y_bottom != nullptr) {
    *ra8_ceu_reg32(k_ra8_ceu_off_cdbyr) = (uint32_t)(uintptr_t)bufs->y_bottom;
  }
  /* HUM Ch 60.2.16 "CDBCR : Capture Data C Bottom-Field Register" p 3659 */
  if (bufs->c_bottom != nullptr) {
    *ra8_ceu_reg32(k_ra8_ceu_off_cdbcr) = (uint32_t)(uintptr_t)bufs->c_bottom;
  }
  /* HUM Ch 60.2 "Register Descriptions" p 3629 */
  /* Bundle-2 group sits at offsets 0x90..0x9C per HUM Table 60.4. */
  if (bufs->y_top_2 != nullptr) {
    *ra8_ceu_reg32(k_ra8_ceu_off_cdayr2) = (uint32_t)(uintptr_t)bufs->y_top_2;
  }
  if (bufs->c_top_2 != nullptr) {
    *ra8_ceu_reg32(k_ra8_ceu_off_cdacr2) = (uint32_t)(uintptr_t)bufs->c_top_2;
  }
  if (bufs->y_bottom_2 != nullptr) {
    *ra8_ceu_reg32(k_ra8_ceu_off_cdbyr2) = (uint32_t)(uintptr_t)bufs->y_bottom_2;
  }
  if (bufs->c_bottom_2 != nullptr) {
    *ra8_ceu_reg32(k_ra8_ceu_off_cdbcr2) = (uint32_t)(uintptr_t)bufs->c_bottom_2;
  }
  if (bufs->bundle_size_bytes != 0U) {
    /* HUM Ch 60.2.17 "CBDSR : Capture Bundle Destination Size" p 3660 */
    *ra8_ceu_reg32(k_ra8_ceu_off_cbdsr) = bufs->bundle_size_bytes & ~k_ra8_ceu_bundle_align_mask;
  }
}

ra8_err_t ra8_ceu_init(const ra8_ceu_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* Continuous capture is only legal in image-capture mode (HUM Ch
   * 60.2.2 "Note" on CTNCP). */
  if ((cfg->capture_mode == k_ra8_ceu_capture_continuous) &&
      (cfg->capture_format != k_ra8_ceu_fmt_image_capture)) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_ceu);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "ceu_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 60.2.23 "CSTSR" p 3673 */ /* + HUM Ch 60.2.1 "CAPSR" p 3630 */
  const ra8_err_t idle_err = internal_wait_idle();
  RA8_RETURN_ON_ERROR(idle_err, s_tag, "ceu_init: wait idle");

  ra8_ceu_program_format(cfg);
  ra8_ceu_program_geometry(cfg);
  ra8_ceu_program_destination(cfg);

  s_ceu_int_enable     = (uint32_t)cfg->interrupts;
  s_ceu_image_area     = cfg->image_area_size;
  s_ceu_capture_format = cfg->capture_format;

  /* HUM Ch 60.2.21 "CEIER : Capture Event Interrupt Enable Register" p 3668 */
  *ra8_ceu_reg32(k_ra8_ceu_off_ceier) = s_ceu_int_enable;

  ra8_log_info_val(s_tag, "ceu_init width", (uint32_t)cfg->width_px);
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_deinit(void)
{
  /* HUM Ch 60.2.21 "CEIER : Capture Event Interrupt Enable Register" p 3668 */
  *ra8_ceu_reg32(k_ra8_ceu_off_ceier) = 0U;
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
  /* Abort any active capture on the way out. */
  *ra8_ceu_reg32(k_ra8_ceu_off_capsr) = k_ra8_ceu_capsr_mask_cpkil;

  s_ceu_fn         = nullptr;
  s_ceu_ctx        = nullptr;
  s_ceu_int_enable = 0U;
  s_ceu_image_area = 0U;
  s_ceu_dma_buf    = nullptr;
  s_ceu_dma_len    = 0U;

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra8_mstp_disable(k_ra8_mstp_ceu);
}

ra8_err_t ra8_ceu_reset(void)
{
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
  *ra8_ceu_reg32(k_ra8_ceu_off_capsr) = k_ra8_ceu_capsr_mask_cpkil;
  return internal_wait_idle();
}

ra8_err_t ra8_ceu_get_status(uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3669 */
  *out_mask = *ra8_ceu_reg32(k_ra8_ceu_off_cetcr);
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_clear_status(uint32_t mask)
{
  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3669
   * CETCR bits clear when 0 is written; preserve bits not in `mask`
   * by writing back a value that is `current & ~mask`. The hardware
   * never resets a CETCR bit on a 1 write, so 1s leave the existing
   * flag asserted. */
  volatile uint32_t* reg     = ra8_ceu_reg32(k_ra8_ceu_off_cetcr);
  const uint32_t     current = *reg;
  *reg                       = current & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_status_snapshot(ra8_ceu_status_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3669 */
  out->events = *ra8_ceu_reg32(k_ra8_ceu_off_cetcr);
  /* HUM Ch 60.2.24 "CDSSR : Capture Data Size Register" p 3674 */
  out->data_size = *ra8_ceu_reg32(k_ra8_ceu_off_cdssr);
  /* HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3673 */
  const uint32_t cstsr = *ra8_ceu_reg32(k_ra8_ceu_off_cstsr);
  out->capturing       = ((cstsr & k_ra8_ceu_cstsr_mask_cpton) != 0U);
  out->top_field       = ((cstsr & k_ra8_ceu_cstsr_mask_cpfld) != 0U);
  out->active_plane =
    ((cstsr & k_ra8_ceu_cstsr_mask_crst) != 0U) ? k_ra8_ceu_plane_b : k_ra8_ceu_plane_a;
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
  const uint32_t capsr = *ra8_ceu_reg32(k_ra8_ceu_off_capsr);
  out->reset_in_flight = ((capsr & k_ra8_ceu_capsr_mask_cpkil) != 0U);
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_data_size_get(uint32_t* out_bytes)
{
  RA8_CHECK_NULL_PTR(out_bytes, s_tag, "out_bytes must not be nullptr");
  /* HUM Ch 60.2.24 "CDSSR : Capture Data Size Register" p 3674 */
  *out_bytes = *ra8_ceu_reg32(k_ra8_ceu_off_cdssr);
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_interrupts_set(uint32_t mask)
{
  s_ceu_int_enable = mask;
  /* HUM Ch 60.2.21 "CEIER : Capture Event Interrupt Enable Register" p 3668 */
  *ra8_ceu_reg32(k_ra8_ceu_off_ceier) = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_attach_handler(ra8_ceu_event_fn_t fn, void* ctx)
{
  s_ceu_fn  = fn;
  s_ceu_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_ceu_dispatch(void)
{
  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3669
   * CETCR bits clear when 0 is written; preserve bits not observed by
   * re-reading and writing back `current & ~pending`. Writing a 1 to a
   * CETCR bit leaves that flag asserted, so the prior observed bits
   * become 0 and any newly-asserted-during-dispatch bits stay set. */
  volatile uint32_t* reg     = ra8_ceu_reg32(k_ra8_ceu_off_cetcr);
  const uint32_t     pending = *reg;
  const uint32_t     current = *reg;
  *reg                       = current & ~pending;

  const ra8_ceu_event_fn_t fn  = s_ceu_fn;
  void* const              ctx = s_ceu_ctx;
  if (fn != nullptr) {
    fn(ctx, pending & s_ceu_int_enable);
  }
}

ra8_err_t ra8_ceu_enter_stop(void)
{
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 */
  *ra8_ceu_reg32(k_ra8_ceu_off_capsr) = k_ra8_ceu_capsr_mask_cpkil;
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra8_mstp_disable(k_ra8_mstp_ceu);
}

ra8_err_t ra8_ceu_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra8_mstp_enable(k_ra8_mstp_ceu);
}

/**
 * @brief Common arming sequence for capture_start / capture_start_ex.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] bufs See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_arm_capture(const ra8_ceu_buffers_t* bufs)
{
  /* HUM Ch 60.2.23 "CSTSR : Capture Status Register" p 3673 -- a
   * non-zero CPTON means a capture is already running. */
  const uint32_t cstsr = *ra8_ceu_reg32(k_ra8_ceu_off_cstsr);
  if ((cstsr & k_ra8_ceu_cstsr_mask_cpton) != 0U) {
    return k_ra8_err_busy;
  }
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 -- if a
   * software reset is in flight the engine is not ready. */
  const uint32_t capsr_now = *ra8_ceu_reg32(k_ra8_ceu_off_capsr);
  if ((capsr_now & k_ra8_ceu_capsr_mask_cpkil) != 0U) {
    return k_ra8_err_busy;
  }

  /* HUM Ch 60.2.18 "CFWCR : Firewall Operation Control Register" p 3661
   * In data-enable-fetch mode arm the firewall to the cached image
   * area; in image / data-sync modes leave the firewall disabled. */
  if (s_ceu_capture_format == k_ra8_ceu_fmt_data_enable) {
    // mcdc-deactivated: TU-local helper internal_arm_capture; bufs->y_top non-NULL is enforced by upstream public-API ra8_ceu_capture_start contract, and s_ceu_image_area is non-zero whenever data-enable-fetch mode is configured -- the AND's two conditions cannot be flipped independently from any reachable call site.
    if ((s_ceu_image_area != 0U) && (bufs->y_top != nullptr)) {
      const uint32_t upper = (uint32_t)(uintptr_t)bufs->y_top + s_ceu_image_area - 1U;
      *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr) =
        k_ra8_ceu_cfwcr_mask_fwe | (upper & k_ra8_ceu_cfwcr_mask_fwv);
    }
  } else {
    *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr) = 0U;
  }

  /* HUM Ch 60.2.13 "CDAYR : Capture Data Y Address Register" p 3656.
   * Program the destination after the data-enable firewall, matching the
   * hardware sequence used by Renesas FSP R_CEU_CaptureStart. */
  internal_program_addresses(bufs);

  /* HUM Ch 60.2.22 "CETCR : Capture Event Flag Clear Register" p 3669 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cetcr) = 0U;

  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 -- arm the
   * CE bit; capture begins on the next VD edge. */
  *ra8_ceu_reg32(k_ra8_ceu_off_capsr) = k_ra8_ceu_capsr_mask_ce;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_capture_arm(uint8_t* buffer)
{
  RA8_CHECK_NULL_PTR(buffer, s_tag, "buffer must not be nullptr");

  /* HUM Ch 60.2.13 "CDAYR : Capture Data Y Address Register" p 3656 --
   * lower 3 bits must be zero (8-byte alignment). */
  if (((uintptr_t)buffer & k_ra8_ceu_buffer_align_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_ceu_buffers_t bufs = {
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

  const ra8_err_t arm_err = internal_arm_capture(&bufs);
  if (arm_err != k_ra8_ok) {
    return arm_err;
  }
  ra8_log_info_val(s_tag, "ceu_capture_start buf", (uint32_t)(uintptr_t)buffer);
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_capture_start_ex(const ra8_ceu_buffers_t* bufs)
{
  RA8_CHECK_NULL_PTR(bufs, s_tag, "bufs must not be nullptr");
  RA8_CHECK_NULL_PTR(bufs->y_top, s_tag, "bufs->y_top must not be nullptr");

  const ra8_err_t align_err = internal_validate_buffers(bufs);
  if (align_err != k_ra8_ok) {
    return align_err;
  }

  return internal_arm_capture(bufs);
}

ra8_err_t ra8_ceu_capture_disarm(void)
{
  /* HUM Ch 60.2.1 "CAPSR : Capture Start Register" p 3630 -- clear
   * CE so the next VD does not start a new frame; in-flight frame
   * runs to completion. */
  volatile uint32_t* reg = ra8_ceu_reg32(k_ra8_ceu_off_capsr);
  const uint32_t     val = *reg;
  *reg                   = val & ~k_ra8_ceu_capsr_mask_ce;
  return k_ra8_ok;
}

/**
 * @brief Mirror every 3-plane register from Plane A to Plane B.
 *
 * @details
 * HUM Ch 60.2 Table 60.4 "Register configuration of CEU" p 3629. Used
 * by ``ra8_ceu_plane_b_program`` to build a clean baseline before the
 * caller's per-bundle overrides are written on top.
 *
 * @pre Module clock ungated; engine in plane-pending state.
 * @post Plane B mirrors Plane A for every register listed below.
 *
 * @note Internal helper, not thread-safe.
 *
 * @since 0.1.0
 *
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL
static void internal_plane_b_mirror_from_a(void)
{
  static const ra8_ceu_off_t s_plane_offsets[] = {
    k_ra8_ceu_off_camor,
    k_ra8_ceu_off_capwr,
    k_ra8_ceu_off_cflcr,
    k_ra8_ceu_off_cfszr,
    k_ra8_ceu_off_cdwdr,
    k_ra8_ceu_off_cdayr,
    k_ra8_ceu_off_cdacr,
    k_ra8_ceu_off_cdbyr,
    k_ra8_ceu_off_cdbcr,
    k_ra8_ceu_off_cbdsr,
    k_ra8_ceu_off_clfcr,
    k_ra8_ceu_off_cdocr,
    k_ra8_ceu_off_cdayr2,
    k_ra8_ceu_off_cdacr2,
    k_ra8_ceu_off_cdbyr2,
    k_ra8_ceu_off_cdbcr2,
  };
  for (uint32_t i = 0U; i < (sizeof(s_plane_offsets) / sizeof(s_plane_offsets[0])); i++) {
    const uint32_t a = *ra8_ceu_reg32_plane(s_plane_offsets[i], k_ra8_ceu_plane_a_off);
    *ra8_ceu_reg32_plane(s_plane_offsets[i], k_ra8_ceu_plane_b_off) = a;
  }
}

/**
 * @brief Apply caller-supplied buffer overrides to Plane B.
 *
 * @details
 * Each non-null pointer in ``bufs`` overwrites the matching CDxyR /
 * CDxyR2 register in Plane B. ``bundle_size_bytes`` overrides CBDSR
 * after rounding to the documented alignment mask.
 *
 * @param[in] bufs Caller-supplied buffer set; must not be nullptr.
 *
 * @pre ``bufs`` already passed ``internal_validate_buffers``.
 * @pre Plane B has been mirrored from Plane A.
 * @post Plane B reflects the overrides; Plane A unchanged.
 *
 * @note Internal helper, not thread-safe.
 *
 * @since 0.1.0
 *
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL
static void internal_plane_b_apply_overrides(const ra8_ceu_buffers_t* bufs)
{
  if (bufs->y_top != nullptr) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdayr, k_ra8_ceu_plane_b_off) =
      (uint32_t)(uintptr_t)bufs->y_top;
  }
  if (bufs->c_top != nullptr) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdacr, k_ra8_ceu_plane_b_off) =
      (uint32_t)(uintptr_t)bufs->c_top;
  }
  if (bufs->y_bottom != nullptr) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdbyr, k_ra8_ceu_plane_b_off) =
      (uint32_t)(uintptr_t)bufs->y_bottom;
  }
  if (bufs->c_bottom != nullptr) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdbcr, k_ra8_ceu_plane_b_off) =
      (uint32_t)(uintptr_t)bufs->c_bottom;
  }
  if (bufs->y_top_2 != nullptr) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdayr2, k_ra8_ceu_plane_b_off) =
      (uint32_t)(uintptr_t)bufs->y_top_2;
  }
  if (bufs->c_top_2 != nullptr) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdacr2, k_ra8_ceu_plane_b_off) =
      (uint32_t)(uintptr_t)bufs->c_top_2;
  }
  if (bufs->y_bottom_2 != nullptr) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdbyr2, k_ra8_ceu_plane_b_off) =
      (uint32_t)(uintptr_t)bufs->y_bottom_2;
  }
  if (bufs->c_bottom_2 != nullptr) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cdbcr2, k_ra8_ceu_plane_b_off) =
      (uint32_t)(uintptr_t)bufs->c_bottom_2;
  }
  if (bufs->bundle_size_bytes != 0U) {
    *ra8_ceu_reg32_plane(k_ra8_ceu_off_cbdsr, k_ra8_ceu_plane_b_off) =
      bufs->bundle_size_bytes & ~k_ra8_ceu_bundle_align_mask;
  }
}

ra8_err_t ra8_ceu_plane_b_program(const ra8_ceu_buffers_t* bufs)
{
  if (bufs != nullptr) {
    const ra8_err_t align_err = internal_validate_buffers(bufs);
    if (align_err != k_ra8_ok) {
      return align_err;
    }
  }

  internal_plane_b_mirror_from_a();
  if (bufs != nullptr) {
    internal_plane_b_apply_overrides(bufs);
  }

  /* HUM Ch 60.2.8 "CRCNTR : CEU Register Control Register" p 3649 --
   * enable both planes and arm the VD-synchronised swap. */
  *ra8_ceu_reg32(k_ra8_ceu_off_crcntr) =
    k_ra8_ceu_crcntr_mask_rc | k_ra8_ceu_crcntr_mask_rs | k_ra8_ceu_crcntr_mask_rvs;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_plane_swap_force(void)
{
  /* HUM Ch 60.2.9 "CRCMPR : CEU Register Forcible Control Register" p 3650 */
  *ra8_ceu_reg32(k_ra8_ceu_off_crcmpr) = k_ra8_ceu_crcmpr_mask_ra;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_firewall_set(bool enable, uint32_t upper_bound)
{
  uint32_t cfwcr = 0U;
  if (enable) {
    cfwcr |= k_ra8_ceu_cfwcr_mask_fwe;
    cfwcr |= (upper_bound & k_ra8_ceu_cfwcr_mask_fwv);
  }
  /* HUM Ch 60.2.18 "CFWCR : Firewall Operation Control Register" p 3661 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cfwcr) = cfwcr;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_byte_swap_set(const ra8_ceu_byte_swap_t* swap)
{
  RA8_CHECK_NULL_PTR(swap, s_tag, "swap must not be nullptr");
  /* HUM Ch 60.2.20 "CDOCR : Capture Data Output Control Register" p 3662 */
  volatile uint32_t* reg     = ra8_ceu_reg32(k_ra8_ceu_off_cdocr);
  uint32_t           current = *reg;
  current &= ~(k_ra8_ceu_cdocr_mask_cobs | k_ra8_ceu_cdocr_mask_cows | k_ra8_ceu_cdocr_mask_cols);
  if (swap->swap_8_bit) {
    current |= k_ra8_ceu_cdocr_mask_cobs;
  }
  if (swap->swap_16_bit) {
    current |= k_ra8_ceu_cdocr_mask_cows;
  }
  if (swap->swap_32_bit) {
    current |= k_ra8_ceu_cdocr_mask_cols;
  }
  *reg = current;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_bundle_size_set(uint32_t size_bytes)
{
  /* HUM Ch 60.2.17 "CBDSR : Capture Bundle Destination Size" p 3660 */
  *ra8_ceu_reg32(k_ra8_ceu_off_cbdsr) = size_bytes & ~k_ra8_ceu_bundle_align_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_low_pass_set(bool enable)
{
  /* HUM Ch 60.2.19 "CLFCR : Capture Low-Pass Filter Control" p 3662 */
  uint32_t value = 0U;
  if (enable) {
    value = k_ra8_ceu_clfcr_mask_lpf;
  }
  *ra8_ceu_reg32(k_ra8_ceu_off_clfcr) = value;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_capture_mode_set(ra8_ceu_capture_mode_t mode)
{
  /* HUM Ch 60.2.2 "CAPCR : Capture Control Register" p 3634 */
  volatile uint32_t* reg     = ra8_ceu_reg32(k_ra8_ceu_off_capcr);
  uint32_t           current = *reg;
  current &= ~k_ra8_ceu_capcr_mask_ctncp;
  if (mode == k_ra8_ceu_capture_continuous) {
    current |= k_ra8_ceu_capcr_mask_ctncp;
  }
  *reg = current;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_frame_drop_set(uint8_t count)
{
  /* HUM Ch 60.2.2 "CAPCR : Capture Control Register" p 3634 */
  volatile uint32_t* reg     = ra8_ceu_reg32(k_ra8_ceu_off_capcr);
  uint32_t           current = *reg;
  current &= ~k_ra8_ceu_capcr_mask_fdrp;
  current |= ((uint32_t)count) << k_ra8_ceu_capcr_shift_fdrp;
  *reg = current;
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_dma_pump(uint8_t channel, const uint8_t* src, uint8_t* dst, uint32_t bytes)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(dst, s_tag, "dst must not be nullptr");
  if (bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((bytes & k_ra8_ceu_dma_byte_align_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }

  /* The CEU writes pixels with its own bus initiator; the DMAC is only
   * used by the application-level pump that ferries finished
   * frames. Configure for word-wide block transfer with both
   * pointers incrementing. */
  const ra8_dmac_config_t cfg = {
    .src     = (uint32_t)(uintptr_t)src,
    .dst     = (uint32_t)(uintptr_t)dst,
    .count   = (uint16_t)(bytes >> k_ra8_ceu_dma_count_shift),
    .width   = k_ra8_dmac_width_word,
    .src_inc = true,
    .dst_inc = true,
  };
  return ra8_dmac_start(channel, &cfg);
}

/* =============================================================================
 * Sweep 17: DMA-driven framebuffer + multi-frame capture
 * =============================================================================
 */

ra8_err_t ra8_ceu_set_dma_buffer(uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (((uintptr_t)buf & k_ra8_ceu_buffer_align_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  s_ceu_dma_buf = buf;
  s_ceu_dma_len = len;
  ra8_log_info_val(s_tag, "ceu_set_dma_buffer len", len);
  return k_ra8_ok;
}

ra8_err_t ra8_ceu_capture_start(uint32_t num_frames)
{
  if (s_ceu_dma_buf == nullptr) {
    return k_ra8_err_invalid_state;
  }
  /* HUM Ch 60.2.2 "CAPCR" p 3634-3635 -- CTNCP[16] = 1 selects
 * continuous capture, 0 selects single-shot. */
  volatile uint32_t* capcr_reg = ra8_ceu_reg32(k_ra8_ceu_off_capcr);
  uint32_t           capcr     = *capcr_reg;
  if (num_frames == 0U) {
    capcr |= ((uint32_t)1U << k_ra8_ceu_capcr_shift_ctncp);
  } else {
    capcr &= ~((uint32_t)1U << k_ra8_ceu_capcr_shift_ctncp);
  }
  *capcr_reg = capcr;

  return ra8_ceu_capture_arm(s_ceu_dma_buf);
}

ra8_err_t ra8_ceu_capture_stop(void)
{
  return ra8_ceu_capture_disarm();
}
