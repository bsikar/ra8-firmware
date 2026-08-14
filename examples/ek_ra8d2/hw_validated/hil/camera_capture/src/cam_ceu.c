/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/src/cam_ceu.c
 * @brief GPIO activity diagnostic for the J35 parallel DVP camera signals.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Owns only the 11-entry GPIO pin map and bounded signal-activity sampler used
 * by the HIL diagnostics. The reusable `ra8_camera_source_ceu` backend owns CEU
 * dispatch while the application supplies its state and DMA buffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "cam_ceu.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_port_regs.h"
#include "ra8_port_utils.h"
#include "ra8_systick.h"

/* =============================================================================
 * DVP diagnostic constants (typed enums -- no magic numbers)
 * =============================================================================
 */

/** @brief Bounded GPIO signal-activity sampling constants. */
typedef enum : uint32_t {
  k_cam_sync_samples    = 2000000U, /**< GPIO samples for live-sync diagnostic.    */
  k_cam_data_d2_pin_bit = 5U,       /**< P405 bit position for camera D2.          */
  k_cam_data_d5_out_bit = 5U,       /**< Reconstructed byte bit for camera D5.     */
  k_cam_data_d7_out_bit = 7U,       /**< Reconstructed byte bit for camera D7.     */
  k_cam_long_line_pclk  = 1024U,    /**< Diagnostic threshold for long HREF lines. */
} cam_ceu_poll_t;

/** @brief One CEU DVP pin: MCU port/pin routed to the CEU peripheral. */
typedef struct {
  ra8_port_t port; /**< MCU port.                */
  ra8_pin_t  pin;  /**< MCU pin within the port. */
} cam_ceu_pin_t;

/** @brief GPIO register banks used while sampling the parallel camera bus. */
typedef struct {
  volatile r_port_regs_t* port4;  /**< D0, D2, and D3 input bank. */
  volatile r_port_regs_t* port7;  /**< D4 through D7 input bank.  */
  volatile r_port_regs_t* port9;  /**< D1 input bank.             */
  volatile r_port_regs_t* port11; /**< VSYNC, HSYNC, and PCLK.    */
} cam_probe_ports_t;

/** @brief Accumulator for the bounded sync/data sampling pass. */
typedef struct {
  uint32_t prior;             /**< Previous masked sync sample.      */
  uint32_t run_samples;       /**< Samples in the current HSYNC run. */
  uint32_t run_start_cycles;  /**< Cycle counter at HSYNC run start. */
  uint32_t prior_pclk_cycles; /**< Cycle counter at prior PCLK edge. */
  uint32_t high_min;          /**< Minimum high run in samples.      */
  uint32_t high_max;          /**< Maximum high run in samples.      */
  uint32_t low_min;           /**< Minimum low run in samples.       */
  uint32_t low_max;           /**< Maximum low run in samples.       */
  uint32_t high_cycles_min;   /**< Minimum high run in CPU cycles.   */
  uint32_t high_cycles_max;   /**< Maximum high run in CPU cycles.   */
  uint8_t  data_min;          /**< Minimum sampled camera byte.      */
  uint8_t  data_max;          /**< Maximum sampled camera byte.      */
  uint8_t  data_and;          /**< AND reduction of camera bytes.    */
  uint8_t  data_or;           /**< OR reduction of camera bytes.     */
  uint8_t  prior_data;        /**< Previously sampled camera byte.   */
} cam_sync_state_t;

/**
 * @var s_ceu_pins
 * @brief The 11 EK-RA8D2 J35 parallel-camera pins that feed the CEU:
 *        VIO_D[7:0], VIO_VD, VIO_HD, VIO_CLK (EK-RA8D2 UM Table 35 p 48).
 * @since 0.1.0
 */
static const cam_ceu_pin_t s_ceu_pins[] = {
  {k_ra8_port_4, k_ra8_pin_0},  /* D0    P400 (VIO_D0)  */
  {k_ra8_port_9, k_ra8_pin_2},  /* D1    P902 (VIO_D1)  */
  {k_ra8_port_4, k_ra8_pin_5},  /* D2    P405 (VIO_D2)  */
  {k_ra8_port_4, k_ra8_pin_6},  /* D3    P406 (VIO_D3)  */
  {k_ra8_port_7, k_ra8_pin_0},  /* D4    P700 (VIO_D4)  */
  {k_ra8_port_7, k_ra8_pin_1},  /* D5    P701 (VIO_D5)  */
  {k_ra8_port_7, k_ra8_pin_2},  /* D6    P702 (VIO_D6)  */
  {k_ra8_port_7, k_ra8_pin_3},  /* D7    P703 (VIO_D7)  */
  {k_ra8_port_11, k_ra8_pin_2}, /* VSYNC PB02 (VIO_VD)  */
  {k_ra8_port_11, k_ra8_pin_3}, /* HSYNC PB03 (VIO_HD)  */
  {k_ra8_port_11, k_ra8_pin_4}, /* PCLK  PB04 (VIO_CLK) */
};

