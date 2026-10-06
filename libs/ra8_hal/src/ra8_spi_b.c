/**
 * @file ra8_spi_b.c
 * @brief SPI_B controller driver (polling + IRQ dispatch + DMA pipes)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the public ``ra8_spi`` API in ``ra8_spi.h`` against the
 * RA8D2 SPI_B (Type-B SPI) peripheral. Mirrors the controller-mode
 * polling flow from FSP ``r_spi_b.c`` (FSP ``R_SPI_B_Open`` /
 * ``r_spi_b_hw_config`` / ``r_spi_b_start_transfer``):
 *
 *  - ``ra8_spi_init`` / ``ra8_spi_deinit`` / ``ra8_spi_controller_init``
 *    (the ``R_SPI_B_Open`` + ``r_spi_b_hw_config`` sequence) are Zig
 *    (src/spi_b_setup_abi.zig).
 *  - ``ra8_spi_xfer8`` is a single-frame full-duplex polled xfer that
 *    follows HUM Ch 43.3.13 controller-mode operation section (p 2911) and the
 *    FSP ``r_spi_b_transmit`` / ``r_spi_b_receive`` pair: wait for
 *    SPTEF, write SPDR, wait for SPRF, read SPDR, clear SPSR via
 *    SPSRC. Driver explicitly polls SPSR (HUM Ch 43.2.9 p 2898) and
 *    write-1-clears via SPSRC (HUM Ch 43.2.13 p 2905).
 *  - ``ra8_spi_set_clock`` rewrites SPCR3.SPBR (HUM Ch 43.2.6 p 2891);
 *    it and the error-status calls are Zig (src/spi_b_clock_abi.zig).
 *  - ``ra8_spi_attach_transfer_handler``, Stop mode and the ISR
 *    dispatchers are Zig (src/spi_b_events_abi.zig).
 *
 * The legacy 8-bit SPI block ``SPCR/SPPCR/SPBR/SSLND/SPND/SPCKD``
 * register set has been removed -- those registers do not exist on
 * RA8D2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"

static const char* const s_tag = "SPI_B";

/* =============================================================================
 * Constants and lookup tables
 * =============================================================================
 */

/**
 * @enum ra8_spi_b_poll_t
 * @brief Polling-loop budget. Used to bound HW waits.
 */
typedef enum : uint32_t {
  k_ra8_spi_b_poll_limit = 200000U, /**< RA8 SPI b poll limit. */
} ra8_spi_b_poll_t;

/**
 * @enum ra8_spi_b_unit_bytes_t
 * @brief Bytes-per-frame for each supported transfer width.
 *
 * @details
 * Eliminates magic numbers in the bit-width-aware load / store loops
 * (CLAUDE.md "No Magic Numbers" rule). Each value is the number of
 * caller-buffer bytes consumed (or produced) per shifted SPI frame.
 */
typedef enum : uint8_t {
  k_ra8_spi_b_bytes_per_unit_8  = 1U, /**< 8-bit frame -> 1 byte.   */
  k_ra8_spi_b_bytes_per_unit_16 = 2U, /**< 16-bit frame -> 2 bytes. */
  k_ra8_spi_b_bytes_per_unit_32 = 4U, /**< 32-bit frame -> 4 bytes. */
} ra8_spi_b_unit_bytes_t;

/**
 * @enum ra8_spi_b_dummy_t
 * @brief Dummy TX values written when ``ra8_spi_read`` has no caller payload.
 *
 * @details
 * Idle-line value matches the SD-card / SPI-flash convention of
 * driving COPI high while only RX matters.
 */
typedef enum : uint32_t {
  k_ra8_spi_b_dummy_tx_8  = 0x000000FFUL, /**< 8-bit dummy.  */
  k_ra8_spi_b_dummy_tx_16 = 0x0000FFFFUL, /**< 16-bit dummy. */
  k_ra8_spi_b_dummy_tx_32 = 0xFFFFFFFFUL, /**< 32-bit dummy. */
} ra8_spi_b_dummy_t;

