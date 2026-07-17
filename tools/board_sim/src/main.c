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
  k_ra8_err_no_data  = 0x10AU, /**< ra8_err_t value: no RX data.         */
  k_ra8_err_inval_st = 0x104U, /**< ra8_err_t value: invalid state.      */
  k_strtol_base10    = 10U,    /**< Base-10 radix for strtol.            */
  k_max_panel_px     = 4096U,  /**< Largest accepted --size dimension.   */
  k_record_dir_mode  = 0755U,  /**< mkdir mode for the --record dir.     */
  k_dump_sym_max     = 8U,     /**< Max --dump-sym globals per run.      */
  k_sectors_per_mib  = 2048U,  /**< 512-byte sectors per MiB (--sd-new). */
} board_sim_misc_t;

/** @brief 64-bit byte/size units used by --sd-new card sizing. */
typedef enum : uint64_t {
  k_bytes_per_sector = 512ULL,        /**< SD logical sector size in bytes.    */
  k_size_kib         = 1024ULL,       /**< One kibibyte (k suffix multiplier). */
  k_sd_u32_max       = 0xFFFFFFFFULL, /**< 32-bit sector-count ceiling.        */
  k_fat32_min_mib    = 512ULL,        /**< FAT32 default threshold, in MiB.    */
} board_sim_size_t;

/** @brief EK-RA8D2 user-switch GPIO coordinates (active-low): SW1 P009, SW2 P008. */
typedef enum : uint8_t {
  k_sim_sw_port = 0U, /**< Both user switches sit on PORT0. */
  k_sim_sw1_pin = 9U, /**< SW1 -> P009.                     */
  k_sim_sw2_pin = 8U, /**< SW2 -> P008.                     */
} sim_sw_pin_t;

/* Touch on the EK-RA8D2 is now modelled end-to-end: the firmware's real
 * ra8_touch_open / ra8_touch_read run unchanged and drive the GoodIX GT911 over
 * ra8_i3c_transfer (the I3C peripheral in legacy I2C mode), which board_periph
 * models as an I2C bus with a GT911 device. board_sim feeds --click / window
 * clicks into that device (board_periph_touch_inject), so a tap returns through
 * the genuine ra8_touch -> I3C -> GT911 path -- there is no function-level stub. */

/* Live-view (--view) and snapshot (--ppm) presentation settings. */
typedef enum : uint32_t {
  k_view_default_w     = 1024U,    /**< Default window width (EK-RA8D2 panel).      */
  k_view_default_h     = 600U,     /**< Default window height (EK-RA8D2 panel).     */
  k_view_present_every = 16U,      /**< Present the frame every Nth chunk.          */
  k_view_max_chunks    = 4000000U, /**< Cap in --view; closing the window ends.     */
  k_view_idle_us       = 16000U,   /**< ~60 Hz idle pump after the run ends.        */
  k_view_frame_us      = 16000U,   /**< Min wall-us between live presents (~60 Hz). */
  k_view_yield_us      = 2000U,    /**< Yield this long when a present is skipped.  */
  k_us_per_s           = 1000000U, /**< Microseconds per second.                    */
  k_ns_per_us          = 1000U,    /**< Nanoseconds per microsecond.                */
  k_uart_log_max       = 64U,      /**< Console ring depth (mirrors SCI model).     */
  k_reboot_settle      = 1500U,    /**< Chunks to run before a scheduled --reboot.  */
  /* --record settings. One outer chunk advances SysTick once (~1 ms of emulated
   * time at the firmware's 1 kHz tick), so ~1000 chunks == one emulated second.
   * Recording dumps a frame every k_record_every chunks for k_record_fps fps. */
  k_record_ms_per_sec = 1000U, /**< Emulated ms (= chunks) per second.        */
  k_record_fps        = 20U,   /**< Recorded frames per emulated second.      */
  k_record_every      = 50U,   /**< Chunks between recorded frames (1000/20). */
} view_cfg_t;

/* GLCDC graphics-layer 1 (GR1) framebuffer registers + field decode. The HAL
 * programs FLM6.FORMAT[30:28], FLM3.LNOFF[31:16] (line stride in bytes), and
 * FLM5.LNNUM[26:16] (lines - 1); reverse those to recover the framebuffer. */
typedef enum : uint64_t {
  k_glcdc_gr1_saddr = 0x4034310CUL, /**< GR[0].FLM2 framebuffer base.      */
  k_glcdc_gr1_flm3  = 0x40343110UL, /**< GR[0].FLM3 line stride (LNOFF).   */
  k_glcdc_gr1_flm5  = 0x40343118UL, /**< GR[0].FLM5 lines (LNNUM/DATANUM). */
  k_glcdc_gr1_fmt   = 0x4034311CUL, /**< GR[0].FLM6 pixel FORMAT.          */
} glcdc_gr_t;

typedef enum : uint32_t {
  k_glcdc_fmt_rgb565  = 2U,      /**< FLM6.FORMAT code for RGB565.             */
  k_glcdc_fmt_shift   = 28U,     /**< FORMAT[30:28].                           */
  k_glcdc_fmt_mask    = 0x7U,    /**< FORMAT field width.                      */
  k_glcdc_high_shift  = 16U,     /**< FLM3 stride / FLM5 lnnum live in [*:16]. */
  k_glcdc_stride_mask = 0xFFFFU, /**< FLM3.LNOFF is 16 bits.                   */
  k_glcdc_lnnum_mask  = 0x7FFU,  /**< FLM5.LNNUM is 11 bits.                   */
} glcdc_decode_t;

/* Run-state telemetry the board view shows (updated by the run loop each
 * present): the current PC, the emulation-chunk counter, and whether the run
 * loop is still live (set false once the run parks / faults / exits). */
static uint32_t s_view_pc;
static uint32_t s_view_chunks;
static bool     s_view_running = true;
/* Console scrollback state, Arduino-Serial-Monitor style. s_view_scroll is the
 * offset in lines back from the newest line (0 = the live tail). When
 * s_view_autoscroll is true the view follows the tail (offset pinned to 0);
 * scrolling up pauses autoscroll and the view holds its ABSOLUTE lines as new
 * output arrives (the offset is advanced by each frame's new-line delta, tracked
 * via s_view_log_seen). Scrolling back down to the tail re-enables autoscroll. */
static uint32_t s_view_scroll;
static bool     s_view_autoscroll = true;
static uint32_t s_view_log_seen;
/* Active console tab: which board_console channel the console panel shows. The
 * tab bar (ALL | UART | ITM | SPI | I2C) switches it on a click; switching
 * resets the scrollback to the live tail of the newly selected channel. */
static board_console_ch_t s_view_console_ch = k_board_console_ch_all;

/** @brief Which core the firmware targets: gates the M85-only instruction seams. */
typedef enum : uint8_t {
  k_core_m85 = 0U, /**< Cortex-M85 primary (default): MVE/long-shift seams armed.    */
  k_core_m33 = 1U, /**< Cortex-M33 primary: the M85-only instruction seams stay off. */
} board_primary_core_t;

/* Core-control state (#152 dual-core CLI/GUI asks). s_primary_core gates the
 * M85-only instruction seams and relabels telemetry; s_low_power shrinks the
 * run-chunk budget to model the M33's 4:1-slower clock (Model A), and the GUI
 * low-power button + the --low-power flag both drive it. */
static board_primary_core_t s_primary_core = k_core_m85;
static bool                 s_low_power    = false;

/**
 * @brief Fallback NS vector-table base for the BLXNS world switch.
 * @details ::on_blxns first reads the live VTOR_NS word (the Secure boot's
 *          ``ra8_tz_secure_boot_jump_ns`` stores the NS vector base to the
 *          0xE002ED08 alias -- plain PPB RAM here -- right before its BLXNS),
 *          so a single-image TZ app whose NS half lives at its MRAM LMA
 *          (cpu1_pingpong_ipc: 0x02080000) and a two-image app whose NS image
 *          was copied to the SRAM2 run alias both resolve without flags. This
 *          fallback covers a zero VTOR_NS: it defaults to the RAM-resident NS
 *          run alias (@ref k_ns_sram2_base) and is overridden at `--ns` load
 *          to the loaded NS image's minimum PT_LOAD p_vaddr, so an XIP NS
 *          image linked at the OSPI window (0x90000000, VMA == LMA, no copy)
 *          still transitions correctly.
 * @note Single-threaded; set once before the run loop and read in ::on_blxns.
 * @since 0.1.0
 */
static uint32_t s_ns_vector_base = (uint32_t)k_ns_sram2_base;

/** @brief SCB VTOR_NS alias address (ARMv8-M B3.2.4; Secure-state view). */
typedef enum : uint64_t {
  k_scb_vtor_ns_addr = 0xE002ED08UL, /**< VTOR_NS: NS vector-table base. */
} vtor_ns_addr_t;

/**
 * @enum blxns_op_t
 * @brief Thumb encoding of the BLXNS instruction (scanned in jump_ns).
 * @details BLXNS Rm = 0x4780 | (Rm << 3) | 0x04; masking with k_blxns_mask
 *          isolates the fixed bits (0x4784) so any Rm matches.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_blxns_mask  = 0xFF87U, /**< Mask isolating the BLXNS fixed bits. */
  k_blxns_match = 0x4784U, /**< BLXNS fixed-bit pattern (any Rm).    */
} blxns_op_t;

