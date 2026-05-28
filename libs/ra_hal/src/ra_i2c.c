/**
 * @file ra_i2c.c
 * @brief I2C Bus Interface (IIC) controller driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode controller driver for the RA8D2 RIIC peripheral
 * (channels IIC0/IIC1/IIC2). Mirrors the public init / start / write /
 * read / stop flow from FSP ``r_iic_master`` and HUM Ch 39.3 "Operation"
 * (p 2394-2410) collapsed into synchronous helpers -- no DTC fast path,
 * no interrupt path, no peripheral (responder) mode.
 *
 * The state machine implemented here is a synchronous reduction of the
 * interrupt-driven flow described in HUM Ch 39.3.3 / 39.3.4:
 *
 * @verbatim
 * Write (send_stop = true):
 *     IDLE --START--> ADDR_TX --TDRE--> DATA_TX --TEND--STOP--> IDLE
 *
 * Write (send_stop = false):
 *     IDLE --START--> ADDR_TX --TDRE--> DATA_TX --TEND--hold--> (chain)
 *
 * Read:
 *     IDLE --START--> ADDR_TX(R) --RDRF--> DATA_RX --NACK last byte-->
 *     STOP --> IDLE
 *
 * Combined transfer:
 *     IDLE --START--> ADDR_TX --DATA_TX--RESTART--> ADDR_TX(R) -->
 *     DATA_RX --NACK last byte--> STOP --> IDLE
 * @endverbatim
 *
 * Owns every write to the RIIC register block. See HUM Ch 39
 * "I2C Bus Interface (IIC)", p 2367-2470.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra_i2c.h"

#include <stdint.h>

#include "ra8d2_i2c_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_i2c_internal.h"
#include "ra_log.h"
#include "ra_mstp.h"

/** @brief Log tag for this driver. */
static const char* s_tag = "I2C";

/**
 * @enum ra_i2c_internal_t
 * @brief Implementation constants -- spin budgets and addressing helpers.
 */
typedef enum : uint32_t {
  /** Generic spin budget for status-flag polls. ~200k iterations keeps
   * the worst-case stall under a few ms at the slowest PCLKB. */
  k_ra_i2c_poll_limit = 200000U,
  /** Shift count to convert a 7-bit address into the on-the-wire byte. */
  k_ra_i2c_addr_shift = 1U,
  /** R/W bit value for a write transaction (0 in LSB). */
  k_ra_i2c_addr_rw_write = 0U,
  /** R/W bit value for a read transaction (1 in LSB). */
  k_ra_i2c_addr_rw_read = 1U,
} ra_i2c_internal_t;

/**
 * @enum ra_i2c_brate_t
 * @brief Bit-rate divider field limits (HUM Ch 39.2.15 / 39.2.16).
 */
typedef enum : uint32_t {
  /** ICBRL / ICBRH counter fields are 5 bits ([4:0]). */
  k_ra_i2c_br_field_max = 0x1FU,
  /** Upper 3 reserved bits of ICBRL / ICBRH read as 1. */
  k_ra_i2c_br_reserved_hi = 0xE0U,
  /** CKS divider exponent ceiling (CKS[2:0] selects PCLKB / 2^CKS). */
  k_ra_i2c_cks_max = 7U,
  /** Period split between SCL high and low halves. */
  k_ra_i2c_period_split = 2U,
} ra_i2c_brate_t;

/**
 * @brief Pure predicate: either clock argument is zero.
 *
 * @details Promoted so the OR decision can be driven under MC/DC.
 * @param[in] bus_hz   Target bus clock.
 * @param[in] pclkb_hz Reference PCLKB clock.
 * @return Boolean reject predicate.
 * @retval true  At least one clock is zero (invalid).
 * @retval false Both clocks are non-zero.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_i2c_internal_clk_invalid(uint32_t bus_hz, uint32_t pclkb_hz)
{
  return (bus_hz == 0U) || (pclkb_hz == 0U);
}

/**
 * @brief Pure predicate: ``index`` is the second-to-last byte of ``len``.
 *
 * @details Promoted so the AND decision (WAIT-arming on the
 * second-to-last receive byte) can be driven under MC/DC.
 * @param[in] index Zero-based byte index about to be read.
 * @param[in] len   Total bytes to read.
 * @return Boolean WAIT-arm predicate.
 * @retval true  ``index`` is the second-to-last byte (arm WAIT).
 * @retval false Otherwise.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_i2c_internal_is_wait_byte(uint32_t index, uint32_t len)
{
  return (len >= (uint32_t)k_ra_i2c_period_split) &&
         (index == (len - (uint32_t)k_ra_i2c_period_split));
}

/**
 * @struct ra_i2c_state_t
 * @brief Per-channel driver state.
 */
typedef struct {
  bool initialized; /**< Tracks ``ra_i2c_init`` / ``ra_i2c_deinit``.       */
  bool bus_held;    /**< True when the previous write returned with
                       ``send_stop=false`` so the next call must inject a
                       repeated-START instead of a fresh START.           */
} ra_i2c_state_t;

/**
 * @var s_i2c_state
 * @brief Per-channel state table indexed by channel.
 *
 * @warning Mutated only by the driver under the not-thread-safe contract.
 */
static ra_i2c_state_t s_i2c_state[k_ra_i2c_channel_count];

