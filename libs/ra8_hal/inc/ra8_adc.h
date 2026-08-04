/**
 * @file ra8_adc.h
 * @brief Full-featured ADC_B driver
 * @ingroup grp_hal_analog
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full build-out of the RA8 ADC16H peripheral (the 16-bit SAR block
 * shared byte-for-byte by the RA8D2 and RA8P1). Extends the polling
 * stub with: descriptor-based init, deinit, runtime resolution
 * reconfigure (ADDOPCRCn.ADPRC per-channel data-format), status +
 * clear, interrupt-mode attach / dispatch, power transition.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_adc_resolution_t
 * @brief ADDOPCRCn.ADPRC per-channel conversion-resolution (data-format) selector.
 *
 * @details
 * The RA8 ADC16H sets conversion precision per virtual channel through the
 * ADDOPCRCn.ADPRC[1:0] data-format field (HUM Ch 53.2.3.4 p 3339), NOT through
 * any unit-wide ADMDR mode. The driver translates each selector below into its
 * ADPRC code and programs it into every result-producing channel's ADDOPCRCn.
 * The full 16-bit range is the headline RA8P1 ADC16H capability; the RA8D2
 * exposes the same block and the identical register set. The 12-bit code that
 * legacy FSP comments assumed is just one point on this selector, not a fixed
 * hardware limit.
 *
 * @invariant Each value maps 1:1 onto a ::ra8_addopcrc_adprc_t code.
 *
 * @see ra8_adc_set_resolution
 * @see ra8_addopcrc_adprc_t
 */
typedef enum : uint8_t {
  k_ra8_adc_res_16bit = 0U, /**< 16-bit conversion result (ADPRC = 0b00). */
  k_ra8_adc_res_14bit = 1U, /**< 14-bit conversion result (ADPRC = 0b01). */
  k_ra8_adc_res_12bit = 2U, /**< 12-bit conversion result (ADPRC = 0b10). */
  k_ra8_adc_res_10bit = 3U, /**< 10-bit conversion result (ADPRC = 0b11). */
} ra8_adc_resolution_t;

/**
 * @enum ra8_adc_trigger_t
 * @brief ADCSR trigger mode.
 */
typedef enum : uint8_t {
  k_ra8_adc_trig_software = 0U, /**< Software trigger (write ADST). */
  k_ra8_adc_trig_external = 1U, /**< External trigger pin.          */
  k_ra8_adc_trig_elc      = 2U, /**< ELC event routed trigger.      */
} ra8_adc_trigger_t;

/**
 * @struct ra8_adc_cfg_t
 * @brief Configuration descriptor for ``ra8_adc_init_configured``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_adc_init_configured`` in
 * ``libs/ra8_hal/src/adc.c``.
 */
typedef struct {
  ra8_adc_resolution_t resolution;    /**< 16 / 14 / 12 / 10 bit.   */
  ra8_adc_trigger_t    trigger;       /**< Trigger source.          */
  bool                 right_aligned; /**< True -> right-align.     */
  bool                 scan_mode;     /**< True -> continuous scan. */
} ra8_adc_cfg_t;

/**
 * @enum ra8_adc_status_mask_t
 * @brief ADCSR status mask.
 */
typedef enum : uint16_t {
  k_ra8_adc_status_none = 0x0000U, /**< RA8 ADC status none.               */
  k_ra8_adc_status_busy = 0x8000U, /**< ADCSR.ADST (conversion in flight). */
  k_ra8_adc_status_ie   = 0x1000U, /**< ADCSR.ADIE.                        */
} ra8_adc_status_mask_t;

/**
 * @typedef ra8_adc_complete_fn_t
 * @brief Conversion-complete callback.
 * @param[in] ctx Caller context.
 * @param[in] result Last conversion result (raw ADDRxx).
 */
typedef void (*ra8_adc_complete_fn_t)(void* ctx, uint16_t result);