/**
 * @brief UC_HOOK_CODE at the Secure->NS BLXNS -- hand-emulate the world switch.
 *
 * @details Unicorn's emulated M33 is all-Secure with no IDAU, so the real BLXNS
 *          in ra8_tz_secure_boot_jump_ns cannot transition to the Non-Secure
 *          world (it stalls / wanders). This hook fires on that instruction and
 *          performs the switch by hand in board_sim's single flat domain: it
 *          reads the NS initial MSP (NS vector[0]) and the NS reset handler (NS
 *          vector[1]) from the NS run base, sets SP + PC to them (Thumb bit
 *          masked), and stops the chunk so the run loop resumes executing the NS
 *          reset handler -- ThreadX and the e-reader threads then run directly.
 *          Mirrors the existing SG-stub-by-address TrustZone workaround.
 *
 * @param[in] uc      Unicorn engine mid-chunk at the BLXNS.
 * @param[in] address The BLXNS instruction address; unused.
 * @param[in] size    Instruction size in bytes; unused.
 * @param[in] user    Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre The NS vectors are live at @ref s_ns_vector_base -- either copied to the
 *      RAM run alias by the Secure boot, or XIP-resident in the OSPI window.
 * @pre The hook is registered only for the jump_ns BLXNS site (under --ns).
 * @post SP = NS MSP, PC = NS reset handler, and the chunk is stopped.
 * @post The next run-loop chunk executes the Non-Secure reset handler.
 * @note Not thread-safe; board_sim is single-threaded.
 * @since 0.1.0
 */
static void on_blxns(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  /* Prefer the live VTOR_NS: ra8_tz_secure_boot_jump_ns stores the NS vector
   * base to the 0xE002ED08 alias (plain PPB RAM here) right before its BLXNS,
   * so this resolves the NS table wherever the app placed it (MRAM-resident
   * 0x02080000, SRAM2 run alias 0x32100000, or OSPI XIP) with no per-app
   * knowledge. Fall back to ::s_ns_vector_base when the app never wrote it. */
  uint32_t vector_base = 0U;
  (void)uc_mem_read(uc, (uint64_t)k_scb_vtor_ns_addr, &vector_base, sizeof(vector_base));
  if (vector_base == 0U) {
    vector_base = s_ns_vector_base;
  }
  uint32_t ns_msp   = 0U;
  uint32_t ns_reset = 0U;
  (void)uc_mem_read(uc, (uint64_t)vector_base, &ns_msp, sizeof(ns_msp));
  (void)uc_mem_read(uc,
                    (uint64_t)vector_base + (uint64_t)sizeof(uint32_t),
                    &ns_reset,
                    sizeof(ns_reset));
  const uint32_t ns_pc = ns_reset & ~1U; /* mask the Thumb bit for the PC write */
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &ns_msp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &ns_pc);
  (void)uc_emu_stop(uc);
}

/* ============================================================================
 * Virtual USB host-mode device: a HID boot keyboard behind the ra8_usb_host_*
 * seam.
 *
 * board_usb.c models the USBFS controller in DEVICE mode (a virtual host drives
 * the firmware's device stack). The inverse case -- the firmware acting as USB
 * HOST -- drives the USBHS controller (0x40351000, unmodelled) through the
 * first-party `ra8_usb_host_*` primitives, and with no peer the control transfer
 * wedges (SUREQ never acked -> k_ra8_err_busy). Rather than model a second
 * controller register-by-register, seam those primitives the same way ra8_eth_*
 * is seamed to a virtual net peer (the register model "cannot satisfy" that
 * sequence either): present a virtual boot keyboard that answers chapter-9
 * enumeration and streams interrupt-IN reports. This lets a host example
 * (usb_host_keyboard) enumerate + read reports end to end with no hardware --
 * validating the host stack's control/data logic, not silicon timing.
 * ==========================================================================*/

/** @brief bRequest / descriptor-type / sizing constants for the virtual device. */
typedef enum : uint16_t {
  k_vkbd_breq_get_descriptor = 0x06U, /**< Standard GET_DESCRIPTOR bRequest.       */
  k_vkbd_dt_device           = 0x01U, /**< DEVICE descriptor (wValue hi byte).     */
  k_vkbd_dt_config           = 0x02U, /**< CONFIGURATION descriptor.               */
  k_vkbd_dt_string           = 0x03U, /**< STRING descriptor.                      */
  k_vkbd_dt_hid_report       = 0x22U, /**< HID REPORT descriptor.                  */
  k_vkbd_lnst_attached       = 0x02U, /**< SYSSTS0.LNST J-state (device on bus).   */
  k_vkbd_report_len          = 8U,    /**< Boot-keyboard input report width.       */
  k_vkbd_num_keys            = 5U,    /**< Keycodes typed ("R A 8 D 2").           */
  k_vkbd_dev_desc_len        = 18U,   /**< DEVICE descriptor length.               */
  k_vkbd_cfg_desc_len        = 34U,   /**< Full CONFIGURATION descriptor length.   */
  k_vkbd_stop_reports        = 8U,    /**< Reports streamed before USB_STOP fires. */
} vkbd_const_t;

/** @brief 18-byte DEVICE descriptor: class defined at interface, EP0 MPS 64. */
static const uint8_t k_vkbd_device_desc[k_vkbd_dev_desc_len] = {
  0x12,
  0x01,
  0x00,
  0x02,
  0x00,
  0x00,
  0x00,
  0x40, /* len,DEVICE,bcdUSB2.00,class0,MPS64 */
  0x6A,
  0x1A,
  0x88,
  0x42,
  0x00,
  0x01,
  0x00,
  0x00, /* idVendor 0x1A6A, idProduct 0x4288 */
  0x00,
  0x01, /* bcdDevice, iM/iP/iS=0, 1 config */
};

/** @brief 34-byte CONFIGURATION: 1 HID boot-keyboard iface, 1 interrupt-IN EP1. */
static const uint8_t k_vkbd_config_desc[k_vkbd_cfg_desc_len] = {
  0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32, /* CONFIG: wTotalLen 34, 1 iface */
  0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00, /* IFACE: HID, boot, keyboard    */
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00, /* HID: report desc len 0x3F     */
  0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,             /* EP1 IN, interrupt, MPS64, 1ms */
};

/** @brief Standard boot-keyboard HID REPORT descriptor (63 bytes, USB HID 1.11 E.6). */
static const uint8_t k_vkbd_report_desc[63] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
  0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
  0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
  0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

/** @brief HID Usage-Table keycodes (Usage Page 0x07) for the typed string. */
typedef enum : uint8_t {
  k_vkbd_key_r = 0x15U, /**< HID usage for 'R'. */
  k_vkbd_key_a = 0x04U, /**< HID usage for 'A'. */
  k_vkbd_key_8 = 0x25U, /**< HID usage for '8'. */
  k_vkbd_key_d = 0x07U, /**< HID usage for 'D'. */
  k_vkbd_key_2 = 0x1FU, /**< HID usage for '2'. */
} vkbd_keycode_t;

/** @brief HID Usage-Table keycodes the virtual keyboard "types": R A 8 D 2. */
static const uint8_t k_vkbd_keycodes[k_vkbd_num_keys] = {k_vkbd_key_r,
                                                         k_vkbd_key_a,
                                                         k_vkbd_key_8,
                                                         k_vkbd_key_d,
                                                         k_vkbd_key_2};

static uint8_t  s_vkbd_seq           = 0U; /**< Rolling report seq (report byte 0). */
static uint32_t s_vkbd_ctrl_serviced = 0U; /**< Control transfers answered.         */
static uint32_t s_vkbd_reports_sent  = 0U; /**< Interrupt-IN reports streamed.      */

/** @brief Read the 5th (stack-passed) argument of an AAPCS call: mem32[SP]. */
static uint32_t usbh_arg5(uc_engine* uc)
{
  uint32_t sp = 0U;
  uint32_t p  = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_SP, &sp);
  (void)uc_mem_read(uc, (uint64_t)sp, &p, sizeof(p));
  return p;
}

/** @brief Hook a host primitive that just succeeds (init / reset / target / pipe). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_ok(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_host_line_state(): report the virtual device attached. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_line_state(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, (uint32_t)k_vkbd_lnst_attached);
}

/** @brief Hook ra8_usb_host_control_xfer(): answer chapter-9 from the virtual device. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_control_xfer(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t setup_ptr = 0U;
  uint32_t data_ptr  = 0U;
  uint32_t data_len  = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &setup_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &data_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &data_len);
  const uint32_t out_ptr = usbh_arg5(uc);

  uint8_t s[8] = {};
  (void)uc_mem_read(uc, (uint64_t)setup_ptr, s, sizeof(s));
  const uint8_t  b_request = s[1];
  const uint8_t  desc_type = s[3]; /* wValue high byte = descriptor type. */
  const uint8_t* src       = nullptr;
  uint16_t       src_len   = 0U;
  if (b_request == (uint8_t)k_vkbd_breq_get_descriptor) {
    if (desc_type == (uint8_t)k_vkbd_dt_device) {
      src     = k_vkbd_device_desc;
      src_len = (uint16_t)k_vkbd_dev_desc_len;
    } else if (desc_type == (uint8_t)k_vkbd_dt_config) {
      src     = k_vkbd_config_desc;
      src_len = (uint16_t)k_vkbd_cfg_desc_len;
    } else if (desc_type == (uint8_t)k_vkbd_dt_hid_report) {
      src     = k_vkbd_report_desc;
      src_len = (uint16_t)sizeof(k_vkbd_report_desc);
    }
  }
  uint16_t n = 0U;
  if ((src != nullptr) && (data_ptr != 0U)) {
    n = (src_len < (uint16_t)data_len) ? src_len : (uint16_t)data_len;
    (void)uc_mem_write(uc, (uint64_t)data_ptr, src, n);
  }
  /* No-data control writes (SET_ADDRESS / SET_CONFIGURATION / SET_IDLE /
   * SET_PROTOCOL) just leave n = 0; the host treats that as a successful ack. */
  if (out_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)out_ptr, &n, sizeof(n));
  }
  s_vkbd_ctrl_serviced++;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_host_bulk_in(): stream one boot-keyboard input report. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_usbh_bulk_in(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t buf     = 0U;
  uint32_t max_len = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &buf);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &max_len);
  const uint32_t out_ptr = usbh_arg5(uc);
  /* Boot-keyboard report: [seq][reserved 0][keycodes R A 8 D 2][0]. The host
   * ignores byte 0 and pattern-checks bytes 1.. -- it streams "RA8D2". */
  uint8_t rep[k_vkbd_report_len] = {};
  rep[0]                         = s_vkbd_seq++;
  for (uint32_t i = 0U; i < (uint32_t)k_vkbd_num_keys; i++) {
    rep[2U + i] = k_vkbd_keycodes[i];
  }
  uint16_t n = (uint16_t)k_vkbd_report_len;
  if ((uint32_t)n > max_len) {
    n = (uint16_t)max_len;
  }
  if (buf != 0U) {
    (void)uc_mem_write(uc, (uint64_t)buf, rep, n);
  }
  if (out_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)out_ptr, &n, sizeof(n));
  }
  s_vkbd_reports_sent++;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/* ----------------------------------------------------------------------------
 * Virtual USB host-mode MSC device: a read-only FAT16 disk whose one file
 * MRAM.BIN is the 1 MiB MRAM code window. Seams the first-party `ra8_usb_hmsc_*`
 * class API (one level above the BOT/SCSI bulk transport) so a host MSC app
 * (usb_host_msc_browse) enumerates, READ_CAPACITYs, mounts the FAT16, browses
 * the root directory, and content-verifies MRAM.BIN -- all with no peer device.
 * The boot/FAT/root sectors are a byte-identical replica of the device's
 * selftest_fat_fill_sector; the data region is read live from emulated MRAM, so
 * it matches the host's own MRAM compare byte-for-byte.
 * --------------------------------------------------------------------------*/

