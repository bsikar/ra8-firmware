/**
 * @file ra_spi_b.c
 * @brief SPI_B controller driver (polling + IRQ dispatch + DMA pipes)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the public ``ra_spi`` API in ``ra_spi.h`` against the
 * RA8D2 SPI_B (Type-B SPI) peripheral. Mirrors the controller-mode
 * polling flow from FSP ``r_spi_b.c`` (FSP ``R_SPI_B_Open`` /
 * ``r_spi_b_hw_config`` / ``r_spi_b_start_transfer``):
 *
 *  - ``ra_spi_init`` mirrors ``R_SPI_B_Open`` + ``r_spi_b_hw_config``:
 *    enables MSTP, clears SPSR, programmes SPCR3 (SPBR), SPDECR
 *    (delays), SPCR2, SPCMD0 (CPHA/CPOL/SPB/LSBF), SPDCR, then
 *    asserts SPCR with MSTR + SPE.
 *  - ``ra_spi_xfer8`` is a single-frame full-duplex polled xfer that
 *    follows HUM Ch 43.3.13 controller-mode operation section (p 2911) and the
 *    FSP ``r_spi_b_transmit`` / ``r_spi_b_receive`` pair: wait for
 *    SPTEF, write SPDR, wait for SPRF, read SPDR, clear SPSR via
 *    SPSRC. Driver explicitly polls SPSR (HUM Ch 43.2.9 p 2898) and
 *    write-1-clears via SPSRC (HUM Ch 43.2.13 p 2905).
 *  - ``ra_spi_set_clock`` rewrites SPCR3.SPBR (HUM Ch 43.2.6 p 2891).
 *  - ``ra_spi_attach_transfer_handler`` registers a callback that
 *    fires from the SPEI dispatch path; SPI_B status flags are
 *    cleared via SPSRC (write-1).
 *
 * The legacy 8-bit SPI block ``SPCR/SPPCR/SPBR/SSLND/SPND/SPCKD``
 * register set has been removed -- those registers do not exist on
 * RA8D2.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_spi_regs.h"
#include "ra_check.h"
#include "ra_dma.h"
#include "ra_dmac.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_spi.h"

static const char* s_tag = "SPI_B";

/* =============================================================================
 * Constants and lookup tables
 * =============================================================================
 */

/**
 * @enum ra_spi_b_channel_count_t
 * @brief Channel count addressed by this driver.
 */
typedef enum : uint8_t {
  k_ra_spi_b_channel_count = 2U, /**< SPI0 + SPI1.                              */
} ra_spi_b_channel_count_t;

/**
 * @var s_spi_mstp_table
 * @brief Channel-index -> MSTP id (HUM Ch 11.2.7 "MSTPCRB", p 444).
 */
static const ra_mstp_t s_spi_mstp_table[k_ra_spi_b_channel_count] = {
  k_ra_mstp_spi0,
  k_ra_mstp_spi1,
};

/**
 * @enum ra_spi_b_poll_t
 * @brief Polling-loop budget. Used to bound HW waits.
 */
typedef enum : uint32_t {
  k_ra_spi_b_poll_limit = 200000U,
} ra_spi_b_poll_t;

/**
 * @enum ra_spi_b_default_t
 * @brief Default register values for the legacy ``ra_spi_master_init`` shim.
 *
 * @details
 * These are the pre-existing defaults retained so the legacy
 * ``ra_spi_master_init`` API continues to work (mode 0, no LSB
 * first, ~1.9 MHz at PCLKA = 125 MHz). FSP encodes the same
 * concept in its default extended config.
 */
typedef enum : uint32_t {
  k_ra_spi_b_default_baud_hz  = 1900000UL,
  k_ra_spi_b_default_pclka_hz = 125000000UL,
} ra_spi_b_default_t;

/**
 * @enum ra_spi_b_unit_bytes_t
 * @brief Bytes-per-frame for each supported transfer width.
 *
 * @details
 * Eliminates magic numbers in the bit-width-aware load / store loops
 * (CLAUDE.md "No Magic Numbers" rule). Each value is the number of
 * caller-buffer bytes consumed (or produced) per shifted SPI frame.
 */
