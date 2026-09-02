/**
 * @file ra8_spi_b_target.c
 * @brief SPI_B peripheral (target) mode driver -- polling single-byte xfer
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Companion translation unit to ``ra8_spi_b.c`` for the RA8D2 SPI_B
 * (Type-B SPI) peripheral. Where ``ra8_spi_b.c`` implements the
 * controller role -- the MCU drives RSPCK and SSL --
 * this file implements the target (peripheral) role in which an
 * EXTERNAL controller drives RSPCK and asserts SSL (CS), and the MCU
 * responds on CIPO (SDO) while receiving on COPI (SDI).
 *
 * Register-level design (HUM Ch 43 "Serial Peripheral Interface" p 2877):
 *
 *  - ``ra8_spi_b_target_init`` mirrors ``ra8_spi_init`` from
 *    ``ra8_spi_b.c`` but omits SPBR (irrelevant: clock is external) and
 *    writes SPCR with MSTR=0 (target mode) instead of MSTR=1. SCKASE
 *    (SCK auto-stop, bit 12) is left clear -- it is only meaningful in
 *    controller mode (HUM Ch 43.3.14 p 2912).
 *
 *  - ``ra8_spi_b_target_xfer`` performs a polled full-duplex single-byte
 *    exchange in the target role:
 *      1. Waits for SPSR.SPTEF (TX buffer empty -- safe to load the
 *         response byte).
 *      2. Writes the caller's TX byte to SPDR (CIPO data).
 *      3. Clears SPTEF via SPSRC.
 *      4. Waits for SPSR.SPRF (RX buffer full -- controller has clocked
 *         a complete frame in).
 *      5. Reads the COPI byte from SPDR.
 *      6. Clears SPRF via SPSRC.
 *
 * The SPSR flag waits run the same bounded polling loop on target and on
 * the host unit-test build; on host the ``ra8_hw_err`` MMIO fault seam
 * (``ra8_fake_mmio_*``) drives that real loop to succeed-after-N or to time
 * out, so both the success and timeout legs execute on host. See
 * ``internal_target_wait_spsr`` for the full rationale.
 *
 * @note This driver does NOT share the ``s_spi_state`` table in
 *       ``ra8_spi_b.c`` (that table is ``static`` to its TU). To tear
 *       down a channel initialised with ``ra8_spi_b_target_init``, call
 *       ``ra8_spi_deinit(channel)`` from ``ra8_spi.h``.
 *
 * Inclusive terminology (this driver's names -> RA8D2 register effect):
 *  - Controller role = SPCR.MSTR=1 (the MCU drives RSPCK / SSL)
 *  - Peripheral / target role = SPCR.MSTR=0 (an external controller drives the bus)
 *  - CS (Chip Select) = SSL pin, asserted by the external controller
 *  - COPI (Controller Out Peripheral In) = SDI data received from the controller
 *  - CIPO (Controller In Peripheral Out) = SDO data this peripheral drives out
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

static const char* s_tag = "SPI_B_TGT";

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @var s_spi_mstp_table
 * @brief Channel-index -> MSTP id (HUM Ch 11.2.7 "MSTPCRB", p 444).
 *
 * @details Matches the table in ``ra8_spi_b.c``; this TU manages its own
 *          MSTP state rather than calling into the controller driver.
 * @note Read-only after initialisation; thread-safe for concurrent reads.
 * @warning Do not modify directly.
 * @since 0.1.0
 */
static const ra8_mstp_t s_spi_mstp_table[k_ra8_spi_b_channel_count] = {
  k_ra8_mstp_spi0,
  k_ra8_mstp_spi1,
};

/**
 * @enum ra8_spi_b_target_poll_t
 * @brief Polling-loop budget for SPSR flag waits in target mode.
 *
 * @details Matches ``k_ra8_spi_b_poll_limit`` in ``ra8_spi_b.c`` so that
 *          both controller and target paths share the same worst-case
 *          timeout budget per flag wait. The value is large enough to
 *          cover the maximum SPI frame period at the minimum supported
 *          bit-rate (about 10 kHz) across the SPI_B clock divider range.
 */