/**
 * @brief Release a bounded prefix of the temporary camera GPIO claims.
 *
 * @param[in] count Number of entries in ::s_ceu_pins to release.
 * @pre `count` does not exceed the pin-table length.
 * @pre Each selected pin is either claimed or safe to release.
 * @post Every selected pin has been offered back to the GPIO owner.
 * @post Pins outside the selected prefix are untouched.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static void cam_release_probe_pins(uint32_t count)
{
  const uint32_t pin_count = (uint32_t)(sizeof(s_ceu_pins) / sizeof(s_ceu_pins[0]));
  const uint32_t bounded   = (count < pin_count) ? count : pin_count;
  for (uint32_t i = 0U; i < bounded; i += 1U) {
    (void)ra8_gpio_release(RA8_PIN(s_ceu_pins[i].port, s_ceu_pins[i].pin));
  }
}

/**
 * @brief Claim every parallel-camera pin as a temporary GPIO input.
 *
 * @return Error status from the claim sequence.
 * @retval k_ra8_ok Every camera pin is configured as an input.
 * @retval k_ra8_err_gpio_conflict A pin could not be claimed.
 * @pre The CEU has not claimed the parallel-camera pins.
 * @pre GPIO ownership tracking is initialized.
 * @post Success leaves every camera pin claimed as an input.
 * @post Failure releases every pin claimed by this call.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra8_err_t cam_claim_probe_pins(void)
{
  const uint32_t pin_count = (uint32_t)(sizeof(s_ceu_pins) / sizeof(s_ceu_pins[0]));
  uint32_t       claimed   = 0U;
  for (; claimed < pin_count; claimed += 1U) {
    const ra8_port_pin_t pin = RA8_PIN(s_ceu_pins[claimed].port, s_ceu_pins[claimed].pin);
    if (ra8_gpio_input_init(pin, k_ra8_pull_none) != k_ra8_ok) {
      cam_release_probe_pins(claimed);
      return k_ra8_err_gpio_conflict;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Resolve the four register banks that carry the camera signals.
 *
 * @param[out] ports Receives non-NULL register-bank pointers.
 * @return Error status from register-bank resolution.
 * @retval k_ra8_ok Every required bank is addressable.
 * @retval k_ra8_err_hw_error A required bank is unavailable.
 * @retval k_ra8_err_null_ptr `ports` was NULL.
 * @pre `ports` is non-NULL.
 * @pre The GPIO register layer is initialized.
 * @post Success fills all four bank pointers.
 * @post Failure does not dereference an unavailable bank.
 * @note Thread safety: pure register-address resolution.
 * @since 0.1.0
 */
static ra8_err_t cam_get_probe_ports(cam_probe_ports_t* ports)
{
  RA8_CHECK_NULL_PTR(ports, "cam", "probe_ports");
  const cam_probe_ports_t resolved = {
    .port4  = ra8_port(k_ra8_port_4),
    .port7  = ra8_port(k_ra8_port_7),
    .port9  = ra8_port(k_ra8_port_9),
    .port11 = ra8_port(k_ra8_port_11),
  };
  if ((resolved.port4 == nullptr) || (resolved.port7 == nullptr) || (resolved.port9 == nullptr) ||
      (resolved.port11 == nullptr)) {
    return k_ra8_err_hw_error;
  }
  *ports = resolved;
  return k_ra8_ok;
}

/**
 * @brief Reconstruct VIO_D[7:0] from the three GPIO input banks.
 *
 * @param[in] ports Resolved camera GPIO banks.
 * @return Current parallel-camera data byte.
 * @pre Every pointer in `ports` is valid.
 * @pre Camera pins are configured as GPIO inputs.
 * @post The return value represents one coherent best-effort bus sample.
 * @post No GPIO register is modified.
 * @note Thread safety: pure MMIO reads.
 * @since 0.1.0
 */
