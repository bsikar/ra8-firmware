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
#include <sys/stat.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unistd.h>

#include "board_net.h"
#include "board_overlay.h"
#include "board_periph.h"
#include "board_periph_sd.h"
#include "board_usb.h"
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
  k_nvic_ipr_base  = 0xE000E400UL, /**< NVIC IPR priority bytes (one per IRQ).*/
  k_nvic_ispr_base = 0xE000E200UL, /**< NVIC ISPR set-pending (per-IRQ bit).  */
  k_nvic_iser_base = 0xE000E100UL, /**< NVIC ISER set-enable array base.      */
  k_nvic_icer_base = 0xE000E180UL, /**< NVIC ICER clear-enable array base.    */
  k_nvic_en_words  = 8UL,          /**< ISER/ICER words modelled (256 lines). */
  k_nvic_en_span   = 8UL * 4UL,    /**< Byte span of one set/clear array.     */
  k_exc_irq_vec0   = 16UL,         /**< Vector index of IRQ0 (16 + IRQn).     */
} cortexm_scs_t;

/* Armv7E-M / Armv8-M exception model constants (the part Unicorn's Cortex-M33
 * core leaves to software here -- it has no NVIC/exception unit). EXC_RETURN
 * magic values steer the unstack: bit2 picks the return stack (1 = PSP, 0 =
 * MSP), bit3 the return mode (1 = Thread, 0 = Handler), bit4 the frame type
 * (1 = no FP extended frame). ThreadX runs with FPCA clear so FType is always
 * 1 and no S0-S31 are stacked -- see _tx_thread_schedule (TST LR,#0x10 skips
 * the VFP save/restore for these values). */
typedef enum : uint32_t {
  k_exc_frame_words  = 8U,          /**< {R0-R3,R12,LR,PC,xPSR} basic frame.  */
  k_exc_frame_bytes  = 32U,         /**< 8 words * 4 bytes.                    */
  k_exc_ret_base     = 0xFFFFFFF0U, /**< EXC_RETURN values live in [F0..FF].  */
  k_exc_ret_handler  = 0xFFFFFFF1U, /**< Return to Handler mode, MSP.         */
  k_exc_ret_msp      = 0xFFFFFFF9U, /**< Return to Thread mode, MSP.          */
  k_exc_ret_psp      = 0xFFFFFFFDU, /**< Return to Thread mode, PSP.          */
  k_exc_ret_spsel    = 0x4U,        /**< EXC_RETURN bit2: return stack = PSP. */
  k_exc_ret_mode     = 0x8U,        /**< EXC_RETURN bit3: return to Thread.   */
  k_control_spsel    = 0x2U,        /**< CONTROL.SPSEL: thread SP = PSP.      */
  k_xpsr_t_bit       = 0x01000000U, /**< xPSR.T (Thumb) -- must stay set.     */
  k_xpsr_align9      = 0x00000200U, /**< xPSR bit9: stack-frame realignment.  */
  k_xpsr_ipsr_mask   = 0x000001FFU, /**< xPSR[8:0] = IPSR (active exception).  */
  k_exc_prio_none    = 0x100U,      /**< Sentinel "no handler active" prio.   */
  k_exc_prio_max     = 0xFFU,       /**< Lowest configurable priority value.  */
  k_exc_nest_max     = 4U,          /**< Tracked active-exception nesting cap.*/
  k_byte_bits        = 8U,          /**< Bits per byte (SHPR field width).   */
  k_frame_off_r3     = 12U,         /**< Basic exception-frame offset of R3. */
  k_frame_off_lr     = 20U,         /**< Basic exception-frame offset of LR. */
  k_frame_off_pc     = 24U,         /**< Basic exception-frame offset of PC. */
  k_frame_off_xpsr   = 28U,         /**< Basic exception-frame offset of xPSR.*/
  k_exc_ret_grp_mask = 0xFFFFFFF0U, /**< Masks a PC to the EXC_RETURN group. */
  k_vector_erased    = 0xFFFFFFFEU, /**< Erased-flash / invalid vector word. */
  k_nvic_prio_shift  = 4U,          /**< Implemented priority is the 4 MSBs. */
  k_lo4_mask         = 0xFU,        /**< Low nibble (register / cond field). */
} cortexm_exc_t;

/**
 * @enum board_sim_misc_t
 * @brief Named constants for ELF parsing, Thumb decode, and assorted literals.
 */
typedef enum : uint32_t {
  /* ELF32 layout, in bytes. */
  k_elf_ehdr_size      = 52U, /**< ELF32 file-header size.            */
  k_elf_em_arm         = 40U, /**< e_machine == EM_ARM.               */
  k_elf_e_phoff_off    = 28U, /**< e_phoff in the file header.        */
  k_elf_ph_paddr_off   = 12U, /**< p_paddr in a program header.       */
  k_elf_shentsize_min  = 40U, /**< ELF32 section-header entry size.   */
  k_elf_sh_size_off    = 20U, /**< sh_size in a section header.       */
  k_elf_sh_link_off    = 24U, /**< sh_link in a section header.       */
  k_elf_sh_entsize_off = 36U, /**< sh_entsize in a section header.    */
  /* Thumb / conditional-select instruction decode. */
  k_thumb_op5_shift = 11U,   /**< op5 = hw0[15:11].                  */
  k_thumb_op5_mask  = 0x1FU, /**< 5-bit op5 field.                   */
  k_thumb32_op5_min = 0x1DU, /**< op5 >= this -> 32-bit instruction. */
  k_cs_op_shift     = 12U,   /**< CSEL-family op = hw2[13:12].       */
  k_cs_op_mask      = 0x3U,  /**< 2-bit op field.                    */
  /* Assorted. */
  k_u32_all_ones    = 0xFFFFFFFFU, /**< All bits set (MMIO read toggle).   */
  k_ra_err_no_data  = 0x10AU,      /**< ra_err_t value: no RX data.        */
  k_eth_link_speed  = 100U,        /**< Link speed (Mbps) in the link blob.*/
  k_eth_bmsr_lo     = 0x2DU,       /**< PHY BMSR low byte.                 */
  k_eth_bmsr_hi     = 0x78U,       /**< PHY BMSR high byte.               */
  k_strtol_base10   = 10U,         /**< Base-10 radix for strtol.          */
  k_max_panel_px    = 4096U,       /**< Largest accepted --size dimension. */
  k_record_dir_mode = 0755U,       /**< mkdir mode for the --record dir.   */
  k_byte_mask       = 0xFFU,       /**< Low 8 bits of a value (one byte).  */
} board_sim_misc_t;