typedef enum : uint8_t {
  k_ra_spi_b_bytes_per_unit_8  = 1U, /**<  8-bit frame -> 1 byte.   */
  k_ra_spi_b_bytes_per_unit_16 = 2U, /**< 16-bit frame -> 2 bytes.  */
  k_ra_spi_b_bytes_per_unit_32 = 4U, /**< 32-bit frame -> 4 bytes.  */
} ra_spi_b_unit_bytes_t;

/**
 * @enum ra_spi_b_dummy_t
 * @brief Dummy TX values written when ``ra_spi_read`` has no caller payload.
 *
 * @details
 * Idle-line value matches the SD-card / SPI-flash convention of
 * driving COPI high while only RX matters.
 */
typedef enum : uint32_t {
  k_ra_spi_b_dummy_tx_8  = 0x000000FFUL, /**<  8-bit dummy. */
  k_ra_spi_b_dummy_tx_16 = 0x0000FFFFUL, /**< 16-bit dummy. */
  k_ra_spi_b_dummy_tx_32 = 0xFFFFFFFFUL, /**< 32-bit dummy. */
} ra_spi_b_dummy_t;

/* =============================================================================
 * Per-channel runtime state
 * =============================================================================
 */

/**
 * @struct ra_spi_state_t
 * @brief Per-channel dispatch state owned by this driver.
 */
typedef struct {
  ra_spi_complete_fn_t cb;          /**< Transfer-complete callback. */
  void*                ctx;         /**< Callback context.            */
  bool                 initialized; /**< True after ``ra_spi_init``.  */
} ra_spi_state_t;

/**
 * @var s_spi_state
 * @brief Per-channel state table indexed by channel.
 */
static ra_spi_state_t s_spi_state[k_ra_spi_b_channel_count];

/* =============================================================================
 * Bit-rate helper
 * =============================================================================
 */

/**
 * @brief Compute SPCR3.SPBR for a requested bit-rate.
 *
 * @details
 * SPI_B bit-rate equation (HUM Ch 43.2.6 p 2891 + FSP
 * ``R_SPI_B_CalculateBitrate``):
 *
 *   ``f_RSPCK = TCLK / (2 * (SPBR + 1) * 2^N)``
 *
 * where ``N = SPCMDn.BRDV``. The bring-up driver leaves BRDV = 0 so
 * the equation reduces to ``SPBR = (TCLK / (2 * baud)) - 1``.
 *
 * @param[in] baud_hz Desired bit-rate in Hz.
 * @param[in] pclka_hz Active PCLKA frequency in Hz.
 *
 * @return SPBR value, clamped to [0, 0xFF].
 *
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint8_t internal_spbr(uint32_t baud_hz, uint32_t pclka_hz)
{
  if ((baud_hz == 0U) || (pclka_hz == 0U)) {
    return 0U;
  }
  const uint32_t divisor = 2U * baud_hz;
  const uint32_t n       = pclka_hz / divisor;
  if (n == 0U) {
    return 0U;
  }
  const uint32_t result = n - 1U;
  if (result > (uint32_t)k_ra_spbr_max) {
    return k_ra_spbr_max;
  }
  return (uint8_t)result;
}

/**
 * @brief Build SPCMD0 from a ``ra_spi_cfg_t``.
 *
 * @details
 * Bit-mapping (HUM Ch 43.2.7 p 2893, FSP ``r_spi_b_hw_config``):
 *  - ``CPHA``  (bit 0)  from ``cfg->mode``.
 *  - ``CPOL``  (bit 1)  from ``cfg->mode``.
 *  - ``LSBF``  (bit 12) from ``cfg->lsb_first``.
 *  - ``SPB``   [20:16]  set to 8-bit frame (k_ra_spcmd_spb_8bit).
 *  - Delay enables (SPNDEN/SLNDEN/SCKDEN) are left clear; the
 *    bring-up driver does not gate delay registers.
 *
 * @param[in] cfg See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_spcmd(const ra_spi_cfg_t* cfg)
{
  uint32_t v = 0U;
  /* CPHA / CPOL match SPI mode 0..3. */
  if ((cfg->mode == k_ra_spi_mode_1) || (cfg->mode == k_ra_spi_mode_3)) {
    v |= k_ra_spcmd_mask_cpha;
  }
  if ((cfg->mode == k_ra_spi_mode_2) || (cfg->mode == k_ra_spi_mode_3)) {
    v |= k_ra_spcmd_mask_cpol;
  }
  if (cfg->lsb_first) {
    v |= k_ra_spcmd_mask_lsbf;
  }
  /* 8-bit frame. */
  v |= ((uint32_t)k_ra_spcmd_spb_8bit << k_ra_spcmd_bit_spb_lo) & k_ra_spcmd_mask_spb;
  return v;
}

