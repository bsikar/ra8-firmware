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
 *  - ``ra8_spi_init`` mirrors ``R_SPI_B_Open`` + ``r_spi_b_hw_config``:
 *    enables MSTP, clears SPSR, programmes SPCR3 (SPBR), SPDECR
 *    (delays), SPCR2, SPCMD0 (CPHA/CPOL/SPB/LSBF), SPDCR, then
 *    asserts SPCR with MSTR + SPE.
 *  - ``ra8_spi_xfer8`` is a single-frame full-duplex polled xfer that
 *    follows HUM Ch 43.3.13 controller-mode operation section (p 2911) and the
 *    FSP ``r_spi_b_transmit`` / ``r_spi_b_receive`` pair: wait for
 *    SPTEF, write SPDR, wait for SPRF, read SPDR, clear SPSR via
 *    SPSRC. Driver explicitly polls SPSR (HUM Ch 43.2.9 p 2898) and
 *    write-1-clears via SPSRC (HUM Ch 43.2.13 p 2905).
 *  - ``ra8_spi_set_clock`` rewrites SPCR3.SPBR (HUM Ch 43.2.6 p 2891).
 *  - ``ra8_spi_attach_transfer_handler`` registers a callback that
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

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"

static const char* s_tag = "SPI_B";

/* =============================================================================
 * Constants and lookup tables
 * =============================================================================
 */

/**
 * @var s_spi_mstp_table
 * @brief Channel-index -> MSTP id (HUM Ch 11.2.7 "MSTPCRB", p 444).
 */
static const ra8_mstp_t s_spi_mstp_table[k_ra8_spi_b_channel_count] = {
  k_ra8_mstp_spi0,
  k_ra8_mstp_spi1,
};

/**
 * @enum ra8_spi_b_poll_t
 * @brief Polling-loop budget. Used to bound HW waits.
 */
typedef enum : uint32_t {
  k_ra8_spi_b_poll_limit = 200000U, /**< RA8 SPI b poll limit. */
} ra8_spi_b_poll_t;

/**
 * @enum ra8_spi_b_default_t
 * @brief Default register values for the legacy ``ra8_spi_controller_init`` shim.
 *
 * @details
 * These are the pre-existing defaults retained so the legacy
 * ``ra8_spi_controller_init`` API continues to work (mode 0, no LSB
 * first, ~1.9 MHz at PCLKA = 125 MHz). FSP encodes the same
 * concept in its default extended config.
 */
typedef enum : uint32_t {
  k_ra8_spi_b_default_baud_hz  = 1900000UL,   /**< RA8 SPI b default baud Hz.  */
  k_ra8_spi_b_default_pclka_hz = 125000000UL, /**< RA8 SPI b default pclka Hz. */
} ra8_spi_b_default_t;

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
 * Per-channel runtime state
 * =============================================================================
 */

/**
 * @struct ra8_spi_state_t
 * @brief Per-channel dispatch state owned by this driver.
 */
typedef struct {
  ra8_spi_complete_fn_t cb;          /**< Transfer-complete callback.  */
  void*                 ctx;         /**< Callback context.            */
  bool                  initialized; /**< True after ``ra8_spi_init``. */
} ra8_spi_state_t;

/**
 * @var s_spi_state
 * @brief Per-channel state table indexed by channel.
 */
static ra8_spi_state_t s_spi_state[k_ra8_spi_b_channel_count];

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
 * @retval k_ra8_ok Operation succeeded.
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
  if (result > (uint32_t)k_ra8_spbr_max) {
    return k_ra8_spbr_max;
  }
  return (uint8_t)result;
}

