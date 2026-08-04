/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_display_pal.c
 * @brief Display PAL dispatcher -- forwards every public call into
 *        the bound backend's vtable.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Owns one module-static ``display_handle`` and one
 * ``s_initialized`` flag. The dispatcher does no hardware work
 * itself; its only job is to validate arguments, dispatch through
 * the iface, and refuse re-init while a handle is live.
 */

#include "ra8_display_pal.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_display_pal_internal.h"
#include "ra8_err.h"
#include "ra8_log.h"

/* =============================================================================
 * Module-static storage
 * =============================================================================
 */

/** @brief Module log tag. */
static const char* const s_tag = "ra8_display_pal";

/** @brief Single PAL handle -- one display per board. */
static struct display_handle s_handle;

/** @brief True once ``display_init`` has succeeded. */
static bool s_initialized = false;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Reject ``d`` if it is NULL or refers to an un-initialised handle.
 *
 * @details
 * Every dispatch path runs this. Kept as a tiny inline so each
 * caller stays well under the NASA P10 Rule 4 60-line cap.
 *
 * @param[in] d Candidate handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               ``d`` points at the live handle.
 * @retval k_ra8_err_null_ptr     ``d`` was NULL.
 * @retval k_ra8_err_invalid_arg  ``d`` did not match the live handle
 *                               or no handle is initialised.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Returned value reflects the current init flag only.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_handle(const display_handle_t* d)
{
  if (d == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!s_initialized) {
    return k_ra8_err_invalid_arg;
  }
  if (d != &s_handle) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t display_init(const display_cfg_t* cfg, display_handle_t** out_handle)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  RA8_CHECK_NULL_PTR(out_handle, s_tag, "out_handle must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->iface, s_tag, "cfg->iface must not be nullptr");
  RA8_CHECK_NULL_PTR(cfg->iface->init, s_tag, "iface->init must not be nullptr");

  if (s_initialized) {
    ra8_log_error(s_tag, "display_init: PAL already initialised");
    return k_ra8_err_busy;
  }

  void*           backend_ctx = nullptr;
  const ra8_err_t err         = cfg->iface->init(cfg, &backend_ctx);
  if (err != k_ra8_ok) {
    return err;
  }

  s_handle.iface = cfg->iface;
  s_handle.ctx   = backend_ctx;
  s_initialized  = true;
  *out_handle    = &s_handle;
  ra8_log_info(s_tag, "display_init: backend bound");
  return k_ra8_ok;
}

ra8_err_t display_get_caps(const display_handle_t* d, display_caps_t* out)
{
  const ra8_err_t v = internal_validate_handle(d);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return s_handle.iface->get_caps(s_handle.ctx, out);
}

ra8_err_t display_get_framebuffer(display_handle_t* d, display_fb_t* out)
{
  const ra8_err_t v = internal_validate_handle(d);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return s_handle.iface->get_framebuffer(s_handle.ctx, out);
}

ra8_err_t display_flush(display_handle_t* d, display_rect_t rect, display_refresh_hint_t hint)
{
  const ra8_err_t v = internal_validate_handle(d);
  if (v != k_ra8_ok) {
    return v;
  }
  return s_handle.iface->flush(s_handle.ctx, rect, hint);
}

ra8_err_t display_clear(display_handle_t* d, uint32_t color)
{
  const ra8_err_t v = internal_validate_handle(d);
  if (v != k_ra8_ok) {
    return v;
  }
  return s_handle.iface->clear(s_handle.ctx, color);
}

ra8_err_t display_deinit(display_handle_t* d)
{
  const ra8_err_t v = internal_validate_handle(d);
  if (v != k_ra8_ok) {
    return v;
  }
  const ra8_err_t err = s_handle.iface->deinit(s_handle.ctx);
  /* Always drop the handle on deinit so a follow-up init can run, even
   * if the backend reported a tear-down error.  The deinit error is
   * still returned to the caller. */
  s_handle.iface = nullptr;
  s_handle.ctx   = nullptr;
  s_initialized  = false;
  return err;
}

display_rect_t display_full_rect(const display_handle_t* d)
{
  const display_rect_t empty = {0U, 0U, 0U, 0U};
  if (internal_validate_handle(d) != k_ra8_ok) {
    return empty;
  }
  display_caps_t  caps = {};
  const ra8_err_t e    = s_handle.iface->get_caps(s_handle.ctx, &caps);
  if (e != k_ra8_ok) {
    return empty;
  }
  const display_rect_t full = {0U, 0U, caps.width_px, caps.height_px};
  return full;
}