/** @brief FAT16 geometry + boot/dir layout for the virtual MSC volume. */
typedef enum : uint32_t {
  k_vmsc_block_size     = 512U,        /**< Logical block size.                   */
  k_vmsc_total_sectors  = 4146U,       /**< 1 reserved + 17 FAT + 32 root + 4096. */
  k_vmsc_root_lba       = 18U,         /**< First root-directory LBA.             */
  k_vmsc_data_lba       = 50U,         /**< First data-region LBA (cluster 2).    */
  k_vmsc_first_cluster  = 2U,          /**< FAT data area starts at cluster 2.    */
  k_vmsc_last_mram_clus = 2049U,       /**< Last cluster of MRAM.BIN.             */
  k_vmsc_entries_per_fs = 256U,        /**< FAT16 entries per 512-byte sector.    */
  k_vmsc_mram_base      = 0x02000000U, /**< MRAM window base (MRAM.BIN data).     */
  k_vmsc_fat_entry0     = 0xFFF8U,     /**< FAT[0]: media F8 + filler.            */
  k_vmsc_fat_eoc        = 0xFFFFU,     /**< End-of-chain marker.                  */
  k_vmsc_file_bytes     = 0x00100000U, /**< MRAM.BIN size: 1 MiB.                 */
  k_vmsc_volid          = 0x52A8D20AU, /**< Boot-sector volume serial.            */
} vmsc_const_t;

/** @brief FAT16 BPB byte offsets, fixed field values, and store shifts. */
typedef enum : uint32_t {
  k_bpb_shift8         = 8U,    /**< Byte 1 store shift.                */
  k_bpb_shift16        = 16U,   /**< Byte 2 store shift.                */
  k_bpb_shift24        = 24U,   /**< Byte 3 store shift.                */
  k_bpb_jmp0           = 0xEBU, /**< BS_jmpBoot[0]: short jump opcode.  */
  k_bpb_jmp1           = 0x3CU, /**< BS_jmpBoot[1]: jump displacement.  */
  k_bpb_jmp2           = 0x90U, /**< BS_jmpBoot[2]: NOP.                */
  k_bpb_off_oem        = 3U,    /**< BS_OEMName offset.                 */
  k_bpb_off_bytspersec = 11U,   /**< BPB_BytsPerSec offset.             */
  k_bpb_off_secperclus = 13U,   /**< BPB_SecPerClus offset.             */
  k_bpb_off_rsvdseccnt = 14U,   /**< BPB_RsvdSecCnt offset.             */
  k_bpb_off_numfats    = 16U,   /**< BPB_NumFATs offset.                */
  k_bpb_off_rootentcnt = 17U,   /**< BPB_RootEntCnt offset.             */
  k_bpb_off_totsec16   = 19U,   /**< BPB_TotSec16 offset.               */
  k_bpb_off_media      = 21U,   /**< BPB_Media offset.                  */
  k_bpb_off_fatsz16    = 22U,   /**< BPB_FATSz16 offset.                */
  k_bpb_off_secpertrk  = 24U,   /**< BPB_SecPerTrk offset.              */
  k_bpb_off_numheads   = 26U,   /**< BPB_NumHeads offset.               */
  k_bpb_off_drvnum     = 36U,   /**< BS_DrvNum offset.                  */
  k_bpb_off_bootsig    = 38U,   /**< BS_BootSig offset.                 */
  k_bpb_off_volid      = 39U,   /**< BS_VolID offset.                   */
  k_bpb_off_vollab     = 43U,   /**< BS_VolLab offset.                  */
  k_bpb_off_filsystype = 54U,   /**< BS_FilSysType offset.              */
  k_bpb_off_sig0       = 510U,  /**< 0x55 signature byte.               */
  k_bpb_off_sig1       = 511U,  /**< 0xAA signature byte.               */
  k_bpb_secperclus_1   = 1U,    /**< 1 sector per cluster.              */
  k_bpb_rsvdseccnt_1   = 1U,    /**< 1 reserved sector.                 */
  k_bpb_numfats_1      = 1U,    /**< 1 FAT copy.                        */
  k_bpb_rootentcnt_512 = 512U,  /**< 512 root-directory entries.        */
  k_bpb_media_f8       = 0xF8U, /**< Fixed-disk media descriptor.       */
  k_bpb_fatsz16_17     = 17U,   /**< 17 sectors per FAT.                */
  k_bpb_secpertrk_32   = 32U,   /**< 32 sectors per track.              */
  k_bpb_numheads_16    = 16U,   /**< 16 heads.                          */
  k_bpb_drvnum_80      = 0x80U, /**< Drive number (first fixed disk).   */
  k_bpb_bootsig_29     = 0x29U, /**< Extended boot signature.           */
  k_bpb_sig0_55        = 0x55U, /**< Boot-sector signature byte 0.      */
  k_bpb_sig1_aa        = 0xAAU, /**< Boot-sector signature byte 1.      */
  k_dir_off_attr       = 11U,   /**< Directory-entry attribute byte.    */
  k_dir_off_entry      = 32U,   /**< Second 32-byte directory entry.    */
  k_dir_off_fstcluslo  = 26U,   /**< DIR_FstClusLO offset within entry. */
  k_dir_off_filesize   = 28U,   /**< DIR_FileSize offset within entry.  */
  k_dir_attr_vollabel  = 0x08U, /**< ATTR_VOLUME_ID.                    */
  k_dir_attr_readonly  = 0x01U, /**< ATTR_READ_ONLY.                    */
} vmsc_bpb_t;

static const uint8_t k_vmsc_oem[8]    = {'R', 'A', '8', 'D', '2', 'F', 'W', ' '};
static const uint8_t k_vmsc_label[11] = {'R', 'A', '8', 'D', '2', ' ', 'M', 'R', 'A', 'M', ' '};
static const uint8_t k_vmsc_fstype[8] = {'F', 'A', 'T', '1', '6', ' ', ' ', ' '};
static const uint8_t k_vmsc_fname[11] = {'M', 'R', 'A', 'M', ' ', ' ', ' ', ' ', 'B', 'I', 'N'};

/** @brief Set once the host attempts a WRITE(10) into the READ-ONLY disk -- the
 *  last host step before usb_host_msc_browse's PASS (read by the USBH_STOP guard).*/
static bool s_vmsc_write_seen = false;

/** @brief True when the virtual disk is writable (usb_host_file_ops links
 *  `fileops_backend_write`); else the disk is read-only and WRITE(10) is rejected.*/
static bool s_vmsc_writable = false;

/**
 * @struct vmsc_overlay_t
 * @brief One overwritten sector of the otherwise-synthesized FAT16 volume.
 * @details A writable host (usb_host_file_ops) creates a file: ra8_fs rewrites a
 * few FAT / root / data sectors. We keep those writes in a small overlay so the
 * read-back reads them back; everything else is still synthesized on the fly.
 */
typedef struct {
  uint32_t lba;                     /**< Overwritten LBA.             */
  bool     valid;                   /**< Slot in use.                 */
  uint8_t  data[k_vmsc_block_size]; /**< The written 512-byte sector. */
} vmsc_overlay_t;

/** @brief Write overlay for the writable disk (file_ops touches only a handful). */
static vmsc_overlay_t s_vmsc_overlay[64];

/** @brief Return an overwritten sector if @p lba is in the overlay. */
static bool vmsc_overlay_get(uint32_t lba, uint8_t* out)
{
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(s_vmsc_overlay) / sizeof(s_vmsc_overlay[0])); i++) {
    if (s_vmsc_overlay[i].valid && (s_vmsc_overlay[i].lba == lba)) {
      (void)memcpy(out, s_vmsc_overlay[i].data, (size_t)k_vmsc_block_size);
      return true;
    }
  }
  return false;
}

/** @brief Record an overwritten sector (update existing slot or take a free one). */
static void vmsc_overlay_put(uint32_t lba, const uint8_t* in)
{
  const uint32_t slots = (uint32_t)(sizeof(s_vmsc_overlay) / sizeof(s_vmsc_overlay[0]));
  for (uint32_t i = 0U; i < slots; i++) {
    if (s_vmsc_overlay[i].valid && (s_vmsc_overlay[i].lba == lba)) {
      (void)memcpy(s_vmsc_overlay[i].data, in, (size_t)k_vmsc_block_size);
      return;
    }
  }
  for (uint32_t i = 0U; i < slots; i++) {
    if (!s_vmsc_overlay[i].valid) {
      s_vmsc_overlay[i].lba   = lba;
      s_vmsc_overlay[i].valid = true;
      (void)memcpy(s_vmsc_overlay[i].data, in, (size_t)k_vmsc_block_size);
      return;
    }
  }
}

