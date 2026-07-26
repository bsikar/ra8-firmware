/**
 * @file sim_view.h
 * @brief Board-view presentation: frames, composition, input routing, panels
 *
 * @details
 * Everything the emulator presents or routes for a human: the GLCDC frame
 * build from live register state, the RGB565 -> PPM writer, the status
 * sidebar snapshot, panel rotation and click unrotation, the on-screen
 * switch / battery / console-tab click routing shared by the live window and
 * the headless --click, the panel descriptor loader, the console-scrollback
 * view state, and the core-control state (primary core label + the M33
 * low-power clock model) the GUI toggles live.
 *
 * Split out of the board_sim main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "board_overlay.h"
#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief EK-RA8D2 user-switch GPIO coordinates (active-low): SW1 P009, SW2 P008. */
typedef enum : uint8_t {
  k_sim_sw_port = 0U, /**< Both user switches sit on PORT0. */
  k_sim_sw1_pin = 9U, /**< SW1 -> P009.                     */
  k_sim_sw2_pin = 8U, /**< SW2 -> P008.                     */
} sim_sw_pin_t;

/* Live-view (--view) and snapshot (--ppm) presentation settings. */
typedef enum : uint32_t {
  k_view_default_w     = 1024U,    /**< Default window width (EK-RA8D2 panel).      */
  k_view_default_h     = 600U,     /**< Default window height (EK-RA8D2 panel).     */
  k_view_present_every = 16U,      /**< Present the frame every Nth chunk.          */
  k_view_max_chunks    = 4000000U, /**< Cap in --view; closing the window ends.     */
  k_view_idle_us       = 16000U,   /**< ~60 Hz idle pump after the run ends.        */
  k_view_frame_us      = 16000U,   /**< Min wall-us between live presents (~60 Hz). */
  k_view_yield_us      = 2000U,    /**< Yield this long when a present is skipped.  */
  k_us_per_s           = 1000000U, /**< Microseconds per second.                    */
  k_ns_per_us          = 1000U,    /**< Nanoseconds per microsecond.                */
  k_uart_log_max       = 64U,      /**< Console ring depth (mirrors SCI model).     */
  k_reboot_settle      = 1500U,    /**< Chunks to run before a scheduled --reboot.  */
  /* --record settings. One outer chunk advances SysTick once (~1 ms of emulated
   * time at the firmware's 1 kHz tick), so ~1000 chunks == one emulated second.
   * Recording dumps a frame every k_record_every chunks for k_record_fps fps. */
  k_record_ms_per_sec = 1000U, /**< Emulated ms (= chunks) per second.        */
  k_record_fps        = 20U,   /**< Recorded frames per emulated second.      */
  k_record_every      = 50U,   /**< Chunks between recorded frames (1000/20). */
} view_cfg_t;

/** @brief Which core the firmware targets: gates the M85-only instruction seams. */
typedef enum : uint8_t {
  k_core_m85 = 0U, /**< Cortex-M85 primary (default): MVE/long-shift seams armed.    */
  k_core_m33 = 1U, /**< Cortex-M33 primary: the M85-only instruction seams stay off. */
} board_primary_core_t;

/**
 * @enum panel_rotate_t
 * @brief Display rotation (degrees clockwise) applied to the panel for viewing.
 *
 * @details The firmware always renders at its compiled resolution; --rotate only
 * turns the emulated panel for display (e.g. a 1024x600 landscape app shown as a
 * 600x1024 portrait), so a vertically-mounted screen can be previewed. Clicks
 * are mapped back to native coordinates via ::unrotate_click.
 */
typedef enum : uint32_t {
  k_rotate_0   = 0U,   /**< Native orientation.             */
  k_rotate_90  = 90U,  /**< 90 deg clockwise (-> portrait). */
  k_rotate_180 = 180U, /**< Upside down.                    */
  k_rotate_270 = 270U, /**< 90 deg counter-clockwise.       */
} panel_rotate_t;

