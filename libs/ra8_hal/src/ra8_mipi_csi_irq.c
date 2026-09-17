/**
 * @file ra8_mipi_csi_irq.c
 * @brief MIPI CSI-2 receiver HAL driver -- interrupt path and per-VC framing
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Companion translation unit to ``ra8_mipi_csi.c``. Owns the
 * interrupt-driven callback surface of the RA8D2 MIPI CSI-2 receiver:
 * the attach-handler registration functions, the module / data-lane /
 * virtual-channel / power-management / short-packet dispatchers, and
 * the per-VC framing helpers (data-format shadow + error reporting)
 * from HUM Ch 66 ("MIPI CSI Interface" p 3935-3972).
 *
 * All the mutable callback function-pointer and context statics, plus
 * the per-VC format shadow and saved-VCIE arrays, live here. The
 * lifecycle code in ``ra8_mipi_csi.c`` detaches every callback through
 * the promoted ::priv_ra8_mipi_csi_detach_all_handlers helper so that the
 * mutable state stays owned by a single translation unit.
 *
 * Every register access carries a HUM Ch 66 citation immediately
 * above the access, per project policy.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hal_internal.h"
#include "ra8_log.h"
#include "ra8_mipi_csi.h"
#include "ra8_mipi_csi_regs.h"
#include "ra8_mstp.h"

/**
 * @var s_tag
 * @brief Log tag prefix used by every ``ra8_log_*`` call below.
 *
 * @note Module-private; do not expose.
 */
static const char* s_tag = "MIPI_CSI";

/**
 * @enum ra8_mipi_csi_irq_intern_t
 * @brief Internal limits used by this translation unit only.
 *
 * @details
 * ``k_ra8_mipi_csi_vc_invalid_idx`` is the sentinel VC id forwarded to
 * the per-VC callback when reporting the "generic" (non-VC-attributable)
 * MLF/ECD error aggregate.
 */
typedef enum : uint16_t {
  k_ra8_mipi_csi_vc_invalid_idx = 0xFFU, /**< Sentinel for "generic" event. */
} ra8_mipi_csi_irq_intern_t;

/**
 * @var s_mipi_csi_fn
 * @brief Currently-attached receive event callback.
 * @note File-scope; updated only by ``ra8_mipi_csi_attach_handler``.
 */
static ra8_mipi_csi_event_fn_t s_mipi_csi_fn;

/**
 * @var s_mipi_csi_ctx
 * @brief Caller context forwarded to the receive event callback.
 * @note File-scope; updated only by ``ra8_mipi_csi_attach_handler``.
 */
static void* s_mipi_csi_ctx;

/**
 * @var s_mipi_csi_dl_fn
 * @brief Currently-attached data-lane callback.
 * @note File-scope; updated only by ``ra8_mipi_csi_attach_dl_handler``.
 */
static ra8_mipi_csi_dl_event_fn_t s_mipi_csi_dl_fn;

/**
 * @var s_mipi_csi_dl_ctx
 * @brief Caller context forwarded to the data-lane callback.
 * @note File-scope; updated only by ``ra8_mipi_csi_attach_dl_handler``.
 */
static void* s_mipi_csi_dl_ctx;

/**
 * @var s_mipi_csi_vc_fn
 * @brief Currently-attached per-VC callback.
 * @note File-scope; updated only by ``ra8_mipi_csi_attach_vc_handler``.
 */
static ra8_mipi_csi_vc_event_fn_t s_mipi_csi_vc_fn;

/**
 * @var s_mipi_csi_vc_ctx
 * @brief Caller context forwarded to the per-VC callback.
 * @note File-scope; updated only by ``ra8_mipi_csi_attach_vc_handler``.
 */
static void* s_mipi_csi_vc_ctx;

/**
 * @var s_mipi_csi_pm_fn
 * @brief Currently-attached power-management callback.
 * @note File-scope; updated only by ``ra8_mipi_csi_attach_pm_handler``.
 */
static ra8_mipi_csi_pm_event_fn_t s_mipi_csi_pm_fn;

