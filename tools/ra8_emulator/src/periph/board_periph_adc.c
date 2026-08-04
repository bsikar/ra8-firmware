/**
 * @file board_periph_adc.c
 * @brief ADC16H (16-bit SAR) peripheral-block model for the board emulator
 *
 * @details
 * Models the RA8 ADC16H converter (ra8_adc_b_regs.h / adc.c) at base
 * 0x40338000 so a software-triggered conversion produces real result data
 * instead of the sparse fallback's all-ones junk. The driver's single-channel
 * polling read (``ra8_adc_read_channel``) drives the block as:
 *
 *  1. Programme an ADCHCRn slot (CNVCS = physical channel, SGSEL = scan group)
 *     plus that slot's ADDOPCRCn.ADPRC data-format (conversion resolution).
 *  2. Kick the scan by writing ADSTR[group].ADST = 1.
 *  3. Poll ADSR.ADACT0 until it reads 0 (unit idle), then read ADDR[ch].
 *
 * On the ADSTR write this model walks the configured ADCHCRn slots, populates
 * the matching ADDR[ch] result register with a resolution-appropriate mid-scale
 * (half-full-scale) code -- read from the slot's ADDOPCRCn.ADPRC field so a
 * 16-bit conversion reports 0x8000 while a 12-bit one reports 0x0800 (HUM
 * Ch 53.2.3.4 p 3339) -- leaves ADSR.ADACT0 clear (the conversion is modelled
 * as instantaneous, so the firmware's idle poll completes on its first read),
 * and raises the group's scan-complete (ADI) event through the core's ICU ->
 * NVIC path. Control registers read back what was written so the driver's
 * "configure then verify" sequence sees a coherent block.
 *
 * Internal / extended-analog channels (CNVCS >= ::k_adc_ext_chan_base: the
 * self-diagnosis source 0x60, the temperature sensor 0x64, the internal Vref
 * 0x65) report through ADEXDR[CNVCS - base], NOT the ordinary ADDR[] slot (HUM
 * Ch 53.2.13.2 p 3391). For those slots the model populates ADEXDR instead:
 * the self-diagnosis result is the IDEAL value for the armed ADSGDCRn.DIAGVAL
 * mode (0x0000 / 0x8000 / 0x7FFF -- a healthy SAR, so ra8_adc_self_diagnose's
 * band check passes; a stuck-SAR fault cannot be injected here and stays a
 * silicon-only test), the temperature channel returns ::k_adc_temp_code, and
 * every other extended source returns the mid-scale code. This lets
 * adc_diag_tsn_demo run its self-test + die-temperature path headless (paired
 * with the TSN factory-calibration seed ra8_emulator writes at 0x02C1EDA0).
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

/** @brief ADC_B block geometry (ra8_adc_b_regs.h, FSP R_ADC_B0_Type). */
typedef enum : uint64_t {
  k_adc_base          = 0x40338000UL, /**< ADC_B register-window base.           */
  k_adc_span          = 0x2224UL,     /**< Full FSP R_ADC_B0_Type window size.   */
  k_adc_off_adsger    = 0x0048UL,     /**< ADSGER scan-group enable.             */
  k_adc_off_adintcr   = 0x005CUL,     /**< ADINTCR per-group scan-end IE.        */
  k_adc_off_adsgdcr0  = 0x0200UL,     /**< ADSGDCR[0] scan-group self-diag ctrl. */
  k_adc_off_adchcr0   = 0x0600UL,     /**< ADCHCR[0] per-channel config.         */
  k_adc_off_addopcrc0 = 0x060CUL,     /**< ADDOPCRC[0] per-channel ADPRC format. */
  k_adc_off_adtrgenr  = 0x0C08UL,     /**< ADTRGENR per-group HW-trigger enable. */
  k_adc_off_adstr0    = 0x0C20UL,     /**< ADSTR[0] per-group SW start.          */
  k_adc_off_adstopr   = 0x0C60UL,     /**< ADSTOPR force-stop.                   */
  k_adc_off_adsr      = 0x0C80UL,     /**< ADSR conversion status (RO).          */
  k_adc_off_addr0     = 0x2000UL,     /**< ADDR[0] conversion results.           */
  k_adc_off_adexdr0   = 0x2180UL,     /**< ADEXDR[0] extended-analog results.    */
  k_adc_chcr_stride   = 0x10UL,       /**< ADCHCRn occupies 16 bytes per slot.   */
  k_adc_addr_stride   = 0x04UL,       /**< ADDR[n] is 4 bytes per slot.          */
  k_adc_str_stride    = 0x04UL,       /**< ADSTR[n] is 4 bytes per slot.         */
  k_adc_sgdcr_stride  = 0x04UL,       /**< ADSGDCRn is 4 bytes per slot.         */
  k_adc_exdr_stride   = 0x04UL,       /**< ADEXDRn is 4 bytes per slot.          */
} adc_map_t;