[[gnu::always_inline]] static inline uint8_t cam_read_probe_data(const cam_probe_ports_t* ports)
{
  /* HUM Ch 20.2 "PCNTR2 : Port Control Register 2" p 841 */
  const uint32_t p4 = ports->port4->PCNTR2;
  const uint32_t p7 = ports->port7->PCNTR2;
  const uint32_t p9 = ports->port9->PCNTR2;
  return (
    uint8_t)(((p4 >> 0U) & 1U) | (((p9 >> 2U) & 1U) << 1U) |
             (((p4 >> (uint32_t)k_cam_data_d2_pin_bit) & 1U) << 2U) | (((p4 >> 6U) & 1U) << 3U) |
             (((p7 >> 0U) & 1U) << 4U) | (((p7 >> 1U) & 1U) << (uint32_t)k_cam_data_d5_out_bit) |
             (((p7 >> 2U) & 1U) << 6U) | (((p7 >> 3U) & 1U) << (uint32_t)k_cam_data_d7_out_bit));
}

/**
 * @brief Read and mask the three camera sync signals from PORT11.
 *
 * @param[in] ports Resolved camera GPIO banks.
 * @param[in] sync_mask Mask covering VSYNC, HSYNC, and PCLK.
 * @return Current masked camera sync sample.
 * @pre `ports->port11` is valid.
 * @pre Camera sync pins are configured as GPIO inputs.
 * @post The returned value contains no bits outside `sync_mask`.
 * @post No GPIO register is modified.
 * @note Thread safety: pure MMIO read.
 * @since 0.1.0
 */
[[gnu::always_inline]] static inline uint32_t cam_read_probe_sync(const cam_probe_ports_t* ports,
                                                                  uint32_t sync_mask)
{
  /* HUM Ch 20.2 "PCNTR2 : Port Control Register 2" p 841 */
  return ports->port11->PCNTR2 & sync_mask;
}

/**
 * @brief Initialize sync-sampling state from the first masked sample.
 *
 * @param[out] state State accumulator to initialize.
 * @param[in] prior First masked sync sample.
 * @pre `state` is non-NULL.
 * @pre DWT cycle counting is available.
 * @post All minima use their maximum-value sentinels.
 * @post Timing origins describe the instant of initialization.
 * @note Thread safety: caller-owned state only.
 * @since 0.1.0
 */
static void cam_sync_state_init(cam_sync_state_t* state, uint32_t prior)
{
  const uint32_t         now     = ra8_dwt_cyccnt_read();
  const cam_sync_state_t initial = {
    .prior             = prior,
    .run_samples       = 1U,
    .run_start_cycles  = now,
    .prior_pclk_cycles = now,
    .high_min          = UINT32_MAX,
    .low_min           = UINT32_MAX,
    .high_cycles_min   = UINT32_MAX,
    .data_min          = UINT8_MAX,
    .data_and          = UINT8_MAX,
  };
  *state = initial;
}

/**
 * @brief Fold one active-line camera byte into the probe statistics.
 *
 * @param[in,out] state Sync/data accumulator.
 * @param[in,out] probe Public probe result being accumulated.
 * @param[in] data Newly sampled camera byte.
 * @pre `state` and `probe` are non-NULL.
 * @pre `data` was sampled on an active-line PCLK rising edge.
 * @post Byte extrema and reductions include `data`.
 * @post Change count compares `data` with the preceding sample.
 * @note Thread safety: caller-owned state only.
 * @since 0.1.0
 */
[[gnu::always_inline]] static inline void
cam_record_probe_data(cam_sync_state_t* state, cam_ceu_sync_probe_t* probe, uint8_t data)
{
  if ((probe->data_samples != 0U) && (data != state->prior_data)) {
    probe->data_changes += 1U;
  }
  state->data_min   = (data < state->data_min) ? data : state->data_min;
  state->data_max   = (data > state->data_max) ? data : state->data_max;
  state->data_and   = (uint8_t)(state->data_and & data);
  state->data_or    = (uint8_t)(state->data_or | data);
  state->prior_data = data;
  probe->data_samples += 1U;
}

/**
 * @brief Fold one masked sync sample into edge, timing, and data statistics.
 *
 * @param[in] ports Resolved camera GPIO banks.
 * @param[in] current Current masked VSYNC/HSYNC/PCLK sample.
 * @param[in,out] state Private sampling state.
 * @param[in,out] probe Public probe result being accumulated.
 * @pre All pointers are non-NULL and refer to caller-owned state.
 * @pre `current` contains only the three sync bits.
 * @post Edge and run statistics include `current`.
 * @post `state->prior` equals `current`.
 * @note Thread safety: bounded single-caller sampling loop.
 * @since 0.1.0
 */