/**
 * @var s_mipi_csi_pm_ctx
 * @brief Caller context forwarded to the PM callback.
 * @note File-scope; updated only by ``ra8_mipi_csi_attach_pm_handler``.
 */
static void* s_mipi_csi_pm_ctx;

/**
 * @var s_mipi_csi_gst_fn
 * @brief Currently-attached short-packet FIFO callback.
 * @note File-scope; updated only by
 *       ``ra8_mipi_csi_attach_short_packet_handler``.
 */
static ra8_mipi_csi_short_event_fn_t s_mipi_csi_gst_fn;

/**
 * @var s_mipi_csi_gst_ctx
 * @brief Caller context forwarded to the short-packet callback.
 * @note File-scope; updated only by
 *       ``ra8_mipi_csi_attach_short_packet_handler``.
 */
static void* s_mipi_csi_gst_ctx;

/**
 * @var s_mipi_csi_err_fn
 * @brief Callback invoked from ``dispatch_vc`` when an ECC/CRC error
 *        is decoded out of a VCST snapshot.
 * @note File-scope; updated only by
 *       ``ra8_mipi_csi_attach_error_handler``.
 */
static ra8_mipi_csi_error_fn_t s_mipi_csi_err_fn;

/**
 * @var s_mipi_csi_err_ctx
 * @brief Caller context forwarded to the error callback.
 * @note File-scope; updated only by
 *       ``ra8_mipi_csi_attach_error_handler``.
 */
static void* s_mipi_csi_err_ctx;

/**
 * @var s_mipi_csi_vc_format
 * @brief Software shadow of the per-VC payload format selection
 *        published by ``ra8_mipi_csi_set_data_format``.
 * @note Indexed by VC id (0..15). 0 means "no format selected".
 */
static uint8_t s_mipi_csi_vc_format[k_ra8_mipi_csi_vc_count];

/**
 * @var s_mipi_csi_vcie_saved
 * @brief Snapshot of VCIE(M) at the time the last
 *        ``ra8_mipi_csi_set_virtual_channels`` call masked it off.
 *        Restored when the channel is re-enabled.
 */
static uint32_t s_mipi_csi_vcie_saved[k_ra8_mipi_csi_vc_count];

/* =============================================================================
 * Interrupt path (handlers + dispatchers)
 * =============================================================================
 */

