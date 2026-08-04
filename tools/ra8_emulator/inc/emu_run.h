/**
 * @file emu_run.h
 * @brief The chunked emulation run loop, report and exit-code mapping
 *
 * @details
 * Owns everything after setup: the presentation buffers, the run-guard
 * environment knobs (RA8_EMU_WALL_S / MAX_CHUNKS / IDLE_STOP / USB_STOP /
 * USBH_STOP / STOP_ON / STOP_PC / CLICK_SETTLE / the profiler idle-stop
 * tunables), the chunked run loop with its inner exception-resolve loop and
 * seven stop conditions, the run-end report, and the process exit-code
 * mapping the #67 run-every-example matrix keys on. The shared run budgets
 * and assorted sizing constants live here so both the setup banner and the
 * loop read one definition.
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#include "board_view.h"
#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum : uint32_t {
  k_run_chunk_insns = 500000U, /**< Instructions per emulation chunk.     */
  k_low_power_div   = 4U,      /**< Low-power: shrink the chunk budget by */
                               /**< this (the 4:1 M85:M33 clock ratio) so */
                               /**< the modelled core advances ~1/4 as fast. */
  k_run_max_chunks = 40000U,   /**< Chunk budget. Each chunk offers one */
                               /**< SysTick, so RTOS apps whose threads */
                               /**< sleep on hundreds/thousands of ticks */
                               /**< (e.g. ThreadX tx_thread_sleep) need a */
                               /**< far larger budget than bare-metal. */
  k_idle_spin_insns = 2U,      /**< Budget when the core is parked on a
                                * wait-for-interrupt spin (a "b ." self-branch,
                                * `wfi`, or a `cpsie i` + back-branch poll):
                                * collapse the idle wait to the next SysTick
                                * instead of spinning a whole chunk. */
  k_op_branch_self  = 0xE7FEU, /**< Thumb "b ." (branch-to-self idle loop).     */
  k_op_wfi          = 0xBF30U, /**< Thumb `wfi` (wait-for-interrupt).           */
  k_op_cpsie_i      = 0xB662U, /**< Thumb `cpsie i` (re-enable IRQ in a poll).  */
  k_op_bn_mask      = 0xF800U, /**< Mask selecting a Thumb T2 `b.n` opcode.     */
  k_op_bn_base      = 0xE000U, /**< Thumb T2 unconditional `b.n` base value.    */
  k_op_bn_imm       = 0x07FFU, /**< Thumb T2 `b.n` imm11 field mask.            */
  k_bn_imm_sext_shl = 21U,     /**< Shift imm11 bit10 up to bit31 (sign bit).   */
  k_bn_imm_sext_shr = 20U,     /**< Arith >> sign-extends and scales imm by 2.  */
  k_idle_scan_fwd   = 8U,      /**< Halfwords scanned ahead for a loop edge.    */
  k_idle_loop_max   = 32U,     /**< Largest idle loop (bytes) that may hold PC. */
  k_run_wall_s      = 120U,    /**< Wall-clock safety bound (seconds).          */
  k_run_inner_max   = 4096U,   /**< Per-chunk exception-resolve relaunch cap.   */
  k_env_strtol_base = 10U,     /**< Decimal base for env-var integer parse.     */
} emu_budget_t;

/**
 * @enum emu_exit_t
 * @brief Process exit codes for the #67 run-every-example matrix.
 *
 * @details The matrix keys off the process exit code (not the stderr banner):
 * a clean run-to-budget returns success; a firmware BKPT, an emulation fault,
 * or the wall-clock timeout each return a distinct non-zero code so a wedged or
 * trapped run is distinguishable from a healthy one.
 */
typedef enum : int {
  k_emu_exit_ok      = 0, /**< Clean run-to-budget (no fault/BKPT/timeout).   */
  k_emu_exit_fault   = 1, /**< Emulation fault / invalid access ended it.     */
  k_emu_exit_bkpt    = 2, /**< Firmware executed a BKPT (assert/give-up).     */
  k_emu_exit_timeout = 3, /**< Wall-clock budget reached before a clean stop. */
} emu_exit_t;

