/**
 * @file ra8_i2c.c
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

#include "ra8_i2c.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_i2c_internal.h"
#include "ra8_i2c_regs.h"

/**
 * @var s_i2c_tag
 * @brief Log tag for this driver, shared with ``ra8_i2c_config.c``.
 *
 * @details
 * Owning definition for the ``s_i2c_tag`` symbol declared ``extern`` in
 * ``ra8_i2c_internal.h``; both I2C translation units log under "I2C".
 *
 * @note Read-only string pointer; not mutated after static init.
 *
 * @since 0.1.0
 */
const char* const s_i2c_tag = "I2C";

/**
 * @enum ra8_i2c_internal_t
 * @brief Implementation constants -- spin budgets and addressing helpers.
 */
typedef enum : uint32_t {
  /** Generic spin budget for status-flag polls. ~200k iterations keeps
   * the worst-case stall under a few ms at the slowest PCLKB. */
  k_ra8_i2c_poll_limit = 200000U,
  /** Shift count to convert a 7-bit address into the on-the-wire byte. */
  k_ra8_i2c_addr_shift = 1U,
  /** R/W bit value for a write transaction (0 in LSB). */
  k_ra8_i2c_addr_rw_write = 0U,
  /** R/W bit value for a read transaction (1 in LSB). */
  k_ra8_i2c_addr_rw_read = 1U,
} ra8_i2c_internal_t;

/**
 * @enum ra8_i2c_rx_phase_t
 * @brief Controller-receive end-of-frame boundaries (HUM Ch 39.3.4 p 2400).
 *
 * @details Drive the WAIT / NACK / STOP arming as the byte countdown
 * approaches the final byte, mirroring the FSP r_iic_master RXI handler.
 */
typedef enum : uint32_t {
  k_ra8_i2c_rx_short_len   = 2U, /**< len <= this: arm WAIT in the dummy phase. */
  k_ra8_i2c_rx_single_len  = 1U, /**< len == this: NACK in the dummy phase.     */
  k_ra8_i2c_rx_remain_wait = 3U, /**< bytes-remaining == this: arm WAIT.        */
  k_ra8_i2c_rx_remain_nack = 2U, /**< bytes-remaining == this: NACK final byte. */
  k_ra8_i2c_rx_remain_stop = 1U, /**< bytes-remaining == this: request STOP.    */
} ra8_i2c_rx_phase_t;

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
bool ra8_i2c_internal_clk_invalid(uint32_t bus_hz, uint32_t pclkb_hz)
{
  return (bus_hz == 0U) || (pclkb_hz == 0U);
}

/**
 * @var s_i2c_state
 * @brief Per-channel state table indexed by channel.
 *
 * @details
 * Owning definition for the ``s_i2c_state`` array declared ``extern`` in
 * ``ra8_i2c_internal.h`` so the bring-up plane in ``ra8_i2c_config.c`` and
 * the transfer plane here observe identical bus-ownership state.
 *
 * @warning Mutated only by the driver under the not-thread-safe contract.
 *
 * @since 0.1.0
 */
ra8_i2c_state_t s_i2c_state[k_ra8_i2c_channel_count];

/**
 * @brief Wait for a flag in ICSR2 to set, with a bounded spin budget.
 *
 * @details
 * Spins up to ``k_ra8_i2c_poll_limit`` iterations reading ICSR2 and
 * returning success the moment the masked flag is observed set. The
 * fixed bound satisfies NASA P10 Rule 2.
 *
 * @param[in] reg  Channel register block.
 * @param[in] mask Bit mask to test against ICSR2.
 * @return ``ra8_err_t`` outcome of the poll.
 * @retval k_ra8_ok            Masked flag observed set.
 * @retval k_ra8_err_hw_timeout Spin budget exhausted before the flag set.
 *
 * @pre reg is non-NULL.
 * @pre mask is a non-zero single- or multi-bit mask.
 * @post On success the masked flag was observed set.
 * @post On timeout no register write occurs.
 * @note Thread safety: not thread-safe (reads a single channel).
 * @since 0.1.0
 */
