/**
 * @file ra_ctsu.c
 * @brief Capacitive Touch Sensing Unit (CTSU2) driver implementation
 *
 * @details
 * Mirrors the public surface of FSP ``r_ctsu``. The driver keeps
 * per-electrode thresholds and offsets in RAM (FSP does the same in
 * ``ctsu_instance_ctrl_t``) and only programs CTSU registers for
 * lifecycle, scan control and per-electrode calibration.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ctsu.h"

#include <stdint.h>

#include "ra8d2_ctsu_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/**
 * @enum ra_ctsu_state_t
 * @brief Driver state machine.
 */
typedef enum : uint8_t {
  k_ra_ctsu_state_closed   = 0U, /**< Block powered down, init not run. */
  k_ra_ctsu_state_open     = 1U, /**< Block configured, idle. */
  k_ra_ctsu_state_scanning = 2U, /**< Scan currently in progress. */
} ra_ctsu_state_t;

/**
 * @enum ra_ctsu_local_t
 * @brief Local enumerated constants.
 */
typedef enum : uint16_t {
  k_ra_ctsu_calib_su_init = 0x0040U, /**< CTSUSO0 SO seed used during calibration. */
} ra_ctsu_local_t;

/**
 * @enum ra_ctsu_shifts_t
 * @brief Bit-shift constants used when packing CTSUSC0 + CTSUSC1.
 */
typedef enum : uint8_t {
  k_ra_ctsu_shift_high16 = 16U, /**< CTSUSC1 occupies bits [31..16] of the raw count. */
} ra_ctsu_shifts_t;

static const char* s_tag = "CTSU";

/**
 * @var s_state
 * @brief Driver state.
 *
 * @details Module-private state machine. Touched by every public API.
 * @note Single-threaded init context only.
 */
static ra_ctsu_state_t s_state = k_ra_ctsu_state_closed;

/**
 * @var s_event_cb
 * @brief Registered scan-complete / error callback.
 */
static ra_ctsu_event_fn_t s_event_cb = nullptr;

/**
 * @var s_event_ctx
 * @brief Opaque context paired with ``s_event_cb``.
 */
static void* s_event_ctx = nullptr;

/**
 * @var s_thresholds
 * @brief Per-electrode touch threshold table (FSP keeps this in RAM too).
 */
static uint16_t s_thresholds[k_ra_ctsu_max_electrodes];

/**
 * @var s_offsets
 * @brief Per-electrode calibration offset table.
 */
static uint16_t s_offsets[k_ra_ctsu_max_electrodes];

/**
 * @brief Extract one byte of the 36-bit electrode bitmap.
 *
 * @param[in] mask  Source bitmap.
 * @param[in] shift Bit shift (one of ``k_ra_ctsu_shift_b*``).
 * @return The selected byte.
 */
static inline uint8_t internal_ra_ctsu_byte(uint64_t mask, uint8_t shift)
{
  return (uint8_t)((mask >> shift) & (uint64_t)k_ra_ctsu_byte_mask);
}

/**
 * @brief Apply ``cfg`` to the CTSU register block.
 *
 * @param[in] cfg Validated, non-NULL configuration descriptor.
 */
static void internal_ra_ctsu_apply_cfg(const ra_ctsu_cfg_t* cfg)
{
  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();

  /* Set mode in CTSUCR1.MD[1:0] (bits 4..5), preserving other bits. */
  const uint8_t cr1_old    = reg->CTSUCR1;
  const uint8_t cr1_keep   = (uint8_t)(cr1_old & (uint8_t)~k_ra_ctsu_cr1_mask_md);
  const uint8_t mode_field = (uint8_t)((uint32_t)cfg->mode << (uint32_t)k_ra_ctsu_cr1_bit_md0);
  reg->CTSUCR1             = (uint8_t)(cr1_keep | mode_field);

  reg->CTSUDCLKC = (uint8_t)cfg->clock_div;
  reg->CTSUSDPRS = cfg->scan_period;
  reg->CTSUSST   = cfg->stabilise_wait;

  reg->CTSUCHAC0 = internal_ra_ctsu_byte(cfg->electrode_mask, 0U);
  reg->CTSUCHAC1 = internal_ra_ctsu_byte(cfg->electrode_mask, k_ra_ctsu_shift_b1);
  reg->CTSUCHAC2 = internal_ra_ctsu_byte(cfg->electrode_mask, k_ra_ctsu_shift_b2);
  reg->CTSUCHAC3 = internal_ra_ctsu_byte(cfg->electrode_mask, k_ra_ctsu_shift_b3);
  reg->CTSUCHAC4 = internal_ra_ctsu_byte(cfg->electrode_mask, k_ra_ctsu_shift_b4);

  reg->CTSUCHTRC0 = internal_ra_ctsu_byte(cfg->transmit_mask, 0U);
  reg->CTSUCHTRC1 = internal_ra_ctsu_byte(cfg->transmit_mask, k_ra_ctsu_shift_b1);
  reg->CTSUCHTRC2 = internal_ra_ctsu_byte(cfg->transmit_mask, k_ra_ctsu_shift_b2);
  reg->CTSUCHTRC3 = internal_ra_ctsu_byte(cfg->transmit_mask, k_ra_ctsu_shift_b3);
  reg->CTSUCHTRC4 = internal_ra_ctsu_byte(cfg->transmit_mask, k_ra_ctsu_shift_b4);

  reg->CTSUSO0 = cfg->offset_initial;
  reg->CTSUSO1 = 0U;
}