/* Touch on the EK-RA8D2 is now modelled end-to-end: the firmware's real
 * ra_touch_open / ra_touch_read run unchanged and drive the GoodIX GT911 over
 * ra_i3c_transfer (the I3C peripheral in legacy I2C mode), which board_periph
 * models as an I2C bus with a GT911 device. board_sim feeds --click / window
 * clicks into that device (board_periph_touch_inject), so a tap returns through
 * the genuine ra_touch -> I3C -> GT911 path -- there is no function-level stub. */

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
  /* --record settings. One outer chunk advances SysTick once (~1 ms of emulated
   * time at the firmware's 1 kHz tick), so ~1000 chunks == one emulated second.
   * Recording dumps a frame every k_record_every chunks for k_record_fps fps. */
  k_record_ms_per_sec = 1000U, /**< Emulated ms (= chunks) per second.      */
  k_record_fps        = 20U,   /**< Recorded frames per emulated second.    */
  k_record_every      = 50U,   /**< Chunks between recorded frames (1000/20).*/
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
  (void)user;
  s_mmio_reads++;
  /* A modelled peripheral block answers first; the sparse fallback below is
   * only reached for addresses no block in board_periph owns. */
  bool           handled = false;
  const uint64_t modeled = board_periph_read(uc, (uint64_t)k_periph_base + offset, size, &handled);
  if (handled) {
    return modeled;
  }
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
  s_mmio_toggle ^= (uint32_t)k_u32_all_ones;
  return (uint64_t)s_mmio_toggle;
}

/**
 * @brief Side-effect-free read of the last value written to a peripheral reg.
 *
 * @details
 * Returns the value last written to @p addr, or 0 if it was never written --
 * searching the MMIO shadow WITHOUT allocating a slot and WITHOUT advancing the
 * spin-settle toggle that ::mmio_read uses. board_sim's own introspection (e.g.
 * ::build_frame reading GLCDC registers to compose the panel) must see stable
 * state: a firmware that never programs the GLCDC (blink, USB, UART demos) would
 * otherwise read the status-poll fallthrough (an alternating 0/0xFFFFFFFF), which
 * made the panel strobe black<->white every frame. A real read of an unwritten
 * register reset-defaults to 0 here, so the panel is a steady background.
 */
static uint32_t mmio_peek(uint64_t addr)
{
  for (uint32_t i = 0U; i < s_mmio_n; i++) {
    if (s_mmio_addr[i] == addr) {
      return s_mmio_written[i] ? s_mmio_val[i] : 0U;
    }
  }
  return 0U;
}

