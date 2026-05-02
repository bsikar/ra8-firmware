/**
 * @file ra_adc.h
 * @brief Full-featured ADC_B driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * full build-out of the RA8D2 ADC_B peripheral (14-bit
 * SAR). Extends the polling stub with: descriptor-based
 * init, deinit, runtime resolution reconfigure, status + clear,
 * interrupt-mode attach / dispatch, power transition.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_adc_resolution_t
 * @brief ADCER.ADPRC resolution selector.
 */
typedef enum : uint8_t {
  k_ra_adc_res_12bit = 0U,
  k_ra_adc_res_10bit = 1U,
  k_ra_adc_res_14bit = 2U,
} ra_adc_resolution_t;

/**
 * @enum ra_adc_trigger_t
 * @brief ADCSR trigger mode.
 */
typedef enum : uint8_t {
  k_ra_adc_trig_software = 0U, /**< Software trigger (write ADST). */
  k_ra_adc_trig_external = 1U, /**< External trigger pin. */
  k_ra_adc_trig_elc      = 2U, /**< ELC event routed trigger. */
} ra_adc_trigger_t;

/**
 * @struct ra_adc_cfg_t
 * @brief Configuration descriptor for ``ra_adc_init_configured``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_adc_init_configured`` in
 * ``libs/ra_hal/src/adc.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_adc_resolution_t resolution;    /**< 12 / 10 / 14 bit. */
  ra_adc_trigger_t    trigger;       /**< Trigger source. */
  bool                right_aligned; /**< True -> right-align. */
  bool                scan_mode;     /**< True -> continuous scan. */
} ra_adc_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_adc_status_mask_t
 * @brief ADCSR status mask.
 */
typedef enum : uint16_t {
  k_ra_adc_status_none = 0x0000U,
  k_ra_adc_status_busy = 0x8000U, /**< ADCSR.ADST (conversion in flight). */
  k_ra_adc_status_ie   = 0x1000U, /**< ADCSR.ADIE. */
} ra_adc_status_mask_t;

/**
 * @typedef ra_adc_complete_fn_t
 * @brief Conversion-complete callback.
 * @param[in] ctx Caller context.
 * @param[in] result Last conversion result (raw ADDRxx).
 */
typedef void (*ra_adc_complete_fn_t)(void* ctx, uint16_t result);

/**
 * @enum ra_adc_trigger_src_t
 * @brief Per-scan-group trigger source.
 *
 * @details
 * Selects which event kicks the scan group:
 *   - software        - call ``ra_adc_start_group``.
 *   - elc / gpt / pin - hardware path; ADTRGENR.STTRGENn must be set
 *                       so the silicon edge-detector arms the group.
 *
 * The selector value is observable via ``ra_adc_b_adtrgenr()`` -- the
 * driver writes a 1 bit at @p group when the source is non-software.
 */
typedef enum : uint8_t {
  k_ra_adc_trig_src_software = 0U, /**< SW kick (default). */
  k_ra_adc_trig_src_elc      = 1U, /**< ELC event mux. */
  k_ra_adc_trig_src_gpt      = 2U, /**< GPT compare-match. */
  k_ra_adc_trig_src_pin      = 3U, /**< Hardware trigger pin. */
} ra_adc_trigger_src_t;

/**
 * @enum ra_adc_priority_t
 * @brief Scan-group priority class (FSP scan_group_priority).
 *
 * @details
 * The RA8D2 ADC_B routes scan groups through a priority arbiter when
 * multiple groups are armed at once; "high" preempts "low". The driver
 * caches the priority but does not currently emit a register write
 * because the priority routing in HUM Ch 53 lives in registers we have
 * not modelled (scan_group_priority + interrupt_routing). The class is
 * stored so future revisions can wire the missing registers without
 * changing the public surface.
 */
typedef enum : uint8_t {
  k_ra_adc_priority_low  = 0U,
  k_ra_adc_priority_high = 1U,
} ra_adc_priority_t;

/**
 * @enum ra_adc_oversample_t
 * @brief Per-channel ADADC.AVEMD/ADC oversampling configuration.
 *
 * @details
 * Maps a logical oversampling rate onto the (AVEMD, ADC) pair in
 * ADDOPCRB[ch]:
 *   - off  -> AVEMD=0
 *   - 4x   -> AVEMD=2 (average), ADC=2 (4 conversions)
 *   - 16x  -> AVEMD=2, ADC=4
 *   - 64x  -> AVEMD=2, ADC=6
 */
