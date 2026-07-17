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
#include "sim_run.h"
#include "sim_seams.h"
#include "sim_trace.h"
#include "sim_tz.h"
#include "sim_usbh_seam.h"
#include "sim_view.h"

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

  /* Everything from here on -- buffers, env knobs, the chunked run loop, the
   * report and the exit-code mapping -- lives in sim_run_and_report. */
  const sim_run_cfg_t run_cfg = {
    .uc              = uc,
    .elf             = elf,
    .elf_len         = elf_len,
    .initial_pc      = pc,
    .vtor_base       = vtor_base,
    .want_trace      = want_trace,
    .want_view       = want_view,
    .want_click      = want_click,
    .click_x         = click_x,
    .click_y         = click_y,
    .ppm_path        = ppm_path,
    .record_dir      = record_dir,
    .record_secs     = record_secs,
    .rotate_deg      = rotate_deg,
    .reboot_count    = reboot_count,
    .save_sd_path    = save_sd_path,
    .stop_sym_addr   = stop_sym_addr,
    .stop_sym_thresh = stop_sym_thresh,
    .dump_sym_names  = dump_sym_names,
    .dump_sym_addrs  = dump_sym_addrs,
    .dump_sym_n      = dump_sym_n,
    .view_w          = view_w,
    .view_h          = view_h,
    .win_title       = win_title,
  };
  return sim_run_and_report(&run_cfg);
}