static void mmio_write(uc_engine* uc, uint64_t offset, unsigned size, uint64_t value, void* user)
{
  (void)user;
  s_mmio_writes++;
  if (((uint64_t)k_periph_base + offset) == (uint64_t)k_glcdc_bg_bgc) {
    bgc_track((uint32_t)value);
  }
  /* A modelled peripheral block consumes the write first (so GPIO latches,
   * timer control, and ICU event links take real effect); the sparse fallback
   * still records the write for the MMIO table and unmodelled blocks. */
  bool handled = false;
  board_periph_write(uc, (uint64_t)k_periph_base + offset, size, value, &handled);
  if (handled) {
    return;
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
 * the firmware's event_post on the touch path -- so the invalid-instruction
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
/**
 * @enum arm_cond_t
 * @brief ARM/Thumb 4-bit condition-code field encodings (cond[3:0]).
 */
typedef enum : uint32_t {
  k_cond_eq = 0x0U, /**< Equal (Z==1).               */
  k_cond_ne = 0x1U, /**< Not equal (Z==0).           */
  k_cond_cs = 0x2U, /**< Carry set / unsigned >=.    */
  k_cond_cc = 0x3U, /**< Carry clear / unsigned <.   */
  k_cond_mi = 0x4U, /**< Negative.                   */
  k_cond_pl = 0x5U, /**< Positive or zero.           */
  k_cond_vs = 0x6U, /**< Overflow set.               */
  k_cond_vc = 0x7U, /**< Overflow clear.             */
  k_cond_hi = 0x8U, /**< Unsigned higher.            */
  k_cond_ls = 0x9U, /**< Unsigned lower or same.     */
  k_cond_ge = 0xAU, /**< Signed >=.                  */
  k_cond_lt = 0xBU, /**< Signed <.                   */
  k_cond_gt = 0xCU, /**< Signed >.                   */
  k_cond_le = 0xDU, /**< Signed <=.                  */
  k_cond_al = 0xEU, /**< Always.                     */
} arm_cond_t;

static bool cond_holds(uint32_t cond, uint32_t xpsr)
{
  const bool n = ((xpsr >> (uint32_t)k_apsr_n) & 1U) != 0U;
  const bool z = ((xpsr >> (uint32_t)k_apsr_z) & 1U) != 0U;
  const bool c = ((xpsr >> (uint32_t)k_apsr_c) & 1U) != 0U;
  const bool v = ((xpsr >> (uint32_t)k_apsr_v) & 1U) != 0U;
  switch (cond & (uint32_t)k_lo4_mask) {
    case k_cond_eq:
      return z;
    case k_cond_ne:
      return !z;
    case k_cond_cs:
      return c;
    case k_cond_cc:
      return !c;
    case k_cond_mi:
      return n;
    case k_cond_pl:
      return !n;
    case k_cond_vs:
      return v;
    case k_cond_vc:
      return !v;
    case k_cond_hi:
      return c && !z;
    case k_cond_ls:
      return !c || z;
    case k_cond_ge:
      return n == v;
    case k_cond_lt:
      return n != v;
    case k_cond_gt:
      return !z && (n == v);
    case k_cond_le:
      return z || (n != v);
    default:
      return true; /* AL (k_cond_al) / 0xF */
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
  const uint32_t rn   = (uint32_t)(hw1 & (uint32_t)k_lo4_mask);
  const uint32_t op   = (uint32_t)((hw2 >> (uint32_t)k_cs_op_shift) & (uint32_t)k_cs_op_mask);
  const uint32_t rd   = (uint32_t)((hw2 >> 8) & (uint32_t)k_lo4_mask);
  const uint32_t cond = (uint32_t)((hw2 >> 4) & (uint32_t)k_lo4_mask);
  const uint32_t rm   = (uint32_t)(hw2 & (uint32_t)k_lo4_mask);

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

/**
 * @brief Emulate an Armv8-M memory barrier (DSB/DMB/ISB) as a NOP if present.
 *
 * @details
 * Some Unicorn builds -- e.g. 2.0.1, as packaged on the Linux CI runner -- do
 * not decode the self-synchronising barrier instructions DSB / DMB / ISB and
 * trap them as invalid, where a newer build executes them. They have no
 * architectural effect in this single-threaded, in-order emulator (there is no
 * real memory ordering or pipeline to enforce), so recognising the encoding and
 * advancing PC past the 4-byte instruction is a faithful NOP. This keeps the
 * firmware's boot-path barriers (e.g. after a clock / SDRAM register write) from
 * faulting regardless of the host Unicorn version. Anything else is left
 * untouched.
 *
 * @param[in,out] uc   Unicorn engine.
 * @param[in]     pc   Address of the trapped instruction.
 * @param[in]     code The 4 instruction bytes already read at @p pc.
 * @return true if a DSB/DMB/ISB barrier was recognised and PC advanced past it.
 */
static bool emulate_barrier(uc_engine* uc, uint32_t pc, const uint8_t* code)
{
  enum : uint16_t {
    k_barrier_hw1       = 0xF3BFU, /**< First half-word of DSB/DMB/ISB.       */
    k_barrier_hw2_mask  = 0xFF00U, /**< Fixed high byte of the second h-word. */
    k_barrier_hw2_match = 0x8F00U, /**< 0x8F: the barrier group.              */
    k_barrier_op_mask   = 0x00F0U, /**< Barrier subtype field, bits [7:4].    */
    k_barrier_op_dsb    = 0x0040U, /**< DSB.                                  */
    k_barrier_op_dmb    = 0x0050U, /**< DMB.                                  */
    k_barrier_op_isb    = 0x0060U, /**< ISB.                                  */
    k_barrier_len       = 0x0004U, /**< Thumb-2 barrier instruction length.   */
  };
  const uint16_t hw1 = (uint16_t)(code[0] | ((uint16_t)code[1] << 8));
  const uint16_t hw2 = (uint16_t)(code[2] | ((uint16_t)code[3] << 8));
  if ((hw1 != (uint16_t)k_barrier_hw1) ||
      ((hw2 & (uint16_t)k_barrier_hw2_mask) != (uint16_t)k_barrier_hw2_match)) {
    return false;
  }
  const uint16_t op = (uint16_t)(hw2 & (uint16_t)k_barrier_op_mask);
  if ((op != (uint16_t)k_barrier_op_dsb) && (op != (uint16_t)k_barrier_op_dmb) &&
      (op != (uint16_t)k_barrier_op_isb)) {
    return false;
  }
  const uint32_t next = pc + (uint32_t)k_barrier_len;
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
   * GCC emits those for branchless index math on the touch path, so
   * execute them here. emulate_cond_select writes Rd and advances PC past the
   * 4-byte instruction; then uc_emu_stop so the chunked run loop relaunches from
   * the new PC -- editing PC and continuing in-place corrupts Unicorn's block /
   * Thumb state (it then misdecodes the next valid instruction), so the
   * stop-then-relaunch contract the SysTick / touch stubs use is required here. */
  if (emulate_cond_select(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true; /* handled -- run loop resumes at the advanced PC */
  }

  /* Older Unicorn builds (the runner's 2.0.1) trap DSB/DMB/ISB as invalid; a
   * barrier is a NOP in this emulator, so advance past it and relaunch. */
  if (emulate_barrier(uc, pc, code)) {
    (void)uc_emu_stop(uc);
    return true; /* handled -- run loop resumes past the barrier */
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
  const uint32_t op5  = (uint32_t)(hw0 >> (uint32_t)k_thumb_op5_shift) & (uint32_t)k_thumb_op5_mask;
  const uint32_t step = (op5 >= (uint32_t)k_thumb32_op5_min) ? 4U : 2U;
  uint32_t       next = pc + step;
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &next);
  (void)uc_emu_stop(uc);
}

/**
 * @brief UC_HOOK_MEM_WRITE handler for the NVIC ISER / ICER arrays.
 *
 * @details
 * The NVIC set-enable (ISER) and clear-enable (ICER) registers are not normal
 * read/write words: a written 1 sets (ISER) or clears (ICER) that interrupt
 * line and a written 0 has no effect, so independent stores accumulate. The PPB
 * is mapped as plain RAM here, so the raw store would overwrite the whole word
 * and drop every other enabled line -- which breaks any firmware that enables
 * more than one line (e.g. the SCI RXI + TXI + TEI of the interrupt-driven UART
 * path, or several USB controller lines later). This hook decodes the written
 * bits and folds them into board_periph's authoritative enable shadow, which the
 * ICU model consults when deciding whether to pend a line. The raw RAM word is
 * left as-is (nothing reads ISER/ICER back on the modelled paths).
 *
 * @param[in,out] uc    Unicorn engine; unused (state lives in board_periph).
 * @param[in]     type  Memory access type (write); unused.
 * @param[in]     addr  The ISER/ICER word being written.
 * @param[in]     size  Access width in bytes; unused.
 * @param[in]     value The bit-mask the firmware is setting/clearing.
 * @param[in]     user  Hook user pointer; unused.
 * @return Nothing.
 * @since 0.1.0
 */
static void on_nvic_en_write(uc_engine*  uc,
                             uc_mem_type type,
                             uint64_t    addr,
                             int         size,
                             int64_t     value,
                             void*       user)
{
  (void)uc;
  (void)type;
  (void)size;
  (void)user;
  const bool     is_set = (addr >= (uint64_t)k_nvic_iser_base) &&
                          (addr < ((uint64_t)k_nvic_iser_base + (uint64_t)k_nvic_en_span));
  const uint64_t base   = is_set ? (uint64_t)k_nvic_iser_base : (uint64_t)k_nvic_icer_base;
  const uint32_t word   = (uint32_t)((addr - base) / 4U);
  const uint32_t bits   = (uint32_t)value;
  for (uint32_t b = 0U; b < 32U; b++) {
    if ((bits & (1U << b)) != 0U) {
      board_periph_nvic_set_enable((word * 32U) + b, is_set);
    }
  }
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

/* =============================================================================
 * Ethernet frame seam -- shim the firmware's ra_eth_* API to the virtual
 * network peer (board_net) instead of modelling the GWCA DMA. main.c hooks the
 * three exported functions by their ELF symbol address; each hook emulates the
 * call (reads the AAPCS argument registers, moves the frame, sets the return
 * value) and returns to the caller via LR.
 * =============================================================================
 */

/** @brief Max Ethernet frame the seam marshals between guest memory and board_net. */
enum : uint32_t {
  k_eth_seam_buf = 1600U,
};

/** @brief Resolve a symbol from the ELF .symtab (defined below load_elf). */
static uint32_t elf_sym_addr(const uint8_t* elf, long len, const char* name);

/** @brief Emulate "return r0;" from a hooked function: set R0, branch to LR. */
static void eth_hook_return(uc_engine* uc, uint32_t r0)
{
  uint32_t lr = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_LR, &lr);
  (void)uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uint32_t pc = lr & ~1U; /* drop the Thumb bit; the M-class core stays Thumb. */
  (void)uc_reg_write(uc, UC_ARM_REG_PC, &pc);
  (void)uc_emu_stop(uc); /* relaunch from the returned PC (chunk contract). */
}

/** @brief Hook for ra_eth_link_status(out): report the link up at 100 Mb FD. */
static void on_eth_link_status(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t out = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &out);
  if (out != 0U) {
    /* ra_eth_link_t = { u8 link_up; u16 speed_mbps; u8 full_duplex; u16 bmsr }. */
    const uint8_t link[8] = {1U,
                             0U,
                             (uint8_t)k_eth_link_speed,
                             0U,
                             1U,
                             0U,
                             (uint8_t)k_eth_bmsr_lo,
                             (uint8_t)k_eth_bmsr_hi};
    (void)uc_mem_write(uc, out, link, sizeof(link));
  }
  eth_hook_return(uc, 0U); /* k_ra_ok */
}

/** @brief Hook for ra_eth_write(buf, len): forward the frame to the peer. */
static void on_eth_write(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t buf = 0U;
  uint32_t len = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &buf);
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &len);
  if ((len > 0U) && (len <= (uint32_t)k_eth_seam_buf)) {
    uint8_t frame[k_eth_seam_buf];
    if (uc_mem_read(uc, buf, frame, len) == UC_ERR_OK) {
      board_net_on_tx(frame, len);
    }
  }
  eth_hook_return(uc, 0U); /* k_ra_ok */
}

/** @brief Hook for ra_eth_read(buf, max, got): deliver a peer frame if any. */
static void on_eth_read(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  uint32_t buf = 0U;
  uint32_t max = 0U;
  uint32_t got = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_R0, &buf);
  (void)uc_reg_read(uc, UC_ARM_REG_R1, &max);
  (void)uc_reg_read(uc, UC_ARM_REG_R2, &got);
  uint8_t        frame[k_eth_seam_buf];
  const uint32_t cap = (max < (uint32_t)k_eth_seam_buf) ? max : (uint32_t)k_eth_seam_buf;
  const uint32_t n   = board_net_poll_rx(frame, cap);
  if (n > 0U) {
    (void)uc_mem_write(uc, buf, frame, n);
    (void)uc_mem_write(uc, got, &n, 4U);
    eth_hook_return(uc, 0U); /* k_ra_ok */
  } else {
    const uint32_t zero = 0U;
    (void)uc_mem_write(uc, got, &zero, 4U);
    eth_hook_return(uc, (uint32_t)k_ra_err_no_data);
  }
}

/** @brief Hook that just returns k_ra_ok -- shims a HW bring-up to a no-op. */
static void on_eth_ok(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)address;
  (void)size;
  (void)user;
  eth_hook_return(uc, 0U); /* k_ra_ok */
}

