/**
 * @file sim_args.c
 * @brief CLI parsing implementation (see sim_args.h)
 *
 * @details
 * The usage text and the option-decoding loop -- moved verbatim out of the
 * board_sim main translation unit. The contract lives on the declaration of
 * sim_args_parse() in sim_args.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "sim_args.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_periph_eink.h"
#include "board_periph_modem.h"
#include "board_periph_sd.h"
#include "sim_seams.h"
#include "sim_view.h"

bool sim_args_parse(int argc, char** argv, sim_args_t* out)
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
    return false;
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
  const char* dump_sym_names[k_dump_sym_max]   = {}; /* --dump-sym globals to read. */
  uint32_t    dump_sym_n                       = 0U;
  const char* stop_sym_name                    = nullptr; /* --stop-sym watch global.      */
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

  const sim_args_t parsed = {
    .elf_path         = elf_path,
    .want_view        = want_view,
    .ppm_path         = ppm_path,
    .record_dir       = record_dir,
    .record_secs      = record_secs,
    .rotate_deg       = rotate_deg,
    .panel_path       = panel_path,
    .input_str        = input_str,
    .keys_str         = keys_str,
    .touch_seq_str    = touch_seq_str,
    .usb_in_str       = usb_in_str,
    .dump_sym_names   = {},
    .dump_sym_n       = dump_sym_n,
    .stop_sym_name    = stop_sym_name,
    .stop_sym_thresh  = stop_sym_thresh,
    .trace_sym_names  = {},
    .trace_sym_n      = trace_sym_n,
    .save_sd_path     = save_sd_path,
    .ns_elf_path      = ns_elf_path,
    .size_set         = size_set,
    .want_click       = want_click,
    .want_trace       = want_trace,
    .click_x          = click_x,
    .click_y          = click_y,
    .button_press     = button_press,
    .reboot_count     = reboot_count,
    .battery_soc      = battery_soc,
    .battery_charging = battery_charging,
    .battery_opt      = battery_opt,
    .view_w           = view_w,
    .view_h           = view_h,
    .sim_device       = sim_device,
    .usbhs_loop       = usbhs_loop,
  };
  *out = parsed;
  for (uint32_t d = 0U; d < dump_sym_n; d++) {
    out->dump_sym_names[d] = dump_sym_names[d];
  }
  for (uint32_t t = 0U; t < trace_sym_n; t++) {
    out->trace_sym_names[t] = trace_sym_names[t];
  }
  return true;
}
