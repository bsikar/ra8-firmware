/**
 * @file board_periph_adc.c
 * @brief ADC_B (12 / 14-bit SAR) peripheral-block model for the board emulator
 *
 * @details
 * Models the RA8D2 ADC_B converter (ra8d2_adc_b_regs.h / adc.c) at base
 * 0x40338000 so a software-triggered conversion produces real result data
 * instead of the sparse fallback's all-ones junk. The driver's single-channel
 * polling read (``ra_adc_read_channel``) drives the block as:
 *
 *  1. Programme an ADCHCRn slot (CNVCS = physical channel, SGSEL = scan group).
 *  2. Kick the scan by writing ADSTR[group].ADST = 1.
 *  3. Poll ADSR.ADACT0 until it reads 0 (unit idle), then read ADDR[ch].
 *
 * On the ADSTR write this model walks the configured ADCHCRn slots, populates
 * the matching ADDR[ch] result register with a plausible mid-scale code (the
 * 12-bit half-scale, ::k_adc_sample_code), leaves ADSR.ADACT0 clear (the
 * conversion is modelled as instantaneous, so the firmware's idle poll
 * completes on its first read), and raises the group's scan-complete (ADI)
 * event through the core's ICU -> NVIC path. Control registers read back what
 * was written so the driver's "configure then verify" sequence sees a coherent
 * block.
 *
 * Self-registers its descriptor with the board_periph core from a file-scope
 * constructor; the core keeps no central block list -- see board_periph_block.h.
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

/** @brief Console-tap line buffer capacity for an ADC conversion summary. */
typedef enum : uint32_t {
  k_adc_console_line_cap = 48U, /**< Max chars in an "ADC grp=.. .." line. */
} adc_console_t;

/** @brief ADC_B block geometry (ra8d2_adc_b_regs.h, FSP R_ADC_B0_Type). */
typedef enum : uint64_t {
  k_adc_base         = 0x40338000UL, /**< ADC_B register-window base.           */
  k_adc_span         = 0x2224UL,     /**< Full FSP R_ADC_B0_Type window size.   */
  k_adc_off_adsger   = 0x0048UL,     /**< ADSGER scan-group enable.             */
  k_adc_off_adintcr  = 0x005CUL,     /**< ADINTCR per-group scan-end IE.        */
  k_adc_off_adchcr0  = 0x0600UL,     /**< ADCHCR[0] per-channel config.         */
  k_adc_off_adtrgenr = 0x0C08UL,     /**< ADTRGENR per-group HW-trigger enable. */
  k_adc_off_adstr0   = 0x0C20UL,     /**< ADSTR[0] per-group SW start.          */
  k_adc_off_adstopr  = 0x0C60UL,     /**< ADSTOPR force-stop.                   */
  k_adc_off_adsr     = 0x0C80UL,     /**< ADSR conversion status (RO).          */
  k_adc_off_addr0    = 0x2000UL,     /**< ADDR[0] conversion results.           */
  k_adc_chcr_stride  = 0x10UL,       /**< ADCHCRn occupies 16 bytes per slot.   */
  k_adc_addr_stride  = 0x04UL,       /**< ADDR[n] is 4 bytes per slot.          */
  k_adc_str_stride   = 0x04UL,       /**< ADSTR[n] is 4 bytes per slot.         */
} adc_map_t;

/** @brief ADC_B array dimensions (mirror ra_adc_b_limits_t). */
typedef enum : uint32_t {
  k_adc_max_channels = 24U,          /**< ADCHCR0..23 virtual-channel config slots. */
  k_adc_result_regs  = 23U,          /**< ADDR[0..22] result slots.                 */
  k_adc_scan_groups  = 9U,           /**< ADSGER / ADTRGENR / ADSTR width [8:0].    */
  k_adc_reg_words    = 0x2224U / 4U, /**< Backing-store word count.                 */
} adc_dim_t;

