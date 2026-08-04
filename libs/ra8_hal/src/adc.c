/**
 * @file adc.c
 * @brief ADC16H (16-bit SAR) converter driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8 ADC_B peripheral -- the 16-bit ADC16H SAR block
 * shared byte-for-byte by the RA8D2 and RA8P1. Its register layout was
 * cross-verified against FSP ``R_ADC_B0_Type``
 * (``R7KA8D2KF_core0.h`` lines 20460-24938) and against the FSP
 * driver in ``ra/fsp/src/r_adc_b/r_adc_b.c``.
 *
 * The bring-up sequence used here follows the FSP ``R_ADC_B_Open``
 * order:
 *   1. Release MSTP gate (HUM Ch 11.2.9 MSTPCRD).
 *   2. Disable ADCLK (write 0 to ADCLKENR), wait CLKSR == 0.
 *   3. Programme ADCLKCR (clock source / divider).
 *   4. Enable ADCLK (write 1 to ADCLKENR), wait CLKSR == 1.
 *   5. Programme ADMDR per-unit operating mode (ADMD0[3:0], ADMD1[11:8]).
 *   6. Programme each ADCHCRn slot (channel selection) and each
 *      ADDOPCRCn.ADPRC data-format (conversion resolution).
 *   7. Enable scan-group via ADSGER, kick via ADSYSTR / ADSTR.
 *   8. Poll ADSR.ADACT0/1, read ADDR[ch].
 *
 * Conversion resolution (10/12/14/16-bit) is the per-channel
 * ADDOPCRCn.ADPRC[1:0] data-format field (HUM Ch 53.2.3.4 p 3339), NOT
 * an ADMDR mode: ADMDR.ADMD0 only selects the unit's operating mode
 * (single-shot / one-cycle / continuous). The previous version of this
 * driver modelled non-existent ADCSR / ADCER / RESSEL / CVEN bits and
 * folded "resolution" into the ADMD0 mode code; both were wrong and
 * have been deleted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "adc_internal.h"
#include "ra8_adc.h"
#include "ra8_adc_b_regs.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "ADC";

/**
 * @enum ra8_adc_default_group_t
 * @brief Scan-group used by the legacy single-channel polling API.
 */
typedef enum : uint8_t {
  k_ra8_adc_default_group = 0U, /**< Group 0 owns SW-trigger conversions. */
} ra8_adc_default_group_t;

/**
 * @struct ra8_adc_group_cache_t
 * @brief Per-scan-group channel-list cache.
 *
 * @details
 * ``ra8_adc_configure_scan_group`` stores the channel list here so that
 * ``ra8_adc_read_group_results`` can stream the matching ADDR slots
 * back without re-walking ADCHCR. Indexed by scan-group number.
 */
typedef struct {
  uint8_t num_channels;                                /**< 0 means group unconfigured. */
  uint8_t channels[k_ra8_adc_scan_group_max_channels]; /**< Physical channel list.      */
} ra8_adc_group_cache_t;

/**
 * @struct ra8_adc_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra8_adc_complete_fn_t fn;         /**< Conversion-complete callback or nullptr. */
  void*                 ctx;        /**< Callback context.                        */
  ra8_adc_resolution_t  resolution; /**< Last-applied resolution selection.       */
  bool                  configured; /**< True after first successful init.        */
  /** Per-group channel cache. */
  ra8_adc_group_cache_t groups[k_ra8_adc_b_scan_groups];
} ra8_adc_state_t;

static ra8_adc_state_t s_adc_state;

