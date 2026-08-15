/**
 * @file emu_run_guards.c
 * @brief Run-guard environment knob reader (see emu_run.h)
 *
 * @details
 * The RA8_EMU_* budget / stop-condition environment parsing that precedes
 * the chunked run loop -- moved verbatim out of the run TU so each of the
 * two stays under the file-size bar. The contract lives on the declaration
 * of run_read_guards() in emu_run.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "emu_host_io_internal.h"
#include "emu_prof.h"
#include "emu_run.h"
#include "emu_view.h"

/** @brief Default post-click drain window used when the env override is unset. */
enum : uint32_t {
  /* ra8_delay_ms(16) in the render loop spins on SysTick, which advances one
   * tick per chunk, so ~16 chunks == one loop iteration. Give the injected
   * tap many iterations to flow DOWN -> UP -> CLICKED -> tab switch -> repaint. */
  k_click_settle_chunks = 512U, /**< Extra chunks after the click lands. */
};

/**
 * @brief Read a RA8_EMU_* env var as a positive decimal uint32.
 *
 * @details Parses @p name with strtol (base ::k_env_strtol_base). A missing
 * variable or a non-positive value leaves @p dflt in force, matching the
 * "unset keeps the default" contract every budget knob documents.
 *
 * @param[in] name The environment variable name.
 * @param[in] dflt Value returned when unset or parsed as <= 0.
 * @return The parsed positive value, else @p dflt.
 * @retval dflt The variable is unset or did not parse to a positive integer.
 * @pre @p name is a valid NUL-terminated string.
 * @pre The environment is stable for the duration of the call.
 * @post No global state is modified (pure query).
 * @post The returned value is @p dflt or a strictly-positive parse result.
 * @note Not thread-safe; getenv is called on the single setup thread.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_guard_env_u32(const char* name, uint32_t dflt)
{
  const char* const e = getenv(name);
  if (e == nullptr) {
    return dflt;
  }
  const long v = strtol(e, nullptr, (int)k_env_strtol_base);
  return (v > 0L) ? (uint32_t)v : dflt;
}

/**
 * @brief Read a headless-only RA8_EMU_* positive uint32 knob.
 *
 * @details The idle / USB / MAX_CHUNKS stop knobs have no effect in --view
 * (window-driven) mode; when @p view is live the override is ignored and
 * @p dflt is returned, exactly as the inline env parsing did.
 *
 * @param[in] name The environment variable name.
 * @param[in] dflt Value returned when live-view or unset / non-positive.
 * @param[in] view The live window handle (NULL when headless).
 * @return @p dflt when a window is open, otherwise ::internal_guard_env_u32.
 * @retval dflt A live --view window suppresses the override.
 * @pre @p name is a valid NUL-terminated string.
 * @pre @p view is NULL or a valid window handle.
 * @post No global state is modified (pure query).
 * @post The override is honoured only in headless mode.
 * @note Not thread-safe; part of single-threaded setup.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_guard_env_u32_headless(const char* name, uint32_t dflt, const board_view_t* view)
{
  if (view != nullptr) {
    return dflt;
  }
  return internal_guard_env_u32(name, dflt);
}

/**
 * @brief Apply the RA8_EMU_WALL_S CPU-time guard override.
 *
 * @details A positive value sets the bound in seconds; an explicit 0 DISABLES
 * the guard (the #168 footgun fix -- WALL_S=0 no longer silently falls back to
 * the default), leaving the run bounded by chunks alone. Unset keeps both
 * defaults untouched.
 *
 * @param[in,out] wall_s        In: default bound; out: override when > 0.
 * @param[in,out] wall_guard_on In: true; out: false on an explicit WALL_S=0.
 * @return void
 * @pre @p wall_s and @p wall_guard_on are non-NULL and pre-seeded to defaults.
 * @pre The environment is stable for the call.
 * @post @p wall_s holds the effective CPU-time bound in seconds.
 * @post @p wall_guard_on is false iff RA8_EMU_WALL_S was exactly 0.
 * @note Not thread-safe; part of single-threaded setup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_guard_read_wall(double* wall_s, bool* wall_guard_on)
{
  const char* const e_wall = getenv("RA8_EMU_WALL_S");
  if (e_wall == nullptr) {
    return;
  }
  const long v = strtol(e_wall, nullptr, (int)k_env_strtol_base);
  if (v > 0L) {
    *wall_s = (double)v;
  } else if (v == 0L) {
    *wall_guard_on = false; /* explicit opt-out: bound the run by chunks only */
  }
}