/**
 * @brief Build SPCMD0 from a ``ra8_spi_cfg_t``.
 *
 * @details
 * Bit-mapping (HUM Ch 43.2.7 p 2893, FSP ``r_spi_b_hw_config``):
 *  - ``CPHA``  (bit 0)  from ``cfg->mode``.
 *  - ``CPOL``  (bit 1)  from ``cfg->mode``.
 *  - ``LSBF``  (bit 12) from ``cfg->lsb_first``.
 *  - ``SPB``   [20:16]  set to 8-bit frame (k_ra8_spcmd_spb_8bit).
 *  - Delay enables (SPNDEN/SLNDEN/SCKDEN) are left clear; the
 *    bring-up driver does not gate delay registers.
 *
 * @param[in] cfg See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_spcmd(const ra8_spi_cfg_t* cfg)
{
  uint32_t v = 0U;
  /* CPHA / CPOL match SPI mode 0..3. */
  if ((cfg->mode == k_ra8_spi_mode_1) || (cfg->mode == k_ra8_spi_mode_3)) {
    v |= k_ra8_spcmd_mask_cpha;
  }
  if ((cfg->mode == k_ra8_spi_mode_2) || (cfg->mode == k_ra8_spi_mode_3)) {
    v |= k_ra8_spcmd_mask_cpol;
  }
  if (cfg->lsb_first) {
    v |= k_ra8_spcmd_mask_lsbf;
  }
  /* 8-bit frame. */
  v |= ((uint32_t)k_ra8_spcmd_spb_8bit << k_ra8_spcmd_bit_spb_lo) & k_ra8_spcmd_mask_spb;
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
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_spcr_controller(void)
{
  uint32_t v = 0U;
  v |= k_ra8_spcr_mask_mstr;   /* Controller mode. */
  v |= k_ra8_spcr_mask_sckase; /* Auto-stop SCK.   */
  v |= k_ra8_spcr_mask_spe;    /* SPI enable.      */
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
static ra8_err_t internal_wait_spsr(volatile r_spi_regs_t* reg, uint32_t flag_mask)
{
  /* Bounded busy-poll of SPSR. On the host test build the ra8_hw_err MMIO fault
   * seam (ra8_fake_mmio_*) drives this real loop to succeed-after-N or to time out,
   * so both the success and timeout legs are exercised on host (T1-01) rather
   * than compiled out behind an RA8_OFF_TARGET short-circuit. On target it is
   * a plain register spin with a fixed iteration bound. */
  return ra8_hw_wait_flag_set32(&reg->SPSR, flag_mask, (uint32_t)k_ra8_spi_b_poll_limit);
}

/* =============================================================================
 * Lifecycle: init / deinit
 * =============================================================================
 */

/**
 * @brief Programme the polling-controller register set with SPE=0.
 *
 * @details Writes SPCR3 / SPDECR / SPCR2 / SPCMD0 / SPDCR(2) / SPFCR
 * in the order the HUM allows while SPE is still 0. SPCR2 carries the
 * loopback knob (SPLP2 non-inverting); SPSR flags are cleared once
 * before and once after SPFRST so the first ra8_spi_xfer8 sees a clean
 * SPRF.
 *
 * @param[in] reg Channel's register block.
 * @param[in] cfg Caller-supplied config (already null-checked).
 *
 * @pre SPCR.SPE has been cleared.
 * @pre MSTP is already enabled for the channel.
 * @post All control registers programmed; SPSR flags clear.
 * @post SPE is still 0 -- caller writes SPCR with SPE=1.
 * @note Not thread-safe; caller must serialize access to the channel.
 * @since 0.1.0
 */
static void internal_spi_program_regs(volatile r_spi_regs_t* reg, const ra8_spi_cfg_t* cfg)
{
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra8_spsrc_mask_all;

  /* HUM Ch 43.2.6 "SPCR3 : SPI Control Register 3" p 2891 */
  const uint8_t spbr = internal_spbr(cfg->baud_hz, cfg->pclka_hz);
  reg->SPCR3         = ((uint32_t)spbr << k_ra8_spcr3_bit_spbr) & k_ra8_spcr3_mask_spbr;

  /* HUM Ch 43.2.3 "SPDECR : SPI Delay Control Register" p 2883 */
  reg->SPDECR = 0U;

  /* SPCR2 only honors writes while SPE=0; SPLP2 (bit 17) is the */
  /* non-inverting loopback (rx = tx). */
  /* HUM Ch 43.2.4 "SPCR2 : SPI Control Register 2" p 2889 */
  reg->SPCR2 = (cfg->loopback ? (uint32_t)k_ra8_spcr2_mask_splp2 : 0U);

  /* HUM Ch 43.2.7 "SPCMDm : SPI Command Register" p 2893 */
  reg->SPCMD[0] = internal_spcmd(cfg);

  /* HUM Ch 43.2.10 "SPDCR : SPI Data Control Register" p 2896 */
  reg->SPDCR  = 0U;
  reg->SPDCR2 = 0U;

  /* HUM Ch 43.2.14 "SPFCR : SPI FIFO Clear Register" p 2906 */
  reg->SPFCR = k_ra8_spfcr_mask_spfrst;

  /* SPFRST drains residual FIFO contents through the shifter and */
  /* can leave SPRF set with a stale 0x00, so the first xfer8 */
  /* would race; re-clear SPSR right before the SPE assert. */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra8_spsrc_mask_all;
}

ra8_err_t ra8_spi_init(uint8_t channel, const ra8_spi_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "spi_init: cfg");
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_spi_mstp_table[channel]);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "spi_init: mstp"); /* GCOVR_EXCL_BR_LINE */

  /* Disable (SPE=0) before reprogramming. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR = 0U;

  internal_spi_program_regs(reg, cfg);

  /* Re-enable with SPE+MSTR set. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR = internal_spcr_controller();

  s_spi_state[channel].cb          = nullptr;
  s_spi_state[channel].ctx         = nullptr;
  s_spi_state[channel].initialized = true;
  ra8_log_info_val(s_tag, "spi_init channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_spi_deinit(uint8_t channel)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Clear SPE. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR                        = 0U;
  s_spi_state[channel].cb          = nullptr;
  s_spi_state[channel].ctx         = nullptr;
  s_spi_state[channel].initialized = false;
  return ra8_mstp_disable(s_spi_mstp_table[channel]);
}

/* =============================================================================
 * Legacy polling shim
 * =============================================================================
 */