/** @brief Little-endian 16-bit store into a sector buffer. */
static void vmsc_put16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & (uint16_t)k_byte_mask);
  p[1] = (uint8_t)((v >> (uint16_t)k_bpb_shift8) & (uint16_t)k_byte_mask);
}

/** @brief Little-endian 32-bit store into a sector buffer. */
static void vmsc_put32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & (uint32_t)k_byte_mask);
  p[1] = (uint8_t)((v >> (uint32_t)k_bpb_shift8) & (uint32_t)k_byte_mask);
  p[2] = (uint8_t)((v >> (uint32_t)k_bpb_shift16) & (uint32_t)k_byte_mask);
  p[3] = (uint8_t)((v >> (uint32_t)k_bpb_shift24) & (uint32_t)k_byte_mask);
}

/** @brief Synthesize the FAT16 boot sector (BPB), mirroring the device side. */
static void vmsc_fill_boot(uint8_t* out)
{
  out[0] = (uint8_t)k_bpb_jmp0;
  out[1] = (uint8_t)k_bpb_jmp1;
  out[2] = (uint8_t)k_bpb_jmp2; /* jmp + nop */
  (void)memcpy(&out[k_bpb_off_oem], k_vmsc_oem, sizeof(k_vmsc_oem));
  vmsc_put16(&out[k_bpb_off_bytspersec], (uint16_t)k_vmsc_block_size);
  out[k_bpb_off_secperclus] = (uint8_t)k_bpb_secperclus_1;                /* sectors/cluster  */
  vmsc_put16(&out[k_bpb_off_rsvdseccnt], (uint16_t)k_bpb_rsvdseccnt_1);   /* reserved sectors */
  out[k_bpb_off_numfats] = (uint8_t)k_bpb_numfats_1;                      /* number of FATs   */
  vmsc_put16(&out[k_bpb_off_rootentcnt], (uint16_t)k_bpb_rootentcnt_512); /* root entries     */
  vmsc_put16(&out[k_bpb_off_totsec16], (uint16_t)k_vmsc_total_sectors);
  out[k_bpb_off_media] = (uint8_t)k_bpb_media_f8;                      /* media descriptor   */
  vmsc_put16(&out[k_bpb_off_fatsz16], (uint16_t)k_bpb_fatsz16_17);     /* sectors per FAT    */
  vmsc_put16(&out[k_bpb_off_secpertrk], (uint16_t)k_bpb_secpertrk_32); /* sectors per track  */
  vmsc_put16(&out[k_bpb_off_numheads], (uint16_t)k_bpb_numheads_16);   /* heads              */
  out[k_bpb_off_drvnum]  = (uint8_t)k_bpb_drvnum_80;                   /* drive number       */
  out[k_bpb_off_bootsig] = (uint8_t)k_bpb_bootsig_29;                  /* ext boot signature */
  vmsc_put32(&out[k_bpb_off_volid], (uint32_t)k_vmsc_volid);
  (void)memcpy(&out[k_bpb_off_vollab], k_vmsc_label, sizeof(k_vmsc_label));
  (void)memcpy(&out[k_bpb_off_filsystype], k_vmsc_fstype, sizeof(k_vmsc_fstype));
  out[k_bpb_off_sig0] = (uint8_t)k_bpb_sig0_55;
  out[k_bpb_off_sig1] = (uint8_t)k_bpb_sig1_aa;
}

/** @brief Synthesize one FAT sector: MRAM.BIN chains clusters 2..2049. */
static void vmsc_fill_fat(uint32_t fat_sector, uint8_t* out)
{
  const uint32_t first = fat_sector * (uint32_t)k_vmsc_entries_per_fs;
  for (uint32_t j = 0U; j < (uint32_t)k_vmsc_entries_per_fs; j++) {
    const uint32_t entry = first + j;
    uint16_t       value = 0U;
    if (entry == 0U) {
      value = (uint16_t)k_vmsc_fat_entry0;
    } else if (entry == 1U) {
      value = (uint16_t)k_vmsc_fat_eoc;
    } else if (entry < (uint32_t)k_vmsc_last_mram_clus) {
      value = (uint16_t)(entry + 1U);
    } else if (entry == (uint32_t)k_vmsc_last_mram_clus) {
      value = (uint16_t)k_vmsc_fat_eoc;
    }
    vmsc_put16(&out[j * 2U], value);
  }
}

/** @brief Synthesize root-directory sector 0: volume label + MRAM.BIN entry. */
static void vmsc_fill_root(uint32_t root_sector, uint8_t* out)
{
  if (root_sector != 0U) {
    return;
  }
  (void)memcpy(&out[0], k_vmsc_label, sizeof(k_vmsc_label));
  out[k_dir_off_attr] = (uint8_t)k_dir_attr_vollabel; /* volume-label attribute */
  uint8_t* e          = &out[k_dir_off_entry];
  (void)memcpy(e, k_vmsc_fname, sizeof(k_vmsc_fname));
  e[k_dir_off_attr] = (uint8_t)k_dir_attr_readonly; /* read-only attribute */
  vmsc_put16(&e[k_dir_off_fstcluslo], (uint16_t)k_vmsc_first_cluster);
  vmsc_put32(&e[k_dir_off_filesize], (uint32_t)k_vmsc_file_bytes);
}

/** @brief Fill one 512-byte volume sector (boot / FAT / root / live MRAM data). */
static void vmsc_fill_sector(uc_engine* uc, uint32_t lba, uint8_t* out)
{
  (void)memset(out, 0, (size_t)k_vmsc_block_size);
  if (lba == 0U) {
    vmsc_fill_boot(out);
  } else if (lba < (uint32_t)k_vmsc_root_lba) {
    vmsc_fill_fat(lba - 1U, out);
  } else if (lba < (uint32_t)k_vmsc_data_lba) {
    vmsc_fill_root(lba - (uint32_t)k_vmsc_root_lba, out);
  } else {
    const uint32_t cluster = (lba - (uint32_t)k_vmsc_data_lba) + (uint32_t)k_vmsc_first_cluster;
    if (cluster <= (uint32_t)k_vmsc_last_mram_clus) {
      const uint32_t off = (cluster - (uint32_t)k_vmsc_first_cluster) * (uint32_t)k_vmsc_block_size;
      (void)
        uc_mem_read(uc, (uint64_t)k_vmsc_mram_base + (uint64_t)off, out, (size_t)k_vmsc_block_size);
    }
  }
}

/** @brief Hook a host MSC primitive that just succeeds (init / close). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_ok(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief ra8_usb_hmsc_device_t field offsets + reported bulk EP packet/VID/PID. */
typedef enum : uint32_t {
  k_hmsc_off_vid    = 10U,     /**< vid offset in ra8_usb_hmsc_device_t.    */
  k_hmsc_off_pid    = 12U,     /**< pid offset in ra8_usb_hmsc_device_t.    */
  k_hmsc_bulk_mps   = 64U,     /**< Reported bulk-endpoint max packet size. */
  k_hmsc_vendor_id  = 0x1A6AU, /**< Reported USB vendor_id.                 */
  k_hmsc_product_id = 0x4288U, /**< Reported USB product_id.                */
} hmsc_dev_t;

/** @brief Hook ra8_usb_hmsc_enumerate(out_device*): report the virtual disk. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_enumerate(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t dev_ptr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &dev_ptr);
  if (dev_ptr != 0U) {
    /* ra8_usb_hmsc_device_t: addr,bin_ep,bout_ep,max_lun,iface,[pad],in_mps,
     * out_mps,vid,pid. */
    uint8_t d[14] = {};
    d[0]          = 1U;                                          /* device_address      */
    d[1]          = 1U;                                          /* bulk_in_ep          */
    d[2]          = 2U;                                          /* bulk_out_ep         */
    vmsc_put16(&d[6], (uint16_t)k_hmsc_bulk_mps);                /* bulk_in_max_packet  */
    vmsc_put16(&d[8], (uint16_t)k_hmsc_bulk_mps);                /* bulk_out_max_packet */
    vmsc_put16(&d[k_hmsc_off_vid], (uint16_t)k_hmsc_vendor_id);  /* vendor_id           */
    vmsc_put16(&d[k_hmsc_off_pid], (uint16_t)k_hmsc_product_id); /* product_id          */
    (void)uc_mem_write(uc, (uint64_t)dev_ptr, d, sizeof(d));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_read_capacity(lun, *block_count, *block_size). */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_read_capacity(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t bc_ptr = 0U;
  uint32_t bs_ptr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &bc_ptr);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &bs_ptr);
  const uint32_t block_count = (uint32_t)k_vmsc_total_sectors;
  const uint32_t block_size  = (uint32_t)k_vmsc_block_size;
  if (bc_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)bc_ptr, &block_count, sizeof(block_count));
  }
  if (bs_ptr != 0U) {
    (void)uc_mem_write(uc, (uint64_t)bs_ptr, &block_size, sizeof(block_size));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_read10(lun, lba, count, out_buf): serve sectors. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_read10(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t lba   = 0U;
  uint32_t count = 0U;
  uint32_t buf   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &lba);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &count);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &buf);
  count &= (uint32_t)k_lo16_mask; /* block_count is a uint16_t argument. */
  for (uint32_t i = 0U; (i < count) && (buf != 0U); i++) {
    uint8_t sec[k_vmsc_block_size];
    if (!vmsc_overlay_get(lba + i, sec)) { /* a host WRITE(10) wins over the synthesis. */
      vmsc_fill_sector(uc, lba + i, sec);
    }
    (void)uc_mem_write(uc,
                       (uint64_t)buf + ((uint64_t)i * (uint64_t)k_vmsc_block_size),
                       sec,
                       sizeof(sec));
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok */
}