/**
 * @brief Build SPCR (control register 1) for controller polling mode.
 *
 * @details
 * Mirrors the controller-mode subset of FSP ``r_spi_b_hw_config`` (lines
 * 525-670). Sets MSTR + SCKASE + SPE; leaves IRQ-enable bits
 * (SPRIE/SPTIE/SPEIE/CENDIE) clear because the polling driver
 * services SPSR directly.
 *
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_spcr_master(void)
{
  uint32_t v = 0U;
  v |= k_ra_spcr_mask_mstr;   /* Controller mode.            */
  v |= k_ra_spcr_mask_sckase; /* Auto-stop SCK.          */
  v |= k_ra_spcr_mask_spe;    /* SPI enable.             */
  return v;
}

/**
 * @brief Wait for an SPSR flag to assert.
 *
 * @details
 * Bounded polling loop (NASA P10 Rule 2). The SPI_B SPSR flags
 * SPTEF (TX empty) and SPRF (RX full) are clear-on-write through
 * SPSRC -- callers are responsible for clearing after acting on
 * them.
 *
 * The host build (``RA_SIMULATOR_MODE``) does NOT spin. The simulator
 * backs the SPI register file with plain mmap'd RAM, so SPSR cannot
 * advance on its own -- a real busy-poll would either succeed
 * immediately (test pre-staged the flag) or burn the full 200000-cycle
 * budget before returning ``k_ra_err_hw_timeout``. The host
 * short-circuit does a single-shot read of the (test-staged) SPSR
 * value:
 *   - flag set   -> k_ra_ok (happy-path: tests pre-stage SPSR =
 *                   SPTEF|SPRF; the driver's SPSRC writes do not
 *                   touch SPSR in plain-RAM mode, so a single
 *                   pre-stage persists across xfers).
 *   - flag clear -> k_ra_err_hw_timeout, instantly (preserves the
 *                   timeout-path tests in test_ra_spi).
 * This mirrors the pattern used by ``internal_ra_epaper_wait_ready``
 * (commit 57f6c4a28) so any driver layered on top of SPI can run its
 * happy-path through unit tests when the test pre-stages SPSR.
 *
 * @param[in] reg See implementation.
 * @param[in] flag_mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_wait_spsr(volatile r_spi_regs_t* reg, uint32_t flag_mask)
{
#ifdef RA_SIMULATOR_MODE
  if ((reg->SPSR & flag_mask) != 0U) {
    return k_ra_ok;
  }
  return k_ra_err_hw_timeout;
#else
  for (uint32_t i = 0U; i < k_ra_spi_b_poll_limit; i++) { /* GCOVR_EXCL_BR_LINE */
    if ((reg->SPSR & flag_mask) != 0U) {                  /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
#endif
}

/* =============================================================================
 * Lifecycle: init / deinit
 * =============================================================================
 */