typedef enum : uint8_t {
  k_ra_adc_oversample_off = 0U, /**< Single sample, no averaging. */
  k_ra_adc_oversample_4x  = 1U, /**< 4-sample average. */
  k_ra_adc_oversample_16x = 2U, /**< 16-sample average. */
  k_ra_adc_oversample_64x = 3U, /**< 64-sample average. */
} ra_adc_oversample_t;

/**
 * @def k_ra_adc_scan_group_max_channels
 * @brief Maximum channels modelled per scan group.
 */
typedef enum : uint8_t {
  k_ra_adc_scan_group_max_channels = 8U, /**< Per-group channel slot count. */
} ra_adc_scan_group_caps_t;

/**
 * @struct ra_adc_scan_group_cfg_t
 * @brief Configuration for a single scan group.
 *
 * @details
 * Each group enumerates the physical channels it scans plus the
 * trigger source and priority class. ``num_channels`` must be in
 * 1..``k_ra_adc_scan_group_max_channels``.
 *
 * The driver assigns one ADCHCR slot per channel listed (slot index
 * == channel index, matching the read-channel convention used by
 * the legacy polling API).
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t              num_channels;                               /**< 1..8. */
  uint8_t              channels[k_ra_adc_scan_group_max_channels]; /**< Physical-channel list. */
  ra_adc_trigger_src_t trigger;                                    /**< Trigger source. */
  ra_adc_priority_t    priority;                                   /**< Priority class. */
} ra_adc_scan_group_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Legacy init -- 14-bit right-aligned, software trigger.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_init(void);

/**
 * @brief Initialise ADC_B with a full config descriptor.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success ADCSR and ADCER match ``cfg`` and the ADC_B
 * module is powered on via MSTP.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_init_configured(const ra_adc_cfg_t* cfg);

/**
 * @brief Tear down the ADC_B peripheral.
 * @return ``ra_err_t`` error code.
 * @post ADCSR == 0, ADCER == 0, MSTP released.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_deinit(void);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Blocking single-channel sample.
 *
 * @param[in] channel Analog channel index (0..47 on RA8D2 ADC_B).
 * @param[out] out_raw Pointer to receive the 14-bit result right-aligned.
 * @return `ra_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_read_channel(uint8_t channel, uint16_t* out_raw);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the ADC resolution at runtime.
 * @param[in] resolution New resolution selection.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_set_resolution(ra_adc_resolution_t resolution);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the ADC status bits (ADCSR.ADST / ADIE).
 * @param[out] out_mask OR of ``k_ra_adc_status_*``.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_get_status(uint16_t* out_mask);

/**
 * @brief Clear the ADCSR.ADST busy bit (abort in-flight conversion).
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_clear_status(void);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a conversion-complete callback.
 * @param[in] fn Non-NULL callback.
 * @param[in] ctx Context passed to the callback.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_attach_handler(ra_adc_complete_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put ADC_B into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_exit_stop(void);

/* =============================================================================
 * ISR dispatch
 * =============================================================================
 */

/**
 * @brief Dispatch conversion-complete -- read result + fire callback.
 * @param[in] channel Channel the interrupt was reported on.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @return ``ra_err_t`` error code (or void if the signature returns void).
 * @retval k_ra_ok Success path.
 * @retval k_ra_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialised by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra_adc_dispatch_cnv_end(uint8_t channel);

/* =============================================================================
 * Scan groups (FSP r_adc_b parity, Sweep 3 Task 1)
 * =============================================================================
 */