typedef enum : uint32_t {
  k_strtol_base10  = 10U,   /**< Base-10 radix for strtol parses. */
  k_panel_line_max = 256U,  /**< Max panel-config line length.    */
  k_panel_name_max = 64U,   /**< Max panel name (incl NUL).       */
  k_panel_dim_max  = 4096U, /**< Sanity cap on a panel dimension. */
} panel_limits_t;

/** @brief Display descriptor loaded from a flat key=value panel file. */
typedef struct {
  char     name[k_panel_name_max]; /**< Name.         */
  uint16_t width;                  /**< Pixel width.  */
  uint16_t height;                 /**< Pixel height. */
} board_panel_t;

/**
 * @brief Write an RGB565 frame to a binary PPM (P6) for headless inspection.
 *
 * @param[in] path      Output file path.
 * @param[in] fb        RGB565 frame of @p width_px x @p height_px pixels.
 * @param[in] width_px  Frame width in pixels.
 * @param[in] height_px Frame height in pixels.
 * @return 0 on success, -1 when the file cannot be created.
 * @retval 0  The frame was written.
 * @retval -1 fopen failed (path unwritable).
 * @pre @p fb holds the full frame.
 * @pre @p path names a writable location.
 * @post On success the P6 file exists with 8-bit RGB expansion.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV int write_ppm(const char* path, const uint16_t* fb, uint16_t width_px, uint16_t height_px);

/**
 * @brief Map a click in the rotated displayed panel back to native panel coords.
 *
 * @param[in]  cx      Click column in displayed-panel pixels.
 * @param[in]  cy      Click row in displayed-panel pixels.
 * @param[in]  panel_w Native panel width.
 * @param[in]  panel_h Native panel height.
 * @param[in]  deg     Active display rotation (0/90/180/270).
 * @param[out] nx      Receives the native column.
 * @param[out] ny      Receives the native row.
 * @return Nothing.
 * @pre @p nx and @p ny are non-null.
 * @pre @p deg is one of the panel_rotate_t values.
 * @post The native coordinates are written.
 * @note Pure mapping; thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void unrotate_click(uint16_t  cx,
                             uint16_t  cy,
                             uint16_t  panel_w,
                             uint16_t  panel_h,
                             uint32_t  deg,
                             uint16_t* nx,
                             uint16_t* ny);

/**
 * @brief Toggle a user switch's pressed level (active-low) for an on-screen button.
 *
 * @details SW1/SW2 are momentary push-buttons wired active-low: held down
 * drives the pin LOW, released returns it HIGH. Driving the level directly
 * makes a click behave as a real button (press on mouse-down, release on
 * mouse-up), not a latching switch.
 *
 * @param[in] btn     The overlay button hit (SW1 or SW2).
 * @param[in] pressed true while held down, false on release.
 * @return Nothing.
 * @pre @p btn is one of the switch buttons.
 * @pre The GPIO input model is initialised.
 * @post The switch pin level reflects @p pressed.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void set_switch(board_overlay_btn_t btn, bool pressed);

/**
 * @brief Apply a POWER-section click to the fuel-gauge / core model.
 *
 * @details A click on the battery slider track maps the click column to a
 * 0..100 percent and writes it while preserving the charge state; a click on
 * the CHG toggle flips charging while preserving SOC; the CORE low-power
 * button flips the M33 4:1-clock model live. Shared by the live window and
 * the headless @c --click so both behave alike.
 *
 * @param[in] btn    The POWER button hit (slider, CHG, or low-power).
 * @param[in] cx     Click column in composite pixels (for the slider map).
 * @param[in] disp_w Displayed panel width (the sidebar origin).
 * @return Nothing.
 * @pre @p btn is one of the POWER-section buttons.
 * @pre The battery model is initialised.
 * @post The battery / low-power state reflects the click.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void apply_battery_click(board_overlay_btn_t btn, uint16_t cx, uint16_t disp_w);

/**
 * @brief Route a composite-space click to the right input model.
 *
 * @details A console-tab click switches the visible channel; a console-body
 * click toggles autoscroll; an on-screen SW1/SW2 presses that user switch
 * (active-low); a POWER click drives the battery / low-power model; any
 * other click is unrotated and injected as a GT911 touch -- the same path
 * the firmware's real ra8_touch_read drains. Shared by the live window and
 * the headless @c --click so both behave alike.
 *
 * @param[in] cx         Click column in composite pixels.
 * @param[in] cy         Click row in composite pixels.
 * @param[in] panel_w    Native panel width (for the touch unrotate).
 * @param[in] panel_h    Native panel height (for the touch unrotate).
 * @param[in] disp_w     Displayed panel width (the sidebar origin).
 * @param[in] rotate_deg Active display rotation.
 * @return The button hit, or ::k_board_overlay_btn_none for a panel touch.
 * @retval k_board_overlay_btn_none The click was injected as a touch.
 * @pre The overlay geometry matches the composite the click landed on.
 * @pre The input models are initialised.
 * @post Exactly one input model consumed the click.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV board_overlay_btn_t route_click(uint16_t cx,
                                         uint16_t cy,
                                         uint16_t panel_w,
                                         uint16_t panel_h,
                                         uint16_t disp_w,
                                         uint32_t rotate_deg);

/**
 * @brief Build the panel framebuffer, then compose it with the status sidebar.
 *
 * @details Renders the GLCDC panel into @p panel_fb (so a display app's
 * region stays pixel-correct), rotates it for display when requested,
 * snapshots the live peripheral state, and writes the full composite (panel
 * region + status sidebar) into @p composite -- the single buffer that backs
 * both the live window and the @c --ppm snapshot.
 *
 * @param[in,out] uc         Unicorn engine (read for the GLCDC framebuffer).
 * @param[out]    panel_fb   Panel RGB565 buffer (@p panel_w by @p panel_h).
 * @param[out]    rot_fb     Rotated-panel scratch (NULL when rotation is 0).
 * @param[out]    composite  Composite RGB565 buffer (overlay dimensions).
 * @param[in]     panel_w    Panel width in pixels.
 * @param[in]     panel_h    Panel height in pixels.
 * @param[in]     disp_w     Displayed panel width (after rotation).
 * @param[in]     disp_h     Displayed panel height (after rotation).
 * @param[in]     rotate_deg Active display rotation.
 * @param[in]     app_name   Window / app title for the sidebar caption.
 * @return Nothing.
 * @pre The buffers are allocated to the stated dimensions.
 * @pre The overlay geometry matches @p disp_w / @p disp_h.
 * @post @p composite holds the panel + sidebar frame.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void build_composite(uc_engine*  uc,
                              uint16_t*   panel_fb,
                              uint16_t*   rot_fb,
                              uint16_t*   composite,
                              uint16_t    panel_w,
                              uint16_t    panel_h,
                              uint16_t    disp_w,
                              uint16_t    disp_h,
                              uint32_t    rotate_deg,
                              const char* app_name);

/**
 * @brief Load a panel descriptor (name / width / height) from a TOML-ish file.
 *
 * @details A flat ``key = value`` panel descriptor (see
 * ``tools/ra8_emulator/panels/``), so the board emulator becomes whatever
 * display a config describes -- not just the EK-RA8D2 1024x600.
 * Dependency-free bounded parser; blank lines and '#' comments are ignored
 * and quotes are stripped from the name.
 *
 * @param[in]  path Panel config path.
 * @param[out] out  Filled descriptor on success.
 * @return true if a valid width/height were parsed.
 * @retval false The file was unreadable or the dimensions were out of range.
 * @pre @p out is non-null.
 * @pre @p path is NUL-terminated.
 * @post On true @p out holds the descriptor; on false a diagnostic printed.
 * @note Not thread-safe; call during single-threaded setup.
 * @since 0.1.0
 */