/* Implementation of ra_spi_init (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_init(uint8_t channel, const ra_spi_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "spi_init: cfg");
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }

  /* Power up the SPI channel. */
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra_err_t mst_err = ra_mstp_enable(s_spi_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "spi_init: mstp"); /* GCOVR_EXCL_BR_LINE */

  /* Disable SPE before reprogramming. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR = 0U;

  /* Clear every flag in SPSR via the SPSRC write-1-clears register. */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra_spsrc_mask_all;

  /* Programme SPCR3 with the new SPBR (bit-rate divider). */
  /* HUM Ch 43.2.6 "SPCR3 : SPI Control Register 3" p 2891 */
  const uint8_t  spbr  = internal_spbr(cfg->baud_hz, cfg->pclka_hz);
  const uint32_t spcr3 = ((uint32_t)spbr << k_ra_spcr3_bit_spbr) & k_ra_spcr3_mask_spbr;
  reg->SPCR3           = spcr3;

  /* Zero out delays. */
  /* HUM Ch 43.2.3 "SPDECR : SPI Delay Control Register" p 2883 */
  reg->SPDECR = 0U;

  /* HUM Ch 43.2.5 "SPCR2 : SPI Control Register 2" p 2889 */
  reg->SPCR2 = 0U;

  /* HUM Ch 43.2.7 "SPCMDm : SPI Command Register" p 2893 */
  reg->SPCMD[0] = internal_spcmd(cfg);

  /* HUM Ch 43.2.10 "SPDCR : SPI Data Control Register" p 2896 */
  reg->SPDCR  = 0U;
  reg->SPDCR2 = 0U;

  /* Reset both FIFOs before enabling. */
  /* HUM Ch 43.2.14 "SPFCR : SPI FIFO Clear Register" p 2906 */
  reg->SPFCR = k_ra_spfcr_mask_spfrst;

  /* Enable SPI: SPE + MSTR + SCKASE. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR = internal_spcr_master();

  s_spi_state[channel].cb          = nullptr;
  s_spi_state[channel].ctx         = nullptr;
  s_spi_state[channel].initialized = true;
  ra_log_info_val(s_tag, "spi_init channel", (uint32_t)channel);
  return k_ra_ok;
}

/* Implementation of ra_spi_deinit (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_deinit(uint8_t channel)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Clear SPE. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR                        = 0U;
  s_spi_state[channel].cb          = nullptr;
  s_spi_state[channel].ctx         = nullptr;
  s_spi_state[channel].initialized = false;
  return ra_mstp_disable(s_spi_mstp_table[channel]);
}

/* =============================================================================
 * Legacy polling shim
 * =============================================================================
 */

/* Implementation of ra_spi_master_init (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_master_init(uint8_t channel)
{
  const ra_spi_cfg_t cfg = {
    .baud_hz   = k_ra_spi_b_default_baud_hz,
    .pclka_hz  = k_ra_spi_b_default_pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  /* The pre-existing test contract distinguishes "channel out of range"
   * with k_ra_err_null_ptr (because ra_spi() returns nullptr). Preserve. */
  if (ra_spi(channel) == nullptr) {
    return k_ra_err_null_ptr;
  }
  return ra_spi_init(channel, &cfg);
}

/* Implementation of ra_spi_xfer8 (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_xfer8(uint8_t channel, uint8_t tx, uint8_t* rx)
{
  volatile r_spi_regs_t* reg = ra_spi(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");

  /* Wait for TX buffer empty. */
  /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
  ra_err_t err = internal_wait_spsr(reg, k_ra_spsr_mask_sptef);
  if (err != k_ra_ok) {
    return err;
  }

  /* Push TX byte. */
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  reg->SPDR = (uint32_t)tx;

  /* Clear TX-empty flag (write-1). */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra_spsrc_mask_sptefc;

  /* Wait for RX buffer full. */
  /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
  err = internal_wait_spsr(reg, k_ra_spsr_mask_sprf);
  if (err != k_ra_ok) {
    return err;
  }

  /* Drain RX byte. */
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  const uint8_t received = (uint8_t)reg->SPDR;

  /* Clear RX-full flag. */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra_spsrc_mask_sprfc;

  if (rx != nullptr) {
    *rx = received;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Multi-byte / multi-width polling transfers
 * =============================================================================
 */

/**
 * @brief Map a public ``ra_spi_bit_width_t`` to its bytes-per-unit.
 *
 * @param[in] bit_width Public width enum.
 * @param[out] out_bytes Bytes per shifted frame (1, 2, or 4).
 *
 * @retval k_ra_ok ``*out_bytes`` written.
 * @retval k_ra_err_invalid_arg ``bit_width`` not one of the supported widths.
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
static ra_err_t internal_unit_bytes(ra_spi_bit_width_t bit_width, uint8_t* out_bytes)
{
  switch (bit_width) {
    case k_ra_spi_width_8:
      *out_bytes = k_ra_spi_b_bytes_per_unit_8;
      return k_ra_ok;
    case k_ra_spi_width_16:
      *out_bytes = k_ra_spi_b_bytes_per_unit_16;
      return k_ra_ok;
    case k_ra_spi_width_32:
      *out_bytes = k_ra_spi_b_bytes_per_unit_32;
      return k_ra_ok;
    default:
      return k_ra_err_invalid_arg;
  }
}

/**
 * @brief Programme SPCMD0.SPB to the requested bit-width.
 *
 * @details
 * Mirrors FSP ``r_spi_b_bit_width_config`` (lines 701-726). The
 * SPB[20:16] field encodes ``N - 1`` for an ``N``-bit frame; the
 * public ``ra_spi_bit_width_t`` enum already carries the raw
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
static void internal_apply_bit_width(volatile r_spi_regs_t* reg, ra_spi_bit_width_t bit_width)
{
  /* HUM Ch 43.2.7 "SPCMDm : SPI Command Register" p 2893 */
  uint32_t spcmd0 = reg->SPCMD[0] & ~k_ra_spcmd_mask_spb;
  spcmd0 |= ((uint32_t)bit_width << k_ra_spcmd_bit_spb_lo) & k_ra_spcmd_mask_spb;
  reg->SPCMD[0] = spcmd0;
}

