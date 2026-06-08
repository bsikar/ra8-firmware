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
 * Time: bare-metal delays here are SysTick-driven (``ra_time`` enables SysTick
 * with TICKINT and counts exceptions). Nothing advances time on a plain memory
 * model, so the run loop is chunked and, between chunks, cooperatively invokes
 * the firmware's installed SysTick_Handler as a function -- its tick-counter
 * memory write persists while the interrupted context's registers are restored,
 * which is precisely a real SysTick IRQ's observable effect. This carries the
 * firmware past ``ra_delay_ms`` so it reaches its main loop (e.g. driving the
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unistd.h>

#include "board_view.h"

/* RA8D2 memory map (EK board) -- from the linker script / HUM R01UH1065EJ. */
typedef struct {
  const char* name;
  uint64_t    base;
  uint64_t    size;
} mem_region_t;

static const mem_region_t k_regions[] = {
  {"ITCM", 0x00000000UL, 0x00010000UL}, /* 64 KiB tightly-coupled code   */
  {"MRAM", 0x02000000UL, 0x00100000UL}, /* 1 MiB code flash + vectors    */
  {"OFS", 0x0300A000UL, 0x00001000UL},  /* option-setting flash          */
  {"DTCM", 0x20000000UL, 0x00010000UL}, /* 64 KiB tightly-coupled data   */
  {"SRAM", 0x22000000UL, 0x00100000UL}, /* 1 MiB on-chip SRAM            */
  {"DATA_FLASH", 0x27000000UL, 0x00004000UL},
  {"SDRAM", 0x68000000UL, 0x04000000UL}, /* 64 MiB external SDRAM         */
  {"PPB", 0xE0000000UL, 0x00100000UL},   /* ARM private peripheral bus    */
};

/* Renesas peripheral space -- modelled as logged MMIO (returns all-ones so
 * "wait for ready bit" polls fall through instead of spinning forever). */
typedef enum : uint64_t {
  k_periph_base = 0x40000000UL,
  k_periph_size = 0x10000000UL, /* 0x40000000-0x4FFFFFFF: all Renesas peripherals */
} periph_map_t;

typedef enum : uint32_t {
  k_run_chunk_insns = 500000U, /**< Instructions per emulation chunk.       */
  k_run_max_chunks  = 40000U,  /**< Chunk budget. Each chunk offers one     */
                               /**< SysTick, so RTOS apps whose threads     */
                               /**< sleep on hundreds/thousands of ticks    */
                               /**< (e.g. ThreadX tx_thread_sleep) need a    */
                               /**< far larger budget than bare-metal.       */
  k_run_wall_s     = 120U,     /**< Wall-clock safety bound (seconds).      */
  k_run_inner_max  = 4096U,    /**< Per-chunk exception-resolve relaunch cap.*/
  k_mmio_slots     = 2048U,    /**< Distinct MMIO addresses tracked.        */
  k_mmio_settle    = 8U,       /**< Same-addr reads before a poll "settles".*/
  k_mmio_print_max = 256U,     /**< Max MMIO rows printed in the summary.    */
} sim_budget_t;

/* Cortex-M system control space (architectural, all cores) -- inside the PPB.
 * The PPB is mapped as plain RAM here (not callback MMIO), so SCB/NVIC writes
 * the firmware performs land in memory and read back -- the exception model
 * below polls/edits these words to model what real M-profile hardware would do
 * with the NVIC that Unicorn does not implement. */
typedef enum : uint64_t {
  k_syst_csr       = 0xE000E010UL, /**< SysTick control/status (SYST_CSR).   */
  k_syst_csr_run   = 0x3UL,        /**< ENABLE | TICKINT both set.           */
  k_scb_icsr       = 0xE000ED04UL, /**< Interrupt control/state (ICSR).      */
  k_scb_vtor       = 0xE000ED08UL, /**< Vector table offset register.        */
  k_scb_shpr2      = 0xE000ED1CUL, /**< System handler priority 2 (SVC=b3).  */
  k_scb_shpr3      = 0xE000ED20UL, /**< System handler priority 3 (PSV/SYT). */
  k_icsr_pendsvset = 28UL,         /**< ICSR.PENDSVSET bit (request PendSV).  */
  k_icsr_pendstset = 26UL,         /**< ICSR.PENDSTSET bit (request SysTick). */
  k_exc_svcall     = 11UL,         /**< SVCall exception / vector index.      */
  k_exc_pendsv     = 14UL,         /**< PendSV exception / vector index.      */
  k_exc_systick    = 15UL,         /**< SysTick exception / vector index.     */
} cortexm_scs_t;

/* Armv7E-M / Armv8-M exception model constants (the part Unicorn's Cortex-M33
 * core leaves to software here -- it has no NVIC/exception unit). EXC_RETURN
 * magic values steer the unstack: bit2 picks the return stack (1 = PSP, 0 =
 * MSP), bit3 the return mode (1 = Thread, 0 = Handler), bit4 the frame type
 * (1 = no FP extended frame). ThreadX runs with FPCA clear so FType is always
 * 1 and no S0-S31 are stacked -- see _tx_thread_schedule (TST LR,#0x10 skips
 * the VFP save/restore for these values). */
typedef enum : uint32_t {
  k_exc_frame_words = 8U,          /**< {R0-R3,R12,LR,PC,xPSR} basic frame.  */
  k_exc_frame_bytes = 32U,         /**< 8 words * 4 bytes.                    */
  k_exc_ret_base    = 0xFFFFFFF0U, /**< EXC_RETURN values live in [F0..FF].  */
  k_exc_ret_handler = 0xFFFFFFF1U, /**< Return to Handler mode, MSP.         */
  k_exc_ret_msp     = 0xFFFFFFF9U, /**< Return to Thread mode, MSP.          */
  k_exc_ret_psp     = 0xFFFFFFFDU, /**< Return to Thread mode, PSP.          */
  k_exc_ret_spsel   = 0x4U,        /**< EXC_RETURN bit2: return stack = PSP. */
  k_exc_ret_mode    = 0x8U,        /**< EXC_RETURN bit3: return to Thread.   */
  k_control_spsel   = 0x2U,        /**< CONTROL.SPSEL: thread SP = PSP.      */
  k_xpsr_t_bit      = 0x01000000U, /**< xPSR.T (Thumb) -- must stay set.     */
  k_xpsr_align9     = 0x00000200U, /**< xPSR bit9: stack-frame realignment.  */
  k_xpsr_ipsr_mask  = 0x000001FFU, /**< xPSR[8:0] = IPSR (active exception).  */
  k_exc_prio_none   = 0x100U,      /**< Sentinel "no handler active" prio.   */
  k_exc_prio_max    = 0xFFU,       /**< Lowest configurable priority value.  */
  k_exc_nest_max    = 4U,          /**< Tracked active-exception nesting cap.*/
  k_byte_bits       = 8U,          /**< Bits per byte (SHPR field width).    */
} cortexm_exc_t;

/* ra_touch_read stub: the firmware's ra_touch_read is short-circuited by a
 * UC_HOOK_CODE at its entry so board_sim supplies the touch coordinates without
 * modelling the GT911/I2C. AAPCS (R0=out_points, R1=max_count, R2=got_count);
 * k_ra_ok == 0 (libs/ra_core/inc/ra_err.h). The GT911 point fields the firmware
 * decodes are x at byte 0 and y at byte 2 of ra_touch_point_t. */
typedef enum : uint32_t {
  k_touch_ok        = 0U,  /**< k_ra_ok return value for ra_touch_read.   */
  k_touch_pt_x_off  = 0U,  /**< ra_touch_point_t.x byte offset.           */
  k_touch_pt_y_off  = 2U,  /**< ra_touch_point_t.y byte offset.           */
  k_touch_one_point = 1U,  /**< *got_count written for a single contact.  */
  k_touch_no_point  = 0U,  /**< *got_count written when no click pending. */
  k_elf_sht_symtab  = 2U,  /**< SHT_SYMTAB section type.                   */
  k_elf_sht_strtab  = 3U,  /**< SHT_STRTAB section type.                   */
  k_elf_thumb_clear = ~1U, /**< Mask off the Thumb bit of st_value.       */
} touch_stub_t;

/* Renesas peripheral quirks that the generic sparse model cannot reproduce.
 *
 * MRMS frequency latches: the CGC driver (libs/ra_hal/src/ra_cgc.c,
 * internal_wait_mrm_freq) writes ``key | freq_mhz`` to MRCFREQ / MREFREQ and
 * spins until the register reads back == freq_mhz. Real silicon validates the
 * upper key byte then strips it, so the readback is the bare frequency. The
 * generic model reflects the full written word (key still in bits[31:24]), so
 * the readback never equals freq and the poll runs to its 0x40000 timeout ->
 * lcd_panic_halt. Model the hardware: on readback of these two registers,
 * return the stored value with the key byte masked off. */
typedef enum : uint64_t {
  k_mrms_mrcfreq   = 0x4013C004UL, /**< MRICLK freq latch (write key 0x1E). */
  k_mrms_mrefreq   = 0x4013C008UL, /**< MRPCLK freq latch (write key 0xE1). */
  k_mrms_freq_mask = 0x00FFFFFFUL, /**< Key byte (bits[31:24]) stripped.    */
} mrms_quirk_t;

/* GLCDC observation point. The lcd_color_cycle demo proves a live panel by
 * re-writing BG_BGC (background colour) every frame and pulsing BG_EN.VEN to
 * commit it. The generic model only keeps the last value per address, so the
 * distinct colours cycled are tracked separately as the tool's success witness.
 * GLCDC base 0x40342000 + 0x1014 = BG_BGC (HUM Ch 63). */
typedef enum : uint64_t {
  k_glcdc_bg_bgc  = 0x40343014UL, /**< GLCDC BG.BGC background colour.       */
  k_bgc_track_max = 32UL,         /**< Distinct BG_BGC values remembered.   */
} glcdc_obs_t;

/* Live-view (--view) and snapshot (--ppm) presentation settings. */
typedef enum : uint32_t {
  k_view_default_w     = 1024U,    /**< Default window width (EK-RA8D2 panel).  */
  k_view_default_h     = 600U,     /**< Default window height (EK-RA8D2 panel). */
  k_view_present_every = 16U,      /**< Present the frame every Nth chunk.      */
  k_view_max_chunks    = 4000000U, /**< Cap in --view; closing the window ends. */
  k_view_idle_us       = 16000U,   /**< ~60 Hz idle pump after the run ends.    */
} view_cfg_t;

/* GLCDC graphics-layer 1 (GR1) framebuffer registers + field decode. The HAL
 * programs FLM6.FORMAT[30:28], FLM3.LNOFF[31:16] (line stride in bytes), and
 * FLM5.LNNUM[26:16] (lines - 1); reverse those to recover the framebuffer. */
typedef enum : uint64_t {
  k_glcdc_gr1_saddr = 0x4034310CUL, /**< GR[0].FLM2 framebuffer base.       */
  k_glcdc_gr1_flm3  = 0x40343110UL, /**< GR[0].FLM3 line stride (LNOFF).    */
  k_glcdc_gr1_flm5  = 0x40343118UL, /**< GR[0].FLM5 lines (LNNUM/DATANUM).  */
  k_glcdc_gr1_fmt   = 0x4034311CUL, /**< GR[0].FLM6 pixel FORMAT.           */
} glcdc_gr_t;

typedef enum : uint32_t {
  k_glcdc_fmt_rgb565  = 2U,      /**< FLM6.FORMAT code for RGB565.            */
  k_glcdc_fmt_shift   = 28U,     /**< FORMAT[30:28].                          */
  k_glcdc_fmt_mask    = 0x7U,    /**< FORMAT field width.                     */
  k_glcdc_high_shift  = 16U,     /**< FLM3 stride / FLM5 lnnum live in [*:16].*/
  k_glcdc_stride_mask = 0xFFFFU, /**< FLM3.LNOFF is 16 bits.                  */
  k_glcdc_lnnum_mask  = 0x7FFU,  /**< FLM5.LNNUM is 11 bits.                  */
} glcdc_decode_t;

/* Sparse model of the Renesas peripheral space. Each touched address gets a
 * slot: control writes are reflected back on read so "configure then verify"
 * works, but once the firmware spins reading one address (a "wait for
 * ready/idle" poll) past k_mmio_settle, reads alternate 0 / all-ones so a
 * single-bit poll for either edge (flag set OR flag clear) completes instead
 * of running to its timeout. */
static uint64_t s_mmio_addr[k_mmio_slots];
static uint32_t s_mmio_val[k_mmio_slots];
static bool     s_mmio_written[k_mmio_slots];
static uint32_t s_mmio_rcount[k_mmio_slots];
static uint32_t s_mmio_wcount[k_mmio_slots];
static uint32_t s_mmio_n;
static uint32_t s_mmio_reads;
static uint32_t s_mmio_writes;
static uint32_t s_mmio_toggle;
static int      s_mmio_cache    = -1; /**< 1-entry address->slot lookup cache.*/
static int      s_mmio_run_slot = -1; /**< Slot of the current read run.      */
static uint32_t s_mmio_run;           /**< Consecutive reads of that slot.    */
static uint32_t s_systick_fires;

/* Hand-modelled Cortex-M exception state. Unicorn's M33 core has no NVIC /
 * exception unit, so board_sim takes SysTick / PendSV / SVCall by hand: it
 * tracks the active-exception priority stack here (everything else -- MSP/PSP/
 * CONTROL/xPSR/PRIMASK -- is read straight from Unicorn). s_exc_stack holds the
 * priority of each handler currently active so a higher-priority exception
 * (e.g. SysTick, prio 0x40) can pre-empt a lower one (PendSV, prio 0xFF) but
 * not vice-versa, exactly as the real priority logic would nest them. */
static uint32_t s_exc_stack[k_exc_nest_max]; /**< Active-handler priorities.   */
static uint32_t s_exc_depth;                 /**< Number of active handlers.   */
static uint32_t s_pendsv_takes;              /**< PendSV exceptions taken.     */
static uint32_t s_svc_takes;                 /**< SVCall exceptions taken.     */
static uint64_t s_exc_return_pc;             /**< Pending EXC_RETURN to unstack.*/
static bool     s_exc_return_hit;            /**< An EXC_RETURN branch was seen.*/
static bool     s_systick_pending;           /**< SysTick exception is pended.  */

/* BG_BGC colour-cycle witness: total writes and the distinct values seen. */
static uint32_t s_bgc_writes;
static uint32_t s_bgc_distinct[k_bgc_track_max];
static uint32_t s_bgc_distinct_n;

/* Touch injection: a click that the next firmware ra_touch_read should return.
 * The --view loop sets it from board_view_poll_click; --click sets it once.
 * The UC_HOOK_CODE on ra_touch_read drains it (AAPCS: R0=out, R2=got). */
static bool     s_touch_pending;   /**< A click is waiting to be reported.    */
static uint16_t s_touch_x;         /**< Pending click X (panel pixels).       */
static uint16_t s_touch_y;         /**< Pending click Y (panel pixels).       */
static uint64_t s_touch_read_addr; /**< ra_touch_read entry addr (Thumb bit cleared). */
static uint64_t s_touch_open_addr; /**< ra_touch_open entry addr (Thumb bit cleared). */
static uint32_t s_touch_injected;  /**< Count of clicks handed to the firmware.*/

/** @brief Record a BG_BGC write; remember the value if it is a new colour. */
static void bgc_track(uint32_t value)
{
  s_bgc_writes++;
  for (uint32_t i = 0U; i < s_bgc_distinct_n; i++) {
    if (s_bgc_distinct[i] == value) {
      return;
    }
  }
  if (s_bgc_distinct_n < (uint32_t)k_bgc_track_max) {
    s_bgc_distinct[s_bgc_distinct_n++] = value;
  }
}

/** @brief Find (or add) a slot for a distinct MMIO address; -1 if table full. */
static int mmio_index(uint64_t addr)
{
  if ((s_mmio_cache >= 0) && (s_mmio_addr[s_mmio_cache] == addr)) {
    return s_mmio_cache;
  }
  for (uint32_t i = 0U; i < s_mmio_n; i++) {
    if (s_mmio_addr[i] == addr) {
      s_mmio_cache = (int)i;
      return (int)i;
    }
  }
  if (s_mmio_n < (uint32_t)k_mmio_slots) {
    s_mmio_addr[s_mmio_n] = addr;
    s_mmio_cache          = (int)s_mmio_n;
    return (int)(s_mmio_n++);
  }
  return -1;
}

static uint64_t mmio_read(uc_engine* uc, uint64_t offset, unsigned size, void* user)
{
  (void)uc;
  (void)size;
  (void)user;
  s_mmio_reads++;
  const int idx = mmio_index((uint64_t)k_periph_base + offset);
  if (idx >= 0) {
    s_mmio_rcount[idx]++;
    if (idx == s_mmio_run_slot) {
      s_mmio_run++;
    } else {
      s_mmio_run_slot = idx;
      s_mmio_run      = 1U;
    }
    /* Reflect a written control value until a spin-poll forces it to settle. */
    if (s_mmio_written[idx] && (s_mmio_run <= (uint32_t)k_mmio_settle)) {
      const uint64_t addr = (uint64_t)k_periph_base + offset;
      /* MRMS frequency latches strip the write key byte on readback so the
       * driver's "wait until reg == freq" poll completes (see mrms_quirk_t). */
      if ((addr == (uint64_t)k_mrms_mrcfreq) || (addr == (uint64_t)k_mrms_mrefreq)) {
        return (uint64_t)(s_mmio_val[idx] & (uint32_t)k_mrms_freq_mask);
      }
      return (uint64_t)s_mmio_val[idx];
    }
  }
  s_mmio_toggle ^= 0xFFFFFFFFU;
  return (uint64_t)s_mmio_toggle;
}

static void mmio_write(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user)
{
  (void)uc;
  (void)size;
  (void)user;
  s_mmio_writes++;
  if (((uint64_t)k_periph_base + offset) == (uint64_t)k_glcdc_bg_bgc) {
    bgc_track((uint32_t)value);
  }
  const int idx = mmio_index((uint64_t)k_periph_base + offset);
  if (idx >= 0) {
    s_mmio_wcount[idx]++;
    s_mmio_val[idx]     = (uint32_t)value;
    s_mmio_written[idx] = true;
    if (idx == s_mmio_run_slot) {
      s_mmio_run++; /* same-addr read-modify-write spin accumulates toward settle */
    } else {
      s_mmio_run_slot = idx; /* new register -> following reads should see its value */
      s_mmio_run      = 1U;
    }
  }
}

/* Armv8.1-M conditional-select family (CSEL/CSINC/CSINV/CSNEG). The RA8D2 is
 * Cortex-M85 (Armv8.1-M) but Unicorn's nearest core is M33 (Armv8-M), which
 * lacks these. GCC emits them for branchless index/modulo math -- notably in
 * GUIX's gx_generic_event_post on the touch path -- so the invalid-instruction
 * hook decodes and executes them and lets the real firmware path continue.
 * Encoding (T1, two halfwords, little-endian): hw1 = 0xEA5n (n = Rn);
 * hw2 bit15=1, bit14=0, op=bits[13:12], Rd=bits[11:8], cond=bits[7:4],
 * Rm=bits[3:0]. Verified against the GNU assembler for armv8.1-m.main. */
typedef enum : uint32_t {
  k_cs_hw1_mask  = 0xFFF0U, /**< hw1 high 12 bits identify the group.      */
  k_cs_hw1_match = 0xEA50U, /**< hw1[15:4] == 0xEA5 for this family.       */
  k_cs_hw2_b15   = 0x8000U, /**< hw2 bit15 must be 1.                      */
  k_cs_hw2_b14   = 0x4000U, /**< hw2 bit14 must be 0.                      */
  k_cs_op_csel   = 0U,      /**< op == 00: Rd = c ? Rn : Rm.               */
  k_cs_op_csinc  = 1U,      /**< op == 01: Rd = c ? Rn : Rm + 1.           */
  k_cs_op_csinv  = 2U,      /**< op == 10: Rd = c ? Rn : ~Rm.              */
  k_cs_op_csneg  = 3U,      /**< op == 11: Rd = c ? Rn : -Rm.              */
  k_cs_insn_len  = 4U,      /**< Both halfwords: 4 bytes.                  */
} cond_select_t;

/* APSR condition-flag bit positions inside xPSR. */
typedef enum : uint32_t {
  k_apsr_n = 31U, /**< Negative. */
  k_apsr_z = 30U, /**< Zero.     */
  k_apsr_c = 29U, /**< Carry.    */
  k_apsr_v = 28U, /**< Overflow. */
} apsr_bit_t;

/* ARM register index (0..15) -> Unicorn register id. PC(15)/SP(13)/LR(14) are
 * never CSx destinations in practice but are mapped for completeness. */
static const int k_arm_reg_id[16] = {
  UC_ARM_REG_R0,
  UC_ARM_REG_R1,
  UC_ARM_REG_R2,
  UC_ARM_REG_R3,
  UC_ARM_REG_R4,
  UC_ARM_REG_R5,
  UC_ARM_REG_R6,
  UC_ARM_REG_R7,
  UC_ARM_REG_R8,
  UC_ARM_REG_R9,
  UC_ARM_REG_R10,
  UC_ARM_REG_R11,
  UC_ARM_REG_R12,
  UC_ARM_REG_SP,
  UC_ARM_REG_LR,
  UC_ARM_REG_PC,
};

/**
 * @brief Evaluate an ARM condition code against the APSR flags.
 *
 * @param[in] cond 4-bit ARM condition code (0..15).
 * @param[in] xpsr Current xPSR (APSR flags live in the top nibble).
 * @return true if the condition holds.
 */
static bool cond_holds(uint32_t cond, uint32_t xpsr)
{
  const bool n = ((xpsr >> (uint32_t)k_apsr_n) & 1U) != 0U;
  const bool z = ((xpsr >> (uint32_t)k_apsr_z) & 1U) != 0U;
  const bool c = ((xpsr >> (uint32_t)k_apsr_c) & 1U) != 0U;
  const bool v = ((xpsr >> (uint32_t)k_apsr_v) & 1U) != 0U;
  switch (cond & 0xFU) {
    case 0x0U:
      return z; /* EQ */
    case 0x1U:
      return !z; /* NE */
    case 0x2U:
      return c; /* CS/HS */
    case 0x3U:
      return !c; /* CC/LO */
    case 0x4U:
      return n; /* MI */
    case 0x5U:
      return !n; /* PL */
    case 0x6U:
      return v; /* VS */
    case 0x7U:
      return !v; /* VC */
    case 0x8U:
      return c && !z; /* HI */
    case 0x9U:
      return !c || z; /* LS */
    case 0xAU:
      return n == v; /* GE */
    case 0xBU:
      return n != v; /* LT */
    case 0xCU:
      return !z && (n == v); /* GT */
    case 0xDU:
      return z || (n != v); /* LE */
    default:
      return true; /* AL (0xE) / 0xF */
  }
}

/**
 * @brief Emulate one Armv8.1-M conditional-select instruction if present at PC.
 *
 * @details
 * Decodes the CSEL/CSINC/CSINV/CSNEG encoding (see ::cond_select_t), evaluates
 * the condition against the APSR, computes Rd, writes it back, and advances PC
 * past the 4-byte instruction. This lets Unicorn's M33 core execute the M85
 * firmware's branchless index math instead of trapping on an opcode it does not
 * implement. Anything that is not this family is left untouched.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapped instruction.
 * @param[in]     code The 4 instruction bytes already read at @p pc.
 * @return true if a conditional-select was recognised, executed, and PC advanced.
 */
static bool emulate_cond_select(uc_engine* uc, uint32_t pc, const uint8_t* code)
{
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << 8));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << 8));
  if (((hw1 & (uint16_t)k_cs_hw1_mask) != (uint16_t)k_cs_hw1_match) ||
      ((hw2 & (uint16_t)k_cs_hw2_b15) == 0U) || ((hw2 & (uint16_t)k_cs_hw2_b14) != 0U)) {
    return false;
  }
  const uint32_t rn   = (uint32_t)(hw1 & 0x000FU);
  const uint32_t op   = (uint32_t)((hw2 >> 12) & 0x3U);
  const uint32_t rd   = (uint32_t)((hw2 >> 8) & 0xFU);
  const uint32_t cond = (uint32_t)((hw2 >> 4) & 0xFU);
  const uint32_t rm   = (uint32_t)(hw2 & 0x000FU);

  uint32_t xpsr = 0U;
  uint32_t vn   = 0U;
  uint32_t vm   = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_XPSR, &xpsr);
  (void)uc_reg_read(uc, k_arm_reg_id[rn], &vn);
  (void)uc_reg_read(uc, k_arm_reg_id[rm], &vm);

  uint32_t result;
  if (cond_holds(cond, xpsr)) {
    result = vn;
  } else {
    switch (op) {
      case (uint32_t)k_cs_op_csinc:
        result = vm + 1U;
        break;
      case (uint32_t)k_cs_op_csinv:
        result = ~vm;
        break;
      case (uint32_t)k_cs_op_csneg:
        result = (uint32_t)(-(int32_t)vm);
        break;
      case (uint32_t)k_cs_op_csel:
      default:
        result = vm;
        break;
    }
  }
  (void)uc_reg_write(uc, k_arm_reg_id[rd], &result);
  uint32_t next = pc + (uint32_t)k_cs_insn_len;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  return true;
}

