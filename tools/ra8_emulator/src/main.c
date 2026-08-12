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
 *   ra8_emulator <firmware.elf>
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
#include "emu_args.h"
#include "emu_console.h"
#include "emu_cpu1.h"
#include "emu_elf.h"
#include "emu_engine.h"
#include "emu_exc.h"
#include "emu_memmap.h"
#include "emu_mmio.h"
#include "emu_mpu.h"
#include "emu_prof.h"
#include "emu_run.h"
#include "emu_seams.h"
#include "emu_trace.h"
#include "emu_tz.h"
#include "emu_usbh_seam.h"
#include "emu_view.h"

/**
 * @brief Apply a --panel descriptor to the window size and title.
 *
 * @details A --panel descriptor sizes the window to that display (so the
 * emulator can present any panel, not just 1024x600); an explicit --size still
 * wins. Returns the window caption -- the panel name when named, else the
 * default.
 *
 * @param[in]     args   The parsed CLI args (--panel path + --size flag).
 * @param[in,out] view_w In: default width; out: panel width when adopted.
 * @param[in,out] view_h In: default height; out: panel height when adopted.
 * @return The window / sidebar caption string.
 * @retval "ra8_emulator" No named panel was loaded.
 * @pre @p args, @p view_w and @p view_h are non-NULL.
 * @pre @p view_w / @p view_h hold the default or --size dimensions.
 * @post @p view_w / @p view_h reflect the panel size iff adopted.
 * @post The returned pointer outlives the run (argv- or literal-backed).
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static const char* main_apply_panel(const emu_args_t* args, uint16_t* view_w, uint16_t* view_h)
{
  static board_panel_t s_panel    = {};
  bool                 have_panel = false;
  if (args->panel_path != nullptr) {
    have_panel = load_panel(args->panel_path, &s_panel);
    if (have_panel && !args->size_set) {
      *view_w = s_panel.width;
      *view_h = s_panel.height;
    }
  }
  return (have_panel && (s_panel.name[0] != '\0')) ? s_panel.name : "ra8_emulator";
}

/**
 * @brief Open the Unicorn engine, set the memory map, seed hardwired SCS regs.
 *
 * @details Opens an Armv8-M (Thumb, M-class) engine on the closest emulated
 * core to the M85 (Cortex-M33), lays down the RA8D2 memory map, and hardwires
 * MPU_TYPE.DREGION to the M85's 8 data regions so ra8_mpu_configure validates
 * (ra8_emulator does not enforce MPU permissions; the app takes its no-trap host
 * path).
 *
 * @param[out] uc_out Receives the opened engine on success.
 * @return 0 on success, 1 on failure.
 * @retval 0 The engine, memory map and seeds are ready.
 * @retval 1 uc_open or the memory-map init failed (message printed).
 * @pre @p uc_out is non-NULL.
 * @pre No engine is currently open in @p *uc_out.
 * @post On success @p *uc_out is a ready engine with the RA8D2 map.
 * @post On failure @p *uc_out is unchanged or NULL.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static int main_open_engine(uc_engine** uc_out)
{
  uc_engine* uc = nullptr;
  if (uc_open(UC_ARCH_ARM, (uc_mode)(UC_MODE_THUMB | UC_MODE_MCLASS), &uc) != UC_ERR_OK) {
    (void)fprintf(stderr, "uc_open failed\n");
    return 1;
  }
  (void)uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M33); /* closest to the M85. */
  if (!emu_memmap_init(uc)) {
    return 1;
  }
  /* Seed hardwired MPU_TYPE.DREGION (=8) so ra8_mpu_configure validates; the PPB
   * is plain RAM here, so it would otherwise read 0 and panic mpu_partition_simple. */
  wr32(uc, (uint64_t)k_mpu_type, (uint32_t)k_mpu_type_seed);
  *uc_out = uc;
  return 0;
}