/**
 * @enum ra8_adc_trigger_src_t
 * @brief Per-scan-group trigger source.
 *
 * @details
 * Selects which event kicks the scan group:
 *   - software        - call ``ra8_adc_start_group``.
 *   - elc / gpt / pin - hardware path; ADTRGENR.STTRGENn must be set
 *                       so the silicon edge-detector arms the group.
 *
 * The selector value is observable via ``ra8_adc_b_adtrgenr()`` -- the
 * driver writes a 1 bit at @p group when the source is non-software.
 */
typedef enum : uint8_t {
  k_ra8_adc_trig_src_software = 0U, /**< SW kick (default).    */
  k_ra8_adc_trig_src_elc      = 1U, /**< ELC event mux.        */
  k_ra8_adc_trig_src_gpt      = 2U, /**< GPT compare-match.    */
  k_ra8_adc_trig_src_pin      = 3U, /**< Hardware trigger pin. */
} ra8_adc_trigger_src_t;

/**
 * @enum ra8_adc_priority_t
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
  k_ra8_adc_priority_low  = 0U, /**< RA8 ADC priority low.  */
  k_ra8_adc_priority_high = 1U, /**< RA8 ADC priority high. */
} ra8_adc_priority_t;

/**
 * @enum ra8_adc_oversample_t
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
  k_ra8_adc_oversample_off = 0U, /**< Single sample, no averaging. */
  k_ra8_adc_oversample_4x  = 1U, /**< 4-sample average.            */
  k_ra8_adc_oversample_16x = 2U, /**< 16-sample average.           */
  k_ra8_adc_oversample_64x = 3U, /**< 64-sample average.           */
} ra8_adc_oversample_t;

/**
 * @enum ra8_adc_scan_group_caps_t
 * @brief Per-scan-group capacity limits for the ADC scan model.
 */
typedef enum : uint8_t {
  k_ra8_adc_scan_group_max_channels = 8U, /**< Maximum channels per scan group. */
} ra8_adc_scan_group_caps_t;

/**
 * @struct ra8_adc_scan_group_cfg_t
 * @brief Configuration for a single scan group.
 *
 * @details
 * Each group enumerates the physical channels it scans plus the
 * trigger source and priority class. ``num_channels`` must be in
 * 1..``k_ra8_adc_scan_group_max_channels``.
 *
 * The driver assigns one ADCHCR slot per channel listed (slot index
 * == channel index, matching the read-channel convention used by
 * the legacy polling API).
 */
typedef struct {
  uint8_t               num_channels;                                /**< 1..8.                  */
  uint8_t               channels[k_ra8_adc_scan_group_max_channels]; /**< Physical-channel list. */
  ra8_adc_trigger_src_t trigger;                                     /**< Trigger source.        */
  ra8_adc_priority_t    priority;                                    /**< Priority class.        */
} ra8_adc_scan_group_cfg_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Legacy init -- 14-bit right-aligned, software trigger.
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_init(void);

/**
 * @brief Initialise ADC_B with a full config descriptor.
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 * @return ``ra8_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post On success ADCSR and ADCER match ``cfg`` and the ADC_B
 * module is powered on via MSTP.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_init_configured(const ra8_adc_cfg_t* cfg);

/**
 * @brief Tear down the ADC_B peripheral.
 * @return ``ra8_err_t`` error code.
 * @post ADCSR == 0, ADCER == 0, MSTP released.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_deinit(void);

/* =============================================================================
 * Legacy polling API
 * =============================================================================
 */

