/**
 * @file board_periph_ceu.c
 * @brief Capture Engine Unit (CEU) peripheral-block model for ra8_emulator
 *
 * @details
 * Models the RA8D2 Capture Engine Unit at 0x4034_8000 (ra8_ceu_regs.h /
 * ra8_ceu.c, the real HUM Ch 60 parallel-camera capture path). The firmware
 * (``camera_capture`` via cam_ceu.c) initialises the CEU, arms a single-shot
 * capture with ``ra8_ceu_capture_arm`` (which programs the destination address
 * register ``CDAYR`` then sets ``CAPSR.CE``), and polls the event register
 * ``CETCR.CPE`` for one-frame-end. On silicon that end is raised by the sensor's
 * VD edge; in ra8_emulator the capture is instantaneous.
 *
 * On the ``CAPSR.CE`` arm this block synthesises the frame: it derives the
 * destination byte geometry from the capture registers (``CDWDR`` destination
 * stride x ``CAPWR.VWDTH`` lines, falling back to ``CMCYR``), writes a
 * deterministic diagonal-gradient test pattern into the firmware's frame buffer
 * at ``CDAYR`` through the Unicorn engine, latches ``CETCR.CPE`` (and the data
 * size ``CDSSR``), and clears ``CAPSR.CE`` (single-shot end). The status
 * register ``CSTSR`` reads idle (capture is instantaneous) and ``CAPSR.CPKIL``
 * self-clears, so ``ra8_ceu_init``'s idle wait and the arm path both complete.
 * The result: ``camera_capture`` reaches ``frame=OK`` with a plausible
 * non-degenerate min/max/mean and runs its capture -> stats -> verdict path
 * headless.
 *
 * Honest-model boundaries: a capture with no destination address or no valid
 * geometry raises no CPE (no faked frame). And ra8_emulator cannot reproduce the
 * real board's J1/TFT VIO_HD pin conflict (issue #119) -- this exercises the
 * register/capture flow, it is not a claim the silicon captures. Self-registers
 * with the board_periph core from a file-scope constructor -- see
 * board_periph_block.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"

/** @brief CEU register-window geometry (ra8_ceu_regs.h, Plane A view). */
typedef enum : uint64_t {
  k_ceu_base      = 0x40348000UL,  /**< CEU block base (HUM Ch 60).          */
  k_ceu_span      = 0x100UL,       /**< Plane-A register set (0x00..0x9C).   */
  k_ceu_words     = 0x100UL / 4UL, /**< Backing-store word count.            */
  k_ceu_off_capsr = 0x00UL,        /**< CAPSR capture start (CE / CPKIL).    */
  k_ceu_off_cmcyr = 0x0CUL,        /**< CMCYR interface cycle (HCYL / VCYL). */
  k_ceu_off_capwr = 0x14UL,        /**< CAPWR capture width (HWDTH / VWDTH). */
  k_ceu_off_cdwdr = 0x38UL,        /**< CDWDR destination stride.            */
  k_ceu_off_cdayr = 0x3CUL,        /**< CDAYR destination Y address.         */
  k_ceu_off_cetcr = 0x74UL,        /**< CETCR event flags (CPE, ...).        */
  k_ceu_off_cstsr = 0x7CUL,        /**< CSTSR capture status (CPTON).        */
  k_ceu_off_cdssr = 0x84UL,        /**< CDSSR captured data byte count.      */
} ceu_map_t;

/** @brief CEU register bit fields (ra8_ceu_regs.h). */
typedef enum : uint32_t {
  k_ceu_capsr_ce     = 0x00000001U, /**< CAPSR.CE: start capture at next VD.    */
  k_ceu_capsr_cpkil  = 0x00010000U, /**< CAPSR.CPKIL: software reset.           */
  k_ceu_cetcr_cpe    = 0x00000001U, /**< CETCR.CPE: one-frame capture end.      */
  k_ceu_cmcyr_hcyl   = 0x00003FFFU, /**< CMCYR.HCYL[13:0]: byte-cycles/line.    */
  k_ceu_field_14bit  = 0x00003FFFU, /**< 14-bit field (CMCYR VCYL after >>16).  */
  k_ceu_capwr_hwdth  = 0x00001FFFU, /**< CAPWR.HWDTH[12:0] / CDWDR stride mask. */
  k_ceu_field_12bit  = 0x00000FFFU, /**< 12-bit field (CAPWR VWDTH after >>16). */
  k_ceu_pattern_mask = 0x000000FFU, /**< Test-pattern byte modulus.             */
} ceu_field_t;

