/**
 * @file emu_run_report.c
 * @brief Run-end report + finalize (see emu_run_internal.h)
 *
 * @details
 * The run-end report (stop banner, telemetry, peripheral / MMIO summaries,
 * verdict, --dump-sym probes) plus the finalize steps (--ppm / --record
 * outputs, holding the live window, saving the SD image, releasing the buffers
 * + engine and mapping the #67 exit code). Split out of emu_run.c so each TU
 * stays under the file-size bar. Output text and order are exactly the
 * pre-split report's. The run_report / run_write_outputs / run_hold_view /
 * run_cleanup contracts live on their declarations in emu_run_internal.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "board_net.h"
#include "board_periph.h"
#include "board_periph_sd.h"
#include "board_view.h"
#include "emu_console.h"
#include "emu_engine.h"
#include "emu_exc.h"
#include "emu_mmio.h"
#include "emu_prof.h"
#include "emu_run_internal.h"
#include "emu_seams.h"
#include "emu_view.h"

/**
 * @struct run_stop_t
 * @brief How the run ended: final engine status plus every stop flag.
 *
 * @details Snapshot of the run loop's stop bookkeeping, bundled so the
 * report printers receive one read-only view of the outcome instead of a
 * dozen loose parameters.
 *
 * @invariant At most one of the early-stop flags reflects the stop cause;
 *            the report prints every one that is set.
 * @see run_print_stop_summary()  First consumer.
 * @see run_print_verdict()  Second consumer.
 * @since 0.1.0
 */
typedef struct {
  uc_err   err;              /**< Final uc_emu_start status.                 */
  uint32_t run_pc;           /**< PC where the run stopped.                  */
  uint32_t chunks;           /**< Outer chunks executed.                     */
  uint32_t max_chunks;       /**< The chunk budget in force.                 */
  double   wall_s;           /**< The wall-clock bound in force (seconds).   */
  uint32_t idle_stop_chunks; /**< RA8_EMU_IDLE_STOP window (0 = off).        */
  bool     timed_out;        /**< Wall-clock guard fired (truncated run).    */
  bool     idle_stopped;     /**< Idle steady-state stop fired.              */
  bool     usb_stopped;      /**< USB early stop (CONFIGURED / STOP_ON hit). */
  bool     stop_sym_hit;     /**< --stop-sym threshold reached.              */
  bool     prof_stopped;     /**< Profiler compute-idle stop fired.          */
} run_stop_t;

/**
 * @brief Print the attached microSD summary line of the run report.
 *
 * @details Surfaces the SD image's size / FAT format / label the same way
 * the --view sidebar does, so a headless run records the storage setup.
 * Prints nothing when no image is attached.
 *
 * @pre The SD model is in its post-run state.
 * @pre stderr is the report stream.
 * @post Zero or one summary line has been written to stderr.
 * @post The SD model is unchanged (read-only query).
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_print_sd_summary(void)
{
  bool        sd_att = false;
  uint64_t    sd_b   = 0U;
  uint8_t     sd_f   = 0U;
  const char* sd_l   = nullptr;
  board_sd_info(&sd_att, &sd_b, &sd_f, &sd_l);
  const uint64_t gib_bytes = (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
  const uint64_t mib_bytes = (uint64_t)k_size_kib * (uint64_t)k_size_kib;
  const bool     sd_gb     = (sd_b >= gib_bytes);
  const unsigned long sd_sz =
    sd_gb ? (unsigned long)(sd_b / gib_bytes) : (unsigned long)(sd_b / mib_bytes);
  const char* sd_u = sd_gb ? "GB" : "MB";
  if (sd_att && (sd_f != 0U)) {
    (void)fprintf(stderr,
                  "  SD card       : %lu %s FAT%u '%s' (created by --sd-new)\n",
                  sd_sz,
                  sd_u,
                  (unsigned)sd_f,
                  (sd_l != nullptr) ? sd_l : "");
  } else if (sd_att) {
    (void)fprintf(stderr, "  SD card       : %lu %s image attached\n", sd_sz, sd_u);
  }
}

/**
 * @brief Print the stop banner, telemetry counters and peripheral summaries.
 *
 * @details The first half of the run report: the "stopped" cause line, final
 * PC / BKPT note, chunk + tick + seam counters, the profiler report, the
 * exception / touch totals, the flushed console tail, the peripheral +
 * network + MMIO observability reports, the SD summary and the MMIO table.
 * Output text and order are exactly the pre-split report's.
 *
 * @param[in,out] uc The engine (read for the peripheral report).
 * @param[in]     st How the run ended.
 * @pre The run loop has ended and @p st reflects its bookkeeping.
 * @pre stderr is the report stream.
 * @post The summary section has been written to stderr.
 * @post The engine state is unchanged (read-only queries).
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_print_stop_summary(uc_engine* uc, const run_stop_t* st)
{
  (void)fprintf(stderr,
                "\nra8_emulator: stopped -- %s%s%s%s%s%s\n",
                uc_strerror(st->err),
                st->timed_out ? " (wall-clock budget reached)" : "",
                st->idle_stopped ? " (idle steady-state)" : "",
                st->usb_stopped ? " (USB enumerated)" : "",
                st->stop_sym_hit ? " (--stop-sym threshold reached)" : "",
                st->prof_stopped ? " (profile: boot complete)" : "");
  (void)fprintf(stderr, "  final PC      : 0x%08X\n", st->run_pc);
  if (emu_exc_bkpt_hit()) {
    (void)fprintf(stderr,
                  "  => firmware executed a BKPT @ 0x%08X (deliberate trap: "
                  "Default_Handler / failed assert / fault give-up)\n",
                  emu_exc_bkpt_pc());
  }
  (void)fprintf(stderr,
                "  chunks run    : %u   SysTick ticks: %u\n",
                st->chunks,
                emu_exc_systick_fires());
  if (emu_mve_emulated_count() > 0U) {
    (void)fprintf(stderr,
                  "  MVE (Helium)  : %llu instruction(s) emulated (M85 vector ops the M33 "
                  "core lacks)\n",
                  (unsigned long long)emu_mve_emulated_count());
  }
  if (emu_lob_emulated_count() > 0U) {
    (void)fprintf(stderr,
                  "  LOB (loop)    : %llu DLS/LE instruction(s) emulated (M85 hardware-loop "
                  "ops)\n",
                  (unsigned long long)emu_lob_emulated_count());
  }
  prof_report();
  (void)fprintf(stderr,
                "  exceptions    : %u PendSV  %u SVCall (real Cortex-M entry/return)\n",
                emu_exc_pendsv_takes(),
                emu_exc_svc_takes());
  (void)fprintf(stderr,
                "  touch clicks  : %u drained via ra8_touch -> I3C -> GT911\n",
                board_periph_touch_reported());
  /* Emit any console bytes still buffered without a trailing newline. */
  console_flush_line(board_periph_sci_console_channel());
  /* Peripheral-model observability: LED transitions, timer totals, IRQ counts,
   * SCI byte totals. */
  board_periph_report(uc);
  board_net_report();
  emu_mmio_print_counts();
  run_print_sd_summary();
  emu_mmio_print_bgc_and_table();
}