/**
 * @brief Translate a public resolution selector into its ADDOPCRC.ADPRC code.
 *
 * @details
 * Conversion resolution on the ADC16H is the per-channel
 * ADDOPCRCn.ADPRC[1:0] data-format field (HUM Ch 53.2.3.4 p 3339), not
 * an ADMDR mode. This maps each ::ra8_adc_resolution_t selector onto the
 * matching ::ra8_addopcrc_adprc_t code; an unknown selector is rejected
 * so callers can fail cleanly.
 *
 * @param[in]  resolution Public resolution selector.
 * @param[out] out_adprc  Receives the ADPRC[1:0] code on success.
 * @return True when @p resolution is a known selector, false otherwise.
 *
 * @retval true  @p resolution is known; @p out_adprc holds the ADPRC code.
 * @retval false @p resolution is out of range; @p out_adprc left untouched.
 * @pre @p out_adprc is non-null.
 * @pre @p resolution is sourced from ra8_adc_resolution_t.
 * @post @p out_adprc is written only when the function returns true.
 * @post No registers are accessed (pure translation).
 * @note Re-entrant; touches no globals or MMIO.
 * @since 0.1.0
 */
static bool internal_adprc_for_resolution(ra8_adc_resolution_t resolution, uint32_t* out_adprc)
{
  switch (resolution) {
    case k_ra8_adc_res_16bit:
      *out_adprc = (uint32_t)k_ra8_addopcrc_adprc_16bit;
      return true;
    case k_ra8_adc_res_14bit:
      *out_adprc = (uint32_t)k_ra8_addopcrc_adprc_14bit;
      return true;
    case k_ra8_adc_res_12bit:
      *out_adprc = (uint32_t)k_ra8_addopcrc_adprc_12bit;
      return true;
    case k_ra8_adc_res_10bit:
      *out_adprc = (uint32_t)k_ra8_addopcrc_adprc_10bit;
      return true;
    default:
      return false;
  }
}

/**
 * @brief Write the ADPRC data-format field of one ADDOPCRCn slot (RMW).
 *
 * @details
 * Read-modify-writes ADDOPCRC[@p vch], replacing only ADPRC[1:0] and
 * leaving SIGNSEL and the reserved bits intact (HUM Ch 53.2.3.4 p 3339).
 * A slot whose accessor is out of range is skipped defensively.
 *
 * @param[in] vch   Virtual-channel (ADDOPCRC) slot index.
 * @param[in] adprc ADPRC[1:0] data-format code to install.
 *
 * @pre @p vch indexes a result-producing virtual channel.
 * @pre @p adprc is sourced from ra8_addopcrc_adprc_t.
 * @post On a valid slot ADDOPCRCn.ADPRC == @p adprc.
 * @post No write occurs when @p vch is out of range.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_write_adprc(uint8_t vch, uint32_t adprc)
{
  volatile uint32_t* reg = ra8_adc_b_addopcrc(vch);
  if (reg == nullptr) {
    return;
  }
  const uint32_t adprc_field =
    (adprc << (uint32_t)k_ra8_addopcrc_bit_adprc) & k_ra8_addopcrc_mask_adprc;
  /* HUM Ch 53.2.3.4 "ADDOPCRCn : A/D Conversion Data Operation Control C Register n" p 3339 */
  *reg = (*reg & ~k_ra8_addopcrc_mask_adprc) | adprc_field;
}