/* Forward declarations for the Cortex-M exception engine (defined below, after
 * the ELF/memory helpers they build on). The memory hooks above need to vector
 * SVCall and recognise EXC_RETURN before those definitions appear. */
static bool     is_exc_return(uint64_t pc);
static uint32_t exc_vector(uc_engine* uc, uint32_t vtor_base, uint32_t exc_num);
static void     exc_enter(uc_engine* uc, uint32_t exc_num, uint32_t handler);
static uint32_t exc_priority(uc_engine* uc, uint32_t exc_num);
static uint32_t exc_active_prio(void);

/** @brief Disassemble + report an instruction the core could not decode. */
static bool on_invalid_insn(uc_engine* uc, void* user)
{
  (void)user;
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uint8_t code[4] = {};
  (void)uc_mem_read(uc, pc, code, sizeof(code));

  /* The RA8D2 firmware is built for Cortex-M85 (Armv8.1-M); the nearest core
   * Unicorn offers is M33 (Armv8-M), which lacks the conditional-select family.
   * GCC emits those for branchless index math on the GUIX touch path, so
   * execute them here. emulate_cond_select writes Rd and advances PC past the
   * 4-byte instruction; then uc_emu_stop so the chunked run loop relaunches from
   * the new PC -- editing PC and continuing in-place corrupts Unicorn's block /
   * Thumb state (it then misdecodes the next valid instruction), so the
   * stop-then-relaunch contract the SysTick / touch stubs use is required here. */
  if (emulate_cond_select(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true; /* handled -- run loop resumes at the advanced PC */
  }

  (void)fprintf(stderr,
                "  INVALID INSN @ 0x%08X: bytes %02X %02X %02X %02X\n",
                pc,
                code[0],
                code[1],
                code[2],
                code[3]);

  csh cs;
  if (cs_open(CS_ARCH_ARM, (cs_mode)(CS_MODE_THUMB | CS_MODE_MCLASS), &cs) == CS_ERR_OK) {
    cs_insn*     insn = nullptr;
    const size_t n    = cs_disasm(cs, code, sizeof(code), pc, 1, &insn);
    if (n > 0U) {
      (void)fprintf(stderr, "  disasm: %s %s\n", insn[0].mnemonic, insn[0].op_str);
      cs_free(insn, n);
    } else {
      (void)fprintf(stderr, "  disasm: capstone could not decode it either\n");
    }
    cs_close(&cs);
  }
  return false; /* not handled -> stop emulation with UC_ERR_INSN_INVALID */
}

/** @brief Hook fired on access to unmapped memory (peripheral surface gap). */
static bool
on_unmapped(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)size;
  (void)value;
  (void)user;
  /* A FETCH into the EXC_RETURN range is not a fault -- it is the core taking
   * an exception return (the handler ran "BX lr" with an EXC_RETURN magic in
   * LR). Capture it and stop cleanly so the run loop can unstack the frame and
   * resume; the Unicorn fetch-fault error this produces is expected/handled. */
  if ((type == UC_MEM_FETCH_UNMAPPED) && is_exc_return(addr)) {
    s_exc_return_pc  = addr;
    s_exc_return_hit = true;
    (void)uc_emu_stop(uc);
    return false;
  }
  (void)fprintf(stderr,
                "  UNMAPPED %s @ 0x%08llX (extend the memory/peripheral map)\n",
                (type == UC_MEM_READ_UNMAPPED)    ? "read"
                : (type == UC_MEM_WRITE_UNMAPPED) ? "write"
                                                  : "fetch",
                (unsigned long long)addr);
  return false; /* stop emulation and report */
}

