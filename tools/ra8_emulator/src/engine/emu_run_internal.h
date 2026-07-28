/**
 * @file emu_run_internal.h
 * @brief Shared run-loop state + cross-TU run helpers (ra8_emulator internal)
 *
 * @details
 * The chunked run phase is split across three translation units to keep each
 * under the file-size bar: emu_run.c (setup + the per-chunk phase helpers +
 * the driver), emu_run_inner.c (the inner exception-resolve loop) and
 * emu_run_report.c (the run-end report + finalize). This internal header
 * carries the pieces those TUs share: the ::run_loop_t mutable state bundle
 * and the RA8_PRIV declarations of the helpers called across the split. It is
 * NOT part of the ra8_emulator public surface (unlike emu_run.h).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <time.h>
#include <unicorn/unicorn.h>

#include "board_overlay.h"
#include "board_view.h"
#include "emu_run.h"
#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct run_loop_t
 * @brief The whole run's mutable state, threaded through the phase helpers.
 *
 * @details One bundle of the setup products (config, guards, presentation
 * geometry + buffers, click classification) plus every counter the chunk loop
 * mutates, so the loop body and the run-end finalizer become small phase
 * helpers over @c st-> instead of one 500-line function over three dozen
 * locals. Field semantics are unchanged from the pre-split locals.
 *
 * @invariant @c cfg and @c guards are fixed once ::run_loop_setup returns; the
 *            counter fields advance monotonically across the loop.
 * @see run_loop_setup()  The initializer.
 * @see run_loop()  The consumer.
 * @since 0.1.0
 */
typedef struct {
  const emu_run_cfg_t* cfg;              /**< Setup products (engine, image, CLI knobs). */
  run_guards_t         guards;           /**< Env-tunable budgets + stop conditions.     */
  uint16_t             panel_w;          /**< Firmware render width in pixels.           */
  uint16_t             panel_h;          /**< Firmware render height in pixels.          */
  uint16_t             disp_w;           /**< Displayed width (rotation-swapped).        */
  uint16_t             disp_h;           /**< Displayed height (rotation-swapped).       */
  uint16_t             comp_w;           /**< Composite (panel + sidebar) width.         */
  uint16_t             comp_h;           /**< Composite (panel + sidebar) height.        */
  board_view_t*        view;             /**< Live window (NULL when headless).          */
  uint16_t*            panel_fb;         /**< GLCDC render scratch (or NULL).            */
  uint16_t*            rot_fb;           /**< Rotation scratch (or NULL).                */
  uint16_t*            composite;        /**< Composite frame buffer (or NULL).          */
  board_overlay_btn_t  click_btn;        /**< Classified --click target button.          */
  bool                 click_was_tab;    /**< --click landed on a console tab.           */
  int                  reboot_count;     /**< --reboot warm reboots remaining.           */
  uint32_t             run_pc;           /**< Current resume PC.                         */
  uint32_t             chunks;           /**< Outer chunks executed so far.              */
  uint32_t             last_boot_chunk;  /**< Chunk of the last (re)boot.                */
  uint32_t             settle_left;      /**< Post-click drain countdown.                */
  uint32_t             rec_frames;       /**< --record frames written.                   */
  uint32_t             prof_idle_run;    /**< Consecutive profiler-idle chunks.          */
  uint32_t             idle_run;         /**< Consecutive steady-idle chunks.            */
  uint32_t             usb_stop_run;     /**< USB device settle countup.                 */
  uint32_t             usbh_stop_run;    /**< USB host settle countup.                   */
  uint64_t             prof_idle_prev_i; /**< Prior total insns (profiler).              */
  uint64_t             idle_sig_prev;    /**< Prior idle-signature sum.                  */
  uint64_t             last_present_us;  /**< wall-us of the last live present.          */
  uint32_t             prof_prev_pc;     /**< PC charged by the wall profiler.           */
  double               prof_prev_t;      /**< Start time of the prior chunk.             */
  clock_t              t0;               /**< CPU-time origin for the guard.             */
  uc_err               err;              /**< Latest uc_emu_start status.                */
  bool                 prof_stopped;     /**< Profiler stop fired.                       */
  bool                 idle_stopped;     /**< Idle steady-state stop fired.              */
  bool                 usb_stopped;      /**< USB / banner early stop fired.             */
  bool                 stop_sym_hit;     /**< --stop-sym threshold reached.              */
  bool                 timed_out;        /**< Wall-clock guard fired.                    */
  bool                 closed;           /**< Live window was closed.                    */
  bool                 button_fired;     /**< Headless click button fired once.          */
  bool                 slider_grab;      /**< Battery slider grabbed by a drag.          */
  board_overlay_btn_t  held_btn;         /**< SW held down (released on up).             */
} run_loop_t;