/**
 * @brief Install one ADPRC data-format code on every result-producing channel.
 *
 * @details
 * Programs @p adprc into each ADDOPCRC[0..``k_ra8_adc_b_result_regs``-1] slot
 * so a conversion on any result channel reports in the matching precision. The
 * dedicated self-diagnosis slot (top ADCHCR index) is deliberately excluded --
 * ``ra8_adc_self_diagnose`` forces its own 16-bit signed format per run. The
 * caller is responsible for validating the resolution -> ADPRC translation.
 *
 * @param[in] adprc ADPRC[1:0] data-format code (::ra8_addopcrc_adprc_t).
 *
 * @pre @p adprc is a valid ADPRC data-format code.
 * @pre The ADC clock is running (init has completed).
 * @post Every result-channel ADDOPCRCn.ADPRC == @p adprc.
 * @post No slot outside 0..result_regs-1 is modified.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_apply_resolution_code(uint32_t adprc)
{
  for (uint8_t ch = 0U; ch < k_ra8_adc_b_result_regs; ++ch) {
    internal_write_adprc(ch, adprc);
  }
}

/**
 * @brief Best-effort wait for ADCLKSR.CLKSR to match the requested level.
 *
 * @details
 * On real silicon CLKSR follows ADCLKENR within a handful of cycles
 * (HUM Ch 53). On the host ``ra8_fake_mmap`` window CLKSR is just
 * anonymous RAM, so the wait is a bounded no-op. We mirror the
 * FSP ``FSP_HARDWARE_REGISTER_WAIT`` pattern: poll a fixed budget
 * and continue regardless rather than fail-loud.
 *
 * @param[in] desired Either ``k_ra8_adclksr_mask_clksr`` (running) or 0 (off).
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_wait_clksr(uint32_t desired)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint32_t i = 0U; i < k_ra8_adc_clk_wait_limit; ++i) {            /* GCOVR_EXCL_BR_LINE */
    if ((*ra8_adc_b_adclksr() & k_ra8_adclksr_mask_clksr) == desired) { /* GCOVR_EXCL_BR_LINE */
      return;
    }
  }
}