/**
 * @brief Arm the modelled GT911 injection FIFO from a --touch-seq string.
 *
 * @details Parses "x0:y0,x1:y1,..." (bounded per NASA Rule 2) and pushes each
 * valid raw point into the GT911 model, which then serves one queued point per
 * drained frame through the genuine ra8_touch_read decode (a headless multi-tap
 * flow). Must run AFTER board_periph_init so the per-block reset does not clear it.
 *
 * @param[in] touch_seq_str The --touch-seq spec, or NULL when unset.
 * @return void
 * @pre board_periph_init has already run.
 * @pre @p touch_seq_str is a valid string or NULL.
 * @post With a spec, the GT911 FIFO holds the parsed points and a count printed.
 * @post Without a spec, no state changes.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static void main_arm_touch_seq(const char* touch_seq_str)
{
  if (touch_seq_str == nullptr) {
    return;
  }
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
  (void)fprintf(stderr, "ra8_emulator: --touch-seq armed %u raw point(s)\n", (unsigned)pushed);
}

/**
 * @brief Apply --button and --battery / --charge before the firmware boots.
 *
 * @details --button holds a user switch pressed (active-low) so a button-polling
 * app takes its pressed path (SW1 = P009, SW2 = P008); the battery flags set the
 * MAX17048 fuel-gauge state-of-charge and charging status.
 *
 * @param[in] args The parsed CLI args (button / battery fields).
 * @return void
 * @pre board_periph_init has already run.
 * @pre @p args is non-NULL.
 * @post Any requested switch hold / battery state is applied and announced.
 * @post No state changes when neither option is given.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static void main_apply_button_battery(const emu_args_t* args)
{
  if (args->button_press != 0) {
    const uint8_t pin = (args->button_press == 2) ? (uint8_t)k_emu_sw2_pin : (uint8_t)k_emu_sw1_pin;
    board_periph_gpio_set_input((uint8_t)k_emu_sw_port, pin, false);
    (void)fprintf(stderr,
                  "ra8_emulator: --button %d held (SW pin P00%u low/pressed)\n",
                  args->button_press,
                  (unsigned)pin);
  }
  if (args->battery_opt) {
    uint8_t cur_soc = 0U;
    board_periph_battery_get(&cur_soc, nullptr);
    const uint8_t soc = (args->battery_soc >= 0) ? (uint8_t)args->battery_soc : cur_soc;
    board_periph_battery_set(soc, args->battery_charging);
    (void)fprintf(stderr,
                  "ra8_emulator: battery %u%% %s (MAX17048 @ I2C 0x36)\n",
                  (unsigned)soc,
                  args->battery_charging ? "charging" : "discharging");
  }
}

/**
 * @brief Queue the --input / --keys / --usb-in injected byte streams.
 *
 * @details --input feeds the console UART RX (SCI8); --keys pushes through the
 * SAME keystroke FIFO the live window feeds (a headless keyboard test); --usb-in
 * queues bytes for the virtual host to push over the CDC bulk OUT pipe (echoed
 * back on bulk IN). Each decodes escape sequences first.
 *
 * @param[in] args The parsed CLI args (input string fields).
 * @return void
 * @pre board_periph_init has already run.
 * @pre @p args is non-NULL.
 * @post Each provided stream is queued to its endpoint and a count announced.
 * @post No state changes for the streams left unset.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static void main_feed_inputs(const emu_args_t* args)
{
  if (args->input_str != nullptr) {
    uint8_t        rx[k_uart_line_max];
    const uint32_t n = decode_escapes(args->input_str, rx, (uint32_t)sizeof(rx));
    board_periph_sci_feed_rx(board_periph_sci_console_channel(), rx, n);
    (void)fprintf(stderr,
                  "ra8_emulator: queued %u byte(s) to SCI%u RX from --input\n",
                  n,
                  board_periph_sci_console_channel());
  }
  if (args->keys_str != nullptr) {
    uint8_t        kb[k_uart_line_max];
    const uint32_t n = decode_escapes(args->keys_str, kb, (uint32_t)sizeof(kb));
    for (uint32_t k = 0U; k < n; k++) {
      board_input_push_key((char)kb[k]);
    }
    (void)fprintf(stderr, "ra8_emulator: queued %u keystroke(s) via --keys (window-key path)\n", n);
  }
  if (args->usb_in_str != nullptr) {
    uint8_t        ub[k_uart_line_max];
    const uint32_t n = decode_escapes(args->usb_in_str, ub, (uint32_t)sizeof(ub));
    board_usb_feed_bulk_in(ub, n);
    (void)fprintf(stderr, "ra8_emulator: queued %u byte(s) to the USB CDC bulk OUT pipe\n", n);
  }
}

/**
 * @brief Bring up the peripheral models and apply pre-boot input state.
 *
 * @details Resets the peripheral-model framework, arms the --touch-seq FIFO,
 * selects the emulated part, wires the USBHS-loop / external-host bridge, brings
 * up the network model and console TX sink, then applies the button / battery /
 * injected-input options -- all before the firmware boots.
 *
 * @param[in] args The parsed CLI args.
 * @return void
 * @pre The engine is open and mapped.
 * @pre @p args is non-NULL.
 * @post The peripheral models are reset and all pre-boot input state applied.
 * @post The console TX sink and network model are live.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static void main_bringup_peripherals(const emu_args_t* args)
{
  board_periph_init(args->want_trace);
  main_arm_touch_seq(args->touch_seq_str);
  board_periph_set_device(args->emu_device); /* gates the RA8P1-only NPU block. */
  /* --usbhs-loop: activate the on-chip USBHS host model and hand the USBFS device
   * to its bridge so the built-in virtual host stands down (chip-internal loop). */
  board_periph_set_usbhs_loop(args->usbhs_loop);
  board_usb_set_external_host(args->usbhs_loop);
  board_net_init(args->want_trace);
  board_periph_sci_set_tx_sink(console_tx_sink);
  main_apply_button_battery(args);
  main_feed_inputs(args);
}