/**
 * @brief Blocking single-channel sample.
 *
 * @param[in] channel Analog channel index (0..47 on RA8D2 ADC_B).
 * @param[out] out_raw Pointer to receive the 14-bit result right-aligned.
 * @return `ra8_err_t` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_read_channel(uint8_t channel, uint16_t* out_raw);

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/**
 * @brief Change the ADC conversion resolution at runtime.
 *
 * @details
 * Translates @p resolution into its ADDOPCRCn.ADPRC[1:0] data-format code
 * (HUM Ch 53.2.3.4 p 3339) and programs it into every result-producing
 * virtual channel's ADDOPCRCn, so the next conversion on any of those
 * channels reports in the requested precision. The dedicated self-diagnosis
 * slot is left untouched -- ``ra8_adc_self_diagnose`` forces its own format.
 *
 * @param[in] resolution New resolution selector (::ra8_adc_resolution_t).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              ADPRC updated on every result channel.
 * @retval k_ra8_err_invalid_arg @p resolution is not a known selector.
 *
 * @pre ``ra8_adc_init`` (or ``ra8_adc_init_configured``) has been called.
 * @pre IRQs masked or single-threaded so no scan races the ADDOPCRC writes.
 * @post On success every result-channel ADDOPCRCn.ADPRC matches @p resolution.
 * @post On the error path no register is modified.
 *
 * @note Not thread-safe.
 * @see ra8_adc_resolution_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_set_resolution(ra8_adc_resolution_t resolution);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read the ADC status bits (ADCSR.ADST / ADIE).
 * @param[out] out_mask OR of ``k_ra8_adc_status_*``.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_get_status(uint16_t* out_mask);

/**
 * @brief Clear the ADCSR.ADST busy bit (abort in-flight conversion).
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_clear_status(void);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a conversion-complete callback.
 * @param[in] fn Non-NULL callback.
 * @param[in] ctx Context passed to the callback.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_attach_handler(ra8_adc_complete_fn_t fn, void* ctx);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @brief Put ADC_B into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_exit_stop(void);

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
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 */
void ra8_adc_dispatch_cnv_end(uint8_t channel);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Group programmed.
 * @retval k_ra8_err_null_ptr       @p cfg is NULL.
 * @retval k_ra8_err_out_of_range   @p group >= 9 or any channel index >= 24
 *                                 or @c num_channels not in 1..8.
 *
 * @pre  ``ra8_adc_init_configured`` (or ``ra8_adc_init``) has been called.
 * @pre  IRQs masked or ADSTOPR cleared so we are not racing a scan.
 * @post ADSGER bit @p group is set.
 * @post Each listed ADCHCR slot has SGSEL == @p group and CNVCS ==
 *       channels[i].
 *
 * @note Not thread-safe; caller serialises configuration.
 * @see ra8_adc_start_group
 * @see ra8_adc_stop_group
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_configure_scan_group(uint8_t                         group,
                                                     const ra8_adc_scan_group_cfg_t* cfg);

/**
 * @brief Kick a scan group via the per-group ADSTR register.
 *
 * @param[in] group Scan-group index (0..8).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Group started.
 * @retval k_ra8_err_out_of_range @p group >= 9.
 *
 * @pre  Group has been configured via ``ra8_adc_configure_scan_group``.
 * @post ADSTR[group].ADST == 1.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_start_group(uint8_t group);

/**
 * @brief Stop a scan group: clear ADSTR[group] and write ADSTOPR.
 *
 * @param[in] group Scan-group index (0..8).
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok               Group stop requested.
 * @retval k_ra8_err_out_of_range @p group >= 9.
 *
 * @post ADSTR[group].ADST == 0.
 * @post ADSTOPR.ADSTOP0|ADSTOP1 written so silicon force-stops both units.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_stop_group(uint8_t group);

/**
 * @brief Drain conversion results for every channel mapped to a group.
 *
 * @details
 * Walks the cached channel list captured by ``ra8_adc_configure_scan_group``,
 * reads ADDR[ch].DATA for each, and writes the masked 16-bit value into
 * @p out_buf in order. @p out_count receives the number of channels
 * written; it is also written on the error paths so callers can fail
 * cleanly.
 *
 * @param[in]  group     Scan-group index (0..8).
 * @param[out] out_buf   Result buffer, must hold ``cfg->num_channels`` entries.
 * @param[out] out_count Number of channels read.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Results copied.
 * @retval k_ra8_err_null_ptr       @p out_buf or @p out_count is NULL.
 * @retval k_ra8_err_out_of_range   @p group >= 9 or group never configured.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_adc_read_group_results(uint8_t group, uint16_t* out_buf, uint8_t* out_count);

/**
 * @brief Switch a scan group between one-shot and continuous (free-running).
 *
 * @details
 * Maps onto ADMDR.ADMD0: code @c k_ra8_admdr_mode_continuous when
 * @p enable is true, @c k_ra8_admdr_mode_one_cycle otherwise. The
 * @p group argument is currently advisory because ADMDR is per-unit
 * (ADC0/ADC1), not per scan-group; future silicon revisions may add a
 * per-group bit.
 *
 * @param[in] group  Scan-group index (0..8).
 * @param[in] enable True for continuous scan, false for one-shot.
 * @return ``ra8_err_t`` error code.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_set_continuous_scan(uint8_t group, bool enable);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Window programmed.
 * @retval k_ra8_err_invalid_arg    @p high < @p low.
 * @retval k_ra8_err_out_of_range   @p channel >= 8 (no compare table).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_set_compare_window(uint8_t channel, uint16_t low, uint16_t high);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 Mode applied.
 * @retval k_ra8_err_invalid_arg    @p mode not in @ref ra8_adc_oversample_t.
 * @retval k_ra8_err_out_of_range   @p channel >= 24.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_set_oversampling(uint8_t channel, ra8_adc_oversample_t mode);

/* =============================================================================
 * Self-diagnosis + internal / extended-analog channels (HUM Ch 53)
 * =============================================================================
 */