/**
 * @brief Pull one TX unit out of ``tx`` (or use a dummy) and write SPDR.
 *
 * @details
 * Mirrors FSP ``r_spi_b_transmit`` (lines 981-1024) but the bit-width
 * branch uses the public ``ra_spi_bit_width_t`` value (already raw
 * SPB encoding) compared against ``k_ra_spi_width_*``. When ``tx``
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
static void internal_push_unit(volatile r_spi_regs_t* reg,
                               const void*            tx,
                               uint32_t               idx,
                               ra_spi_bit_width_t     bit_width)
{
  uint32_t value = 0U;
  if (tx == nullptr) {
    if (bit_width == k_ra_spi_width_32) {
      value = k_ra_spi_b_dummy_tx_32;
    } else if (bit_width == k_ra_spi_width_16) {
      value = k_ra_spi_b_dummy_tx_16;
    } else {
      value = k_ra_spi_b_dummy_tx_8;
    }
  } else if (bit_width == k_ra_spi_width_32) {
    value = ((const uint32_t*)tx)[idx];
  } else if (bit_width == k_ra_spi_width_16) {
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
static void
internal_pop_unit(volatile r_spi_regs_t* reg, void* rx, uint32_t idx, ra_spi_bit_width_t bit_width)
{
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  const uint32_t value = reg->SPDR;
  if (rx == nullptr) {
    return;
  }
  if (bit_width == k_ra_spi_width_32) {
    ((uint32_t*)rx)[idx] = value;
  } else if (bit_width == k_ra_spi_width_16) {
    ((uint16_t*)rx)[idx] = (uint16_t)value;
  } else {
    ((uint8_t*)rx)[idx] = (uint8_t)value;
  }
}

/**
 * @brief Common engine for ``ra_spi_write`` / ``ra_spi_read`` / ``ra_spi_write_read``.
 *
 * @details
 * Mirrors FSP ``r_spi_b_write_read_common`` (lines 795-930) with the
 * polling transfer loop spelled out instead of dispatched through the
 * SPTI/SPRI interrupts. The bound is the existing
 * ``k_ra_spi_b_poll_limit`` budget per SPSR wait, which already
 * tracks the canonical ``k_ra_timeout_default_ms`` budget at the
 * NS-world tick rate the driver is wired against.
 *
 * Per FSP, exactly one of ``tx`` or ``rx`` may be NULL but never
 * both. Length 0 returns success without touching the bus.
 *
 * @par NASA Power of 10 Compliance:
 * - Rule 2: Outer loop bounded by ``len`` (caller-supplied);
 *   inner SPSR wait bounded by ``k_ra_spi_b_poll_limit``.
 * - Rule 5: 4 preconditions, 2 postconditions.
 *
 * @param[in] channel See implementation.
 * @param[in] tx See implementation.
 * @param[in] rx See implementation.
 * @param[in] len See implementation.
 * @param[in] bit_width See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_xfer_common(uint8_t            channel,
                                     const void*        tx,
                                     void*              rx,
                                     uint32_t           len,
                                     ra_spi_bit_width_t bit_width)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  uint8_t        bytes_per_unit = 0U;
  const ra_err_t bw_err         = internal_unit_bytes(bit_width, &bytes_per_unit);
  if (bw_err != k_ra_ok) {
    return bw_err;
  }
  if (len == 0U) {
    return k_ra_ok;
  }
  // mcdc-deactivated: TU-local helper internal_apply_bit_width null-pair guard; the public-API ra_spi_b_transfer entry validates that at least one of (tx, rx) is non-NULL before calling this helper, so the AND's two conditions cannot both be true on any reachable path -- defensive depth guard only.
  if ((tx == nullptr) && (rx == nullptr)) {
    return k_ra_err_null_ptr;
  }
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }

  /* Lock SPCMD0.SPB to the requested width before pushing data. */
  internal_apply_bit_width(reg, bit_width);
  /* Suppress unused warning when callers never pass a 32-bit frame. */
  (void)bytes_per_unit;

  for (uint32_t i = 0U; i < len; i++) {
    /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
    ra_err_t err = internal_wait_spsr(reg, k_ra_spsr_mask_sptef);
    if (err != k_ra_ok) {
      return err;
    }
    internal_push_unit(reg, tx, i, bit_width);
    /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
    reg->SPSRC = k_ra_spsrc_mask_sptefc;

    err = internal_wait_spsr(reg, k_ra_spsr_mask_sprf);
    if (err != k_ra_ok) {
      return err;
    }
    internal_pop_unit(reg, rx, i, bit_width);
    /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
    reg->SPSRC = k_ra_spsrc_mask_sprfc;
  }
  return k_ra_ok;
}