/**
 * @brief Attach the module-level CSI event callback.
 *
 * @details Stores the supplied callback and opaque context for later
 * invocation by ::ra8_mipi_csi_dispatch when a module-level event fires.
 *
 * @param[in] fn  Event-callback function pointer (NULL to detach).
 * @param[in] ctx Opaque context passed back to the callback.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok Always returned in the current implementation.
 *
 * @pre Module has been initialized via ::ra8_mipi_csi_init.
 * @pre Caller serialises with the IRQ source if registering at runtime.
 * @post Subsequent ::ra8_mipi_csi_dispatch calls invoke ``fn`` with ``ctx``.
 * @post Previously registered callback (if any) is overwritten.
 *
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
ra8_err_t ra8_mipi_csi_attach_handler(ra8_mipi_csi_event_fn_t fn, void* ctx)
{
  s_mipi_csi_fn  = fn;
  s_mipi_csi_ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_attach_dl_handler(ra8_mipi_csi_dl_event_fn_t fn, void* ctx)
{
  s_mipi_csi_dl_fn  = fn;
  s_mipi_csi_dl_ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_attach_vc_handler(ra8_mipi_csi_vc_event_fn_t fn, void* ctx)
{
  s_mipi_csi_vc_fn  = fn;
  s_mipi_csi_vc_ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_attach_pm_handler(ra8_mipi_csi_pm_event_fn_t fn, void* ctx)
{
  s_mipi_csi_pm_fn  = fn;
  s_mipi_csi_pm_ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_attach_short_packet_handler(ra8_mipi_csi_short_event_fn_t fn, void* ctx)
{
  s_mipi_csi_gst_fn  = fn;
  s_mipi_csi_gst_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_mipi_csi_dispatch(void)
{
  /* HUM Ch 66.3.12 "RXST : Receive Status Register" p 3944 */
  const uint32_t rxst = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxst);
  /* HUM Ch 66.3.13 "RXSC : Receive Status Clear Register" p 3945
   * Clear sticky RACTDET so the next edge can be observed. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_rxsc) = rxst & k_ra8_mipi_csi_rxsc_ractdetc_mask;

  const ra8_mipi_csi_event_fn_t fn  = s_mipi_csi_fn;
  void* const                   ctx = s_mipi_csi_ctx;
  if (fn != nullptr) {
    fn(ctx, rxst);
  }
}

RA8_ISR_SAFE
void ra8_mipi_csi_dispatch_dl(void)
{
  /* HUM Ch 66.3.9 "MIST : Module Interrupt Status" p 3941 */
  const uint32_t mist = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist);

  /* HUM Ch 66.3.15 "DLST(N) : Data Lane N Status" p 3946 */
  const uint32_t dlst0 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlst0);
  const uint32_t dlst1 = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlst1);

  /* HUM Ch 66.3.16 "DLSC(N) : Data Lane N Status Clear" p 3947
   * ULP bit (24) is RO; mask it out before W1C. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlsc0) = dlst0 & k_ra8_mipi_csi_dlsc_all_mask;
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dlsc1) = dlst1 & k_ra8_mipi_csi_dlsc_all_mask;

  const ra8_mipi_csi_dl_event_fn_t fn  = s_mipi_csi_dl_fn;
  void* const                      ctx = s_mipi_csi_dl_ctx;
  if (fn == nullptr) {
    return;
  }

  if ((mist & k_ra8_mipi_csi_mist_dl0s_mask) != 0UL) {
    fn(ctx, 0U, dlst0);
  }
  if ((mist & k_ra8_mipi_csi_mist_dl1s_mask) != 0UL) {
    fn(ctx, 1U, dlst1);
  }
}

RA8_ISR_SAFE
void ra8_mipi_csi_dispatch_vc(void)
{
  /* HUM Ch 66.3.9 "MIST : Module Interrupt Status" p 3941 */
  const uint32_t mist     = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_mist);
  const uint32_t vc_flags = (mist & k_ra8_mipi_csi_mist_vc_mask) >> k_ra8_mipi_csi_mist_vc_shift;

  const ra8_mipi_csi_vc_event_fn_t fn  = s_mipi_csi_vc_fn;
  void* const                      ctx = s_mipi_csi_vc_ctx;

  uint32_t generic_errors = 0UL;
  for (uint8_t vc = 0U; vc < (uint8_t)k_ra8_mipi_csi_vc_count; ++vc) {
    if ((vc_flags & ((uint32_t)1U << vc)) == 0UL) {
      continue;
    }
    /* HUM Ch 66.3.18 "VCST(M) : Virtual Channel M Status" p 3949 */
    const ra8_mipi_csi_off_t st_off  = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcst0, vc);
    const ra8_mipi_csi_off_t clr_off = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcsc0, vc);
    const uint32_t           vcst    = *ra8_mipi_csi_reg32(st_off);

    /* HUM Ch 66.3.19 "VCSC(M) : Virtual Channel M Status Clear" p 3951 */
    *ra8_mipi_csi_reg32(clr_off) = vcst & k_ra8_mipi_csi_vcsc_per_vc_mask;

    /* MLF + ECD apply to every VC because the offending packet's VC
     * cannot be determined. Surface them once at the end. */
    generic_errors |= vcst & k_ra8_mipi_csi_vcst_generic_err_mask;
    const uint32_t vc_specific = vcst & ~k_ra8_mipi_csi_vcst_generic_err_mask;

    if (fn != nullptr) {
      fn(ctx, vc, vc_specific);
    }

    /* Surface ECC + CRC errors to the dedicated error handler if any
     * are set in this VC's snapshot. HUM Ch 66.3.18 p 3949. */
    const ra8_mipi_csi_error_fn_t err_fn = s_mipi_csi_err_fn;
    if (err_fn != nullptr) {
      const uint32_t err_bits =
        vcst & (k_ra8_mipi_csi_vcst_ecc_mask | k_ra8_mipi_csi_vcst_ecd_mask |
                k_ra8_mipi_csi_vcst_crc_mask);
      if (err_bits != 0UL) {
        const ra8_mipi_csi_error_report_t report = {
          .vc                = vc,
          .ecc_corrected     = ((vcst & k_ra8_mipi_csi_vcst_ecc_mask) != 0UL),
          .ecc_two_bit_error = ((vcst & k_ra8_mipi_csi_vcst_ecd_mask) != 0UL),
          .crc_error         = ((vcst & k_ra8_mipi_csi_vcst_crc_mask) != 0UL),
          .raw_vcst          = vcst,
        };
        err_fn(s_mipi_csi_err_ctx, &report);
      }
    }
  }

  if (fn != nullptr) {
    fn(ctx, (uint8_t)k_ra8_mipi_csi_vc_invalid_idx, generic_errors);
  }
}