/** @brief Hook ra8_usb_hmsc_write10(): reject -- the volume is read-only. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_hmsc_write10(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  if (!s_vmsc_writable) {
    /* Read-only disk (usb_host_msc_browse): reject. This is that app's last step
     * before PASS, so flag it for the host early-stop. */
    s_vmsc_write_seen = true;
    eth_hook_return(uc, (uint32_t)k_ra8_err_inval_st); /* write protected */
    return;
  }
  /* Writable disk (usb_host_file_ops): stash the written sectors in the overlay
   * so the host's read-back sees them, and accept. The write is mid-ladder here,
   * so do NOT trip the write-seen early-stop -- that app stops on its banner. */
  uint32_t lba   = 0U;
  uint32_t count = 0U;
  uint32_t buf   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &lba);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &count);
  (void)uc_reg_read(uc, UC_ARM_REG_R3, &buf);
  count &= (uint32_t)k_lo16_mask;
  for (uint32_t i = 0U; (i < count) && (buf != 0U); i++) {
    uint8_t sec[k_vmsc_block_size];
    (void)uc_mem_read(uc,
                      (uint64_t)buf + ((uint64_t)i * (uint64_t)k_vmsc_block_size),
                      sec,
                      sizeof(sec));
    vmsc_overlay_put(lba + i, sec);
  }
  eth_hook_return(uc, 0U); /* k_ra8_ok -- write accepted */
}

/**
 * @brief Install the virtual USB host-mode device seam if the host stack is linked.
 *
 * @details Picks the virtual device class from the firmware's linked host stack:
 * an MSC host (links `ra8_usb_hmsc_read10`) gets a read-only FAT16 disk seamed at
 * the `ra8_usb_hmsc_*` class API; otherwise a USB-host-capable firmware (links
 * `ra8_usb_host_control_xfer`) gets a HID boot keyboard seamed at the
 * `ra8_usb_host_*` primitives. Device-mode apps link neither call path, so the
 * hooks are inert there and board_usb.c's device-mode virtual host is untouched.
 *
 * @param[in,out] uc  Active Unicorn engine.
 * @param[in]     elf Loaded ELF image (symbol resolution).
 * @param[in]     len ELF image length in bytes.
 *
 * @pre @p uc is initialised and @p elf holds @p len bytes.
 * @post On a host app, the linked host API answers a virtual device.
 *
 * @note No effect on device-mode apps (the hooked symbols are never called).
 * @return true when a seam family was installed (the register-level USBHS
 *         host model must then stay dormant -- see board_usb_host.h).
 * @retval true  hmsc- or primitive-level seams now shadow the host API.
 * @retval false No usb-host seams; the register path is the real one.
 * @since 0.1.0
 */
static bool usbh_seam_install(uc_engine* uc, const uint8_t* elf, long len)
{
  const uint32_t msc = elf_sym_addr(elf, len, "ra8_usb_hmsc_read10", nullptr);
  if (msc != 0U) {
    /* usb_host_file_ops creates a file (it links fileops_backend_write) -> the
     * virtual disk is writable; usb_host_msc_browse tests a read-only LUN. */
    s_vmsc_writable = (elf_sym_addr(elf, len, "fileops_backend_write", nullptr) != 0U);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_init", (void*)on_hmsc_ok);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_enumerate", (void*)on_hmsc_enumerate);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_read_capacity", (void*)on_hmsc_read_capacity);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_read10", (void*)on_hmsc_read10);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_write10", (void*)on_hmsc_write10);
    eth_seam_hook(uc, elf, len, "ra8_usb_hmsc_close", (void*)on_hmsc_ok);
    (void)fprintf(stderr,
                  "  usb-host seam : hmsc=0x%08X (virtual MSC FAT16 disk, file MRAM.BIN, %s)\n",
                  msc,
                  s_vmsc_writable ? "read-write" : "read-only");
    return true;
  }
  const uint32_t cx = elf_sym_addr(elf, len, "ra8_usb_host_control_xfer", nullptr);
  if (cx == 0U) {
    return false; /* not a USB-host-capable firmware -- nothing to seam. */
  }
  eth_seam_hook(uc, elf, len, "ra8_usb_host_line_state", (void*)on_usbh_line_state);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_control_xfer", (void*)on_usbh_control_xfer);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_bulk_in", (void*)on_usbh_bulk_in);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_bus_reset", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_set_uact", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_set_target", (void*)on_usbh_ok);
  eth_seam_hook(uc, elf, len, "ra8_usb_host_pipe_setup", (void*)on_usbh_ok);
  (void)fprintf(stderr,
                "  usb-host seam : control=0x%08X (virtual HID boot keyboard \"RA8D2\")\n",
                cx);
  return true;
}

/**
 * @enum colour_pack_t
 * @brief Field masks/shifts for 24-bit RGB888 <-> 16-bit RGB565 packing.
 */
typedef enum : uint32_t {
  k_rgb888_r_shift = 16U,         /**< Red byte position in 0x00RRGGBB.         */
  k_rgb888_g_shift = 8U,          /**< Green byte position in 0x00RRGGBB.       */
  k_rgb565_r5_keep = 0xF8U,       /**< Top 5 bits of an 8-bit red channel.      */
  k_rgb565_g6_keep = 0xFCU,       /**< Top 6 bits of an 8-bit green channel.    */
  k_rgb565_r_pos   = 8U,          /**< Red field shift when packing RGB565.     */
  k_rgb565_g_pos   = 3U,          /**< Green field shift when packing RGB565.   */
  k_rgb565_b_drop  = 3U,          /**< Bits dropped from an 8-bit blue channel. */
  k_rgb888_mask    = 0x00FFFFFFU, /**< 24-bit colour (BG_BGC low bytes).        */
  k_rgb565_r_shift = 11U,         /**< Red field position in RGB565.            */
  k_rgb565_g_shift = 5U,          /**< Green field position in RGB565.          */
  k_rgb565_5bit    = 0x1FU,       /**< 5-bit channel mask (red / blue).         */
  k_rgb565_6bit    = 0x3FU,       /**< 6-bit channel mask (green).              */
} colour_pack_t;

/** @brief Pack a 0x00RRGGBB colour into RGB565. */
static uint16_t rgb888_to_565(uint32_t rgb)
{
  const uint32_t r = (rgb >> (uint32_t)k_rgb888_r_shift) & (uint32_t)k_byte_mask;
  const uint32_t g = (rgb >> (uint32_t)k_rgb888_g_shift) & (uint32_t)k_byte_mask;
  const uint32_t b = rgb & (uint32_t)k_byte_mask;
  return (uint16_t)(((r & (uint32_t)k_rgb565_r5_keep) << (uint32_t)k_rgb565_r_pos) |
                    ((g & (uint32_t)k_rgb565_g6_keep) << (uint32_t)k_rgb565_g_pos) |
                    (b >> (uint32_t)k_rgb565_b_drop));
}

/** @brief True if addr is in an emulated RAM region a framebuffer could use. */
static bool addr_is_ram(uint32_t addr)
{
  return (((addr >= (uint32_t)k_dtcm_base) && (addr < (uint32_t)k_dtcm_end)) ||
          ((addr >= (uint32_t)k_sram_base) && (addr < (uint32_t)k_sram_end)) ||
          ((addr >= (uint32_t)k_sdram_base) && (addr < (uint32_t)k_sdram_end)));
}

/**
 * @brief Build the current display frame (RGB565) from emulated GLCDC state.
 *
 * @details
 * Fills the buffer with the BG_BGC background colour (what lcd_color_cycle
 * scans out), then, if GR1 has an RGB565 framebuffer programmed in emulated
 * RAM, blits it over the top-left -- so apps that draw real pixels into a
 * graphics layer (e.g. display_pal_animation) show their actual content. The
 * GR1 base/stride/lines are read live each call, so a double-buffered or
 * animating app updates frame to frame.
 *
 * @param[in]  uc Unicorn engine (read-only here).
 * @param[out] fb RGB565 frame buffer of width*height pixels.
 * @param[in]  width_px  Frame width.
 * @param[in]  height_px Frame height.
 */
static void build_frame(uc_engine* uc, uint16_t* fb, uint16_t width_px, uint16_t height_px)
{
  /* Read GLCDC registers from the stable shadow (mmio_peek), never the guest-
   * facing toggling read -- otherwise a firmware that never programs the GLCDC
   * makes the panel strobe black<->white (see mmio_peek). */
  const uint16_t bg = rgb888_to_565(mmio_peek((uint64_t)k_glcdc_bg_bgc) & (uint32_t)k_rgb888_mask);
  const size_t   n  = (size_t)width_px * (size_t)height_px;
  for (size_t i = 0U; i < n; i++) {
    fb[i] = bg;
  }

  const uint32_t saddr = mmio_peek((uint64_t)k_glcdc_gr1_saddr);
  const uint32_t fmt   = (mmio_peek((uint64_t)k_glcdc_gr1_fmt) >> (uint32_t)k_glcdc_fmt_shift) &
                         (uint32_t)k_glcdc_fmt_mask;
  if (!addr_is_ram(saddr) || (fmt != (uint32_t)k_glcdc_fmt_rgb565)) {
    return; /* no graphics layer -- background-only frame */
  }
  const uint32_t stride = (mmio_peek((uint64_t)k_glcdc_gr1_flm3) >> (uint32_t)k_glcdc_high_shift) &
                          (uint32_t)k_glcdc_stride_mask;
  const uint32_t lnnum  = (mmio_peek((uint64_t)k_glcdc_gr1_flm5) >> (uint32_t)k_glcdc_high_shift) &
                          (uint32_t)k_glcdc_lnnum_mask;
  if (stride < 2U) {
    return;
  }
  const uint32_t fb_w = stride / 2U; /* RGB565: 2 bytes per pixel */
  const uint32_t fb_h = lnnum + 1U;
  const uint32_t cw   = (fb_w < (uint32_t)width_px) ? fb_w : (uint32_t)width_px;
  const uint32_t ch   = (fb_h < (uint32_t)height_px) ? fb_h : (uint32_t)height_px;
  for (uint32_t y = 0U; y < ch; y++) {
    (void)uc_mem_read(uc,
                      (uint64_t)saddr + ((uint64_t)y * (uint64_t)stride),
                      &fb[(size_t)y * (size_t)width_px],
                      (size_t)cw * sizeof(uint16_t));
  }
}