/** @brief Hook one symbol (if present) to @p cb; record it for the report. */
static void eth_seam_hook(uc_engine* uc, const uint8_t* elf, long len, const char* name, void* cb)
{
  const uint32_t addr = elf_sym_addr(elf, len, name);
  if (addr == 0U) {
    return;
  }
  static uc_hook  handles[8];
  static uint32_t n;
  if (n < (uint32_t)(sizeof(handles) / sizeof(handles[0]))) {
    (void)uc_hook_add(uc, &handles[n], UC_HOOK_CODE, cb, nullptr, addr, addr);
    n++;
  }
}

/** @brief Trace hook: log a function entry (user = name) without altering it. */
/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
static void on_net_trace(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)uc;
  (void)size;
  (void)fprintf(stderr, "  [nettrace] %s @ 0x%08X\n", (const char*)user, (unsigned)address);
}

/** @brief Add an entry-trace hook for @p name (if present); leaves it running. */
static void eth_trace_hook(uc_engine* uc, const uint8_t* elf, long len, const char* name)
{
  const uint32_t addr = elf_sym_addr(elf, len, name);
  if (addr == 0U) {
    return;
  }
  static uc_hook  th[4];
  static uint32_t tn;
  if (tn < (uint32_t)(sizeof(th) / sizeof(th[0]))) {
    (void)uc_hook_add(uc, &th[tn], UC_HOOK_CODE, (void*)on_net_trace, (void*)name, addr, addr);
    tn++;
  }
}