[[gnu::always_inline]] static inline void cam_record_sync_sample(const cam_probe_ports_t* ports,
                                                                 uint32_t                 current,
                                                                 cam_sync_state_t*        state,
                                                                 cam_ceu_sync_probe_t*    probe)
{
  const uint32_t changed = current ^ state->prior;
  probe->vsync_edges += ((changed & (1UL << (uint32_t)k_ra8_pin_2)) != 0U) ? 1U : 0U;
  probe->hsync_edges += ((changed & (1UL << (uint32_t)k_ra8_pin_3)) != 0U) ? 1U : 0U;
  if ((changed & (1UL << (uint32_t)k_ra8_pin_4)) != 0U) {
    const uint32_t now         = ra8_dwt_cyccnt_read();
    const uint32_t half_period = now - state->prior_pclk_cycles;
    probe->pclk_edges += 1U;
    probe->pclk_half_cycles_min =
      (half_period < probe->pclk_half_cycles_min) ? half_period : probe->pclk_half_cycles_min;
    state->prior_pclk_cycles = now;
    if (((current & (1UL << (uint32_t)k_ra8_pin_4)) != 0U) &&
        ((current & (1UL << (uint32_t)k_ra8_pin_3)) != 0U)) {
      cam_record_probe_data(state, probe, cam_read_probe_data(ports));
    }
  }
  if ((current & (1UL << (uint32_t)k_ra8_pin_3)) ==
      (state->prior & (1UL << (uint32_t)k_ra8_pin_3))) {
    state->run_samples += 1U;
  } else if ((state->prior & (1UL << (uint32_t)k_ra8_pin_3)) != 0U) {
    const uint32_t run_cycles = ra8_dwt_cyccnt_read() - state->run_start_cycles;
    state->high_min = (state->run_samples < state->high_min) ? state->run_samples : state->high_min;
    state->high_max = (state->run_samples > state->high_max) ? state->run_samples : state->high_max;
    state->high_cycles_min =
      (run_cycles < state->high_cycles_min) ? run_cycles : state->high_cycles_min;
    state->high_cycles_max =
      (run_cycles > state->high_cycles_max) ? run_cycles : state->high_cycles_max;
    state->run_samples      = 1U;
    state->run_start_cycles = ra8_dwt_cyccnt_read();
  } else {
    state->low_min = (state->run_samples < state->low_min) ? state->run_samples : state->low_min;
    state->low_max = (state->run_samples > state->low_max) ? state->run_samples : state->low_max;
    state->run_samples      = 1U;
    state->run_start_cycles = ra8_dwt_cyccnt_read();
  }
  state->prior = current;
}

/**
 * @brief Run the bounded sync/data sampling pass and finalize its statistics.
 *
 * @param[in] ports Resolved camera GPIO banks.
 * @param[in] sync_mask Mask covering VSYNC, HSYNC, and PCLK.
 * @param[in,out] probe Probe result to populate.
 * @pre All pointers are valid and camera GPIOs are claimed.
 * @pre `sync_mask` selects the three camera sync signals.
 * @post `probe` contains bounded edge, run, timing, and data statistics.
 * @post Sentinel minima are converted to zero when no sample was observed.
 * @note Thread safety: not thread-safe; reads live GPIO state.
 * @since 0.1.0
 */
static void
cam_measure_sync(const cam_probe_ports_t* ports, uint32_t sync_mask, cam_ceu_sync_probe_t* probe)
{
  cam_sync_state_t state = {};
  cam_sync_state_init(&state, cam_read_probe_sync(ports, sync_mask));
  probe->pclk_half_cycles_min = UINT32_MAX;
  for (uint32_t i = 0U; i < (uint32_t)k_cam_sync_samples; i += 1U) {
    cam_record_sync_sample(ports, cam_read_probe_sync(ports, sync_mask), &state, probe);
  }
  probe->hsync_high_min        = (state.high_min == UINT32_MAX) ? 0U : state.high_min;
  probe->hsync_high_max        = state.high_max;
  probe->hsync_low_min         = (state.low_min == UINT32_MAX) ? 0U : state.low_min;
  probe->hsync_low_max         = state.low_max;
  probe->hsync_high_cycles_min = (state.high_cycles_min == UINT32_MAX) ? 0U : state.high_cycles_min;
  probe->hsync_high_cycles_max = state.high_cycles_max;
  probe->pclk_half_cycles_min =
    (probe->pclk_half_cycles_min == UINT32_MAX) ? 0U : probe->pclk_half_cycles_min;
  probe->data_min = (state.data_min == UINT8_MAX) ? 0U : state.data_min;
  probe->data_max = state.data_max;
  probe->data_and = (probe->data_samples == 0U) ? 0U : state.data_and;
  probe->data_or  = state.data_or;
}

