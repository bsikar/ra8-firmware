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

/**
 * @brief Print the board_sim CLI usage text to stderr.
 *
 * @details The full option reference, printed when board_sim is invoked
 * without a firmware path. Text is verbatim from the pre-split parser.
 *
 * @return void
 * @pre stderr is writable.
 * @pre The caller is about to return a parse failure.
 * @post The usage text has been written to stderr.
 * @post No parse state is produced.
 * @note Not thread-safe; single-threaded CLI setup.
 * @since 0.1.0
 */
static void args_print_usage(void)
{
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
}

/**
 * @brief Seed the parsed-args struct with every option's default.
 *
 * @details Zero-initializes @p out (so all flags / counts / string pointers are
 * false / 0 / NULL) then sets the non-zero defaults: the ELF path, the unset
 * sentinels (-1 for click / battery), the default panel size, and the RA8D2
 * device. The option parsers overwrite only the fields their flags set.
 *
 * @param[in]  elf_path The firmware path from argv[1].
 * @param[out] out      The struct to seed.
 * @return void
 * @pre @p elf_path and @p out are non-NULL.
 * @pre The caller has verified argc >= 2.
 * @post @p out holds every field's documented default.
 * @post @p out->elf_path == @p elf_path.
 * @note Not thread-safe; single-threaded CLI setup.
 * @since 0.1.0
 */
static void args_defaults(const char* elf_path, sim_args_t* out)
{
  *out             = (sim_args_t){};
  out->elf_path    = elf_path;
  out->rotate_deg  = (uint32_t)k_rotate_0;
  out->click_x     = -1;
  out->click_y     = -1;
  out->battery_soc = -1;
  out->view_w      = (uint16_t)k_view_default_w;
  out->view_h      = (uint16_t)k_view_default_h;
  out->sim_device  = k_board_device_ra8d2;
}

/**
 * @brief Parse the mode / core / device / attach-flag options.
 *
 * @details Handles --view / --usbhs-loop / --trace / --low-power / --fast-sd /
 * --eink / --modem (no-value flags and side-effect attaches) plus the
 * --primary-core and --device value options. Exact-match strncmp, so the group
 * is order-independent relative to the other parsers.
 *
 * @param[in]     argc The argument count.
 * @param[in]     argv The argument vector.
 * @param[in,out] i    Index of the current option; advanced past consumed values.
 * @param[in,out] out  The struct to update.
 * @return true when the option at @p *i belonged to this group.
 * @retval true  The option was recognized and applied.
 * @retval false Not one of this group's options; try the next parser.
 * @pre @p argv, @p i and @p out are non-NULL and @p *i is in [2, argc).
 * @pre @p out has been seeded by ::args_defaults.
 * @post On true, @p out and @p *i reflect the consumed option.
 * @note Not thread-safe; single-threaded CLI setup.
 * @since 0.1.0
 */