/**
 * @enum emu_misc_t
 * @brief Named constants for ELF parsing, Thumb decode, and assorted literals.
 */
typedef enum : uint32_t {
  /* Thumb / conditional-select instruction decode. */
  k_thumb_op5_shift = 11U,   /**< op5 = hw0[15:11].                  */
  k_thumb_op5_mask  = 0x1FU, /**< 5-bit op5 field.                   */
  k_thumb32_op5_min = 0x1DU, /**< op5 >= this -> 32-bit instruction. */
  k_cs_op_shift     = 12U,   /**< CSEL-family op = hw2[13:12].       */
  k_cs_op_mask      = 0x3U,  /**< 2-bit op field.                    */
  /* Assorted. */
  k_max_panel_px    = 4096U, /**< Largest accepted --size dimension.   */
  k_record_dir_mode = 0755U, /**< mkdir mode for the --record dir.     */
  k_dump_sym_max    = 8U,    /**< Max --dump-sym globals per run.      */
  k_sectors_per_mib = 2048U, /**< 512-byte sectors per MiB (--sd-new). */
} emu_misc_t;

/** @brief 64-bit byte/size units used by --sd-new card sizing. */
typedef enum : uint64_t {
  k_bytes_per_sector = 512ULL,        /**< SD logical sector size in bytes.    */
  k_size_kib         = 1024ULL,       /**< One kibibyte (k suffix multiplier). */
  k_sd_u32_max       = 0xFFFFFFFFULL, /**< 32-bit sector-count ceiling.        */
  k_fat32_min_mib    = 512ULL,        /**< FAT32 default threshold, in MiB.    */
} emu_size_t;

/**
 * @struct emu_run_cfg_t
 * @brief Everything setup hands the run loop: engine, image, CLI products.
 *
 * @details One read-only bundle of the values main()'s parse + setup phases
 * produce, so the run loop, report and exit-code mapping consume exactly the
 * state they always did.
 *
 * @invariant The referenced buffers/strings outlive emu_run_and_report()
 *            (argv strings and the run-long ELF image).
 * @see emu_run_and_report()  The consumer.
 * @since 0.1.0
 */
typedef struct {
  uc_engine*         uc;              /**< The prepared cpu0 engine.                 */
  uint8_t*           elf;             /**< Firmware image (owned; freed at run end). */
  long               elf_len;         /**< Image length in bytes.                    */
  uint32_t           initial_pc;      /**< Reset-vector PC (Thumb bit set).          */
  uint32_t           vtor_base;       /**< VTOR fallback (the MRAM vector base).     */
  bool               want_trace;      /**< --trace (forwarded to warm reboots).      */
  bool               want_view;       /**< --view (live window requested).           */
  bool               want_click;      /**< --click armed.                            */
  int                click_x;         /**< --click column (composite pixels).        */
  int                click_y;         /**< --click row (composite pixels).           */
  const char*        ppm_path;        /**< --ppm output path (NULL when unused).     */
  const char*        record_dir;      /**< --record directory (NULL when unused).    */
  uint32_t           record_secs;     /**< --record-secs bound (0 = unbounded).      */
  uint32_t           rotate_deg;      /**< --rotate display rotation.                */
  int                reboot_count;    /**< --reboot N scheduled warm reboots.        */
  const char*        save_sd_path;    /**< --save-sd dump path (NULL when unused).   */
  uint32_t           stop_sym_addr;   /**< --stop-sym resolved address (0 = off).    */
  uint32_t           stop_sym_thresh; /**< --stop-sym threshold value.               */
  const char* const* dump_sym_names;  /**< --dump-sym names (argv-backed).           */
  const uint32_t*    dump_sym_addrs;  /**< --dump-sym resolved addresses.            */
  uint32_t           dump_sym_n;      /**< Count of --dump-sym entries.              */
  uint16_t           view_w;          /**< Panel width in pixels.                    */
  uint16_t           view_h;          /**< Panel height in pixels.                   */
  const char*        win_title;       /**< Window / sidebar caption.                 */
} emu_run_cfg_t;