RA8_ISR_SAFE
void ra8_mipi_csi_dispatch_pm(void)
{
  /* HUM Ch 66.3.21 "PMST : Power Management Status" p 3954 */
  const uint32_t pmst = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmst);
  /* HUM Ch 66.3.22 "PMSC : Power Management Status Clear" p 3955
   * Only [7:0] are W1C; the upper state bits are RO. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_pmsc) = pmst & k_ra8_mipi_csi_pmst_w1c_mask;

  const ra8_mipi_csi_pm_event_fn_t fn  = s_mipi_csi_pm_fn;
  void* const                      ctx = s_mipi_csi_pm_ctx;
  if (fn != nullptr) {
    fn(ctx, pmst);
  }
}

RA8_ISR_SAFE
void ra8_mipi_csi_dispatch_short_packet(void)
{
  /* HUM Ch 66.3.25 "GSST : Generic Short Packet Status" p 3957 */
  const uint32_t gsst = *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gsst);
  /* HUM Ch 66.3.26 "GSSC : Generic Short Packet Status Clear" p 3958
   * Clear GOV so the next overflow can be observed. */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_gssc) = gsst & k_ra8_mipi_csi_gssc_govc_mask;

  const ra8_mipi_csi_short_event_fn_t fn  = s_mipi_csi_gst_fn;
  void* const                         ctx = s_mipi_csi_gst_ctx;
  if (fn != nullptr) {
    fn(ctx, gsst);
  }
}

void priv_ra8_mipi_csi_detach_all_handlers(void)
{
  s_mipi_csi_fn      = nullptr;
  s_mipi_csi_ctx     = nullptr;
  s_mipi_csi_dl_fn   = nullptr;
  s_mipi_csi_dl_ctx  = nullptr;
  s_mipi_csi_vc_fn   = nullptr;
  s_mipi_csi_vc_ctx  = nullptr;
  s_mipi_csi_pm_fn   = nullptr;
  s_mipi_csi_pm_ctx  = nullptr;
  s_mipi_csi_gst_fn  = nullptr;
  s_mipi_csi_gst_ctx = nullptr;
}

/* =============================================================================
 * Sweep 6: per-VC framing helpers + error reporting
 * =============================================================================
 */

/* Translate a public ::ra8_mipi_csi_data_format_t to its DT bit -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t
internal_format_to_bit(ra8_mipi_csi_data_format_t format, bool* is_high, uint32_t* bit)
{
  switch (format) {
    case k_ra8_mipi_csi_format_off: {
      *is_high = false;
      *bit     = 0UL;
      return k_ra8_ok;
    }
    case k_ra8_mipi_csi_format_yuv422_8: {
      *is_high = false;
      *bit     = k_ra8_mipi_csi_dtel_yuv422_8_mask;
      return k_ra8_ok;
    }
    case k_ra8_mipi_csi_format_yuv422_10: {
      *is_high = false;
      *bit     = k_ra8_mipi_csi_dtel_yuv422_10_mask;
      return k_ra8_ok;
    }
    case k_ra8_mipi_csi_format_rgb888: {
      *is_high = true;
      *bit     = k_ra8_mipi_csi_dteh_rgb888_mask;
      return k_ra8_ok;
    }
    case k_ra8_mipi_csi_format_raw8: {
      *is_high = true;
      *bit     = k_ra8_mipi_csi_dteh_raw8_mask;
      return k_ra8_ok;
    }
    case k_ra8_mipi_csi_format_raw10:
    case k_ra8_mipi_csi_format_yuv420:
    default: {
      /* RAW10 / YUV420 are not exposed by RA8D2 DTEL/DTEH. */
      return k_ra8_err_invalid_arg;
    }
  }
}