/**
 * @brief Apply the --record chunk bound and create the frame directory.
 *
 * @details Headless --record-secs bounds the run to exactly the recording
 * window (so the dumped frame sequence spans the requested emulated duration);
 * then, whenever a record directory is set, the directory is created and the
 * recording banner is printed. Both steps are inert when --record is off.
 *
 * @param[in]     cfg        The run configuration (record dir / seconds).
 * @param[in]     view       The live window handle (NULL when headless).
 * @param[in,out] max_chunks In: current budget; out: record-secs bound when set.
 * @return void
 * @pre @p cfg and @p max_chunks are non-NULL.
 * @pre @p view is NULL or a valid window handle.
 * @post With headless --record-secs, @p max_chunks equals the recording window.
 * @post With --record active, the frame directory exists and the banner printed.
 * @note Not thread-safe; performs mkdir + injected error sink output during setup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_guard_setup_record(const emu_run_cfg_t* cfg,
                                                     const board_view_t*  view,
                                                     uint32_t*            max_chunks)
{
  const char* const record_dir  = cfg->record_dir;
  const uint32_t    record_secs = cfg->record_secs;
  if ((record_dir != nullptr) && (record_secs > 0U) && (view == nullptr)) {
    *max_chunks = record_secs * (uint32_t)k_record_ms_per_sec;
  }
  if (record_dir != nullptr) {
    (void)mkdir(record_dir, (mode_t)k_record_dir_mode);
    (void)priv_emu_io_errf(
      "ra8_emulator: recording to %s/frame_NNNNNN.ppm (every %u chunks, ~%u fps)\n",
      record_dir,
      (unsigned)k_record_every,
      (unsigned)k_record_fps);
  }
}

/**
 * @brief Resolve the RA8_EMU_STOP_ON console-banner stop substring.
 *
 * @details Stops the headless run as soon as the console UART's last line
 * contains the substring -- a generic "stop on a banner" guard for apps that
 * loop forever after a success line. Empty, unset, or --view all disable it.
 *
 * @param[in] view The live window handle (NULL when headless).
 * @return The stop substring, or NULL when disabled.
 * @retval NULL RA8_EMU_STOP_ON is unset, empty, or a window is open.
 * @pre @p view is NULL or a valid window handle.
 * @pre The environment is stable for the call.
 * @post No global state is modified (pure query).
 * @post A non-NULL result points at a non-empty env string.
 * @note Not thread-safe; part of single-threaded setup.
 * @since 0.1.0
 */
RA8_INTERNAL static const char* internal_guard_read_stop_on(const board_view_t* view)
{
  const char* stop_on = getenv("RA8_EMU_STOP_ON");
  if ((stop_on != nullptr) && ((stop_on[0] == '\0') || (view != nullptr))) {
    stop_on = nullptr;
  }
  return stop_on;
}

/**
 * @brief Hand RA8_EMU_STOP_PC (if set) to the profiler stop hook.
 *
 * @details Ends the run the first time PC reaches this address (effective in
 * profile insn mode, via prof_insn_hook), letting the profiler cover exactly
 * the boot path. The supplied address has its Thumb bit cleared.
 *
 * @return void
 * @pre The environment is stable for the call.
 * @pre The profiler module is initialised.
 * @post With RA8_EMU_STOP_PC set, the profiler stop PC is armed.
 * @post Without it set, no profiler state changes.
 * @note Not thread-safe; part of single-threaded setup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_guard_apply_stop_pc(void)
{
  const char* const e_spc = getenv("RA8_EMU_STOP_PC");
  if (e_spc != nullptr) {
    const unsigned long v = strtoul(e_spc, nullptr, (int)k_env_strtol_base);
    emu_prof_set_stop_pc((uint32_t)(v & ~1UL)); /* clear the Thumb bit if supplied. */
  }
}