/**
 * @brief Bring ADCLK up: ENR=0 -> wait CLKSR=0 -> CR=src -> ENR=1 -> CLKSR=1.
 *
 * @param[in] clkcr_value ADCLKCR value to programme (CLKSEL[1:0], DIVR[18:16]).
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_clock_bring_up(uint32_t clkcr_value)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra8_adc_b_adclkenr() = 0U;
  internal_wait_clksr(0U);
  *ra8_adc_b_adclkcr()  = clkcr_value;
  *ra8_adc_b_adclkenr() = k_ra8_adclkenr_mask_clken;
  internal_wait_clksr(k_ra8_adclksr_mask_clksr);
}

/**
 * @brief Default-zero every per-channel ADCHCRn slot.
 *
 * @details
 * FSP ``R_ADC_B_Open`` clears every ADCHCRn before the user
 * populates per-channel descriptors; we mirror that. This also
 * ensures a stale CNVCS / SGSEL field cannot leak between
 * configurations.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_zero_channel_table(void)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint8_t ch = 0U; ch < k_ra8_adc_b_max_channels; ++ch) {
    volatile uint32_t* reg = ra8_adc_b_adchcr(ch);
    if (reg != nullptr) {
      *reg = 0U;
    }
  }
}

/**
 * @brief Programme an ADCHCRn slot to map a physical channel into the
 *        default scan group with no special sample-and-hold.
 *
 * @param[in] virtual_ch  Index of the ADCHCR slot to programme (0..23).
 * @param[in] physical_ch Physical analog channel selector (0..127).
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_program_channel(uint8_t virtual_ch, uint8_t physical_ch)
{
  volatile uint32_t* reg = ra8_adc_b_adchcr(virtual_ch);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  const uint32_t cnvcs =
    ((uint32_t)physical_ch << (uint32_t)k_ra8_adchcr_bit_cnvcs) & k_ra8_adchcr_mask_cnvcs;
  const uint32_t sgsel = ((uint32_t)k_ra8_adc_default_group << (uint32_t)k_ra8_adchcr_bit_sgsel) &
                         k_ra8_adchcr_mask_sgsel;
  *reg                 = (cnvcs | sgsel);
}

/**
 * @brief Build the ADMDR value (ADMD0 only -- ADC1 stays disabled).
 *
 * @details
 * ADMDR.ADMD0[3:0] selects the ADC0 operating mode: continuous scan when
 * @p scan_mode is set, one-cycle scan otherwise. Conversion resolution is
 * orthogonal (ADDOPCRC.ADPRC) and is not encoded here.
 *
 * @param[in] scan_mode True for continuous scan, false for one-shot.
 * @return Composite ADMDR value (ADMD0 field only).
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_build_admdr(bool scan_mode)
{
  const uint32_t admd0_code =
    scan_mode ? (uint32_t)k_ra8_admdr_mode_continuous : (uint32_t)k_ra8_admdr_mode_one_cycle;
  return (admd0_code << (uint32_t)k_ra8_admdr_bit_admd0) & k_ra8_admdr_mask_admd0;
}

ra8_err_t ra8_adc_init(void)
{
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D" p 449 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_adc16h);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "adc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  internal_clock_bring_up(0U);

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra8_adc_b_admdr()   = internal_build_admdr(false);
  *ra8_adc_b_adintcr() = 0U;
  *ra8_adc_b_adsger()  = (uint32_t)(1UL << (uint32_t)k_ra8_adc_default_group);

  internal_zero_channel_table();
  internal_apply_resolution_code((uint32_t)k_ra8_addopcrc_adprc_14bit);

  for (uint8_t g = 0U; g < k_ra8_adc_b_scan_groups; ++g) {
    s_adc_state.groups[g].num_channels = 0U;
  }

  s_adc_state.resolution = k_ra8_adc_res_14bit;
  s_adc_state.configured = true;
  ra8_log_info(s_tag, "adc_init ready");
  return k_ra8_ok;
}

ra8_err_t ra8_adc_read_channel(uint8_t channel, uint16_t* out_raw)
{
  RA8_CHECK_NULL_PTR(out_raw, s_tag, "out_raw must not be nullptr");

  volatile uint32_t* chcr = ra8_adc_b_adchcr(channel);
  volatile uint32_t* addr = ra8_adc_b_addr(channel);
  if ((chcr == nullptr) || (addr == nullptr)) {
    return k_ra8_err_out_of_range;
  }

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Map this slot onto physical channel == ``channel`` in the
   * default scan group (driver convention: virtual_ch == physical_ch). */
  internal_program_channel(channel, channel);

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Kick the conversion via per-group ADSTR[default_group].ADST. */
  volatile uint32_t* adstr = ra8_adc_b_adstr(k_ra8_adc_default_group);
  if (adstr != nullptr) {
    *adstr = k_ra8_adstr_mask_adst;
  }

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Poll ADSR.ADACT0 -- hardware clears it when the unit becomes idle. */
  for (uint32_t i = 0U; i < k_ra8_adc_busy_wait_limit; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*ra8_adc_b_adsr() & k_ra8_adsr_mask_adact0) == 0U) { /* GCOVR_EXCL_BR_LINE */
      *out_raw = (uint16_t)(*addr & k_ra8_addr_mask_data);
      return k_ra8_ok;
    }
  }

  *out_raw = 0U;
  ra8_log_error(s_tag, "ADACT0 poll timeout");
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_adc_init_configured(const ra8_adc_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* Validate the resolution -> ADPRC translation before touching hardware so
   * a bad descriptor cannot leave the module half-configured. */
  uint32_t adprc = 0U;
  if (!internal_adprc_for_resolution(cfg->resolution, &adprc)) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_adc16h);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "adc_init_cfg: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  internal_clock_bring_up(0U);

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra8_adc_b_admdr()   = internal_build_admdr(cfg->scan_mode);
  *ra8_adc_b_adintcr() = 0U;
  *ra8_adc_b_adsger()  = (uint32_t)(1UL << (uint32_t)k_ra8_adc_default_group);

  internal_zero_channel_table();
  internal_apply_resolution_code(adprc);

  for (uint8_t g = 0U; g < k_ra8_adc_b_scan_groups; ++g) {
    s_adc_state.groups[g].num_channels = 0U;
  }

  s_adc_state.resolution = cfg->resolution;
  s_adc_state.configured = true;
  ra8_log_info(s_tag, "adc_init_configured ready");
  return k_ra8_ok;
}