typedef enum : uint32_t {
  k_spi_b_target_poll_limit = 200000U, /**< Max iterations per flag wait. */
} ra8_spi_b_target_poll_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Wait for a single SPSR flag to assert, with a bounded poll.
 *
 * @details
 * Delegates to ``ra8_hw_wait_flag_set32``, a bounded polling loop
 * (NASA P10 Rule 2) that spins up to ``k_spi_b_target_poll_limit``
 * iterations before returning ``k_ra8_err_hw_timeout``. That loop is
 * consulted by the host-test MMIO fault seam (``ra8_fake_mmio_*``): a
 * test pre-staging SPSR with the awaited flag succeeds on the first
 * poll (seam transparent), ``fail_wait`` drives the timeout leg, and
 * ``satisfy_after(n)`` steps the loop's continuation branch for MC/DC.
 * Both the success and timeout legs therefore run on host, rather than
 * being compiled out behind an ``RA8_OFF_TARGET`` single-shot
 * short-circuit (T1-01).
 *
 * @param[in] reg       Pointer to the channel's SPI_B register block.
 * @param[in] flag_mask Single SPSR flag mask (e.g. ``k_ra8_spsr_mask_sprf``).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Flag asserted within the poll budget.
 * @retval k_ra8_err_hw_timeout  Flag did not assert within the budget.
 *
 * @pre ``reg`` is a valid, mapped SPI_B register block pointer.
 * @pre ``flag_mask`` is one of the single-bit ``k_ra8_spsr_mask_*`` constants.
 * @post On ``k_ra8_ok`` the indicated SPSR flag is still asserted (caller
 *       must clear via SPSRC).
 * @post On ``k_ra8_err_hw_timeout`` no SPSR flag has been cleared.
 *
 * @note Not thread-safe; designed for single-caller polling paths.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_target_wait_spsr(volatile r_spi_regs_t* reg,
                                                        uint32_t               flag_mask)
{
  /* Bounded busy-poll of SPSR. On the host test build the ra8_hw_err MMIO fault
   * seam (ra8_fake_mmio_*) drives this real loop to succeed-after-N or to time out,
   * so both the success and timeout legs are exercised on host (T1-01) rather
   * than compiled out behind an RA8_OFF_TARGET short-circuit. On target it is
   * a plain register spin with a fixed iteration bound. */
  /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
  return ra8_hw_wait_flag_set32(&reg->SPSR, flag_mask, (uint32_t)k_spi_b_target_poll_limit);
}

/**
 * @brief Build the SPCR value for target (peripheral) mode.
 *
 * @details
 * Sets SPE (enable) and MODFEN (mode fault detection) while leaving
 * MSTR=0 (peripheral/target role), SCKASE=0 (controller-only feature),
 * and all interrupt-enable bits clear (polling driver). See HUM
 * Ch 43.2.4 "SPCR : SPI Control Register" p 2884 and HUM Ch 43.3.14
 * "SPI peripheral-mode operation" p 2912 for the register specification.
 *
 * @return SPCR value suitable for writing to the register.
 * @retval (k_ra8_spcr_mask_spe | k_ra8_spcr_mask_modfen)  The only value ever
 *         returned: SPE and MODFEN set; MSTR and all interrupt-enable bits
 *         left clear.
 *
 * @pre Called only from ``ra8_spi_b_target_init`` after SPE has been cleared.
 * @pre The channel's MSTP gate is already open.
 * @post Bit MSTR (bit 30) is clear in the returned value (target mode).
 * @post Bit SPE  (bit  0) is set in the returned value (function enable).
 *
 * @note Pure; no side effects; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_target_spcr(void)
{
  uint32_t v = 0U;
  /* SPCR.MSTR (bit 30) is NOT set -- Peripheral (Target) mode. */
  /* SPCR.SCKASE (bit 12) is NOT set -- valid in controller mode only. */
  v |= (uint32_t)k_ra8_spcr_mask_spe;    /* SPI Function Enable.        */
  v |= (uint32_t)k_ra8_spcr_mask_modfen; /* Mode Fault Error Detection. */
  return v;
}