static bool args_try_mode(int argc, char** argv, int* i, sim_args_t* out)
{
  const int idx = *i;
  if (strncmp(argv[idx], "--view", sizeof("--view")) == 0) {
    out->want_view = true;
  } else if (strncmp(argv[idx], "--usbhs-loop", sizeof("--usbhs-loop")) == 0) {
    /* Model the on-chip USBHS host controller (board_periph_usbhs_host.c) and
     * bridge it to the USBFS device, instead of the ra8_usb_host_* function
     * seam: for apps whose HS host enumerates the SAME chip's FS device. */
    out->usbhs_loop = true;
  } else if (strncmp(argv[idx], "--trace", sizeof("--trace")) == 0) {
    out->want_trace = true;
  } else if (strncmp(argv[idx], "--low-power", sizeof("--low-power")) == 0) {
    sim_set_low_power(true);
  } else if (strncmp(argv[idx], "--fast-sd", sizeof("--fast-sd")) == 0) {
    sim_fast_sd_enable();
  } else if (strncmp(argv[idx], "--eink", sizeof("--eink")) == 0) {
    (void)board_eink_attach(); /* answer the ra8_epaper SPI path (IT8951 model) */
  } else if (strncmp(argv[idx], "--modem", sizeof("--modem")) == 0) {
    (void)board_modem_attach(); /* answer the ra8_modem_at AT path (SCI7 modem model) */
  } else if ((strncmp(argv[idx], "--primary-core", sizeof("--primary-core")) == 0) &&
             ((idx + 1) < argc)) {
    sim_set_primary_core((strncmp(argv[idx + 1], "m33", sizeof("m33")) == 0) ? k_core_m33
                                                                             : k_core_m85);
    (*i)++;
  } else if ((strncmp(argv[idx], "--device", sizeof("--device")) == 0) && ((idx + 1) < argc)) {
    /* Select the emulated part; anything but "ra8p1" (incl. "ra8d2") = RA8D2. */
    out->sim_device = (strncmp(argv[idx + 1], "ra8p1", sizeof("ra8p1")) == 0)
                        ? k_board_device_ra8p1
                        : k_board_device_ra8d2;
    (*i)++;
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Parse the display / output options.
 *
 * @details Handles --ppm / --record-secs / --record / --rotate / --panel /
 * --size. Exact-match strncmp keeps the group order-independent.
 *
 * @param[in]     argc The argument count.
 * @param[in]     argv The argument vector.
 * @param[in,out] i    Index of the current option; advanced past consumed values.
 * @param[in,out] out  The struct to update.
 * @return true when the option at @p *i belonged to this group.
 * @retval true  The option was recognized and applied.
 * @retval false Not one of this group's options; try the next parser.
 * @pre @p argv, @p i and @p out are non-NULL and @p *i is in [2, argc).
 * @pre @p out has been seeded by ::args_defaults.
 * @post On true, @p out and @p *i reflect the consumed option.
 * @note Not thread-safe; single-threaded CLI setup.
 * @since 0.1.0
 */
static bool args_try_display(int argc, char** argv, int* i, sim_args_t* out)
{
  const int idx = *i;
  if ((strncmp(argv[idx], "--ppm", sizeof("--ppm")) == 0) && ((idx + 1) < argc)) {
    out->ppm_path = argv[idx + 1];
    (*i)++;
  } else if ((strncmp(argv[idx], "--record-secs", sizeof("--record-secs")) == 0) &&
             ((idx + 1) < argc)) {
    const long s     = strtol(argv[idx + 1], nullptr, (int)k_strtol_base10);
    out->record_secs = (s > 0L) ? (uint32_t)s : 0U;
    (*i)++;
  } else if ((strncmp(argv[idx], "--record", sizeof("--record")) == 0) && ((idx + 1) < argc)) {
    out->record_dir = argv[idx + 1];
    (*i)++;
  } else if ((strncmp(argv[idx], "--rotate", sizeof("--rotate")) == 0) && ((idx + 1) < argc)) {
    const long deg = strtol(argv[idx + 1], nullptr, (int)k_strtol_base10);
    if ((deg == (long)k_rotate_90) || (deg == (long)k_rotate_180) || (deg == (long)k_rotate_270)) {
      out->rotate_deg = (uint32_t)deg;
    }
    (*i)++;
  } else if ((strncmp(argv[idx], "--panel", sizeof("--panel")) == 0) && ((idx + 1) < argc)) {
    out->panel_path = argv[idx + 1];
    (*i)++;
  } else if ((strncmp(argv[idx], "--size", sizeof("--size")) == 0) && ((idx + 1) < argc)) {
    char*      end = nullptr;
    const long w   = strtol(argv[idx + 1], &end, (int)k_strtol_base10);
    const long h =
      ((end != nullptr) && (*end == 'x')) ? strtol(end + 1, nullptr, (int)k_strtol_base10) : 0L;
    if ((w > 0L) && (w <= (long)k_max_panel_px) && (h > 0L) && (h <= (long)k_max_panel_px)) {
      out->view_w   = (uint16_t)w;
      out->view_h   = (uint16_t)h;
      out->size_set = true;
    }
    (*i)++;
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Parse the input-injection options.
 *
 * @details Handles --ns / --input / --keys / --touch-seq / --usb-in / --click.
 * Exact-match strncmp keeps the group order-independent.
 *
 * @param[in]     argc The argument count.
 * @param[in]     argv The argument vector.
 * @param[in,out] i    Index of the current option; advanced past consumed values.
 * @param[in,out] out  The struct to update.
 * @return true when the option at @p *i belonged to this group.
 * @retval true  The option was recognized and applied.
 * @retval false Not one of this group's options; try the next parser.
 * @pre @p argv, @p i and @p out are non-NULL and @p *i is in [2, argc).
 * @pre @p out has been seeded by ::args_defaults.
 * @post On true, @p out and @p *i reflect the consumed option.
 * @note Not thread-safe; single-threaded CLI setup.
 * @since 0.1.0
 */
static bool args_try_input(int argc, char** argv, int* i, sim_args_t* out)
{
  const int idx = *i;
  if ((strncmp(argv[idx], "--ns", sizeof("--ns")) == 0) && ((idx + 1) < argc)) {
    /* Second ELF: the Non-Secure image of a two-image TrustZone app (src/app).
     * Loaded at its LMA so the Secure boot's NS-image copy + BLXNS land on it. */
    out->ns_elf_path = argv[idx + 1];
    (*i)++;
  } else if ((strncmp(argv[idx], "--input", sizeof("--input")) == 0) && ((idx + 1) < argc)) {
    out->input_str = argv[idx + 1];
    (*i)++;
  } else if ((strncmp(argv[idx], "--keys", sizeof("--keys")) == 0) && ((idx + 1) < argc)) {
    out->keys_str = argv[idx + 1];
    (*i)++;
  } else if ((strncmp(argv[idx], "--touch-seq", sizeof("--touch-seq")) == 0) &&
             ((idx + 1) < argc)) {
    out->touch_seq_str = argv[idx + 1];
    (*i)++;
  } else if ((strncmp(argv[idx], "--usb-in", sizeof("--usb-in")) == 0) && ((idx + 1) < argc)) {
    out->usb_in_str = argv[idx + 1];
    (*i)++;
  } else if ((strncmp(argv[idx], "--click", sizeof("--click")) == 0) && ((idx + 2) < argc)) {
    out->click_x    = (int)strtol(argv[idx + 1], nullptr, (int)k_strtol_base10);
    out->click_y    = (int)strtol(argv[idx + 2], nullptr, (int)k_strtol_base10);
    out->want_click = (out->click_x >= 0) && (out->click_y >= 0);
    *i += 2;
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Parse a `--sd-new` size spec and attach a blank formatted card.
 *
 * @details
 * The spec is `<N>[k|m|g|t][:fat16|fat32]`. A bare number is MiB; a k/m/g/t
 * suffix sets the unit (so "30g" is 30 GiB). The FAT flavour defaults by size
 * the way a real SD card is shipped (FAT32 at or above @ref k_fat32_min_mib),
 * because FAT16 cannot address a multi-GB card; an explicit `:fat16` / `:fat32`
 * overrides that.
 *
 * @param[in] spec Size specification text from the command line.
 *
 * @pre @p spec is a NUL-terminated argument.
 * @pre The SD model has no card attached yet.
 * @post A card is attached only when the size is non-zero and within the
 *       32-bit sector count FAT can address.
 * @post An over-large request is diagnosed on stderr and attaches nothing.
 *
 * @note Not thread-safe; argument parsing runs once at startup.
 */
static void args_attach_blank_sd(const char* spec)
{
  char*      endp = nullptr;
  const long num  = strtol(spec, &endp, (int)k_strtol_base10);
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
}

/**
 * @brief Parse the microSD options.
 *
 * @details Handles --sd (attach an image), --sd-new (create + attach a blank
 * formatted card, decoding the N[k|m|g|t][:fat16|fat32] spec) and --save-sd.
 * Exact-match strncmp keeps the group order-independent.
 *
 * @param[in]     argc The argument count.
 * @param[in]     argv The argument vector.
 * @param[in,out] i    Index of the current option; advanced past consumed values.
 * @param[in,out] out  The struct to update.
 * @return true when the option at @p *i belonged to this group.
 * @retval true  The option was recognized and applied.
 * @retval false Not one of this group's options; try the next parser.
 * @pre @p argv, @p i and @p out are non-NULL and @p *i is in [2, argc).
 * @pre @p out has been seeded by ::args_defaults.
 * @post On true, @p out / @p *i and any SD attach reflect the consumed option.
 * @note Not thread-safe; single-threaded CLI setup.
 * @since 0.1.0
 */
static bool args_try_sd(int argc, char** argv, int* i, sim_args_t* out)
{
  const int idx = *i;
  if ((strncmp(argv[idx], "--sd", sizeof("--sd")) == 0) && ((idx + 1) < argc)) {
    (void)board_sd_attach(argv[idx + 1]); /* serve this FAT image to ra8_sdmmc_spi */
    (*i)++;
  } else if ((strncmp(argv[idx], "--sd-new", sizeof("--sd-new")) == 0) && ((idx + 1) < argc)) {
    /* "<N>[k|m|g|t][:fat16|fat32]" -- create + attach a blank formatted card.
     * A bare number is MiB; a k/m/g/t suffix sets the unit (so "30g" = 30 GiB).
     * Format defaults by size (FAT32 >= 512 MiB) like a real SD card; FAT16
     * cannot exceed its cluster ceiling, so multi-GB cards are FAT32. */
    args_attach_blank_sd(argv[idx + 1]);
    (*i)++;
  } else if ((strncmp(argv[idx], "--save-sd", sizeof("--save-sd")) == 0) && ((idx + 1) < argc)) {
    out->save_sd_path = argv[idx + 1];
    (*i)++;
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Parse the symbol-probe options.
 *
 * @details Handles --dump-sym (append a global to read after the run),
 * --stop-sym (watch a global + threshold) and --trace-sym (log a function's
 * entries). The dump / trace lists cap at their array bounds. Exact-match
 * strncmp keeps the group order-independent.
 *
 * @param[in]     argc The argument count.
 * @param[in]     argv The argument vector.
 * @param[in,out] i    Index of the current option; advanced past consumed values.
 * @param[in,out] out  The struct to update.
 * @return true when the option at @p *i belonged to this group.
 * @retval true  The option was recognized and applied.
 * @retval false Not one of this group's options; try the next parser.
 * @pre @p argv, @p i and @p out are non-NULL and @p *i is in [2, argc).
 * @pre @p out has been seeded by ::args_defaults.
 * @post On true, @p out and @p *i reflect the consumed option.
 * @note Not thread-safe; single-threaded CLI setup.
 * @since 0.1.0
 */
static bool args_try_sym(int argc, char** argv, int* i, sim_args_t* out)
{
  const int idx = *i;
  if ((strncmp(argv[idx], "--dump-sym", sizeof("--dump-sym")) == 0) && ((idx + 1) < argc)) {
    if (out->dump_sym_n < (uint32_t)k_dump_sym_max) {
      out->dump_sym_names[out->dump_sym_n] = argv[idx + 1];
      out->dump_sym_n++;
    }
    (*i)++;
  } else if ((strncmp(argv[idx], "--stop-sym", sizeof("--stop-sym")) == 0) && ((idx + 2) < argc)) {
    /* End the run the instant 32-bit global <name> reaches <threshold> -- the
     * counter analog of BOARD_SIM_STOP_ON, for the jlink_memprobe SIL mode: a
     * free-running progress counter that hits its floor stops the run at once,
     * so a passing probe finishes in a few chunks instead of the full budget. */
    out->stop_sym_name   = argv[idx + 1];
    out->stop_sym_thresh = (uint32_t)strtoul(argv[idx + 2], nullptr, (int)k_strtol_base10);
    *i += 2;
  } else if ((strncmp(argv[idx], "--trace-sym", sizeof("--trace-sym")) == 0) &&
             ((idx + 1) < argc)) {
    if (out->trace_sym_n < (uint32_t)k_trace_sym_max) {
      out->trace_sym_names[out->trace_sym_n] = argv[idx + 1];
      out->trace_sym_n++;
    }
    (*i)++;
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Parse the button / battery / reboot run-control options.
 *
 * @details Handles --button, --battery, --charge and --reboot. --battery and
 * --charge both latch battery_opt. Exact-match strncmp keeps the group
 * order-independent.
 *
 * @param[in]     argc The argument count.
 * @param[in]     argv The argument vector.
 * @param[in,out] i    Index of the current option; advanced past consumed values.
 * @param[in,out] out  The struct to update.
 * @return true when the option at @p *i belonged to this group.
 * @retval true  The option was recognized and applied.
 * @retval false Not one of this group's options; try the next parser.
 * @pre @p argv, @p i and @p out are non-NULL and @p *i is in [2, argc).
 * @pre @p out has been seeded by ::args_defaults.
 * @post On true, @p out and @p *i reflect the consumed option.
 * @note Not thread-safe; single-threaded CLI setup.
 * @since 0.1.0
 */
static bool args_try_control(int argc, char** argv, int* i, sim_args_t* out)
{
  const int idx = *i;
  if ((strncmp(argv[idx], "--button", sizeof("--button")) == 0) && ((idx + 1) < argc)) {
    out->button_press = (int)strtol(argv[idx + 1], nullptr, (int)k_strtol_base10);
    *i += 1;
  } else if ((strncmp(argv[idx], "--battery", sizeof("--battery")) == 0) && ((idx + 1) < argc)) {
    out->battery_soc = (int)strtol(argv[idx + 1], nullptr, (int)k_strtol_base10);
    out->battery_opt = true;
    *i += 1;
  } else if (strncmp(argv[idx], "--charge", sizeof("--charge")) == 0) {
    out->battery_charging = true;
    out->battery_opt      = true;
  } else if ((strncmp(argv[idx], "--reboot", sizeof("--reboot")) == 0) && ((idx + 1) < argc)) {
    out->reboot_count = (int)strtol(argv[idx + 1], nullptr, (int)k_strtol_base10);
    *i += 1;
  } else {
    return false;
  }
  return true;
}

bool sim_args_parse(int argc, char** argv, sim_args_t* out)
{
  if (argc < 2) {
    args_print_usage();
    return false;
  }
  args_defaults(argv[1], out);
  for (int i = 2; i < argc; i++) {
    (void)(args_try_mode(argc, argv, &i, out) || args_try_display(argc, argv, &i, out) ||
           args_try_input(argc, argv, &i, out) || args_try_sd(argc, argv, &i, out) ||
           args_try_sym(argc, argv, &i, out) || args_try_control(argc, argv, &i, out));
  }
  return true;
}