/**
 * @brief Map a channel index to its MSTP gate id.
 *
 * @details
 * IIC0 = MSTPB9, IIC1 = MSTPB8, IIC2 = MSTPB7 per HUM Ch 11.2.7
 * "MSTPCRB" p 444, encoded in ``ra8d2_mstp_regs.h``.
 *
 * @param[in] channel Channel index (already range-checked by caller).
 * @return The matching ``k_ra_mstp_iicN`` enum value.
 * @retval k_ra_mstp_iic0 ``channel`` is 0.
 * @retval k_ra_mstp_iic1 ``channel`` is 1.
 * @retval k_ra_mstp_iic2 ``channel`` is 2 (or any other value, defensively).
 *
 * @pre ``channel`` is 0, 1 or 2.
 * @pre Caller resolved a non-NULL register pointer for ``channel``.
 * @post Return value is one of the three IIC MSTP ids.
 * @post No global state is mutated.
 * @note Thread safety: pure mapping, no state.
 * @since 0.1.0
 */
static ra_mstp_t internal_i2c_mstp_id(uint8_t channel)
{
  if (channel == 0U) {
    return k_ra_mstp_iic0;
  }
  if (channel == 1U) {
    return k_ra_mstp_iic1;
  }
  return k_ra_mstp_iic2;
}

/**
 * @brief Pick the smallest CKS divider and divide ``*total`` to match.
 *
 * @details
 * Repeatedly halves the candidate bit-period cycle count until a single
 * SCL half-period fits inside the 5-bit ICBRL/ICBRH field, returning the
 * CKS exponent used. The loop is bounded by ``k_ra_i2c_cks_max + 1``
 * iterations (NASA P10 Rule 2).
 *
 * @param[in,out] total On entry the full ``PCLKB / bus_hz`` cycle count;
 *                      on return divided by ``2^CKS``.
 *
 * @return The chosen CKS exponent, clamped to ``[0, k_ra_i2c_cks_max]``.
 * @retval 0 The full period already fits the field.
 *
 * @pre total is non-NULL.
 * @pre ``*total`` was derived from non-zero clocks.
 * @post ``*total`` reflects the divided cycle count.
 * @post Return value is in ``[0, k_ra_i2c_cks_max]``.
 * @note Thread safety: pure on the supplied pointer; thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_i2c_pick_cks(uint32_t* total)
{
  uint32_t cks = 0U;
  for (uint32_t i = 0U; i <= (uint32_t)k_ra_i2c_cks_max; i++) { /* GCOVR_EXCL_BR_LINE */
    const uint32_t half = *total / (uint32_t)k_ra_i2c_period_split;
    if (half <= ((uint32_t)k_ra_i2c_br_field_max + 1U)) {
      break;
    }
    cks    = i + 1U;
    *total = *total >> 1U;
  }
  if (cks > (uint32_t)k_ra_i2c_cks_max) {
    cks = (uint32_t)k_ra_i2c_cks_max;
  }
  return (uint8_t)cks;
}

/**
 * @brief Clamp one SCL half-period count to the 5-bit BR field.
 *
 * @details
 * Splits ``total`` evenly between the SCL high and low halves, subtracts
 * the +1 the hardware adds, and clamps to ``k_ra_i2c_br_field_max``.
 *
 * @param[in] total Divided bit-period cycle count.
 *
 * @return The 5-bit half-period field value.
 * @retval 0 The divided period collapsed to a single cycle.
 *
 * @pre total is the output of ``internal_i2c_pick_cks``.
 * @pre None.
 * @post Return value is in ``[0, k_ra_i2c_br_field_max]``.
 * @post No state is mutated.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_i2c_clamp_half(uint32_t total)
{
  uint32_t half = total / (uint32_t)k_ra_i2c_period_split;
  if (half == 0U) {
    half = 1U;
  }
  uint32_t field = half - 1U;
  if (field > (uint32_t)k_ra_i2c_br_field_max) {
    field = (uint32_t)k_ra_i2c_br_field_max;
  }
  return (uint8_t)field;
}

/**
 * @brief Compute the CKS divider and ICBRH/ICBRL half-period counts.
 *
 * @details
 * The RIIC internal reference clock is ``IICphi = PCLKB / 2^CKS`` (HUM
 * Ch 39.2.3 p 2374). One SCL bit period is ``(BRH + 1) + (BRL + 1)``
 * IICphi cycles for the SCLE=0 / NFE=0 transfer-rate expression (HUM
 * Ch 39.2.16 expression (1) p 2392). Delegates the CKS search and field
 * clamp to ``internal_i2c_pick_cks`` / ``internal_i2c_clamp_half``.
 *
 * @param[in]  bus_hz   Target bus clock (non-zero).
 * @param[in]  pclkb_hz PCLKB frequency (non-zero).
 * @param[out] out_cks  CKS exponent [0..7].
 * @param[out] out_brh  ICBRH register value (5-bit count + reserved hi).
 * @param[out] out_brl  ICBRL register value (5-bit count + reserved hi).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Divider computed.
 * @retval k_ra_err_invalid_arg A clock argument was zero.
 *
 * @pre out_cks and out_brh are non-NULL.
 * @pre bus_hz and pclkb_hz are non-zero.
 * @post On success ``*out_cks <= 7`` and the BR fields are clamped.
 * @post On error no output is written.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_i2c_bitrate(uint32_t bus_hz,
                                     uint32_t pclkb_hz,
                                     uint8_t* out_cks,
                                     uint8_t* out_brh,
                                     uint8_t* out_brl)
{
  RA_CHECK_NULL_PTR(out_cks, s_tag, "bitrate: out_cks");
  RA_CHECK_NULL_PTR(out_brh, s_tag, "bitrate: out_brh");
  if (ra_i2c_internal_clk_invalid(bus_hz, pclkb_hz)) {
    return k_ra_err_invalid_arg;
  }

  /* Each transferred bit needs (BRH+1)+(BRL+1) IICphi cycles. Pick the
   * smallest CKS that fits both half-periods in the 5-bit field, then
   * derive the clamped half-period count for ICBRL / ICBRH. */
  uint32_t      total = pclkb_hz / bus_hz;
  const uint8_t cks   = internal_i2c_pick_cks(&total);
  const uint8_t field = internal_i2c_clamp_half(total);

  *out_cks = cks;
  *out_brh = (uint8_t)((uint32_t)k_ra_i2c_br_reserved_hi | (uint32_t)field);
  *out_brl = (uint8_t)((uint32_t)k_ra_i2c_br_reserved_hi | (uint32_t)field);
  return k_ra_ok;
}