/** @brief ADC_B array dimensions (mirror ra8_adc_b_limits_t). */
typedef enum : uint32_t {
  k_adc_max_channels = 24U,          /**< ADCHCR0..23 virtual-channel config slots. */
  k_adc_result_regs  = 23U,          /**< ADDR[0..22] result slots.                 */
  k_adc_ext_regs     = 23U,          /**< ADEXDR[0..22] extended-analog slots.      */
  k_adc_scan_groups  = 9U,           /**< ADSGER / ADTRGENR / ADSTR width [8:0].    */
  k_adc_reg_words    = 0x2224U / 4U, /**< Backing-store word count.                 */
} adc_dim_t;

/** @brief ADC_B field shifts / masks the model consults. */
typedef enum : uint32_t {
  k_adc_adst_mask     = 0x00000001UL, /**< ADSTR[n].ADST start bit.          */
  k_adc_adact0_mask   = 0x00000001UL, /**< ADSR.ADACT0 unit-0 busy flag.     */
  k_adc_addr_data     = 0x0000FFFFUL, /**< ADDR[n] DATA[15:0] field.         */
  k_adc_chcr_sgsel    = 0x0000001FUL, /**< ADCHCRn.SGSEL[4:0] scan group.    */
  k_adc_chcr_cnvcs_m  = 0x00007F00UL, /**< ADCHCRn.CNVCS[14:8] phys channel. */
  k_adc_chcr_cnvcs_s  = 8U,           /**< ADCHCRn.CNVCS shift.              */
  k_adc_sgdcr_diagval = 0x00000007UL, /**< ADSGDCRn.DIAGVAL[2:0] mode.       */
  k_adc_opcrc_adprc_m = 0x00030000UL, /**< ADDOPCRCn.ADPRC[17:16] format.    */
  k_adc_opcrc_adprc_s = 16U,          /**< ADDOPCRCn.ADPRC shift.            */
} adc_field_t;

/**
 * @brief ADDOPCRCn.ADPRC[1:0] data-format codes (HUM Ch 53.2.3.4 p 3339).
 *
 * @details
 * Selects the conversion resolution / result width per virtual channel:
 * 0b00 -> 16-bit, 0b01 -> 14-bit, 0b10 -> 12-bit, 0b11 -> 10-bit. The model
 * uses this to pick a resolution-appropriate mid-scale (::adc_midscale_t)
 * result code for a converted channel.
 */
typedef enum : uint32_t {
  k_adc_adprc_16bit = 0x0U, /**< ADPRC 0b00: 16-bit result. */
  k_adc_adprc_14bit = 0x1U, /**< ADPRC 0b01: 14-bit result. */
  k_adc_adprc_12bit = 0x2U, /**< ADPRC 0b10: 12-bit result. */
  k_adc_adprc_10bit = 0x3U, /**< ADPRC 0b11: 10-bit result. */
} adc_adprc_t;

/**
 * @brief Resolution-appropriate mid-scale (half-full-scale) result codes.
 *
 * @details
 * A "mid-scale" analog input converts to half of the full-scale code, whose
 * magnitude tracks the ADPRC data-format width. The model reports these so a
 * 16-bit conversion looks 16-bit (0x8000) and a 12-bit one looks 12-bit
 * (0x0800), instead of always the old fixed 12-bit half-scale.
 */
typedef enum : uint16_t {
  k_adc_mid_16bit = 0x8000U, /**< 16-bit half-scale. */
  k_adc_mid_14bit = 0x2000U, /**< 14-bit half-scale. */
  k_adc_mid_12bit = 0x0800U, /**< 12-bit half-scale. */
  k_adc_mid_10bit = 0x0200U, /**< 10-bit half-scale. */
} adc_midscale_t;

/**
 * @brief Extended-analog (internal) channel CNVCS codes (ra8_adc_b_regs.h).
 *
 * @details
 * CNVCS values at or above ::k_adc_ext_chan_base do not sample an external pin;
 * they route an on-chip source whose result is read back from ADEXDR[CNVCS -
 * base], NOT the ordinary ADDR[] slot (HUM Ch 53.2.13.2 p 3391). The model
 * populates ADEXDR for these so ra8_adc_self_diagnose / ra8_adc_read_internal_channel
 * see real data instead of an all-zero result.
 */