/**
 * @brief Read + load the primary firmware ELF and print the load banner.
 *
 * @details Reads the ELF file, prints the load + device + primary-core banner,
 * and copies its PT_LOAD segments into Unicorn memory. The host-side buffer is
 * returned to the caller (kept alive for symbol resolution + warm reboots).
 *
 * @param[in]  uc       The engine to load into.
 * @param[in]  elf_path The firmware ELF path.
 * @param[out] elf_out  Receives the host-side image buffer on success.
 * @param[out] len_out  Receives the image length on success.
 * @return 0 on success, 1 on failure.
 * @retval 0 The image is loaded and @p elf_out / @p len_out are set.
 * @retval 1 The file could not be read or loaded (message printed).
 * @pre @p uc is a ready engine; the out params are non-NULL.
 * @pre @p elf_path is a valid path.
 * @post On success the image is in Unicorn memory and owned by the caller.
 * @post On failure any partial buffer is freed.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static int main_load_primary(uc_engine* uc, const char* elf_path, uint8_t** elf_out, long* len_out)
{
  long           elf_len = 0;
  uint8_t* const elf     = read_file(elf_path, &elf_len);
  if (elf == nullptr) {
    (void)fprintf(stderr, "cannot read %s\n", elf_path);
    return 1;
  }
  (void)fprintf(stderr, "ra8_emulator: loading %s (%ld bytes)\n", elf_path, elf_len);
  (void)fprintf(stderr,
                "  device        : %s\n",
                (board_periph_device() == k_board_device_ra8p1)
                  ? "RA8P1 (R7KA8P1KFLCAC) -- +Ethos-U55 NPU"
                  : "RA8D2 (R7KA8D2KFLCAC)");
  (void)fprintf(stderr,
                "  primary core  : %s%s\n",
                (emu_primary_core() == k_core_m33) ? "Cortex-M33 (Armv8-M)"
                                                   : "Cortex-M85 (Armv8.1-M, MVE seams armed)",
                emu_low_power() ? "  [low-power: 1/4 chunk budget]" : "");
  if (load_elf(uc, elf, elf_len) != 0) {
    free(elf);
    return 1;
  }
  *elf_out = elf;
  *len_out = elf_len;
  return 0;
}

/**
 * @brief Read + load the optional --ns second (Non-Secure) image.
 *
 * @details Loads the Non-Secure image of a two-image TrustZone app at its LMA so
 * the Secure boot's NS-image copy + BLXNS land on it, and records its actual
 * vector base for the world switch. A read / load failure frees the primary ELF
 * and fails. A no-op when --ns is unset (@p ns_out set to NULL).
 *
 * @param[in]  uc          The engine to load into.
 * @param[in]  ns_elf_path The --ns path, or NULL when unset.
 * @param[in]  elf         The primary image (freed on this path's failure).
 * @param[out] ns_out      Receives the NS image buffer (or NULL).
 * @param[out] ns_len_out  Receives the NS image length (or 0).
 * @return 0 on success or when --ns is unset, 1 on failure.
 * @retval 0 The NS image loaded, or --ns was not requested.
 * @retval 1 The --ns file could not be read or loaded (message printed).
 * @pre @p uc is a ready engine; the out params are non-NULL.
 * @pre @p elf is the loaded primary image.
 * @post On success @p ns_out is the NS buffer (or NULL) and vector base set.
 * @post On failure @p elf has been freed.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static int main_load_ns(uc_engine*  uc,
                        const char* ns_elf_path,
                        uint8_t*    elf,
                        uint8_t**   ns_out,
                        long*       ns_len_out)
{
  long           ns_len = 0;
  uint8_t* const ns_elf = (ns_elf_path != nullptr) ? read_file(ns_elf_path, &ns_len) : nullptr;
  if (ns_elf_path != nullptr) {
    if (ns_elf == nullptr) {
      (void)fprintf(stderr, "cannot read --ns %s\n", ns_elf_path);
      free(elf);
      return 1;
    }
    (void)fprintf(stderr, "ra8_emulator: loading NS image %s (%ld bytes)\n", ns_elf_path, ns_len);
    if (load_elf(uc, ns_elf, ns_len) != 0) {
      free(ns_elf);
      free(elf);
      return 1;
    }
    /* Track the NS image's vector base (0x32100000 RAM-resident, 0x90000000 XIP
     * from OSPI) so the BLXNS world switch reads MSP/reset from the right place. */
    const uint32_t ns_vbase = elf_vector_base(ns_elf, ns_len);
    if (ns_vbase != 0U) {
      emu_tz_set_ns_vector_base(ns_vbase);
    }
    (void)fprintf(stderr, "ra8_emulator: NS vector base @ 0x%08X\n", emu_tz_ns_vector_base());
  }
  *ns_out     = ns_elf;
  *ns_len_out = ns_len;
  return 0;
}