/**
 * @brief Configure scan group @p group with a list of channels + trigger.
 *
 * @details
 * Programs every ADCHCR slot listed in @p cfg with SGSEL == @p group and
 * CNVCS == the matching @c cfg->channels[i] entry, then enables the
 * group via ADSGER. If @p cfg->trigger is non-software, the matching
 * STTRGENn bit is set in ADTRGENR so the silicon edge-detector arms
 * the group on the corresponding ELC / GPT / pin event.
 *
 * @param[in] group Scan-group index (0..8 inclusive on RA8D2).
 * @param[in] cfg   Non-NULL configuration descriptor.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Group programmed.
 * @retval k_ra_err_null_ptr       @p cfg is NULL.
 * @retval k_ra_err_out_of_range   @p group >= 9 or any channel index >= 24
 *                                 or @c num_channels not in 1..8.
 *
 * @pre  ``ra_adc_init_configured`` (or ``ra_adc_init``) has been called.
 * @pre  IRQs masked or ADSTOPR cleared so we are not racing a scan.
 * @post ADSGER bit @p group is set.
 * @post Each listed ADCHCR slot has SGSEL == @p group and CNVCS ==
 *       channels[i].
 *
 * @note Not thread-safe; caller serialises configuration.
 * @see ra_adc_start_group
 * @see ra_adc_stop_group
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_configure_scan_group(uint8_t                        group,
                                                   const ra_adc_scan_group_cfg_t* cfg);

/**
 * @brief Kick a scan group via the per-group ADSTR register.
 *
 * @param[in] group Scan-group index (0..8).
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Group started.
 * @retval k_ra_err_out_of_range @p group >= 9.
 *
 * @pre  Group has been configured via ``ra_adc_configure_scan_group``.
 * @post ADSTR[group].ADST == 1.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_start_group(uint8_t group);

/**
 * @brief Stop a scan group: clear ADSTR[group] and write ADSTOPR.
 *
 * @param[in] group Scan-group index (0..8).
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Group stop requested.
 * @retval k_ra_err_out_of_range @p group >= 9.
 *
 * @post ADSTR[group].ADST == 0.
 * @post ADSTOPR.ADSTOP0|ADSTOP1 written so silicon force-stops both units.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_stop_group(uint8_t group);

/**
 * @brief Drain conversion results for every channel mapped to a group.
 *
 * @details
 * Walks the cached channel list captured by ``ra_adc_configure_scan_group``,
 * reads ADDR[ch].DATA for each, and writes the masked 16-bit value into
 * @p out_buf in order. @p out_count receives the number of channels
 * written; it is also written on the error paths so callers can fail
 * cleanly.
 *
 * @param[in]  group     Scan-group index (0..8).
 * @param[out] out_buf   Result buffer, must hold ``cfg->num_channels`` entries.
 * @param[out] out_count Number of channels read.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Results copied.
 * @retval k_ra_err_null_ptr       @p out_buf or @p out_count is NULL.
 * @retval k_ra_err_out_of_range   @p group >= 9 or group never configured.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_adc_read_group_results(uint8_t group, uint16_t* out_buf, uint8_t* out_count);

/**
 * @brief Switch a scan group between one-shot and continuous (free-running).
 *
 * @details
 * Maps onto ADMDR.ADMD0: code @c k_ra_admdr_mode_continuous when
 * @p enable is true, @c k_ra_admdr_mode_one_cycle otherwise. The
 * @p group argument is currently advisory because ADMDR is per-unit
 * (ADC0/ADC1), not per scan-group; future silicon revisions may add a
 * per-group bit.
 *
 * @param[in] group  Scan-group index (0..8).
 * @param[in] enable True for continuous scan, false for one-shot.
 * @return ``ra_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_set_continuous_scan(uint8_t group, bool enable);

/* =============================================================================
 * Window comparator (FSP r_adc_b parity)
 * =============================================================================
 */

/**
 * @brief Programme a window comparator for a single channel.
 *
 * @details
 * Maps channel @p channel onto compare table @p channel (1:1 within the
 * 0..7 supported table set), then writes:
 *   - ADCMPTBR[channel] = (high << 16) | low
 *   - ADCMPMDR{0,1}.CMPMD<channel> = inside-window mode (1)
 *   - ADCMPENR.CMPENn |= (1 << channel)
 *   - ADDOPCRB[channel].CMPTBLEm |= (1 << channel)
 *
 * @param[in] channel Channel / table index (0..7).
 * @param[in] low     Low-side threshold.
 * @param[in] high    High-side threshold (must be >= low).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Window programmed.
 * @retval k_ra_err_invalid_arg    @p high < @p low.
 * @retval k_ra_err_out_of_range   @p channel >= 8 (no compare table).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_set_compare_window(uint8_t channel, uint16_t low, uint16_t high);

/* =============================================================================
 * Oversampling
 * =============================================================================
 */

/**
 * @brief Programme ADDOPCRB[ch].AVEMD/ADC for a per-channel averaging mode.
 *
 * @param[in] channel Virtual-channel index (0..23).
 * @param[in] mode    Logical oversampling rate.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Mode applied.
 * @retval k_ra_err_invalid_arg    @p mode not in @ref ra_adc_oversample_t.
 * @retval k_ra_err_out_of_range   @p channel >= 24.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_adc_set_oversampling(uint8_t channel, ra_adc_oversample_t mode);

#ifdef __cplusplus
}
#endif