/**
 * @brief Build SPCMD0 value from a caller-supplied configuration.
 *
 * @details
 * Encodes CPHA (bit 0), CPOL (bit 1), LSBF (bit 12), and SPB[20:16]
 * from the public ``ra8_spi_cfg_t``. The bit-rate divider and SSL-select
 * fields are left at zero (clock is external; SSL is an input in target
 * mode). See HUM Ch 43.2.7 "SPCMDm : SPI Command Register" p 2893.
 *
 * A switch statement is used in place of ``||`` chains so that compiler
 * MC/DC instrumentation tracks each mode as a distinct branch rather
 * than a compound boolean.
 *
 * @param[in] cfg Caller-supplied configuration; must be non-NULL (validated
 *                by ``ra8_spi_b_target_init`` before this call).
 *
 * @return SPCMD0 value ready to write to ``reg->SPCMD[0]``.
 * @retval SPCMD0  CPHA/CPOL bits encode ``cfg->mode``; LSBF set when
 *         ``cfg->lsb_first`` is true; SPB[4:0] set to ``k_ra8_spcmd_spb_8bit``;
 *         all other fields (bit-rate divider, SSL-select) left zero.
 *
 * @pre ``cfg`` is non-NULL.
 * @pre ``cfg->mode`` is one of ``k_ra8_spi_mode_0`` through
 *      ``k_ra8_spi_mode_3``.
 * @post Returned CPHA/CPOL bits match the requested SPI mode.
 * @post Returned SPB[4:0] encodes an 8-bit frame (k_ra8_spcmd_spb_8bit).
 *
 * @note Pure; no side effects; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_spcmd_target(const ra8_spi_cfg_t* cfg)
{
  uint32_t v = 0U;
  switch (cfg->mode) {
    case k_ra8_spi_mode_1:
      v |= (uint32_t)k_ra8_spcmd_mask_cpha;
      break;
    case k_ra8_spi_mode_2:
      v |= (uint32_t)k_ra8_spcmd_mask_cpol;
      break;
    case k_ra8_spi_mode_3:
      v |= (uint32_t)k_ra8_spcmd_mask_cpha;
      v |= (uint32_t)k_ra8_spcmd_mask_cpol;
      break;
    case k_ra8_spi_mode_0:
    default:
      break;
  }
  if (cfg->lsb_first) {
    v |= (uint32_t)k_ra8_spcmd_mask_lsbf;
  }
  /* 8-bit frame. */
  v |= ((uint32_t)k_ra8_spcmd_spb_8bit << k_ra8_spcmd_bit_spb_lo) & k_ra8_spcmd_mask_spb;
  return v;
}