RA8_PRIV bool load_panel(const char* path, board_panel_t* out);

/**
 * @brief Publish the run-loop telemetry the board view shows.
 *
 * @param[in] pc     Current program counter.
 * @param[in] chunks Emulation-chunk counter.
 * @return Nothing.
 * @pre The run loop is at a chunk boundary.
 * @pre None otherwise.
 * @post The next present shows the new PC / chunk count.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_view_publish(uint32_t pc, uint32_t chunks);

/**
 * @brief Mark the run ended: the held frame shows "parked" at @p pc.
 *
 * @param[in] pc Final program counter.
 * @return Nothing.
 * @pre The run loop has exited.
 * @pre None otherwise.
 * @post The view telemetry reads not-running at @p pc.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_view_mark_stopped(uint32_t pc);

/**
 * @brief Page the console scrollback by mouse-wheel notches.
 *
 * @details Scrolling up reveals older lines AND pauses autoscroll (so new
 * output no longer yanks the view to the bottom); scrolling back down to the
 * tail re-enables autoscroll. The status snapshot clamps the offset and
 * holds the absolute position while paused.
 *
 * @param[in] notches Wheel notches (positive = up / older lines).
 * @return Nothing.
 * @pre The live window is polling wheel events.
 * @pre None otherwise.
 * @post The scrollback offset / autoscroll state reflect the wheel.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_view_wheel(int32_t notches);

/**
 * @brief Switch the visible console channel to @p tab_idx (tab-bar click).
 *
 * @details Resets the scrollback to the live tail of the newly selected
 * channel, exactly as a tab click in the window does; the headless --click
 * classification shares this path.
 *
 * @param[in] tab_idx The board_console channel index selected.
 * @return Nothing.
 * @pre @p tab_idx is a valid board_console channel.
 * @pre None otherwise.
 * @post The selected tab shows its live tail with autoscroll on.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_view_select_console_tab(uint32_t tab_idx);

/**
 * @brief Reset the console-scrollback view (warm-reboot support).
 *
 * @return Nothing.
 * @pre A warm reboot is re-initialising the console surfaces.
 * @pre None otherwise.
 * @post The view shows the ALL tab at the live tail with autoscroll on.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_view_reset_console(void);

/**
 * @brief The primary core the firmware targets (label + seam gating).
 *
 * @return The selected primary core.
 * @retval k_core_m85 Default: the M85-only instruction seams are armed.
 * @retval k_core_m33 --primary-core m33: the M85 seams stay off.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV board_primary_core_t sim_primary_core(void);

/**
 * @brief Select the primary core (CLI --primary-core).
 *
 * @param[in] core The core to model.
 * @return Nothing.
 * @pre Called during setup, before the seams install.
 * @pre None otherwise.
 * @post sim_primary_core() reports @p core.
 * @note Not thread-safe; single-threaded setup only.
 * @since 0.1.0
 */
RA8_PRIV void sim_set_primary_core(board_primary_core_t core);

/**
 * @brief Whether the M33 4:1-slower low-power clock model is active.
 *
 * @return true while the low-power model shrinks the chunk budget.
 * @retval false Full-speed (M85 clock) modelling.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @note The GUI low-power button flips this live under --view.
 * @since 0.1.0
 */
RA8_PRIV bool sim_low_power(void);

/**
 * @brief Set the low-power clock model (CLI --low-power / GUI toggle).
 *
 * @param[in] on true to shrink the chunk budget by the 4:1 clock ratio.
 * @return Nothing.
 * @pre None (safe to flip live).
 * @pre None.
 * @post sim_low_power() reports @p on.
 * @note Not thread-safe; the emulator is single-threaded host-side.
 * @since 0.1.0
 */
RA8_PRIV void sim_set_low_power(bool on);

#ifdef __cplusplus
}
#endif
