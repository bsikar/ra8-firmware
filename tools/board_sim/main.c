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
#include <unistd.h>
#include <unicorn/unicorn.h>

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
  k_run_max_chunks  = 6000U,   /**< Chunk budget (~3e9 insns ceiling).      */
  k_run_wall_s      = 10U,     /**< Wall-clock safety bound (seconds).      */
  k_systick_insns   = 200000U, /**< Max insns per SysTick handler call.     */
  k_systick_us      = 100000U, /**< Wall bound per SysTick handler call.    */
  k_mmio_slots      = 2048U,   /**< Distinct MMIO addresses tracked.        */
  k_mmio_settle     = 8U,      /**< Same-addr reads before a poll "settles".*/
  k_mmio_print_max  = 256U,    /**< Max MMIO rows printed in the summary.    */
} sim_budget_t;

/* Cortex-M system control space (architectural, all cores) -- inside the PPB. */
typedef enum : uint64_t {
  k_syst_csr     = 0xE000E010UL, /**< SysTick control/status.            */
  k_syst_csr_run = 0x3UL,        /**< ENABLE | TICKINT both set.         */
  k_scb_vtor     = 0xE000ED08UL, /**< Vector table offset register.      */
  k_exc_systick  = 15UL,         /**< SysTick exception / vector index.  */
  k_systick_ret  = 0x4UL,        /**< Sentinel return addr for the call. */
} cortexm_scs_t;

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
  k_mrms_mrcfreq    = 0x4013C004UL,   /**< MRICLK freq latch (write key 0x1E). */
  k_mrms_mrefreq    = 0x4013C008UL,   /**< MRPCLK freq latch (write key 0xE1). */
  k_mrms_freq_mask  = 0x00FFFFFFUL,   /**< Key byte (bits[31:24]) stripped.    */
} mrms_quirk_t;

/* GLCDC observation point. The lcd_color_cycle demo proves a live panel by
 * re-writing BG_BGC (background colour) every frame and pulsing BG_EN.VEN to
 * commit it. The generic model only keeps the last value per address, so the
 * distinct colours cycled are tracked separately as the tool's success witness.
 * GLCDC base 0x40342000 + 0x1014 = BG_BGC (HUM Ch 63). */