ra8_err_t ra8_adc_deinit(void)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra8_adc_b_adstopr()    = k_ra8_adstopr_mask_adstop0 | k_ra8_adstopr_mask_adstop1;
  *ra8_adc_b_admdr()      = 0U;
  *ra8_adc_b_adsger()     = 0U;
  *ra8_adc_b_adintcr()    = 0U;
  *ra8_adc_b_adtrgenr()   = 0U;
  *ra8_adc_b_adcmpenr()   = 0U;
  *ra8_adc_b_adcmpintcr() = 0U;
  *ra8_adc_b_adclkenr()   = 0U;
  s_adc_state.fn          = nullptr;
  s_adc_state.ctx         = nullptr;
  s_adc_state.configured  = false;
  for (uint8_t g = 0U; g < k_ra8_adc_b_scan_groups; ++g) {
    s_adc_state.groups[g].num_channels = 0U;
  }
  return ra8_mstp_disable(k_ra8_mstp_adc16h);
}

ra8_err_t ra8_adc_set_resolution(ra8_adc_resolution_t resolution)
{
  uint32_t adprc = 0U;
  if (!internal_adprc_for_resolution(resolution, &adprc)) {
    return k_ra8_err_invalid_arg;
  }
  s_adc_state.resolution = resolution;
  /* Resolution is the per-channel ADDOPCRC.ADPRC data-format, not an ADMDR
   * mode: install it on every result-producing virtual channel so the next
   * conversion reports in the requested precision. */
  internal_apply_resolution_code(adprc);
  return k_ra8_ok;
}

ra8_err_t ra8_adc_get_status(uint16_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  const uint32_t adsr_now = *ra8_adc_b_adsr();
  uint16_t       out      = 0U;
  if ((adsr_now & k_ra8_adsr_mask_adact) != 0U) {
    out |= k_ra8_adc_status_busy;
  }
  if (*ra8_adc_b_adintcr() != 0U) {
    out |= k_ra8_adc_status_ie;
  }
  *out_mask = out;
  return k_ra8_ok;
}

ra8_err_t ra8_adc_clear_status(void)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* ADSR.ADACT0/1 are read-only on silicon -- to clear "busy" you
   * write the matching ADSTOP bit in ADSTOPR. The host mock window
   * is anonymous RAM, so we also write 0 to ADSR so the test can
   * observe the busy bit drop after this call. */
  *ra8_adc_b_adstopr() = k_ra8_adstopr_mask_adstop0 | k_ra8_adstopr_mask_adstop1;
  *ra8_adc_b_adsr()    = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_adc_attach_handler(ra8_adc_complete_fn_t fn, void* ctx)
{
  s_adc_state.fn  = fn;
  s_adc_state.ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_adc_enter_stop(void)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra8_adc_b_admdr() = 0U;
  return ra8_mstp_disable(k_ra8_mstp_adc16h);
}

ra8_err_t ra8_adc_exit_stop(void)
{
  return ra8_mstp_enable(k_ra8_mstp_adc16h);
}