/** @brief Field shift + fill bounds. */
typedef enum : uint32_t {
  k_ceu_shift_vert = 16U,   /**< Vertical field shift (VCYL / VWDTH).         */
  k_ceu_max_line   = 8192U, /**< Max modelled line width (bytes) -- QVGA=640. */
  k_ceu_max_lines  = 4096U, /**< Max modelled line count -- QVGA=240.         */
} ceu_bound_t;

/** @brief Console anti-flood cadence for the CEU (DMA/transfer) tab. */
typedef enum : uint32_t {
  k_ceu_console_line_cap = 56U, /**< Max chars in a "CEU frame .." line. */
} ceu_console_t;

/** @brief CEU model state: register backing store + capture observability. */
typedef struct {
  uint32_t reg[k_ceu_words]; /**< Plane-A register backing store.    */
  uint32_t frames;           /**< Frames synthesised (report).       */
  uint32_t last_bytes;       /**< Bytes written by the last capture. */
  uint32_t last_w;           /**< Last capture line width (bytes).   */
  uint32_t last_h;           /**< Last capture line count.           */
} ceu_state_t;

static ceu_state_t s_ceu;
static uint8_t     s_ceu_line[k_ceu_max_line]; /**< One-line fill scratch. */

/** @brief Word index into the backing store for an in-window byte offset. */
static uint32_t ceu_word(uint64_t off)
{
  return (uint32_t)(off >> 2U);
}

/** @brief Destination line width (bytes): CDWDR stride, else CMCYR.HCYL. */
static uint32_t ceu_line_bytes(void)
{
  const uint32_t stride = s_ceu.reg[ceu_word(k_ceu_off_cdwdr)] & (uint32_t)k_ceu_capwr_hwdth;
  if (stride != 0U) {
    return stride;
  }
  return s_ceu.reg[ceu_word(k_ceu_off_cmcyr)] & (uint32_t)k_ceu_cmcyr_hcyl;
}

/** @brief Destination line count: CAPWR.VWDTH, else CMCYR.VCYL. */
static uint32_t ceu_line_count(void)
{
  const uint32_t vwdth = (s_ceu.reg[ceu_word(k_ceu_off_capwr)] >> (uint32_t)k_ceu_shift_vert) &
                         (uint32_t)k_ceu_field_12bit;
  if (vwdth != 0U) {
    return vwdth;
  }
  return (s_ceu.reg[ceu_word(k_ceu_off_cmcyr)] >> (uint32_t)k_ceu_shift_vert) &
         (uint32_t)k_ceu_field_14bit;
}

/**
 * @brief Synthesise one frame into the CDAYR buffer and latch CETCR.CPE.
 *
 * @details
 * Derives the destination geometry from the capture registers, clamps it to the
 * modelled bounds, and writes a diagonal-gradient test pattern
 * ((x + y) & 0xFF -- black..white so a real grab always varies) line-by-line
 * into emulated memory at CDAYR. Raises CETCR.CPE + records CDSSR on success.
 * No CDAYR or no geometry -> no frame is written and CPE stays clear (honest).
 *
 * @param[in,out] uc Unicorn engine (the pattern is written to guest memory).
 * @return true when a frame was synthesised and CPE latched.
 *
 * @pre CAPSR.CE was just written (the arm event).
 * @pre The CDAYR buffer, when non-zero, is mapped emulated memory.
 * @post On true, CETCR.CPE and CDSSR reflect a filled frame; CAPSR.CE is clear.
 * @post On false, no guest memory and no CETCR bit changed.
 * @note Not thread-safe; the run loop is single-threaded host-side.
 * @since 0.1.0
 */