/* Implementation of ra_spi_write (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_write(uint8_t channel, const void* tx, uint32_t len, ra_spi_bit_width_t bit_width)
{
  if ((tx == nullptr) && (len > 0U)) {
    return k_ra_err_null_ptr;
  }
  return internal_xfer_common(channel, tx, nullptr, len, bit_width);
}

/* Implementation of ra_spi_read (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_read(uint8_t channel, void* rx, uint32_t len, ra_spi_bit_width_t bit_width)
{
  if ((rx == nullptr) && (len > 0U)) {
    return k_ra_err_null_ptr;
  }
  return internal_xfer_common(channel, nullptr, rx, len, bit_width);
}

/* Implementation of ra_spi_write_read (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_write_read(uint8_t            channel,
                           const void*        tx,
                           void*              rx,
                           uint32_t           len,
                           ra_spi_bit_width_t bit_width)
{
  if (len > 0U) {
    if (tx == nullptr) {
      return k_ra_err_null_ptr;
    }
    if (rx == nullptr) {
      return k_ra_err_null_ptr;
    }
  }
  return internal_xfer_common(channel, tx, rx, len, bit_width);
}

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

/* Implementation of ra_spi_set_clock (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_set_clock(uint8_t channel, uint32_t baud_hz, uint32_t pclka_hz)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  if (baud_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* Update SPCR3.SPBR[15:8]. */
  /* HUM Ch 43.2.6 "SPCR3 : SPI Control Register 3" p 2891 */
  const uint8_t  spbr     = internal_spbr(baud_hz, pclka_hz);
  const uint32_t spcr3_in = reg->SPCR3 & ~k_ra_spcr3_mask_spbr;
  reg->SPCR3 = spcr3_in | (((uint32_t)spbr << k_ra_spcr3_bit_spbr) & k_ra_spcr3_mask_spbr);
  return k_ra_ok;
}

/* =============================================================================
 * Error status
 * =============================================================================
 */

/* Implementation of ra_spi_get_errors (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "spi get_errors");
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile const r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  const uint32_t ss = reg->SPSR;
  uint8_t        m  = k_ra_spi_err_none;
  if ((ss & k_ra_spsr_mask_ovrf) != 0U) {
    m |= k_ra_spi_err_overrun;
  }
  if ((ss & k_ra_spsr_mask_modf) != 0U) {
    m |= k_ra_spi_err_mode;
  }
  if ((ss & k_ra_spsr_mask_perf) != 0U) {
    m |= k_ra_spi_err_parity;
  }
  if ((ss & k_ra_spsr_mask_udrf) != 0U) {
    m |= k_ra_spi_err_underrun;
  }
  *out_mask = m;
  return k_ra_ok;
}

/* Implementation of ra_spi_clear_errors (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_clear_errors(uint8_t channel)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* SPI_B status flags clear via SPSRC (write-1). */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra_spsr_mask_errs;
  return k_ra_ok;
}