/**
 * @brief Wait for a flag in ICSR2 to set, with a bounded spin budget.
 *
 * @details
 * Spins up to ``k_ra_i2c_poll_limit`` iterations reading ICSR2 and
 * returning success the moment the masked flag is observed set. The
 * fixed bound satisfies NASA P10 Rule 2.
 *
 * @param[in] reg  Channel register block.
 * @param[in] mask Bit mask to test against ICSR2.
 * @return ``ra_err_t`` outcome of the poll.
 * @retval k_ra_ok            Masked flag observed set.
 * @retval k_ra_err_hw_timeout Spin budget exhausted before the flag set.
 *
 * @pre reg is non-NULL.
 * @pre mask is a non-zero single- or multi-bit mask.
 * @post On success the masked flag was observed set.
 * @post On timeout no register write occurs.
 * @note Thread safety: not thread-safe (reads a single channel).
 * @since 0.1.0
 */
static ra_err_t internal_i2c_wait_icsr2(volatile const r_i2c_regs_t* reg, uint8_t mask)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra_i2c_poll_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
    if ((reg->ICSR2 & mask) != 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Map latched ICSR2 error bits to a high-level status code.
 *
 * @details
 * NACK and arbitration-loss carry distinct codes so a caller can
 * distinguish "peripheral declined" from "another controller won the
 * bus".
 *
 * @param[in] icsr2 Snapshot of ICSR2.
 * @return ``ra_err_t`` mapped from the highest-priority latched fault.
 * @retval k_ra_ok       No fault latched.
 * @retval k_ra_err_nack ICSR2.NACKF set.
 * @retval k_ra_err_hw_error ICSR2.AL set (arbitration lost).
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the input snapshot.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_i2c_status_from_icsr2(uint8_t icsr2)
{
  if ((icsr2 & (uint8_t)k_ra_i2c_msk_icsr2_nackf) != 0U) {
    return k_ra_err_nack;
  }
  if ((icsr2 & (uint8_t)k_ra_i2c_msk_icsr2_al) != 0U) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/**
 * @brief Decode latched ICSR2 error bits into a ``k_ra_i2c_err_*`` mask.
 *
 * @details
 * Tests the AL, NACKF and TMOF flags independently and ORs the matching
 * ``k_ra_i2c_err_*`` bit into the result so a caller sees every latched
 * fault in a single mask.
 *
 * @param[in] icsr2 Snapshot of ICSR2.
 * @return OR of ``k_ra_i2c_err_*`` bits.
 * @retval k_ra_i2c_err_none No fault bit set.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the input snapshot.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t internal_i2c_decode_errors(uint8_t icsr2)
{
  uint8_t mask = (uint8_t)k_ra_i2c_err_none;
  if ((icsr2 & (uint8_t)k_ra_i2c_msk_icsr2_al) != 0U) {
    mask |= (uint8_t)k_ra_i2c_err_arb_lost;
  }
  if ((icsr2 & (uint8_t)k_ra_i2c_msk_icsr2_nackf) != 0U) {
    mask |= (uint8_t)k_ra_i2c_err_nack;
  }
  /* TMOF lives in ICSR2 bit 0; reuse the timeout mask via the position. */
  if ((icsr2 & (uint8_t)(1U << (uint8_t)k_ra_i2c_icsr2_tmof_pos)) != 0U) {
    mask |= (uint8_t)k_ra_i2c_err_timeout;
  }
  return mask;
}

/**
 * @brief Clear the START / STOP / NACK status flags ahead of a transfer.
 *
 * @details
 * Writes 0 to the W0C condition-detect and fault flags (START, STOP,
 * NACKF, AL) so the next transaction observes fresh edges. TDRE / TEND /
 * RDRF are left untouched.
 *
 * @param[in] reg Channel register block.
 *
 * @pre reg is non-NULL.
 * @pre Channel previously initialized.
 * @post ICSR2.START / STOP / NACKF / AL read back zero.
 * @post ICSR2.TDRE / TEND / RDRF are preserved.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_clear_status(volatile r_i2c_regs_t* reg)
{
  enum : uint8_t {
    k_ra_i2c_status_clear_mask =
      (uint8_t)((1U << (uint8_t)k_ra_i2c_icsr2_start_pos) | (uint8_t)k_ra_i2c_msk_icsr2_stop |
                (uint8_t)k_ra_i2c_msk_icsr2_nackf | (uint8_t)k_ra_i2c_msk_icsr2_al),
  };
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2 -- W0C" p 2384 */
  reg->ICSR2 = (uint8_t)(reg->ICSR2 & (uint8_t)~(uint8_t)k_ra_i2c_status_clear_mask);
}