static ra8_err_t internal_i2c_wait_icsr2(volatile const r_i2c_regs_t* reg, uint8_t mask)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_i2c_poll_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_poll(&reg->ICSR2, i, (reg->ICSR2 & mask) != 0U)) { /* GCOVR_EXCL_BR_LINE */
#else
    if ((reg->ICSR2 & mask) != 0U) { /* GCOVR_EXCL_BR_LINE */
#endif
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
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
 * @return ``ra8_err_t`` mapped from the highest-priority latched fault.
 * @retval k_ra8_ok       No fault latched.
 * @retval k_ra8_err_nack ICSR2.NACKF set.
 * @retval k_ra8_err_hw_error ICSR2.AL set (arbitration lost).
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on the input snapshot.
 * @note Thread safety: pure; thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_i2c_status_from_icsr2(uint8_t icsr2)
{
  if ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_nackf) != 0U) {
    return k_ra8_err_nack;
  }
  if ((icsr2 & (uint8_t)k_ra8_i2c_msk_icsr2_al) != 0U) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
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
    k_ra8_i2c_status_clear_mask =
      (uint8_t)((1U << (uint8_t)k_ra8_i2c_icsr2_start_pos) | (uint8_t)k_ra8_i2c_msk_icsr2_stop |
                (uint8_t)k_ra8_i2c_msk_icsr2_nackf |
                (uint8_t)k_ra8_i2c_msk_icsr2_al), /**< RA8 I2C status clear mask. */
  };
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2 -- W0C" p 2384 */
  reg->ICSR2 = (uint8_t)(reg->ICSR2 & (uint8_t) ~(uint8_t)k_ra8_i2c_status_clear_mask);
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
  reg->ICCR2 = (uint8_t)(reg->ICCR2 | (uint8_t)k_ra8_i2c_msk_iccr2_st);
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
  reg->ICCR2 = (uint8_t)(reg->ICCR2 | (uint8_t)k_ra8_i2c_msk_iccr2_rs);
  /* RS auto-clears once the restart condition is issued. The peripheral
   * address must be written to ICDRT only after RS reads 0 -- a write
   * while RS = 1 is silently dropped (HUM Ch 39.11 "Issuing a Restart
   * Condition" Note, p 2434). After a send_stop=false write TDRE is
   * already 1, so without this wait send_address would write the address
   * before the restart completed and the transmit would never happen. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_i2c_poll_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_rs) == 0U) {    /* GCOVR_EXCL_BR_LINE */
      break;
    }
  }
}

/**
 * @brief Spin (bounded) until the bus is free (ICCR2.BBSY clears).
 *
 * @details
 * BBSY -- not the ICSR2.STOP flag -- is what the next transaction's busy
 * gate checks, and it clears a few cycles after the STOP edge. Waiting on
 * it before returning keeps a following START from racing a busy bus
 * (k_ra8_err_busy).
 *
 * @param[in] reg Channel register block.
 * @pre reg is non-NULL.
 * @pre A STOP has been requested (or the bus is otherwise idle).
 * @post BBSY is observed clear, or the bounded poll expired.
 * @post No register is modified (read-only spin).
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_wait_bus_free(volatile const r_i2c_regs_t* reg)
{
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2 -- BBSY" p 2371 */
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_i2c_poll_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_bbsy) == 0U) {  /* GCOVR_EXCL_BR_LINE */
      break;
    }
  }
}

/**
 * @brief Request a STOP condition without waiting for it to complete.
 *
 * @details
 * Clears the prior ICSR2.STOP flag (W0C) so the next transaction sees a
 * fresh edge, then sets ICCR2.SP to request the STOP. The receive path
 * must use this form: during a controller read the STOP only actually fires
 * after the final ICDRR read and the WAIT clear (HUM Ch 39.3.4 step 7),
 * so waiting for BBSY here would deadlock before the last byte is read.
 *
 * @param[in] reg Channel register block.
 * @pre reg is non-NULL.
 * @pre Channel previously initialized.
 * @post ICCR2.SP is set; the STOP fires once preconditions are met.
 * @post ICSR2.STOP is cleared so the next STOP edge is detectable.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_stop_request(volatile r_i2c_regs_t* reg)
{
  /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  reg->ICSR2 = (uint8_t)(reg->ICSR2 & (uint8_t) ~(uint8_t)k_ra8_i2c_msk_icsr2_stop);
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2" p 2371 */
  reg->ICCR2 = (uint8_t)(reg->ICCR2 | (uint8_t)k_ra8_i2c_msk_iccr2_sp);
}