/** @brief Install the ra_eth frame-seam hooks if the symbols are present. */
static void eth_seam_install(uc_engine* uc, const uint8_t* elf, long len, bool trace)
{
  const uint32_t w = elf_sym_addr(elf, len, "ra_eth_write");
  const uint32_t r = elf_sym_addr(elf, len, "ra_eth_read");
  const uint32_t l = elf_sym_addr(elf, len, "ra_eth_link_status");
  if ((w == 0U) && (r == 0U) && (l == 0U)) {
    return; /* not a networking firmware -- nothing to shim. */
  }
  /* Frame I/O: route to the virtual peer. */
  eth_seam_hook(uc, elf, len, "ra_eth_write", (void*)on_eth_write);
  eth_seam_hook(uc, elf, len, "ra_eth_read", (void*)on_eth_read);
  eth_seam_hook(uc, elf, len, "ra_eth_link_status", (void*)on_eth_link_status);
  /* HW bring-up / teardown: shim to no-ops so the GWCA/ETHA/PHY sequence the
   * sparse model cannot satisfy does not fail the demo's setup. */
  eth_seam_hook(uc, elf, len, "ra_board_ethernet_init", (void*)on_eth_ok);
  eth_seam_hook(uc, elf, len, "ra_eth_open", (void*)on_eth_ok);
  eth_seam_hook(uc, elf, len, "ra_eth_close", (void*)on_eth_ok);
  /* With --trace, log the NetX echo-loop entries so the app thread's progress
   * (accept -> receive -> send) is visible -- useful when debugging the stack. */
  if (trace) {
    eth_trace_hook(uc, elf, len, "_nx_tcp_server_socket_accept");
    eth_trace_hook(uc, elf, len, "_nx_tcp_socket_receive");
    eth_trace_hook(uc, elf, len, "_nx_tcp_socket_send");
  }
  (void)fprintf(stderr,
                "  ra_eth seam   : write=0x%08X read=0x%08X link=0x%08X (virtual net peer)\n",
                w,
                r,
                l);
}

/** @brief Load ELF32 PT_LOAD segments into emulated memory at their LMA. */
static int load_elf(uc_engine* uc, const uint8_t* elf, long len)
{
  if ((len < (long)k_elf_ehdr_size) ||
      (memcmp(elf,
              "\x7F"
              "ELF",
              4) != 0) ||
      (elf[4] != 1) /* ELFCLASS32 */) {
    (void)fprintf(stderr, "not a 32-bit ELF\n");
    return -1;
  }
  const uint16_t e_machine = (uint16_t)(elf[18] | (elf[19] << 8));
  if (e_machine != (uint16_t)k_elf_em_arm) {
    (void)fprintf(stderr, "ELF e_machine %u != ARM(40)\n", e_machine);
    return -1;
  }
  uint32_t phoff = 0U;
  (void)memcpy(&phoff, elf + (uint32_t)k_elf_e_phoff_off, 4);
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
    (void)memcpy(&p_paddr, ph + (uint32_t)k_elf_ph_paddr_off, 4); /* load address (LMA) */
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

/**
 * @brief Resolve a function symbol's entry address from the ELF .symtab.
 *
 * @details
 * Walks the ELF32 section headers for the SHT_SYMTAB table and its linked string
 * table, then matches @p name and returns its st_value with the Thumb bit
 * cleared (so it can be used as a UC_HOOK_CODE address). Used to shim the
 * firmware's ra_eth_* frame API for the virtual network peer. Returns 0 if the
 * symbol (or a symbol table) is absent.
 *
 * @param[in] elf  Mapped ELF image.
 * @param[in] len  ELF image length in bytes.
 * @param[in] name NUL-terminated symbol name to find.
 * @return Even (Thumb-cleared) symbol address, or 0 if not found.
 */
static uint32_t elf_sym_addr(const uint8_t* elf, long len, const char* name)
{
  if (len < (long)k_elf_ehdr_size) {
    return 0U;
  }
  uint32_t shoff = 0U;
  (void)memcpy(&shoff, elf + 32, 4);
  const uint16_t shentsize = (uint16_t)(elf[46] | (elf[47] << 8));
  const uint16_t shnum     = (uint16_t)(elf[48] | (elf[49] << 8));
  const size_t   nlen      = strlen(name) + 1U;
  if ((shoff == 0U) || (shentsize < (uint32_t)k_elf_shentsize_min)) {
    return 0U;
  }
  for (uint16_t i = 0U; i < shnum; i++) {
    const uint8_t* sh = elf + shoff + ((uint32_t)i * shentsize);
    if (((size_t)(sh - elf) + (size_t)k_elf_shentsize_min) > (size_t)len) {
      break;
    }
    uint32_t sh_type = 0U;
    (void)memcpy(&sh_type, sh + 4, 4);
    if (sh_type != 2U /* SHT_SYMTAB */) {
      continue;
    }
    uint32_t sym_off = 0U, sym_size = 0U, sym_link = 0U, sym_entsize = 0U;
    (void)memcpy(&sym_off, sh + 16, 4);
    (void)memcpy(&sym_size, sh + (uint32_t)k_elf_sh_size_off, 4);
    (void)memcpy(&sym_link, sh + (uint32_t)k_elf_sh_link_off, 4);
    (void)memcpy(&sym_entsize, sh + (uint32_t)k_elf_sh_entsize_off, 4);
    if ((sym_entsize < 16U) || (sym_link >= shnum)) {
      continue;
    }
    const uint8_t* strsh   = elf + shoff + ((uint32_t)sym_link * shentsize);
    uint32_t       str_off = 0U;
    (void)memcpy(&str_off, strsh + 16, 4);
    const uint32_t nsym = sym_size / sym_entsize;
    for (uint32_t s = 0U; s < nsym; s++) {
      const uint8_t* sym = elf + sym_off + (s * sym_entsize);
      if (((size_t)(sym - elf) + 16U) > (size_t)len) {
        break;
      }
      uint32_t st_name = 0U, st_value = 0U;
      (void)memcpy(&st_name, sym + 0, 4);
      (void)memcpy(&st_value, sym + 4, 4);
      const size_t pos = (size_t)str_off + (size_t)st_name;
      if ((st_name == 0U) || ((pos + nlen) > (size_t)len)) {
        continue;
      }
      if (memcmp(elf + pos, name, nlen) == 0) {
        return st_value & ~1U; /* clear the Thumb bit for the hook address. */
      }
    }
  }
  return 0U;
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
    return (rd32(uc, (uint64_t)k_scb_shpr2) >> (3U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
  }
  if (exc_num == (uint32_t)k_exc_pendsv) {
    return (rd32(uc, (uint64_t)k_scb_shpr3) >> (2U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
  }
  if (exc_num == (uint32_t)k_exc_systick) {
    return (rd32(uc, (uint64_t)k_scb_shpr3) >> (3U * (uint32_t)k_byte_bits)) &
           (uint32_t)k_byte_mask;
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
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_r3, reg_get(uc, UC_ARM_REG_R3));
  wr32(uc, (uint64_t)sp + 16U, reg_get(uc, UC_ARM_REG_R12));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_lr, reg_get(uc, UC_ARM_REG_LR));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_pc, reg_get(uc, UC_ARM_REG_PC));
  wr32(uc, (uint64_t)sp + (uint64_t)k_frame_off_xpsr, frame_xpsr);

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
  const uint32_t r3   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_r3);
  const uint32_t r12  = rd32(uc, (uint64_t)sp + 16U);
  const uint32_t lr   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_lr);
  const uint32_t pc   = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_pc);
  const uint32_t xpsr = rd32(uc, (uint64_t)sp + (uint64_t)k_frame_off_xpsr);

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
  return (pc & (uint64_t)k_exc_ret_grp_mask) == (uint64_t)k_exc_ret_base;
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
  if ((handler == 0U) || (handler == (uint32_t)k_vector_erased)) {
    return 0U;
  }
  return handler;
}

