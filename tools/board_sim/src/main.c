/**
 * @file main.c
 * @brief RA8D2 board emulator -- boot a real .elf on a CPU emulator, with ticks
 *
 * @details
 * Loads an EK-RA8D2 firmware ELF into an emulated Cortex-M memory map (Unicorn,
 * QEMU's CPU core as a library) and boots it from the vector table, with the
 * RA8D2 peripheral space modelled as logged MMIO.
 *
 * Feasibility (proven): Unicorn 2.x tops out at Cortex-M33 (Armv8-M) while the
 * RA8D2 is M85 (Armv8.1-M), yet the GCC-built firmware executes on the M33 core
 * -- no v8.1-M-only opcode (e.g. low-overhead loops) is emitted on the boot
 * path. The invalid-instruction trap below still reports exactly where and what
 * if that ever changes.
 *
 * Time: bare-metal delays here are SysTick-driven (``ra8_time`` enables SysTick
 * with TICKINT and counts exceptions). Nothing advances time on a plain memory
 * model, so the run loop is chunked and, between chunks, cooperatively invokes
 * the firmware's installed SysTick_Handler as a function -- its tick-counter
 * memory write persists while the interrupted context's registers are restored,
 * which is precisely a real SysTick IRQ's observable effect. This carries the
 * firmware past ``ra8_delay_ms`` so it reaches its main loop (e.g. driving the
 * GLCDC), instead of spinning forever on a tick that never increments.
 *
 *   board_sim <firmware.elf>
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <capstone/capstone.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unistd.h>

#include "board_console.h"
#include "board_input.h"
#include "board_net.h"
#include "board_overlay.h"
#include "board_periph.h"
#include "board_periph_eink.h"
#include "board_periph_modem.h"
#include "board_periph_sd.h"
#include "board_usb.h"
#include "board_usb_host.h"
#include "board_view.h"
#include "sim_console.h"
#include "sim_cpu1.h"
#include "sim_elf.h"
#include "sim_engine.h"
#include "sim_exc.h"
#include "sim_memmap.h"
#include "sim_mmio.h"
#include "sim_mpu.h"
#include "sim_prof.h"
#include "sim_seams.h"
#include "sim_trace.h"
#include "sim_tz.h"
#include "sim_usbh_seam.h"
#include "sim_view.h"

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
} sim_budget_t;

/**
 * @enum board_sim_exit_t
 * @brief Process exit codes for the #67 run-every-example matrix.
 *
 * @details The matrix keys off the process exit code (not the stderr banner):
 * a clean run-to-budget returns success; a firmware BKPT, an emulation fault,
 * or the wall-clock timeout each return a distinct non-zero code so a wedged or
 * trapped run is distinguishable from a healthy one.
 */
typedef enum : int {
  k_board_sim_exit_ok      = 0, /**< Clean run-to-budget (no fault/BKPT/timeout).   */
  k_board_sim_exit_fault   = 1, /**< Emulation fault / invalid access ended it.     */
  k_board_sim_exit_bkpt    = 2, /**< Firmware executed a BKPT (assert/give-up).     */
  k_board_sim_exit_timeout = 3, /**< Wall-clock budget reached before a clean stop. */
} board_sim_exit_t;

/**
 * @enum board_sim_misc_t
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
} board_sim_misc_t;

/** @brief 64-bit byte/size units used by --sd-new card sizing. */
typedef enum : uint64_t {
  k_bytes_per_sector = 512ULL,        /**< SD logical sector size in bytes.    */
  k_size_kib         = 1024ULL,       /**< One kibibyte (k suffix multiplier). */
  k_sd_u32_max       = 0xFFFFFFFFULL, /**< 32-bit sector-count ceiling.        */
  k_fat32_min_mib    = 512ULL,        /**< FAT32 default threshold, in MiB.    */
} board_sim_size_t;