/**
 * @brief Resolve the --dump-sym globals and the --stop-sym watch address.
 *
 * @details Resolves each probe to a memory address while the ELF symbol tables
 * are still resident (the software analog of the JLink memprobe HIL mode). A
 * two-image app's probe counters live in the NS image, so a miss in the primary
 * table falls through to the --ns table; an unresolved name is reported.
 *
 * @param[in]  elf           The primary image buffer.
 * @param[in]  elf_len       The primary image length.
 * @param[in]  ns_elf        The NS image buffer, or NULL.
 * @param[in]  ns_len        The NS image length.
 * @param[in]  args          The parsed CLI args (probe names).
 * @param[out] dump_sym_addrs Receives the resolved --dump-sym addresses.
 * @param[out] stop_sym_addr  Receives the resolved --stop-sym address (0 = off).
 * @return void
 * @pre The ELF buffers are still resident and the out params non-NULL.
 * @pre @p dump_sym_addrs has room for @p args->dump_sym_n entries.
 * @post Each probe holds its address, or 0 with a reported miss.
 * @post @p *stop_sym_addr is the watch address or 0.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static void main_resolve_symbols(uint8_t*          elf,
                                 long              elf_len,
                                 uint8_t*          ns_elf,
                                 long              ns_len,
                                 const emu_args_t* args,
                                 uint32_t*         dump_sym_addrs,
                                 uint32_t*         stop_sym_addr)
{
  for (uint32_t d = 0U; d < args->dump_sym_n; d++) {
    dump_sym_addrs[d] = elf_sym_addr(elf, elf_len, args->dump_sym_names[d], nullptr);
    if ((dump_sym_addrs[d] == 0U) && (ns_elf != nullptr)) {
      dump_sym_addrs[d] = elf_sym_addr(ns_elf, ns_len, args->dump_sym_names[d], nullptr);
    }
    if (dump_sym_addrs[d] == 0U) {
      (void)fprintf(stderr,
                    "ra8_emulator: --dump-sym %s not found in symbol table\n",
                    args->dump_sym_names[d]);
    }
  }
  *stop_sym_addr = 0U;
  if (args->stop_sym_name != nullptr) {
    *stop_sym_addr = elf_sym_addr(elf, elf_len, args->stop_sym_name, nullptr);
    if ((*stop_sym_addr == 0U) && (ns_elf != nullptr)) {
      *stop_sym_addr = elf_sym_addr(ns_elf, ns_len, args->stop_sym_name, nullptr);
    }
    if (*stop_sym_addr == 0U) {
      (void)fprintf(stderr,
                    "ra8_emulator: --stop-sym %s not found in symbol table\n",
                    args->stop_sym_name);
    }
  }
}

/**
 * @brief Read the reset vector, set the initial CPU registers, print the banner.
 *
 * @details Cortex-M reset: SP = vectors[0], PC = vectors[1] (Thumb). Sets SP /
 * PC / xPSR.T so Unicorn enters Thumb decoding, and prints the reset banner.
 *
 * @param[in,out] uc The engine (memory read, registers written).
 * @return The initial PC (Thumb bit set) for the run configuration.
 * @retval (PC) vectors[1] | 1 from the MRAM vector table.
 * @pre @p uc is a ready engine with the vector table in MRAM.
 * @pre The image has been loaded.
 * @post The engine's SP / PC / xPSR are set for the first instruction.
 * @post The reset banner has been printed to stderr.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static uint32_t main_reset_vector(uc_engine* uc)
{
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  /* MRAM[0] */
  (void)uc_mem_read(uc, emu_memmap_mram_base() + 0U, &sp, 4);
  /* MRAM[4] (Thumb: bit0=1) */
  (void)uc_mem_read(uc, emu_memmap_mram_base() + 4U, &pc, 4);
  pc |= 1U;                               /* M-profile is always Thumb (EPSR.T must be 1). */
  uint32_t xpsr = (uint32_t)k_xpsr_t_bit; /* xPSR.T                                        */
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_reg_write(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)fprintf(
    stderr,
    "ra8_emulator: reset SP=0x%08X PC=0x%08X -- running (<= %u x %u insns, %u s wall)\n",
    sp,
    pc,
    (unsigned)k_run_max_chunks,
    (unsigned)k_run_chunk_insns,
    (unsigned)k_run_wall_s);
  return pc;
}

