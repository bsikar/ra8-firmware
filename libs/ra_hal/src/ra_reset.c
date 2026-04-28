/**
 * @file ra_reset.c
 * @brief Reset cause introspection + software-reset trigger
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implementation of the public API declared in ``ra_reset.h``. Reads
 * RSTSR0/1/2/3 to decode the latched reset cause, mirrors the bits
 * out via ``ra_reset_clear_cause``, surfaces RSTSAR for security
 * attribution, and triggers Cortex-M85 ``AIRCR.SYSRESETREQ`` for a
 * software reset.
 *
 * Every register access carries a HUM Ch 6 citation. The driver does
 * **not** unlock PRCR -- the RSTSRn registers are R/W with no PRCR
 * gate (HUM Ch 6.2.2 p 257 "PRCR write enable" column is absent), and
 * RSTSAR is read-only from this driver's perspective.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_reset.h"

#include <stdint.h>

#include "ra8d2_reset_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/**
 * @var s_tag
 * @brief Logging tag for the reset driver.
 */
static const char* s_tag = "RESET";

/**
 * @enum ra_reset_clear_layout_t
 * @brief Bit layout of the ``mask`` argument to ``ra_reset_clear_cause``.
 *
 * @details
 * The clear mask packs three register selectors into one 32-bit word:
 *
 *   - ``mask[7:0]``    -- RSTSR0 bits to clear (PORF, LVDxRF, DPSRSTF)
 *   - ``mask[30:8]``   -- RSTSR1 bits to clear, shifted right by 8 here
 *                         (so caller passes RSTSR1 mask << 8).
 *   - ``mask[31]``     -- request to write 1 to RSTSR2.CWSF (rare; HUM
 *                         Ch 6.2.4 Note 2 p 261 says CWSF is set by
 *                         writing 1, not cleared by writing 0).
 */
typedef enum : uint32_t {
  k_ra_reset_mask_rstsr0_msk    = 0x000000FFUL, /**< RSTSR0 selector window. */
  k_ra_reset_mask_rstsr1_shift  = 8U,           /**< RSTSR1 mask <<8.        */
  k_ra_reset_mask_rstsr1_window = 0x7FFFFF00UL, /**< RSTSR1 selector window. */
  k_ra_reset_mask_rstsr2_cwsf   = 0x80000000UL, /**< Write 1 to RSTSR2.CWSF. */
} ra_reset_clear_layout_t;

/* =============================================================================
 * Cached snapshot
 * =============================================================================
 */

/**
 * @struct ra_reset_state_t
 * @brief File-scope cached snapshot of reset cause + raw registers.
 */
typedef struct {
  bool             initialised; /**< True once ``ra_reset_init`` ran.    */
  ra_reset_cause_t cause;       /**< Decoded primary cause.              */
  ra_reset_raw_t   raw;         /**< Raw register words at boot.         */
} ra_reset_state_t;

/**
 * @var s_state
 * @brief File-scope snapshot. Zero-initialised by the C runtime.
 */
static ra_reset_state_t s_state;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Read RSTSR0/1/2/3 into ``out``.
 *
 * @param[out] out Destination raw struct (caller-validated non-NULL).
 */
static void internal_read_raw(ra_reset_raw_t* out)
{
  /* HUM Ch 6.2.2 "RSTSR0 : Reset Status Register 0", p 257 */
  out->rstsr0 = *ra_reset_rstsr0();
  /* HUM Ch 6.2.3 "RSTSR1 : Reset Status Register 1", p 258 */
  out->rstsr1 = *ra_reset_rstsr1();
  /* HUM Ch 6.2.4 "RSTSR2 : Reset Status Register 2", p 261 */
  out->rstsr2 = *ra_reset_rstsr2();
  /* HUM Ch 6.2.5 "RSTSR3 : Reset Status Register 3", p 261 */
  out->rstsr3 = *ra_reset_rstsr3();
}