/**
 * @brief UC_HOOK_INTR handler: take the SVCall exception on an `svc` opcode.
 *
 * @details
 * Unicorn raises UC_HOOK_INTR when the firmware executes the Thumb `svc`
 * instruction but, lacking an exception unit, does not vector it. This models
 * SVCall (#11): the basic frame is stacked and the core vectors to SVC_Handler
 * via ::exc_enter, then emulation is stopped so the chunked run loop relaunches
 * cleanly from the handler entry (editing PC mid-block and continuing corrupts
 * Unicorn's block/Thumb state -- the same stop-then-relaunch contract the touch
 * and conditional-select stubs use). ThreadX in single-mode never issues an
 * SVC, but bare-metal / future RTOS paths that start the first thread via `svc`
 * are handled correctly here. PRIMASK does not mask SVCall (it is synchronous),
 * matching hardware.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     int_no    Interrupt/exception number reported by Unicorn.
 * @param[in]     user_data Hook user pointer (unused; signature fixed by Unicorn).
 * @return Nothing.
 *
 * @pre @p uc has just executed an `svc` instruction or branched to EXC_RETURN.
 * @pre The vector table (at VTOR or the MRAM fallback) holds SVC_Handler.
 * @post Either an exception was taken/returned (PC updated) or, on a missing
 *       SVC handler, the core is left untouched.
 * @post Emulation is stopped so the run loop resumes from the new PC.
 * @note Only the SVC interrupt class is acted on; other int_no values are
 *       ignored so unrelated traps fall through.
 * @since 0.1.0
 */
