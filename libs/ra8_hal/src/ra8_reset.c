/**
 * @file ra8_reset.c
 * @brief Reset cause introspection + software-reset trigger
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implementation of the public API declared in ``ra8_reset.h``. Reads
 * RSTSR0/1/2/3 to decode the latched reset cause, mirrors the bits
 * out via ``ra8_reset_clear_cause``, surfaces RSTSAR for security
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

#include "ra8_reset.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_intrinsics.h"
#include "ra8_log.h"
#include "ra8_reset_regs.h"

/**
 * @var s_tag
 * @brief Logging tag for the reset driver.
 */
static const char* s_tag = "RESET";

/**
 * @enum ra8_reset_clear_layout_t
 * @brief Bit layout of the ``mask`` argument to ``ra8_reset_clear_cause``.
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
  k_ra8_reset_mask_rstsr0_msk    = 0x000000FFUL, /**< RSTSR0 selector window. */
  k_ra8_reset_mask_rstsr1_shift  = 8U,           /**< RSTSR1 mask <<8.        */
  k_ra8_reset_mask_rstsr1_window = 0x7FFFFF00UL, /**< RSTSR1 selector window. */
  k_ra8_reset_mask_rstsr2_cwsf   = 0x80000000UL, /**< Write 1 to RSTSR2.CWSF. */
} ra8_reset_clear_layout_t;

/* =============================================================================
 * Cached snapshot
 * =============================================================================
 */

/**
 * @struct ra8_reset_state_t
 * @brief File-scope cached snapshot of reset cause + raw registers.
 */
typedef struct {
  bool              initialized; /**< True once ``ra8_reset_init`` ran. */
  ra8_reset_cause_t cause;       /**< Decoded primary cause.            */
  ra8_reset_raw_t   raw;         /**< Raw register words at boot.       */
} ra8_reset_state_t;

/**
 * @var s_state
 * @brief File-scope snapshot. Zero-initialized by the C runtime.
 */
static ra8_reset_state_t s_state;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Read RSTSR0/1/2/3 into ``out``.
 *
 * @param[out] out Destination raw struct (caller-validated non-NULL).
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_read_raw(ra8_reset_raw_t* out)
{
  /* HUM Ch 6.2.2 "RSTSR0 : Reset Status Register 0", p 257 */
  out->rstsr0 = *ra8_reset_rstsr0();
  /* HUM Ch 6.2.3 "RSTSR1 : Reset Status Register 1", p 258 */
  out->rstsr1 = *ra8_reset_rstsr1();
  /* HUM Ch 6.2.4 "RSTSR2 : Reset Status Register 2", p 261 */
  out->rstsr2 = *ra8_reset_rstsr2();
  /* HUM Ch 6.2.5 "RSTSR3 : Reset Status Register 3", p 261 */
  out->rstsr3 = *ra8_reset_rstsr3();
}