/**
 * @brief Install the core execution, exception, console, TrustZone + MPU seams.
 *
 * @details Arms the invalid-instruction + long-shift/MVE decode seams' core, the
 * Cortex-M exception entry/return, the ITM/console echo, the TrustZone S->NS
 * BLXNS world switch (armed only for a secure-boot image), the SCB/NVIC model
 * and the MPU model.
 *
 * @param[in,out] uc      The engine.
 * @param[in]     elf     The primary image buffer (symbol tables).
 * @param[in]     elf_len The primary image length.
 * @return void
 * @pre @p uc is a ready engine and @p elf is resident.
 * @pre The image has been loaded into Unicorn memory.
 * @post The core / exception / console / TZ / MPU hooks are installed.
 * @post No firmware code has run yet.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static void main_install_core_seams(uc_engine* uc, uint8_t* elf, long elf_len)
{
  emu_insn_seams_install(uc);
  emu_exc_install_core(uc);
  /* Seed the ITM ready bits + echo stimulus-port writes so ra8_log prints [itm]. */
  emu_console_install(uc);
  /* TrustZone S->NS boot seams (SAU_TYPE seed + hand-emulated BLXNS), armed only
   * when the firmware links the secure boot. */
  emu_tz_install(uc, elf, elf_len);
  emu_exc_install_scb_nvic(uc);
  emu_mpu_install(uc);
}

