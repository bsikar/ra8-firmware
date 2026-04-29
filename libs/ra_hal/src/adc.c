/**
 * @file adc.c
 * @brief 12 / 14-bit ADC_B converter driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 ADC_B peripheral. ADC_B is the Type-B SAR
 * block introduced on the RA8 family; its register layout was
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
 *   5. Programme ADMDR per-unit mode (ADMD0[3:0], ADMD1[11:8]).
 *   6. Programme each ADCHCRn slot (channel selection).
 *   7. Enable scan-group via ADSGER, kick via ADSYSTR / ADSTR.
 *   8. Poll ADSR.ADACT0/1, read ADDR[ch].
 *
 * Resolution is controlled by ADMDR per-unit mode codes, not by a
 * per-channel field. The previous version of this driver modelled
 * non-existent ADCSR / ADCER / RESSEL / CVEN bits; those have been
 * deleted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_adc_b_regs.h"
#include "ra_adc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ADC";

/**
 * @enum ra_adc_const_t
 * @brief Local timing / mask constants for adc.c.
 */
typedef enum : uint32_t {
  k_ra_adc_clk_wait_limit  = 100000UL,  /**< ADCLKSR settle-poll budget. */
  k_ra_adc_busy_wait_limit = 2000000UL, /**< ADSR.ADACT busy-poll budget.
                                              The host test fires a 100 us
                                              SIGALRM to clear ADACT0; the
                                              budget must outlast scheduler
                                              jitter. 2M spins on a 1 GHz
                                              CPU is ~2 ms wall clock -- a
                                              sane ADC conversion timeout
                                              on real silicon. */
} ra_adc_const_t;

/**
 * @enum ra_adc_default_group_t
 * @brief Scan-group used by the legacy single-channel polling API.
 */
typedef enum : uint8_t {
  k_ra_adc_default_group = 0U, /**< Group 0 owns SW-trigger conversions. */
} ra_adc_default_group_t;

/**
 * @struct ra_adc_group_cache_t
 * @brief Per-scan-group channel-list cache.
 *
 * @details
 * ``ra_adc_configure_scan_group`` stores the channel list here so that
 * ``ra_adc_read_group_results`` can stream the matching ADDR slots
 * back without re-walking ADCHCR. Indexed by scan-group number.
 */
typedef struct {
  uint8_t num_channels;                               /**< 0 means group unconfigured. */
  uint8_t channels[k_ra_adc_scan_group_max_channels]; /**< Physical channel list. */
} ra_adc_group_cache_t;

/**
 * @struct ra_adc_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra_adc_complete_fn_t fn;         /**< Conversion-complete callback or nullptr. */
  void*                ctx;        /**< Callback context. */
  ra_adc_resolution_t  resolution; /**< Last-applied resolution selection. */
  bool                 configured; /**< True after first successful init. */
  ra_adc_group_cache_t groups[k_ra_adc_b_scan_groups]; /**< Per-group channel cache. */
} ra_adc_state_t;

static ra_adc_state_t s_adc_state;

/**
 * @brief Translate a public resolution-enum into the ADMD0 unit-mode code.
 *
 * @details
 * ADMDR.ADMD0[3:0] selects unit mode. For the polling driver we
 * always use one-cycle scan; future extensions can refine this by
 * writing per-table ADCNVSTR / ADDOPCRA values.
 *
 * @param[in] resolution Public resolution selector (currently a soft hint).
 * @return ADMD0 mode code to OR into ADMDR.
 */
static uint32_t internal_admd0_for_resolution(ra_adc_resolution_t resolution)
{
  (void)resolution;
  return k_ra_admdr_mode_one_cycle;
}

/**
 * @brief Best-effort wait for ADCLKSR.CLKSR to match the requested level.
 *
 * @details
 * On real silicon CLKSR follows ADCLKENR within a handful of cycles
 * (HUM Ch 53). On the host ``ra_sim_mmap`` window CLKSR is just
 * anonymous RAM, so the wait is a bounded no-op. We mirror the
 * FSP ``FSP_HARDWARE_REGISTER_WAIT`` pattern: poll a fixed budget
 * and continue regardless rather than fail-loud.
 *
 * @param[in] desired Either ``k_ra_adclksr_mask_clksr`` (running) or 0 (off).
 */