/**
 * @brief Decode the RSTSR0 byte.
 *
 * @details
 * HUM Ch 6.2.2 "RSTSR0 : Reset Status Register 0" p 257. Returns the
 * highest-priority active flag, or ``k_ra8_reset_cause_unknown`` if no
 * RSTSR0 flag is set.
 *
 * @param[in] rstsr0 Raw RSTSR0 byte snapshot.
 * @return Mapped reset cause for RSTSR0 flags, or
 *         ``k_ra8_reset_cause_unknown`` when no flag is set.
 *
 * @pre ``rstsr0`` reflects the snapshot taken on entry to ``ra8_reset_init``.
 * @post Returned value is one of the documented enumerator values.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static ra8_reset_cause_t internal_decode_rstsr0(uint8_t rstsr0)
{
  if ((rstsr0 & k_ra8_reset_rstsr0_porf_msk) != 0U) {
    return k_ra8_reset_cause_power_on;
  }
  if ((rstsr0 & k_ra8_reset_rstsr0_lvd0rf_msk) != 0U) {
    return k_ra8_reset_cause_lvd0;
  }
  if ((rstsr0 & k_ra8_reset_rstsr0_lvd1rf_msk) != 0U) {
    return k_ra8_reset_cause_lvd1;
  }
  if ((rstsr0 & k_ra8_reset_rstsr0_lvd2rf_msk) != 0U) {
    return k_ra8_reset_cause_lvd2;
  }
  if ((rstsr0 & k_ra8_reset_rstsr0_lvd4rf_msk) != 0U) {
    return k_ra8_reset_cause_lvd4;
  }
  if ((rstsr0 & k_ra8_reset_rstsr0_lvd5rf_msk) != 0U) {
    return k_ra8_reset_cause_lvd5;
  }
  if ((rstsr0 & k_ra8_reset_rstsr0_dpsrstf_msk) != 0U) {
    return k_ra8_reset_cause_deep_sw_standby;
  }
  return k_ra8_reset_cause_unknown;
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
 *         ``k_ra8_reset_cause_unknown`` when no flag is set.
 *
 * @pre ``rstsr1`` reflects the snapshot taken on entry to ``ra8_reset_init``.
 * @post Returned value is one of the documented enumerator values.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static ra8_reset_cause_t internal_decode_rstsr1(uint32_t rstsr1)
{
  if ((rstsr1 & k_ra8_reset_rstsr1_iwdtrf_msk) != 0U) {
    return k_ra8_reset_cause_iwdt;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_wdtrf_msk) != 0U) {
    return k_ra8_reset_cause_wdt0;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_swrf_msk) != 0U) {
    return k_ra8_reset_cause_software;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_clurf_msk) != 0U) {
    return k_ra8_reset_cause_lockup0;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_lm0rf_msk) != 0U) {
    return k_ra8_reset_cause_local_memory0;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_bussrf_msk) != 0U) {
    return k_ra8_reset_cause_bus_peripheral_mpu;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_cmrf_msk) != 0U) {
    return k_ra8_reset_cause_common_memory;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_wdt1rf_msk) != 0U) {
    return k_ra8_reset_cause_wdt1;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_clu1rf_msk) != 0U) {
    return k_ra8_reset_cause_lockup1;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_lm1rf_msk) != 0U) {
    return k_ra8_reset_cause_local_memory1;
  }
  if ((rstsr1 & k_ra8_reset_rstsr1_nwrf_msk) != 0U) {
    return k_ra8_reset_cause_network;
  }
  return k_ra8_reset_cause_unknown;
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
 *         ``k_ra8_reset_cause_unknown`` when no flag is set.
 *
 * @pre ``rstsr3`` reflects the snapshot taken on entry to ``ra8_reset_init``.
 * @post Returned value is one of the documented enumerator values.
 *
 * @note Internal helper, not thread-safe.
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static ra8_reset_cause_t internal_decode_rstsr3(uint8_t rstsr3)
{
  if ((rstsr3 & k_ra8_reset_rstsr3_cvmrf_msk) != 0U) {
    return k_ra8_reset_cause_core_voltage;
  }
  if ((rstsr3 & k_ra8_reset_rstsr3_ocprf_msk) != 0U) {
    return k_ra8_reset_cause_overcurrent;
  }
  if ((rstsr3 & k_ra8_reset_rstsr3_temprf_msk) != 0U) {
    return k_ra8_reset_cause_temperature;
  }
  return k_ra8_reset_cause_unknown;
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
 * @return Decoded cause.
 */