/**
 * @brief Issue a START condition (HUM Ch 39.3.3 step 2 p 2396).
 *
 * @details
 * Sets ICCR2.ST; the hardware issues the START once BBSY is clear and
 * automatically transitions to controller-transmit mode.
 *
 * @param[in] reg Channel register block.
 *
 * @pre reg is non-NULL and the bus is free.
 * @pre Channel previously initialized.
 * @post ICCR2.ST is set; hardware issues a START when BBSY clears.
 * @post No other register is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_start(volatile r_i2c_regs_t* reg)
{
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2" p 2371 */
  reg->ICCR2 = (uint8_t)(reg->ICCR2 | (uint8_t)k_ra_i2c_msk_iccr2_st);
}

/**
 * @brief Issue a repeated-START condition (HUM Ch 39.11 p 2434).
 *
 * @details
 * Sets ICCR2.RS; the hardware issues the restart while BBSY = 1 and
 * MST = 1, keeping the bus held without an intervening STOP.
 *
 * @param[in] reg Channel register block.
 *
 * @pre reg is non-NULL and the bus is held by this controller.
 * @pre Channel previously initialized.
 * @post ICCR2.RS is set; hardware issues a restart on the held bus.
 * @post No other register is modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_restart(volatile r_i2c_regs_t* reg)
{
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2" p 2371 */
  reg->ICCR2 = (uint8_t)(reg->ICCR2 | (uint8_t)k_ra_i2c_msk_iccr2_rs);
}

/**
 * @brief Issue a STOP condition and clear the STOP-detect flag.
 *
 * @details
 * Clears the prior ICSR2.STOP flag (W0C) so the next transaction sees a
 * fresh edge, then sets ICCR2.SP to request the STOP; the hardware
 * drives STOP and releases the bus.
 *
 * @param[in] reg Channel register block.
 *
 * @pre reg is non-NULL.
 * @pre Channel previously initialized.
 * @post ICCR2.SP is set; hardware issues a STOP and releases the bus.
 * @post ICSR2.STOP is cleared ahead of the new STOP edge.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_stop(volatile r_i2c_regs_t* reg)
{
  /* Clear the prior STOP-detect flag so the NEXT transaction sees a
   * fresh edge (W0C).
   * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  reg->ICSR2 = (uint8_t)(reg->ICSR2 & (uint8_t)~(uint8_t)k_ra_i2c_msk_icsr2_stop);
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2" p 2371 */
  reg->ICCR2 = (uint8_t)(reg->ICCR2 | (uint8_t)k_ra_i2c_msk_iccr2_sp);
}

/**
 * @brief Issue START or RESTART based on whether the bus is held.
 *
 * @details
 * Dispatches to ``internal_i2c_restart`` when the channel already holds
 * the bus from a prior ``send_stop = false`` write, otherwise issues a
 * fresh START.
 *
 * @param[in] reg      Channel register block.
 * @param[in] bus_held True to inject a repeated-START.
 *
 * @pre reg is non-NULL.
 * @pre Channel previously initialized.
 * @post Exactly one of ICCR2.ST / ICCR2.RS is requested.
 * @post No data register is touched.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_open_phase(volatile r_i2c_regs_t* reg, bool bus_held)
{
  if (bus_held) {
    internal_i2c_restart(reg);
  } else {
    internal_i2c_start(reg);
  }
}

/**
 * @brief Bus-busy gate: reject a fresh transaction while BBSY is set,
 *        unless the channel currently holds the bus for a restart.
 *
 * @details
 * A held bus (mid-RESTART) always proceeds; otherwise the gate reads
 * ICCR2.BBSY and accepts only when the bus is free.
 *
 * @param[in] reg      Channel register block.
 * @param[in] bus_held True when a prior call left the bus held.
 * @return ``k_ra_ok`` when a transaction may proceed, else
 *         ``k_ra_err_busy``.
 * @retval k_ra_ok       Bus is held or free.
 * @retval k_ra_err_busy Bus is busy and not held by this controller.
 *
 * @pre reg is non-NULL.
 * @pre Channel previously initialized.
 * @post No register write occurs.
 * @post No driver state is mutated.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_i2c_busy_gate(volatile const r_i2c_regs_t* reg, bool bus_held)
{
  if (bus_held) {
    return k_ra_ok;
  }
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2 -- BBSY" p 2371 */
  return ((reg->ICCR2 & (uint8_t)k_ra_i2c_msk_iccr2_bbsy) == 0U) ? k_ra_ok : k_ra_err_busy;
}