typedef enum : uint64_t {
  k_glcdc_bg_bgc     = 0x40343014UL, /**< GLCDC BG.BGC background colour.       */
  k_bgc_track_max    = 32UL,         /**< Distinct BG_BGC values remembered.   */
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

/** @brief Disassemble + report an instruction the core could not decode. */
static bool on_invalid_insn(uc_engine* uc, void* user)
{
  (void)user;
  uint32_t pc = 0U;
  (void)uc_reg_read(uc, UC_ARM_REG_PC, &pc);
  uint8_t code[4] = {0};
  (void)uc_mem_read(uc, pc, code, sizeof(code));
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
  (void)uc;
  (void)size;
  (void)value;
  (void)user;
  (void)fprintf(stderr,
                "  UNMAPPED %s @ 0x%08llX (extend the memory/peripheral map)\n",
                (type == UC_MEM_READ_UNMAPPED)    ? "read"
                : (type == UC_MEM_WRITE_UNMAPPED) ? "write"
                                                  : "fetch",
                (unsigned long long)addr);
  return false; /* stop emulation and report */
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

/** @brief Read a 32-bit little-endian word from emulated memory. */
static uint32_t rd32(uc_engine* uc, uint64_t addr)
{
  uint32_t v = 0U;
  (void)uc_mem_read(uc, addr, &v, sizeof(v));
  return v;
}

/* Integer context saved/restored around a cooperative SysTick handler call. */
static const int k_ctx_regs[] = {
  UC_ARM_REG_R0,  UC_ARM_REG_R1, UC_ARM_REG_R2,  UC_ARM_REG_R3, UC_ARM_REG_R4,
  UC_ARM_REG_R5,  UC_ARM_REG_R6, UC_ARM_REG_R7,  UC_ARM_REG_R8, UC_ARM_REG_R9,
  UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR,
  UC_ARM_REG_PC,  UC_ARM_REG_XPSR,
};

/**
 * @brief Cooperatively run one SysTick tick if SysTick is armed.
 *
 * @details
 * Models a SysTick interrupt without a real NVIC: if SYST_CSR has ENABLE and
 * TICKINT set, the installed SysTick_Handler (vector index 15, relative to
 * VTOR) is invoked as an ordinary function between emulation chunks. The full
 * integer context (R0-R12, SP, LR, PC, xPSR) is snapshotted and restored, so
 * only the handler's memory effect -- the incremented tick counter -- survives,
 * which is exactly what the interrupted code would observe after a real tick.
 *
 * @param[in,out] uc        Unicorn engine.
 * @param[in]     vtor_base Fallback vector table base if VTOR reads as 0.
 */
static void systick_fire(uc_engine* uc, uint32_t vtor_base)
{
  if ((rd32(uc, (uint64_t)k_syst_csr) & (uint32_t)k_syst_csr_run) != (uint32_t)k_syst_csr_run) {
    return; /* SysTick not armed (no enable+tickint) -- nothing to tick */
  }
  uint32_t vtor = rd32(uc, (uint64_t)k_scb_vtor);
  if (vtor == 0U) {
    vtor = vtor_base;
  }
  const uint32_t handler = rd32(uc, (uint64_t)vtor + ((uint32_t)k_exc_systick * 4U)) & ~1U;
  if ((handler == 0U) || (handler == 0xFFFFFFFEU)) {
    return; /* no handler installed at the SysTick vector slot */
  }

  uint32_t save[sizeof(k_ctx_regs) / sizeof(k_ctx_regs[0])];
  for (size_t i = 0U; i < (sizeof(k_ctx_regs) / sizeof(k_ctx_regs[0])); i++) {
    (void)uc_reg_read(uc, k_ctx_regs[i], &save[i]);
  }

  /* Return to a sentinel we stop on; LR bit0=1 keeps Thumb across bx/pop pc. */
  uint32_t lr = (uint32_t)k_systick_ret | 1U;
  (void)uc_reg_write(uc, UC_ARM_REG_LR, &lr);
  (void)uc_emu_start(uc,
                     (uint64_t)handler | 1U,
                     (uint64_t)k_systick_ret,
                     (uint64_t)k_systick_us,
                     (size_t)k_systick_insns);

  for (size_t i = 0U; i < (sizeof(k_ctx_regs) / sizeof(k_ctx_regs[0])); i++) {
    (void)uc_reg_write(uc, k_ctx_regs[i], &save[i]);
  }
  s_systick_fires++;
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
  const uint32_t stride =
    (rd32(uc, (uint64_t)k_glcdc_gr1_flm3) >> (uint32_t)k_glcdc_high_shift) &
    (uint32_t)k_glcdc_stride_mask;
  const uint32_t lnnum =
    (rd32(uc, (uint64_t)k_glcdc_gr1_flm5) >> (uint32_t)k_glcdc_high_shift) &
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

int main(int argc, char** argv)
{
  if (argc < 2) {
    (void)fprintf(stderr,
                  "usage: board_sim <firmware.elf> [--view] [--ppm <out.ppm>] [--size WxH]\n"
                  "  --view        open a macOS window and show the emulated panel live\n"
                  "  --ppm <file>  write the final frame to a binary PPM (headless ok)\n"
                  "  --size WxH    frame size in pixels (default 1024x600)\n");
    return 2;
  }
  const char* elf_path  = argv[1];
  bool        want_view = false;
  const char* ppm_path  = nullptr;
  uint16_t    view_w    = (uint16_t)k_view_default_w;
  uint16_t    view_h    = (uint16_t)k_view_default_h;
  for (int i = 2; i < argc; i++) {
    if (strncmp(argv[i], "--view", sizeof("--view")) == 0) {
      want_view = true;
    } else if ((strncmp(argv[i], "--ppm", sizeof("--ppm")) == 0) && ((i + 1) < argc)) {
      ppm_path = argv[i + 1];
      i++;
    } else if ((strncmp(argv[i], "--size", sizeof("--size")) == 0) && ((i + 1) < argc)) {
      char*      end = nullptr;
      const long w   = strtol(argv[i + 1], &end, 10);
      const long h   = ((end != nullptr) && (*end == 'x')) ? strtol(end + 1, nullptr, 10) : 0L;
      if ((w > 0L) && (w <= 4096L) && (h > 0L) && (h <= 4096L)) {
        view_w = (uint16_t)w;
        view_h = (uint16_t)h;
      }
      i++;
    }
  }

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

  uc_hook h_invalid;
  uc_hook h_unmapped;
  (void)uc_hook_add(uc, &h_invalid, UC_HOOK_INSN_INVALID, (void*)on_invalid_insn, nullptr, 1, 0);
  (void)uc_hook_add(uc, &h_unmapped, UC_HOOK_MEM_UNMAPPED, (void*)on_unmapped, nullptr, 1, 0);

  /* Optional live window; the frame buffer also backs the --ppm snapshot. */
  board_view_t* view  = nullptr;
  uint16_t*     frame = nullptr;
  if (want_view) {
    view = board_view_open(view_w, view_h, "board_sim");
    if (view == nullptr) {
      (void)fprintf(stderr, "board_sim: could not open window; continuing headless\n");
    }
  }
  if ((view != nullptr) || (ppm_path != nullptr)) {
    frame = (uint16_t*)malloc((size_t)view_w * (size_t)view_h * sizeof(uint16_t));
  }

  /* Chunked run: emulate a block, tick SysTick, repeat. Headless runs stop on a
   * chunk budget + wall-clock guard; in --view the loop runs until the window
   * is closed, presenting the live GLCDC output every k_view_present_every. */
  const uint32_t vtor_base  = (uint32_t)k_regions[1].base; /* MRAM = vectors */
  const uint32_t max_chunks =
    (view != nullptr) ? (uint32_t)k_view_max_chunks : (uint32_t)k_run_max_chunks;
  const clock_t t0        = clock();
  uc_err        err       = UC_ERR_OK;
  uint32_t      run_pc    = pc;
  uint32_t      chunks    = 0U;
  bool          timed_out = false;
  bool          closed    = false;
  for (; chunks < max_chunks; chunks++) {
    err = uc_emu_start(uc, (uint64_t)run_pc | 1U, 0, 0, (size_t)k_run_chunk_insns);
    (void)uc_reg_read(uc, UC_ARM_REG_PC, &run_pc);
    if (err != UC_ERR_OK) {
      break;
    }
    systick_fire(uc, vtor_base);
    if (view != nullptr) {
      if ((chunks % (uint32_t)k_view_present_every) == 0U) {
        build_frame(uc, frame, view_w, view_h);
        board_view_present(view, frame, view_w, view_h);
      }
      if (board_view_pump(view)) {
        closed = true;
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
  const uint32_t shown =
    (s_mmio_n < (uint32_t)k_mmio_print_max) ? s_mmio_n : (uint32_t)k_mmio_print_max;
  for (uint32_t i = 0U; i < shown; i++) {
    if (s_mmio_written[i]) {
      (void)fprintf(stderr, "    0x%08llX %10u %10u   0x%08X\n", (unsigned long long)s_mmio_addr[i],
                    s_mmio_rcount[i], s_mmio_wcount[i], s_mmio_val[i]);
    } else {
      (void)fprintf(stderr, "    0x%08llX %10u %10u %12s\n", (unsigned long long)s_mmio_addr[i],
                    s_mmio_rcount[i], s_mmio_wcount[i], "-");
    }
  }
  if (s_mmio_n > shown) {
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