/* =============================================================================
 * SPSR wait helper
 * =============================================================================
 */

/**
 * @brief Wait for an SPSR flag to assert.
 *
 * @details
 * Bounded polling loop (NASA P10 Rule 2). The SPI_B SPSR flags
 * SPTEF (TX empty) and SPRF (RX full) are clear-on-write through
 * SPSRC -- callers are responsible for clearing after acting on
 * them.
 *
 * Delegates to ``ra8_hw_wait_flag_set32``, whose loop is consulted by the
 * host-test MMIO fault seam (``ra8_fake_mmio_*``): a test pre-staging SPSR =
 * SPTEF|SPRF succeeds on the first poll (seam transparent), ``fail_wait``
 * drives the timeout leg, and ``satisfy_after(n)`` steps the loop's
 * continuation branch for MC/DC. Both the success and timeout legs therefore
 * run on host, unlike the deleted ``RA8_OFF_TARGET`` single-shot
 * short-circuit (T1-01).
 *
 * @param[in] reg See implementation.
 * @param[in] flag_mask See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_wait_spsr(volatile r_spi_regs_t* reg, uint32_t flag_mask)
{
  /* Bounded busy-poll of SPSR. On the host test build the ra8_hw_err MMIO fault
   * seam (ra8_fake_mmio_*) drives this real loop to succeed-after-N or to time out,
   * so both the success and timeout legs are exercised on host (T1-01) rather
   * than compiled out behind an RA8_OFF_TARGET short-circuit. On target it is
   * a plain register spin with a fixed iteration bound. */
  return ra8_hw_wait_flag_set32(&reg->SPSR, flag_mask, (uint32_t)k_ra8_spi_b_poll_limit);
}

/* =============================================================================
 * Legacy polling shim
 * =============================================================================
 */