/**
 * @brief Transmit one address byte (HUM Ch 39.3.3 step 3 p 2396).
 *
 * @details
 * Waits for ICSR2.TDRE, writes the pre-shifted address byte to ICDRT,
 * then maps the freshly latched ICSR2 status so an immediate
 * address-phase NACK is reported.
 *
 * @param[in] reg          Channel register block.
 * @param[in] address_byte Pre-shifted 7-bit address with R/W bit.
 * @return ``k_ra_ok`` once TDRE re-arms, else timeout / NACK status.
 * @retval k_ra_ok            Address queued, no fault latched.
 * @retval k_ra_err_hw_timeout TDRE never set within the spin budget.
 * @retval k_ra_err_nack      ICSR2.NACKF latched after the write.
 * @retval k_ra_err_hw_error  ICSR2.AL latched (arbitration lost).
 *
 * @pre reg is non-NULL.
 * @pre A START / RESTART was issued before this call.
 * @post The address byte was written to ICDRT.
 * @post On NACK the latched ICSR2.NACKF is reflected in the return.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_i2c_send_address(volatile r_i2c_regs_t* reg, uint8_t address_byte)
{
  /* Wait for TDRE (set after START issues and TRS = transmit).
   * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  ra_err_t err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra_i2c_msk_icsr2_tdre);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 39.2.17 "ICDRT : I2C Bus Transmit Data Register" p 2393 */
  reg->ICDRT = address_byte;
  return internal_i2c_status_from_icsr2(reg->ICSR2);
}

/**
 * @brief Push ``len`` bytes from ``data`` into ICDRT.
 *
 * @details
 * Each iteration waits for ICSR2.TDRE, bails on NACK, then writes one
 * byte. Mirrors HUM Ch 39.3.3 step 4 p 2396.
 *
 * @param[in] reg  Channel register block.
 * @param[in] data Send buffer.
 * @param[in] len  Byte count.
 * @return ``k_ra_ok`` once every byte is queued, else timeout / NACK.
 * @retval k_ra_ok            All bytes queued.
 * @retval k_ra_err_hw_timeout TDRE never re-armed within the spin budget.
 * @retval k_ra_err_nack      ICSR2.NACKF latched mid-payload.
 *
 * @pre reg and data are non-NULL.
 * @pre The address byte was already acknowledged.
 * @post Either all bytes were queued or an error path stopped early.
 * @post No STOP is issued by this helper.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_i2c_drain_tx(volatile r_i2c_regs_t* reg, const uint8_t* data, uint32_t len)
{
  ra_err_t err = k_ra_ok;
  for (uint32_t i = 0U; i < len; i++) {
    err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra_i2c_msk_icsr2_tdre);
    if (err != k_ra_ok) {
      break;
    }
    /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2 -- NACKF" p 2384 */
    if ((reg->ICSR2 & (uint8_t)k_ra_i2c_msk_icsr2_nackf) != 0U) {
      err = k_ra_err_nack;
      break;
    }
    /* HUM Ch 39.2.17 "ICDRT : I2C Bus Transmit Data Register" p 2393 */
    reg->ICDRT = data[i];
  }
  return err;
}

/**
 * @brief Finish a write: wait for TEND, then STOP or hold the bus.
 *
 * @details
 * On a clean data phase the helper waits for ICSR2.TEND, then either
 * issues STOP and clears ``bus_held`` (``send_stop`` or any error) or
 * records ``bus_held`` so a chained call injects a RESTART.
 *
 * @param[in] reg       Channel register block.
 * @param[in] channel   Channel index.
 * @param[in] err       Result of the data phase.
 * @param[in] send_stop True to issue STOP, false to hold for a restart.
 * @return The propagated ``err`` (TEND timeout overrides ``k_ra_ok``).
 * @retval k_ra_ok            Data phase + TEND succeeded.
 * @retval k_ra_err_hw_timeout TEND never set within the spin budget.
 * @retval other              The incoming data-phase ``err`` is propagated.
 *
 * @pre reg is non-NULL and channel is in range.
 * @pre The data phase has completed (success or fault).
 * @post On STOP the bus is released; otherwise ``bus_held`` is recorded.
 * @post ``s_i2c_state[channel].bus_held`` reflects the new bus ownership.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t
internal_i2c_finish_tx(volatile r_i2c_regs_t* reg, uint8_t channel, ra_err_t err, bool send_stop)
{
  if (err == k_ra_ok) {
    /* Wait for TEND before issuing STOP (Master Transmit step 5).
     * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
    err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra_i2c_msk_icsr2_tend);
  }
  if ((err != k_ra_ok) || send_stop) {
    internal_i2c_stop(reg);
    internal_i2c_clear_status(reg);
    s_i2c_state[channel].bus_held = false;
  } else {
    s_i2c_state[channel].bus_held = true;
  }
  return err;
}

/* ra_i2c_write -- see header for full description. */
ra_err_t ra_i2c_write(uint8_t        channel,
                      uint8_t        peripheral_7b,
                      const uint8_t* data,
                      uint32_t       len,
                      bool           send_stop)
{
  volatile r_i2c_regs_t* reg = ra_i2c_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "i2c_write: channel");
  RA_CHECK_NULL_PTR(data, s_tag, "i2c_write: data");

  const bool     bus_held  = s_i2c_state[channel].bus_held;
  const ra_err_t busy_gate = internal_i2c_busy_gate(reg, bus_held);
  if (busy_gate != k_ra_ok) {
    return busy_gate;
  }

  internal_i2c_clear_status(reg);
  internal_i2c_open_phase(reg, bus_held);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)peripheral_7b << (uint32_t)k_ra_i2c_addr_shift) |
              (uint32_t)k_ra_i2c_addr_rw_write);
  ra_err_t err = internal_i2c_send_address(reg, address_byte);
  if (err != k_ra_ok) {
    internal_i2c_stop(reg);
    s_i2c_state[channel].bus_held = false;
    return err;
  }

  err = internal_i2c_drain_tx(reg, data, len);
  return internal_i2c_finish_tx(reg, channel, err, send_stop);
}