ra8_err_t ra8_spi_controller_init(uint8_t channel)
{
  const ra8_spi_cfg_t cfg = {
    .baud_hz   = k_ra8_spi_b_default_baud_hz,
    .pclka_hz  = k_ra8_spi_b_default_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  /* The pre-existing test contract distinguishes "channel out of range"
   * with k_ra8_err_null_ptr (because ra8_spi() returns nullptr). Preserve. */
  if (ra8_spi(channel) == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return ra8_spi_init(channel, &cfg);
}

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
static ra8_err_t internal_unit_bytes(ra8_spi_bit_width_t bit_width, uint8_t* out_bytes)
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
static void internal_apply_bit_width(volatile r_spi_regs_t* reg, ra8_spi_bit_width_t bit_width)
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
static void internal_push_unit(volatile r_spi_regs_t* reg,
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
static void
internal_pop_unit(volatile r_spi_regs_t* reg, void* rx, uint32_t idx, ra8_spi_bit_width_t bit_width)
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
static ra8_err_t internal_xfer_common(uint8_t             channel,
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
  // mcdc-deactivated: TU-local helper internal_apply_bit_width null-pair guard; the public-API ra8_spi_b_transfer entry validates that at least one of (tx, rx) is non-NULL before calling this helper, so the AND's two conditions cannot both be true on any reachable path -- defensive depth guard only.
  if ((tx == nullptr) && (rx == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
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

/* =============================================================================
 * Runtime reconfigure
 * =============================================================================
 */

ra8_err_t ra8_spi_set_clock(uint8_t channel, uint32_t baud_hz, uint32_t pclka_hz)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  if (baud_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* Update SPCR3.SPBR[15:8]. */
  /* HUM Ch 43.2.6 "SPCR3 : SPI Control Register 3" p 2891 */
  const uint8_t  spbr     = internal_spbr(baud_hz, pclka_hz);
  const uint32_t spcr3_in = reg->SPCR3 & ~k_ra8_spcr3_mask_spbr;
  reg->SPCR3 = spcr3_in | (((uint32_t)spbr << k_ra8_spcr3_bit_spbr) & k_ra8_spcr3_mask_spbr);
  return k_ra8_ok;
}

/* =============================================================================
 * Error status
 * =============================================================================
 */

ra8_err_t ra8_spi_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "spi get_errors");
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile const r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  const uint32_t ss = reg->SPSR;
  uint8_t        m  = k_ra8_spi_err_none;
  if ((ss & k_ra8_spsr_mask_ovrf) != 0U) {
    m |= k_ra8_spi_err_overrun;
  }
  if ((ss & k_ra8_spsr_mask_modf) != 0U) {
    m |= k_ra8_spi_err_mode;
  }
  if ((ss & k_ra8_spsr_mask_perf) != 0U) {
    m |= k_ra8_spi_err_parity;
  }
  if ((ss & k_ra8_spsr_mask_udrf) != 0U) {
    m |= k_ra8_spi_err_underrun;
  }
  *out_mask = m;
  return k_ra8_ok;
}

ra8_err_t ra8_spi_clear_errors(uint8_t channel)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* SPI_B status flags clear via SPSRC (write-1). */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = k_ra8_spsr_mask_errs;
  return k_ra8_ok;
}

ra8_err_t ra8_spi_attach_transfer_handler(uint8_t channel, ra8_spi_complete_fn_t fn, void* ctx)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  s_spi_state[channel].cb  = fn;
  s_spi_state[channel].ctx = ctx;
  return k_ra8_ok;
}

/* =============================================================================
 * Power transition
 * =============================================================================
 */

ra8_err_t ra8_spi_enter_stop(uint8_t channel)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Clear SPE. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR = 0U;
  return ra8_mstp_disable(s_spi_mstp_table[channel]);
}

ra8_err_t ra8_spi_exit_stop(uint8_t channel)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_enable(s_spi_mstp_table[channel]);
}

/* =============================================================================
 * ISR dispatch -- placeholder; real IRQ routing lands with NVIC wiring.
 * =============================================================================
 */

RA8_ISR_SAFE
void ra8_spi_dispatch_spti(uint8_t channel)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return;
  }
  (void)s_spi_state[channel].cb;
}

RA8_ISR_SAFE
void ra8_spi_dispatch_spri(uint8_t channel)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return;
  }
  (void)s_spi_state[channel].cb;
}

RA8_ISR_SAFE
void ra8_spi_dispatch_spei(uint8_t channel)
{
  if (channel >= k_ra8_spi_b_channel_count) {
    return;
  }
  uint8_t mask = 0U;
  (void)ra8_spi_get_errors(channel, &mask);
  (void)ra8_spi_clear_errors(channel);
  const ra8_spi_complete_fn_t cb = s_spi_state[channel].cb;
  if ((mask != 0U) && (cb != nullptr)) {
    cb(s_spi_state[channel].ctx, mask);
  }
}