/**
 * @brief Measure active-line PCLK counts with three bounded sampling loops.
 *
 * @param[in] ports Resolved camera GPIO banks.
 * @param[in] sync_mask Mask covering VSYNC, HSYNC, and PCLK.
 * @param[in,out] probe Probe result to populate.
 * @pre All pointers are valid and camera GPIOs are claimed.
 * @pre The sensor is configured for continuous DVP streaming.
 * @post Line-count fields describe only complete observed HSYNC pulses.
 * @post A missing line produces zero-valued minimum and mean fields.
 * @note Thread safety: not thread-safe; reads live GPIO state.
 * @since 0.1.0
 */
static void
cam_measure_lines(const cam_probe_ports_t* ports, uint32_t sync_mask, cam_ceu_sync_probe_t* probe)
{
  uint32_t prior = cam_read_probe_sync(ports, sync_mask);
  for (uint32_t i = 0U;
       (i < (uint32_t)k_cam_sync_samples) && ((prior & (1UL << (uint32_t)k_ra8_pin_3)) != 0U);
       i += 1U) {
    prior = cam_read_probe_sync(ports, sync_mask);
  }
  for (uint32_t i = 0U;
       (i < (uint32_t)k_cam_sync_samples) && ((prior & (1UL << (uint32_t)k_ra8_pin_3)) == 0U);
       i += 1U) {
    prior = cam_read_probe_sync(ports, sync_mask);
  }
  uint32_t line_pclk = 0U;
  uint32_t line_sum  = 0U;
  uint32_t line_min  = UINT32_MAX;
  for (uint32_t i = 0U; i < (uint32_t)k_cam_sync_samples; i += 1U) {
    const uint32_t current = cam_read_probe_sync(ports, sync_mask);
    const uint32_t changed = current ^ prior;
    if (((changed & (1UL << (uint32_t)k_ra8_pin_4)) != 0U) &&
        ((current & (1UL << (uint32_t)k_ra8_pin_4)) != 0U) &&
        ((current & (1UL << (uint32_t)k_ra8_pin_3)) != 0U)) {
      line_pclk += 1U;
    }
    if (((changed & (1UL << (uint32_t)k_ra8_pin_3)) != 0U) &&
        ((current & (1UL << (uint32_t)k_ra8_pin_3)) == 0U)) {
      line_min             = (line_pclk < line_min) ? line_pclk : line_min;
      probe->line_pclk_max = (line_pclk > probe->line_pclk_max) ? line_pclk : probe->line_pclk_max;
      line_sum += line_pclk;
      probe->line_pclk_long += (line_pclk > (uint32_t)k_cam_long_line_pclk) ? 1U : 0U;
      probe->measured_lines += 1U;
      line_pclk = 0U;
    }
    prior = current;
  }
  probe->line_pclk_min  = (line_min == UINT32_MAX) ? 0U : line_min;
  probe->line_pclk_mean = (probe->measured_lines == 0U) ? 0U : (line_sum / probe->measured_lines);
}

ra8_err_t cam_probe_sync_activity(cam_ceu_sync_probe_t* out_probe)
{
  RA8_CHECK_NULL_PTR(out_probe, "cam", "sync_probe");
  *out_probe    = (cam_ceu_sync_probe_t){};
  ra8_err_t err = cam_claim_probe_pins();
  if (err != k_ra8_ok) {
    return err;
  }
  cam_probe_ports_t ports = {};
  err                     = cam_get_probe_ports(&ports);
  if (err == k_ra8_ok) {
    const uint32_t sync_mask = (1UL << (uint32_t)k_ra8_pin_2) | (1UL << (uint32_t)k_ra8_pin_3) |
                               (1UL << (uint32_t)k_ra8_pin_4);
    cam_measure_sync(&ports, sync_mask, out_probe);
    cam_measure_lines(&ports, sync_mask, out_probe);
  }
  cam_release_probe_pins((uint32_t)(sizeof(s_ceu_pins) / sizeof(s_ceu_pins[0])));
  return err;
}