/** @brief ADC_B field shifts / masks the model consults. */
typedef enum : uint32_t {
  k_adc_adst_mask    = 0x00000001UL, /**< ADSTR[n].ADST start bit.          */
  k_adc_adact0_mask  = 0x00000001UL, /**< ADSR.ADACT0 unit-0 busy flag.     */
  k_adc_addr_data    = 0x0000FFFFUL, /**< ADDR[n] DATA[15:0] field.         */
  k_adc_chcr_sgsel   = 0x0000001FUL, /**< ADCHCRn.SGSEL[4:0] scan group.    */
  k_adc_chcr_cnvcs_m = 0x00007F00UL, /**< ADCHCRn.CNVCS[14:8] phys channel. */
  k_adc_chcr_cnvcs_s = 8U,           /**< ADCHCRn.CNVCS shift.              */
} adc_field_t;

/** @brief Plausible conversion result reported on every scan. */
typedef enum : uint16_t {
  k_adc_sample_code = 2048U, /**< 12-bit half-scale (~VREFH/2) mid-scale. */
} adc_sample_t;

/**
 * @brief ADC_B scan-complete (ADI) ELC event the model raises on a scan start.
 *
 * @details
 * RA8D2 ELC event signal table (HUM Ch 19): the ADC0 scan-end / conversion-end
 * group interrupt (ADI0) is 0x09C. A firmware that routes scan-complete through
 * ra_isr_register writes the same number into an IELSR slot, so the ICU model
 * can match the raised event to that slot. The polling demo does not arm it,
 * but raising it keeps an interrupt-driven scan path observable.
 */
typedef enum : uint16_t {
  k_event_adc0_scan_end = 0x09CU, /**< ADC0 ADI scan-complete event. */
} adc_elc_event_t;

/**
 * @brief Backing store for the whole ADC_B window (word-addressed).
 *
 * @details
 * Most ADC_B registers are plain read-back control words, so a flat word array
 * answers them; the model only adds behaviour for ADSTR (scan start), ADSR
 * (busy flag held idle) and ADDR (populated result). A scan-result counter
 * feeds the end-of-run report line.
 */
static uint32_t s_adc_reg[k_adc_reg_words];
static uint32_t s_adc_scans;     /**< Conversions kicked via ADSTR.        */
static uint16_t s_adc_last_code; /**< Last result code written to an ADDR. */

/** @brief Word index into the backing store for an in-window offset. */
static uint32_t adc_word(uint64_t off)
{
  return (uint32_t)(off >> 2U);
}

/** @brief Read an ADCHCRn slot's raw config word from the backing store. */
static uint32_t adc_chcr(uint32_t ch)
{
  return s_adc_reg[adc_word((uint64_t)k_adc_off_adchcr0 +
                            (uint64_t)ch * (uint64_t)k_adc_chcr_stride)];
}

/** @brief Store a freshly converted code into ADDR[ch] (DATA[15:0], ERR clear). */
static void adc_set_result(uint32_t ch, uint16_t code)
{
  if (ch >= (uint32_t)k_adc_result_regs) {
    return;
  }
  const uint64_t off       = (uint64_t)k_adc_off_addr0 + (uint64_t)ch * (uint64_t)k_adc_addr_stride;
  s_adc_reg[adc_word(off)] = (uint32_t)code & (uint32_t)k_adc_addr_data;
  s_adc_last_code          = code;
}

/**
 * @brief Populate result registers for every channel enrolled in @p group.
 *
 * @details
 * Walks the 24 ADCHCRn slots; any slot whose SGSEL matches the started group
 * gets its CNVCS physical channel's ADDR[ch] filled with the mid-scale sample.
 * ADDR is indexed by the same physical-channel number the driver reads back
 * (driver convention virtual_ch == physical_ch), so the polling read lands on
 * a populated slot.
 */
