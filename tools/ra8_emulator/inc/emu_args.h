/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file emu_args.h
 * @brief ra8_emulator command-line parsing (usage text + option decoding)
 *
 * @details
 * Decodes the ra8_emulator CLI into one ::emu_args_t bundle: the firmware image
 * path, presentation options (--view / --ppm / --record / --rotate / --panel
 * / --size), input injectors (--click / --touch-seq / --input / --keys /
 * --usb-in / --button / --battery), storage attachments (--sd / --sd-new /
 * --save-sd / --fast-sd / --eink / --modem), the probe family (--dump-sym /
 * --stop-sym / --trace-sym), the TrustZone second image (--ns), and the
 * device / core / power model selectors. Options that attach a peripheral
 * model or set a global model knob take effect during the parse, exactly as
 * the original inline loop did.
 *
 * Split out of the ra8_emulator main translation unit; behaviour unchanged.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "board_periph.h"
#include "emu_run.h"
#include "emu_trace.h"
#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct emu_args_t
 * @brief Every value the ra8_emulator CLI parse produces for the setup phase.
 *
 * @details One bundle of the decoded options, in the exact types the setup
 * and run phases consume. Pointer fields alias argv strings, so the struct
 * is valid for the lifetime of main(). Fields a later phase resolves from
 * the ELF (the --dump-sym addresses, the --stop-sym address) intentionally
 * stay OUT of this struct: only raw CLI products live here.
 *
 * @invariant String members alias argv and are never freed.
 * @invariant dump_sym_n <= k_dump_sym_max and trace_sym_n <= k_trace_sym_max.
 * @see emu_args_parse()  The producer.
 * @since 0.1.0
 */
typedef struct {
  const char*    elf_path;                         /**< Firmware ELF path (argv[1]).            */
  bool           want_view;                        /**< --view live window.                     */
  const char*    ppm_path;                         /**< --ppm output path (NULL = off).         */
  const char*    record_dir;                       /**< --record directory (NULL = off).        */
  uint32_t       record_secs;                      /**< --record-secs bound (0 = unbounded).    */
  uint32_t       rotate_deg;                       /**< --rotate display rotation (0 default).  */
  const char*    panel_path;                       /**< --panel descriptor path (NULL = none).  */
  const char*    input_str;                        /**< --input console RX string.              */
  const char*    keys_str;                         /**< --keys window-key path string.          */
  const char*    touch_seq_str;                    /**< --touch-seq raw-point FIFO string.      */
  const char*    usb_in_str;                       /**< --usb-in CDC bulk OUT string.           */
  const char*    dump_sym_names[k_dump_sym_max];   /**< --dump-sym globals to read.             */
  uint32_t       dump_sym_n;                       /**< Count of --dump-sym entries.            */
  const char*    stop_sym_name;                    /**< --stop-sym watch global (NULL = off).   */
  uint32_t       stop_sym_thresh;                  /**< --stop-sym stop threshold.              */
  const char*    trace_sym_names[k_trace_sym_max]; /**< --trace-sym functions to log.           */
  uint32_t       trace_sym_n;                      /**< Count of --trace-sym entries.           */
  const char*    save_sd_path;                     /**< --save-sd dump path (NULL = off).       */
  const char*    ns_elf_path;                      /**< --ns second (NS) image path.            */
  bool           size_set;                         /**< --size given (wins over --panel).       */
  bool           want_click;                       /**< --click armed with valid coordinates.   */
  bool           want_trace;                       /**< --trace transition logging.             */
  int            click_x;                          /**< --click column (-1 = unset).            */
  int            click_y;                          /**< --click row (-1 = unset).               */
  int            button_press;                     /**< --button: 1=SW1, 2=SW2, 0=none.         */
  int            reboot_count;                     /**< --reboot N warm reboots.                */
  int            battery_soc;                      /**< --battery pct (-1 = model default).     */
  bool           battery_charging;                 /**< --charge given.                         */
  bool           battery_opt;                      /**< Any battery flag given.                 */
  uint16_t       view_w;                           /**< Panel width in pixels.                  */
  uint16_t       view_h;                           /**< Panel height in pixels.                 */
  board_device_t emu_device;                       /**< --device selected part (RA8D2 default). */
  bool           usbhs_loop;                       /**< --usbhs-loop chip-internal self-loop.   */
} emu_args_t;

/**
 * @brief Parse the ra8_emulator command line into @p out (or print usage).
 *
 * @details
 * With no firmware argument the full usage text is printed to stderr and
 * false is returned (the caller exits 2). Otherwise the option loop decodes
 * argv[2..] into @p out with the original loop's exact semantics, including
 * the parse-time side effects: --sd / --sd-new attach the SD model, --eink /
 * --modem attach their device models, --fast-sd enables the block seam, and
 * --low-power / --primary-core set the core-model knobs.
 *
 * @param[in]  argc Argument count from main().
 * @param[in]  argv Argument vector from main(); strings are aliased by @p out.
 * @param[out] out  Receives every decoded option (fully overwritten).
 *
 * @return Whether a firmware path was supplied and the parse ran.
 * @retval true  @p out is populated; parse-time attachments are in effect.
 * @retval false No firmware argument; usage was printed, nothing decoded.
 *
 * @pre @p argv has @p argc entries that outlive the run (main's argv).
 * @pre @p out is non-null.
 * @post On true, @p out->elf_path is argv[1] and all fields hold their
 *       defaults or decoded values.
 * @post On false, stderr carries the usage text and @p out is untouched.
 *
 * @note Not thread-safe; call once from main() before setup.
 * @see emu_run_and_report()  Consumes the values via ::emu_run_cfg_t.
 * @since 0.1.0
 */
RA8_PRIV bool emu_args_parse(int argc, char** argv, emu_args_t* out);

#ifdef __cplusplus
}
#endif