/**
 * @brief Arm NACK on the final byte and WAIT on the second-to-last byte.
 *
 * @details
 * HUM Ch 39.3.4 steps 4-6 p 2400: insert a WAIT before the
 * second-to-last byte and set ICMR3.ACKBT (NACK, paired with ACKWP)
 * before reading the last byte so the peripheral releases SDA at STOP.
 *
 * @param[in] reg   Channel register block.
 * @param[in] index Zero-based index of the byte about to be read.
 * @param[in] len   Total number of bytes to read.
 *
 * @pre reg is non-NULL and len is non-zero.
 * @pre A receive transaction is in progress.
 * @post For the last byte ICMR3.ACKBT is set under ACKWP.
 * @post For the second-to-last byte ICMR3.WAIT is armed.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_rx_ack_phase(volatile r_i2c_regs_t* reg, uint32_t index, uint32_t len)
{
  if (ra_i2c_internal_is_wait_byte(index, len)) {
    /* HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3 -- WAIT" p 2376 */
    reg->ICMR3 = (uint8_t)(reg->ICMR3 | (uint8_t)k_ra_i2c_msk_icmr3_wait);
  }
  if (index == (len - 1U)) {
    /* HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3 -- ACKBT" p 2376 */
    reg->ICMR3 =
      (uint8_t)(reg->ICMR3 | (uint8_t)k_ra_i2c_msk_icmr3_ackwp | (uint8_t)k_ra_i2c_msk_icmr3_ackbt);
  }
}

/**
 * @brief Drain ``len`` bytes from ICDRR into ``out``.
 *
 * @details
 * Mirrors HUM Ch 39.3.4 steps 4-7 p 2400. The first ICDRR read is a
 * dummy that starts the SCL clock; subsequent reads return payload once
 * ICSR2.RDRF sets. The last byte issues STOP before its ICDRR read.
 *
 * @param[in]  reg Channel register block.
 * @param[out] out Destination buffer.
 * @param[in]  len Byte count (non-zero).
 * @return ``k_ra_ok`` once all bytes drained, else timeout status.
 * @retval k_ra_ok            All bytes drained and STOP issued.
 * @retval k_ra_err_hw_timeout RDRF never set within the spin budget.
 *
 * @pre reg and out are non-NULL and len is non-zero.
 * @pre The read address byte was acknowledged.
 * @post STOP has been issued and ICMR3 WAIT/ACKBT defaults restored.
 * @post ICMR3.WAIT and ICMR3.ACKBT read back zero for the next transfer.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_i2c_drain_rx(volatile r_i2c_regs_t* reg, uint8_t* out, uint32_t len)
{
  /* Dummy-read ICDRR to start the SCL clock.
   * HUM Ch 39.3.4 "Master Receive Operation" p 2400 */
  ra_err_t err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra_i2c_msk_icsr2_rdrf);
  if (err == k_ra_ok) {
    /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
    (void)reg->ICDRR;
  }
  for (uint32_t i = 0U; (err == k_ra_ok) && (i < len); i++) {
    internal_i2c_rx_ack_phase(reg, i, len);
    err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra_i2c_msk_icsr2_rdrf);
    if (err != k_ra_ok) {
      break;
    }
    if (i == (len - 1U)) {
      internal_i2c_stop(reg);
    }
    /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
    out[i] = reg->ICDRR;
  }
  /* Restore ICMR3 defaults (no WAIT, ACK) for the next transaction.
   * HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3" p 2376 */
  reg->ICMR3 = (uint8_t)(reg->ICMR3 & (uint8_t)~(uint8_t)((uint8_t)k_ra_i2c_msk_icmr3_wait |
                                                          (uint8_t)k_ra_i2c_msk_icmr3_ackbt));
  return err;
}

/* ra_i2c_read -- see header for full description. */
ra_err_t ra_i2c_read(uint8_t channel, uint8_t peripheral_7b, uint8_t* data, uint32_t len)
{
  volatile r_i2c_regs_t* reg = ra_i2c_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "i2c_read: channel");
  RA_CHECK_NULL_PTR(data, s_tag, "i2c_read: data");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }

  const bool     bus_held  = s_i2c_state[channel].bus_held;
  const ra_err_t busy_gate = internal_i2c_busy_gate(reg, bus_held);
  if (busy_gate != k_ra_ok) {
    return busy_gate;
  }

  internal_i2c_clear_status(reg);
  internal_i2c_open_phase(reg, bus_held);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)peripheral_7b << (uint32_t)k_ra_i2c_addr_shift) |
              (uint32_t)k_ra_i2c_addr_rw_read);
  ra_err_t err = internal_i2c_send_address(reg, address_byte);
  if (err != k_ra_ok) {
    internal_i2c_stop(reg);
    s_i2c_state[channel].bus_held = false;
    return err;
  }

  err                       = internal_i2c_drain_rx(reg, data, len);
  const ra_err_t status_err = internal_i2c_status_from_icsr2(reg->ICSR2);
  if (status_err != k_ra_ok) {
    err = status_err;
  }
  internal_i2c_clear_status(reg);
  s_i2c_state[channel].bus_held = false;
  return err;
}