[[nodiscard]] ra_err_t ra_ctsu_open(const ra_ctsu_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  if (cfg->electrode_mask == 0U) {
    ra_log_error(s_tag, "ctsu_open: empty electrode mask");
    return k_ra_err_invalid_arg;
  }

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();

  /* Software reset, then power on. */
  reg->CTSUCR0 = k_ra_ctsu_cr0_mask_init;
  reg->CTSUCR0 = k_ra_ctsu_cr0_mask_pon;

  internal_ra_ctsu_apply_cfg(cfg);

  for (uint8_t i = 0U; i < k_ra_ctsu_max_electrodes; ++i) {
    s_thresholds[i] = 0U;
    s_offsets[i]    = 0U;
  }

  s_state = k_ra_ctsu_state_open;
  ra_log_info(s_tag, "ctsu_open");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ctsu_close(void)
{
  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  reg->CTSUCR0                = 0U;
  s_state                     = k_ra_ctsu_state_closed;
  s_event_cb                  = nullptr;
  s_event_ctx                 = nullptr;
  ra_log_info(s_tag, "ctsu_close");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ctsu_scan_start(void)
{
  if (s_state == k_ra_ctsu_state_closed) {
    ra_log_error(s_tag, "scan_start: not initialized");
    return k_ra_err_not_initialized;
  }

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  reg->CTSUCR0                = (uint8_t)(reg->CTSUCR0 | k_ra_ctsu_cr0_mask_strt);
  s_state                     = k_ra_ctsu_state_scanning;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ctsu_scan_stop(void)
{
  if (s_state == k_ra_ctsu_state_closed) {
    ra_log_error(s_tag, "scan_stop: not initialized");
    return k_ra_err_not_initialized;
  }

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  reg->CTSUCR0                = (uint8_t)(reg->CTSUCR0 & (uint8_t)~k_ra_ctsu_cr0_mask_strt);
  s_state                     = k_ra_ctsu_state_open;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ctsu_get_data(uint8_t electrode, uint32_t* raw_count)
{
  RA_CHECK_NULL_PTR(raw_count, s_tag, "raw_count must not be nullptr");

  if (electrode >= k_ra_ctsu_max_electrodes) {
    return k_ra_err_invalid_arg;
  }
  if (s_state == k_ra_ctsu_state_closed) {
    return k_ra_err_not_initialized;
  }

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();
  const uint16_t          lo  = reg->CTSUSC0;
  const uint16_t          hi  = reg->CTSUSC1;
  *raw_count                  = (uint32_t)lo | ((uint32_t)hi << (uint32_t)k_ra_ctsu_shift_high16);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ctsu_attach_handler(ra_ctsu_event_fn_t fn, void* ctx)
{
  s_event_cb  = fn;
  s_event_ctx = ctx;
  return k_ra_ok;
}

void ra_ctsu_dispatch(uint8_t electrode, ra_err_t err)
{
  if (electrode >= k_ra_ctsu_max_electrodes) {
    return;
  }
  if (s_event_cb != nullptr) {
    s_event_cb(s_event_ctx, electrode, err);
  }
}

[[nodiscard]] ra_err_t ra_ctsu_calibrate(uint8_t electrode)
{
  if (electrode >= k_ra_ctsu_max_electrodes) {
    return k_ra_err_invalid_arg;
  }
  if (s_state == k_ra_ctsu_state_closed) {
    return k_ra_err_not_initialized;
  }

  volatile r_ctsu_regs_t* reg = ra_ctsu_regs();

  /* Seed CTSUSO0 with the FSP calibration offset and run three dummy
   * scans, averaging CTSUSC0 across them.
   */
  reg->CTSUSO0  = k_ra_ctsu_calib_su_init;
  reg->CTSUMCH0 = electrode;
  uint32_t sum  = 0U;
  for (uint8_t i = 0U; i < k_ra_ctsu_calib_dummy_cnt; ++i) {
    reg->CTSUCR0 = (uint8_t)(reg->CTSUCR0 | k_ra_ctsu_cr0_mask_strt);
    reg->CTSUCR0 = (uint8_t)(reg->CTSUCR0 & (uint8_t)~k_ra_ctsu_cr0_mask_strt);
    sum += (uint32_t)reg->CTSUSC0;
  }
  const uint16_t average = (uint16_t)(sum / (uint32_t)k_ra_ctsu_calib_dummy_cnt);
  s_offsets[electrode]   = average;
  reg->CTSUSO0           = average;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ctsu_set_threshold(uint8_t electrode, uint16_t threshold)
{
  if (electrode >= k_ra_ctsu_max_electrodes) {
    return k_ra_err_invalid_arg;
  }
  s_thresholds[electrode] = threshold;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ctsu_get_threshold(uint8_t electrode, uint16_t* out_threshold)
{
  RA_CHECK_NULL_PTR(out_threshold, s_tag, "out_threshold must not be nullptr");
  if (electrode >= k_ra_ctsu_max_electrodes) {
    return k_ra_err_invalid_arg;
  }
  *out_threshold = s_thresholds[electrode];
  return k_ra_ok;
}