typedef enum : uint32_t {
  k_adc_ext_chan_base = 0x60U, /**< ADEXDR index origin (n = CNVCS - base). */
  k_adc_chan_selfdiag = 0x60U, /**< Self-diagnosis A/D unit 0.              */
  k_adc_chan_temp     = 0x64U, /**< On-chip temperature sensor.             */
  k_adc_chan_int_vref = 0x65U, /**< Internal reference voltage.             */
} adc_ext_chan_t;

/**
 * @brief ADSGDCRn.DIAGVAL self-diagnosis mode codes + ideal 16-bit results.
 *
 * @details
 * HUM Table 53.19 (p 3412): mode 1 (DIAGVAL 0x4) drives 0x0000, mode 2 (0x5)
 * negative full-scale 0x8000, mode 3 (0x6) positive full-scale 0x7FFF. This
 * model returns the IDEAL per mode: it represents a healthy SAR + reference
 * (which is the only faithful behaviour when there is no analog core to fault),
 * so ra8_adc_self_diagnose's +/-256-LSB band check passes. The fault path (a
 * stuck SAR) cannot be injected here and is a silicon-only test.
 */
typedef enum : uint32_t {
  k_adc_diagval_mode1  = 0x4U,    /**< DIAGVAL mode 1: zero.                */
  k_adc_diagval_mode2  = 0x5U,    /**< DIAGVAL mode 2: negative full-scale. */
  k_adc_diagval_mode3  = 0x6U,    /**< DIAGVAL mode 3: positive full-scale. */
  k_adc_selfdiag_zero  = 0x0000U, /**< Mode-1 ideal 0x0000.                 */
  k_adc_selfdiag_negfs = 0x8000U, /**< Mode-2 ideal 0x8000 (-32768).        */
  k_adc_selfdiag_posfs = 0x7FFFU, /**< Mode-3 ideal 0x7FFF (+32767).        */
} adc_selfdiag_t;

/** @brief Plausible conversion result reported on every scan. */
typedef enum : uint16_t {
  k_adc_sample_code = 2048U, /**< 12-bit half-scale (~VREFH/2) mid-scale. */
  /* Temperature-sensor code: with the factory calibration ra8_emulator seeds at
   * 0x02C1EDA0 (TSCDR=3000 @125C, TSCDR2=1000 @-40C), the two-point math in
   * ra8_tsn_convert_to_milli_c maps this 12-bit code to ~26 degC -- a plausible,
   * deterministic die temperature. See main.c k_tsn_cal_* for the paired seed. */
  k_adc_temp_code = 1800U, /**< 12-bit temperature-sensor code (~26 degC). */
} adc_sample_t;

/**
 * @brief ADC_B scan-complete (ADI) ELC event the model raises on a scan start.
 *
 * @details
 * RA8D2 ELC event signal table (HUM Ch 19): the ADC0 scan-end / conversion-end
 * group interrupt (ADI0) is 0x09C. A firmware that routes scan-complete through
 * ra8_isr_register writes the same number into an IELSR slot, so the ICU model
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
                            ((uint64_t)ch * (uint64_t)k_adc_chcr_stride))];
}

/** @brief Store a freshly converted code into ADDR[ch] (DATA[15:0], ERR clear). */
static void adc_set_result(uint32_t ch, uint16_t code)
{
  if (ch >= (uint32_t)k_adc_result_regs) {
    return;
  }
  const uint64_t off = (uint64_t)k_adc_off_addr0 + ((uint64_t)ch * (uint64_t)k_adc_addr_stride);
  s_adc_reg[adc_word(off)] = (uint32_t)code & (uint32_t)k_adc_addr_data;
  s_adc_last_code          = code;
}

/** @brief Store an extended-analog result into ADEXDR[idx] (DATA[15:0], ERR clear). */
static void adc_set_ext_result(uint32_t idx, uint16_t code)
{
  if (idx >= (uint32_t)k_adc_ext_regs) {
    return;
  }
  const uint64_t off = (uint64_t)k_adc_off_adexdr0 + ((uint64_t)idx * (uint64_t)k_adc_exdr_stride);
  s_adc_reg[adc_word(off)] = (uint32_t)code & (uint32_t)k_adc_addr_data;
  s_adc_last_code          = code;
}

/** @brief Read ADSGDCR[group].DIAGVAL[2:0] (self-diagnosis mode) from the store. */
static uint32_t adc_diagval(uint32_t group)
{
  const uint64_t off =
    (uint64_t)k_adc_off_adsgdcr0 + ((uint64_t)group * (uint64_t)k_adc_sgdcr_stride);
  return s_adc_reg[adc_word(off)] & (uint32_t)k_adc_sgdcr_diagval;
}