/**
 * @brief Run one outer chunk's inner exception-resolve loop.
 *
 * @details Runs a chunk, then services exceptions to a steady state before the
 * next chunk (zero-time relaunches, EXC_RETURN unstack + tail-chain, MPU / div-0
 * fault synthesis, tick / PendSV take). Returns once the full budget elapsed
 * with nothing pending, or on a fault / BKPT / stop request. Defined in
 * emu_run_inner.c; called by the run loop in emu_run.c.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base VTOR fallback for exception vectoring.
 * @param[in,out] run_pc_io Resume PC in, final PC out.
 * @param[out]    err_out   Final uc_emu_start status for the report.
 * @return true when the run must end (fault or BKPT), false to continue.
 * @retval true  The outer loop breaks and reports the stop cause.
 * @retval false The chunk resolved cleanly; the outer loop continues.
 * @pre The engine is set up and @p run_pc_io holds a valid resume PC.
 * @pre The seams / exception hooks are installed.
 * @post @p run_pc_io and @p err_out reflect the loop's final state.
 * @note Not thread-safe; this is the single-threaded run core.
 * @since 0.1.0
 */
RA8_PRIV bool run_inner(uc_engine* uc, uint32_t vtor_base, uint32_t* run_pc_io, uc_err* err_out);

/**
 * @brief Print the run-end report (stop summary, verdict, --dump-sym probes).
 *
 * @details Defined in emu_run_report.c; called by emu_run_and_report after the
 * run loop returns.
 *
 * @param[in] st The run state (final counters + flags).
 * @return void
 * @pre The run loop has ended and @p st reflects its outcome.
 * @pre stderr is the report stream.
 * @post The report section has been written to stderr.
 * @post No engine or process state changes beyond output.
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
RA8_PRIV void run_report(const run_loop_t* st);

/**
 * @brief Write the --ppm snapshot and the --record summary line.
 *
 * @details Defined in emu_run_report.c; called by emu_run_and_report.
 *
 * @param[in] st The run state (buffers + output paths).
 * @return void
 * @pre The run loop has ended and @p st reflects its outcome.
 * @pre stderr is the report stream.
 * @post The --ppm file is written and/or the --record line printed as requested.
 * @post No engine or process state changes beyond output.
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
RA8_PRIV void run_write_outputs(const run_loop_t* st);

/**
 * @brief Hold the live window on the final frame until the user closes it.
 *
 * @details Defined in emu_run_report.c; called by emu_run_and_report. Inert in
 * headless mode.
 *
 * @param[in] st The run state (window + buffers).
 * @return void
 * @pre The run loop has ended and @p st reflects its outcome.
 * @pre @p st->view is a live window or NULL.
 * @post Any live window has been presented (if needed) and closed.
 * @post No engine or process state changes beyond the window.
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
RA8_PRIV void run_hold_view(const run_loop_t* st);

/**
 * @brief Save the SD image, release buffers + engine, map the exit code.
 *
 * @details Defined in emu_run_report.c; called by emu_run_and_report as the
 * final teardown step.
 *
 * @param[in] st The run state (buffers, engine, stop flags).
 * @return The process exit status (::emu_exit_t).
 * @retval k_emu_exit_ok      Clean run-to-budget.
 * @retval k_emu_exit_bkpt    Firmware executed a BKPT.
 * @retval k_emu_exit_fault   Emulation fault ended the run.
 * @retval k_emu_exit_timeout Wall-clock budget reached.
 * @pre The run loop has ended and outputs have been written.
 * @pre @p st owns the buffers / engine / image being released.
 * @post The buffers, image and engine have been freed / closed.
 * @note Not thread-safe; part of the single-threaded teardown.
 * @since 0.1.0
 */
RA8_PRIV int run_cleanup(const run_loop_t* st);

#ifdef __cplusplus
}
#endif