/**
 * @brief Issue a STOP condition and wait for the bus to be released.
 *
 * @details
 * Requests the STOP then spins until BBSY clears. Used by the transmit
 * path, where the STOP fires immediately after TEND; returning before
 * BBSY clears lets the next START race a busy bus and fail with
 * k_ra8_err_busy.
 *
 * @param[in] reg Channel register block.
 * @pre reg is non-NULL.
 * @pre Channel previously initialized.
 * @post Hardware has issued a STOP and BBSY is observed clear.
 * @post The bus is idle and ready for the next START.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_stop(volatile r_i2c_regs_t* reg)
{
  internal_i2c_stop_request(reg);
  internal_i2c_wait_bus_free(reg);
}

/**
 * @brief Set ICMR3.ACKBT (transmit NACK) under ACKWP write-enable.
 *
 * @details
 * ACKBT is write-protected by ACKWP; per HUM Ch 39.2.5 Note 1 the
 * write-enable, the ACKBT set, and the write-disable must be separate
 * register writes. Used by the receive path to NACK the final byte so
 * the peripheral stops driving the bus.
 *
 * @param[in] reg Channel register block.
 * @pre reg is non-NULL.
 * @pre The controller is in controller-receive.
 * @post ICMR3.ACKBT is set; ACKWP is left clear (write-protected again).
 * @post The next received byte will be answered with a NACK.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void internal_i2c_set_nack(volatile r_i2c_regs_t* reg)
{
  /* HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3 -- ACKWP/ACKBT" p 2376 */
  reg->ICMR3 = (uint8_t)(reg->ICMR3 | (uint8_t)k_ra8_i2c_msk_icmr3_ackwp);
  reg->ICMR3 = (uint8_t)(reg->ICMR3 | (uint8_t)k_ra8_i2c_msk_icmr3_ackbt);
  reg->ICMR3 = (uint8_t)(reg->ICMR3 & (uint8_t) ~(uint8_t)k_ra8_i2c_msk_icmr3_ackwp);
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
 * @return ``k_ra8_ok`` when a transaction may proceed, else
 *         ``k_ra8_err_busy``.
 * @retval k_ra8_ok       Bus is held or free.
 * @retval k_ra8_err_busy Bus is busy and not held by this controller.
 *
 * @pre reg is non-NULL.
 * @pre Channel previously initialized.
 * @post No register write occurs.
 * @post No driver state is mutated.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_i2c_busy_gate(volatile const r_i2c_regs_t* reg, bool bus_held)
{
  if (bus_held) {
    return k_ra8_ok;
  }
  /* HUM Ch 39.2.2 "ICCR2 : I2C Bus Control Register 2 -- BBSY" p 2371 */
  return ((reg->ICCR2 & (uint8_t)k_ra8_i2c_msk_iccr2_bbsy) == 0U) ? k_ra8_ok : k_ra8_err_busy;
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
 * @return ``k_ra8_ok`` once TDRE re-arms, else timeout / NACK status.
 * @retval k_ra8_ok            Address queued, no fault latched.
 * @retval k_ra8_err_hw_timeout TDRE never set within the spin budget.
 * @retval k_ra8_err_nack      ICSR2.NACKF latched after the write.
 * @retval k_ra8_err_hw_error  ICSR2.AL latched (arbitration lost).
 *
 * @pre reg is non-NULL.
 * @pre A START / RESTART was issued before this call.
 * @post The address byte was written to ICDRT.
 * @post On NACK the latched ICSR2.NACKF is reflected in the return.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_i2c_send_address(volatile r_i2c_regs_t* reg, uint8_t address_byte)
{
  /* Wait for TDRE (set after START issues and TRS = transmit).
   * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  ra8_err_t err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra8_i2c_msk_icsr2_tdre);
  if (err != k_ra8_ok) {
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
 * @return ``k_ra8_ok`` once every byte is queued, else timeout / NACK.
 * @retval k_ra8_ok            All bytes queued.
 * @retval k_ra8_err_hw_timeout TDRE never re-armed within the spin budget.
 * @retval k_ra8_err_nack      ICSR2.NACKF latched mid-payload.
 *
 * @pre reg and data are non-NULL.
 * @pre The address byte was already acknowledged.
 * @post Either all bytes were queued or an error path stopped early.
 * @post No STOP is issued by this helper.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
internal_i2c_drain_tx(volatile r_i2c_regs_t* reg, const uint8_t* data, uint32_t len)
{
  ra8_err_t err = k_ra8_ok;
  for (uint32_t i = 0U; i < len; i++) {
    err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra8_i2c_msk_icsr2_tdre);
    if (err != k_ra8_ok) {
      break;
    }
    /* HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2 -- NACKF" p 2384 */
    if ((reg->ICSR2 & (uint8_t)k_ra8_i2c_msk_icsr2_nackf) != 0U) {
      err = k_ra8_err_nack;
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
 * @return The propagated ``err`` (TEND timeout overrides ``k_ra8_ok``).
 * @retval k_ra8_ok            Data phase + TEND succeeded.
 * @retval k_ra8_err_hw_timeout TEND never set within the spin budget.
 * @retval other              The incoming data-phase ``err`` is propagated.
 *
 * @pre reg is non-NULL and channel is in range.
 * @pre The data phase has completed (success or fault).
 * @post On STOP the bus is released; otherwise ``bus_held`` is recorded.
 * @post ``s_i2c_state[channel].bus_held`` reflects the new bus ownership.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
internal_i2c_finish_tx(volatile r_i2c_regs_t* reg, uint8_t channel, ra8_err_t err, bool send_stop)
{
  if (err == k_ra8_ok) {
    /* Wait for TEND before issuing STOP (Controller Transmit step 5).
     * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
    err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra8_i2c_msk_icsr2_tend);
  }
  if ((err != k_ra8_ok) || send_stop) {
    internal_i2c_stop(reg);
    internal_i2c_clear_status(reg);
    s_i2c_state[channel].bus_held = false;
  } else {
    s_i2c_state[channel].bus_held = true;
  }
  return err;
}

ra8_err_t ra8_i2c_write(uint8_t        channel,
                        uint8_t        peripheral_7b,
                        const uint8_t* data,
                        uint32_t       len,
                        bool           send_stop)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_i2c_tag, "i2c_write: channel");
  RA8_CHECK_NULL_PTR(data, s_i2c_tag, "i2c_write: data");

  const bool      bus_held  = s_i2c_state[channel].bus_held;
  const ra8_err_t busy_gate = internal_i2c_busy_gate(reg, bus_held);
  if (busy_gate != k_ra8_ok) {
    return busy_gate;
  }

  internal_i2c_clear_status(reg);
  internal_i2c_open_phase(reg, bus_held);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)peripheral_7b << (uint32_t)k_ra8_i2c_addr_shift) |
              (uint32_t)k_ra8_i2c_addr_rw_write);
  ra8_err_t err = internal_i2c_send_address(reg, address_byte);
  if (err != k_ra8_ok) {
    internal_i2c_stop(reg);
    s_i2c_state[channel].bus_held = false;
    return err;
  }

  err = internal_i2c_drain_tx(reg, data, len);
  return internal_i2c_finish_tx(reg, channel, err, send_stop);
}

/**
 * @brief Drain ``len`` bytes from ICDRR into ``out`` (controller receive).
 *
 * @details
 * Mirrors the FSP r_iic_master RXI sequence / HUM Ch 39.3.4 p 2400.
 * After the address-phase RDRF, WAIT (for 1-2 byte reads) and NACK (for
 * a 1-byte read) are armed before the dummy ICDRR read that starts the
 * data clock. Per received byte, the end-of-frame controls are armed by
 * the bytes-remaining countdown: WAIT at remain==3, NACK at remain==2,
 * and a STOP *request* at remain==1. The STOP must be a request only --
 * it physically fires after the final ICDRR read and the WAIT clear, so
 * waiting for BBSY before reading the last byte would deadlock.
 *
 * @param[in]  reg Channel register block.
 * @param[out] out Destination buffer.
 * @param[in]  len Byte count (non-zero).
 * @return ``k_ra8_ok`` once all bytes drained, else timeout status.
 * @retval k_ra8_ok            All bytes drained and STOP completed.
 * @retval k_ra8_err_hw_timeout RDRF never set within the spin budget.
 *
 * @pre reg and out are non-NULL and len is non-zero.
 * @pre The read address byte was acknowledged.
 * @post STOP has fired, the bus is free, and ICMR3 WAIT/ACKBT are clear.
 * @post ICMR3.WAIT and ICMR3.ACKBT read back zero for the next transfer.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t internal_i2c_drain_rx(volatile r_i2c_regs_t* reg, uint8_t* out, uint32_t len)
{
  /* First RDRF marks the address-phase completion. Arm the short-read
   * end-of-frame controls before the dummy read kicks off the data clock.
   * HUM Ch 39.3.4 "Controller Receive Operation" p 2400 */
  ra8_err_t err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
  if (err != k_ra8_ok) {
    return err;
  }
  if (len <= (uint32_t)k_ra8_i2c_rx_short_len) {
    /* HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3 -- WAIT" p 2376 */
    reg->ICMR3 = (uint8_t)(reg->ICMR3 | (uint8_t)k_ra8_i2c_msk_icmr3_wait);
  }
  if (len == (uint32_t)k_ra8_i2c_rx_single_len) {
    internal_i2c_set_nack(reg);
  }
  /* Dummy read starts the data clock.
   * HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
  (void)reg->ICDRR;

  for (uint32_t loaded = 0U; loaded < len; loaded++) {
    err = internal_i2c_wait_icsr2(reg, (uint8_t)k_ra8_i2c_msk_icsr2_rdrf);
    if (err != k_ra8_ok) {
      break;
    }
    const uint32_t remain = len - loaded;
    if (remain == (uint32_t)k_ra8_i2c_rx_remain_wait) {
      /* HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3 -- WAIT" p 2376 */
      reg->ICMR3 = (uint8_t)(reg->ICMR3 | (uint8_t)k_ra8_i2c_msk_icmr3_wait);
    } else if (remain == (uint32_t)k_ra8_i2c_rx_remain_nack) {
      internal_i2c_set_nack(reg);
    } else if (remain == (uint32_t)k_ra8_i2c_rx_remain_stop) {
      internal_i2c_stop_request(reg);
    } else {
      /* Mid-stream byte: no end-of-frame control to arm. */
    }
    /* HUM Ch 39.2.18 "ICDRR : I2C Bus Receive Data Register" p 2393 */
    out[loaded] = reg->ICDRR;
  }
  /* Clear WAIT (lets the requested STOP fire) and ACKBT for the next
   * transaction, then wait for the bus to be released.
   * HUM Ch 39.2.5 "ICMR3 : I2C Bus Mode Register 3" p 2376 */
  reg->ICMR3 = (uint8_t)(reg->ICMR3 & (uint8_t) ~(uint8_t)((uint8_t)k_ra8_i2c_msk_icmr3_wait |
                                                           (uint8_t)k_ra8_i2c_msk_icmr3_ackbt));
  internal_i2c_wait_bus_free(reg);
  return err;
}

ra8_err_t ra8_i2c_read(uint8_t channel, uint8_t peripheral_7b, uint8_t* data, uint32_t len)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_i2c_tag, "i2c_read: channel");
  RA8_CHECK_NULL_PTR(data, s_i2c_tag, "i2c_read: data");
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }

  const bool      bus_held  = s_i2c_state[channel].bus_held;
  const ra8_err_t busy_gate = internal_i2c_busy_gate(reg, bus_held);
  if (busy_gate != k_ra8_ok) {
    return busy_gate;
  }

  internal_i2c_clear_status(reg);
  internal_i2c_open_phase(reg, bus_held);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)peripheral_7b << (uint32_t)k_ra8_i2c_addr_shift) |
              (uint32_t)k_ra8_i2c_addr_rw_read);
  ra8_err_t err = internal_i2c_send_address(reg, address_byte);
  if (err != k_ra8_ok) {
    internal_i2c_stop(reg);
    s_i2c_state[channel].bus_held = false;
    return err;
  }

  err                        = internal_i2c_drain_rx(reg, data, len);
  const ra8_err_t status_err = internal_i2c_status_from_icsr2(reg->ICSR2);
  if (status_err != k_ra8_ok) {
    err = status_err;
  }
  internal_i2c_clear_status(reg);
  s_i2c_state[channel].bus_held = false;
  return err;
}