/**
 * @brief Install the USB, symbol-trace, ISA-emulation and profiler seams.
 *
 * @details The virtual USB host-mode device is skipped under --usbhs-loop (the
 * real ra8_usb_host_* must drive the modelled USBHS controller); the
 * register-level host model is allowed only for an unseamed, non-loop firmware.
 * Arms --trace-sym hooks, the M85-only long-shift/MVE seams (gated by
 * --primary-core), the div-0 UsageFault sites, --fast-sd, the profiler and the
 * cpu1 engine.
 *
 * @param[in,out] uc      The engine.
 * @param[in]     elf     The primary image buffer (symbol tables).
 * @param[in]     elf_len The primary image length.
 * @param[in]     args    The parsed CLI args (usbhs-loop / trace-sym).
 * @return void
 * @pre @p uc is a ready engine and @p elf is resident.
 * @pre ::main_install_core_seams has already run.
 * @post The USB / trace / ISA / profiler / cpu1 seams are installed / armed.
 * @post No firmware code has run yet.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static void
main_install_run_seams(uc_engine* uc, uint8_t* elf, long elf_len, const emu_args_t* args)
{
  bool usbh_seamed = false;
  if (!args->usbhs_loop) {
    usbh_seamed = usbh_seam_install(uc, elf, elf_len);
  }
  /* Register-level USBHS host model: only an UNSEAMED, non-loop firmware engages
   * it, so the C-level seam / loop-only register block do not both answer. */
  board_usb_host_set_allowed(!usbh_seamed && !args->usbhs_loop);
  sym_trace_install(uc, elf, elf_len, args->trace_sym_names, args->trace_sym_n);
  /* M85-only long-shift (LSLL/LSRL/ASRL) + MVE (Helium) seams: off under
   * --primary-core m33 (pure Armv8-M), inert but honest. CSEL rides the invalid-
   * instruction hook and stays armed (also inert for an M33 image). */
  if (emu_primary_core() == k_core_m85) {
    long_shift_seam_install(uc, elf, elf_len);
  }
  div0_seam_install(elf, elf_len);        /* UDIV/SDIV sites, patched only under DIV_0_TRP. */
  fast_sd_seam_install(uc, elf, elf_len); /* --fast-sd whole-block serve; else inert.       */
  prof_load(elf, elf_len);                /* RA8_EMU_PROFILE FUNC symbols + code hook.      */
  emu_prof_install(uc);
  emu_cpu1_init(elf, elf_len); /* dual-core cpu1 engine (shares SRAM); NULL for single-core. */
}

/**
 * @brief Build the run configuration handed to emu_run_and_report().
 *
 * @details Bundles the engine, image, resolved reset PC / vector base, the
 * output-mode CLI knobs and the resolved symbol probes into the read-only
 * ::emu_run_cfg_t. The @p dump_sym_addrs pointer aliases the caller's array,
 * which must outlive the run.
 *
 * @param[in] args           The parsed CLI args.
 * @param[in] uc             The prepared engine.
 * @param[in] elf            The run-long image buffer.
 * @param[in] elf_len        The image length.
 * @param[in] pc             The initial (Thumb) PC.
 * @param[in] vtor_base      The MRAM vector-table base.
 * @param[in] view_w         The panel width.
 * @param[in] view_h         The panel height.
 * @param[in] win_title      The window / sidebar caption.
 * @param[in] dump_sym_addrs The resolved --dump-sym addresses (caller-owned).
 * @param[in] stop_sym_addr  The resolved --stop-sym address (0 = off).
 * @return The populated run configuration (by value).
 * @retval (cfg) Every field mirrors its argument / CLI source.
 * @pre All pointer arguments outlive the returned config's use.
 * @pre @p args and @p uc are non-NULL.
 * @post The returned config references, not copies, the array-backed fields.
 * @note Not thread-safe; single-threaded setup.
 * @since 0.1.0
 */