/**
 * @brief Print the run verdict: TRUNCATED warning or EXECUTED-to-budget.
 *
 * @details Distinguishes a wall-clock truncation (host overload, #168) from
 * a genuine full-budget or early-stop completion, with the idle / USB stop
 * variants; a BKPT run prints neither (the banner already carried it).
 *
 * @param[in] st How the run ended.
 * @pre @p st is the run's final stop snapshot.
 * @pre stderr is the report stream.
 * @post Zero or one verdict paragraph has been written to stderr.
 * @post No process state changes (pure reporting).
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_print_verdict(const run_stop_t* st)
{
  if (st->timed_out && !st->idle_stopped && !st->usb_stopped && !st->stop_sym_hit &&
      !st->prof_stopped && !emu_exc_bkpt_hit()) {
    /* The wall-clock guard fired (clock() is CPU-time, so a heavily-loaded host
     * burns the budget faster). This is a TRUNCATED run, NOT a completed one --
     * say so plainly, and do NOT print the "EXECUTED to the run budget" line a
     * full-budget run prints. Conflating the two let a load-correlated truncation
     * masquerade as success, dropping a deterministic banner without failing the
     * gate (#168). A caller that wants the run bounded by a deterministic event
     * (not by CPU-time) should pass RA8_EMU_STOP_ON / RA8_EMU_MAX_CHUNKS. */
    (void)fprintf(stderr,
                  "  => ra8_emulator TRUNCATED by the wall-clock guard at chunk %u of %u "
                  "(%.0fs CPU-time elapsed); this is NOT a full-budget run (host "
                  "overloaded -- see #168).\n",
                  st->chunks,
                  st->max_chunks,
                  st->wall_s);
  } else if (((st->err == UC_ERR_OK) || st->idle_stopped || st->usb_stopped || st->stop_sym_hit ||
              st->prof_stopped) &&
             !emu_exc_bkpt_hit()) {
    if (st->idle_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (idle steady-state: no "
                    "observable change for %u chunks, stopped at chunk %u).\n",
                    st->idle_stop_chunks,
                    st->chunks);
    } else if (st->usb_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (USB enumerated: device "
                    "CONFIGURED, stopped at chunk %u).\n",
                    st->chunks);
    } else {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (no invalid opcode / fault).\n");
    }
  }
}

/**
 * @brief Print each --dump-sym probe's value from emulated memory.
 *
 * @details Reads every resolved 32-bit global out of Unicorn memory and
 * prints name, address and value so a test can probe firmware state after
 * the run without a debugger; unresolved or unreadable symbols say so.
 *
 * @param[in,out] uc  The engine whose memory is probed.
 * @param[in]     cfg The run configuration carrying the probe list.
 * @pre The run has ended (values are the firmware's final state).
 * @pre @p cfg->dump_sym_addrs / names hold @p cfg->dump_sym_n entries.
 * @post One line per probe has been written to stderr.
 * @post The engine memory is unchanged (read-only probes).
 * @note Not thread-safe; part of the single-threaded report.
 * @since 0.1.0
 */