int main(int argc, char** argv)
{
  if (argc < 2) {
    (void)fprintf(
      stderr,
      "usage: board_sim <firmware.elf> [--view] [--ppm <out.ppm>]"
      " [--panel <file.toml>] [--size WxH] [--click X Y] [--touch-seq S] [--input <str>]"
      " [--sd <image>]"
      " [--usb-in <str>] [--button <1|2>] [--reboot <N>] [--dump-sym <name>]"
      " [--stop-sym <name> <N>]"
      " [--trace-sym <name>] [--sd-new <N[k|m|g][:fat16|fat32]>] [--save-sd <out>] [--fast-sd]"
      " [--eink] [--device ra8d2|ra8p1]\n"
      "  --view          open a macOS window: live board view; click panel"
      " (touch) / on-screen SW1/SW2, type -> UART\n"
      "  --ppm <file>    write the final composite (panel + status) to a PPM\n"
      "  --record <dir>  record frames (panel + status) to <dir>/frame_NNNNNN.ppm\n"
      "  --record-secs N headless: record N emulated seconds, then stop (~20 fps)\n"
      "  --rotate DEG    display rotation 90/180/270 (e.g. portrait)\n"
      "  --panel <file>  display descriptor (name/width/height) to size the panel\n"
      "  --size WxH      panel size in pixels; overrides --panel (default 1024x600)\n"
      "  --click X Y     headless: inject one touch at X,Y once the UI is up\n"
      "  --touch-seq S   headless: queue raw GT911 taps \"x0:y0,x1:y1,...\" served\n"
      "                  one per drained frame (multi-tap flows, e.g. touch_cal)\n"
      "  --input <str>   feed <str> to the console UART RX (SCI8); \\n / \\r / \\t ok\n"
      "  --keys <str>    type <str> via the window-key path -> console UART RX\n"
      "  --usb-in <str>  feed <str> to the USB CDC bulk OUT pipe (echo test)\n"
      "  --button <1|2>  hold user switch SW1 (P009) / SW2 (P008) pressed\n"
      "  --battery <pct> set the fuel-gauge state-of-charge (0..100; MAX17048 @ 0x36)\n"
      "  --charge        mark the battery charging (fuel-gauge CRATE reads positive)\n"
      "  --reboot <N>    warm-reboot N times (re-run from reset vector; VBATT-backup\n"
      "                  + reset-cause survive) to exercise reset-survival apps\n"
      "  --sd <image>    serve a FAT/exFAT image as the microSD card (read + write)\n"
      "  --sd-new <N[k|m|g][:fat16|fat32]>  blank FAT card of N MiB (k/m/g unit; e.g. 30g)\n"
      "  --save-sd <out> after the run, dump the SD card image (with firmware writes)\n"
      "  --fast-sd       serve SD blocks direct from the image (skip the per-byte SPI\n"
      "                  protocol; byte-identical render) so a big book loads fast; opt-in\n"
      "  --eink          attach a modelled IT8951 e-paper controller on SPI_B (drives\n"
      "                  the ra8_epaper path: HRDY ready, GET_DEV_INFO, load + display)\n"
      "  --dump-sym <s>  print 32-bit global <s> from memory after the run (memprobe)\n"
      "  --stop-sym <s> <N>  end the run once 32-bit global <s> reaches N (counter memprobe)\n"
      "  --trace-sym <s> log every entry to function <s> (+LR): trace a bring-up path\n"
      "  --trace         log each LED/GPIO transition + NVIC IRQ as it happens\n"
      "  --primary-core m85|m33  label the primary core; m33 leaves the M85-only\n"
      "                  instruction seams (MVE / long-shift) off (default m85)\n"
      "  --device ra8d2|ra8p1  select the emulated part (default ra8d2). ra8p1 adds\n"
      "                  the Ethos-U55 NPU (0x40140000) as a mapped-but-unmodelled\n"
      "                  window; else identical to the RA8D2 model\n"
      "  --low-power     model the M33's 4:1-slower clock (1/4 chunk budget); the\n"
      "                  GUI low-power button toggles it live under --view\n");
    return 2;
  }
  const char* elf_path                         = argv[1];
  bool        want_view                        = false;
  const char* ppm_path                         = nullptr;
  const char* record_dir                       = nullptr;
  uint32_t    record_secs                      = 0U;
  uint32_t    rotate_deg                       = (uint32_t)k_rotate_0;
  const char* panel_path                       = nullptr;
  const char* input_str                        = nullptr;
  const char* keys_str                         = nullptr;
  const char* touch_seq_str                    = nullptr; /* --touch-seq raw-point FIFO. */
  const char* usb_in_str                       = nullptr;
  const char* dump_sym_names[k_dump_sym_max]   = {}; /* --dump-sym globals to read.  */
  uint32_t    dump_sym_addrs[k_dump_sym_max]   = {}; /* resolved while ELF is alive. */
  uint32_t    dump_sym_n                       = 0U;
  const char* stop_sym_name                    = nullptr; /* --stop-sym watch global.      */
  uint32_t    stop_sym_addr                    = 0U;      /* resolved while ELF is alive.  */
  uint32_t    stop_sym_thresh                  = 0U;      /* stop once *global >= this.    */
  const char* trace_sym_names[k_trace_sym_max] = {};      /* --trace-sym functions to log. */
  uint32_t    trace_sym_n                      = 0U;
  const char* save_sd_path                     = nullptr; /* --save-sd dump path. */
  const char* ns_elf_path                      = nullptr; /* --ns: 2nd (NS) elf.  */
  bool        size_set                         = false;
  bool        want_click                       = false;
  bool        want_trace                       = false;
  int         click_x                          = -1;
  int         click_y                          = -1;
  int         button_press                     = 0;     /* 1=SW1, 2=SW2, 0=none.        */
  int         reboot_count                     = 0;     /* --reboot N: warm reboots.    */
  int         battery_soc                      = -1;    /* --battery <pct>, -1=default. */
  bool        battery_charging                 = false; /* --charge.                    */
  bool        battery_opt                      = false; /* any battery flag given.      */
  uint16_t    view_w                           = (uint16_t)k_view_default_w;
  uint16_t    view_h                           = (uint16_t)k_view_default_h;

  board_device_t sim_device = k_board_device_ra8d2; /* --device (RA8P1 profile). */
  bool           usbhs_loop = false;                /* --usbhs-loop: chip-internal USB self-loop. */
  for (int i = 2; i < argc; i++) {
    if (strncmp(argv[i], "--view", sizeof("--view")) == 0) {
      want_view = true;
    } else if (strncmp(argv[i], "--usbhs-loop", sizeof("--usbhs-loop")) == 0) {
      /* Model the on-chip USBHS host controller (board_periph_usbhs_host.c) and
       * bridge it to the USBFS device, instead of the ra8_usb_host_* function
       * seam: for apps whose HS host enumerates the SAME chip's FS device. */
      usbhs_loop = true;
    } else if (strncmp(argv[i], "--trace", sizeof("--trace")) == 0) {
      want_trace = true;
    } else if (strncmp(argv[i], "--low-power", sizeof("--low-power")) == 0) {
      sim_set_low_power(true);
    } else if ((strncmp(argv[i], "--primary-core", sizeof("--primary-core")) == 0) &&
               ((i + 1) < argc)) {
      sim_set_primary_core((strncmp(argv[i + 1], "m33", sizeof("m33")) == 0) ? k_core_m33
                                                                             : k_core_m85);
      i++;
    } else if ((strncmp(argv[i], "--device", sizeof("--device")) == 0) && ((i + 1) < argc)) {
      /* Select the emulated part; anything but "ra8p1" (incl. "ra8d2") = RA8D2. */
      sim_device = (strncmp(argv[i + 1], "ra8p1", sizeof("ra8p1")) == 0) ? k_board_device_ra8p1
                                                                         : k_board_device_ra8d2;
      i++;
    } else if ((strncmp(argv[i], "--ppm", sizeof("--ppm")) == 0) && ((i + 1) < argc)) {
      ppm_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--record-secs", sizeof("--record-secs")) == 0) &&
               ((i + 1) < argc)) {
      const long s = strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      record_secs  = (s > 0L) ? (uint32_t)s : 0U;
      i++;
    } else if ((strncmp(argv[i], "--record", sizeof("--record")) == 0) && ((i + 1) < argc)) {
      record_dir = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--rotate", sizeof("--rotate")) == 0) && ((i + 1) < argc)) {
      const long deg = strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      if ((deg == (long)k_rotate_90) || (deg == (long)k_rotate_180) ||
          (deg == (long)k_rotate_270)) {
        rotate_deg = (uint32_t)deg;
      }
      i++;
    } else if ((strncmp(argv[i], "--panel", sizeof("--panel")) == 0) && ((i + 1) < argc)) {
      panel_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--ns", sizeof("--ns")) == 0) && ((i + 1) < argc)) {
      /* Second ELF: the Non-Secure image of a two-image TrustZone app (src/app).
       * Loaded at its LMA so the Secure boot's NS-image copy + BLXNS land on it. */
      ns_elf_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--input", sizeof("--input")) == 0) && ((i + 1) < argc)) {
      input_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--keys", sizeof("--keys")) == 0) && ((i + 1) < argc)) {
      keys_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--touch-seq", sizeof("--touch-seq")) == 0) && ((i + 1) < argc)) {
      touch_seq_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--usb-in", sizeof("--usb-in")) == 0) && ((i + 1) < argc)) {
      usb_in_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--sd", sizeof("--sd")) == 0) && ((i + 1) < argc)) {
      (void)board_sd_attach(argv[i + 1]); /* serve this FAT image to ra8_sdmmc_spi */
      i++;
    } else if ((strncmp(argv[i], "--sd-new", sizeof("--sd-new")) == 0) && ((i + 1) < argc)) {
      /* "<N>[k|m|g|t][:fat16|fat32]" -- create + attach a blank formatted card.
       * A bare number is MiB; a k/m/g/t suffix sets the unit (so "30g" = 30 GiB).
       * Format defaults by size (FAT32 >= 512 MiB) like a real SD card; FAT16
       * cannot exceed its cluster ceiling, so multi-GB cards are FAT32. */
      char*      endp = nullptr;
      const long num  = strtol(argv[i + 1], &endp, (int)k_strtol_base10);
      uint64_t   mult =
        (uint64_t)k_sectors_per_mib * (uint64_t)k_bytes_per_sector; /* default unit: MiB. */
      const char unit = ((endp != nullptr) && (*endp != '\0')) ? (char)tolower((int)*endp) : 'm';
      if (unit == 'k') {
        mult = (uint64_t)k_size_kib;
      } else if (unit == 'g') {
        mult = (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
      } else if (unit == 't') {
        mult =
          (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
      }
      const uint64_t bytes   = (num > 0L) ? ((uint64_t)num * mult) : 0ULL;
      const uint64_t sectors = bytes / (uint64_t)k_bytes_per_sector;
      const char*    colon   = (endp != nullptr) ? strchr(endp, ':') : nullptr;
      const uint64_t fat32_min_bytes =
        (uint64_t)k_fat32_min_mib * (uint64_t)k_size_kib * (uint64_t)k_size_kib;
      uint8_t fat = (bytes >= fat32_min_bytes) ? (uint8_t)32U : (uint8_t)16U;
      if (colon != nullptr) {
        fat = (strstr(colon, "32") != nullptr) ? (uint8_t)32U : (uint8_t)16U;
      }
      if ((sectors > 0ULL) && (sectors <= (uint64_t)k_sd_u32_max)) {
        (void)board_sd_attach_blank((uint32_t)sectors, fat, "BOARDSIM");
      } else if (sectors > (uint64_t)k_sd_u32_max) {
        (void)fprintf(stderr, "board_sim: --sd-new: size exceeds the 2 TiB FAT limit\n");
      }
      i++;
    } else if ((strncmp(argv[i], "--save-sd", sizeof("--save-sd")) == 0) && ((i + 1) < argc)) {
      save_sd_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--dump-sym", sizeof("--dump-sym")) == 0) && ((i + 1) < argc)) {
      if (dump_sym_n < (uint32_t)k_dump_sym_max) {
        dump_sym_names[dump_sym_n] = argv[i + 1];
        dump_sym_n++;
      }
      i++;
    } else if ((strncmp(argv[i], "--stop-sym", sizeof("--stop-sym")) == 0) && ((i + 2) < argc)) {
      /* End the run the instant 32-bit global <name> reaches <threshold> -- the
       * counter analog of BOARD_SIM_STOP_ON, for the jlink_memprobe SIL mode: a
       * free-running progress counter that hits its floor stops the run at once,
       * so a passing probe finishes in a few chunks instead of the full budget. */
      stop_sym_name   = argv[i + 1];
      stop_sym_thresh = (uint32_t)strtoul(argv[i + 2], nullptr, (int)k_strtol_base10);
      i += 2;
    } else if ((strncmp(argv[i], "--trace-sym", sizeof("--trace-sym")) == 0) && ((i + 1) < argc)) {
      if (trace_sym_n < (uint32_t)k_trace_sym_max) {
        trace_sym_names[trace_sym_n] = argv[i + 1];
        trace_sym_n++;
      }
      i++;
    } else if (strncmp(argv[i], "--fast-sd", sizeof("--fast-sd")) == 0) {
      sim_fast_sd_enable();
    } else if (strncmp(argv[i], "--eink", sizeof("--eink")) == 0) {
      (void)board_eink_attach(); /* answer the ra8_epaper SPI path (IT8951 model) */
    } else if (strncmp(argv[i], "--modem", sizeof("--modem")) == 0) {
      (void)board_modem_attach(); /* answer the ra8_modem_at AT path (SCI7 modem model) */
    } else if ((strncmp(argv[i], "--click", sizeof("--click")) == 0) && ((i + 2) < argc)) {
      click_x    = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      click_y    = (int)strtol(argv[i + 2], nullptr, (int)k_strtol_base10);
      want_click = (click_x >= 0) && (click_y >= 0);
      i += 2;
    } else if ((strncmp(argv[i], "--button", sizeof("--button")) == 0) && ((i + 1) < argc)) {
      button_press = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      i += 1;
    } else if ((strncmp(argv[i], "--battery", sizeof("--battery")) == 0) && ((i + 1) < argc)) {
      battery_soc = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      battery_opt = true;
      i += 1;
    } else if (strncmp(argv[i], "--charge", sizeof("--charge")) == 0) {
      battery_charging = true;
      battery_opt      = true;
    } else if ((strncmp(argv[i], "--reboot", sizeof("--reboot")) == 0) && ((i + 1) < argc)) {
      reboot_count = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      i += 1;
    } else if ((strncmp(argv[i], "--size", sizeof("--size")) == 0) && ((i + 1) < argc)) {
      char*      end = nullptr;
      const long w   = strtol(argv[i + 1], &end, (int)k_strtol_base10);
      const long h =
        ((end != nullptr) && (*end == 'x')) ? strtol(end + 1, nullptr, (int)k_strtol_base10) : 0L;
      if ((w > 0L) && (w <= (long)k_max_panel_px) && (h > 0L) && (h <= (long)k_max_panel_px)) {
        view_w   = (uint16_t)w;
        view_h   = (uint16_t)h;
        size_set = true;
      }
      i++;
    }
  }

  /* A --panel descriptor sizes the window to that display (so the emulator can
   * present any panel, not just 1024x600); an explicit --size still wins. */
  board_panel_t panel      = {};
  bool          have_panel = false;
  if (panel_path != nullptr) {
    have_panel = load_panel(panel_path, &panel);
    if (have_panel && !size_set) {
      view_w = panel.width;
      view_h = panel.height;
    }
  }
  const char* win_title = (have_panel && (panel.name[0] != '\0')) ? panel.name : "board_sim";

  uc_engine* uc = nullptr;
  if (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), &uc) != UC_ERR_OK) {
    (void)fprintf(stderr, "uc_open failed\n");
    return 1;
  }
  /* Closest emulated core to the M85: M33 (Armv8-M). */
  (void)uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M33);

  if (!sim_memmap_init(uc)) {
    return 1;
  }

  /* Seed read-only/hardwired SCS registers the firmware reads back. The PPB is
   * plain RAM here, so MPU_TYPE would otherwise read 0 (DREGION=0) and
   * ra8_mpu_configure() would reject any region (region_count > 0 implemented),
   * panicking mpu_partition_simple. Hardwire DREGION to the M85's 8 data
   * regions so the configure path validates; board_sim does not yet *enforce*
   * MPU permissions, so the app then takes its documented no-trap host path. */
  wr32(uc, (uint64_t)k_mpu_type, (uint32_t)k_mpu_type_seed);

  /* Reset the peripheral-model framework (GPIO/PORT, AGT/GPT timers, ICU/NVIC,
   * SCI UART) before the firmware boots so its register writes land in real
   * block state. Install the console sink so transmitted bytes reach stdout,
   * and queue any --input as console-channel (SCI8) RX. */
  board_periph_init(want_trace);
  /* --touch-seq "x0:y0,x1:y1,...": load the modelled GT911 injection FIFO with
   * synthetic raw points for a headless multi-tap flow (touch_cal's five-point
   * calibration). Loaded AFTER board_periph_init so the per-block reset above
   * does not clear it; the GT911 model then serves one queued point per drained
   * frame through the genuine ra8_touch_read decode. */
  if (touch_seq_str != nullptr) {
    enum : uint32_t {
      k_touch_seq_parse_max = 32U, /**< Bounded pair-parse iterations (NASA R2). */
    };
    board_periph_touch_seq_reset();
    const char* p      = touch_seq_str;
    uint32_t    pushed = 0U;
    for (uint32_t n = 0U; (n < (uint32_t)k_touch_seq_parse_max) && (*p != '\0'); n++) {
      char*      end = nullptr;
      const long x   = strtol(p, &end, (int)k_strtol_base10);
      if ((end == p) || (*end != ':')) {
        break;
      }
      p            = end + 1;
      const long y = strtol(p, &end, (int)k_strtol_base10);
      if (end == p) {
        break;
      }
      p = end;
      if ((x >= 0L) && (y >= 0L) && board_periph_touch_seq_push((uint16_t)x, (uint16_t)y)) {
        pushed++;
      }
      if (*p == ',') {
        p++;
      }
    }
    (void)fprintf(stderr, "board_sim: --touch-seq armed %u raw point(s)\n", (unsigned)pushed);
  }
  /* Select the emulated part BEFORE the run: gates the RA8P1-only NPU block.
   * Default RA8D2 leaves every RA8D2 run unchanged. */
  board_periph_set_device(sim_device);
  /* --usbhs-loop: activate the on-chip USBHS host model (owns 0x40351000) and
   * hand the USBFS device over to its bridge so the built-in virtual host stands
   * down -- one real host drives the device, closing the chip-internal loop. */
  board_periph_set_usbhs_loop(usbhs_loop);
  board_usb_set_external_host(usbhs_loop);
  board_net_init(want_trace);
  board_periph_sci_set_tx_sink(console_tx_sink);
  /* --button N: hold a user switch pressed (active-low) before the firmware
   * boots, so a button-polling app (e.g. gpio_input_demo: SW1 -> LED1) takes
   * its pressed path. SW1 = P009, SW2 = P008. */
  if (button_press != 0) {
    const uint8_t pin = (button_press == 2) ? (uint8_t)k_sim_sw2_pin : (uint8_t)k_sim_sw1_pin;
    board_periph_gpio_set_input((uint8_t)k_sim_sw_port, pin, false);
    (void)fprintf(stderr,
                  "board_sim: --button %d held (SW pin P00%u low/pressed)\n",
                  button_press,
                  (unsigned)pin);
  }
  if (battery_opt) {
    uint8_t cur_soc = 0U;
    board_periph_battery_get(&cur_soc, nullptr);
    const uint8_t soc = (battery_soc >= 0) ? (uint8_t)battery_soc : cur_soc;
    board_periph_battery_set(soc, battery_charging);
    (void)fprintf(stderr,
                  "board_sim: battery %u%% %s (MAX17048 @ I2C 0x36)\n",
                  (unsigned)soc,
                  battery_charging ? "charging" : "discharging");
  }
  if (input_str != nullptr) {
    uint8_t        rx[k_uart_line_max];
    const uint32_t n = decode_escapes(input_str, rx, (uint32_t)sizeof(rx));
    board_periph_sci_feed_rx(board_periph_sci_console_channel(), rx, n);
    (void)fprintf(stderr,
                  "board_sim: queued %u byte(s) to SCI%u RX from --input\n",
                  n,
                  board_periph_sci_console_channel());
  }
  /* --keys: push a scripted string through the SAME keystroke FIFO the live
   * --view window feeds, so the run loop drains it to the console UART RX over
   * the identical path -- a headless, deterministic test of the keyboard input
   * (no window, no OS key events). */
  if (keys_str != nullptr) {
    uint8_t        kb[k_uart_line_max];
    const uint32_t n = decode_escapes(keys_str, kb, (uint32_t)sizeof(kb));
    for (uint32_t k = 0U; k < n; k++) {
      board_input_push_key((char)kb[k]);
    }
    (void)fprintf(stderr, "board_sim: queued %u keystroke(s) via --keys (window-key path)\n", n);
  }
  /* Queue any --usb-in bytes for the virtual host to push over the CDC bulk OUT
   * pipe once the device is configured; the device echoes them back on bulk IN. */
  if (usb_in_str != nullptr) {
    uint8_t        ub[k_uart_line_max];
    const uint32_t n = decode_escapes(usb_in_str, ub, (uint32_t)sizeof(ub));
    board_usb_feed_bulk_in(ub, n);
    (void)fprintf(stderr, "board_sim: queued %u byte(s) to the USB CDC bulk OUT pipe\n", n);
  }

  long           elf_len = 0;
  uint8_t* const elf     = read_file(elf_path, &elf_len);
  if (elf == nullptr) {
    (void)fprintf(stderr, "cannot read %s\n", elf_path);
    return 1;
  }
  (void)fprintf(stderr, "board_sim: loading %s (%ld bytes)\n", elf_path, elf_len);
  (void)fprintf(stderr,
                "  device        : %s\n",
                (board_periph_device() == k_board_device_ra8p1)
                  ? "RA8P1 (R7KA8P1KFLCAC) -- +Ethos-U55 NPU"
                  : "RA8D2 (R7KA8D2KFLCAC)");
  (void)fprintf(stderr,
                "  primary core  : %s%s\n",
                (sim_primary_core() == k_core_m33) ? "Cortex-M33 (Armv8-M)"
                                                   : "Cortex-M85 (Armv8.1-M, MVE seams armed)",
                sim_low_power() ? "  [low-power: 1/4 chunk budget]" : "");
  if (load_elf(uc, elf, elf_len) != 0) {
    free(elf);
    return 1;
  }

  /* Two-image TrustZone app (--ns): load the Non-Secure image at its LMA
   * (0x02080000 in MRAM). The Secure boot copies LMA -> the NS_SRAM2 run alias
   * (0x32100000) and BLXNS-es to it, so the NS world (ThreadX + the e-reader)
   * executes real code instead of an empty alias. The host-side buffer stays
   * alive through the --dump-sym / --stop-sym resolution below: those probe
   * counters live in the NS image's OWN symbol table (the Secure ELF has no
   * NS-image symbols), so both tables are consulted. */
  long           ns_len = 0;
  uint8_t* const ns_elf = (ns_elf_path != nullptr) ? read_file(ns_elf_path, &ns_len) : nullptr;
  if (ns_elf_path != nullptr) {
    if (ns_elf == nullptr) {
      (void)fprintf(stderr, "cannot read --ns %s\n", ns_elf_path);
      free(elf);
      return 1;
    }
    (void)fprintf(stderr, "board_sim: loading NS image %s (%ld bytes)\n", ns_elf_path, ns_len);
    if (load_elf(uc, ns_elf, ns_len) != 0) {
      free(ns_elf);
      free(elf);
      return 1;
    }
    /* Track the NS image's actual vector base (lowest executable PT_LOAD VMA) so
     * the BLXNS world switch reads MSP/reset from the right place: 0x32100000
     * for a RAM-resident NS image, 0x90000000 for one that runs XIP from OSPI.
     * Keep the default if the header cannot be parsed. */
    const uint32_t ns_vbase = elf_vector_base(ns_elf, ns_len);
    if (ns_vbase != 0U) {
      sim_tz_set_ns_vector_base(ns_vbase);
    }
    (void)fprintf(stderr, "board_sim: NS vector base @ 0x%08X\n", sim_tz_ns_vector_base());
  }
  /* NB: keep the host-side `elf` buffer alive until after the seam installers --
   * load_elf has copied the image into Unicorn memory, but usbh_seam_install /
   * sym_trace_install still scan the ELF symbol table from this buffer (freed
   * right after, below). Ethernet no longer needs it: board_periph_eth models
   * the R-Switch registers, so the genuine ra8_eth driver runs (no symbol seam). */

  /* Resolve any --dump-sym globals to addresses now, while the ELF symbol
   * tables are still resident; the values are read back from Unicorn memory
   * after the run (the software analog of the JLink memprobe HIL mode). A
   * two-image app's probe counters live in the NS image, so a miss in the
   * primary table falls through to the --ns symbol table. */
  for (uint32_t d = 0U; d < dump_sym_n; d++) {
    dump_sym_addrs[d] = elf_sym_addr(elf, elf_len, dump_sym_names[d], nullptr);
    if ((dump_sym_addrs[d] == 0U) && (ns_elf != nullptr)) {
      dump_sym_addrs[d] = elf_sym_addr(ns_elf, ns_len, dump_sym_names[d], nullptr);
    }
    if (dump_sym_addrs[d] == 0U) {
      (void)fprintf(stderr,
                    "board_sim: --dump-sym %s not found in symbol table\n",
                    dump_sym_names[d]);
    }
  }

  /* Resolve the --stop-sym watch global the same way (the address is valid for
   * the whole run; read at each chunk boundary in the run loop below). */
  if (stop_sym_name != nullptr) {
    stop_sym_addr = elf_sym_addr(elf, elf_len, stop_sym_name, nullptr);
    if ((stop_sym_addr == 0U) && (ns_elf != nullptr)) {
      stop_sym_addr = elf_sym_addr(ns_elf, ns_len, stop_sym_name, nullptr);
    }
    if (stop_sym_addr == 0U) {
      (void)fprintf(stderr, "board_sim: --stop-sym %s not found in symbol table\n", stop_sym_name);
    }
  }
  /* NS-image symbol table no longer needed (its bytes are in Unicorn memory). */
  free(ns_elf);

  /* TrustZone NSC pointer validation: patch cmse_check_address_range to
   * `BX LR` in board_sim's single flat domain (see sim_tz_patch_cmse). */
  sim_tz_patch_cmse(uc, elf, elf_len);

  /* Cortex-M reset: SP = vectors[0], PC = vectors[1] (Thumb, clear bit0). */
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)uc_mem_read(uc, sim_memmap_mram_base() + 0U, &sp, 4); /* MRAM[0]                 */
  (void)uc_mem_read(uc, sim_memmap_mram_base() + 4U, &pc, 4); /* MRAM[4] (Thumb: bit0=1) */
  /* M-profile is always Thumb (EPSR.T must be 1). Keep the reset vector's bit0
   * and set the xPSR Thumb bit so Unicorn enters Thumb, not ARM, decoding. */
  pc |= 1U;
  uint32_t xpsr = (uint32_t)k_xpsr_t_bit; /* xPSR.T */
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_reg_write(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)fprintf(stderr,
                "board_sim: reset SP=0x%08X PC=0x%08X -- running (<= %u x %u insns, %u s wall)\n",
                sp,
                pc,
                (unsigned)k_run_max_chunks,
                (unsigned)k_run_chunk_insns,
                (unsigned)k_run_wall_s);

  /* MRAM holds the vector table; the run loop passes this as the VTOR fallback
   * to exc_take_pending (the live VTOR, set by SystemInit, is read inside). */
  const uint32_t vtor_base = (uint32_t)sim_memmap_mram_base();

  sim_insn_seams_install(uc);
  sim_exc_install_core(uc);
  /* Seed the ITM "ready" bits in PPB RAM and echo stimulus-port writes, so
   * ra8_log output (e.g. the TrustZone e-reader's ra8_nsc_log_emit lines) prints
   * as `[itm] ...`. STIM0 is in PPB RAM, so the write-hook is the only way to
   * observe the log bytes (see sim_console_install in sim_console.c). */
  sim_console_install(uc);
  /* TrustZone S->NS boot seams: SAU_TYPE seed + the hand-emulated BLXNS world
   * switch, armed only when the firmware links the secure boot (see sim_tz). */
  sim_tz_install(uc, elf, elf_len);
  sim_exc_install_scb_nvic(uc);
  sim_mpu_install(uc);
  /* Ethernet: the ra8_eth / ra8_etha / ra8_rmac / ra8_eth_gwca register path runs
   * for real against the board_periph_eth R-Switch model -- no frame-API seam.
   * The virtual peer (board_net) is fed by that model's descriptor DMA. */
  /* Virtual USB host-mode device (HID boot keyboard): inert unless the firmware
   * links the ra8_usb_host_* primitives, so device-mode apps are unaffected.
   * Skipped under --usbhs-loop: there the real ra8_usb_host_* functions must run
   * so they drive the modelled USBHS host controller (board_periph_usbhs_host.c),
   * which bridges to the on-chip USBFS device -- the chip-internal self-loop. */
  bool usbh_seamed = false;
  if (!usbhs_loop) {
    usbh_seamed = usbh_seam_install(uc, elf, elf_len);
  }
  /* Register-level USBHS host model (board_usb_host.c): only an UNSEAMED,
   * non-loop firmware may engage it. A C-level seam family already shadows a
   * seamed app's host API with a virtual device, and under --usbhs-loop the
   * loop-only registered block (board_periph_usbhs_host.c) owns the USBHS
   * window, so the two register models must not both answer it. The TrustZone
   * two-image NS host (tz_nsc_cgc_usb) is unseamed by construction: the
   * installer scans only the primary (Secure) ELF, which links no host
   * symbols. */
  board_usb_host_set_allowed(!usbh_seamed && !usbhs_loop);
  /* Arm any --trace-sym entry hooks (debugging instrument: watch a bring-up
   * sequence reach -- or stall before -- a given function). Done while the
   * host-side `elf` buffer is still alive for symbol resolution. */
  sym_trace_install(uc, elf, elf_len, trace_sym_names, trace_sym_n);
  /* The long-shift (LSLL/LSRL/ASRL) and MVE (Helium VSTRW.32) seams emulate
   * Armv8.1-M instructions that only the Cortex-M85 emits but Unicorn's M33
   * core mis-executes. With --primary-core m33 the firmware is pure Armv8-M, so
   * leave the M85-only seams off (they would be inert anyway -- no sites -- but
   * gating keeps the M33 model honest and the telemetry accurate). The
   * cond-select (CSEL) emulation rides the invalid-instruction hook and stays
   * armed; it is likewise inert for an M33 image. */
  if (sim_primary_core() == k_core_m85) {
    long_shift_seam_install(uc, elf, elf_len);
    mve_seam_install(uc, elf, elf_len);
  }
  /* Divide-by-zero UsageFault (CCR.DIV_0_TRP): track every UDIV/SDIV site for
   * every core -- the sites are overwritten with UDF by on_scb_ctrl_write only once
   * the firmware sets DIV_0_TRP, so a normal app pays nothing (see div0_seam_install
   * / div0_patch_sites / emulate_div0_patched). */
  div0_seam_install(elf, elf_len);
  /* --fast-sd (opt-in): serve whole SD blocks from the image in one C hook entry
   * instead of clocking 512 SPI bytes each, so a book-sized read loads fast.
   * Inert without the flag or without an SD-capable firmware (see fast_sd). */
  fast_sd_seam_install(uc, elf, elf_len);
  /* BOARD_SIM_PROFILE: collect FUNC symbols for the profiler; in per-instruction
   * mode (=full) arm a code hook that tallies every instruction + call. */
  prof_load(elf, elf_len);
  sim_prof_install(uc);
  /* Spin up the cpu1 engine for dual-core firmware (shares SRAM with cpu0).
   * NULL for single-core apps -- cpu0 then runs exactly as before. The elf
   * buffer is kept alive for the whole run (freed after the run loop) because a
   * warm reboot re-loads its PT_LOAD segments from it; cpu1_engine_init also
   * reads cpu1's image from it. */
  sim_cpu1_init(elf, elf_len);

  /* The board view is the panel framebuffer (panel_w x panel_h, left) plus a
   * status sidebar (LEDs / USB / UART / IRQ / touch); the composite buffer is
   * what both the live window and the --ppm snapshot show. panel_fb holds the
   * GLCDC render before it is composited into the sidebar-widened composite. */
  const uint16_t panel_w = view_w;
  const uint16_t panel_h = view_h;
  /* --rotate turns the displayed panel for viewing (90/270 swap W and H, so a
   * landscape app can be shown portrait); the firmware still renders panel_w x
   * panel_h, which build_composite rotates into rot_fb. */
  const bool rot_swap =
    (rotate_deg == (uint32_t)k_rotate_90) || (rotate_deg == (uint32_t)k_rotate_270);
  const uint16_t disp_w    = rot_swap ? panel_h : panel_w;
  const uint16_t disp_h    = rot_swap ? panel_w : panel_h;
  const uint16_t comp_w    = board_overlay_total_width(disp_w);
  const uint16_t comp_h    = board_overlay_total_height(disp_h);
  board_view_t*  view      = nullptr;
  uint16_t*      panel_fb  = nullptr;
  uint16_t*      rot_fb    = nullptr;
  uint16_t*      composite = nullptr;
  if (want_view) {
    view = board_view_open(comp_w, comp_h, win_title);
    if (view == nullptr) {
      (void)fprintf(stderr, "board_sim: could not open window; continuing headless\n");
    }
  }
  if ((view != nullptr) || (ppm_path != nullptr) || want_click || (record_dir != nullptr)) {
    panel_fb  = (uint16_t*)malloc((size_t)panel_w * (size_t)panel_h * sizeof(uint16_t));
    composite = (uint16_t*)malloc((size_t)comp_w * (size_t)comp_h * sizeof(uint16_t));
    if (rotate_deg != (uint32_t)k_rotate_0) {
      rot_fb = (uint16_t*)malloc((size_t)disp_w * (size_t)disp_h * sizeof(uint16_t));
    }
  }

  if (want_click) {
    (void)fprintf(stderr, "board_sim: --click armed at (%d,%d)\n", click_x, click_y);
  }
  enum : uint32_t {
    /* ra8_delay_ms(16) in the render loop spins on SysTick, which advances one
     * tick per chunk, so ~16 chunks == one loop iteration. Give the injected
     * tap many iterations to flow DOWN -> UP -> CLICKED -> tab switch -> repaint. */
    k_click_settle_chunks = 512U, /**< Extra chunks after the click lands. */
  };
  /* BOARD_SIM_CLICK_SETTLE=N: widen the post-click drain for a tap that kicks off
   * a long operation (e.g. opening a big book from SD: read + inflate + decode +
   * render can need far more than the default window). Mirrors BOARD_SIM_MAX_CHUNKS;
   * unset keeps the default so ordinary tap captures stay snappy. */
  uint32_t click_settle_chunks = (uint32_t)k_click_settle_chunks;
  {
    const char* e_settle = getenv("BOARD_SIM_CLICK_SETTLE");
    if (e_settle != nullptr) {
      const long v = strtol(e_settle, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        click_settle_chunks = (uint32_t)v;
      }
    }
  }

  /* Chunked run: emulate a block, take a SysTick (and any pending PendSV),
   * repeat. Within a chunk, exception returns (a handler's "BX lr" into an
   * EXC_RETURN magic) and SVCalls are resolved inline -- the inner loop unstacks
   * / vectors and relaunches from the new PC without consuming a chunk -- so a
   * context switch does not cost a whole scheduling quantum and the once-per-
   * chunk SysTick cadence (one ThreadX tick) is preserved. Headless runs stop on
   * a chunk budget + wall-clock guard; in --view the loop runs until the window
   * is closed, presenting the live GLCDC output every k_view_present_every. */
  uint32_t max_chunks =
    (view != nullptr) ? (uint32_t)k_view_max_chunks : (uint32_t)k_run_max_chunks;
  /* Headless runs of heavy apps (e.g. sd_font_render: a 400 KB SD font read
   * plus stb_truetype rasterisation) can need more than the default budget.
   * BOARD_SIM_WALL_S / BOARD_SIM_MAX_CHUNKS override the guards without a
   * recompile; they have no effect in --view (window-driven) mode.
   *
   * The wall-clock guard is CPU-time (clock(), see the run loop), so a
   * heavily-loaded host burns it faster than wall time -- which made it
   * truncate the otherwise instruction-deterministic chunk budget under CI load
   * and silently drop a late-printing banner (#168). A run that must stay
   * deterministic regardless of host load sets BOARD_SIM_WALL_S=0 to DISABLE the
   * guard entirely and rely on the deterministic BOARD_SIM_MAX_CHUNKS bound (and
   * BOARD_SIM_STOP_ON) instead. (Previously WALL_S=0 silently fell back to the
   * default -- the footgun that hid the #168 root cause.) */
  double wall_s        = (double)k_run_wall_s;
  bool   wall_guard_on = true;
  {
    const char* e_wall = getenv("BOARD_SIM_WALL_S");
    if (e_wall != nullptr) {
      const long v = strtol(e_wall, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        wall_s = (double)v;
      } else if (v == 0L) {
        wall_guard_on = false; /* explicit opt-out: bound the run by chunks only */
      }
    }
    const char* e_chunks = getenv("BOARD_SIM_MAX_CHUNKS");
    if ((e_chunks != nullptr) && (view == nullptr)) {
      const long v = strtol(e_chunks, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        max_chunks = (uint32_t)v;
      }
    }
  }
  /* Headless --record-secs bounds the run to exactly the recording window so the
   * dumped frame sequence spans the requested emulated duration. */
  if ((record_dir != nullptr) && (record_secs > 0U) && (view == nullptr)) {
    max_chunks = record_secs * (uint32_t)k_record_ms_per_sec;
  }
  if (record_dir != nullptr) {
    (void)mkdir(record_dir, (mode_t)k_record_dir_mode);
    (void)fprintf(stderr,
                  "board_sim: recording to %s/frame_NNNNNN.ppm (every %u chunks, ~%u fps)\n",
                  record_dir,
                  (unsigned)k_record_every,
                  (unsigned)k_record_fps);
  }
  /* BOARD_SIM_IDLE_STOP=N: in a plain headless run, stop early once observable
   * state has not changed for N consecutive chunks -- the firmware has reached
   * steady-state idle (an RTOS idle spin, a `while(1) wfi`), so there is nothing
   * left to run. Off (0) by default, so normal runs are unaffected; opt in for
   * RTOS/idle apps that would otherwise burn the whole wall-clock budget. The
   * tracked counters (peripheral MMIO reads/writes, PendSV/SVCall, peripheral
   * IRQs) are all monotonic, so an unchanged sum means none of them advanced. */
  uint32_t idle_stop_chunks = 0U;
  {
    const char* e_idle = getenv("BOARD_SIM_IDLE_STOP");
    if ((e_idle != nullptr) && (view == nullptr)) {
      const long v = strtol(e_idle, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        idle_stop_chunks = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_USB_STOP=N: stop a plain headless run N chunks after the virtual USB
   * host reports the device CONFIGURED. The USB device apps never go idle -- HID
   * jiggles its boot mouse forever, MSC keeps answering host polls -- so the
   * idle-stop above never fires for them. This makes the smoke gate fast and
   * deterministic: it runs exactly long enough to confirm enumeration + the first
   * class traffic, then stops, instead of burning the whole chunk/wall budget. */
  uint32_t usb_stop_settle = 0U;
  {
    const char* e_usb = getenv("BOARD_SIM_USB_STOP");
    if ((e_usb != nullptr) && (view == nullptr)) {
      const long v = strtol(e_usb, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        usb_stop_settle = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_USBH_STOP=N: as above but for a USB HOST-mode app -- stop N chunks
   * after the virtual host-mode keyboard has streamed its reports. Kept distinct
   * from USB_STOP so a host app that also runs a device worker is not stopped by
   * that worker reaching CONFIGURED before the host side completes. */
  uint32_t usbh_stop_settle = 0U;
  {
    const char* e_usbh = getenv("BOARD_SIM_USBH_STOP");
    if ((e_usbh != nullptr) && (view == nullptr)) {
      const long v = strtol(e_usbh, nullptr, (int)k_env_strtol_base);
      if (v > 0L) {
        usbh_stop_settle = (uint32_t)v;
      }
    }
  }
  /* BOARD_SIM_STOP_ON="<substr>": stop the headless run as soon as the console
   * UART's last line contains <substr> -- a generic "stop on a banner" guard for
   * apps that loop forever after a success line (e.g. usb_host_file_ops retries
   * its ladder every 5 s). Empty / unset disables it. */
  const char* stop_on = getenv("BOARD_SIM_STOP_ON");
  if ((stop_on != nullptr) && ((stop_on[0] == '\0') || (view != nullptr))) {
    stop_on = nullptr;
  }
  /* BOARD_SIM_STOP_PC=0x...: end the run the first time PC reaches this address
   * (effective in profile insn mode, via prof_insn_hook). Lets the profiler cover
   * exactly the boot path -- the idle main-loop PC the firmware parks at -- when
   * the build-stable compute-idle auto-stop below is not specific enough. */
  {
    const char* e_spc = getenv("BOARD_SIM_STOP_PC");
    if (e_spc != nullptr) {
      const unsigned long v = strtoul(e_spc, nullptr, (int)k_env_strtol_base);
      sim_prof_set_stop_pc((uint32_t)(v & ~1UL)); /* clear the Thumb bit if supplied. */
    }
  }
  /* BOARD_SIM_PROFILE compute-idle stop (insn mode, build-stable): once boot has
   * fallen into the steady frame loop the firmware retires almost no instructions
   * per chunk (each frame polls a little, then WFI-halts), whereas boot chunks are
   * compute-heavy. Stop after k_prof_idle_need consecutive chunks that each retire
   * fewer than k_prof_idle_insns instructions, so the profile spans boot + the
   * first rendered frame and not the idle tail. Armed only after k_prof_idle_arm
   * chunks so an early cheap chunk cannot trip it. */
  enum : uint32_t {
    k_prof_idle_insns = 4000U, /**< Per-chunk insns below which a chunk is idle. */
    k_prof_idle_need  = 600U,  /**< Consecutive idle chunks that end the run.    */
    k_prof_idle_arm   = 16U,   /**< Chunks to run before the stop is armed.      */
  };
  /* All three are overridable so the boot window can be tuned per app (a long
   * boot-time settle delay is a run of cheap chunks that must not be mistaken
   * for the steady idle loop). NEED defaults high enough to clear the settle
   * delays in these apps; raise it (or set BOARD_SIM_STOP_PC) for a longer boot. */
  uint32_t prof_idle_insns = (uint32_t)k_prof_idle_insns;
  uint32_t prof_idle_need  = (uint32_t)k_prof_idle_need;
  uint32_t prof_idle_arm   = (uint32_t)k_prof_idle_arm;
  {
    const char* e_pi = getenv("BOARD_SIM_PROFILE_IDLE_INSNS");
    const char* e_pn = getenv("BOARD_SIM_PROFILE_IDLE_NEED");
    const char* e_pa = getenv("BOARD_SIM_PROFILE_IDLE_ARM");
    if (e_pi != nullptr) {
      prof_idle_insns = (uint32_t)strtoul(e_pi, nullptr, (int)k_strtol_base10);
    }
    if (e_pn != nullptr) {
      prof_idle_need = (uint32_t)strtoul(e_pn, nullptr, (int)k_strtol_base10);
    }
    if (e_pa != nullptr) {
      prof_idle_arm = (uint32_t)strtoul(e_pa, nullptr, (int)k_strtol_base10);
    }
  }
  uint64_t            prof_idle_prev_i = 0U;
  uint32_t            prof_idle_run    = 0U;
  bool                prof_stopped     = false;
  uint64_t            idle_sig_prev    = 0U;
  uint32_t            idle_run         = 0U;
  bool                idle_stopped     = false;
  uint32_t            usb_stop_run     = 0U;
  uint32_t            usbh_stop_run    = 0U;
  bool                usb_stopped      = false;
  bool                stop_sym_hit     = false; /* --stop-sym threshold reached.           */
  uint32_t            rec_frames       = 0U;    /* frames written when --record is active. */
  const clock_t       t0               = clock();
  uc_err              err              = UC_ERR_OK;
  uint32_t            run_pc           = pc;
  uint32_t            chunks           = 0U;
  bool                timed_out        = false;
  bool                closed           = false;
  uint32_t            settle_left      = 0U;    /* >0 once the click landed: chunks to drain. */
  uint32_t            last_boot_chunk  = 0U;    /* chunk of the last (re)boot for --reboot.   */
  bool                slider_grab      = false; /* true while a press grabbed the battery slider. */
  board_overlay_btn_t held_btn = k_board_overlay_btn_none; /* SW held down (released on up). */
  uint64_t            last_present_us = 0U; /* wall-us of the last live --view present. */
  /* Classify a headless --click once. A click on the console tab bar switches the
   * active console channel (a one-shot view change, same as the window path); an
   * on-screen sidebar button toggles a user switch (fired once); anything else is
   * a panel touch (re-armed until drained). */
  bool     click_was_tab = false;
  uint32_t click_tab_idx = 0U;
  if (want_click) {
    if (board_overlay_hit_console_tab((uint16_t)click_x,
                                      (uint16_t)click_y,
                                      disp_w,
                                      (uint32_t)k_board_console_ch_count,
                                      &click_tab_idx)) {
      sim_view_select_console_tab(click_tab_idx);
      click_was_tab = true;
    }
  }
  const board_overlay_btn_t click_btn =
    (want_click && !click_was_tab)
      ? board_overlay_hit_button((uint16_t)click_x, (uint16_t)click_y, disp_w)
      : k_board_overlay_btn_none;
  bool     button_fired = false;
  uint32_t prof_prev_pc = 0U;
  double   prof_prev_t  = 0.0;
  for (; chunks < max_chunks; chunks++) {
    /* BOARD_SIM_PROFILE: charge the wall time of the previous chunk to the
     * function its execution started in (so a cheap WFI-halt chunk and an
     * expensive compute chunk are weighted by real time, not by chunk count). */
    if (sim_prof_mode() == k_prof_wall) {
      const double now = board_now_s();
      if (prof_prev_t > 0.0) {
        prof_add(prof_prev_pc, now - prof_prev_t);
      }
      prof_prev_pc = run_pc;
      prof_prev_t  = now;
    }
    /* Publish run telemetry for the board view (PC + chunk counter). */
    sim_view_publish(run_pc, chunks);

    /* --reboot N: after each boot runs a settle window (enough to exercise its
     * first-boot path -- e.g. plant a backup-RAM sentinel), force a power-on warm
     * reboot. The reset-retained models (VBATT backup) survive, so an app proves
     * reset-survival across the reboot. This is a clean power-on (PORF stays). */
    if ((reboot_count > 0) && ((chunks - last_boot_chunk) >= (uint32_t)k_reboot_settle)) {
      reboot_count--;
      board_periph_reset_set_cause(true, false, false, false); /* power-on reboot */
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      last_boot_chunk = chunks;
      continue;
    }
    /* A peripheral (the watchdog) may have requested a reset on its tick: latch
     * the watchdog cause and warm-reboot so the next boot reads reset_by=wdt. */
    bool wdt_rst  = false;
    bool iwdt_rst = false;
    if (board_periph_reset_take_request(&wdt_rst, &iwdt_rst)) {
      board_periph_reset_set_cause(false, false, wdt_rst, iwdt_rst);
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      last_boot_chunk = chunks;
      continue;
    }

    /* Each outer chunk is one SysTick period: arm the periodic tick so the
     * boundary (or a tail-chain) takes it once interrupts permit. */
    sim_exc_arm_systick();

    /* Advance the free-running DWT cycle counter one chunk's worth of cycles so
     * a masked-context ra8_delay_ms (which spins on CYCCNT while PRIMASK is set)
     * makes progress instead of stalling the whole run. Inert unless the
     * firmware enabled the counter, so this cannot change any other app. */
    dwt_cyccnt_advance(uc);

    /* Advance the modelled timers one tick-period and let the ICU pend any IRQ
     * a counter wrap raised; the exception layer below takes it like SysTick. */
    board_periph_tick(uc);
    board_net_tick();

    /* Drain any buffered keystrokes into the console UART RX (the same SCI
     * channel --input targets). Sources are the live --view window (keyDown)
     * and the headless --keys injector, both via board_input -- so the keyboard
     * path is identical whether typed in the window or scripted on the CLI. */
    char key_byte = 0;
    while (board_input_pop_key(&key_byte)) {
      const uint8_t kb = (uint8_t)key_byte;
      board_periph_sci_feed_rx(board_periph_sci_console_channel(), &kb, 1U);
    }

    /* Headless --click: keep one contact armed in the GT911 model until the
     * firmware's real ra8_touch_read drains it (board_periph_touch_reported
     * increments). Re-arming each chunk is needed because ra8_touch_open clears
     * the GT911 status byte during bring-up; once the render loop reads the
     * point it is consumed once and never re-armed. */
    if (want_click && (click_btn != k_board_overlay_btn_none)) {
      if (!button_fired) {
        if ((click_btn == k_board_overlay_btn_battery) ||
            (click_btn == k_board_overlay_btn_batt_chg) ||
            (click_btn == k_board_overlay_btn_lowpower)) {
          apply_battery_click(click_btn, (uint16_t)click_x, disp_w); /* SOC / CHG / low-power. */
        } else {
          set_switch(click_btn, true); /* headless --click SW1/SW2: press + hold. */
        }
        button_fired = true;
      }
    } else if (want_click && !click_was_tab && (board_periph_touch_reported() == 0U)) {
      uint16_t cnx = (uint16_t)click_x;
      uint16_t cny = (uint16_t)click_y;
      unrotate_click((uint16_t)click_x,
                     (uint16_t)click_y,
                     panel_w,
                     panel_h,
                     rotate_deg,
                     &cnx,
                     &cny);
      board_periph_touch_inject(cnx, cny);
    }

    /* Inner loop: run a chunk, then service exceptions to a steady state before
     * the next chunk. An EXC_RETURN branch is unstacked and the NVIC re-checked
     * so a still-pending lower-priority exception tail-chains (e.g. PendSV right
     * after SysTick) exactly as hardware would, instead of briefly resuming the
     * interrupted code. SVCall (taken in on_intr) just leaves PC at the handler,
     * which the next relaunch runs. */
    bool faulted = false;
    for (uint32_t inner = 0U; inner < (uint32_t)k_run_inner_max; inner++) {
      sim_exc_clear_pendsv_stop(); /* set by on_icsr_write iff this run ends on PENDSVSET */
      /* Idle fast-forward: when the core is parked on a wait-for-interrupt spin
       * (ThreadX's __tx_wait_here `b .`, or a `wfi` halt), running a full
       * k_run_chunk_insns just burns wall time to reach the SAME SysTick that is
       * already armed once per outer chunk. Cap the budget to k_idle_spin_insns
       * so the spin returns at once and the boundary below takes the tick now.
       * This skips only idle wall-time: the tick stays armed once per outer
       * chunk and fired once, so ThreadX's tick COUNT -- and every sleep/heartbeat
       * deadline measured in ticks -- is identical to a full idle chunk. Busy
       * firmware never parks on these opcodes, so it runs the full budget. */
      /* Low-power (Model A): the M33 runs ~4x slower than the M85, so a busy
       * chunk advances 1/4 as many instructions per modelled tick. Idle spins
       * still collapse to k_idle_spin_insns regardless. */
      size_t busy_budget = (size_t)k_run_chunk_insns;
      if (sim_low_power()) {
        busy_budget = (size_t)k_run_chunk_insns / (size_t)k_low_power_div;
      }
      const size_t run_budget = idle_spin_at(uc, run_pc) ? (size_t)k_idle_spin_insns : busy_budget;
      err                     = uc_emu_start(uc, (uint64_t)run_pc | 1U, 0, 0, run_budget);
      (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
      if (sim_seam_take_relaunch()) {
        /* A --fast-sd byte-exchange returned to its caller. This consumed no
         * modelled time, so relaunch from the returned PC without advancing the
         * SysTick or charging a chunk (the inner-loop cap still bounds the run). */
        continue;
      }
      if (sim_prof_stop_hit()) {
        break; /* BOARD_SIM_STOP_PC reached (set in prof_insn_hook) -- end the run. */
      }
      if (sim_exc_reboot_requested()) {
        break; /* AIRCR.SYSRESETREQ -- the outer wrapper performs the reboot. */
      }
      if (sim_exc_bkpt_hit()) {
        faulted = true; /* firmware trapped on a BKPT -- end the run, report it. */
        break;
      }
      uint64_t exc_ret_pc = 0U;
      if (sim_exc_take_exc_return(&exc_ret_pc)) {
        exc_return(uc, (uint32_t)exc_ret_pc);
        /* Tail-chain the next pend, but do NOT advance the SysTick: an exception
         * return consumes no modelled time, so time advances only on a full
         * instruction budget (below). This keeps a deferred tick from firing the
         * instant a PendSV context switch unstacks into the new thread. */
        (void)exc_take_pending(uc, vtor_base, false);
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      if (err != UC_ERR_OK) {
        faulted = true;
        break;
      }
      if (sim_mpu_fault_pending()) {
        /* A store hit a read-only MPU region (on_mpu_ro_write stopped us).
         * Synthesise MemManage at this boundary -- no time advances (a fault is
         * synchronous), and the stacked PC is the faulting store. */
        sim_mpu_clear_fault();
        mpu_synth_memmanage(uc, vtor_base);
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      if (sim_div0_fault_pending()) {
        /* A UDIV/SDIV by zero with CCR.DIV_0_TRP set (emulate_div0_patched stopped
         * us). Synthesise UsageFault at this boundary -- synchronous, no time
         * advances, and the stacked PC is the trapping divide. */
        sim_div0_clear_fault();
        div0_synth_usagefault(uc, vtor_base);
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      if (sim_exc_pendsv_stop()) {
        /* Context-switch stop: a thread wrote PENDSVSET to yield. Take PendSV
         * but do NOT advance the SysTick -- a context switch consumes no
         * modelled time, so a thread that just suspended on a tick wait keeps
         * waiting and the scheduler runs the highest-priority READY thread. This
         * is what lets a low-priority worker (e.g. the NetX echo thread) run to
         * completion before a higher-priority sleeper's tick expires. */
        (void)exc_take_pending(uc, vtor_base, false);
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      /* Full instruction budget elapsed: a tick's worth of genuine execution (or
       * an idle spin) has passed, so advance time -- take the highest-priority
       * pending exception (SysTick this period, and/or PendSV) and resolve it in
       * this same chunk so a context switch does not cost a scheduling quantum. */
      if (exc_take_pending(uc, vtor_base, true)) {
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      break;
    }
    if (faulted) {
      break;
    }
    if (sim_prof_stop_hit()) {
      prof_stopped = true; /* BOARD_SIM_STOP_PC reached -- end the profiled run. */
      break;
    }
    /* AIRCR.SYSRESETREQ requested a reset: latch a software-reset cause and warm
     * reboot the firmware from its reset vector, then keep running. */
    if (sim_exc_reboot_requested()) {
      board_periph_reset_set_cause(false, true, false, false); /* software reset */
      run_pc = warm_reboot(uc, elf, elf_len, want_trace);
      /* Cleared after the reboot (the engine is idle during warm_reboot, so no
       * AIRCR write can be lost here, but clearing last keeps it robust if
       * warm_reboot ever runs code). */
      sim_exc_clear_reboot_request();
      if (run_pc == 0U) {
        break; /* reboot failed to reload the image -- end the run */
      }
      continue;
    }

    /* Dual-core: boot cpu1 on the CPU1ACTCSR release (SP/PC from its vector
     * table at CPU1INITVTOR), then step it interleaved with cpu0. cpu1 shares
     * the on-chip SRAM, so its poll of the ping struct sees cpu0's writes and
     * its pong reply is visible back to cpu0 -- a real second core, not a model.
     * cpu1 is a tight poll loop (no interrupts), so a plain stepped run suffices;
     * a cpu1 fault just stops cpu1 (cpu0 keeps running). */
    sim_cpu1_step();
    /* --record: dump the composite (panel + status) every k_record_every chunks
     * as a numbered PPM, so the run becomes a frame sequence (assemble to a video
     * with ffmpeg, e.g. `ffmpeg -framerate 20 -i frame_%06d.ppm out.mp4`). */
    if ((record_dir != nullptr) && (composite != nullptr) &&
        ((chunks % (uint32_t)k_record_every) == 0U)) {
      build_composite(uc,
                      panel_fb,
                      rot_fb,
                      composite,
                      panel_w,
                      panel_h,
                      disp_w,
                      disp_h,
                      rotate_deg,
                      win_title);
      char fpath[1024];
      (void)snprintf(fpath, sizeof(fpath), "%s/frame_%06u.ppm", record_dir, (unsigned)rec_frames);
      if (write_ppm(fpath, composite, comp_w, comp_h) == 0) {
        rec_frames++;
      }
    }
    if (view != nullptr) {
      /* Live window: mouse-down on an on-screen SW1/SW2 PRESSES that user switch
       * (route_click drives it active-low); the matching mouse-up releases it, so
       * the on-screen buttons act as momentary push-buttons, not latching switches.
       * A press anywhere on the panel arms one GT911 contact instead. */
      uint16_t cx = 0U;
      uint16_t cy = 0U;
      if (board_view_poll_click(view, &cx, &cy)) {
        /* A press on the battery slider "grabs" it, so a subsequent drag keeps
         * setting the SOC from the cursor column even if the mouse leaves the
         * track row -- standard slider grab semantics. */
        const board_overlay_btn_t hit = route_click(cx, cy, panel_w, panel_h, disp_w, rotate_deg);
        slider_grab                   = (hit == k_board_overlay_btn_battery);
        held_btn = ((hit == k_board_overlay_btn_sw1) || (hit == k_board_overlay_btn_sw2))
                     ? hit
                     : k_board_overlay_btn_none;
      }
      /* Mouse-up: release a held push-button and drop any slider grab. */
      if (board_view_poll_release(view)) {
        if (held_btn != k_board_overlay_btn_none) {
          set_switch(held_btn, false);
          held_btn = k_board_overlay_btn_none;
        }
        slider_grab = false;
      }
      uint16_t dx = 0U;
      uint16_t dy = 0U;
      if (board_view_poll_drag(view, &dx, &dy) && slider_grab) {
        apply_battery_click(k_board_overlay_btn_battery, dx, disp_w);
      }
      /* Mouse-wheel pages the console scrollback, Arduino-Serial-Monitor style:
       * scrolling up reveals older lines AND pauses autoscroll (so new output no
       * longer yanks the view to the bottom); scrolling back down to the tail
       * re-enables autoscroll. fill_status clamps the offset + holds the absolute
       * position while paused. */
      sim_view_wheel(board_view_poll_scroll(view));
      if ((chunks % (uint32_t)k_view_present_every) == 0U) {
        /* Compositing + uploading the full panel+sidebar frame is the dominant
         * host cost. The firmware advances far faster than a display needs, so
         * cap the live present to ~60 Hz wall-clock and yield the CPU when a
         * present is skipped -- otherwise an idle app (cheap chunks) would spin a
         * host core redrawing identical frames thousands of times a second, which
         * reads as window lag for "nothing happening". */
        struct timespec ts_now = {};
        (void)clock_gettime(CLOCK_MONOTONIC, &ts_now);
        const uint64_t now_us = ((uint64_t)ts_now.tv_sec * (uint64_t)k_us_per_s) +
                                ((uint64_t)ts_now.tv_nsec / (uint64_t)k_ns_per_us);
        if ((now_us - last_present_us) >= (uint64_t)k_view_frame_us) {
          build_composite(uc,
                          panel_fb,
                          rot_fb,
                          composite,
                          panel_w,
                          panel_h,
                          disp_w,
                          disp_h,
                          rotate_deg,
                          win_title);
          board_view_present(view, composite, comp_w, comp_h);
          last_present_us = now_us;
        } else {
          (void)usleep((useconds_t)k_view_yield_us);
        }
      }
      if (board_view_pump(view)) {
        closed = true;
        break;
      }
    } else if (want_click) {
      /* Headless --click: run a bounded tail after the input lands (a drained
       * touch, or a fired on-screen button), then stop deterministically so the
       * dumped frame shows the switched tab / lit LED. */
      const bool click_acted = (click_btn != k_board_overlay_btn_none)
                                 ? button_fired
                                 : (board_periph_touch_reported() > 0U);
      if (click_acted) {
        settle_left = (settle_left == 0U) ? click_settle_chunks : (settle_left - 1U);
        if (settle_left == 1U) {
          break;
        }
      }
      if (wall_guard_on && (((double)(clock() - t0) / (double)CLOCKS_PER_SEC) >= wall_s)) {
        timed_out = true;
        break;
      }
    } else { /* plain headless run (no window, no scripted click) */
      /* Profiler compute-idle early-stop (insn mode): a chunk that retires very
       * few instructions is an idle frame (poll + WFI-halt), whereas boot chunks
       * are compute-heavy. After enough consecutive idle chunks the firmware has
       * reached its steady frame loop -- end the run so the profile is boot, not
       * the idle tail. Build-stable (no per-app address), unlike STOP_PC. */
      if ((sim_prof_mode() == k_prof_insn) && (record_dir == nullptr) &&
          (chunks >= prof_idle_arm)) {
        const uint64_t d = sim_prof_total_insns() - prof_idle_prev_i;
        prof_idle_prev_i = sim_prof_total_insns();
        if (d < (uint64_t)prof_idle_insns) {
          prof_idle_run++;
          if (prof_idle_run >= prof_idle_need) {
            prof_stopped = true;
            break;
          }
        } else {
          prof_idle_run = 0U;
        }
      }
      /* Steady-state idle early-stop (opt-in; not while recording, which must
       * span its full window). All tracked counters are monotonic, so an
       * unchanged sum means no MMIO / IRQ / context switch happened this chunk. */
      if ((idle_stop_chunks > 0U) && (record_dir == nullptr)) {
        const uint64_t idle_sig = (uint64_t)sim_mmio_reads() + (uint64_t)sim_mmio_writes() +
                                  (uint64_t)sim_exc_pendsv_takes() + (uint64_t)sim_exc_svc_takes() +
                                  (uint64_t)board_periph_irq_total();
        if (idle_sig == idle_sig_prev) {
          idle_run++;
          if (idle_run >= idle_stop_chunks) {
            idle_stopped = true;
            break;
          }
        } else {
          idle_sig_prev = idle_sig;
          idle_run      = 0U;
        }
      }
      /* USB device-mode early-stop: once the device reaches CONFIGURED, run a
       * short settle window (for the first class traffic + report line), stop. */
      if ((usb_stop_settle > 0U) && (record_dir == nullptr) && board_usb_configured()) {
        usb_stop_run++;
        if (usb_stop_run >= usb_stop_settle) {
          usb_stopped = true;
          break;
        }
      }
      /* USB host-mode early-stop: once the virtual device has served the host
       * its last request -- the keyboard streamed its reports, or the MSC host
       * reached its read-only WRITE(10) test (the step right before PASS) --
       * settle (for the PASS banner) and stop. Separate from USB_STOP because a
       * host app may ALSO run a device worker whose CONFIGURED would otherwise
       * stop the run before the host side finishes. */
      const bool usbh_done = sim_usbh_done();
      if ((usbh_stop_settle > 0U) && (record_dir == nullptr) && usbh_done) {
        usbh_stop_run++;
        if (usbh_stop_run >= usbh_stop_settle) {
          usb_stopped = true;
          break;
        }
      }
      /* Generic banner stop: the firmware printed its success line to the
       * console. Check ALL THREE text endpoints -- the UART (the SCI-TX `[uart]`
       * banners), the ITM/SWO stimulus stream (the `[itm]` ra8_log lines
       * surfaced by on_itm_stim_write), and the SEGGER RTT up-buffer drain (the
       * `[rtt]` lines board_periph_rtt.c pulls out of the in-RAM control
       * block). ra8_log-only apps -- e.g. the dual-core demos, whose PASS
       * verdict is an `ra8_log_info` line that never touches the UART -- and
       * RTT-only apps (rtt_log_demo) would otherwise run to the full chunk
       * budget; matching the ITM / RTT lines lets BOARD_SIM_STOP_ON end the run
       * the instant the verdict is emitted, exactly as it already does for the
       * UART-banner apps. */
      if (stop_on != nullptr) {
        const char* last_uart = board_periph_uart_last_line();
        if ((last_uart != nullptr) && (strstr(last_uart, stop_on) != nullptr)) {
          usb_stopped = true;
          break;
        }
        const char* last_itm = board_console_line(k_board_console_ch_itm, 0U);
        if ((last_itm != nullptr) && (strstr(last_itm, stop_on) != nullptr)) {
          usb_stopped = true;
          break;
        }
        const char* last_rtt = board_console_line(k_board_console_ch_rtt, 0U);
        if ((last_rtt != nullptr) && (strstr(last_rtt, stop_on) != nullptr)) {
          usb_stopped = true;
          break;
        }
      }
      /* --stop-sym: end the run the instant the watched 32-bit global reaches its
       * threshold (the jlink_memprobe counter floor). Checked at the chunk
       * boundary like STOP_ON, so the stop is deterministic and host-load
       * independent -- a passing probe stops in a handful of chunks. */
      if (stop_sym_addr != 0U) {
        uint32_t sv = 0U;
        if ((uc_mem_read(uc, (uint64_t)stop_sym_addr, &sv, sizeof(sv)) == UC_ERR_OK) &&
            (sv >= stop_sym_thresh)) {
          stop_sym_hit = true;
          break;
        }
      }
      if (wall_guard_on && (((double)(clock() - t0) / (double)CLOCKS_PER_SEC) >= wall_s)) {
        timed_out = true;
        break;
      }
    }
  }

  /* The run has ended: the board view's final / held frame shows "parked". */
  sim_view_mark_stopped(run_pc);

  (void)fprintf(stderr,
                "\nboard_sim: stopped -- %s%s%s%s%s%s\n",
                uc_strerror(err),
                timed_out ? " (wall-clock budget reached)" : "",
                idle_stopped ? " (idle steady-state)" : "",
                usb_stopped ? " (USB enumerated)" : "",
                stop_sym_hit ? " (--stop-sym threshold reached)" : "",
                prof_stopped ? " (profile: boot complete)" : "");
  (void)fprintf(stderr, "  final PC      : 0x%08X\n", run_pc);
  if (sim_exc_bkpt_hit()) {
    (void)fprintf(stderr,
                  "  => firmware executed a BKPT @ 0x%08X (deliberate trap: "
                  "Default_Handler / failed assert / fault give-up)\n",
                  sim_exc_bkpt_pc());
  }
  (void)
    fprintf(stderr, "  chunks run    : %u   SysTick ticks: %u\n", chunks, sim_exc_systick_fires());
  if (sim_mve_emulated_count() > 0U) {
    (void)fprintf(stderr,
                  "  MVE (Helium)  : %llu instruction(s) emulated (M85 vector ops the M33 "
                  "core lacks)\n",
                  (unsigned long long)sim_mve_emulated_count());
  }
  if (sim_lob_emulated_count() > 0U) {
    (void)fprintf(stderr,
                  "  LOB (loop)    : %llu DLS/LE instruction(s) emulated (M85 hardware-loop "
                  "ops)\n",
                  (unsigned long long)sim_lob_emulated_count());
  }
  prof_report();
  (void)fprintf(stderr,
                "  exceptions    : %u PendSV  %u SVCall (real Cortex-M entry/return)\n",
                sim_exc_pendsv_takes(),
                sim_exc_svc_takes());
  (void)fprintf(stderr,
                "  touch clicks  : %u drained via ra8_touch -> I3C -> GT911\n",
                board_periph_touch_reported());
  /* Emit any console bytes still buffered without a trailing newline. */
  console_flush_line(board_periph_sci_console_channel());
  /* Peripheral-model observability: LED transitions, timer totals, IRQ counts,
   * SCI byte totals. */
  board_periph_report(uc);
  board_net_report();
  sim_mmio_print_counts();
  /* Device summary: surface the attached microSD card so a headless run shows
   * its size / format the same way the --view sidebar does. */
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
  sim_mmio_print_bgc_and_table();
  if (timed_out && !idle_stopped && !usb_stopped && !stop_sym_hit && !prof_stopped &&
      !sim_exc_bkpt_hit()) {
    /* The wall-clock guard fired (clock() is CPU-time, so a heavily-loaded host
     * burns the budget faster). This is a TRUNCATED run, NOT a completed one --
     * say so plainly, and do NOT print the "EXECUTED to the run budget" line a
     * full-budget run prints. Conflating the two let a load-correlated truncation
     * masquerade as success, dropping a deterministic banner without failing the
     * gate (#168). A caller that wants the run bounded by a deterministic event
     * (not by CPU-time) should pass BOARD_SIM_STOP_ON / BOARD_SIM_MAX_CHUNKS. */
    (void)fprintf(stderr,
                  "  => board_sim TRUNCATED by the wall-clock guard at chunk %u of %u "
                  "(%.0fs CPU-time elapsed); this is NOT a full-budget run (host "
                  "overloaded -- see #168).\n",
                  chunks,
                  max_chunks,
                  wall_s);
  } else if (((err == UC_ERR_OK) || idle_stopped || usb_stopped || stop_sym_hit || prof_stopped) &&
             !sim_exc_bkpt_hit()) {
    if (idle_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (idle steady-state: no "
                    "observable change for %u chunks, stopped at chunk %u).\n",
                    idle_stop_chunks,
                    chunks);
    } else if (usb_stopped) {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (USB enumerated: device "
                    "CONFIGURED, stopped at chunk %u).\n",
                    chunks);
    } else {
      (void)fprintf(stderr,
                    "  => firmware EXECUTED to the run budget (no invalid opcode / fault).\n");
    }
  }

  /* --dump-sym: read each resolved global from Unicorn memory and print its
   * 32-bit value (and the address), so a test can probe firmware state (e.g. an
   * init-step or mismatch counter) after the run without a debugger. */
  for (uint32_t d = 0U; d < dump_sym_n; d++) {
    if (dump_sym_addrs[d] == 0U) {
      (void)fprintf(stderr, "  dump-sym      : %s = <unresolved>\n", dump_sym_names[d]);
      continue;
    }
    uint32_t v = 0U;
    if (uc_mem_read(uc, (uint64_t)dump_sym_addrs[d], &v, sizeof(v)) == UC_ERR_OK) {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = %u (0x%08X)\n",
                    dump_sym_names[d],
                    dump_sym_addrs[d],
                    v,
                    v);
    } else {
      (void)fprintf(stderr,
                    "  dump-sym      : %s @0x%08X = <unreadable>\n",
                    dump_sym_names[d],
                    dump_sym_addrs[d]);
    }
  }

  if ((ppm_path != nullptr) && (composite != nullptr)) {
    build_composite(uc,
                    panel_fb,
                    rot_fb,
                    composite,
                    panel_w,
                    panel_h,
                    disp_w,
                    disp_h,
                    rotate_deg,
                    win_title);
    if (write_ppm(ppm_path, composite, comp_w, comp_h) == 0) {
      (void)fprintf(stderr, "  wrote %s (%ux%u)\n", ppm_path, (unsigned)comp_w, (unsigned)comp_h);
    } else {
      (void)fprintf(stderr, "  could not write %s\n", ppm_path);
    }
  }
  if (record_dir != nullptr) {
    (void)fprintf(stderr,
                  "  recorded %u frame(s) to %s (%ux%u, ~%u fps)\n",
                  rec_frames,
                  record_dir,
                  (unsigned)comp_w,
                  (unsigned)comp_h,
                  (unsigned)k_record_fps);
  }
  if (view != nullptr) {
    if (!closed) { /* run ended on its own -- keep the last frame up until closed */
      build_composite(uc,
                      panel_fb,
                      rot_fb,
                      composite,
                      panel_w,
                      panel_h,
                      disp_w,
                      disp_h,
                      rotate_deg,
                      win_title);
      board_view_present(view, composite, comp_w, comp_h);
      (void)fprintf(stderr, "board_sim: run ended; close the window to exit\n");
      while (!board_view_pump(view)) {
        (void)usleep((useconds_t)k_view_idle_us);
      }
    }
    board_view_close(view);
  }
  /* --save-sd: dump the (possibly firmware-modified) SD card image for reuse. */
  if (save_sd_path != nullptr) {
    (void)board_sd_save(save_sd_path);
  }
  free(panel_fb);
  free(rot_fb);
  free(composite);
  free(elf); /* kept alive for the whole run so a warm reboot can re-load it */
  (void)uc_close(uc);

  /* Exit status for the #67 run-every-example matrix: a clean run-to-budget is
   * 0; a firmware BKPT, an emulation fault, or the wall-clock timeout each map
   * to a distinct non-zero code so the matrix can flag a wedged or trapped run
   * by exit code alone. */
  board_sim_exit_t exit_code = k_board_sim_exit_ok;
  if (sim_exc_bkpt_hit()) {
    exit_code = k_board_sim_exit_bkpt;
  } else if (err != UC_ERR_OK) {
    exit_code = k_board_sim_exit_fault;
  } else if (timed_out) {
    exit_code = k_board_sim_exit_timeout;
  }
  return (int)exit_code;
}