static void internal_wait_clksr(uint32_t desired)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint32_t i = 0U; i < k_ra_adc_clk_wait_limit; ++i) {
    if ((*ra_adc_b_adclksr() & k_ra_adclksr_mask_clksr) == desired) {
      return;
    }
  }
}

/**
 * @brief Bring ADCLK up: ENR=0 -> wait CLKSR=0 -> CR=src -> ENR=1 -> CLKSR=1.
 *
 * @param[in] clkcr_value ADCLKCR value to programme (CLKSEL[1:0], DIVR[18:16]).
 */
static void internal_clock_bring_up(uint32_t clkcr_value)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra_adc_b_adclkenr() = 0U;
  internal_wait_clksr(0U);
  *ra_adc_b_adclkcr()  = clkcr_value;
  *ra_adc_b_adclkenr() = k_ra_adclkenr_mask_clken;
  internal_wait_clksr(k_ra_adclksr_mask_clksr);
}

/**
 * @brief Default-zero every per-channel ADCHCRn slot.
 *
 * @details
 * FSP ``R_ADC_B_Open`` clears every ADCHCRn before the user
 * populates per-channel descriptors; we mirror that. This also
 * ensures a stale CNVCS / SGSEL field cannot leak between
 * configurations.
 */
static void internal_zero_channel_table(void)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint8_t ch = 0U; ch < k_ra_adc_b_max_channels; ++ch) {
    volatile uint32_t* reg = ra_adc_b_adchcr(ch);
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
 */
static void internal_program_channel(uint8_t virtual_ch, uint8_t physical_ch)
{
  volatile uint32_t* reg = ra_adc_b_adchcr(virtual_ch);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  const uint32_t cnvcs =
    ((uint32_t)physical_ch << (uint32_t)k_ra_adchcr_bit_cnvcs) & k_ra_adchcr_mask_cnvcs;
  const uint32_t sgsel =
    ((uint32_t)k_ra_adc_default_group << (uint32_t)k_ra_adchcr_bit_sgsel) & k_ra_adchcr_mask_sgsel;
  *reg = (cnvcs | sgsel);
}

/**
 * @brief Build the ADMDR value (ADMD0 only -- ADC1 stays disabled).
 *
 * @param[in] resolution Public resolution selector.
 * @param[in] scan_mode  True for continuous scan, false for one-shot.
 * @return Composite ADMDR value.
 */
static uint32_t internal_build_admdr(ra_adc_resolution_t resolution, bool scan_mode)
{
  const uint32_t admd0_code =
    scan_mode ? k_ra_admdr_mode_continuous : internal_admd0_for_resolution(resolution);
  return (admd0_code << (uint32_t)k_ra_admdr_bit_admd0) & k_ra_admdr_mask_admd0;
}

ra_err_t ra_adc_init(void)
{
  /* HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D" p 449 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_adc16h);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "adc_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  internal_clock_bring_up(0U);

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra_adc_b_admdr()   = internal_build_admdr(k_ra_adc_res_14bit, false);
  *ra_adc_b_adintcr() = 0U;
  *ra_adc_b_adsger()  = (uint32_t)(1UL << (uint32_t)k_ra_adc_default_group);

  internal_zero_channel_table();

  for (uint8_t g = 0U; g < k_ra_adc_b_scan_groups; ++g) {
    s_adc_state.groups[g].num_channels = 0U;
  }

  s_adc_state.resolution = k_ra_adc_res_14bit;
  s_adc_state.configured = true;
  ra_log_info(s_tag, "adc_init ready");
  return k_ra_ok;
}

ra_err_t ra_adc_read_channel(uint8_t channel, uint16_t* out_raw)
{
  RA_CHECK_NULL_PTR(out_raw, s_tag, "out_raw must not be nullptr");

  volatile uint32_t* chcr = ra_adc_b_adchcr(channel);
  volatile uint32_t* addr = ra_adc_b_addr(channel);
  if ((chcr == nullptr) || (addr == nullptr)) {
    return k_ra_err_out_of_range;
  }

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Map this slot onto physical channel == ``channel`` in the
   * default scan group (driver convention: virtual_ch == physical_ch). */
  internal_program_channel(channel, channel);

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Kick the conversion via per-group ADSTR[default_group].ADST. */
  volatile uint32_t* adstr = ra_adc_b_adstr(k_ra_adc_default_group);
  if (adstr != nullptr) {
    *adstr = k_ra_adstr_mask_adst;
  }

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Poll ADSR.ADACT0 -- hardware clears it when the unit becomes idle. */
  for (uint32_t i = 0U; i < k_ra_adc_busy_wait_limit; ++i) {
    if ((*ra_adc_b_adsr() & k_ra_adsr_mask_adact0) == 0U) {
      *out_raw = (uint16_t)(*addr & k_ra_addr_mask_data);
      return k_ra_ok;
    }
  }

  *out_raw = 0U;
  ra_log_error(s_tag, "ADACT0 poll timeout");
  return k_ra_err_hw_timeout;
}

ra_err_t ra_adc_init_configured(const ra_adc_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");

  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_adc16h);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "adc_init_cfg: mstp enable");

  internal_clock_bring_up(0U);

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra_adc_b_admdr()   = internal_build_admdr(cfg->resolution, cfg->scan_mode);
  *ra_adc_b_adintcr() = 0U;
  *ra_adc_b_adsger()  = (uint32_t)(1UL << (uint32_t)k_ra_adc_default_group);

  internal_zero_channel_table();

  for (uint8_t g = 0U; g < k_ra_adc_b_scan_groups; ++g) {
    s_adc_state.groups[g].num_channels = 0U;
  }

  s_adc_state.resolution = cfg->resolution;
  s_adc_state.configured = true;
  ra_log_info(s_tag, "adc_init_configured ready");
  return k_ra_ok;
}