static void on_intr(uc_engine* uc, uint32_t int_no, void* user_data)
{
  (void)int_no;
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);

  /* Cortex-M exception RETURN. Unicorn's M-profile core does not pop the
   * exception frame itself, but it DOES trap a branch to an EXC_RETURN magic
   * (0xFFFFFFFx) by raising an interrupt with the magic left in PC (Thumb bit
   * masked off, but the stack/mode selector bits 2/3 intact). Unstack the basic
   * frame and resume the interrupted context. This is how every handler that
   * board_sim vectors in (SysTick / PendSV / SVCall) returns. */
  if (is_exc_return((uint64_t)pc)) {
    s_exc_return_pc  = (uint64_t)pc;
    s_exc_return_hit = true;
    (void)uc_emu_stop(uc);
    return;
  }

  /* Otherwise it is a synchronous `svc` -- take SVCall (#11). ThreadX in single
   * mode never issues one, but bare-metal / future RTOS first-thread-start
   * paths do. The VTOR fallback is the MRAM vector-table base; the live VTOR
   * (set by SystemInit) is read inside exc_vector. */
  (void)user_data;
  const uint32_t vtor_base = (uint32_t)k_regions[1].base;
  const uint32_t handler   = exc_vector(uc, vtor_base, (uint32_t)k_exc_svcall);
  if (handler != 0U) {
    exc_enter(uc, (uint32_t)k_exc_svcall, handler);
    s_svc_takes++;
  }
  (void)uc_emu_stop(uc);
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for SCB ICSR -- take PendSV promptly.
 *
 * @details
 * On hardware, writing ICSR.PENDSVSET pends PendSV, and -- because ThreadX
 * follows the store with DSB+ISB and PendSV is enabled at a priority above
 * thread level with interrupts unmasked -- the exception activates at the very
 * next instruction. board_sim runs the CPU in long chunks, so without help it
 * would only notice PENDSVSET at the end of a 500k-instruction chunk; by then
 * the requesting thread (e.g. a thread suspending inside tx_queue_receive) has
 * run far past the point where it expected to be switched out and observes
 * inconsistent scheduler state. Stopping the chunk the instant PENDSVSET is
 * written hands control straight back to the run loop, which takes PendSV at
 * that boundary -- restoring next-instruction activation semantics. The store
 * itself has already landed in PPB RAM, so exc_take_pending sees the bit set.
 *
 * @param[in,out] uc    Unicorn engine.
 * @param[in]     type  Memory access type (write); unused.
 * @param[in]     addr  Faulting/observed address (the ICSR word).
 * @param[in]     size  Access width in bytes; unused.
 * @param[in]     value The value being written to ICSR.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 *
 * @pre @p uc is mid-chunk executing the store to ICSR.
 * @pre The hook is registered for the 4-byte ICSR word only.
 * @post Emulation is stopped iff the write sets PENDSVSET.
 * @post The PENDSVSET bit is left in PPB RAM for exc_take_pending to read.
 * @note PENDSVCLR / status-only writes do not stop the chunk.
 * @since 0.1.0
 */
static void
on_icsr_write(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void* user)
{
  (void)type;
  (void)addr;
  (void)size;
  (void)user;
  if (((uint32_t)value & (1U << (uint32_t)k_icsr_pendsvset)) == 0U) {
    return; /* not a PendSV request (e.g. PENDSVCLR / status write) */
  }
  /* Only end the chunk when PendSV could actually activate now: interrupts
   * unmasked and no equal/higher-priority handler already running. If masked
   * (the store sits inside a ThreadX critical section), do NOT stop -- the run
   * loop would otherwise relaunch from this same store and re-pend forever,
   * since uc_emu_stop here leaves PC on the store. With those guards, the run
   * loop's exc_take_pending takes PendSV and moves PC to the handler, so the
   * store is never re-executed. The masked case is picked up at the next
   * boundary once TX_RESTORE re-enables interrupts. */
  uint32_t primask = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PRIMASK, &primask);
  if ((primask & 1U) != 0U) {
    return;
  }
  if (exc_priority(uc, (uint32_t)k_exc_pendsv) >= exc_active_prio()) {
    return; /* a higher/equal-priority handler is active -- defer */
  }
  /* Advance PC past the storing instruction before stopping so the PendSV we
   * are about to take stacks the return address of the NEXT instruction -- as
   * real hardware does (PendSV activates after the store retires), not the store
   * itself. Re-stacking the store would re-pend PendSV on every return and spin.
   * Thumb length: a halfword whose top 5 bits are 0b111xx with xx != 00 starts a
   * 32-bit instruction; otherwise it is 16-bit. */
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uint16_t hw0 = 0U;
  (void)uc_mem_read(uc, pc, &hw0, sizeof(hw0));
  const uint32_t op5  = (uint32_t)(hw0 >> 11) & 0x1FU;
  const uint32_t step = ((op5 == 0x1DU) || (op5 == 0x1EU) || (op5 == 0x1FU)) ? 4U : 2U;
  uint32_t       next = pc + step;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  (void)uc_emu_stop(uc);
}

static uint8_t* read_file(const char* path, long* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return nullptr;
  }
  (void)fseek(f, 0, SEEK_END);
  const long len = ftell(f);
  (void)fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)len);
  if ((buf != nullptr) && (fread(buf, 1U, (size_t)len, f) != (size_t)len)) {
    free(buf);
    buf = nullptr;
  }
  (void)fclose(f);
  *out_len = len;
  return buf;
}

/** @brief Load ELF32 PT_LOAD segments into emulated memory at their LMA. */
static int load_elf(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((len < 52) ||
      (memcmp(elf,
              "\x7F"
              "ELF",
              4) != 0) ||
      (elf[4] != 1) /* ELFCLASS32 */) {
    (void)fprintf(stderr, "not a 32-bit ELF\n");
    return -1;
  }
  const uint16_t e_machine = (uint16_t)(elf[18] | (elf[19] << 8));
  if (e_machine != 40 /* EM_ARM */) {
    (void)fprintf(stderr, "ELF e_machine %u != ARM(40)\n", e_machine);
    return -1;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + 28, 4);
  uint16_t phentsize = (uint16_t)(elf[42] | (elf[43] << 8));
  uint16_t phnum     = (uint16_t)(elf[44] | (elf[45] << 8));
  int      loaded    = 0;
  for (uint16_t i = 0U; i < phnum; i++) {
    const uint8_t* ph = elf + phoff + ((uint32_t)i * phentsize);
    uint32_t       p_type;
    uint32_t       p_offset;
    uint32_t       p_paddr;
    uint32_t       p_filesz;
    (void)memcpy(&p_type, ph + 0, 4);
    (void)memcpy(&p_offset, ph + 4, 4);
    (void)memcpy(&p_paddr, ph + 12, 4); /* load address (LMA) */
    (void)memcpy(&p_filesz, ph + 16, 4);
    if ((p_type != 1U /* PT_LOAD */) || (p_filesz == 0U)) {
      continue;
    }
    if (uc_mem_write(uc, p_paddr, elf + p_offset, p_filesz) != UC_ERR_OK) {
      (void)fprintf(stderr, "uc_mem_write seg @0x%08X (%u bytes) failed\n", p_paddr, p_filesz);
      return -1;
    }
    (void)fprintf(stderr, "  loaded %u bytes @ 0x%08X\n", p_filesz, p_paddr);
    loaded++;
  }
  return (loaded > 0) ? 0 : -1;
}

/* ELF32 header / section-header / symbol field offsets (little-endian). */
typedef enum : uint32_t {
  k_ehdr_shoff     = 32U, /**< e_shoff: section-header table offset.    */
  k_ehdr_shentsize = 46U, /**< e_shentsize: bytes per section header.   */
  k_ehdr_shnum     = 48U, /**< e_shnum: section-header count.           */
  k_shdr_type      = 4U,  /**< sh_type.                                 */
  k_shdr_offset    = 16U, /**< sh_offset: section file offset.          */
  k_shdr_size      = 20U, /**< sh_size: section byte size.              */
  k_shdr_link      = 24U, /**< sh_link: associated strtab section index.*/
  k_shdr_entsize   = 36U, /**< sh_entsize: bytes per symbol entry.      */
  k_sym_name       = 0U,  /**< st_name: index into the strtab.          */
  k_sym_value      = 4U,  /**< st_value: symbol address (Thumb bit set).*/
  k_sym_entsize    = 16U, /**< sizeof(Elf32_Sym).                       */
} elf_field_t;

/** @brief Read a 32-bit little-endian word from a raw ELF buffer at @p off. */
static uint32_t elf_rd32(const uint8_t* elf, uint32_t off)
{
  uint32_t v = 0U;
  (void)memcpy(&v, elf + off, sizeof(v));
  return v;
}

/** @brief Read a 16-bit little-endian half from a raw ELF buffer at @p off. */
static uint16_t elf_rd16(const uint8_t* elf, uint32_t off)
{
  return (uint16_t)(elf[off] | ((uint16_t)elf[off + 1U] << 8));
}

/**
 * @brief Resolve a function symbol's address from the ELF .symtab.
 *
 * @details
 * Bounded SHT_SYMTAB walk: scan the section-header table for the symbol table,
 * take its linked string table (sh_link), then compare each entry's st_name
 * (a strtab offset) against @p name. On a match, return st_value with the Thumb
 * bit masked off so it can be used as a Unicorn code address. Every file offset
 * is checked against @p len before it is dereferenced, so a truncated or hostile
 * ELF cannot read out of bounds.
 *
 * @param[in]  elf  Raw ELF image.
 * @param[in]  len  Image length in bytes.
 * @param[in]  name Symbol name to find (NUL-terminated).
 * @param[out] out  Resolved address (Thumb bit cleared) on success.
 * @return true if the symbol was found.
 */
static bool elf_find_symbol(const uint8_t* elf, long len, const char* name, uint64_t* out)
{
  if ((elf == nullptr) || (len < 64) || (name == nullptr) || (out == nullptr)) {
    return false;
  }
  const uint32_t ulen      = (uint32_t)len;
  const uint32_t shoff     = elf_rd32(elf, (uint32_t)k_ehdr_shoff);
  const uint16_t shentsize = elf_rd16(elf, (uint32_t)k_ehdr_shentsize);
  const uint16_t shnum     = elf_rd16(elf, (uint32_t)k_ehdr_shnum);
  if ((shoff == 0U) || (shentsize < 40U) ||
      (shoff + ((uint32_t)shnum * (uint32_t)shentsize) > ulen)) {
    return false;
  }
  const size_t name_len = strlen(name);
  for (uint16_t i = 0U; i < shnum; i++) {
    const uint32_t sh = shoff + ((uint32_t)i * (uint32_t)shentsize);
    if (elf_rd32(elf, sh + (uint32_t)k_shdr_type) != (uint32_t)k_elf_sht_symtab) {
      continue;
    }
    const uint32_t sym_off = elf_rd32(elf, sh + (uint32_t)k_shdr_offset);
    const uint32_t sym_sz  = elf_rd32(elf, sh + (uint32_t)k_shdr_size);
    const uint32_t entsz   = elf_rd32(elf, sh + (uint32_t)k_shdr_entsize);
    const uint32_t strndx  = elf_rd32(elf, sh + (uint32_t)k_shdr_link);
    if ((entsz < (uint32_t)k_sym_entsize) || (sym_off + sym_sz > ulen) || (strndx >= shnum)) {
      continue;
    }
    /* The linked string table holds the symbol names. */
    const uint32_t str_sh  = shoff + (strndx * (uint32_t)shentsize);
    const uint32_t str_off = elf_rd32(elf, str_sh + (uint32_t)k_shdr_offset);
    const uint32_t str_sz  = elf_rd32(elf, str_sh + (uint32_t)k_shdr_size);
    if (str_off + str_sz > ulen) {
      continue;
    }
    for (uint32_t s = 0U; (s + entsz) <= sym_sz; s += entsz) {
      const uint32_t st_name = elf_rd32(elf, sym_off + s + (uint32_t)k_sym_name);
      if ((st_name == 0U) || (st_name >= str_sz)) {
        continue;
      }
      /* Bounded compare: the candidate name must fit before the strtab end. */
      if (((size_t)st_name + name_len + 1U) > (size_t)str_sz) {
        continue;
      }
      if (memcmp(elf + str_off + st_name, name, name_len + 1U) == 0) {
        const uint32_t st_value = elf_rd32(elf, sym_off + s + (uint32_t)k_sym_value);
        *out                    = (uint64_t)(st_value & (uint32_t)k_elf_thumb_clear);
        return true;
      }
    }
  }
  return false;
}

