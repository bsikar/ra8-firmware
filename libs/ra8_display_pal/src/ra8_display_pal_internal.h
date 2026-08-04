/**
 * @file ra8_display_pal_internal.h
 * @brief Internal vtable + handle types for the display PAL
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Not part of the public API. Only the PAL dispatcher and the
 * backend TUs include this file. Tests under ``tests/`` MAY also
 * include it to drive MC/DC vectors against internal helpers, but
 * application code never should.
 *
 * Defines:
 *
 *   - ``display_backend_iface`` -- the function-pointer table each
 *     backend fills in. Forward-declared as opaque in
 *     ``ra8_display_pal.h``.
 *   - ``display_handle`` -- the concrete struct that
 *     ``ra8_display_pal.h`` forward-declares as opaque. Holds the
 *     bound iface and the backend's private context pointer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_display_pal.h"
#include "ra8_err.h"

/**
 * @struct display_backend_iface
 * @brief One vtable per backend. Each callback receives the backend's
 *        opaque ``ctx`` so backends can keep their state private.
 *
 * @details
 * Backends fill in a ``static const display_backend_iface_t`` in
 * their TU and export its address through their public
 * ``ra8_display_pal_<name>.h`` header. The dispatcher in
 * ``ra8_display_pal.c`` is the only consumer at run time.
 *
 * @invariant All function pointers are non-NULL.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
struct display_backend_iface {
  /**
   * @brief Bring the backend up against ``cfg`` and return its
   *        private context pointer through ``*out_ctx``.
   *
   * @details The init callback runs the actual hardware bring-up
   *          (panel power, controller init, scan-out enable for
   *          LCD; SPI setup + waveform load for e-ink).
   */
  ra8_err_t (*init)(const display_cfg_t* cfg, void** out_ctx);

  /** @brief Fill in ``*out`` with the backend's capabilities. */
  ra8_err_t (*get_caps)(const void* ctx, display_caps_t* out);

  /** @brief Hand out the framebuffer descriptor for paint loops. */
  ra8_err_t (*get_framebuffer)(void* ctx, display_fb_t* out);

  /** @brief Commit pending FB changes to the panel. */
  ra8_err_t (*flush)(void* ctx, display_rect_t rect, display_refresh_hint_t hint);

  /** @brief Clear the framebuffer to ``color``. */
  ra8_err_t (*clear)(void* ctx, uint32_t color);

  /** @brief Tear down. Mirror of ``init``. */
  ra8_err_t (*deinit)(void* ctx);
};

/**
 * @struct display_handle
 * @brief Concrete definition of the opaque handle apps see.
 *
 * @details
 * One module-static instance lives in the dispatcher's TU; the
 * address is returned to callers by ``display_init``. The handle
 * carries the bound iface and the backend's private context
 * pointer -- the dispatcher never reaches into ``ctx`` itself.
 *
 * @invariant ``iface`` is non-NULL while the handle is initialised.
 *
 * @since 0.1.0
 */
struct display_handle {
  const display_backend_iface_t* iface; /**< Bound backend vtable.        */
  void*                          ctx;   /**< Backend-private context ptr. */
};
/* cppcheck-suppress-end [unusedStructMember] */

#ifdef __cplusplus
}
#endif