RA8_ISR_SAFE
void ra8_adc_dispatch_cnv_end(uint8_t channel)
{
  volatile uint32_t* addr = ra8_adc_b_addr(channel);
  if (addr == nullptr) {
    return;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  const uint16_t              result = (uint16_t)(*addr & k_ra8_addr_mask_data);
  const ra8_adc_complete_fn_t fn     = s_adc_state.fn;
  void* const                 ctx    = s_adc_state.ctx;
  if (fn != nullptr) {
    fn(ctx, result);
  }
}

/* =============================================================================
 * Scan-group / trigger / window-comparator / oversampling
 * ----------------------------------------------------------------------------
 * The following functions bring the ADC_B driver to FSP r_adc_b parity for
 * scan-group orchestration. Implementation mirrors
 *   /opt/fsp/ra/fsp/src/r_adc_b/r_adc_b.c::R_ADC_B_ScanCfg
 *   /opt/fsp/ra/fsp/src/r_adc_b/r_adc_b.c::R_ADC_B_ScanStart
 *   /opt/fsp/ra/fsp/src/r_adc_b/r_adc_b.c::R_ADC_B_ScanStop
 *   /opt/fsp/ra/fsp/src/r_adc_b/r_adc_b.c::R_ADC_B_Read
 *   /opt/fsp/ra/fsp/src/r_adc_b/r_adc_b.c::R_ADC_B_WindowCfg
 * =============================================================================
 */

/**
 * @brief Validate a scan-group configuration descriptor.
 *
 * @param[in] group Scan-group index.
 * @param[in] cfg   Non-NULL configuration descriptor.
 * @return ``k_ra8_ok`` on pass, otherwise an error code.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra8_err_t internal_validate_group_cfg(uint8_t group, const ra8_adc_scan_group_cfg_t* cfg)
{
  if (group >= k_ra8_adc_b_scan_groups) {
    return k_ra8_err_out_of_range;
  }
  if ((cfg->num_channels == 0U) || (cfg->num_channels > k_ra8_adc_scan_group_max_channels)) {
    return k_ra8_err_out_of_range;
  }
  for (uint8_t i = 0U; i < cfg->num_channels; ++i) {
    if (cfg->channels[i] >= k_ra8_adc_b_max_channels) {
      return k_ra8_err_out_of_range;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Translate the public oversampling enum into ADDOPCRB AVEMD/ADC.
 *
 * @param[in]  mode      Public oversampling selector.
 * @param[out] out_avemd AVEMD field value (00 = off, 10 = average).
 * @param[out] out_adc   ADC averaging-times code.
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_invalid_arg`` for unknown modes.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra8_err_t
internal_oversample_encode(ra8_adc_oversample_t mode, uint32_t* out_avemd, uint32_t* out_adc)
{
  switch (mode) {
    case k_ra8_adc_oversample_off:
      *out_avemd = (uint32_t)k_ra8_avemd_off;
      *out_adc   = (uint32_t)k_ra8_adc_avg_1x;
      return k_ra8_ok;
    case k_ra8_adc_oversample_4x:
      *out_avemd = (uint32_t)k_ra8_avemd_average;
      *out_adc   = (uint32_t)k_ra8_adc_avg_4x;
      return k_ra8_ok;
    case k_ra8_adc_oversample_16x:
      *out_avemd = (uint32_t)k_ra8_avemd_average;
      *out_adc   = (uint32_t)k_ra8_adc_avg_16x;
      return k_ra8_ok;
    case k_ra8_adc_oversample_64x:
      *out_avemd = (uint32_t)k_ra8_avemd_average;
      *out_adc   = (uint32_t)k_ra8_adc_avg_64x;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Walk the channel list and program ADCHCR[ch] = (SGSEL, CNVCS).
 *
 * @param[in] group Scan-group index (already validated).
 * @param[in] cfg   Validated configuration descriptor.
 * @return ``k_ra8_ok`` or ``k_ra8_err_out_of_range`` if any ADCHCR slot is
 *         unmapped (defensive -- channels were already range-checked).
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra8_err_t internal_program_group_channels(uint8_t group, const ra8_adc_scan_group_cfg_t* cfg)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint8_t i = 0U; i < cfg->num_channels; ++i) {
    const uint8_t      ch  = cfg->channels[i];
    volatile uint32_t* reg = ra8_adc_b_adchcr(ch);
    if (reg == nullptr) {
      return k_ra8_err_out_of_range;
    }
    const uint32_t cnvcs =
      ((uint32_t)ch << (uint32_t)k_ra8_adchcr_bit_cnvcs) & k_ra8_adchcr_mask_cnvcs;
    const uint32_t sgsel =
      ((uint32_t)group << (uint32_t)k_ra8_adchcr_bit_sgsel) & k_ra8_adchcr_mask_sgsel;
    *reg = (cnvcs | sgsel);
  }
  return k_ra8_ok;
}

/**
 * @brief Cache a group's channel list into ``s_adc_state.groups``.
 *
 * @details See implementation.
 * @param[in] group See implementation.
 * @param[in] cfg See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_cache_group_channels(uint8_t group, const ra8_adc_scan_group_cfg_t* cfg)
{
  s_adc_state.groups[group].num_channels = cfg->num_channels;
  for (uint8_t i = 0U; i < cfg->num_channels; ++i) {
    s_adc_state.groups[group].channels[i] = cfg->channels[i];
  }
}

/**
 * @brief Apply the per-group enable bit (ADSGER) and trigger source (ADTRGENR).
 *
 * @details See implementation.
 * @param[in] group See implementation.
 * @param[in] trigger See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_apply_group_enable(uint8_t group, ra8_adc_trigger_src_t trigger)
{
  const uint32_t group_bit = (uint32_t)(1UL << (uint32_t)group);
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra8_adc_b_adsger() |= group_bit;
  if (trigger == k_ra8_adc_trig_src_software) {
    *ra8_adc_b_adtrgenr() &= ~group_bit;
  } else {
    *ra8_adc_b_adtrgenr() |= group_bit;
  }
}

ra8_err_t ra8_adc_configure_scan_group(uint8_t group, const ra8_adc_scan_group_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  const ra8_err_t v_err = internal_validate_group_cfg(group, cfg);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "configure_scan_group: validation"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t prog_err = internal_program_group_channels(group, cfg);
  RA8_RETURN_ON_ERROR(prog_err,
                      s_tag,
                      "configure_scan_group: program channels"); /* GCOVR_EXCL_BR_LINE */

  internal_cache_group_channels(group, cfg);
  internal_apply_group_enable(group, cfg->trigger);

  (void)cfg->priority; /* Cached for future scan_group_priority register. */
  return k_ra8_ok;
}