/* ra_i2c_transfer -- see header for full description. */
ra_err_t ra_i2c_transfer(uint8_t        channel,
                         uint8_t        peripheral_7b,
                         const uint8_t* wr,
                         uint32_t       wr_len,
                         uint8_t*       rd,
                         uint32_t       rd_len)
{
  if (ra_i2c_regs(channel) == nullptr) {
    return k_ra_err_null_ptr;
  }
  if ((wr_len == 0U) && (rd_len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  if ((wr_len != 0U) && (wr == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if ((rd_len != 0U) && (rd == nullptr)) {
    return k_ra_err_null_ptr;
  }

  /* Phase 1: write, holding the bus when a read phase follows. */
  if (wr_len != 0U) {
    const ra_err_t w_err =
      ra_i2c_write(channel, peripheral_7b, wr, wr_len, /*send_stop=*/(rd_len == 0U));
    if (w_err != k_ra_ok) {
      return w_err;
    }
  }

  /* Phase 2: read (issues STOP and releases the bus). */
  if (rd_len != 0U) {
    return ra_i2c_read(channel, peripheral_7b, rd, rd_len);
  }
  return k_ra_ok;
}

/* ra_i2c_scan -- see header for full description. */
ra_err_t ra_i2c_scan(uint8_t channel, uint8_t peripheral_7b, bool* out_acked)
{
  volatile r_i2c_regs_t* reg = ra_i2c_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "i2c_scan: channel");
  RA_CHECK_NULL_PTR(out_acked, s_tag, "i2c_scan: out_acked");

  *out_acked               = false;
  const ra_err_t busy_gate = internal_i2c_busy_gate(reg, s_i2c_state[channel].bus_held);
  if (busy_gate != k_ra_ok) {
    return busy_gate;
  }

  internal_i2c_clear_status(reg);
  internal_i2c_start(reg);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)peripheral_7b << (uint32_t)k_ra_i2c_addr_shift) |
              (uint32_t)k_ra_i2c_addr_rw_write);
  ra_err_t err = internal_i2c_send_address(reg, address_byte);
  /* Address NACK is a valid probe outcome, not a hard failure. */
  if ((err != k_ra_ok) && (err != k_ra_err_nack)) {
    internal_i2c_stop(reg);
    s_i2c_state[channel].bus_held = false;
    return err;
  }

  /* Wait for TEND (peripheral ACKed) or NACKF (no peripheral answered).
   * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  err = internal_i2c_wait_icsr2(
    reg,
    (uint8_t)((uint8_t)k_ra_i2c_msk_icsr2_tend | (uint8_t)k_ra_i2c_msk_icsr2_nackf));
  if (err == k_ra_ok) {
    *out_acked = (reg->ICSR2 & (uint8_t)k_ra_i2c_msk_icsr2_nackf) == 0U;
  }

  internal_i2c_stop(reg);
  internal_i2c_clear_status(reg);
  s_i2c_state[channel].bus_held = false;
  return err;
}

/* =============================================================================
 * Init / deinit -- mirrors HUM Ch 39.3.2 "Initial Settings" p 2395.
 * =============================================================================
 */

/**
 * @brief Apply the bring-up register sequence for an IIC channel.
 *
 * @details
 * Follows HUM Ch 39.3.2 p 2395: hold IIC reset (ICCR1.IICRST with
 * ICE = 0), enable internal reset (ICE = 1), program CKS / ICBRL /
 * ICBRH and the ICFER function bits, then release the reset. FMPE is
 * set for the Fast-mode Plus (>= 1 MHz) bus rate.
 *
 * @param[in] reg Channel register block.
 * @param[in] cks CKS divider exponent.
 * @param[in] brh ICBRH register value.
 * @param[in] brl ICBRL register value.
 * @param[in] fast_plus True when the bus runs at Fast-mode Plus.
 *
 * @pre reg is non-NULL.
 * @pre Channel MSTP gate already ungated.
 * @post ICCR1.ICE is set and the channel is out of reset.
 * @post ICMR1.CKS, ICBRL, ICBRH and ICFER hold the programmed values.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_apply_init_regs(volatile r_i2c_regs_t* reg,
                                         uint8_t                cks,
                                         uint8_t                brh,
                                         uint8_t                brl,
                                         bool                   fast_plus)
{
  /* Initial-settings step 1 (Figure 39.5): ICE = 0, pins inactive.
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = 0U;
  /* Initial-settings step 2: IICRST = 1 (IIC reset, ICE still 0).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)k_ra_i2c_msk_iccr1_iicrst;
  /* Initial-settings step 3: ICE = 1 (internal reset, pins active).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)((uint8_t)k_ra_i2c_msk_iccr1_iicrst | (uint8_t)k_ra_i2c_msk_iccr1_ice);

  /* HUM Ch 39.2.3 "ICMR1 : I2C Bus Mode Register 1 -- CKS[6:4]" p 2374 */
  reg->ICMR1 = (uint8_t)((uint32_t)cks << (uint32_t)k_ra_i2c_icmr1_cks_pos);
  /* HUM Ch 39.2.15 "ICBRL : I2C Bus Bit Rate Low-Level Register" p 2391 */
  reg->ICBRL = brl;
  /* HUM Ch 39.2.16 "ICBRH : I2C Bus Bit Rate High-Level Register" p 2392 */
  reg->ICBRH = brh;

  /* Enable arbitration-lost detection, NACK transfer suspension, the SCL
   * synchronous circuit, and (for Fm+) the slope-control circuit.
   * HUM Ch 39.2.6 "ICFER : I2C Bus Function Enable Register" p 2378 */
  uint8_t icfer = (uint8_t)((uint8_t)k_ra_i2c_msk_icfer_male | (uint8_t)k_ra_i2c_msk_icfer_nacke |
                            (uint8_t)k_ra_i2c_msk_icfer_scle);
  if (fast_plus) {
    icfer |= (uint8_t)k_ra_i2c_msk_icfer_fmpe;
  }
  reg->ICFER = icfer;

  /* Initial-settings step 5: release the internal reset (IICRST = 0).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)k_ra_i2c_msk_iccr1_ice;
}

/* ra_i2c_init -- see header for full description. */
ra_err_t ra_i2c_init(uint8_t channel, const ra_i2c_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "i2c_init: cfg");
  if (ra_i2c_internal_clk_invalid(cfg->bus_hz, cfg->pclkb_hz)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_i2c_regs_t* reg = ra_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }

  const ra_err_t mst_err = ra_mstp_enable(internal_i2c_mstp_id(channel));
  RA_RETURN_ON_ERROR(mst_err, s_tag, "i2c_init: mstp"); /* GCOVR_EXCL_BR_LINE */

  uint8_t        cks    = 0U;
  uint8_t        brh    = 0U;
  uint8_t        brl    = 0U;
  const ra_err_t br_err = internal_i2c_bitrate(cfg->bus_hz, cfg->pclkb_hz, &cks, &brh, &brl);
  RA_RETURN_ON_ERROR(br_err, s_tag, "i2c_init: bitrate"); /* GCOVR_EXCL_BR_LINE */

  internal_i2c_apply_init_regs(reg,
                               cks,
                               brh,
                               brl,
                               cfg->bus_hz >= (uint32_t)k_ra_i2c_speed_fast_plus);

  s_i2c_state[channel].initialized = true;
  s_i2c_state[channel].bus_held    = false;

  ra_log_info_val(s_tag, "i2c_init channel", (uint32_t)channel);
  return k_ra_ok;
}