/**
 * @brief Read the profiler compute-idle early-stop tunables.
 *
 * @details Once boot has fallen into the steady frame loop the firmware
 * retires almost no instructions per chunk, whereas boot chunks are
 * compute-heavy. Stop after @p need consecutive chunks that each retire fewer
 * than @p insns instructions, armed only after @p arm chunks. All three are
 * RA8_EMU_PROFILE_IDLE_* overridable (strtoul, base ::k_strtol_base10) so
 * the boot window can be tuned per app.
 *
 * @param[out] insns Per-chunk insn count below which a chunk is idle.
 * @param[out] need  Consecutive idle chunks that end the run.
 * @param[out] arm   Chunks to run before the stop is armed.
 * @return void
 * @pre @p insns, @p need and @p arm are non-NULL.
 * @pre The environment is stable for the call.
 * @post Each output holds its default or its env override.
 * @post No other global state is modified.
 * @note Not thread-safe; part of single-threaded setup.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_guard_read_prof_idle(uint32_t* insns, uint32_t* need, uint32_t* arm)
{
  enum : uint32_t {
    k_prof_idle_insns = 4000U, /**< Per-chunk insns below which a chunk is idle. */
    k_prof_idle_need  = 600U,  /**< Consecutive idle chunks that end the run.    */
    k_prof_idle_arm   = 16U,   /**< Chunks to run before the stop is armed.      */
  };
  *insns                 = (uint32_t)k_prof_idle_insns;
  *need                  = (uint32_t)k_prof_idle_need;
  *arm                   = (uint32_t)k_prof_idle_arm;
  const char* const e_pi = getenv("RA8_EMU_PROFILE_IDLE_INSNS");
  const char* const e_pn = getenv("RA8_EMU_PROFILE_IDLE_NEED");
  const char* const e_pa = getenv("RA8_EMU_PROFILE_IDLE_ARM");
  if (e_pi != nullptr) {
    *insns = (uint32_t)strtoul(e_pi, nullptr, (int)k_strtol_base10);
  }
  if (e_pn != nullptr) {
    *need = (uint32_t)strtoul(e_pn, nullptr, (int)k_strtol_base10);
  }
  if (e_pa != nullptr) {
    *arm = (uint32_t)strtoul(e_pa, nullptr, (int)k_strtol_base10);
  }
}

run_guards_t run_read_guards(const emu_run_cfg_t* cfg, const board_view_t* view)
{
  /* RA8_EMU_CLICK_SETTLE=N: widen the post-click drain for a tap that kicks off
   * a long operation (e.g. opening a big book from SD). Unset keeps the default. */
  const uint32_t click_settle_chunks =
    internal_guard_env_u32("RA8_EMU_CLICK_SETTLE", (uint32_t)k_click_settle_chunks);

  /* Chunked run: one SysTick (one ThreadX tick) per outer chunk. Headless runs
   * stop on a chunk budget + wall-clock guard; --view runs until the window is
   * closed. RA8_EMU_WALL_S / MAX_CHUNKS override the guards without a recompile
   * and have no effect in --view mode. */
  uint32_t max_chunks =
    (view != nullptr) ? (uint32_t)k_view_max_chunks : (uint32_t)k_run_max_chunks;
  double wall_s        = (double)k_run_wall_s;
  bool   wall_guard_on = true;
  internal_guard_read_wall(&wall_s, &wall_guard_on);
  max_chunks = internal_guard_env_u32_headless("RA8_EMU_MAX_CHUNKS", max_chunks, view);
  internal_guard_setup_record(cfg, view, &max_chunks);

  /* Headless-only early-stop knobs: idle steady-state (IDLE_STOP), USB device
   * CONFIGURED (USB_STOP) and USB host complete (USBH_STOP). Off (0) by default. */
  const uint32_t idle_stop_chunks = internal_guard_env_u32_headless("RA8_EMU_IDLE_STOP", 0U, view);
  const uint32_t usb_stop_settle  = internal_guard_env_u32_headless("RA8_EMU_USB_STOP", 0U, view);
  const uint32_t usbh_stop_settle = internal_guard_env_u32_headless("RA8_EMU_USBH_STOP", 0U, view);
  const char* const stop_on       = internal_guard_read_stop_on(view);
  internal_guard_apply_stop_pc();
  uint32_t prof_idle_insns = 0U;
  uint32_t prof_idle_need  = 0U;
  uint32_t prof_idle_arm   = 0U;
  internal_guard_read_prof_idle(&prof_idle_insns, &prof_idle_need, &prof_idle_arm);

  return (run_guards_t){
    .click_settle_chunks = click_settle_chunks,
    .max_chunks          = max_chunks,
    .wall_s              = wall_s,
    .wall_guard_on       = wall_guard_on,
    .idle_stop_chunks    = idle_stop_chunks,
    .usb_stop_settle     = usb_stop_settle,
    .usbh_stop_settle    = usbh_stop_settle,
    .stop_on             = stop_on,
    .prof_idle_insns     = prof_idle_insns,
    .prof_idle_need      = prof_idle_need,
    .prof_idle_arm       = prof_idle_arm,
  };
}