/**
 * @brief Write all control registers while SPCR.SPE=0.
 *
 * @details
 * Called from ``ra8_spi_b_target_init`` after SPCR has been cleared to
 * zero (SPE=0). All registers that the HUM restricts to writes while
 * SPE=0 (SPCR2, SPCMD0) are programmed here. SPCR3 is written with
 * zero because SPBR is irrelevant in target mode (the external controller
 * drives RSPCK). The FIFO is reset via SPFCR and SPSR flags are cleared
 * twice (before and after the FIFO reset) to ensure the first call to
 * ``ra8_spi_b_target_xfer`` sees a clean status.
 *
 * @param[in] reg  Channel register block; validated non-NULL by caller.
 * @param[in] cfg  Caller-supplied config; validated non-NULL by caller.
 *
 * @pre SPCR.SPE has been cleared (``reg->SPCR == 0``).
 * @pre MSTP gate for the channel is already open.
 * @post All control registers are programmed for target mode.
 * @post SPSR flags are cleared; FIFO contents are flushed.
 *
 * @note Not thread-safe; caller must serialise channel access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_target_program_regs(volatile r_spi_regs_t* reg,
                                                      const ra8_spi_cfg_t*   cfg)
{
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = (uint32_t)k_ra8_spsrc_mask_all;

  /* SPBR=0 in target mode: clock is external; field ignored by HW. */
  /* HUM Ch 43.2.6 "SPCR3 : SPI Control Register 3" p 2891 */
  reg->SPCR3 = 0U;

  /* HUM Ch 43.2.3 "SPDECR : SPI Delay Control Register" p 2883 */
  reg->SPDECR = 0U;

  /* No loopback in target mode. SPCR2 must be written while SPE=0. */
  /* HUM Ch 43.2.4 "SPCR2 : SPI Control Register 2" p 2889 */
  reg->SPCR2 = 0U;

  /* HUM Ch 43.2.7 "SPCMDm : SPI Command Register" p 2893 */
  reg->SPCMD[0] = internal_spcmd_target(cfg);

  /* HUM Ch 43.2.10 "SPDCR : SPI Data Control Register" p 2896 */
  reg->SPDCR  = 0U;
  reg->SPDCR2 = 0U;

  /* HUM Ch 43.2.14 "SPFCR : SPI FIFO Clear Register" p 2906 */
  reg->SPFCR = (uint32_t)k_ra8_spfcr_mask_spfrst;

  /* Re-clear status after FIFO reset in case SPRF was set by flush. */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = (uint32_t)k_ra8_spsrc_mask_all;
}

/* =============================================================================
 * Public API -- definitions (declarations + Doxygen live in ra8_spi.h)
 * =============================================================================
 */

ra8_err_t ra8_spi_b_target_init(uint8_t channel, const ra8_spi_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "target_init: cfg");
  if (channel >= (uint8_t)k_ra8_spi_b_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE -- bounded channel yields non-null reg */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE -- bounded channel yields non-null reg    */
  }
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_spi_mstp_table[channel]);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "target_init: mstp");
  /* GCOVR_EXCL_BR_STOP */

  /* Disable (SPE=0) before reprogramming. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  reg->SPCR = 0U;

  internal_target_program_regs(reg, cfg);

  /* Enable SPI in target/peripheral mode: MSTR=0, MODFEN=1, SPE=1. */
  /* HUM Ch 43.2.4 "SPCR : SPI Control Register" p 2884 */
  /* HUM Ch 43.3.14 "SPI peripheral-mode operation" p 2912 */
  reg->SPCR = internal_target_spcr();

  ra8_log_info_val(s_tag, "target_init channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_spi_b_target_xfer(uint8_t channel, uint8_t tx, uint8_t* rx)
{
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "target_xfer: channel out of range");

  /* Wait until the TX buffer is empty (safe to pre-load response). */
  /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
  ra8_err_t err = internal_target_wait_spsr(reg, (uint32_t)k_ra8_spsr_mask_sptef);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Pre-load the response byte -- shifted out on CIPO when the external */
  /* controller asserts CS and provides RSPCK. */
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  reg->SPDR = (uint32_t)tx;

  /* Clear TX-empty flag (write-1-to-clear). */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = (uint32_t)k_ra8_spsrc_mask_sptefc;

  /* Wait for the external controller to complete a full frame (SPRF). */
  /* HUM Ch 43.2.9 "SPSR : SPI Status Register" p 2898 */
  err = internal_target_wait_spsr(reg, (uint32_t)k_ra8_spsr_mask_sprf);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Drain the byte received on COPI from the RX FIFO front-end. */
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  const uint8_t received = (uint8_t)reg->SPDR;

  /* Clear RX-full flag. */
  /* HUM Ch 43.2.13 "SPSRC : SPI Status Clear Register" p 2905 */
  reg->SPSRC = (uint32_t)k_ra8_spsrc_mask_sprfc;

  if (rx != nullptr) {
    *rx = received;
  }
  return k_ra8_ok;
}