/**
 * @brief Decode raw RSTSRn words into a single primary cause.
 *
 * @details
 * Priority order: RSTSR0 (POR, LVD, DPSRSTF) -> RSTSR3 (core voltage,
 * overcurrent, temp) -> RSTSR1 (IWDT, WDT, SW, lockup, LM, BUS, CM,
 * WDT1, CLU1, LM1, NW) -> RSTSR2 cold/warm. POR wins over LVD wins
 * over watchdog, matching HUM Ch 6.1 Table 6.1 p 244 reset-source
 * dominance (every higher-priority reset clears the lower-priority
 * flag, so by the time firmware runs only the dominant cause is set).
 *
 * @param[in] raw  Raw register snapshot.
 * @return Decoded cause.
 */
/**
 * @brief Decode the RSTSR0 byte.
 *
 * @details
 * HUM Ch 6.2.2 "RSTSR0 : Reset Status Register 0" p 257. Returns the
 * highest-priority active flag, or ``k_ra_reset_cause_unknown`` if no
 * RSTSR0 flag is set.
 *
 * @param[in] rstsr0 Raw RSTSR0 byte snapshot.
 * @return Mapped reset cause for RSTSR0 flags, or
 *         ``k_ra_reset_cause_unknown`` when no flag is set.
 *
 * @pre ``rstsr0`` reflects the snapshot taken on entry to ``ra_reset_init``.
 * @post Returned value is one of the documented enumerator values.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_reset_cause_t internal_decode_rstsr0(uint8_t rstsr0)
{
  if ((rstsr0 & (uint8_t)k_ra_reset_rstsr0_porf_msk) != 0U) {
    return k_ra_reset_cause_power_on;
  }
  if ((rstsr0 & (uint8_t)k_ra_reset_rstsr0_lvd0rf_msk) != 0U) {
    return k_ra_reset_cause_lvd0;
  }
  if ((rstsr0 & (uint8_t)k_ra_reset_rstsr0_lvd1rf_msk) != 0U) {
    return k_ra_reset_cause_lvd1;
  }
  if ((rstsr0 & (uint8_t)k_ra_reset_rstsr0_lvd2rf_msk) != 0U) {
    return k_ra_reset_cause_lvd2;
  }
  if ((rstsr0 & (uint8_t)k_ra_reset_rstsr0_lvd4rf_msk) != 0U) {
    return k_ra_reset_cause_lvd4;
  }
  if ((rstsr0 & (uint8_t)k_ra_reset_rstsr0_lvd5rf_msk) != 0U) {
    return k_ra_reset_cause_lvd5;
  }
  if ((rstsr0 & (uint8_t)k_ra_reset_rstsr0_dpsrstf_msk) != 0U) {
    return k_ra_reset_cause_deep_sw_standby;
  }
  return k_ra_reset_cause_unknown;
}

/**
 * @brief Decode the RSTSR1 word.
 *
 * @details
 * HUM Ch 6.2.3 "RSTSR1 : Reset Status Register 1" p 258. Higher-priority
 * causes appear first.
 *
 * @param[in] rstsr1 Raw RSTSR1 word snapshot.
 * @return Mapped reset cause for RSTSR1 flags, or
 *         ``k_ra_reset_cause_unknown`` when no flag is set.
 *
 * @pre ``rstsr1`` reflects the snapshot taken on entry to ``ra_reset_init``.
 * @post Returned value is one of the documented enumerator values.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_reset_cause_t internal_decode_rstsr1(uint32_t rstsr1)
{
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_iwdtrf_msk) != 0U) {
    return k_ra_reset_cause_iwdt;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_wdtrf_msk) != 0U) {
    return k_ra_reset_cause_wdt0;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_swrf_msk) != 0U) {
    return k_ra_reset_cause_software;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_clurf_msk) != 0U) {
    return k_ra_reset_cause_lockup0;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_lm0rf_msk) != 0U) {
    return k_ra_reset_cause_local_memory0;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_bussrf_msk) != 0U) {
    return k_ra_reset_cause_bus_slave_mpu;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_cmrf_msk) != 0U) {
    return k_ra_reset_cause_common_memory;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_wdt1rf_msk) != 0U) {
    return k_ra_reset_cause_wdt1;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_clu1rf_msk) != 0U) {
    return k_ra_reset_cause_lockup1;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_lm1rf_msk) != 0U) {
    return k_ra_reset_cause_local_memory1;
  }
  if ((rstsr1 & (uint32_t)k_ra_reset_rstsr1_nwrf_msk) != 0U) {
    return k_ra_reset_cause_network;
  }
  return k_ra_reset_cause_unknown;
}

/**
 * @brief Decode the RSTSR3 byte.
 *
 * @details
 * HUM Ch 6.2.5 "RSTSR3 : Reset Status Register 3" p 261. Higher-priority
 * causes appear first.
 *
 * @param[in] rstsr3 Raw RSTSR3 byte snapshot.
 * @return Mapped reset cause for RSTSR3 flags, or
 *         ``k_ra_reset_cause_unknown`` when no flag is set.
 *
 * @pre ``rstsr3`` reflects the snapshot taken on entry to ``ra_reset_init``.
 * @post Returned value is one of the documented enumerator values.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_reset_cause_t internal_decode_rstsr3(uint8_t rstsr3)
{
  if ((rstsr3 & (uint8_t)k_ra_reset_rstsr3_cvmrf_msk) != 0U) {
    return k_ra_reset_cause_core_voltage;
  }
  if ((rstsr3 & (uint8_t)k_ra_reset_rstsr3_ocprf_msk) != 0U) {
    return k_ra_reset_cause_overcurrent;
  }
  if ((rstsr3 & (uint8_t)k_ra_reset_rstsr3_temprf_msk) != 0U) {
    return k_ra_reset_cause_temperature;
  }
  return k_ra_reset_cause_unknown;
}

static ra_reset_cause_t internal_decode(const ra_reset_raw_t* raw)
{
  ra_reset_cause_t cause = internal_decode_rstsr0(raw->rstsr0);
  if (cause != k_ra_reset_cause_unknown) {
    return cause;
  }

  cause = internal_decode_rstsr3(raw->rstsr3);
  if (cause != k_ra_reset_cause_unknown) {
    return cause;
  }

  cause = internal_decode_rstsr1(raw->rstsr1);
  if (cause != k_ra_reset_cause_unknown) {
    return cause;
  }

  /* HUM Ch 6.2.4 "RSTSR2 : Reset Status Register 2" p 261 */
  if ((raw->rstsr2 & (uint8_t)k_ra_reset_rstsr2_cwsf_msk) != 0U) {
    return k_ra_reset_cause_warm_start;
  }

  return k_ra_reset_cause_unknown;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra_err_t ra_reset_init(void)
{
  internal_read_raw(&s_state.raw);
  s_state.cause       = internal_decode(&s_state.raw);
  s_state.initialised = true;
  ra_log_info_val(s_tag, "boot cause", (uint32_t)s_state.cause);
  return k_ra_ok;
}