/* ra_i2c_deinit -- see header for full description. */
ra_err_t ra_i2c_deinit(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* ICE = 0: place the SCL/SDA pins back in the inactive state.
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1                       = 0U;
  s_i2c_state[channel].initialized = false;
  s_i2c_state[channel].bus_held    = false;
  return ra_mstp_disable(internal_i2c_mstp_id(channel));
}

/* ra_i2c_set_clock -- see header for full description. */
ra_err_t ra_i2c_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclkb_hz)
{
  volatile r_i2c_regs_t* reg = ra_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  uint8_t        cks    = 0U;
  uint8_t        brh    = 0U;
  uint8_t        brl    = 0U;
  const ra_err_t br_err = internal_i2c_bitrate(bus_hz, pclkb_hz, &cks, &brh, &brl);
  if (br_err != k_ra_ok) {
    return br_err;
  }
  /* HUM Ch 39.2.3 "ICMR1 : I2C Bus Mode Register 1 -- CKS[6:4]" p 2374 */
  reg->ICMR1 = (uint8_t)(((uint32_t)reg->ICMR1 & ~((uint32_t)(uint8_t)k_ra_i2c_cks_max
                                                   << (uint32_t)k_ra_i2c_icmr1_cks_pos)) |
                         ((uint32_t)cks << (uint32_t)k_ra_i2c_icmr1_cks_pos));
  /* HUM Ch 39.2.15 "ICBRL : I2C Bus Bit Rate Low-Level Register" p 2391 */
  reg->ICBRL = brl;
  /* HUM Ch 39.2.16 "ICBRH : I2C Bus Bit Rate High-Level Register" p 2392 */
  reg->ICBRH = brh;
  return k_ra_ok;
}

/* =============================================================================
 * Status helpers.
 * =============================================================================
 */

/* ra_i2c_get_errors -- see header for full description. */
ra_err_t ra_i2c_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "i2c_get_errors: out_mask");
  volatile const r_i2c_regs_t* reg = ra_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  *out_mask = internal_i2c_decode_errors(reg->ICSR2);
  return k_ra_ok;
}

/* ra_i2c_clear_errors -- see header for full description. */
ra_err_t ra_i2c_clear_errors(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  enum : uint8_t {
    k_ra_i2c_err_clear_mask =
      (uint8_t)((uint8_t)k_ra_i2c_msk_icsr2_al | (uint8_t)k_ra_i2c_msk_icsr2_nackf |
                (uint8_t)(1U << (uint8_t)k_ra_i2c_icsr2_tmof_pos)),
  };
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2 -- W0C" p 2384 */
  reg->ICSR2 = (uint8_t)(reg->ICSR2 & (uint8_t)~(uint8_t)k_ra_i2c_err_clear_mask);
  return k_ra_ok;
}