ra8_err_t ra8_adc_start_group(uint8_t group)
{
  volatile uint32_t* adstr = ra8_adc_b_adstr(group);
  if (adstr == nullptr) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *adstr = k_ra8_adstr_mask_adst;
  return k_ra8_ok;
}

ra8_err_t ra8_adc_stop_group(uint8_t group)
{
  volatile uint32_t* adstr = ra8_adc_b_adstr(group);
  if (adstr == nullptr) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *adstr               = 0U;
  *ra8_adc_b_adstopr() = k_ra8_adstopr_mask_adstop0 | k_ra8_adstopr_mask_adstop1;
  return k_ra8_ok;
}

ra8_err_t ra8_adc_read_group_results(uint8_t group, uint16_t* out_buf, uint8_t* out_count)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "out_buf must not be nullptr");
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  *out_count = 0U;

  if (group >= k_ra8_adc_b_scan_groups) {
    return k_ra8_err_out_of_range;
  }
  const ra8_adc_group_cache_t* cache = &s_adc_state.groups[group];
  if (cache->num_channels == 0U) {
    return k_ra8_err_out_of_range;
  }

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint8_t i = 0U; i < cache->num_channels; ++i) {
    volatile uint32_t* addr = ra8_adc_b_addr(cache->channels[i]);
    if (addr == nullptr) {
      return k_ra8_err_out_of_range;
    }
    out_buf[i] = (uint16_t)(*addr & k_ra8_addr_mask_data);
  }
  *out_count = cache->num_channels;
  return k_ra8_ok;
}

ra8_err_t ra8_adc_set_continuous_scan(uint8_t group, bool enable)
{
  if (group >= k_ra8_adc_b_scan_groups) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* ADMDR.ADMD0 holds the unit-level mode; we map continuous-scan to
   * the matching code. The group argument is held for forward
   * compatibility with a per-group mode register. */
  const uint32_t mdr_now = *ra8_adc_b_admdr() & ~k_ra8_admdr_mask_admd0;
  const uint32_t code =
    enable ? (uint32_t)k_ra8_admdr_mode_continuous : (uint32_t)k_ra8_admdr_mode_one_cycle;
  const uint32_t admd0 = (code << (uint32_t)k_ra8_admdr_bit_admd0) & k_ra8_admdr_mask_admd0;
  *ra8_adc_b_admdr()   = mdr_now | admd0;
  return k_ra8_ok;
}