static void adc_convert_group(uint32_t group)
{
  uint32_t converted = 0U;
  for (uint32_t slot = 0U; slot < (uint32_t)k_adc_max_channels; slot++) {
    const uint32_t chcr = adc_chcr(slot);
    if ((chcr & (uint32_t)k_adc_chcr_sgsel) != group) {
      continue;
    }
    const uint32_t phys = (chcr & (uint32_t)k_adc_chcr_cnvcs_m) >> (uint32_t)k_adc_chcr_cnvcs_s;
    adc_set_result(phys, (uint16_t)k_adc_sample_code);
    converted++;
  }
  /* Console ADC tab: one line per scan-group conversion (coalesced -- a whole
   * group is one push, never one per channel), skipped when the group is empty. */
  if (converted != 0U) {
    char ln[k_adc_console_line_cap];
    (void)snprintf(ln,
                   sizeof(ln),
                   "ADC grp=%u %uch code=%u",
                   (unsigned)group,
                   (unsigned)converted,
                   (unsigned)k_adc_sample_code);
    board_console_push(k_board_console_ch_adc, ln);
  }
}

/** @brief MMIO read: ADSR holds ADACT0 idle; everything else reads back. */
static uint64_t adc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_adc_base;
  if (off == (uint64_t)k_adc_off_adsr) {
    /* Conversion is modelled as instantaneous: ADACT0 reads idle so the
     * driver's busy-poll completes on its first read and fetches ADDR. */
    return s_adc_reg[adc_word(off)] & ~(uint32_t)k_adc_adact0_mask;
  }
  if (off >= (uint64_t)k_adc_span) {
    return 0U;
  }
  return s_adc_reg[adc_word(off)];
}

/** @brief MMIO write: ADSTR kicks a scan; other registers latch the value. */
static void adc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint64_t off = addr - (uint64_t)k_adc_base;
  if (off >= (uint64_t)k_adc_span) {
    return;
  }
  const uint64_t str_lo = (uint64_t)k_adc_off_adstr0;
  const uint64_t str_hi = str_lo + (uint64_t)k_adc_scan_groups * (uint64_t)k_adc_str_stride;
  if ((off >= str_lo) && (off < str_hi) && ((value & (uint64_t)k_adc_adst_mask) != 0U)) {
    const uint32_t group     = (uint32_t)((off - str_lo) / (uint64_t)k_adc_str_stride);
    s_adc_reg[adc_word(off)] = (uint32_t)value;
    adc_convert_group(group);
    s_adc_scans++;
    board_periph_icu_raise_event(uc, (uint16_t)k_event_adc0_scan_end);
    if (board_periph_trace()) {
      (void)fprintf(stderr,
                    "  [trace] ADC_B scan grp%u -> code=%u\n",
                    group,
                    (unsigned)k_adc_sample_code);
    }
    return;
  }
  s_adc_reg[adc_word(off)] = (uint32_t)value;
}

/** @brief Clear all ADC_B register state and observability counters. */
static void adc_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_adc_reg_words; i++) {
    s_adc_reg[i] = 0U;
  }
  s_adc_scans     = 0U;
  s_adc_last_code = 0U;
}

/** @brief Print the ADC_B scan count + last reported result code. */
static void adc_report(void)
{
  if (s_adc_scans == 0U) {
    return;
  }
  (void)fprintf(stderr,
                "  ADC_B         : scans=%u last_code=%u (0x%03X)\n",
                s_adc_scans,
                s_adc_last_code,
                s_adc_last_code);
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t k_adc_block = {
  .base   = (uint64_t)k_adc_base,
  .span   = (uint64_t)k_adc_span,
  .order  = (uint32_t)k_block_order_i2c, /* After the timers/UART; no tick. */
  .read   = adc_read,
  .write  = adc_write,
  .tick   = nullptr,
  .reset  = adc_reset,
  .report = adc_report,
  .name   = "ADC_B",
};

/** @brief Self-register the ADC_B block before main runs (decentralized). */
__attribute__((constructor)) static void board_periph_adc_register(void)
{
  board_periph_register_block(&k_adc_block);
}
