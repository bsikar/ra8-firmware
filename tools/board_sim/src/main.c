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
#include "sim_args.h"
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
  sim_args_t args = {};
  if (!sim_args_parse(argc, argv, &args)) {
    return 2;
  }
  const char* const    elf_path                       = args.elf_path;
  const bool           want_view                      = args.want_view;
  const char* const    ppm_path                       = args.ppm_path;
  const char* const    record_dir                     = args.record_dir;
  const uint32_t       record_secs                    = args.record_secs;
  const uint32_t       rotate_deg                     = args.rotate_deg;
  const char* const    panel_path                     = args.panel_path;
  const char* const    input_str                      = args.input_str;
  const char* const    keys_str                       = args.keys_str;
  const char* const    touch_seq_str                  = args.touch_seq_str;
  const char* const    usb_in_str                     = args.usb_in_str;
  const char* const*   dump_sym_names                 = args.dump_sym_names;
  uint32_t             dump_sym_addrs[k_dump_sym_max] = {}; /* resolved while ELF is alive. */
  const uint32_t       dump_sym_n                     = args.dump_sym_n;
  const char* const    stop_sym_name                  = args.stop_sym_name;
  uint32_t             stop_sym_addr                  = 0U; /* resolved while ELF is alive. */
  const uint32_t       stop_sym_thresh                = args.stop_sym_thresh;
  const char* const*   trace_sym_names                = args.trace_sym_names;
  const uint32_t       trace_sym_n                    = args.trace_sym_n;
  const char* const    save_sd_path                   = args.save_sd_path;
  const char* const    ns_elf_path                    = args.ns_elf_path;
  const bool           size_set                       = args.size_set;
  const bool           want_click                     = args.want_click;
  const bool           want_trace                     = args.want_trace;
  const int            click_x                        = args.click_x;
  const int            click_y                        = args.click_y;
  const int            button_press                   = args.button_press;
  const int            reboot_count                   = args.reboot_count;
  const int            battery_soc                    = args.battery_soc;
  const bool           battery_charging               = args.battery_charging;
  const bool           battery_opt                    = args.battery_opt;
  uint16_t             view_w                         = args.view_w;
  uint16_t             view_h                         = args.view_h;
  const board_device_t sim_device                     = args.sim_device;
  const bool           usbhs_loop                     = args.usbhs_loop;

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