ra8_err_t ra8_adc_set_compare_window(uint8_t channel, uint16_t low, uint16_t high)
{
  if (channel >= k_ra8_adc_b_cmp_tables) {
    return k_ra8_err_out_of_range;
  }
  if (high < low) {
    return k_ra8_err_invalid_arg;
  }
  volatile uint32_t* tbl = ra8_adc_b_adcmptbr(channel);
  if (tbl == nullptr) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Pack (low,high) into a single 32-bit ADCMPTBRn slot. */
  *tbl = ((uint32_t)low << (uint32_t)k_ra8_adcmptbr_bit_low) |
         ((uint32_t)high << (uint32_t)k_ra8_adcmptbr_bit_high);

  /* CMPMDn -> "inside-window" mode. ADCMPMDR0 carries CMPMD0..3,
   * ADCMPMDR1 carries CMPMD4..7. Each field is 2 bits at byte stride. */
  const uint8_t      which = (uint8_t)((channel < k_ra8_adcmpmd_per_reg) ? 0U : 1U);
  volatile uint32_t* mdr   = ra8_adc_b_adcmpmdr(which);
  if (mdr != nullptr) {
    const uint8_t  local_idx = (uint8_t)(channel % k_ra8_adcmpmd_per_reg);
    const uint8_t  shift     = (uint8_t)(local_idx * k_ra8_adcmpmd_field_step);
    const uint32_t mask      = ((uint32_t)0x3UL) << shift;
    const uint32_t code      = ((uint32_t)k_ra8_adcmpmd_inside) << shift;
    *mdr                     = (*mdr & ~mask) | code;
  }

  /* ADCMPENR.CMPENn is the main enable for table N. */
  const uint32_t enable_bit = (uint32_t)(1UL << channel);
  *ra8_adc_b_adcmpenr() |= enable_bit;

  /* Per-channel CMPTBLEm in ADDOPCRB enables table N for this channel. */
  volatile uint32_t* opcrb = ra8_adc_b_addopcrb(channel);
  if (opcrb != nullptr) {
    const uint32_t cmptble_bit =
      ((uint32_t)(1UL << channel) << (uint32_t)k_ra8_addopcrb_bit_cmptblem) &
      k_ra8_addopcrb_mask_cmptblem;
    *opcrb =
      (*opcrb & ~k_ra8_addopcrb_mask_cmptblem) | (cmptble_bit & k_ra8_addopcrb_mask_cmptblem);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_adc_set_oversampling(uint8_t channel, ra8_adc_oversample_t mode)
{
  uint32_t        avemd   = 0U;
  uint32_t        adc     = 0U;
  const ra8_err_t enc_err = internal_oversample_encode(mode, &avemd, &adc);
  RA8_RETURN_ON_ERROR(enc_err, s_tag, "set_oversampling: invalid mode"); /* GCOVR_EXCL_BR_LINE */

  volatile uint32_t* opcrb = ra8_adc_b_addopcrb(channel);
  if (opcrb == nullptr) {
    return k_ra8_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  const uint32_t avemd_field =
    (avemd << (uint32_t)k_ra8_addopcrb_bit_avemd) & k_ra8_addopcrb_mask_avemd;
  const uint32_t adc_field = (adc << (uint32_t)k_ra8_addopcrb_bit_adc) & k_ra8_addopcrb_mask_adc;
  const uint32_t cleared   = *opcrb & ~(k_ra8_addopcrb_mask_avemd | k_ra8_addopcrb_mask_adc);
  *opcrb                   = cleared | avemd_field | adc_field;
  return k_ra8_ok;
}