/** @brief Write an RGB565 frame to a binary PPM (P6) for headless inspection. */
static int write_ppm(const char* path, const uint16_t* fb, uint16_t width_px, uint16_t height_px)
{
  FILE* f = fopen(path, "wb"); /* alloc-allow: host dev tool, not firmware */
  if (f == nullptr) {
    return -1;
  }
  (void)fprintf(f, "P6\n%u %u\n255\n", (unsigned)width_px, (unsigned)height_px);
  const size_t n = (size_t)width_px * (size_t)height_px;
  for (size_t i = 0U; i < n; i++) {
    const uint16_t p      = fb[i];
    const uint32_t r5     = (uint32_t)((p >> (uint32_t)k_rgb565_r_shift) & (uint32_t)k_rgb565_5bit);
    const uint32_t g6     = (uint32_t)((p >> (uint32_t)k_rgb565_g_shift) & (uint32_t)k_rgb565_6bit);
    const uint32_t b5     = (uint32_t)(p & (uint32_t)k_rgb565_5bit);
    const uint8_t  rgb[3] = {(uint8_t)((r5 << 3) | (r5 >> 2)),
                             (uint8_t)((g6 << 2) | (g6 >> 4)),
                             (uint8_t)((b5 << 3) | (b5 >> 2))};
    (void)fwrite(rgb, 1U, 3U, f);
  }
  (void)fclose(f);
  return 0;
}

/**
 * @brief Snapshot the live peripheral state into a board-view status struct.
 *
 * @details
 * Reads the read-only board_periph / board_usb getters -- per-LED level + lit
 * colour, the USB device-state string, the last captured UART line, the NVIC
 * IRQ totals, and the last drained touch point -- so the overlay can render the
 * status sidebar without reaching into any model's internals.
 *
 * @param[out] st        Status struct to fill.
 * @param[in]  app_name  Window / app title to caption the sidebar with.
 * @return Nothing.
 */
static void fill_status(board_status_t* st, const char* app_name)
{
  static const char* const k_led_labels[k_overlay_led_count] = {"LED1", "LED2", "LED3"};
  *st                                                        = (board_status_t){};
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_led_count; i++) {
    st->leds[i].on    = board_periph_led_level((board_led_id_t)i) != 0U;
    st->leds[i].color = board_periph_led_color_rgb565((board_led_id_t)i);
    st->leds[i].label = k_led_labels[i];
  }
  st->usb_state = board_usb_state_string();
  st->uart_line = board_periph_uart_last_line();
  st->irq_total = board_periph_irq_total();
  st->irq0      = board_periph_irq_count(0U);
  st->irq1      = board_periph_irq_count(1U);
  st->has_touch = board_periph_touch_last(&st->touch_x, &st->touch_y);
  /* User switches are active-low: a held button reads its pin low. */
  st->sw1_pressed = !board_periph_gpio_get_input((uint8_t)k_sim_sw_port, (uint8_t)k_sim_sw1_pin);
  st->sw2_pressed = !board_periph_gpio_get_input((uint8_t)k_sim_sw_port, (uint8_t)k_sim_sw2_pin);
  board_periph_battery_get(&st->battery_soc, &st->battery_charging);
  st->low_power   = s_low_power;
  st->core_is_m33 = (s_primary_core == k_core_m33);
  st->app_name    = app_name;
  board_sd_info(&st->sd_attached, &st->sd_bytes, &st->sd_fat_bits, &st->sd_label);
  /* Tabbed console: each board_console channel (ALL | UART | ITM | SPI | I2C) is
   * a tab; the active one (s_view_console_ch) fills the console panel. Populate
   * the tab-bar metadata (names + live line counts + which tab is active) so the
   * overlay can draw the bar and hit-test clicks. */
  st->console_ch_count  = (uint32_t)k_board_console_ch_count;
  st->console_active_ch = (uint32_t)s_view_console_ch;
  for (uint32_t c = 0U; c < (uint32_t)k_board_console_ch_count; c++) {
    if (c >= (uint32_t)k_overlay_console_tabs_max) {
      break;
    }
    st->console_ch_name[c]        = board_console_name((board_console_ch_t)c);
    st->console_ch_count_lines[c] = board_console_count((board_console_ch_t)c);
  }
  /* Console window: copy up to k_overlay_console_rows lines from the active
   * channel's scrollback ring, starting s_view_scroll lines back from the
   * newest, so the mouse-wheel can page through history. console[0] is the
   * newest visible line (= ring line s_view_scroll). */
  const uint32_t avail = board_console_count(s_view_console_ch);
  const uint32_t total = board_console_total(s_view_console_ch);
  if (s_view_autoscroll) {
    s_view_scroll = 0U; /* follow the live tail */
  } else if (total > s_view_log_seen) {
    /* Paused: new lines pushed the tail forward, so advance the back-offset by
     * the same amount to keep the reader's current (absolute) lines on screen.
     * Capped at the ring depth -- lines older than the ring have aged out. */
    const uint32_t delta = total - s_view_log_seen;
    s_view_scroll += delta;
    if (s_view_scroll > (uint32_t)k_uart_log_max) {
      s_view_scroll = (uint32_t)k_uart_log_max;
    }
  }
  s_view_log_seen = total;
  const uint32_t max_scroll =
    (avail > (uint32_t)k_overlay_console_rows) ? (avail - (uint32_t)k_overlay_console_rows) : 0U;
  if (s_view_scroll > max_scroll) {
    s_view_scroll = max_scroll; /* re-clamp if history shrank or is shallow */
  }
  const uint32_t scroll = s_view_scroll;
  uint32_t       rows   = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_console_rows; i++) {
    const char* line = board_console_line(s_view_console_ch, scroll + i);
    if (line == nullptr) {
      break;
    }
    (void)snprintf(st->console[i], (size_t)k_overlay_line_cap, "%s", line);
    rows++;
  }
  st->console_count      = rows;
  st->console_scroll     = scroll;
  st->console_total      = avail;
  st->console_autoscroll = s_view_autoscroll;
  /* Run-state telemetry (set by the run loop before each present). */
  st->pc            = s_view_pc;
  st->chunks        = s_view_chunks;
  st->running       = s_view_running;
  st->mmio_reads    = sim_mmio_reads();
  st->mmio_writes   = sim_mmio_writes();
  st->uart_tx_total = board_periph_uart_tx_total();
}

/**
 * @brief Build the panel framebuffer, then compose it with the status sidebar.
 *
 * @details
 * Renders the GLCDC panel into @p panel_fb (so a display app's region stays
 * pixel-correct), snapshots the live peripheral state, and writes the full
 * composite (panel region + status sidebar) into @p composite -- the single
 * buffer that backs both the live window and the @c --ppm snapshot.
 *
 * @param[in,out] uc        Unicorn engine (read for the GLCDC framebuffer).
 * @param[out]    panel_fb  Panel RGB565 buffer (@p panel_w by @p panel_h).
 * @param[out]    composite Composite RGB565 buffer (overlay total dimensions).
 * @param[in]     panel_w   Panel width in pixels.
 * @param[in]     panel_h   Panel height in pixels.
 * @param[in]     app_name  Window / app title for the sidebar caption.
 * @return Nothing.
 */
/**
 * @enum panel_rotate_t
 * @brief Display rotation (degrees clockwise) applied to the panel for viewing.
 *
 * @details The firmware always renders at its compiled resolution; --rotate only
 * turns the emulated panel for display (e.g. a 1024x600 landscape app shown as a
 * 600x1024 portrait), so a vertically-mounted screen can be previewed. Clicks
 * are mapped back to native coordinates via ::unrotate_click.
 */
typedef enum : uint32_t {
  k_rotate_0   = 0U,   /**< Native orientation.             */
  k_rotate_90  = 90U,  /**< 90 deg clockwise (-> portrait). */
  k_rotate_180 = 180U, /**< Upside down.                    */
  k_rotate_270 = 270U, /**< 90 deg counter-clockwise.       */
} panel_rotate_t;

/** @brief Rotate a row-major RGB565 panel (@p sw x @p sh) into @p dst, @p deg CW. */
static void rotate_panel(const uint16_t* src, uint16_t sw, uint16_t sh, uint16_t* dst, uint32_t deg)
{
  for (uint16_t y = 0U; y < sh; y++) {
    for (uint16_t x = 0U; x < sw; x++) {
      const uint16_t p  = src[(size_t)y * (size_t)sw + (size_t)x];
      uint32_t       dx = x;
      uint32_t       dy = y;
      uint32_t       dw = sw;
      if (deg == (uint32_t)k_rotate_90) {
        dx = (uint32_t)(sh - 1U - y);
        dy = x;
        dw = sh;
      } else if (deg == (uint32_t)k_rotate_180) {
        dx = (uint32_t)(sw - 1U - x);
        dy = (uint32_t)(sh - 1U - y);
      } else if (deg == (uint32_t)k_rotate_270) {
        dx = y;
        dy = (uint32_t)(sw - 1U - x);
        dw = sh;
      }
      dst[(size_t)dy * (size_t)dw + (size_t)dx] = p;
    }
  }
}