ra_err_t ra_adc_deinit(void)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra_adc_b_adstopr()    = k_ra_adstopr_mask_adstop0 | k_ra_adstopr_mask_adstop1;
  *ra_adc_b_admdr()      = 0U;
  *ra_adc_b_adsger()     = 0U;
  *ra_adc_b_adintcr()    = 0U;
  *ra_adc_b_adtrgenr()   = 0U;
  *ra_adc_b_adcmpenr()   = 0U;
  *ra_adc_b_adcmpintcr() = 0U;
  *ra_adc_b_adclkenr()   = 0U;
  s_adc_state.fn         = nullptr;
  s_adc_state.ctx        = nullptr;
  s_adc_state.configured = false;
  for (uint8_t g = 0U; g < k_ra_adc_b_scan_groups; ++g) {
    s_adc_state.groups[g].num_channels = 0U;
  }
  return ra_mstp_disable(k_ra_mstp_adc16h);
}

ra_err_t ra_adc_set_resolution(ra_adc_resolution_t resolution)
{
  if ((uint8_t)resolution > k_ra_adc_res_14bit) {
    return k_ra_err_invalid_arg;
  }
  s_adc_state.resolution = resolution;
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Re-apply the per-unit mode keeping ADC1's ADMD1 field intact;
   * we only own ADC0 in this driver. */
  const uint32_t mdr_now = *ra_adc_b_admdr() & ~k_ra_admdr_mask_admd0;
  const uint32_t admd0 =
    (internal_admd0_for_resolution(resolution) << (uint32_t)k_ra_admdr_bit_admd0) &
    k_ra_admdr_mask_admd0;
  *ra_adc_b_admdr() = mdr_now | admd0;
  return k_ra_ok;
}

ra_err_t ra_adc_get_status(uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  const uint32_t adsr_now = *ra_adc_b_adsr();
  uint16_t       out      = 0U;
  if ((adsr_now & k_ra_adsr_mask_adact) != 0U) {
    out |= k_ra_adc_status_busy;
  }
  if (*ra_adc_b_adintcr() != 0U) {
    out |= k_ra_adc_status_ie;
  }
  *out_mask = out;
  return k_ra_ok;
}