/**
 * @enum ra8_adc_selfdiag_mode_t
 * @brief ADC_B built-in self-diagnosis modes (HUM Ch 53.3.11 p 3411-3414).
 *
 * @details
 * The converter can inject one of three internal reference levels into
 * the SAR core and compare the conversion result against a known ideal,
 * proving the SAR + reference are healthy without external stimulus.
 * The numeric ideal (16-bit signed) for each mode is tabulated in HUM
 * Table 53.19 (p 3412):
 *   - Mode 1: both inputs at VREFL0 -> differential 0 -> 0x0000.
 *   - Mode 2: AINP=VREFL0, AINN=VREFH0 -> negative FS -> 0x8000 (-32768).
 *   - Mode 3: AINP=VREFH0, AINN=VREFL0 -> positive FS -> 0x7FFF (+32767).
 *
 * @see ra8_adc_self_diagnose
 */
typedef enum : uint8_t {
  k_ra8_adc_selfdiag_mode_1 = 1U, /**< Zero-differential reference. */
  k_ra8_adc_selfdiag_mode_2 = 2U, /**< Negative full-scale.         */
  k_ra8_adc_selfdiag_mode_3 = 3U, /**< Positive full-scale.         */
} ra8_adc_selfdiag_mode_t;

/**
 * @enum ra8_adc_internal_chan_t
 * @brief Internal / extended-analog channels readable through the HAL.
 *
 * @details
 * CNVCS[6:0] codes for on-chip analog sources (HUM Ch 53.2.3.1 Table
 * p 3335). Their conversion result is read from the matching ADEXDRn
 * register, not from ADDR[n] (HUM Ch 53.2.13.2 p 3391). The numeric
 * values match ::ra8_adc_b_ext_chan_t in ``ra8_adc_b_regs.h``; a
 * ``static_assert`` in ``adc.c`` guards against drift.
 *
 * @see ra8_adc_read_internal_channel
 */
typedef enum : uint8_t {
  k_ra8_adc_chan_temperature  = 0x64U, /**< On-chip die-temperature sensor. */
  k_ra8_adc_chan_int_ref_volt = 0x65U, /**< Internal reference voltage.     */
} ra8_adc_internal_chan_t;