/** @brief Ideal 16-bit self-diagnosis code for the armed DIAGVAL mode. */
static uint16_t adc_selfdiag_ideal(uint32_t diagval)
{
  switch (diagval) {
    case (uint32_t)k_adc_diagval_mode2:
      return (uint16_t)k_adc_selfdiag_negfs;
    case (uint32_t)k_adc_diagval_mode3:
      return (uint16_t)k_adc_selfdiag_posfs;
    default:
      return (uint16_t)k_adc_selfdiag_zero; /* mode 1 / DIAGVAL off. */
  }
}

/** @brief Synthetic ADEXDR code for an extended-analog channel (phys >= base). */
static uint16_t adc_ext_value(uint32_t phys, uint32_t group)
{
  if (phys == (uint32_t)k_adc_chan_selfdiag) {
    return adc_selfdiag_ideal(adc_diagval(group));
  }
  if (phys == (uint32_t)k_adc_chan_temp) {
    return (uint16_t)k_adc_temp_code;
  }
  return (uint16_t)k_adc_sample_code; /* internal Vref + any other ext source. */
}

/** @brief Read ADDOPCRC[slot].ADPRC[1:0] data-format code from the store. */
static uint32_t adc_adprc(uint32_t slot)
{
  const uint64_t off =
    (uint64_t)k_adc_off_addopcrc0 + ((uint64_t)slot * (uint64_t)k_adc_chcr_stride);
  return (s_adc_reg[adc_word(off)] & (uint32_t)k_adc_opcrc_adprc_m) >>
         (uint32_t)k_adc_opcrc_adprc_s;
}

/** @brief Resolution-appropriate mid-scale result code for an ADPRC format. */
static uint16_t adc_midscale_for_adprc(uint32_t adprc)
{
  switch (adprc) {
    case (uint32_t)k_adc_adprc_16bit:
      return (uint16_t)k_adc_mid_16bit;
    case (uint32_t)k_adc_adprc_14bit:
      return (uint16_t)k_adc_mid_14bit;
    case (uint32_t)k_adc_adprc_10bit:
      return (uint16_t)k_adc_mid_10bit;
    default: /* 12-bit (0b10) and any unmodelled code. */
      return (uint16_t)k_adc_mid_12bit;
  }
}

/**
 * @brief Populate result registers for every channel enrolled in @p group.
 *
 * @details
 * Walks the 24 ADCHCRn slots; any slot whose SGSEL matches the started group is
 * a virtual channel that converts. A normal (external-pin) channel reports to
 * its own virtual-channel result register ADDR[slot] -- the same index the
 * driver reads back (driver convention virtual_ch == physical_ch) -- filled
 * with a resolution-scaled mid-scale sample taken from the slot's ADDOPCRCn.ADPRC
 * data-format. A slot with no ADDR register (the top diagnostic slot) produces
 * no ordinary result; an internal / extended-analog channel reports through
 * ADEXDR instead.
 */
static void adc_convert_group(uint32_t group)
{
  uint32_t converted = 0U;
  uint16_t last_code = 0U;
  for (uint32_t slot = 0U; slot < (uint32_t)k_adc_max_channels; slot++) {
    const uint32_t chcr = adc_chcr(slot);
    if ((chcr & (uint32_t)k_adc_chcr_sgsel) != group) {
      continue;
    }
    const uint32_t phys = (chcr & (uint32_t)k_adc_chcr_cnvcs_m) >> (uint32_t)k_adc_chcr_cnvcs_s;
    if (phys >= (uint32_t)k_adc_ext_chan_base) {
      /* An internal / extended-analog channel (self-diagnosis, temperature
       * sensor, internal Vref) reports through ADEXDR[CNVCS - base], not ADDR[]. */
      last_code = adc_ext_value(phys, group);
      adc_set_ext_result(phys - (uint32_t)k_adc_ext_chan_base, last_code);
      converted++;
    } else if (slot < (uint32_t)k_adc_result_regs) {
      /* Normal channel -> its virtual-channel result register ADDR[slot].
       * Resolution is the slot's ADDOPCRCn.ADPRC data-format, so a mid-scale
       * input converts to that format's half-full-scale code. */
      last_code = adc_midscale_for_adprc(adc_adprc(slot));
      adc_set_result(slot, last_code);
      converted++;
    }
    /* else: a normal channel on a virtual slot with no ADDR result register
     * (the diagnostic slot) produces no ordinary result. */
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
                   (unsigned)last_code);
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
  const uint64_t str_hi = str_lo + ((uint64_t)k_adc_scan_groups * (uint64_t)k_adc_str_stride);
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
                    (unsigned)s_adc_last_code);
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
[[gnu::constructor]] static void board_periph_adc_register(void)
{
  board_periph_register_block(&k_adc_block);
}