/* Recompute DTEL/DTEH from the per-VC format shadow -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_recompute_dt_filter(void)
{
  uint32_t low  = 0UL;
  uint32_t high = 0UL;
  for (uint8_t vc = 0U; vc < (uint8_t)k_ra8_mipi_csi_vc_count; ++vc) {
    bool                             is_high = false;
    uint32_t                         bit     = 0UL;
    const ra8_mipi_csi_data_format_t fmt     = (ra8_mipi_csi_data_format_t)s_mipi_csi_vc_format[vc];
    /* Unknown shadow values would have been rejected by setter -- safe. */
    (void)internal_format_to_bit(fmt, &is_high, &bit);
    if (is_high) {
      high |= bit;
    } else {
      low |= bit;
    }
  }
  /* HUM Ch 66.3.10 "DTEL : Receive Data Type Enable Low" p 3943 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dtel) = low;
  /* HUM Ch 66.3.11 "DTEH : Receive Data Type Enable High" p 3944 */
  *ra8_mipi_csi_reg32(k_ra8_mipi_csi_off_dteh) = high;
}

ra8_err_t ra8_mipi_csi_set_virtual_channels(uint16_t vc_mask)
{
  if (vc_mask == 0U) {
    ra8_log_error(s_tag, "set_virtual_channels: empty mask rejected");
    return k_ra8_err_invalid_arg;
  }
  for (uint8_t vc = 0U; vc < (uint8_t)k_ra8_mipi_csi_vc_count; ++vc) {
    /* HUM Ch 66.3.20 "VCIE(M) : Virtual Channel M Interrupt Enable" p 3952 */
    const ra8_mipi_csi_off_t off  = ra8_mipi_csi_vc_off(k_ra8_mipi_csi_off_vcie0, vc);
    const bool               keep = ((vc_mask & ((uint16_t)1U << vc)) != 0U);
    if (keep) {
      /* If we previously masked this VC off, restore the saved enable.
       * Otherwise leave whatever the caller has currently programmed.  */
      const uint32_t saved = s_mipi_csi_vcie_saved[vc];
      if (saved != 0UL) {
        *ra8_mipi_csi_reg32(off)  = saved;
        s_mipi_csi_vcie_saved[vc] = 0UL;
      }
    } else {
      const uint32_t cur = *ra8_mipi_csi_reg32(off);
      if (cur != 0UL) {
        s_mipi_csi_vcie_saved[vc] = cur;
      }
      *ra8_mipi_csi_reg32(off) = 0UL;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_set_data_format(uint8_t vc, ra8_mipi_csi_data_format_t format)
{
  if (vc >= (uint8_t)k_ra8_mipi_csi_vc_count) {
    return k_ra8_err_invalid_arg;
  }
  bool            is_high = false;
  uint32_t        bit     = 0UL;
  const ra8_err_t lookup  = internal_format_to_bit(format, &is_high, &bit);
  if (lookup != k_ra8_ok) {
    return lookup;
  }
  s_mipi_csi_vc_format[vc] = (uint8_t)format;
  internal_recompute_dt_filter();
  return k_ra8_ok;
}

ra8_err_t ra8_mipi_csi_attach_error_handler(ra8_mipi_csi_error_fn_t fn, void* ctx)
{
  s_mipi_csi_err_fn  = fn;
  s_mipi_csi_err_ctx = ctx;
  return k_ra8_ok;
}