static emu_run_cfg_t main_build_run_cfg(const emu_args_t* args,
                                        uc_engine*        uc,
                                        uint8_t*          elf,
                                        long              elf_len,
                                        uint32_t          pc,
                                        uint32_t          vtor_base,
                                        uint16_t          view_w,
                                        uint16_t          view_h,
                                        const char*       win_title,
                                        const uint32_t*   dump_sym_addrs,
                                        uint32_t          stop_sym_addr)
{
  return (emu_run_cfg_t){
    .uc              = uc,
    .elf             = elf,
    .elf_len         = elf_len,
    .initial_pc      = pc,
    .vtor_base       = vtor_base,
    .want_trace      = args->want_trace,
    .want_view       = args->want_view,
    .want_click      = args->want_click,
    .click_x         = args->click_x,
    .click_y         = args->click_y,
    .ppm_path        = args->ppm_path,
    .record_dir      = args->record_dir,
    .record_secs     = args->record_secs,
    .rotate_deg      = args->rotate_deg,
    .reboot_count    = args->reboot_count,
    .save_sd_path    = args->save_sd_path,
    .stop_sym_addr   = stop_sym_addr,
    .stop_sym_thresh = args->stop_sym_thresh,
    .dump_sym_names  = args->dump_sym_names,
    .dump_sym_addrs  = dump_sym_addrs,
    .dump_sym_n      = args->dump_sym_n,
    .view_w          = view_w,
    .view_h          = view_h,
    .win_title       = win_title,
  };
}

int main(int argc, char** argv)
{
  emu_args_t args = {};
  if (!emu_args_parse(argc, argv, &args)) {
    return 2;
  }
  uint16_t          view_w    = args.view_w;
  uint16_t          view_h    = args.view_h;
  const char* const win_title = main_apply_panel(&args, &view_w, &view_h);

  uc_engine* uc = nullptr;
  if (main_open_engine(&uc) != 0) {
    return 1;
  }
  main_bringup_peripherals(&args);

  uint8_t* elf     = nullptr;
  long     elf_len = 0;
  if (main_load_primary(uc, args.elf_path, &elf, &elf_len) != 0) {
    return 1;
  }
  uint8_t* ns_elf = nullptr;
  long     ns_len = 0;
  if (main_load_ns(uc, args.ns_elf_path, elf, &ns_elf, &ns_len) != 0) {
    return 1;
  }

  uint32_t dump_sym_addrs[k_dump_sym_max] = {}; /* resolved while ELF is alive. */
  uint32_t stop_sym_addr                  = 0U; /* resolved while ELF is alive. */
  main_resolve_symbols(elf, elf_len, ns_elf, ns_len, &args, dump_sym_addrs, &stop_sym_addr);
  free(ns_elf); /* NS-image symbol table no longer needed (bytes are in Unicorn memory). */
  /* TrustZone NSC pointer validation: patch cmse_check_address_range to BX LR. */
  emu_tz_patch_cmse(uc, elf, elf_len);

  const uint32_t pc = main_reset_vector(uc);
  /* MRAM holds the vector table; the run loop passes this as the VTOR fallback. */
  const uint32_t vtor_base = (uint32_t)emu_memmap_mram_base();
  main_install_core_seams(uc, elf, elf_len);
  main_install_run_seams(uc, elf, elf_len, &args);

  const emu_run_cfg_t run_cfg = main_build_run_cfg(&args,
                                                   uc,
                                                   elf,
                                                   elf_len,
                                                   pc,
                                                   vtor_base,
                                                   view_w,
                                                   view_h,
                                                   win_title,
                                                   dump_sym_addrs,
                                                   stop_sym_addr);
  return emu_run_and_report(&run_cfg);
}