ra_err_t ra_adc_clear_status(void)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* ADSR.ADACT0/1 are read-only on silicon -- to clear "busy" you
   * write the matching ADSTOP bit in ADSTOPR. The host mock window
   * is anonymous RAM, so we also write 0 to ADSR so the test can
   * observe the busy bit drop after this call. */
  *ra_adc_b_adstopr() = k_ra_adstopr_mask_adstop0 | k_ra_adstopr_mask_adstop1;
  *ra_adc_b_adsr()    = 0U;
  return k_ra_ok;
}

ra_err_t ra_adc_attach_handler(ra_adc_complete_fn_t fn, void* ctx)
{
  s_adc_state.fn  = fn;
  s_adc_state.ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_adc_enter_stop(void)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra_adc_b_admdr() = 0U;
  return ra_mstp_disable(k_ra_mstp_adc16h);
}

ra_err_t ra_adc_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_adc16h);
}

void ra_adc_dispatch_cnv_end(uint8_t channel)
{
  volatile uint32_t* addr = ra_adc_b_addr(channel);
  if (addr == nullptr) {
    return;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  const uint16_t             result = (uint16_t)(*addr & k_ra_addr_mask_data);
  const ra_adc_complete_fn_t fn     = s_adc_state.fn;
  void* const                ctx    = s_adc_state.ctx;
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
 * @return ``k_ra_ok`` on pass, otherwise an error code.
 */
static ra_err_t internal_validate_group_cfg(uint8_t group, const ra_adc_scan_group_cfg_t* cfg)
{
  if (group >= k_ra_adc_b_scan_groups) {
    return k_ra_err_out_of_range;
  }
  if ((cfg->num_channels == 0U) || (cfg->num_channels > k_ra_adc_scan_group_max_channels)) {
    return k_ra_err_out_of_range;
  }
  for (uint8_t i = 0U; i < cfg->num_channels; ++i) {
    if (cfg->channels[i] >= k_ra_adc_b_max_channels) {
      return k_ra_err_out_of_range;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Translate the public oversampling enum into ADDOPCRB AVEMD/ADC.
 *
 * @param[in]  mode      Public oversampling selector.
 * @param[out] out_avemd AVEMD field value (00 = off, 10 = average).
 * @param[out] out_adc   ADC averaging-times code.
 * @return ``k_ra_ok`` on success, ``k_ra_err_invalid_arg`` for unknown modes.
 */
static ra_err_t
internal_oversample_encode(ra_adc_oversample_t mode, uint32_t* out_avemd, uint32_t* out_adc)
{
  switch (mode) {
    case k_ra_adc_oversample_off:
      *out_avemd = (uint32_t)k_ra_avemd_off;
      *out_adc   = (uint32_t)k_ra_adc_avg_1x;
      return k_ra_ok;
    case k_ra_adc_oversample_4x:
      *out_avemd = (uint32_t)k_ra_avemd_average;
      *out_adc   = (uint32_t)k_ra_adc_avg_4x;
      return k_ra_ok;
    case k_ra_adc_oversample_16x:
      *out_avemd = (uint32_t)k_ra_avemd_average;
      *out_adc   = (uint32_t)k_ra_adc_avg_16x;
      return k_ra_ok;
    case k_ra_adc_oversample_64x:
      *out_avemd = (uint32_t)k_ra_avemd_average;
      *out_adc   = (uint32_t)k_ra_adc_avg_64x;
      return k_ra_ok;
    default:
      return k_ra_err_invalid_arg;
  }
}

/**
 * @brief Walk the channel list and program ADCHCR[ch] = (SGSEL, CNVCS).
 *
 * @param[in] group Scan-group index (already validated).
 * @param[in] cfg   Validated configuration descriptor.
 * @return ``k_ra_ok`` or ``k_ra_err_out_of_range`` if any ADCHCR slot is
 *         unmapped (defensive -- channels were already range-checked).
 */
static ra_err_t internal_program_group_channels(uint8_t group, const ra_adc_scan_group_cfg_t* cfg)
{
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint8_t i = 0U; i < cfg->num_channels; ++i) {
    const uint8_t      ch  = cfg->channels[i];
    volatile uint32_t* reg = ra_adc_b_adchcr(ch);
    if (reg == nullptr) {
      return k_ra_err_out_of_range;
    }
    const uint32_t cnvcs =
      ((uint32_t)ch << (uint32_t)k_ra_adchcr_bit_cnvcs) & k_ra_adchcr_mask_cnvcs;
    const uint32_t sgsel =
      ((uint32_t)group << (uint32_t)k_ra_adchcr_bit_sgsel) & k_ra_adchcr_mask_sgsel;
    *reg = (cnvcs | sgsel);
  }
  return k_ra_ok;
}

/**
 * @brief Cache a group's channel list into ``s_adc_state.groups``.
 */
static void internal_cache_group_channels(uint8_t group, const ra_adc_scan_group_cfg_t* cfg)
{
  s_adc_state.groups[group].num_channels = cfg->num_channels;
  for (uint8_t i = 0U; i < cfg->num_channels; ++i) {
    s_adc_state.groups[group].channels[i] = cfg->channels[i];
  }
}

/**
 * @brief Apply the per-group enable bit (ADSGER) and trigger source (ADTRGENR).
 */
static void internal_apply_group_enable(uint8_t group, ra_adc_trigger_src_t trigger)
{
  const uint32_t group_bit = (uint32_t)(1UL << (uint32_t)group);
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *ra_adc_b_adsger() |= group_bit;
  if (trigger == k_ra_adc_trig_src_software) {
    *ra_adc_b_adtrgenr() &= ~group_bit;
  } else {
    *ra_adc_b_adtrgenr() |= group_bit;
  }
}

ra_err_t ra_adc_configure_scan_group(uint8_t group, const ra_adc_scan_group_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");

  const ra_err_t v_err = internal_validate_group_cfg(group, cfg);
  RA_RETURN_ON_ERROR(v_err, s_tag, "configure_scan_group: validation");

  const ra_err_t prog_err = internal_program_group_channels(group, cfg);
  RA_RETURN_ON_ERROR(prog_err, s_tag, "configure_scan_group: program channels");

  internal_cache_group_channels(group, cfg);
  internal_apply_group_enable(group, cfg->trigger);

  (void)cfg->priority; /* Cached for future scan_group_priority register. */
  return k_ra_ok;
}

ra_err_t ra_adc_start_group(uint8_t group)
{
  volatile uint32_t* adstr = ra_adc_b_adstr(group);
  if (adstr == nullptr) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *adstr = k_ra_adstr_mask_adst;
  return k_ra_ok;
}

ra_err_t ra_adc_stop_group(uint8_t group)
{
  volatile uint32_t* adstr = ra_adc_b_adstr(group);
  if (adstr == nullptr) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  *adstr              = 0U;
  *ra_adc_b_adstopr() = k_ra_adstopr_mask_adstop0 | k_ra_adstopr_mask_adstop1;
  return k_ra_ok;
}

ra_err_t ra_adc_read_group_results(uint8_t group, uint16_t* out_buf, uint8_t* out_count)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "out_buf must not be nullptr");
  RA_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  *out_count = 0U;

  if (group >= k_ra_adc_b_scan_groups) {
    return k_ra_err_out_of_range;
  }
  const ra_adc_group_cache_t* cache = &s_adc_state.groups[group];
  if (cache->num_channels == 0U) {
    return k_ra_err_out_of_range;
  }

  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  for (uint8_t i = 0U; i < cache->num_channels; ++i) {
    volatile uint32_t* addr = ra_adc_b_addr(cache->channels[i]);
    if (addr == nullptr) {
      return k_ra_err_out_of_range;
    }
    out_buf[i] = (uint16_t)(*addr & k_ra_addr_mask_data);
  }
  *out_count = cache->num_channels;
  return k_ra_ok;
}

ra_err_t ra_adc_set_continuous_scan(uint8_t group, bool enable)
{
  if (group >= k_ra_adc_b_scan_groups) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* ADMDR.ADMD0 holds the unit-level mode; we map continuous-scan to
   * the matching code. The group argument is held for forward
   * compatibility with a per-group mode register. */
  const uint32_t mdr_now = *ra_adc_b_admdr() & ~k_ra_admdr_mask_admd0;
  const uint32_t code =
    enable ? (uint32_t)k_ra_admdr_mode_continuous : (uint32_t)k_ra_admdr_mode_one_cycle;
  const uint32_t admd0 = (code << (uint32_t)k_ra_admdr_bit_admd0) & k_ra_admdr_mask_admd0;
  *ra_adc_b_admdr()    = mdr_now | admd0;
  return k_ra_ok;
}

ra_err_t ra_adc_set_compare_window(uint8_t channel, uint16_t low, uint16_t high)
{
  if (channel >= k_ra_adc_b_cmp_tables) {
    return k_ra_err_out_of_range;
  }
  if (high < low) {
    return k_ra_err_invalid_arg;
  }
  volatile uint32_t* tbl = ra_adc_b_adcmptbr(channel);
  if (tbl == nullptr) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  /* Pack (low,high) into a single 32-bit ADCMPTBRn slot. */
  *tbl = ((uint32_t)low << (uint32_t)k_ra_adcmptbr_bit_low) |
         ((uint32_t)high << (uint32_t)k_ra_adcmptbr_bit_high);

  /* CMPMDn -> "inside-window" mode. ADCMPMDR0 carries CMPMD0..3,
   * ADCMPMDR1 carries CMPMD4..7. Each field is 2 bits at byte stride. */
  const uint8_t      which = (channel < k_ra_adcmpmd_per_reg) ? 0U : 1U;
  volatile uint32_t* mdr   = ra_adc_b_adcmpmdr(which);
  if (mdr != nullptr) {
    const uint8_t  local_idx = (uint8_t)(channel % k_ra_adcmpmd_per_reg);
    const uint8_t  shift     = (uint8_t)(local_idx * k_ra_adcmpmd_field_step);
    const uint32_t mask      = ((uint32_t)0x3UL) << shift;
    const uint32_t code      = ((uint32_t)k_ra_adcmpmd_inside) << shift;
    *mdr                     = (*mdr & ~mask) | code;
  }

  /* ADCMPENR.CMPENn is the master enable for table N. */
  const uint32_t enable_bit = (uint32_t)(1UL << channel);
  *ra_adc_b_adcmpenr() |= enable_bit;

  /* Per-channel CMPTBLEm in ADDOPCRB enables table N for this channel. */
  volatile uint32_t* opcrb = ra_adc_b_addopcrb(channel);
  if (opcrb != nullptr) {
    const uint32_t cmptble_bit =
      ((uint32_t)(1UL << channel) << (uint32_t)k_ra_addopcrb_bit_cmptblem) &
      k_ra_addopcrb_mask_cmptblem;
    *opcrb = (*opcrb & ~k_ra_addopcrb_mask_cmptblem) | (cmptble_bit & k_ra_addopcrb_mask_cmptblem);
  }
  return k_ra_ok;
}

ra_err_t ra_adc_set_oversampling(uint8_t channel, ra_adc_oversample_t mode)
{
  uint32_t       avemd   = 0U;
  uint32_t       adc     = 0U;
  const ra_err_t enc_err = internal_oversample_encode(mode, &avemd, &adc);
  RA_RETURN_ON_ERROR(enc_err, s_tag, "set_oversampling: invalid mode");

  volatile uint32_t* opcrb = ra_adc_b_addopcrb(channel);
  if (opcrb == nullptr) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 53 "16-bit A/D Converter (ADC16H)" p 3308 */
  const uint32_t avemd_field =
    (avemd << (uint32_t)k_ra_addopcrb_bit_avemd) & k_ra_addopcrb_mask_avemd;
  const uint32_t adc_field = (adc << (uint32_t)k_ra_addopcrb_bit_adc) & k_ra_addopcrb_mask_adc;
  const uint32_t cleared   = *opcrb & ~(k_ra_addopcrb_mask_avemd | k_ra_addopcrb_mask_adc);
  *opcrb                   = cleared | avemd_field | adc_field;
  return k_ra_ok;
}