/**
 * @struct run_guards_t
 * @brief The run-guard knobs (env-tunable budgets + stop conditions).
 *
 * @details Read once before the run loop from the RA8_EMU_* environment
 * (CLICK_SETTLE / WALL_S / MAX_CHUNKS / IDLE_STOP / USB_STOP / USBH_STOP /
 * STOP_ON / STOP_PC / the profiler idle-stop tunables), preserving the
 * documented semantics -- notably WALL_S=0 truly DISABLES the CPU-time guard
 * (the #168 footgun).
 *
 * @invariant Values are fixed for the whole run once read.
 * @see run_read_guards()  The reader.
 * @since 0.1.0
 */
typedef struct {
  uint32_t    click_settle_chunks; /**< Post-click drain window (chunks).        */
  uint32_t    max_chunks;          /**< Outer-chunk budget.                      */
  double      wall_s;              /**< CPU-time guard bound, seconds.           */
  bool        wall_guard_on;       /**< false: WALL_S=0 disabled the guard.      */
  uint32_t    idle_stop_chunks;    /**< Idle steady-state stop (0 = off).        */
  uint32_t    usb_stop_settle;     /**< USB device-mode early-stop settle.       */
  uint32_t    usbh_stop_settle;    /**< USB host-mode early-stop settle.         */
  const char* stop_on;             /**< Console-banner stop substring (or NULL). */
  uint32_t    prof_idle_insns;     /**< Profiler idle chunk threshold.           */
  uint32_t    prof_idle_need;      /**< Consecutive idle chunks that stop.       */
  uint32_t    prof_idle_arm;       /**< Chunks before the profiler stop arms.    */
} run_guards_t;

/**
 * @brief Read the run-guard environment knobs (see ::run_guards_t).
 *
 * @details Moved verbatim from the run preamble: every default, override
 * precedence and --view/--record interaction is unchanged, including the
 * --record-secs chunk bound and the RA8_EMU_STOP_PC hand-off to the
 * profiler.
 *
 * @param[in] cfg  The run configuration (record/click fields).
 * @param[in] view The live window handle (NULL when headless).
 * @return The populated guard set.
 * @retval (by value) Every field holds its default or env override.
 * @pre The environment is stable for the run.
 * @pre @p cfg outlives the call.
 * @post RA8_EMU_STOP_PC (if set) has been handed to the profiler.
 * @post With --record active, the frame directory exists (mkdir) and the
 *       recording banner has been printed to stderr.
 * @note Not thread-safe; call once during setup.
 * @since 0.1.0
 */
RA8_PRIV run_guards_t run_read_guards(const emu_run_cfg_t* cfg, const board_view_t* view);

/**
 * @brief Run the firmware to a stop condition, print the report, map the exit code.
 *
 * @details
 * Allocates the presentation buffers (window / --ppm / --record / --click),
 * reads the run-guard environment knobs, executes the chunked run loop --
 * one SysTick period per outer chunk, exceptions resolved to a steady state
 * by the inner loop, cpu1 interleaved, seven stop conditions -- then prints
 * the run-end report (stop cause, telemetry, peripheral summaries, MMIO
 * table, --dump-sym probes), writes the --ppm / --record outputs, holds the
 * live window until closed, saves the SD image, releases the buffers and the
 * engine, and returns the #67 matrix exit code.
 *
 * @param[in] cfg The setup products (see ::emu_run_cfg_t).
 * @return Process exit status.
 * @retval 0 Clean run-to-budget (no fault / BKPT / timeout).
 * @retval 1 Emulation fault / invalid access ended the run.
 * @retval 2 Firmware executed a BKPT (assert / give-up).
 * @retval 3 Wall-clock budget reached before a clean stop.
 * @pre @p cfg is fully populated and its engine is ready to run.
 * @pre The seams / hooks are installed.
 * @post The engine is closed and the image buffer freed.
 * @note Not thread-safe; this IS the single-threaded run.
 * @since 0.1.0
 */
RA8_PRIV int emu_run_and_report(const emu_run_cfg_t* cfg);

#ifdef __cplusplus
}
#endif