/* Implementation of ra_spi_attach_transfer_handler (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_attach_transfer_handler(uint8_t channel, ra_spi_complete_fn_t fn, void* ctx)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_spi_state[channel].cb  = fn;
  s_spi_state[channel].ctx = ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/* Implementation of ra_spi_enter_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_enter_stop(uint8_t channel)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Clear SPE. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR = 0U;
  return ra_mstp_disable(s_spi_mstp_table[channel]);
}

/* Implementation of ra_spi_exit_stop (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_exit_stop(uint8_t channel)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_spi_mstp_table[channel]);
}

/* =============================================================================
 * DMA TX / RX
 * =============================================================================
 */

/* Implementation of ra_spi_write_dma (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_write_dma(uint8_t              channel,
                          const uint8_t*       data,
                          uint16_t             len,
                          ra_dma_complete_fn_t on_complete,
                          void*                ctx,
                          uint8_t*             out_dma_channel)
{
  RA_CHECK_NULL_PTR(data, s_tag, "spi_write_dma: data");
  RA_CHECK_NULL_PTR(out_dma_channel, s_tag, "spi_write_dma: out_dma_channel");
  if ((channel >= k_ra_spi_b_channel_count) || (len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Byte-wide DMA writes target the low byte of SPDR; wider-frame
   * streaming is a future wave. */
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  ra_dma_request_t req = {};
  req.src_addr         = (uintptr_t)data;
  req.dst_addr         = (uintptr_t)&reg->SPDR;
  req.count            = len;
  req.width            = k_ra_dmac_width_byte;
  req.src_inc          = true;
  req.dst_inc          = false;
  req.trigger          = (ra_elc_event_t)0;
  req.on_complete      = on_complete;
  req.ctx              = ctx;
  return ra_dma_request(&req, out_dma_channel);
}

/* Implementation of ra_spi_read_dma (see header for full contract) -- see header for the documented contract. */
ra_err_t ra_spi_read_dma(uint8_t              channel,
                         uint8_t*             out_buf, // NOLINT(readability-non-const-parameter)
                         uint16_t             len,
                         ra_dma_complete_fn_t on_complete,
                         void*                ctx,
                         uint8_t*             out_dma_channel)
{
  RA_CHECK_NULL_PTR(out_buf, s_tag, "spi_read_dma: out_buf");
  RA_CHECK_NULL_PTR(out_dma_channel, s_tag, "spi_read_dma: out_dma_channel");
  if ((channel >= k_ra_spi_b_channel_count) || (len == 0U)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra_spi(channel);
  if (reg == nullptr) {          /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  ra_dma_request_t req = {};
  req.src_addr         = (uintptr_t)&reg->SPDR;
  req.dst_addr         = (uintptr_t)out_buf;
  req.count            = len;
  req.width            = k_ra_dmac_width_byte;
  req.src_inc          = false;
  req.dst_inc          = true;
  req.trigger          = (ra_elc_event_t)0;
  req.on_complete      = on_complete;
  req.ctx              = ctx;
  return ra_dma_request(&req, out_dma_channel);
}

/* =============================================================================
 * ISR dispatch -- placeholder; real IRQ routing lands with NVIC wiring.
 * =============================================================================
 */

/* Implementation of ra_spi_dispatch_spti (see header for full contract) -- see header for the documented contract. */
void ra_spi_dispatch_spti(uint8_t channel)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return;
  }
  (void)s_spi_state[channel].cb;
}

/* Implementation of ra_spi_dispatch_spri (see header for full contract) -- see header for the documented contract. */
void ra_spi_dispatch_spri(uint8_t channel)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return;
  }
  (void)s_spi_state[channel].cb;
}

/* Implementation of ra_spi_dispatch_spei (see header for full contract) -- see header for the documented contract. */
void ra_spi_dispatch_spei(uint8_t channel)
{
  if (channel >= k_ra_spi_b_channel_count) {
    return;
  }
  uint8_t mask = 0U;
  (void)ra_spi_get_errors(channel, &mask);
  (void)ra_spi_clear_errors(channel);
  const ra_spi_complete_fn_t cb = s_spi_state[channel].cb;
  if ((mask != 0U) && (cb != nullptr)) {
    cb(s_spi_state[channel].ctx, mask);
  }
}