/** @brief Read a 32-bit little-endian word from emulated memory. */
static uint32_t rd32(uc_engine* uc, uint64_t addr)
{
  uint32_t v = 0U;
  (void)uc_mem_read(uc, addr, &v, sizeof(v));
  return v;
}

/** @brief Write a 32-bit little-endian word to emulated memory. */
static void wr32(uc_engine* uc, uint64_t addr, uint32_t v)
{
  (void)uc_mem_write(uc, addr, &v, sizeof(v));
}

/** @brief Read a Unicorn 32-bit register by its UC_ARM_REG_* id. */
static uint32_t reg_get(uc_engine* uc, int reg)
{
  uint32_t v = 0U;
  (void)uc_reg_read(uc, reg, &v);
  return v;
}

/** @brief Write a Unicorn 32-bit register by its UC_ARM_REG_* id. */
static void reg_set(uc_engine* uc, int reg, uint32_t v)
{
  (void)uc_reg_write(uc, reg, &v);
}

/** @brief Priority value (lower = higher) of the active handler, or sentinel. */
static uint32_t exc_active_prio(void)
{
  return (s_exc_depth == 0U) ? (uint32_t)k_exc_prio_none : s_exc_stack[s_exc_depth - 1U];
}

/**
 * @brief Read a system-handler priority byte from an SHPR register.
 *
 * @details
 * Cortex-M packs four 8-bit handler priorities per SHPRn word. SVCall (#11) is
 * byte 3 of SHPR2; PendSV (#14) is byte 2 and SysTick (#15) byte 3 of SHPR3.
 * ThreadX's tx_initialize_low_level programs these (SysTick 0x40, PendSV/SVC
 * 0xFF), and the value drives whether one exception may pre-empt another. The
 * PPB is plain RAM here, so the firmware's stores are simply read back.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_num Exception/vector number (11, 14, or 15).
 * @return The 8-bit configured priority (0 = highest, 0xFF = lowest).
 * @retval 0xFF when @p exc_num is not one of the modelled system handlers.
 *
 * @pre @p uc is an initialised engine with the PPB mapped as RAM.
 * @pre SystemInit / tx_initialize_low_level have programmed SHPR2/SHPR3.
 * @post No register or memory state is modified (read-only).
 * @post The returned value is in [0, 0xFF].
 * @note Sub-priority / priority grouping is ignored -- only the raw byte is
 *       compared, which is sufficient for the SysTick > PendSV nesting ThreadX
 *       relies on.
 * @since 0.1.0
 */
static uint32_t exc_priority(uc_engine* uc, uint32_t exc_num)
{
  if (exc_num == (uint32_t)k_exc_svcall) {
    return (rd32(uc, (uint64_t)k_scb_shpr2) >> (3U * (uint32_t)k_byte_bits)) & 0xFFU;
  }
  if (exc_num == (uint32_t)k_exc_pendsv) {
    return (rd32(uc, (uint64_t)k_scb_shpr3) >> (2U * (uint32_t)k_byte_bits)) & 0xFFU;
  }
  if (exc_num == (uint32_t)k_exc_systick) {
    return (rd32(uc, (uint64_t)k_scb_shpr3) >> (3U * (uint32_t)k_byte_bits)) & 0xFFU;
  }
  return (uint32_t)k_exc_prio_max;
}

/**
 * @brief Enter a Cortex-M exception: stack the basic frame and vector in.
 *
 * @details
 * Reproduces Armv7E-M / Armv8-M exception entry that Unicorn's core does not
 * model. The active stack is chosen exactly as hardware would: PSP when in
 * Thread mode with CONTROL.SPSEL set, else MSP. The 8-word basic frame
 * {R0,R1,R2,R3,R12,LR,PC,xPSR} is pushed with 8-byte alignment (the realign
 * pad is recorded in the stacked xPSR bit 9 so exit can undo it), the banked SP
 * is updated, the core is switched to Handler mode on MSP, LR is loaded with
 * the matching EXC_RETURN, IPSR is set to @p exc_num, and PC is vectored to the
 * handler fetched from the VTOR-relative table. The handler's priority is
 * pushed on the active-exception stack so nesting respects priority.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_num Exception number to take (11, 14, or 15).
 * @param[in]     handler Handler entry address (Thumb bit ignored).
 * @return Nothing.
 *
 * @pre @p uc has MSP/PSP/CONTROL/xPSR readable and the target stack mapped.
 * @pre Taking @p exc_num is permitted now (priority/PRIMASK already checked).
 * @post The core is in Handler mode (IPSR == @p exc_num) running on MSP.
 * @post LR holds a valid EXC_RETURN and the outgoing frame is on the old stack.
 * @note FType is forced (no FP frame); valid because ThreadX keeps FPCA clear.
 * @since 0.1.0
 */
static void exc_enter(uc_engine* uc, uint32_t exc_num, uint32_t handler)
{
  const uint32_t xpsr_in   = reg_get(uc, UC_ARM_REG_XPSR);
  const uint32_t control   = reg_get(uc, UC_ARM_REG_CONTROL);
  const bool     in_thread = (xpsr_in & (uint32_t)k_xpsr_ipsr_mask) == 0U;
  const bool     use_psp   = in_thread && ((control & (uint32_t)k_control_spsel) != 0U);

  const int sp_reg = use_psp ? UC_ARM_REG_PSP : UC_ARM_REG_MSP;
  uint32_t  sp     = reg_get(uc, sp_reg);

  /* Hardware aligns the stack pointer to 8 bytes on entry and flags the pad in
   * the stacked xPSR (bit 9) so the matching exception return can remove it. */
  uint32_t frame_xpsr = xpsr_in;
  if ((sp & 0x4U) != 0U) {
    sp -= 4U;
    frame_xpsr |= (uint32_t)k_xpsr_align9;
  } else {
    frame_xpsr &= ~(uint32_t)k_xpsr_align9;
  }
  sp -= (uint32_t)k_exc_frame_bytes;

  wr32(uc, (uint64_t)sp + 0U, reg_get(uc, UC_ARM_REG_R0));
  wr32(uc, (uint64_t)sp + 4U, reg_get(uc, UC_ARM_REG_R1));
  wr32(uc, (uint64_t)sp + 8U, reg_get(uc, UC_ARM_REG_R2));
  wr32(uc, (uint64_t)sp + 12U, reg_get(uc, UC_ARM_REG_R3));
  wr32(uc, (uint64_t)sp + 16U, reg_get(uc, UC_ARM_REG_R12));
  wr32(uc, (uint64_t)sp + 20U, reg_get(uc, UC_ARM_REG_LR));
  wr32(uc, (uint64_t)sp + 24U, reg_get(uc, UC_ARM_REG_PC));
  wr32(uc, (uint64_t)sp + 28U, frame_xpsr);

  /* Commit the new value of whichever stack the frame went onto. */
  reg_set(uc, sp_reg, sp);

  /* EXC_RETURN encodes where to unstack: Thread/PSP, Thread/MSP, or (when an
   * exception pre-empts another) Handler/MSP. */
  uint32_t exc_ret;
  if (!in_thread) {
    exc_ret = (uint32_t)k_exc_ret_handler;
  } else if (use_psp) {
    exc_ret = (uint32_t)k_exc_ret_psp;
  } else {
    exc_ret = (uint32_t)k_exc_ret_msp;
  }

  /* Handler mode always runs on MSP with CONTROL.SPSEL clear. */
  reg_set(uc, UC_ARM_REG_CONTROL, control & ~(uint32_t)k_control_spsel);
  reg_set(uc, UC_ARM_REG_SP, reg_get(uc, UC_ARM_REG_MSP));

  uint32_t handler_xpsr =
    (xpsr_in & ~(uint32_t)k_xpsr_ipsr_mask) | (exc_num & (uint32_t)k_xpsr_ipsr_mask);
  handler_xpsr |= (uint32_t)k_xpsr_t_bit; /* M-profile is always Thumb. */
  reg_set(uc, UC_ARM_REG_XPSR, handler_xpsr);
  reg_set(uc, UC_ARM_REG_LR, exc_ret);
  reg_set(uc, UC_ARM_REG_PC, handler & ~1U);

  if (s_exc_depth < (uint32_t)k_exc_nest_max) {
    s_exc_stack[s_exc_depth] = exc_priority(uc, exc_num);
    s_exc_depth++;
  }
}

/**
 * @brief Perform a Cortex-M exception return for an observed EXC_RETURN branch.
 *
 * @details
 * The inverse of ::exc_enter. @p exc_return (the magic value the core branched
 * to) selects the stack to unstack from (bit2: PSP vs MSP) and the mode to
 * return to (bit3: Thread vs Handler). The 8-word basic frame is popped, the
 * recorded 8-byte realignment (stacked xPSR bit 9) is undone, the banked SP and
 * CONTROL.SPSEL are restored, xPSR (hence IPSR) is reloaded, the active-
 * exception stack is popped, and PC resumes the interrupted instruction stream.
 *
 * @param[in,out] uc      Unicorn engine.
 * @param[in]     exc_ret The EXC_RETURN value (0xFFFFFFFx) being returned to.
 * @return Nothing.
 *
 * @pre @p uc is in Handler mode with a valid basic frame on the indicated stack.
 * @pre @p exc_ret is in the range [0xFFFFFFF0, 0xFFFFFFFF].
 * @post The core has resumed the unstacked context (PC/SP/xPSR restored).
 * @post The active-exception nesting depth has decreased by one (if non-zero).
 * @note No FP frame is unstacked -- FType is assumed set, matching ::exc_enter.
 * @since 0.1.0
 */
