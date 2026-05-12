/**
 * @file ra_display_pal_guix.c
 * @brief Display PAL -> GUIX bind dispatcher.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Backend-agnostic wrapper around the iface's optional
 * ``bind_guix`` hook. Backends with a GUIX driver shim populate the
 * hook; backends without one leave it NULL and this dispatcher
 * returns ``k_ra_err_not_supported``.
 *
 * Casts the opaque ``void*`` setup-fn the iface returns back to the
 * ``UINT (*)(GX_DISPLAY*)`` signature ``gx_display_create`` expects
 * (skipped under ``RA_SIMULATOR_MODE`` since host tests do not link
 * the GUIX vendor tree).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_display_pal_guix.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_display_pal.h"
#include "ra_display_pal_internal.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_display_pal_guix";

/* display_bind_guix -- see header for full description. */
ra_err_t display_bind_guix(display_handle_t* d, display_guix_attach_t* out)
{
  RA_CHECK_NULL_PTR(d, s_tag, "d");
  RA_CHECK_NULL_PTR(out, s_tag, "out");
  if (d->iface == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (d->iface->bind_guix == nullptr) {
    return k_ra_err_not_supported;
  }

  void*          raw_setup_fn = nullptr;
  uint16_t       width_px     = 0U;
  uint16_t       height_px    = 0U;
  const ra_err_t err          = d->iface->bind_guix(d->ctx, &raw_setup_fn, &width_px, &height_px);
  if (err != k_ra_ok) {
    return err;
  }
  /* Carry the GUIX-typed callback through as a generic
   * ``void (*)(void)``.  Apps that consume ``out`` cast it back to
   * the real ``UINT (*)(GX_DISPLAY*)`` signature at the call site
   * where the GUIX headers are already in scope. */
  out->setup_fn  = (void (*)(void))raw_setup_fn;
  out->width_px  = width_px;
  out->height_px = height_px;
  return k_ra_ok;
}