static void run_print_dump_syms(uc_engine* uc, const emu_run_cfg_t* cfg)
{
  /* --dump-sym: read each resolved global from Unicorn memory and print its
   * 32-bit value (and the address), so a test can probe firmware state (e.g. an
   * init-step or mismatch counter) after the run without a debugger. */
  for (uint32_t d = 0U; d < cfg->dump_sym_n; d++) {
    if (cfg->dump_sym_addrs[d] == 0U) {
      (void)fprintf(stderr, "  dump-sym      : %s = <unresolved>\n", cfg->dump_sym_names[d]);
      continue;
    }
    uint32_t v = 0U;
    if (uc_mem_read(uc, (uint64_t)cfg->dump_sym_addrs[d], &v, sizeof(v)) == UC_ERR_OK) {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = %u (0x%08X)\n",
                    cfg->dump_sym_names[d],
                    cfg->dump_sym_addrs[d],
                    v,
                    v);
    } else {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = <unreadable>\n",
                    cfg->dump_sym_names[d],
                    cfg->dump_sym_addrs[d]);
    }
  }
}

void run_report(const run_loop_t* st)
{
  emu_view_mark_stopped(st->run_pc); /* final / held frame shows "parked". */
  const run_stop_t stop = {
    .err              = st->err,
    .run_pc           = st->run_pc,
    .chunks           = st->chunks,
    .max_chunks       = st->guards.max_chunks,
    .wall_s           = st->guards.wall_s,
    .idle_stop_chunks = st->guards.idle_stop_chunks,
    .timed_out        = st->timed_out,
    .idle_stopped     = st->idle_stopped,
    .usb_stopped      = st->usb_stopped,
    .stop_sym_hit     = st->stop_sym_hit,
    .prof_stopped     = st->prof_stopped,
  };
  run_print_stop_summary(st->cfg->uc, &stop);
  run_print_verdict(&stop);
  run_print_dump_syms(st->cfg->uc, st->cfg);
}

void run_write_outputs(const run_loop_t* st)
{
  const emu_run_cfg_t* cfg = st->cfg;
  if ((cfg->ppm_path != nullptr) && (st->composite != nullptr)) {
    build_composite(cfg->uc,
                    st->panel_fb,
                    st->rot_fb,
                    st->composite,
                    st->panel_w,
                    st->panel_h,
                    st->disp_w,
                    st->disp_h,
                    cfg->rotate_deg,
                    cfg->win_title);
    if (write_ppm(cfg->ppm_path, st->composite, st->comp_w, st->comp_h) == 0) {
      (void)fprintf(stderr,
                    "  wrote %s (%ux%u)\n",
                    cfg->ppm_path,
                    (unsigned)st->comp_w,
                    (unsigned)st->comp_h);
    } else {
      (void)fprintf(stderr, "  could not write %s\n", cfg->ppm_path);
    }
  }
  if (cfg->record_dir != nullptr) {
    (void)fprintf(stderr,
                  "  recorded %u frame(s) to %s (%ux%u, ~%u fps)\n",
                  st->rec_frames,
                  cfg->record_dir,
                  (unsigned)st->comp_w,
                  (unsigned)st->comp_h,
                  (unsigned)k_record_fps);
  }
}

void run_hold_view(const run_loop_t* st)
{
  const emu_run_cfg_t* cfg = st->cfg;
  if (st->view == nullptr) {
    return;
  }
  if (!st->closed) { /* run ended on its own -- keep the last frame up until closed */
    build_composite(cfg->uc,
                    st->panel_fb,
                    st->rot_fb,
                    st->composite,
                    st->panel_w,
                    st->panel_h,
                    st->disp_w,
                    st->disp_h,
                    cfg->rotate_deg,
                    cfg->win_title);
    board_view_present(st->view, st->composite, st->comp_w, st->comp_h);
    (void)fprintf(stderr, "ra8_emulator: run ended; close the window to exit\n");
    while (!board_view_pump(st->view)) {
      (void)usleep((useconds_t)k_view_idle_us);
    }
  }
  board_view_close(st->view);
}

int run_cleanup(const run_loop_t* st)
{
  const emu_run_cfg_t* cfg = st->cfg;
  if (cfg->save_sd_path != nullptr) {
    (void)board_sd_save(cfg->save_sd_path);
  }
  free(st->panel_fb);
  free(st->rot_fb);
  free(st->composite);
  free(cfg->elf); /* kept alive for the whole run so a warm reboot can re-load it */
  (void)uc_close(cfg->uc);

  emu_exit_t exit_code = k_emu_exit_ok;
  if (emu_exc_bkpt_hit()) {
    exit_code = k_emu_exit_bkpt;
  } else if (st->err != UC_ERR_OK) {
    exit_code = k_emu_exit_fault;
  } else if (st->timed_out) {
    exit_code = k_emu_exit_timeout;
  }
  return (int)exit_code;
}