ra8_err_t ra8_i2c_transfer(uint8_t        channel,
                           uint8_t        peripheral_7b,
                           const uint8_t* wr,
                           uint32_t       wr_len,
                           uint8_t*       rd,
                           uint32_t       rd_len)
{
  if (ra8_i2c_regs(channel) == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((wr_len == 0U) && (rd_len == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  if ((wr_len != 0U) && (wr == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((rd_len != 0U) && (rd == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  /* Phase 1: write, holding the bus when a read phase follows. */
  if (wr_len != 0U) {
    const ra8_err_t w_err =
      ra8_i2c_write(channel, peripheral_7b, wr, wr_len, /*send_stop=*/(rd_len == 0U));
    if (w_err != k_ra8_ok) {
      return w_err;
    }
  }

  /* Phase 2: read (issues STOP and releases the bus). */
  if (rd_len != 0U) {
    return ra8_i2c_read(channel, peripheral_7b, rd, rd_len);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_i2c_scan(uint8_t channel, uint8_t peripheral_7b, bool* out_acked)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_i2c_tag, "i2c_scan: channel");
  RA8_CHECK_NULL_PTR(out_acked, s_i2c_tag, "i2c_scan: out_acked");

  *out_acked                = false;
  const ra8_err_t busy_gate = internal_i2c_busy_gate(reg, s_i2c_state[channel].bus_held);
  if (busy_gate != k_ra8_ok) {
    return busy_gate;
  }

  internal_i2c_clear_status(reg);
  internal_i2c_start(reg);

  const uint8_t address_byte =
    (uint8_t)(((uint32_t)peripheral_7b << (uint32_t)k_ra8_i2c_addr_shift) |
              (uint32_t)k_ra8_i2c_addr_rw_write);
  ra8_err_t err = internal_i2c_send_address(reg, address_byte);
  /* Address NACK is a valid probe outcome, not a hard failure. */
  if ((err != k_ra8_ok) && (err != k_ra8_err_nack)) {
    internal_i2c_stop(reg);
    s_i2c_state[channel].bus_held = false;
    return err;
  }

  /* Wait for TEND (peripheral ACKed) or NACKF (no peripheral answered).
   * HUM Ch 39.2.10 "ICSR2 : I2C Bus Status Register 2" p 2384 */
  err = internal_i2c_wait_icsr2(
    reg,
    (uint8_t)((uint8_t)k_ra8_i2c_msk_icsr2_tend | (uint8_t)k_ra8_i2c_msk_icsr2_nackf));
  if (err == k_ra8_ok) {
    *out_acked = (reg->ICSR2 & (uint8_t)k_ra8_i2c_msk_icsr2_nackf) == 0U;
  }

  internal_i2c_stop(reg);
  internal_i2c_clear_status(reg);
  s_i2c_state[channel].bus_held = false;
  return err;
}