/**
 * @brief Run an ADC_B built-in self-diagnosis pass on A/D unit 0.
 *
 * @details
 * Implements the HUM Ch 53.3.11.1 procedure (p 3412):
 *  1. Map the self-diagnosis channel (CNVCS = 0x60) onto a dedicated
 *     virtual channel in differential input mode (HUM Ch 53.2.3.1
 *     p 3335-3336).
 *  2. Force the 16-bit signed data format on that channel via ADDOPCRC
 *     (ADPRC = 0, SIGNSEL = 0) as required by HUM Ch 53.3.11.3 p 3414.
 *  3. Set ADSGDCRn.DIAGVAL[2:0] for the dedicated scan group to the
 *     requested mode (HUM Ch 53.2.4.1 p 3340).
 *  4. Start the scan group, poll ADSR.ADACT0, and read the result from
 *     ADEXDR0.DATA[15:0] plus its ERR flag (HUM Ch 53.2.13.2 p 3391).
 *  5. Compare the signed result against the mode's ideal value
 *     (Table 53.19 p 3412) within a gross-fault tolerance band and
 *     clear DIAGVAL back to off so later normal scans are unaffected.
 *
 * @param[in]  mode      Self-diagnosis mode (1 / 2 / 3).
 * @param[out] out_code  Raw 16-bit ADEXDR0 result code (signed value).
 * @param[out] out_pass  True iff ERR == 0 and the result is within the
 *                       expected-value tolerance band.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Diagnosis ran (inspect @p out_pass).
 * @retval k_ra8_err_null_ptr     @p out_code or @p out_pass is nullptr.
 * @retval k_ra8_err_invalid_arg  @p mode is not 1 / 2 / 3.
 * @retval k_ra8_err_hw_timeout   ADSR.ADACT0 never cleared.
 *
 * @pre ``ra8_adc_init`` (or ``ra8_adc_init_configured``) has been called.
 * @pre IRQs masked or single-threaded so no other scan races the group.
 * @post ADSGDCRn.DIAGVAL for the dedicated diagnostic group is back to 0.
 * @post @p out_code and @p out_pass are written on every k_ra8_ok return.
 *
 * @note Not thread-safe; serialise with normal conversions.
 * @note Tolerance band is a gross-fault detector; tighten it to the
 *       HUM Ch 69 accuracy spec for production sign-off.
 * @see ra8_adc_selfdiag_mode_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_adc_self_diagnose(ra8_adc_selfdiag_mode_t mode, uint16_t* out_code, bool* out_pass);

/**
 * @brief Blocking single conversion of an internal / extended-analog channel.
 *
 * @details
 * Maps @p chan (e.g. temperature sensor CNVCS = 0x64) onto a dedicated
 * virtual channel in single-ended mode, forces the 12-bit unsigned data
 * format, starts the dedicated scan group, polls ADSR.ADACT0, and reads
 * the 12-bit code from the matching ADEXDRn register (n = CNVCS - 0x60,
 * HUM Ch 53.2.13.2 p 3391).
 *
 * @param[in]  chan    Internal channel selector.
 * @param[out] out_raw Receives the masked conversion code.
 *
 * @return ra8_err_t Status code.
 * @retval k_ra8_ok               Conversion read into @p out_raw.
 * @retval k_ra8_err_null_ptr     @p out_raw is nullptr.
 * @retval k_ra8_err_invalid_arg  @p chan is not a supported internal channel.
 * @retval k_ra8_err_hw_timeout   ADSR.ADACT0 never cleared.
 * @retval k_ra8_err_out_of_range The mapped ADEXDR slot does not exist.
 *
 * @pre ``ra8_adc_init`` has been called.
 * @pre For the temperature channel, ``ra8_tsn_init`` has routed the
 *      sensor to the ADC mux (TSCR.TSOE).
 * @post @p out_raw == 0 on every error path.
 * @post No scan-group state other than the dedicated diagnostic group
 *       is modified.
 *
 * @note Not thread-safe.
 * @see ra8_adc_internal_chan_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_adc_read_internal_channel(ra8_adc_internal_chan_t chan,
                                                      uint16_t*               out_raw);

#ifdef __cplusplus
}
#endif