/**
 * @brief Take one pending peripheral NVIC IRQ the ICU has queued, if allowed.
 *
 * @details
 * The peripheral counterpart to the SysTick / PendSV logic in
 * ::exc_take_pending. board_periph's ICU model queues an IRQ whenever a
 * peripheral event is event-linked through IELSR and its NVIC line is enabled;
 * this pops one and -- if its NVIC priority (IPR byte, top nibble used) outranks
 * the active execution priority -- vectors it in as a real Cortex-M exception
 * (vector 16 + IRQn read from VTOR), exactly the path a hardware IRQ takes. The
 * ISR therefore runs in genuine handler context and returns via the same
 * EXC_RETURN unstack as every other exception. The matching ISPR pending bit is
 * cleared on activation, as hardware does.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector base when VTOR reads as 0.
 * @param[in]     active    Current active-handler priority (sentinel if none).
 * @return true if a peripheral IRQ was taken (PC now points at its ISR).
 *
 * @pre @p uc has stopped at an instruction boundary; PRIMASK already checked.
 * @post At most one IRQ is taken; its ISPR pending bit is cleared if so.
 * @note If no handler is installed at the vector, the IRQ is dropped, not spun.
 * @since 0.1.0
 */
static bool exc_take_periph_irq(uc_engine* uc, uint32_t vtor_base, uint32_t active)
{
  uint32_t irq = 0U;
  if (!board_periph_next_irq(&irq)) {
    return false;
  }
  const uint8_t  prio_byte = (uint8_t)rd32(uc, (uint64_t)k_nvic_ipr_base + irq);
  const uint32_t prio =
    (uint32_t)(prio_byte >> (uint32_t)k_nvic_prio_shift) & (uint32_t)k_lo4_mask; /* 4 MSBs used */
  if (prio >= active) {
    return false; /* an equal/higher-priority handler is active -- defer */
  }
  const uint32_t handler = exc_vector(uc, vtor_base, (uint32_t)k_exc_irq_vec0 + irq);
  if (handler == 0U) {
    return false; /* no ISR installed: drop (default handler would just return) */
  }
  /* Clear the NVIC ISPR pending bit on activation, as hardware does. */
  const uint64_t ispr_word = (uint64_t)k_nvic_ispr_base + ((uint64_t)(irq / 32U) * 4U);
  uint32_t       ispr      = rd32(uc, ispr_word);
  ispr &= ~(1U << (irq % 32U));
  wr32(uc, ispr_word, ispr);

  exc_enter(uc, (uint32_t)k_exc_irq_vec0 + irq, handler);
  board_periph_note_irq_taken(irq);
  return true;
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

  /* Peripheral NVIC IRQs queued by the ICU model (timer overflow / underflow
   * routed through IELSR). Taken last among the modelled exceptions but via the
   * identical real entry/return path, with NVIC IPR priority honoured. */
  return exc_take_periph_irq(uc, vtor_base, active);
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
  k_rgb888_mask    = 0x00FFFFFFU, /**< 24-bit colour (BG_BGC low bytes).   */
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

/**
 * @enum ram_region_t
 * @brief Emulated RAM windows a GLCDC framebuffer may legally live in.
 */
typedef enum : uint32_t {
  k_dtcm_base  = 0x20000000U, /**< Data TCM start.   */
  k_dtcm_end   = 0x20010000U, /**< Data TCM end.     */
  k_sram_base  = 0x22000000U, /**< On-chip SRAM start.*/
  k_sram_end   = 0x22100000U, /**< On-chip SRAM end. */
  k_sdram_base = 0x68000000U, /**< External SDRAM start.*/
  k_sdram_end  = 0x6C000000U, /**< External SDRAM end.*/
} ram_region_t;

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
  st->app_name  = app_name;
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
  k_rotate_0   = 0U,   /**< Native orientation.              */
  k_rotate_90  = 90U,  /**< 90 deg clockwise (-> portrait).  */
  k_rotate_180 = 180U, /**< Upside down.                     */
  k_rotate_270 = 270U, /**< 90 deg counter-clockwise.        */
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

/* Console-UART presentation. The SCI_B model captures every byte the firmware
 * transmits and hands it to console_tx_sink, which echoes it to stdout with a
 * clear [uart] prefix so a console example's output is captured and greppable.
 * The same byte is mirrored raw so piping board_sim's stdout reconstructs the
 * exact serial stream. RX is fed from --input / stdin into the console channel
 * (SCI8 on the EK-RA8D2). */
typedef enum : uint32_t {
  k_uart_line_max = 256U, /**< Pretty-print line buffer for the [uart] prefix. */
} console_cfg_t;

static char     s_uart_line[k_uart_line_max]; /**< Pending [uart] line text.   */
static uint32_t s_uart_line_len;              /**< Chars buffered in the line.  */

/** @brief Flush the pending [uart] line to stdout with its channel prefix. */
static void console_flush_line(uint8_t channel)
{
  if (s_uart_line_len == 0U) {
    return;
  }
  s_uart_line[s_uart_line_len] = '\0';
  (void)fprintf(stdout, "[uart] SCI%u: %s\n", channel, s_uart_line);
  (void)fflush(stdout);
  s_uart_line_len = 0U;
}

/**
 * @brief SCI TX sink: print each transmitted byte (prefixed line + raw mirror).
 *
 * @details
 * Installed via board_periph_sci_set_tx_sink. Printable bytes accumulate into a
 * line that is flushed -- with an @c [uart] SCIn: prefix -- on newline or when
 * the buffer fills, so console output reads cleanly in the log; CR is dropped
 * from the pretty line. Every byte is also written verbatim to a raw mirror on
 * stdout so redirecting board_sim reproduces the exact serial stream.
 *
 * @param[in] channel SCI channel that transmitted the byte.
 * @param[in] byte    The transmitted data byte.
 * @return Nothing.
 */
static void console_tx_sink(uint8_t channel, uint8_t byte)
{
  if (byte == (uint8_t)'\n') {
    console_flush_line(channel);
    return;
  }
  if ((byte != (uint8_t)'\r') && (s_uart_line_len < (uint32_t)(k_uart_line_max - 1U))) {
    s_uart_line[s_uart_line_len++] = (char)byte;
  }
  if (s_uart_line_len == (uint32_t)(k_uart_line_max - 1U)) {
    console_flush_line(channel); /* avoid an unbounded line on a stream with no LF */
  }
}

/**
 * @brief Decode a C-style escaped --input string into a raw byte buffer.
 *
 * @details
 * Translates @c \\n / @c \\r / @c \\t / @c \\0 / @c \\\\ in @p in so a shell
 * argument can carry the line endings a console example expects (e.g.
 * @c --input "ping\r\n"); any other character (including an unrecognised
 * escape's backslash) is copied verbatim. Bounded by @p cap; never overruns.
 *
 * @param[in]  in  NUL-terminated source string.
 * @param[out] out Destination byte buffer.
 * @param[in]  cap Capacity of @p out in bytes.
 * @return Number of bytes written to @p out.
 */
static uint32_t decode_escapes(const char* in, uint8_t* out, uint32_t cap)
{
  uint32_t n = 0U;
  for (uint32_t i = 0U; (in[i] != '\0') && (n < cap); i++) {
    char c = in[i];
    if ((c == '\\') && (in[i + 1U] != '\0')) {
      i++;
      switch (in[i]) {
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case '0':
          c = '\0';
          break;
        default:
          c = in[i]; /* \\ -> \, and any other escape is taken literally */
          break;
      }
    }
    out[n++] = (uint8_t)c;
  }
  return n;
}

int main(int argc, char** argv)
{
  if (argc < 2) {
    (void)fprintf(stderr,
                  "usage: board_sim <firmware.elf> [--view] [--ppm <out.ppm>]"
                  " [--panel <file.toml>] [--size WxH] [--click X Y] [--input <str>] [--sd <image>]"
                  " [--usb-in <str>]\n"
                  "  --view          open a macOS window: live board view (panel + status)\n"
                  "  --ppm <file>    write the final composite (panel + status) to a PPM\n"
                  "  --record <dir>  record frames (panel + status) to <dir>/frame_NNNNNN.ppm\n"
                  "  --record-secs N headless: record N emulated seconds, then stop (~20 fps)\n"
                  "  --rotate DEG    display rotation 90/180/270 (e.g. portrait)\n"
                  "  --panel <file>  display descriptor (name/width/height) to size the panel\n"
                  "  --size WxH      panel size in pixels; overrides --panel (default 1024x600)\n"
                  "  --click X Y     headless: inject one touch at X,Y once the UI is up\n"
                  "  --input <str>   feed <str> to the console UART RX (SCI8); \\n / \\r / \\t ok\n"
                  "  --usb-in <str>  feed <str> to the USB CDC bulk OUT pipe (echo test)\n"
                  "  --trace         log each LED/GPIO transition + NVIC IRQ as it happens\n");
    return 2;
  }
  const char* elf_path    = argv[1];
  bool        want_view   = false;
  const char* ppm_path    = nullptr;
  const char* record_dir  = nullptr;
  uint32_t    record_secs = 0U;
  uint32_t    rotate_deg  = (uint32_t)k_rotate_0;
  const char* panel_path  = nullptr;
  const char* input_str   = nullptr;
  const char* usb_in_str  = nullptr;
  bool        size_set    = false;
  bool        want_click  = false;
  bool        want_trace  = false;
  int         click_x     = -1;
  int         click_y     = -1;
  uint16_t    view_w      = (uint16_t)k_view_default_w;
  uint16_t    view_h      = (uint16_t)k_view_default_h;
  for (int i = 2; i < argc; i++) {
    if (strncmp(argv[i], "--view", sizeof("--view")) == 0) {
      want_view = true;
    } else if (strncmp(argv[i], "--trace", sizeof("--trace")) == 0) {
      want_trace = true;
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
    } else if ((strncmp(argv[i], "--input", sizeof("--input")) == 0) && ((i + 1) < argc)) {
      input_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--usb-in", sizeof("--usb-in")) == 0) && ((i + 1) < argc)) {
      usb_in_str = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--sd", sizeof("--sd")) == 0) && ((i + 1) < argc)) {
      (void)board_sd_attach(argv[i + 1]); /* serve this FAT image to ra_sdmmc_spi */
      i++;
    } else if ((strncmp(argv[i], "--click", sizeof("--click")) == 0) && ((i + 2) < argc)) {
      click_x    = (int)strtol(argv[i + 1], nullptr, (int)k_strtol_base10);
      click_y    = (int)strtol(argv[i + 2], nullptr, (int)k_strtol_base10);
      want_click = (click_x >= 0) && (click_y >= 0);
      i += 2;
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

  /* Reset the peripheral-model framework (GPIO/PORT, AGT/GPT timers, ICU/NVIC,
   * SCI UART) before the firmware boots so its register writes land in real
   * block state. Install the console sink so transmitted bytes reach stdout,
   * and queue any --input as console-channel (SCI8) RX. */
  board_periph_init(want_trace);
  board_net_init(want_trace);
  board_periph_sci_set_tx_sink(console_tx_sink);
  if (input_str != nullptr) {
    uint8_t        rx[k_uart_line_max];
    const uint32_t n = decode_escapes(input_str, rx, (uint32_t)sizeof(rx));
    board_periph_sci_feed_rx(board_periph_sci_console_channel(), rx, n);
    (void)fprintf(stderr,
                  "board_sim: queued %u byte(s) to SCI%u RX from --input\n",
                  n,
                  board_periph_sci_console_channel());
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
  if (load_elf(uc, elf, elf_len) != 0) {
    free(elf);
    return 1;
  }
  /* NB: keep the host-side `elf` buffer alive until after eth_seam_install --
   * load_elf has copied the image into Unicorn memory, but the eth seam still
   * scans the ELF symbol table from this buffer (freed right after, below). */

  /* Cortex-M reset: SP = vectors[0], PC = vectors[1] (Thumb, clear bit0). */
  uint32_t sp = 0U;
  uint32_t pc = 0U;
  (void)uc_mem_read(uc, k_regions[1].base + 0U, &sp, 4); /* MRAM[0] */
  (void)uc_mem_read(uc, k_regions[1].base + 4U, &pc, 4); /* MRAM[4] (Thumb: bit0=1) */
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
  /* NVIC ISER / ICER are set-enable / clear-enable: fold each written bit into
   * board_periph's enable shadow so enabling several lines does not clobber the
   * earlier ones (see on_nvic_en_write). The PPB is RAM, so this hook is the
   * only place the W1S/W1C semantics can be applied. */
  uc_hook h_nvic_iser;
  uc_hook h_nvic_icer;
  (void)uc_hook_add(uc,
                    &h_nvic_iser,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_nvic_en_write,
                    nullptr,
                    (uint64_t)k_nvic_iser_base,
                    (uint64_t)k_nvic_iser_base + (uint64_t)k_nvic_en_span - 1U);
  (void)uc_hook_add(uc,
                    &h_nvic_icer,
                    UC_HOOK_MEM_WRITE,
                    (void*)on_nvic_en_write,
                    nullptr,
                    (uint64_t)k_nvic_icer_base,
                    (uint64_t)k_nvic_icer_base + (uint64_t)k_nvic_en_span - 1U);
  /* Shim the ra_eth frame API to the virtual network peer (no-op unless the
   * firmware exports those symbols, i.e. a NetX networking example). */
  eth_seam_install(uc, elf, elf_len, want_trace);
  free(elf);

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
  uint32_t max_chunks =
    (view != nullptr) ? (uint32_t)k_view_max_chunks : (uint32_t)k_run_max_chunks;
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
  uint32_t      rec_frames  = 0U; /* frames written when --record is active. */
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

    /* Advance the modelled timers one tick-period and let the ICU pend any IRQ
     * a counter wrap raised; the exception layer below takes it like SysTick. */
    board_periph_tick(uc);
    board_net_tick();

    /* Headless --click: keep one contact armed in the GT911 model until the
     * firmware's real ra_touch_read drains it (board_periph_touch_reported
     * increments). Re-arming each chunk is needed because ra_touch_open clears
     * the GT911 status byte during bring-up; once the render loop reads the
     * point it is consumed once and never re-armed. */
    if (want_click && (board_periph_touch_reported() == 0U)) {
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
      /* Live window: each left mouse-down arms one GT911 contact, so the
       * firmware's next real ra_touch_read returns it through the I3C path. */
      uint16_t cx = 0U;
      uint16_t cy = 0U;
      if (board_view_poll_click(view, &cx, &cy)) {
        uint16_t nx = cx;
        uint16_t ny = cy;
        unrotate_click(cx, cy, panel_w, panel_h, rotate_deg, &nx, &ny);
        board_periph_touch_inject(nx, ny);
      }
      if ((chunks % (uint32_t)k_view_present_every) == 0U) {
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
      }
      if (board_view_pump(view)) {
        closed = true;
        break;
      }
    } else if (want_click) {
      /* Headless --click: run a bounded tail after the tap is drained, then
       * stop deterministically so the dumped frame shows the switched tab. */
      if (board_periph_touch_reported() > 0U) {
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
  (void)fprintf(stderr,
                "  touch clicks  : %u drained via ra_touch -> I3C -> GT911\n",
                board_periph_touch_reported());
  /* Emit any console bytes still buffered without a trailing newline. */
  console_flush_line(board_periph_sci_console_channel());
  /* Peripheral-model observability: LED transitions, timer totals, IRQ counts,
   * SCI byte totals. */
  board_periph_report(uc);
  board_net_report();
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
  free(panel_fb);
  free(rot_fb);
  free(composite);
  (void)uc_close(uc);
  return 0;
}