/** @brief Map a click in the rotated displayed panel back to native panel coords. */
static void unrotate_click(uint16_t  cx,
                           uint16_t  cy,
                           uint16_t  panel_w,
                           uint16_t  panel_h,
                           uint32_t  deg,
                           uint16_t* nx,
                           uint16_t* ny)
{
  if (deg == (uint32_t)k_rotate_90) {
    *nx = cy;
    *ny = (uint16_t)(panel_h - 1U - cx);
  } else if (deg == (uint32_t)k_rotate_180) {
    *nx = (uint16_t)(panel_w - 1U - cx);
    *ny = (uint16_t)(panel_h - 1U - cy);
  } else if (deg == (uint32_t)k_rotate_270) {
    *nx = (uint16_t)(panel_w - 1U - cy);
    *ny = cx;
  } else {
    *nx = cx;
    *ny = cy;
  }
}

/** @brief Toggle a user switch's pressed level (active-low) for an on-screen button. */
static void set_switch(board_overlay_btn_t btn, bool pressed)
{
  const uint8_t pin =
    (btn == k_board_overlay_btn_sw2) ? (uint8_t)k_sim_sw2_pin : (uint8_t)k_sim_sw1_pin;
  /* SW1/SW2 are momentary push-buttons wired active-low (internal pull-up): held
   * down drives the pin LOW (input false), released lets it return HIGH (true).
   * Driving the level directly -- instead of toggling -- makes a click behave as a
   * real button (press on mouse-down, release on mouse-up), not a latching switch. */
  board_periph_gpio_set_input((uint8_t)k_sim_sw_port, pin, !pressed);
}

/**
 * @brief Apply a POWER-section click to the fuel-gauge model.
 *
 * @details A click on the battery slider track maps the click column to a 0..100
 * percent (::board_overlay_battery_pct_at) and writes it while preserving the
 * charge state; a click on the CHG toggle flips charging while preserving SOC.
 * Shared by the live window and the headless @c --click so both behave alike.
 *
 * @param[in] btn    The POWER button hit (battery slider or CHG toggle).
 * @param[in] cx     Click column in composite pixels (for the slider map).
 * @param[in] disp_w Displayed panel width (the sidebar origin).
 */
static void apply_battery_click(board_overlay_btn_t btn, uint16_t cx, uint16_t disp_w)
{
  if (btn == k_board_overlay_btn_battery) {
    uint8_t pct = 0U;
    if (board_overlay_battery_pct_at(cx, disp_w, &pct)) {
      bool charging = false;
      board_periph_battery_get(nullptr, &charging);
      board_periph_battery_set(pct, charging);
    }
  } else if (btn == k_board_overlay_btn_batt_chg) {
    uint8_t soc      = 0U;
    bool    charging = false;
    board_periph_battery_get(&soc, &charging);
    board_periph_battery_set(soc, !charging);
  } else if (btn == k_board_overlay_btn_lowpower) {
    /* CORE low-power toggle: flip the M33 4:1-clock model live (same effect as
     * the headless --low-power flag). */
    s_low_power = !s_low_power;
  }
}

/**
 * @brief Route a composite-space click to the right input model.
 *
 * @details An on-screen SW1 / SW2 button toggles that user switch (active-low);
 * any other click is mapped back through @ref unrotate_click and injected as a
 * GT911 touch -- the same path the firmware's real ra8_touch_read drains. This is
 * shared by the live window and the headless @c --click so both behave alike.
 *
 * @param[in] cx         Click column in composite pixels.
 * @param[in] cy         Click row in composite pixels.
 * @param[in] panel_w    Native panel width (for the touch unrotate).
 * @param[in] panel_h    Native panel height (for the touch unrotate).
 * @param[in] disp_w     Displayed panel width (the sidebar origin for buttons).
 * @param[in] rotate_deg Active display rotation.
 * @return The button hit, or ::k_board_overlay_btn_none for a panel touch.
 */
static board_overlay_btn_t route_click(uint16_t cx,
                                       uint16_t cy,
                                       uint16_t panel_w,
                                       uint16_t panel_h,
                                       uint16_t disp_w,
                                       uint32_t rotate_deg)
{
  /* A click on the console tab bar switches the active channel (and takes
   * precedence over the console-body autoscroll toggle below, since the tab row
   * sits inside the console region). Switching resets the scrollback to the live
   * tail of the newly selected channel. */
  uint32_t tab_idx = 0U;
  if (board_overlay_hit_console_tab(cx, cy, disp_w, (uint32_t)k_board_console_ch_count, &tab_idx)) {
    s_view_console_ch = (board_console_ch_t)tab_idx;
    s_view_scroll     = 0U;
    s_view_autoscroll = true;
    s_view_log_seen   = board_console_total(s_view_console_ch);
    return k_board_overlay_btn_console;
  }
  const board_overlay_btn_t btn = board_overlay_hit_button(cx, cy, disp_w);
  if (btn == k_board_overlay_btn_console) {
    /* Toggle the console autoscroll, Arduino-Serial-Monitor style: pause it
     * while reading, or click again to jump back to the live tail. */
    s_view_autoscroll = !s_view_autoscroll;
    if (s_view_autoscroll) {
      s_view_scroll = 0U; /* resume following the newest line */
    }
    return btn;
  }
  if ((btn == k_board_overlay_btn_battery) || (btn == k_board_overlay_btn_batt_chg) ||
      (btn == k_board_overlay_btn_lowpower)) {
    apply_battery_click(btn, cx, disp_w);
    return btn;
  }
  if (btn != k_board_overlay_btn_none) {
    set_switch(btn, true); /* mouse-down -> press; mouse-up releases it (run loop). */
    return btn;
  }
  uint16_t nx = cx;
  uint16_t ny = cy;
  unrotate_click(cx, cy, panel_w, panel_h, rotate_deg, &nx, &ny);
  board_periph_touch_inject(nx, ny);
  return k_board_overlay_btn_none;
}

static void build_composite(uc_engine*  uc,
                            uint16_t*   panel_fb,
                            uint16_t*   rot_fb,
                            uint16_t*   composite,
                            uint16_t    panel_w,
                            uint16_t    panel_h,
                            uint16_t    disp_w,
                            uint16_t    disp_h,
                            uint32_t    rotate_deg,
                            const char* app_name)
{
  build_frame(uc, panel_fb, panel_w, panel_h);
  const uint16_t* shown = panel_fb;
  if ((rotate_deg != (uint32_t)k_rotate_0) && (rot_fb != nullptr)) {
    rotate_panel(panel_fb, panel_w, panel_h, rot_fb, rotate_deg);
    shown = rot_fb;
  }
  board_status_t st = {};
  fill_status(&st, app_name);
  board_overlay_compose(composite, shown, disp_w, disp_h, &st);
}

/**
 * @brief Warm-reboot the firmware: re-run from the reset vector in place.
 *
 * @details
 * Mirrors a Cortex-M reset without tearing down the Unicorn engine. Restores the
 * code image and the .data initial values by re-writing the ELF's PT_LOAD
 * segments (the firmware's own Reset_Handler then re-zeroes .bss and re-copies
 * .data), re-reads SP/PC from the vector table, resets the peripheral models
 * (the reset-cause RSTSRn and the VBATT backup domain deliberately survive their
 * reset hooks, so the reboot cause and battery-backed state persist), and clears
 * the host-side exception / scheduler bookkeeping so the next boot starts clean.
 * The installed Unicorn hooks persist across this, so they are NOT re-added, and
 * one-shot CLI input (--keys / --input / --usb-in) is NOT re-injected.
 *
 * @param[in,out] uc    Unicorn engine to reboot (kept alive).
 * @param[in]     elf   The firmware ELF image bytes (re-loaded).
 * @param[in]     len   ELF image length in bytes.
 * @param[in]     trace Whether --trace is active (forwarded to board_periph_init).
 * @return The reset-vector PC to resume the run loop from (Thumb bit set).
 * @since 0.1.0
 */
static uint32_t warm_reboot(uc_engine* uc, const uint8_t* elf, long len, bool trace)
{
  /* 1. Restore the code + .data initial image from the ELF PT_LOAD segments.
   * The same elf/len loaded successfully at startup, so a failure here means the
   * engine's memory writes started failing -- report and return PC=0 so the
   * caller ends the run rather than execute a stale image. */
  if (load_elf(uc, elf, len) != 0) {
    (void)fprintf(stderr, "board_sim: warm_reboot: load_elf failed -- ending run\n");
    return 0U;
  }

  /* 2. Reset the peripheral + network models (RSTSRn / VBATT backup survive). */
  board_periph_init(trace);
  board_net_init(trace);

  /* 3. Clear host-side exception / scheduler bookkeeping. */
  sim_exc_reset();
  sim_mpu_clear_fault();
  sim_div0_clear_fault();
  sim_div0_disarm(); /* a warm reboot re-loads the image, un-patching sites. */
  /* Clear the multi-channel console store + the in-flight ITM line, and reset
   * the tabbed-console view so the rebooted firmware starts with an empty
   * console on the ALL tab (the SCI model's own reset clears its line buffers). */
  board_console_reset();
  sim_console_reset();
  s_view_console_ch = k_board_console_ch_all;
  s_view_scroll     = 0U;
  s_view_autoscroll = true;
  s_view_log_seen   = 0U;

  /* 4. Re-read the Cortex-M reset vector (SP = vectors[0], PC = vectors[1]). */
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)uc_mem_read(uc, sim_memmap_mram_base() + 0U, &sp, 4);
  (void)uc_mem_read(uc, sim_memmap_mram_base() + 4U, &pc, 4);
  pc |= 1U; /* Thumb */
  uint32_t xpsr = (uint32_t)k_xpsr_t_bit;
  (void)uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_reg_write(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)fprintf(stderr, "board_sim: warm reboot -- reset SP=0x%08X PC=0x%08X\n", sp, pc);
  return pc;
}