static ra8_reset_cause_t internal_decode(const ra8_reset_raw_t* raw)
{
  ra8_reset_cause_t cause = internal_decode_rstsr0(raw->rstsr0);
  if (cause != k_ra8_reset_cause_unknown) {
    return cause;
  }

  cause = internal_decode_rstsr3(raw->rstsr3);
  if (cause != k_ra8_reset_cause_unknown) {
    return cause;
  }

  cause = internal_decode_rstsr1(raw->rstsr1);
  if (cause != k_ra8_reset_cause_unknown) {
    return cause;
  }

  /* HUM Ch 6.2.4 "RSTSR2 : Reset Status Register 2" p 261 */
  if ((raw->rstsr2 & k_ra8_reset_rstsr2_cwsf_msk) != 0U) {
    return k_ra8_reset_cause_warm_start;
  }

  return k_ra8_reset_cause_unknown;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_reset_init(void)
{
  internal_read_raw(&s_state.raw);
  s_state.cause       = internal_decode(&s_state.raw);
  s_state.initialized = true;
  ra8_log_info_val(s_tag, "boot cause", (uint32_t)s_state.cause);
  return k_ra8_ok;
}

void ra8_reset_test_only_reset_state(void)
{
  s_state.initialized = false;
  s_state.cause       = k_ra8_reset_cause_unknown;
  s_state.raw         = (ra8_reset_raw_t){};
}

ra8_err_t ra8_reset_get_cause(ra8_reset_cause_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (s_state.initialized) {
    *out = s_state.cause;
    return k_ra8_ok;
  }
  ra8_reset_raw_t raw = {};
  internal_read_raw(&raw);
  *out = internal_decode(&raw);
  return k_ra8_ok;
}

ra8_err_t ra8_reset_get_raw(ra8_reset_raw_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (s_state.initialized) {
    *out = s_state.raw;
    return k_ra8_ok;
  }
  internal_read_raw(out);
  return k_ra8_ok;
}

ra8_err_t ra8_reset_clear_cause(uint32_t mask)
{
  /* HUM Ch 6.2.2 "RSTSR0 : Reset Status Register 0" p 258 */
  /* Note from HUM: only 0 can be written; bit must be read as 1 first. */
  const uint8_t rstsr0_clear = (uint8_t)(mask & k_ra8_reset_mask_rstsr0_msk);
  if (rstsr0_clear != 0U) {
    /* HUM Ch 6.2.2 "RSTSR0", p 257 */
    const uint8_t before = *ra8_reset_rstsr0();
    *ra8_reset_rstsr0()  = (uint8_t)(before & (uint8_t)~rstsr0_clear);
  }

  /* HUM Ch 6.2.3 "RSTSR1 : Reset Status Register 1" p 258 */
  /* Same write-0-to-clear semantics as RSTSR0. */
  const uint32_t rstsr1_clear =
    (mask & k_ra8_reset_mask_rstsr1_window) >> k_ra8_reset_mask_rstsr1_shift;
  if (rstsr1_clear != 0U) {
    /* HUM Ch 6.2.3 "RSTSR1", p 258 */
    const uint32_t before = *ra8_reset_rstsr1();
    *ra8_reset_rstsr1()   = before & ~rstsr1_clear;
  }

  if ((mask & k_ra8_reset_mask_rstsr2_cwsf) != 0U) {
    /* HUM Ch 6.2.4 "RSTSR2 : Reset Status Register 2" p 261 */
    /* CWSF is set by writing 1 (Note 2). */
    *ra8_reset_rstsr2() = k_ra8_reset_rstsr2_cwsf_msk;
  }

  /* Refresh the cached cause if init was already called -- the next
   * reader should see the post-clear state, not the boot snapshot. */
  if (s_state.initialized) {
    internal_read_raw(&s_state.raw);
    s_state.cause = internal_decode(&s_state.raw);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_reset_get_attribution(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 6.2.1 "RSTSAR : Reset Security Attribution Register", p 256 */
  *out = *ra8_reset_rstsar();
  return k_ra8_ok;
}

void ra8_reset_software_reset(void)
{
  /* HUM Ch 6.1 Table 6.1 p 244 -- "Software reset / Register setting
   * (use the software reset bit AIRCR.SYSRESETREQ)". The Cortex-M85
   * AIRCR is documented in the ARMv8-M Architecture Reference Manual;
   * we reproduce the constants in ra8_reset_regs.h so the driver has
   * a single include. */
  ra8_log_info(s_tag, "software reset");

  const uint32_t aircr_value = (k_ra8_reset_aircr_vectkey_value << k_ra8_reset_aircr_vectkey_pos) |
                               k_ra8_reset_aircr_sysresetreq_msk;

  /* HUM Ch 6.1 "Overview" p 244 */
  /* Table 6.1 names AIRCR.SYSRESETREQ as the software-reset trigger. */
  *ra8_reset_aircr() = aircr_value;

  /* On real hardware the AIRCR write triggers a system reset within a few
   * cycles; ::ra8_hw_wait_for_reset issues a DSB so the write retires, then
   * spins forever so control never falls through into the caller's tail. On
   * the host the seam returns at once so a unit test can inspect the AIRCR
   * write it just staged. */
  ra8_hw_wait_for_reset();
}

/* =============================================================================
 * Reset-source masking (SYRSTMSK0/1/2) internals
 * =============================================================================
 */

/**
 * @brief Resolve a reset source to its SYRSTMSKn register + bit mask.
 *
 * @details
 * Maps each ``ra8_reset_source_t`` to the volatile pointer of the owning
 * mask register (SYRSTMSK0/1/2) and the bit mask within it. The accessor
 * calls compute addresses only -- no register is dereferenced here, so no
 * HUM citation is required until the caller reads/writes ``*reg``.
 *
 * @param[in]  source Reset source to resolve.
 * @param[out] reg    Receives the owning SYRSTMSKn register pointer.
 * @param[out] mask   Receives the bit mask within that register.
 *
 * @return ``true`` if @p source is valid and resolved; ``false`` otherwise.
 * @retval true  @p source resolved; ``*reg`` and ``*mask`` are set.
 * @retval false @p source out of range; ``*reg`` / ``*mask`` untouched.
 *
 * @pre @p reg and @p mask are non-NULL (caller-provided locals).
 * @pre @p source is a ``ra8_reset_source_t`` value.
 * @post On ``true``, ``*reg`` and ``*mask`` identify the mask bit.
 * @post On ``false``, ``*reg`` / ``*mask`` are left unmodified.
 * @note Not thread-safe; pure address resolution.
 * @since 0.1.0
 */
static bool internal_source_loc(ra8_reset_source_t source, volatile uint8_t** reg, uint8_t* mask)
{
  switch (source) {
    case k_ra8_reset_source_iwdt:
      *reg  = ra8_reset_syrstmsk0();
      *mask = k_ra8_reset_syrstmsk0_iwdt_msk;
      return true;
    case k_ra8_reset_source_wdt0:
      *reg  = ra8_reset_syrstmsk0();
      *mask = k_ra8_reset_syrstmsk0_wdt0_msk;
      return true;
    case k_ra8_reset_source_sw:
      *reg  = ra8_reset_syrstmsk0();
      *mask = k_ra8_reset_syrstmsk0_sw_msk;
      return true;
    case k_ra8_reset_source_clu0:
      *reg  = ra8_reset_syrstmsk0();
      *mask = k_ra8_reset_syrstmsk0_clu0_msk;
      return true;
    case k_ra8_reset_source_lm0:
      *reg  = ra8_reset_syrstmsk0();
      *mask = k_ra8_reset_syrstmsk0_lm0_msk;
      return true;
    case k_ra8_reset_source_cm:
      *reg  = ra8_reset_syrstmsk0();
      *mask = k_ra8_reset_syrstmsk0_cm_msk;
      return true;
    case k_ra8_reset_source_bus:
      *reg  = ra8_reset_syrstmsk0();
      *mask = k_ra8_reset_syrstmsk0_bus_msk;
      return true;
    case k_ra8_reset_source_wdt1:
      *reg  = ra8_reset_syrstmsk1();
      *mask = k_ra8_reset_syrstmsk1_wdt1_msk;
      return true;
    case k_ra8_reset_source_clu1:
      *reg  = ra8_reset_syrstmsk1();
      *mask = k_ra8_reset_syrstmsk1_clu1_msk;
      return true;
    case k_ra8_reset_source_lm1:
      *reg  = ra8_reset_syrstmsk1();
      *mask = k_ra8_reset_syrstmsk1_lm1_msk;
      return true;
    case k_ra8_reset_source_pvd1:
      *reg  = ra8_reset_syrstmsk2();
      *mask = k_ra8_reset_syrstmsk2_pvd1_msk;
      return true;
    case k_ra8_reset_source_pvd2:
      *reg  = ra8_reset_syrstmsk2();
      *mask = k_ra8_reset_syrstmsk2_pvd2_msk;
      return true;
    case k_ra8_reset_source_count:
    default:
      return false;
  }
}

ra8_err_t ra8_reset_set_source_mask(ra8_reset_source_t source, bool disable)
{
  volatile uint8_t* reg  = nullptr;
  uint8_t           mask = 0U;
  if (!internal_source_loc(source, &reg, &mask)) {
    ra8_log_error(s_tag, "set_source_mask: invalid reset source");
    return k_ra8_err_invalid_arg;
  }

  volatile uint16_t* prcr = ra8_reset_prcr();
  /* Set PRCR.PRC5 to 1 (write enabled) before rewriting the reset-control
   * group; preserve the other PRC bits. */
  /* HUM Ch 6.2.6 "SYRSTMSK0" p 263 */
  const uint16_t prcr_cur = *prcr;
  *prcr = (uint16_t)((uint16_t)k_ra8_reset_prcr_key |
                     (uint16_t)(prcr_cur & (uint16_t)k_ra8_reset_prcr_pr_bits_msk) |
                     (uint16_t)k_ra8_reset_prcr_prc5_msk);

  /* 1 disables the corresponding reset, 0 enables it. */
  /* HUM Ch 6.2.6 "System Reset Mask Control" p 262 */
  const uint8_t before = *reg;
  *reg                 = disable ? (uint8_t)(before | mask) : (uint8_t)(before & (uint8_t)~mask);

  /* Relock PRCR.PRC5. */
  /* HUM Ch 6.2.6 "SYRSTMSK0" p 263 */
  const uint16_t prcr_now = *prcr;
  *prcr = (uint16_t)((uint16_t)k_ra8_reset_prcr_key |
                     (uint16_t)((prcr_now & (uint16_t)k_ra8_reset_prcr_pr_bits_msk) &
                                (uint16_t)~(uint16_t)k_ra8_reset_prcr_prc5_msk));
  return k_ra8_ok;
}

ra8_err_t ra8_reset_get_source_mask(ra8_reset_source_t source, bool* disabled)
{
  RA8_CHECK_NULL_PTR(disabled, s_tag, "disabled must not be nullptr");

  volatile uint8_t* reg  = nullptr;
  uint8_t           mask = 0U;
  if (!internal_source_loc(source, &reg, &mask)) {
    ra8_log_error(s_tag, "get_source_mask: invalid reset source");
    return k_ra8_err_invalid_arg;
  }

  /* Reads do not require a PRCR unlock. */
  /* HUM Ch 6.2.6 "System Reset Mask Control" p 262 */
  *disabled = ((*reg & mask) != 0U);
  return k_ra8_ok;
}