static bool ceu_do_capture(uc_engine* uc)
{
  uint32_t line_bytes = ceu_line_bytes();
  uint32_t lines      = ceu_line_count();
  if ((line_bytes == 0U) || (lines == 0U)) {
    return false; /* No geometry programmed -> no capture. */
  }
  if (line_bytes > (uint32_t)k_ceu_max_line) {
    line_bytes = (uint32_t)k_ceu_max_line;
  }
  if (lines > (uint32_t)k_ceu_max_lines) {
    lines = (uint32_t)k_ceu_max_lines;
  }
  const uint32_t dst = s_ceu.reg[ceu_word(k_ceu_off_cdayr)];
  if (dst == 0U) {
    return false; /* No destination buffer -> no capture. */
  }
  for (uint32_t y = 0U; y < lines; y++) {
    for (uint32_t x = 0U; x < line_bytes; x++) {
      s_ceu_line[x] = (uint8_t)((x + y) & (uint32_t)k_ceu_pattern_mask);
    }
    (void)uc_mem_write(uc,
                       (uint64_t)dst + ((uint64_t)y * (uint64_t)line_bytes),
                       s_ceu_line,
                       (size_t)line_bytes);
  }
  const uint32_t total = line_bytes * lines;
  s_ceu.reg[ceu_word(k_ceu_off_cetcr)] |= (uint32_t)k_ceu_cetcr_cpe;
  s_ceu.reg[ceu_word(k_ceu_off_cdssr)] = total;
  s_ceu.reg[ceu_word(k_ceu_off_capsr)] &= ~(uint32_t)k_ceu_capsr_ce;
  s_ceu.frames++;
  s_ceu.last_bytes = total;
  s_ceu.last_w     = line_bytes;
  s_ceu.last_h     = lines;

  char ln[k_ceu_console_line_cap];
  (void)snprintf(ln,
                 sizeof(ln),
                 "CEU frame %ux%u -> %u bytes",
                 (unsigned)line_bytes,
                 (unsigned)lines,
                 (unsigned)total);
  board_console_push(k_board_console_ch_dma, ln);
  if (board_periph_trace()) {
    (void)fprintf(stderr, "  [trace] CEU capture %ux%u -> 0x%08X\n", line_bytes, lines, dst);
  }
  return true;
}

/** @brief MMIO read inside the CEU window. */
static uint64_t ceu_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_ceu_base;
  if (off >= (uint64_t)k_ceu_span) {
    return 0U;
  }
  if (off == (uint64_t)k_ceu_off_cstsr) {
    return 0U; /* Capture is instantaneous: CPTON always reads idle. */
  }
  if (off == (uint64_t)k_ceu_off_capsr) {
    /* CPKIL self-clears (reset is instantaneous in the model). */
    return (uint64_t)(s_ceu.reg[ceu_word(off)] & ~(uint32_t)k_ceu_capsr_cpkil);
  }
  return (uint64_t)s_ceu.reg[ceu_word(off)];
}

/** @brief MMIO write inside the CEU window: latch, then act on CAPSR. */
static void ceu_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint64_t off = addr - (uint64_t)k_ceu_base;
  if (off >= (uint64_t)k_ceu_span) {
    return;
  }
  s_ceu.reg[ceu_word(off)] = (uint32_t)value;
  if (off != (uint64_t)k_ceu_off_capsr) {
    return;
  }
  if (((uint32_t)value & (uint32_t)k_ceu_capsr_cpkil) != 0U) {
    /* Software reset: drop pending events and CE; CPKIL self-clears. */
    s_ceu.reg[ceu_word(k_ceu_off_cetcr)] = 0U;
    s_ceu.reg[ceu_word(k_ceu_off_capsr)] =
      (uint32_t)value & ~((uint32_t)k_ceu_capsr_cpkil | (uint32_t)k_ceu_capsr_ce);
    return;
  }
  if (((uint32_t)value & (uint32_t)k_ceu_capsr_ce) != 0U) {
    (void)ceu_do_capture(uc); /* Arm -> synthesise a frame + latch CETCR.CPE. */
  }
}

/** @brief Clear the CEU model to power-on state. */
static void ceu_reset(void)
{
  s_ceu = (ceu_state_t){};
}

/** @brief Print the CEU capture summary if any frame was synthesised. */
static void ceu_report(void)
{
  if (s_ceu.frames == 0U) {
    return;
  }
  (void)fprintf(stderr,
                "  CEU           : frames=%u last=%ux%u (%u bytes) synth test-pattern\n",
                s_ceu.frames,
                s_ceu.last_w,
                s_ceu.last_h,
                s_ceu.last_bytes);
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t k_ceu_block = {
  .base   = (uint64_t)k_ceu_base,
  .span   = (uint64_t)k_ceu_span,
  .order  = (uint32_t)k_block_order_i2c, /* Synchronous on CAPSR.CE; no tick. */
  .read   = ceu_read,
  .write  = ceu_write,
  .tick   = nullptr,
  .reset  = ceu_reset,
  .report = ceu_report,
  .name   = "CEU",
};

/** @brief Self-register the CEU block before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_ceu_register(void)
{
  board_periph_register_block(&k_ceu_block);
}
