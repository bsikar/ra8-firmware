/**
 * @file er_panel.h
 * @brief E-reader Non-Secure panel bring-up and refresh mode selection.
 * @ingroup grp_ereader
 *
 * @details
 * Owns display PAL bring-up, ``ra8_gfx`` surface binding via
 * ``display_bind_gfx()``, initial/mode frame rendering, and selectable
 * full vs. fast refresh mode policy transitions for the standalone e-reader.
 * Pure display-PAL logic lives here cleanly separated from hardware adapters so
 * both the EK-RA8D2 Non-Secure image and host unit tests exercise the exact
 * same rendering and policy state machine.
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
#include "ra8_display_pal_lcd.h"
#include "ra8_display_pal_policy.h"
#include "ra8_err.h"

/**
 * @enum er_refresh_mode_t
 * @brief User-selectable panel refresh mode mapped to display PAL policies.
 */
typedef enum : uint8_t {
  k_er_refresh_fast       = 0U, /**< Fast / low-latency waveform on every turn.  */
  k_er_refresh_full       = 1U, /**< Full high-quality clean refresh every turn. */
  k_er_refresh_fast_clean = 2U, /**< Fast turns with periodic full clean cycle.  */
  k_er_refresh_mode_count = 3U, /**< Number of defined refresh modes.            */
} er_refresh_mode_t;

/**
 * @struct er_panel_state_t
 * @brief Active display handle, bound framebuffer metadata, and refresh policy.
 */
typedef struct {
  display_handle_t*      display;     /**< Active display PAL handle.             */
  display_fb_t           fb;          /**< Framebuffer snapshot handed to gfx.    */
  display_policy_t       policy;      /**< Active refresh cadence policy state.   */
  er_refresh_mode_t      mode;        /**< Currently selected refresh mode.       */
  display_refresh_hint_t last_hint;   /**< Refresh hint emitted on last flush.    */
  display_rect_t         last_rect;   /**< Rectangle passed to last flush.        */
  uint32_t               frame_count; /**< Total rendered and flushed frames.     */
  uint16_t               clean_every; /**< Periodic clean cadence in turns.       */
  bool                   initialized; /**< True once PAL + gfx binding succeeded. */
} er_panel_state_t;

/**
 * @brief Bring up a display PAL backend, bind ``ra8_gfx``, and arm refresh policy.
 *
 * @param[out] state        Caller-owned panel state record.
 * @param[in]  cfg          Display PAL configuration descriptor.
 * @param[in]  initial_mode Initial selectable refresh mode.
 * @param[in]  clean_every  Clean cadence for ``k_er_refresh_fast_clean`` (0 uses default).
 *
 * @return ``k_ra8_ok`` on success, or an error propagated from ``display_init``,
 *         ``display_get_framebuffer``, ``display_bind_gfx``, or ``display_policy_init``.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_panel_init(er_panel_state_t*    state,
                                      const display_cfg_t* cfg,
                                      er_refresh_mode_t    initial_mode,
                                      uint16_t             clean_every);

/**
 * @brief Bring up the EK-RA8D2 GLCDC panel via ``display_pal_bind_glcdc()``.
 *
 * @param[out] state        Caller-owned panel state record.
 * @param[in]  fb_cfg       Application framebuffer descriptor.
 * @param[in]  timing       EK-RA8D2 GLCDC RGB timing descriptor.
 * @param[in]  initial_mode Initial selectable refresh mode.
 * @param[in]  clean_every  Clean cadence for ``k_er_refresh_fast_clean`` (0 uses default).
 *
 * @return ``k_ra8_ok`` on success or an error code from the bind/init chain.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_panel_init_glcdc(er_panel_state_t*         state,
                                            const display_fb_cfg_t*   fb_cfg,
                                            const ra8_glcdc_timing_t* timing,
                                            er_refresh_mode_t         initial_mode,
                                            uint16_t                  clean_every);

/**
 * @brief Tear down the bound ``ra8_gfx`` surface and display PAL handle.
 *
 * @param[in,out] state Panel state previously initialised by ``er_panel_init*``.
 *
 * @return ``k_ra8_ok`` on success, or an argument/state error.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_panel_deinit(er_panel_state_t* state);

/**
 * @brief Switch the active panel refresh mode (full vs. fast vs. fast+clean).
 *
 * @param[in,out] state       Initialised panel state.
 * @param[in]     mode        Target refresh mode.
 * @param[in]     clean_every Clean cadence for ``k_er_refresh_fast_clean`` (0 keeps existing/default).
 *
 * @return ``k_ra8_ok`` on success, or an error code without altering the active mode.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
er_panel_set_refresh_mode(er_panel_state_t* state, er_refresh_mode_t mode, uint16_t clean_every);

/**
 * @brief Return the next mode in the user cycle: fast -> full -> fast+clean -> fast.
 *
 * @param[in] current Current mode value.
 *
 * @return Next valid ``er_refresh_mode_t`` (wraps invalid inputs to ``k_er_refresh_full``).
 * @since 0.1.0
 */
er_refresh_mode_t er_panel_next_refresh_mode(er_refresh_mode_t current);

/**
 * @brief Advance to the next selectable refresh mode on an active panel.
 *
 * @param[in,out] state Initialised panel state.
 *
 * @return ``k_ra8_ok`` on success, or an error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_panel_cycle_refresh_mode(er_panel_state_t* state);

/**
 * @brief Render a status/page frame through ``ra8_gfx`` and flush via policy.
 *
 * @param[in,out] state Initialised panel state.
 * @param[in]     event Navigation/refresh event driving ``display_policy_decide()``.
 * @param[in]     title Short header banner string (non-NULL).
 * @param[in]     body  Short body status line (non-NULL).
 *
 * @return ``k_ra8_ok`` on success, or an error from policy/gfx/PAL flush.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_panel_render_frame(er_panel_state_t*    state,
                                              display_turn_event_t event,
                                              const char*          title,
                                              const char*          body);

/**
 * @brief Return a concise ASCII label for the given refresh mode.
 *
 * @param[in] mode Refresh mode enumerator.
 *
 * @return Static NUL-terminated label string.
 * @since 0.1.0
 */
const char* er_panel_refresh_mode_label(er_refresh_mode_t mode);

#ifdef __cplusplus
}
#endif