typedef enum : uint32_t {
  k_panel_line_max = 256U,  /**< Max panel-config line length.    */
  k_panel_name_max = 64U,   /**< Max panel name (incl NUL).       */
  k_panel_dim_max  = 4096U, /**< Sanity cap on a panel dimension. */
} panel_limits_t;

/** @brief Display descriptor loaded from a flat key=value panel file. */
typedef struct {
  char     name[k_panel_name_max];
  uint16_t width;
  uint16_t height;
} board_panel_t;

/** @brief Trim trailing space/tab/CR/LF in place. */
static void panel_rstrip(char* s)
{
  size_t n = strlen(s);
  while (n > 0U) {
    const char c = s[n - 1U];
    if ((c != ' ') && (c != '\t') && (c != '\r') && (c != '\n')) {
      break;
    }
    s[--n] = '\0';
  }
}

/**
 * @brief Load a panel descriptor (name / width / height) from a TOML-ish file.
 *
 * @details
 * A flat ``key = value`` panel descriptor (see ``tools/board_sim/panels/``), so
 * the board emulator becomes whatever display a config describes -- not just the
 * EK-RA8D2 1024x600. Dependency-free bounded parser (strncmp / strtol, no
 * dynamic allocation beyond the FILE handle); blank lines and '#' comments are
 * ignored and quotes are stripped from the name.
 *
 * @param[in]  path Panel config path.
 * @param[out] out  Filled descriptor on success.
 * @return true if a valid width/height were parsed.
 */
static bool load_panel(const char* path, board_panel_t* out)
{
  (void)memset(out, 0, sizeof(*out));
  FILE* f = fopen(path, "r"); /* alloc-allow: host dev tool, not firmware */
  if (f == nullptr) {
    (void)fprintf(stderr, "board_sim: cannot open panel config %s\n", path);
    return false;
  }
  char line[k_panel_line_max];
  while (fgets(line, (int)sizeof(line), f) != nullptr) {
    char* p = line;
    while ((*p == ' ') || (*p == '\t')) {
      p++;
    }
    if ((*p == '#') || (*p == '\0') || (*p == '\n') || (*p == '\r')) {
      continue;
    }
    char* eq = strchr(p, '=');
    if (eq == nullptr) {
      continue;
    }
    *eq       = '\0';
    char* key = p;
    char* val = eq + 1;
    while ((*val == ' ') || (*val == '\t')) {
      val++;
    }
    panel_rstrip(key);
    panel_rstrip(val);
    if (strncmp(key, "width", sizeof("width")) == 0) {
      out->width = (uint16_t)strtol(val, nullptr, (int)k_strtol_base10);
    } else if (strncmp(key, "height", sizeof("height")) == 0) {
      out->height = (uint16_t)strtol(val, nullptr, (int)k_strtol_base10);
    } else if (strncmp(key, "name", sizeof("name")) == 0) {
      const char* s = val;
      size_t      n = strlen(s);
      if ((n >= 2U) && (s[0] == '"') && (s[n - 1U] == '"')) {
        s += 1;
        n -= 2U;
      }
      if (n >= sizeof(out->name)) {
        n = sizeof(out->name) - 1U;
      }
      (void)memcpy(out->name, s, n);
      out->name[n] = '\0';
    }
  }
  (void)fclose(f);
  if ((out->width == 0U) || (out->width > (uint16_t)k_panel_dim_max) || (out->height == 0U) ||
      (out->height > (uint16_t)k_panel_dim_max)) {
    (void)fprintf(stderr, "board_sim: panel %s: width/height missing or out of range\n", path);
    return false;
  }
  return true;
}

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
      s_low_power = true;
    } else if ((strncmp(argv[i], "--primary-core", sizeof("--primary-core")) == 0) &&
               ((i + 1) < argc)) {
      s_primary_core = (strncmp(argv[i + 1], "m33", sizeof("m33")) == 0) ? k_core_m33 : k_core_m85;
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
                (s_primary_core == k_core_m33) ? "Cortex-M33 (Armv8-M)"
                                               : "Cortex-M85 (Armv8.1-M, MVE seams armed)",
                s_low_power ? "  [low-power: 1/4 chunk budget]" : "");
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
      s_ns_vector_base = ns_vbase;
    }
    (void)fprintf(stderr, "board_sim: NS vector base @ 0x%08X\n", s_ns_vector_base);
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

  /* TrustZone NSC pointer validation. The Non-Secure-Callable veneers guard
   * their pointer args with cmse_check_address_range(), which issues Armv8-M
   * `TT`/`TTA` (Test Target) instructions to read an address's security/MPU
   * attribution and then checks the Non-Secure read/write bit. Unicorn's M33
   * has no SAU/IDAU configured (board_sim maps the PPB as plain RAM, so the
   * core's internal SAU stays at its reset all-Secure state); a native TT thus
   * reports every address as Secure, the NS range-check fails, and the veneer
   * returns k_ra8_err_invalid_arg -- stalling CGC/SD bring-up. board_sim collapses
   * the Secure/Non-Secure split into one flat, fully-accessible domain, so every
   * NS pointer the veneers pass (each already null-checked before the range check)
   * is valid. Model that by patching the routine's entry to `BX LR`: r0 still
   * holds the first argument (the pointer `p`) at entry, so an immediate return
   * yields p != NULL == "address OK". This is a one-time 2-byte memory patch
   * (the function image is already copied into Unicorn memory by load_elf), not a
   * UC_HOOK_CODE -- a code hook forces Unicorn to single-step the whole run
   * (~10x slower), whereas the patch has zero steady-state cost. Absent in
   * non-TZ firmware (symbol not found -> no patch). */
  const uint32_t cmse_check_addr = elf_sym_addr(elf, elf_len, "cmse_check_address_range", nullptr);
  if (cmse_check_addr != 0U) {
    const uint16_t bx_lr = (uint16_t)k_thumb_bx_lr;
    (void)uc_mem_write(uc, (uint64_t)cmse_check_addr, &bx_lr, sizeof(bx_lr));
  }

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
  /* TrustZone S->NS boot seams -- armed whenever the firmware links the secure
   * boot's ra8_tz_secure_boot_jump_ns: a two-image --ns app, OR a single-image
   * app whose NS half is embedded at its MRAM LMA (cpu1_pingpong_ipc). The
   * Secure boot bails to its fallback main() unless SAU_TYPE.SREGION >= 4/5;
   * board_sim maps the PPB as plain RAM (SAU_TYPE reads 0), so seed the M85's
   * 8-region count to let the real SAU programming + NS-image copy + BLXNS
   * run. Firmware without the symbol keeps its current (all-Secure) path. */
  {
    uint32_t       jn_size = 0U;
    const uint32_t jump_ns = elf_sym_addr(elf, elf_len, "ra8_tz_secure_boot_jump_ns", &jn_size);
    if ((jump_ns != 0U) && (jn_size >= (uint32_t)k_thumb_hw_bytes)) {
      const uint32_t sau_type = (uint32_t)k_sau_type_regs;
      (void)uc_mem_write(uc, (uint64_t)k_sau_type_addr, &sau_type, sizeof(sau_type));

      /* Hand-emulate the Secure->NS BLXNS in ra8_tz_secure_boot_jump_ns.
       * Unicorn's all-Secure M33 cannot really switch worlds, so resolve the
       * BLXNS site from the Secure symtab (scan the function for the BLXNS
       * opcode) and hook it to enter NS manually (see on_blxns). Without this
       * the BLXNS stalls and the NS world never runs. */
      uint32_t blxns_at = 0U;
      for (uint32_t a = jump_ns; (a + (uint32_t)k_thumb_hw_bytes) <= (jump_ns + jn_size);
           a += (uint32_t)k_thumb_hw_bytes) {
        uint16_t hw = 0U;
        (void)uc_mem_read(uc, (uint64_t)a, &hw, sizeof(hw));
        if (((uint32_t)hw & (uint32_t)k_blxns_mask) == (uint32_t)k_blxns_match) {
          blxns_at = a;
          break;
        }
      }
      if (blxns_at != 0U) {
        uc_hook h_blxns;
        (void)uc_hook_add(uc,
                          &h_blxns,
                          UC_HOOK_CODE,
                          (void*)on_blxns,
                          nullptr,
                          (uint64_t)blxns_at,
                          (uint64_t)blxns_at);
        (void)fprintf(stderr, "board_sim: TZ BLXNS seam armed @ 0x%08X\n", blxns_at);
      } else {
        (void)fprintf(stderr,
                      "board_sim: TZ warning: no BLXNS found in ra8_tz_secure_boot_jump_ns\n");
      }
    }
  }
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
  if (s_primary_core == k_core_m85) {
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
      s_view_console_ch = (board_console_ch_t)click_tab_idx;
      s_view_scroll     = 0U;
      s_view_autoscroll = true;
      s_view_log_seen   = board_console_total(s_view_console_ch);
      click_was_tab     = true;
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
    s_view_pc     = run_pc;
    s_view_chunks = chunks;

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
      if (s_low_power) {
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
      const int32_t notches = board_view_poll_scroll(view);
      if (notches > 0) {
        s_view_autoscroll = false; /* user scrolled up -> pause the live follow */
        s_view_scroll += (uint32_t)notches;
      } else if (notches < 0) {
        const uint32_t dec = (uint32_t)(-notches);
        if (s_view_scroll > dec) {
          s_view_scroll -= dec;
        } else {
          s_view_scroll     = 0U;
          s_view_autoscroll = true; /* scrolled back to the tail -> resume follow */
        }
      }
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
      const bool usbh_done =
        (s_vkbd_reports_sent >= (uint32_t)k_vkbd_stop_reports) || s_vmsc_write_seen;
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
  s_view_running = false;
  s_view_pc      = run_pc;

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