static void exc_return(uc_engine* uc, uint32_t exc_ret)
{
  const bool to_psp    = (exc_ret & (uint32_t)k_exc_ret_spsel) != 0U;
  const bool to_thread = (exc_ret & (uint32_t)k_exc_ret_mode) != 0U;
  const int  sp_reg    = to_psp ? UC_ARM_REG_PSP : UC_ARM_REG_MSP;
  uint32_t   sp        = reg_get(uc, sp_reg);

  const uint32_t r0   = rd32(uc, (uint64_t)sp + 0U);
  const uint32_t r1   = rd32(uc, (uint64_t)sp + 4U);
  const uint32_t r2   = rd32(uc, (uint64_t)sp + 8U);
  const uint32_t r3   = rd32(uc, (uint64_t)sp + 12U);
  const uint32_t r12  = rd32(uc, (uint64_t)sp + 16U);
  const uint32_t lr   = rd32(uc, (uint64_t)sp + 20U);
  const uint32_t pc   = rd32(uc, (uint64_t)sp + 24U);
  const uint32_t xpsr = rd32(uc, (uint64_t)sp + 28U);

  sp += (uint32_t)k_exc_frame_bytes;
  if ((xpsr & (uint32_t)k_xpsr_align9) != 0U) {
    sp += 4U; /* undo the entry-time 8-byte realignment pad */
  }
  reg_set(uc, sp_reg, sp);

  reg_set(uc, UC_ARM_REG_R0, r0);
  reg_set(uc, UC_ARM_REG_R1, r1);
  reg_set(uc, UC_ARM_REG_R2, r2);
  reg_set(uc, UC_ARM_REG_R3, r3);
  reg_set(uc, UC_ARM_REG_R12, r12);
  reg_set(uc, UC_ARM_REG_LR, lr);

  /* Restore mode: on return to Thread, CONTROL.SPSEL follows EXC_RETURN bit2;
   * on return to a pre-empted handler the core stays on MSP. */
  uint32_t control = reg_get(uc, UC_ARM_REG_CONTROL);
  if (to_thread && to_psp) {
    control |= (uint32_t)k_control_spsel;
  } else {
    control &= ~(uint32_t)k_control_spsel;
  }
  reg_set(uc, UC_ARM_REG_CONTROL, control);

  uint32_t new_xpsr = xpsr | (uint32_t)k_xpsr_t_bit;
  if (to_thread) {
    new_xpsr &= ~(uint32_t)k_xpsr_ipsr_mask; /* Thread mode: IPSR == 0 */
  }
  reg_set(uc, UC_ARM_REG_XPSR, new_xpsr);

  /* Active SP becomes whichever stack the returned-to context uses. */
  reg_set(uc, UC_ARM_REG_SP, reg_get(uc, to_psp && to_thread ? UC_ARM_REG_PSP : UC_ARM_REG_MSP));
  reg_set(uc, UC_ARM_REG_PC, pc & ~1U);

  if (s_exc_depth > 0U) {
    s_exc_depth--;
  }
}

/** @brief True if @p pc is an EXC_RETURN magic value (0xFFFFFFF0..0xFFFFFFFF). */
static bool is_exc_return(uint64_t pc)
{
  return (pc & 0xFFFFFFF0ULL) == (uint64_t)k_exc_ret_base;
}

/**
 * @brief Read the handler address for an exception from the vector table.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base used when VTOR reads as 0.
 * @param[in]     exc_num   Exception/vector index to look up.
 * @return Handler entry address with the Thumb bit cleared.
 * @retval 0 when no usable handler is installed at that vector slot.
 *
 * @pre @p uc has the vector table mapped at VTOR (or @p vtor_base).
 * @pre @p exc_num is a valid vector index (< table length).
 * @post No engine state is modified (read-only).
 * @post The returned address (when non-zero) is halfword-aligned code.
 * @note VTOR lives in PPB RAM here, written by SystemInit at boot.
 * @since 0.1.0
 */
static uint32_t exc_vector(uc_engine* uc, uint32_t vtor_base, uint32_t exc_num)
{
  uint32_t vtor = rd32(uc, (uint64_t)k_scb_vtor);
  if (vtor == 0U) {
    vtor = vtor_base;
  }
  const uint32_t handler = rd32(uc, (uint64_t)vtor + (exc_num * 4U)) & ~1U;
  if ((handler == 0U) || (handler == 0xFFFFFFFEU)) {
    return 0U;
  }
  return handler;
}

/**
 * @brief Take the highest-priority pending exception, if one may activate now.
 *
 * @details
 * The software replacement for the NVIC's "take the highest-priority pending,
 * enabled exception whose priority is greater than the current execution
 * priority" rule -- called at every instruction boundary AND immediately after
 * each exception return (so a lower-priority pend tail-chains exactly as
 * hardware would instead of returning to the interrupted code first). Two
 * sources are modelled:
 *
 *   - SysTick (#15): periodic. ::s_systick_pending is armed once per tick
 *     period by the run loop; this routine consumes it and vectors in #15 so
 *     _tx_timer_interrupt runs in real handler context (correct for ThreadX and
 *     for bare-metal SysTick handlers alike). Arming it elsewhere -- rather than
 *     re-deriving "armed" from SYST_CSR on every call -- is what lets a pending
 *     PendSV run between ticks instead of being starved by a perpetual SysTick.
 *   - PendSV (#14): level-pending via ICSR.PENDSVSET (ThreadX's context-switch
 *     request); the bit is cleared on activation, as hardware does.
 *
 * SysTick (priority 0x40) outranks PendSV (0xFF), so when both are pending
 * SysTick activates first and may even pre-empt a PendSV that is spinning in
 * its idle wait -- exactly the nesting ThreadX relies on to make a sleeping
 * thread runnable. PRIMASK and the active-priority stack are both honoured.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base if VTOR reads as 0.
 * @return true if an exception was taken (PC now points at a handler).
 *
 * @pre @p uc has stopped at an instruction boundary or just returned.
 * @pre The PPB (SYST_CSR / ICSR / SHPRn / VTOR) is mapped as RAM.
 * @post At most one exception is taken per call (the highest-priority due one).
 * @post ICSR.PENDSVSET / ::s_systick_pending is cleared iff that one was taken.
 * @note SysTick is dropped (not queued) if SYST_CSR is disarmed when its period
 *       elapses, matching a masked/disabled SysTick on hardware.
 * @since 0.1.0
 */
static bool exc_take_pending(uc_engine* uc, uint32_t vtor_base)
{
  const uint32_t primask = reg_get(uc, UC_ARM_REG_PRIMASK);
  if ((primask & 1U) != 0U) {
    return false; /* interrupts masked -- no exception may be taken now */
  }
  const uint32_t active = exc_active_prio();

  /* SysTick first: highest-priority of the modelled exceptions, so it can
   * pre-empt a lower-priority PendSV that is spinning for a runnable thread. */
  if (s_systick_pending) {
    const bool armed =
      (rd32(uc, (uint64_t)k_syst_csr) & (uint32_t)k_syst_csr_run) == (uint32_t)k_syst_csr_run;
    if (!armed) {
      s_systick_pending = false; /* disabled SysTick: drop the pended tick */
    } else if (exc_priority(uc, (uint32_t)k_exc_systick) < active) {
      const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_systick);
      if (handler != 0U) {
        s_systick_pending = false;
        exc_enter(uc, (uint32_t)k_exc_systick, handler);
        s_systick_fires++;
        return true;
      }
    }
  }

  /* PendSV: taken when the firmware has requested a context switch. */
  uint32_t icsr = rd32(uc, (uint64_t)k_scb_icsr);
  if ((icsr & (1U << (uint32_t)k_icsr_pendsvset)) != 0U) {
    if (exc_priority(uc, (uint32_t)k_exc_pendsv) < active) {
      const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_pendsv);
      if (handler != 0U) {
        /* Hardware clears PENDSVSET when PendSV is activated. */
        icsr &= ~(1U << (uint32_t)k_icsr_pendsvset);
        wr32(uc, (uint64_t)k_scb_icsr, icsr);
        exc_enter(uc, (uint32_t)k_exc_pendsv, handler);
        s_pendsv_takes++;
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief UC_HOOK_CODE stub for the firmware's ra_touch_read.
 *
 * @details
 * Installed at ra_touch_read's entry so the REAL firmware touch->GUIX path runs
 * with board_sim supplying the coordinates -- no GT911/I2C is modelled. AAPCS:
 * R0 = out_points, R1 = max_count, R2 = got_count. If a click is pending, the
 * x/y are written into the first point (x at +0, y at +2 of ra_touch_point_t),
 * 1 is written to *got_count, and R0 is set to k_ra_ok; otherwise *got_count is
 * 0 and R0 is k_ra_ok (an empty-but-successful frame). The stub then returns:
 * PC = LR & ~1 and uc_emu_stop, so the chunked run loop relaunches from the
 * return address. Rewriting PC mid-block and continuing would corrupt Unicorn's
 * block / Thumb state; stop-then-relaunch (as the run loop already does) is the
 * safe way to "return" from an injected stub.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     address   Hooked address (ra_touch_read entry); unused.
 * @param[in]     size      Instruction size; unused.
 * @param[in]     user_data Hook user pointer; unused.
 */
static void touch_read_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user_data)
{
  (void)address;
  (void)size;
  (void)user_data;

  uint32_t r0 = 0U; /* out_points */
  uint32_t r2 = 0U; /* got_count  */
  uint32_t lr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &r2);
  (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);

  if (s_touch_pending && (r0 != 0U) && (r2 != 0U)) {
    const uint16_t x = s_touch_x;
    const uint16_t y = s_touch_y;
    (void)uc_mem_write(uc, (uint64_t)r0 + (uint64_t)k_touch_pt_x_off, &x, sizeof(x));
    (void)uc_mem_write(uc, (uint64_t)r0 + (uint64_t)k_touch_pt_y_off, &y, sizeof(y));
    const uint8_t got = (uint8_t)k_touch_one_point;
    (void)uc_mem_write(uc, (uint64_t)r2, &got, sizeof(got));
    s_touch_pending = false;
    s_touch_injected++;
  } else if (r2 != 0U) {
    const uint8_t got = (uint8_t)k_touch_no_point;
    (void)uc_mem_write(uc, (uint64_t)r2, &got, sizeof(got));
  }

  uint32_t ret = (uint32_t)k_touch_ok; /* k_ra_ok */
  (void)uc_reg_write(uc, UC_ARM_REG_R0, &ret);

  /* "Return" from the stub: jump to LR (Thumb bit cleared) and stop so the run
   * loop relaunches there with a clean block/Thumb state. */
  uint32_t pc = lr & (uint32_t)k_elf_thumb_clear;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_emu_stop(uc);
}