#ifdef RA_SIMULATOR_MODE
/**
 * @brief Drop the cached snapshot (host-test only).
 *
 * @details
 * Production firmware never tears the cached snapshot down -- the
 * boot cause is supposed to live for the lifetime of the program.
 * Unit tests, however, want every test case to start with the
 * driver in its just-loaded state, so this helper zeroes
 * ``s_state`` and is exposed only when ``RA_SIMULATOR_MODE`` is
 * defined (the test build sets this).
 *
 * @pre None.
 * @pre Caller is the host test harness (else this is a no-op
 *      anyway because the symbol does not exist).
 * @post ``s_state.initialised == false``.
 * @post Subsequent ``ra_reset_get_cause`` reads fresh from the
 *       sim-mmap registers.
 *
 * @note Thread safety: not thread-safe; tests are single-threaded.
 * @since 0.1.0
 */
void ra_reset_test_only_reset_state(void)
{
  s_state.initialised = false;
  s_state.cause       = k_ra_reset_cause_unknown;
  s_state.raw         = (ra_reset_raw_t){};
}
#endif /* RA_SIMULATOR_MODE */

ra_err_t ra_reset_get_cause(ra_reset_cause_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (s_state.initialised) {
    *out = s_state.cause;
    return k_ra_ok;
  }
  ra_reset_raw_t raw = {};
  internal_read_raw(&raw);
  *out = internal_decode(&raw);
  return k_ra_ok;
}