ra8_err_t ra8_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx)
{
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* Wait for TX buffer empty. */
  /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
  ra8_err_t err = internal_wait_spsr(reg, k_ra8_spsr_mask_sptef);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Push TX byte. */
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  reg->SPDR = (uint32_t)tx;

  /* Clear TX-empty flag (write-1). */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra8_spsrc_mask_sptefc;

  /* Wait for RX buffer full. */
  /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
  err = internal_wait_spsr(reg, k_ra8_spsr_mask_sprf);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Drain RX byte. */
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  const uint8_t received = (uint8_t)reg->SPDR;

  /* Clear RX-full flag. */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra8_spsrc_mask_sprfc;

  if (rx != nullptr) {
    *rx = received;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * Multi-byte / multi-width polling transfers
 * =============================================================================
 */

/**
 * @brief Map a public ``ra8_spi_bit_width_t`` to its bytes-per-unit.
 *
 * @param[in] bit_width Public width enum.
 * @param[out] out_bytes Bytes per shifted frame (1, 2, or 4).
 *
 * @retval k_ra8_ok ``*out_bytes`` written.
 * @retval k_ra8_err_invalid_arg ``bit_width`` not one of the supported widths.
 *
 * @details See implementation.
 * @return Result code.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_unit_bytes(ra8_spi_bit_width_t bit_width, uint8_t* out_bytes)
{
  switch (bit_width) {
    case k_ra8_spi_width_8:
      *out_bytes = k_ra8_spi_b_bytes_per_unit_8;
      return k_ra8_ok;
    case k_ra8_spi_width_16:
      *out_bytes = k_ra8_spi_b_bytes_per_unit_16;
      return k_ra8_ok;
    case k_ra8_spi_width_32:
      *out_bytes = k_ra8_spi_b_bytes_per_unit_32;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Programme SPCMD0.SPB to the requested bit-width.
 *
 * @details
 * Mirrors FSP ``r_spi_b_bit_width_config`` (lines 701-726). The
 * SPB[20:16] field encodes ``N - 1`` for an ``N``-bit frame; the
 * public ``ra8_spi_bit_width_t`` enum already carries the raw
 * encoding so it can be shifted into place directly. The driver
 * keeps SSL Level Keep cleared (single-segment polling transfers
 * only); FSP's SSLKP burst handling is out of scope for this wave.
 *
 * @param[in] reg See implementation.
 * @param[in] bit_width See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_apply_bit_width(volatile r_spi_regs_t* reg,
                                                  ra8_spi_bit_width_t    bit_width)
{
  /* HUM Ch 43.2.7 "SPCMDm : SPI Command Register" p 2893 */
  uint32_t spcmd0 = reg->SPCMD[0] & ~k_ra8_spcmd_mask_spb;
  spcmd0 |= ((uint32_t)bit_width << k_ra8_spcmd_bit_spb_lo) & k_ra8_spcmd_mask_spb;
  reg->SPCMD[0] = spcmd0;
}

/**
 * @brief Pull one TX unit out of ``tx`` (or use a dummy) and write SPDR.
 *
 * @details
 * Mirrors FSP ``r_spi_b_transmit`` (lines 981-1024) but the bit-width
 * branch uses the public ``ra8_spi_bit_width_t`` value (already raw
 * SPB encoding) compared against ``k_ra8_spi_width_*``. When ``tx``
 * is NULL the driver writes a dummy (idle-high) value -- this
 * mirrors typical SPI-flash / SD-card RX-only conventions and
 * differs from FSP only in the dummy magnitude (FSP writes 0).
 *
 * @param[in] reg See implementation.
 * @param[in] tx See implementation.
 * @param[in] idx See implementation.
 * @param[in] bit_width See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_push_unit(volatile r_spi_regs_t* reg,
                                            const void*            tx,
                                            uint32_t               idx,
                                            ra8_spi_bit_width_t    bit_width)
{
  uint32_t value = 0U;
  if (tx == nullptr) {
    if (bit_width == k_ra8_spi_width_32) {
      value = k_ra8_spi_b_dummy_tx_32;
    } else if (bit_width == k_ra8_spi_width_16) {
      value = k_ra8_spi_b_dummy_tx_16;
    } else {
      value = k_ra8_spi_b_dummy_tx_8;
    }
  } else if (bit_width == k_ra8_spi_width_32) {
    value = ((const uint32_t*)tx)[idx];
  } else if (bit_width == k_ra8_spi_width_16) {
    value = (uint32_t)((const uint16_t*)tx)[idx];
  } else {
    value = (uint32_t)((const uint8_t*)tx)[idx];
  }
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  reg->SPDR = value;
}

/**
 * @brief Read SPDR into ``rx`` at ``idx`` (or discard).
 *
 * @details
 * Mirrors FSP ``r_spi_b_receive`` (lines 939-972) -- the FIFO
 * front-end of SPDR returns the most-recently shifted-in unit, and
 * the bit-width determines whether the caller buffer is a uint8_t,
 * uint16_t, or uint32_t array.
 *
 * @param[in] reg See implementation.
 * @param[in] rx See implementation.
 * @param[in] idx See implementation.
 * @param[in] bit_width See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pop_unit(volatile const r_spi_regs_t* reg,
                                           void*                        rx,
                                           uint32_t                     idx,
                                           ra8_spi_bit_width_t          bit_width)
{
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  const uint32_t value = reg->SPDR;
  if (rx == nullptr) {
    return;
  }
  if (bit_width == k_ra8_spi_width_32) {
    ((uint32_t*)rx)[idx] = value;
  } else if (bit_width == k_ra8_spi_width_16) {
    ((uint16_t*)rx)[idx] = (uint16_t)value;
  } else {
    ((uint8_t*)rx)[idx] = (uint8_t)value;
  }
}

/**
 * @brief Common engine for ``ra8_spi_write`` / ``ra8_spi_read`` / ``ra8_spi_write_read``.
 *
 * @details
 * Mirrors FSP ``r_spi_b_write_read_common`` (lines 795-930) with the
 * polling transfer loop spelled out instead of dispatched through the
 * SPTI/SPRI interrupts. The bound is the existing
 * ``k_ra8_spi_b_poll_limit`` budget per SPSR wait, which already
 * tracks the canonical ``k_ra8_timeout_default_ms`` budget at the
 * NS-world tick rate the driver is wired against.
 *
 * Per FSP, exactly one of ``tx`` or ``rx`` may be NULL but never
 * both. Length 0 returns success without touching the bus.
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: Outer loop bounded by ``len`` (caller-supplied);
 *   inner SPSR wait bounded by ``k_ra8_spi_b_poll_limit``.
 * - Rule 5: 4 preconditions, 2 postconditions.
 *
 * @param[in] channel See implementation.
 * @param[in] tx See implementation.
 * @param[in] rx See implementation.
 * @param[in] len See implementation.
 * @param[in] bit_width See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_xfer_common(uint8_t             channel,
                                                   const void*         tx,
                                                   void*               rx,
                                                   uint32_t            len,
                                                   ra8_spi_bit_width_t bit_width)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t         bytes_per_unit = 0U;
  const ra8_err_t bw_err         = internal_unit_bytes(bit_width, &bytes_per_unit);
  if (bw_err != k_ra8_ok) {
    return bw_err;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }
  // mcdc-deactivated: TU-local helper internal_xfer_common null-pair guard; the public-API ra8_spi_b_transfer entry validates that at least one of (tx, rx) is non-NULL before calling this helper, so the AND's two conditions cannot both be true on any reachable path -- defensive depth guard only.
  if ((tx == nullptr) && (rx == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE -- bounded channel yields non-null reg */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE -- bounded channel yields non-null reg    */
  }

  /* Lock SPCMD0.SPB to the requested width before pushing data. */
  internal_apply_bit_width(reg, bit_width);
  /* Suppress unused warning when callers never pass a 32-bit frame. */
  (void)bytes_per_unit;

  for (uint32_t i = 0U; i < len; i++) {
    /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
    ra8_err_t err = internal_wait_spsr(reg, k_ra8_spsr_mask_sptef);
    if (err != k_ra8_ok) {
      return err;
    }
    internal_push_unit(reg, tx, i, bit_width);
    /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
    reg->SPSRC = k_ra8_spsrc_mask_sptefc;

    err = internal_wait_spsr(reg, k_ra8_spsr_mask_sprf);
    if (err != k_ra8_ok) {
      return err;
    }
    internal_pop_unit(reg, rx, i, bit_width);
    /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
    reg->SPSRC = k_ra8_spsrc_mask_sprfc;
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_spi_write(uint8_t channel, const void* tx, uint32_t len, ra8_spi_bit_width_t bit_width)
{
  if ((tx == nullptr) && (len > 0U)) {
    return k_ra8_err_null_ptr;
  }
  return internal_xfer_common(channel, tx, nullptr, len, bit_width);
}

ra8_err_t ra8_spi_read(uint8_t channel, void* rx, uint32_t len, ra8_spi_bit_width_t bit_width)
{
  if ((rx == nullptr) && (len > 0U)) {
    return k_ra8_err_null_ptr;
  }
  return internal_xfer_common(channel, nullptr, rx, len, bit_width);
}

ra8_err_t ra8_spi_write_read(uint8_t             channel,
                             const void*         tx,
                             void*               rx,
                             uint32_t            len,
                             ra8_spi_bit_width_t bit_width)
{
  if (len > 0U) {
    if (tx == nullptr) {
      return k_ra8_err_null_ptr;
    }
    if (rx == nullptr) {
      return k_ra8_err_null_ptr;
    }
  }
  return internal_xfer_common(channel, tx, rx, len, bit_width);
}

/* ra8_spi_set_clock, ra8_spi_get_errors and ra8_spi_clear_errors live in
 * src/spi_b_clock_abi.zig (RA8FW-892). */
/* ra8_spi_attach_transfer_handler, ra8_spi_enter_stop, ra8_spi_exit_stop and
 * the SPTI / SPRI / SPEI dispatchers live in src/spi_b_events_abi.zig
 * (RA8FW-894). */
/* ra8_spi_init, ra8_spi_deinit and ra8_spi_controller_init live in
 * src/spi_b_setup_abi.zig (RA8FW-898). */