/**
 * @brief UC_HOOK_CODE stub for the firmware's ra_touch_open.
 *
 * @details
 * Forces ra_touch_open to return k_ra_ok without modelling the GT911 product-id
 * probe over I2C (which the sparse MMIO model cannot answer). This makes the
 * emulator reproduce real hardware faithfully: on the EK-RA8D2 ra_touch_open
 * succeeds, so the app's product path -- open succeeds, then poll ra_touch_read
 * every frame -- runs unchanged here, and the read stub (touch_read_hook)
 * supplies the coordinates. Returns by PC = LR & ~1 + uc_emu_stop, the same
 * stop-then-relaunch contract the run loop already relies on.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     address   Hooked address (ra_touch_open entry); unused.
 * @param[in]     size      Instruction size; unused.
 * @param[in]     user_data Hook user pointer; unused.
 */
static void touch_open_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user_data)
{
  (void)address;
  (void)size;
  (void)user_data;
  uint32_t lr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  uint32_t ret = (uint32_t)k_touch_ok; /* k_ra_ok */
  (void)uc_reg_write(uc, UC_ARM_REG_R0, &ret);
  uint32_t pc = lr & (uint32_t)k_elf_thumb_clear;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_emu_stop(uc);
}

/** @brief Pack a 0x00RRGGBB colour into RGB565. */
static uint16_t rgb888_to_565(uint32_t rgb)
{
  const uint32_t r = (rgb >> 16) & 0xFFU;
  const uint32_t g = (rgb >> 8) & 0xFFU;
  const uint32_t b = rgb & 0xFFU;
  return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

/** @brief True if addr is in an emulated RAM region a framebuffer could use. */
static bool addr_is_ram(uint32_t addr)
{
  return (((addr >= 0x20000000U) && (addr < 0x20010000U)) || /* DTCM */
          ((addr >= 0x22000000U) && (addr < 0x22100000U)) || /* SRAM */
          ((addr >= 0x68000000U) && (addr < 0x6C000000U)));  /* SDRAM */
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
  const uint16_t bg = rgb888_to_565(rd32(uc, (uint64_t)k_glcdc_bg_bgc) & 0x00FFFFFFU);
  const size_t   n  = (size_t)width_px * (size_t)height_px;
  for (size_t i = 0U; i < n; i++) {
    fb[i] = bg;
  }

  const uint32_t saddr = rd32(uc, (uint64_t)k_glcdc_gr1_saddr);
  const uint32_t fmt   = (rd32(uc, (uint64_t)k_glcdc_gr1_fmt) >> (uint32_t)k_glcdc_fmt_shift) &
                         (uint32_t)k_glcdc_fmt_mask;
  if (!addr_is_ram(saddr) || (fmt != (uint32_t)k_glcdc_fmt_rgb565)) {
    return; /* no graphics layer -- background-only frame */
  }
  const uint32_t stride = (rd32(uc, (uint64_t)k_glcdc_gr1_flm3) >> (uint32_t)k_glcdc_high_shift) &
                          (uint32_t)k_glcdc_stride_mask;
  const uint32_t lnnum  = (rd32(uc, (uint64_t)k_glcdc_gr1_flm5) >> (uint32_t)k_glcdc_high_shift) &
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
    const uint32_t r5     = (uint32_t)((p >> 11) & 0x1FU);
    const uint32_t g6     = (uint32_t)((p >> 5) & 0x3FU);
    const uint32_t b5     = (uint32_t)(p & 0x1FU);
    const uint8_t  rgb[3] = {(uint8_t)((r5 << 3) | (r5 >> 2)),
                             (uint8_t)((g6 << 2) | (g6 >> 4)),
                             (uint8_t)((b5 << 3) | (b5 >> 2))};
    (void)fwrite(rgb, 1U, 3U, f);
  }
  (void)fclose(f);
  return 0;
}