ra_err_t ra_reset_get_raw(ra_reset_raw_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (s_state.initialised) {
    *out = s_state.raw;
    return k_ra_ok;
  }
  internal_read_raw(out);
  return k_ra_ok;
}

ra_err_t ra_reset_clear_cause(uint32_t mask)
{
  /* HUM Ch 6.2.2 "RSTSR0 : Reset Status Register 0" p 258 */
  /* Note from HUM: only 0 can be written; bit must be read as 1 first. */
  const uint8_t rstsr0_clear = (uint8_t)(mask & (uint32_t)k_ra_reset_mask_rstsr0_msk);
  if (rstsr0_clear != 0U) {
    /* HUM Ch 6.2.2 "RSTSR0", p 257 */
    const uint8_t before = *ra_reset_rstsr0();
    *ra_reset_rstsr0()   = (uint8_t)(before & (uint8_t)~rstsr0_clear);
  }

  /* HUM Ch 6.2.3 "RSTSR1 : Reset Status Register 1" p 258 */
  /* Same write-0-to-clear semantics as RSTSR0. */
  const uint32_t rstsr1_clear =
    (mask & (uint32_t)k_ra_reset_mask_rstsr1_window) >> (uint32_t)k_ra_reset_mask_rstsr1_shift;
  if (rstsr1_clear != 0U) {
    /* HUM Ch 6.2.3 "RSTSR1", p 258 */
    const uint32_t before = *ra_reset_rstsr1();
    *ra_reset_rstsr1()    = before & ~rstsr1_clear;
  }

  if ((mask & (uint32_t)k_ra_reset_mask_rstsr2_cwsf) != 0U) {
    /* HUM Ch 6.2.4 "RSTSR2 : Reset Status Register 2" p 261 */
    /* CWSF is set by writing 1 (Note 2). */
    *ra_reset_rstsr2() = (uint8_t)k_ra_reset_rstsr2_cwsf_msk;
  }

  /* Refresh the cached cause if init was already called -- the next
   * reader should see the post-clear state, not the boot snapshot. */
  if (s_state.initialised) {
    internal_read_raw(&s_state.raw);
    s_state.cause = internal_decode(&s_state.raw);
  }
  return k_ra_ok;
}

ra_err_t ra_reset_get_attribution(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 6.2.1 "RSTSAR : Reset Security Attribution Register", p 256 */
  *out = *ra_reset_rstsar();
  return k_ra_ok;
}

void ra_reset_software_reset(void)
{
  /* HUM Ch 6.1 Table 6.1 p 244 -- "Software reset / Register setting
   * (use the software reset bit AIRCR.SYSRESETREQ)". The Cortex-M85
   * AIRCR is documented in the ARMv8-M Architecture Reference Manual;
   * we reproduce the constants in ra8d2_reset_regs.h so the driver has
   * a single include. */
  ra_log_info(s_tag, "software reset");

  const uint32_t aircr_value =
    ((uint32_t)k_ra_reset_aircr_vectkey_value << (uint32_t)k_ra_reset_aircr_vectkey_pos) |
    (uint32_t)k_ra_reset_aircr_sysresetreq_msk;

  /* HUM Ch 6.1 "Overview" p 244 */
  /* Table 6.1 names AIRCR.SYSRESETREQ as the software-reset trigger. */
  *ra_reset_aircr() = aircr_value;

#ifndef RA_SIMULATOR_MODE
  /* On real hardware the AIRCR write triggers a system reset within a
   * few cycles; the DSB ensures the write retires before the CPU is
   * gone. The infinite loop is belt-and-braces -- if the reset takes
   * longer than expected we don't want to fall through into whatever
   * follows the call site. */
  __asm__ volatile("dsb 0xF" ::: "memory");
  for (;;) {
    /* GCOVR_EXCL_LINE -- never reached on target. */
  }
#endif
  /* On host (RA_SIMULATOR_MODE) the function returns so unit tests can
   * inspect the AIRCR write. */
}