typedef enum : uint32_t {
  k_panel_line_max = 256U,  /**< Max panel-config line length.        */
  k_panel_name_max = 64U,   /**< Max panel name (incl NUL).           */
  k_panel_dim_max  = 4096U, /**< Sanity cap on a panel dimension.     */
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
 * Same flat ``key = value`` schema as the tools/simulator panel descriptors, so
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
      out->width = (uint16_t)strtol(val, nullptr, 10);
    } else if (strncmp(key, "height", sizeof("height")) == 0) {
      out->height = (uint16_t)strtol(val, nullptr, 10);
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
    (void)fprintf(stderr,
                  "usage: board_sim <firmware.elf> [--view] [--ppm <out.ppm>]"
                  " [--panel <file.toml>] [--size WxH] [--click X Y]\n"
                  "  --view          open a macOS window and show the emulated panel live\n"
                  "  --ppm <file>    write the final frame to a binary PPM (headless ok)\n"
                  "  --panel <file>  display descriptor (name/width/height) to size the window\n"
                  "  --size WxH      frame size in pixels; overrides --panel (default 1024x600)\n"
                  "  --click X Y     headless: inject one touch at X,Y once the UI is up\n");
    return 2;
  }
  const char* elf_path   = argv[1];
  bool        want_view  = false;
  const char* ppm_path   = nullptr;
  const char* panel_path = nullptr;
  bool        size_set   = false;
  bool        want_click = false;
  int         click_x    = -1;
  int         click_y    = -1;
  uint16_t    view_w     = (uint16_t)k_view_default_w;
  uint16_t    view_h     = (uint16_t)k_view_default_h;
  for (int i = 2; i < argc; i++) {
    if (strncmp(argv[i], "--view", sizeof("--view")) == 0) {
      want_view = true;
    } else if ((strncmp(argv[i], "--ppm", sizeof("--ppm")) == 0) && ((i + 1) < argc)) {
      ppm_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--panel", sizeof("--panel")) == 0) && ((i + 1) < argc)) {
      panel_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--click", sizeof("--click")) == 0) && ((i + 2) < argc)) {
      click_x    = (int)strtol(argv[i + 1], nullptr, 10);
      click_y    = (int)strtol(argv[i + 2], nullptr, 10);
      want_click = (click_x >= 0) && (click_y >= 0);
      i += 2;
    } else if ((strncmp(argv[i], "--size", sizeof("--size")) == 0) && ((i + 1) < argc)) {
      char*      end = nullptr;
      const long w   = strtol(argv[i + 1], &end, 10);
      const long h   = ((end != nullptr) && (*end == 'x')) ? strtol(end + 1, nullptr, 10) : 0L;
      if ((w > 0L) && (w <= 4096L) && (h > 0L) && (h <= 4096L)) {
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

  for (size_t i = 0U; i < (sizeof(k_regions) / sizeof(k_regions[0])); i++) {
    if (uc_mem_map(uc, k_regions[i].base, (size_t)k_regions[i].size, UC_PROT_ALL) != UC_ERR_OK) {
      (void)fprintf(stderr,
                    "map %s @0x%08llX failed\n",
                    k_regions[i].name,
                    (unsigned long long)k_regions[i].base);
      return 1;
    }
  }
  if (uc_mmio_map(uc,
                  (uint64_t)k_periph_base,
                  (size_t)k_periph_size,
                  mmio_read,
                  nullptr,
                  mmio_write,
                  nullptr) != UC_ERR_OK) {
    (void)fprintf(stderr, "mmio_map failed\n");
    return 1;
  }

  long           elf_len = 0;
  uint8_t* const elf     = read_file(elf_path, &elf_len);
  if (elf == nullptr) {
    (void)fprintf(stderr, "cannot read %s\n", elf_path);
    return 1;
  }
  (void)fprintf(stderr, "board_sim: loading %s (%ld bytes)\n", elf_path, elf_len);
  if (load_elf(uc, elf, elf_len) != 0) {
    free(elf);
    return 1;
  }
  /* Resolve ra_touch_open / ra_touch_read so UC_HOOK_CODE stubs can short-circuit
   * them and feed the firmware touch coordinates (see touch_open_hook /
   * touch_read_hook). Done before freeing the ELF image since the symbol table
   * lives in the raw file, not in mapped RAM. ra_touch_open is forced to succeed
   * so the app's "open then poll" product path runs exactly as on hardware. */
  const bool have_touch_open = elf_find_symbol(elf, elf_len, "ra_touch_open", &s_touch_open_addr);
  const bool have_touch_read = elf_find_symbol(elf, elf_len, "ra_touch_read", &s_touch_read_addr);
  if (have_touch_read) {
    (void)fprintf(stderr,
                  "board_sim: ra_touch_open @ 0x%08llX  ra_touch_read @ 0x%08llX"
                  " (touch injection armed)\n",
                  (unsigned long long)s_touch_open_addr,
                  (unsigned long long)s_touch_read_addr);
  } else {
    (void)fprintf(stderr, "board_sim: ra_touch_read not found in .symtab -- no touch injection\n");
  }
  free(elf);

  /* Cortex-M reset: SP = vectors[0], PC = vectors[1] (Thumb, clear bit0). */
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)uc_mem_read(uc, k_regions[1].base + 0U, &sp, 4); /* MRAM[0] */
  (void)uc_mem_read(uc, k_regions[1].base + 4U, &pc, 4); /* MRAM[4] (Thumb: bit0=1) */
  /* M-profile is always Thumb (EPSR.T must be 1). Keep the reset vector's bit0
   * and set the xPSR Thumb bit so Unicorn enters Thumb, not ARM, decoding. */
  pc |= 1U;
  uint32_t xpsr = (uint32_t)(1UL << 24); /* xPSR.T */
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
  const uint32_t vtor_base = (uint32_t)k_regions[1].base;

  uc_hook h_invalid;
  uc_hook h_unmapped;
  uc_hook h_intr;
  uc_hook h_icsr;
  (void)uc_hook_add(uc, &h_invalid, UC_HOOK_INSN_INVALID, (void*)on_invalid_insn, nullptr, 1, 0);
  (void)uc_hook_add(uc, &h_unmapped, UC_HOOK_MEM_UNMAPPED, (void*)on_unmapped, nullptr, 1, 0);
  /* SVCall / exception-return: Unicorn raises UC_HOOK_INTR on a Thumb `svc` and
   * on a branch to an EXC_RETURN magic; on_intr vectors / unstacks accordingly. */
  (void)uc_hook_add(uc, &h_intr, UC_HOOK_INTR, (void*)on_intr, nullptr, 1, 0);
  /* Watch the ICSR word so a PENDSVSET store ends the chunk at once, giving
   * PendSV next-instruction activation (see on_icsr_write). ICSR lives in PPB
   * RAM, so this memory-write hook is the only way to observe the request. */
  (void)uc_hook_add(uc,
                    &h_icsr,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_icsr_write,
                    nullptr,
                    (uint64_t)k_scb_icsr,
                    (uint64_t)k_scb_icsr + 3U);

  /* Short-circuit ra_touch_open (force success) and ra_touch_read (supply the
   * click) at their entries so board_sim drives the real firmware touch path
   * without modelling the GT911/I2C. The begin==end address range restricts each
   * UC_HOOK_CODE to that one instruction. */
  uc_hook h_touch_open;
  uc_hook h_touch_read;
  if (have_touch_open) {
    (void)uc_hook_add(uc,
                      &h_touch_open,
                      UC_HOOK_CODE,
                      (void*)touch_open_hook,
                      nullptr,
                      s_touch_open_addr,
                      s_touch_open_addr);
  }
  if (have_touch_read) {
    (void)uc_hook_add(uc,
                      &h_touch_read,
                      UC_HOOK_CODE,
                      (void*)touch_read_hook,
                      nullptr,
                      s_touch_read_addr,
                      s_touch_read_addr);
  }

  /* Optional live window; the frame buffer also backs the --ppm snapshot. */
  board_view_t* view  = nullptr;
  uint16_t*     frame = nullptr;
  if (want_view) {
    view = board_view_open(view_w, view_h, win_title);
    if (view == nullptr) {
      (void)fprintf(stderr, "board_sim: could not open window; continuing headless\n");
    }
  }
  if ((view != nullptr) || (ppm_path != nullptr) || want_click) {
    frame = (uint16_t*)malloc((size_t)view_w * (size_t)view_h * sizeof(uint16_t));
  }

  /* Headless --click: arm the pending click up front. ra_touch_read is only
   * called from the render loop (after the UI is fully built), so the hook
   * consumes it on the first loop iteration -- exactly the tab the firmware's
   * GUIX hit-test resolves it to. After it lands, a few more chunks let
   * gx_host_pump dispatch the pen events and gx_system_canvas_refresh repaint. */
  if (want_click && have_touch_read) {
    s_touch_x       = (uint16_t)click_x;
    s_touch_y       = (uint16_t)click_y;
    s_touch_pending = true;
    (void)fprintf(stderr, "board_sim: --click armed at (%d,%d)\n", click_x, click_y);
  }
  enum : uint32_t {
    /* ra_delay_ms(16) in the render loop spins on SysTick, which advances one
     * tick per chunk, so ~16 chunks == one loop iteration. Give the injected
     * tap many iterations to flow DOWN -> UP -> CLICKED -> tab switch -> repaint. */
    k_click_settle_chunks = 512U, /**< Extra chunks after the click lands.    */
  };

  /* Chunked run: emulate a block, take a SysTick (and any pending PendSV),
   * repeat. Within a chunk, exception returns (a handler's "BX lr" into an
   * EXC_RETURN magic) and SVCalls are resolved inline -- the inner loop unstacks
   * / vectors and relaunches from the new PC without consuming a chunk -- so a
   * context switch does not cost a whole scheduling quantum and the once-per-
   * chunk SysTick cadence (one ThreadX tick) is preserved. Headless runs stop on
   * a chunk budget + wall-clock guard; in --view the loop runs until the window
   * is closed, presenting the live GLCDC output every k_view_present_every. */
  const uint32_t max_chunks =
    (view != nullptr) ? (uint32_t)k_view_max_chunks : (uint32_t)k_run_max_chunks;
  const clock_t t0          = clock();
  uc_err        err         = UC_ERR_OK;
  uint32_t      run_pc      = pc;
  uint32_t      chunks      = 0U;
  bool          timed_out   = false;
  bool          closed      = false;
  uint32_t      settle_left = 0U; /* >0 once the click landed: chunks to drain. */
  for (; chunks < max_chunks; chunks++) {
    /* Each outer chunk is one SysTick period: arm the periodic tick so the
     * boundary (or a tail-chain) takes it once interrupts permit. */
    s_systick_pending = true;

    /* Inner loop: run a chunk, then service exceptions to a steady state before
     * the next chunk. An EXC_RETURN branch is unstacked and the NVIC re-checked
     * so a still-pending lower-priority exception tail-chains (e.g. PendSV right
     * after SysTick) exactly as hardware would, instead of briefly resuming the
     * interrupted code. SVCall (taken in on_intr) just leaves PC at the handler,
     * which the next relaunch runs. */
    bool faulted = false;
    for (uint32_t inner = 0U; inner < (uint32_t)k_run_inner_max; inner++) {
      err = uc_emu_start(uc, (uint64_t)run_pc | 1U, 0, 0, (size_t)k_run_chunk_insns);
      (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
      if (s_exc_return_hit) {
        s_exc_return_hit = false;
        exc_return(uc, (uint32_t)s_exc_return_pc);
        (void)exc_take_pending(uc, vtor_base); /* tail-chain the next pend */
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      if (err != UC_ERR_OK) {
        faulted = true;
        break;
      }
      /* Chunk budget reached at an instruction boundary: take the highest-
       * priority pending exception (SysTick this period, and/or PendSV). If one
       * was taken, run it (and resolve its return) within this same chunk so a
       * context switch does not cost a whole scheduling quantum. */
      if (exc_take_pending(uc, vtor_base)) {
        (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
        continue;
      }
      break;
    }
    if (faulted) {
      break;
    }
    if (view != nullptr) {
      /* Live window: each left mouse-down becomes the next ra_touch_read. */
      uint16_t cx = 0U;
      uint16_t cy = 0U;
      if (have_touch_read && board_view_poll_click(view, &cx, &cy)) {
        s_touch_x       = cx;
        s_touch_y       = cy;
        s_touch_pending = true;
      }
      if ((chunks % (uint32_t)k_view_present_every) == 0U) {
        build_frame(uc, frame, view_w, view_h);
        board_view_present(view, frame, view_w, view_h);
      }
      if (board_view_pump(view)) {
        closed = true;
        break;
      }
    } else if (want_click) {
      /* Headless --click: run a bounded tail after the injected tap lands, then
       * stop deterministically so the dumped frame shows the switched tab. */
      if (s_touch_injected > 0U) {
        settle_left = (settle_left == 0U) ? (uint32_t)k_click_settle_chunks : (settle_left - 1U);
        if (settle_left == 1U) {
          break;
        }
      }
      if (((double)(clock() - t0) / (double)CLOCKS_PER_SEC) >= (double)k_run_wall_s) {
        timed_out = true;
        break;
      }
    } else if (((double)(clock() - t0) / (double)CLOCKS_PER_SEC) >= (double)k_run_wall_s) {
      timed_out = true;
      break;
    }
  }

  (void)fprintf(stderr,
                "\nboard_sim: stopped -- %s%s\n",
                uc_strerror(err),
                timed_out ? " (wall-clock budget reached)" : "");
  (void)fprintf(stderr, "  final PC      : 0x%08X\n", run_pc);
  (void)fprintf(stderr, "  chunks run    : %u   SysTick ticks: %u\n", chunks, s_systick_fires);
  (void)fprintf(stderr,
                "  exceptions    : %u PendSV  %u SVCall (real Cortex-M entry/return)\n",
                s_pendsv_takes,
                s_svc_takes);
  (void)fprintf(stderr, "  touch clicks  : %u injected via ra_touch_read\n", s_touch_injected);
  (void)fprintf(stderr,
                "  MMIO reads    : %u   writes: %u   distinct addrs: %u\n",
                s_mmio_reads,
                s_mmio_writes,
                s_mmio_n);
  /* GLCDC colour-cycle witness: BG_BGC write count + the distinct colours. */
  (void)fprintf(stderr,
                "  BG_BGC writes : %u   distinct colours: %u   [",
                s_bgc_writes,
                s_bgc_distinct_n);
  for (uint32_t i = 0U; i < s_bgc_distinct_n; i++) {
    (void)fprintf(stderr, "%s0x%06X", (i == 0U) ? "" : " ", s_bgc_distinct[i]);
  }
  (void)fprintf(stderr, "]\n");
  (void)fprintf(stderr, "    %-12s %10s %10s %12s\n", "addr", "reads", "writes", "last-write");
  const bool     truncated = (s_mmio_n > (uint32_t)k_mmio_print_max);
  const uint32_t shown     = truncated ? (uint32_t)k_mmio_print_max : s_mmio_n;
  for (uint32_t i = 0U; i < shown; i++) {
    if (s_mmio_written[i]) {
      (void)fprintf(stderr,
                    "    0x%08llX %10u %10u   0x%08X\n",
                    (unsigned long long)s_mmio_addr[i],
                    s_mmio_rcount[i],
                    s_mmio_wcount[i],
                    s_mmio_val[i]);
    } else {
      (void)fprintf(stderr,
                    "    0x%08llX %10u %10u %12s\n",
                    (unsigned long long)s_mmio_addr[i],
                    s_mmio_rcount[i],
                    s_mmio_wcount[i],
                    "-");
    }
  }
  if (truncated) {
    (void)fprintf(stderr, "    ... (%u more)\n", s_mmio_n - shown);
  }
  if ((err == UC_ERR_OK) || timed_out) {
    (void)fprintf(stderr,
                  "  => firmware EXECUTED to the run budget (no invalid opcode / fault).\n");
  }

  if ((ppm_path != nullptr) && (frame != nullptr)) {
    build_frame(uc, frame, view_w, view_h);
    if (write_ppm(ppm_path, frame, view_w, view_h) == 0) {
      (void)fprintf(stderr, "  wrote %s (%ux%u)\n", ppm_path, (unsigned)view_w, (unsigned)view_h);
    } else {
      (void)fprintf(stderr, "  could not write %s\n", ppm_path);
    }
  }
  if (view != nullptr) {
    if (!closed) { /* run ended on its own -- keep the last frame up until closed */
      build_frame(uc, frame, view_w, view_h);
      board_view_present(view, frame, view_w, view_h);
      (void)fprintf(stderr, "board_sim: run ended; close the window to exit\n");
      while (!board_view_pump(view)) {
        (void)usleep((useconds_t)k_view_idle_us);
      }
    }
    board_view_close(view);
  }
  free(frame);
  (void)uc_close(uc);
  return 0;
}
