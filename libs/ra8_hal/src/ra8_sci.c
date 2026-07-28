/**
 * @file ra8_sci.c
 * @brief Full-featured SCI_B driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * SCI_B variant of the RA8D2 SCI peripheral (HUM Ch 38). See
 * `ra8_sci.h` for the public API contract and `ra8_sci_regs.h` for
 * the register layout. This file replaces the prior legacy-SCI driver
 * stub (the legacy 8-bit-register variant is not present on RA8D2).
 *
 * Asynchronous (UART) bring-up sequence implemented in `ra8_sci_init`:
 *   1. Open the per-channel MSTP gate.
 *   2. Clear CCR0 so TE/RE/TIE/RIE/TEIE are off before reconfiguring.
 *   3. Programme CCR1 (parity / inverter / break-data defaults).
 *   4. Programme CCR3 (mode = async, CHR = data length, STP = stop
 *      bits, MOD = 0 for async/multi-proc, MP/FM/DEN/CKE all clear so
 *      the channel uses the on-chip baud generator with no FIFO).
 *   5. Programme CCR2 with BRR computed from PCLKB and the requested
 *      baud (CKS = 0, BGDM = 0, ABCS = 0 -- the standard 16x base
 *      clock path), MDDR left at reset (0xFF -> no modulation).
 *   6. Disable FIFO mode and Manchester / LIN / IIC satellite bits.
 *   7. Clear every CSR / FFCLR latch left over from a prior boot
 *      (mirrors FSP `R_SCI_B_UART_Open` lines 375 & 378).
 *   8. Set CCR0 = TE | RE.
 *
 * @par Intentional FSP Gaps
 * The driver follows the FSP `r_sci_b_uart` reference closely but
 * deliberately omits four steps that do not apply to our usage:
 *
 *   - **`r_sci_b_uart.c` (CCR0 IDSEL pre-seed).** FSP pre-loads
 *     CCR0 with the IDSEL bit when the multi-processor bit is being
 *     turned on. IDSEL is only meaningful when CCR3.MP=1; this driver
 *     never enables multi-processor mode (see `internal_ccr3` -- MOD
 *     stays 000 / async and MP stays 0), so the bit is dead and we
 *     skip the extra write.
 *   - **`r_sci_b_uart.c` (`r_sci_b_uart_synchronization_delay_cfg`).**
 *     The FSP delay loop accounts for the synchronizer hop between
 *     SCICLK and PCLK when those clocks are sourced independently.
 *     In our async-UART configuration the on-chip baud generator is
 *     fed from PCLKB (CCR3.CKE = 00, CCR3.BPEN = 1 -- see
 *     `internal_ccr3`), so SCICLK and PCLK are the same edge and FSP's
 *     own delay-count formula evaluates to zero. The wait is a no-op
 *     for us and is intentionally not ported.
 *   - **`r_sci_b_uart.c` (`SCI_B_UART_FCR_DEFAULT_VALUE = 0x1F1F0000`).**
 *     FSP seeds FCR with RTRG=31 / TTRG=31 even when FIFO mode is
 *     off. RTRG/TTRG are dead bits when CCR3.FM=0 (HUM Ch 38.2.11
 *     p 2215, "valid only when FM = 1"); we keep `FCR = 0` here since
 *     we never enable FIFO mode.
 *   - **`r_sci_b_uart.c` (Close clears CCR3.FM before TE drop).**
 *     FSP's Close path explicitly toggles FM off because there is a
 *     documented hang where TE -> 0 with FM=1 leaves CSR.TEND stuck
 *     at 0 and the peripheral wedged. Since this driver never sets
 *     FM=1, the workaround is unnecessary and `ra8_sci_deinit` writes
 *     CCR0=0 directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sci.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_register_guard.h"
#include "ra8_sci_internal.h"
#include "ra8_sci_regs.h"

static const char* s_tag = "SCI";

/* =============================================================================
 * Per-channel state
 * =============================================================================
 */

/**
 * @var s_sci_state
 * @brief Per-channel allocation + dispatch table.
 *
 * @details
 * Canonical definition of the cross-TU dispatch table declared
 * ``extern`` in ``ra8_sci_internal.h``; ``ra8_sci_dma_isr.c`` reads and
 * mutates the same storage from the ISR dispatch path.
 */
ra8_sci_state_t s_sci_state[k_ra8_sci_channel_count_val];

/**
 * @var s_mstp_table
 * @brief Channel-index -> MSTP id lookup.
 */
static const ra8_mstp_t s_mstp_table[k_ra8_sci_channel_count_val] = {
  k_ra8_mstp_sci0,
  k_ra8_mstp_sci1,
  k_ra8_mstp_sci2,
  k_ra8_mstp_sci3,
  k_ra8_mstp_sci4,
  k_ra8_mstp_sci5,
  k_ra8_mstp_sci6,
  k_ra8_mstp_sci7,
  k_ra8_mstp_sci8,
  k_ra8_mstp_sci9,
};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate ``channel`` and return the register pointer.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_sci_regs_t* internal_reg(uint8_t channel)
{
  if (channel > k_ra8_sci_channel_max_index) {
    return nullptr;
  }
  return ra8_sci(channel);
}

/**
 * @brief Compute the 8-bit BRR value from a target baud and PCLKB.
 *
 * @details
 * HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189
 * Table 38.7. For the default Asynchronous-mode 16x base-clock path
 * (CCR2.BGDM = ABCS = ABCSE = ABCSE2 = 0, CCR3.CKE = 0, CCR2.CKS = 0
 * -> n = 0):
 *
 * @f[ N = \frac{TCLK}{64 \cdot 2^{(2n - 1)} \cdot B} - 1
 *       = \frac{TCLK}{32 \cdot B} - 1 @f]
 *
 * Saturates at 0 if the requested baud is unreachable.
 * @param[in] pclk_hz See declaration: ``uint32_t pclk_hz``.
 * @param[in] baud See declaration: ``uint32_t baud``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static uint8_t internal_brr(uint32_t pclk_hz, uint32_t baud)
{
  if ((baud == 0U) || (pclk_hz == 0U)) {
    return 0U;
  }
  const uint32_t divisor = k_ra8_sci_brr_async_divisor * baud;
  const uint32_t n       = pclk_hz / divisor;
  if (n == 0U) {
    return 0U;
  }
  return (uint8_t)(n - 1U);
}

/**
 * @brief Build the CCR1 value for an async-UART config descriptor.
 *
 * @details HUM Ch 38.2.6 "CCR1 : Common Control Register 1", p 2185.
 * Always sets SPB2DT + SPB2IO so TXD idles HIGH while TE=0 -- without
 * those bits the line floats low and a host UART sees a permanent
 * break, blocking the very first frame. FSP r_sci_b_uart does the
 * same write unconditionally for async-UART configs. Parity is set
 * per `cfg->parity`; the rest (CTSE/CTSPEN/TINV/RINV/SPLP/SHARPS/
 * NFEN) stay at their reset value.
 * @param[in] cfg See declaration: ``const ra8_sci_cfg_t* cfg``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static uint32_t internal_ccr1(const ra8_sci_cfg_t* cfg)
{
  uint32_t ccr1 = (1U << k_ra8_sci_ccr1_bit_spb2dt) | (1U << k_ra8_sci_ccr1_bit_spb2io);
  if (cfg->parity != k_ra8_sci_parity_none) {
    ccr1 |= (1U << k_ra8_sci_ccr1_bit_pe);
    if (cfg->parity == k_ra8_sci_parity_odd) {
      ccr1 |= (1U << k_ra8_sci_ccr1_bit_pm);
    }
  }
  return ccr1;
}

/**
 * @brief Build the CCR3 value for an async-UART config descriptor.
 *
 * @details HUM Ch 38.2.8 "CCR3 : Common Control Register 3", p 2203.
 * MOD = 000 (asynchronous), CHR = 8-bit / 7-bit, STP = 0/1 stop bit
 * = 1 / 2 stop bits. CKE = 00 (on-chip baud generator). FM = 0
 * (non-FIFO). MP = 0 (single-processor). All other bits stay 0.
 * @param[in] cfg See declaration: ``const ra8_sci_cfg_t* cfg``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static uint32_t internal_ccr3(const ra8_sci_cfg_t* cfg)
{
  /* LSBF = 1 (LSB-first) is the UART standard wire order. SCI_B's
   * reset state is MSB-first; without this bit the host receives
   * each byte bit-reversed (e.g. 'h' = 0x68 transmits as 0x16). FSP
   * r_sci_b_uart sets LSBF unconditionally for async configs.
   *
   * BPEN = 1 (Synchronizer Bypass Enable) is required when the bus
   * clock (PCLK) is also used as the operation clock (TCLK) -- which
   * is what we're doing in async mode with the on-chip baud-rate
   * generator on the synchronized clock. Without BPEN the SCI's
   * shift state machine waits forever for an independent SCICLK
   * edge that never arrives, and the chip looks alive at the
   * register level (TDR latches, CCR0.TE=1) but never advances --
   * CSR.TDRE and CSR.TEND stay 0 indefinitely. HUM Ch 38.2.8 p 2207
   * "BPEN bit" is the authoritative source. */
  uint32_t ccr3 = (1U << k_ra8_sci_ccr3_bit_lsbf) | (1U << k_ra8_sci_ccr3_bit_bpen);

  /* MOD = 000 (Asynchronous) -- already 0. */

  /* CHR[1:0]. 8-bit -> 10b, 7-bit -> 11b. */
  if (cfg->data_bits == k_ra8_sci_data_7) {
    ccr3 |= (k_ra8_sci_ccr3_chr_7bit << k_ra8_sci_ccr3_shift_chr);
  } else {
    ccr3 |= (k_ra8_sci_ccr3_chr_8bit << k_ra8_sci_ccr3_shift_chr);
  }

  /* STP -- 1 = 2 stop bits. */
  if (cfg->stop_bits == k_ra8_sci_stop_2) {
    ccr3 |= (1U << k_ra8_sci_ccr3_bit_stp);
  }

  return ccr3;
}

/**
 * @brief Clear every stale CSR / FFCLR latch on a freshly-opened channel.
 *
 * @details
 * Mirrors FSP `r_sci_b_uart.c` (`p_ctrl->p_reg->CFCLR =
 * SCI_B_UART_CFCLR_DEFAULT`) and `r_sci_b_uart.c`
 * (`p_ctrl->p_reg->FFCLR = SCI_B_UART_FFCLR_DEFAULT`). Both clear
 * registers are write-1-to-clear: writing the "all bits" mask drops
 * every defined latch in a single store while leaving the reserved
 * bits at 0. Without this step, residual flags from a prior boot
 * (e.g. ORER set by a stray RX framing error) would surface as a
 * spurious error the moment we re-enable RIE/TIE.
 * @param[in] reg See declaration: ``volatile r_sci_regs_t* reg``.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static void internal_clear_csr_flags(volatile r_sci_regs_t* reg)
{
  /* HUM Ch 38.2.24 "CFCLR : Common Flag Clear Register", p 2238 --
   * one write clears ERS / DCMF / DPER / DFER / ORER / MFF / PER /
   * FER / TDRE / RDRF in CSR. */
  reg->CFCLR = k_ra8_sci_cfclr_default;

  /* HUM Ch 38.2.26 "FFCLR : FIFO Flag Clear Register", p 2239 --
   * clears FRSR.DR (the only defined W1C bit). */
  reg->FFCLR = k_ra8_sci_ffclr_default;
}

/**
 * @brief Spin until CSR.TEND = 1 or the bounded budget runs out.
 *
 * @details
 * Mirrors FSP `r_sci_b_uart.c` and `:809`
 * (`FSP_HARDWARE_REGISTER_WAIT(p_ctrl->p_reg->CSR_b.TEND, 1U)`). TDRE
 * (transmit data register empty) is asserted as soon as TDR is
 * latched into the shift register, but the bits are not yet on the
 * wire. TEND additionally waits for the shift register to drain. We
 * use a bounded medium-budget spin so the call returns in finite
 * time even if the line is wedged.
 *
 * On the host (`RA8_OFF_TARGET`) the fake does not model the
 * shift-register drain -- `*reg` would never see TEND assert -- so we
 * short-circuit and return success.
 *
 * @param[in,out] reg See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra8_err_t internal_wait_tx_end(volatile r_sci_regs_t* reg)
{
  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- TEND (bit 30) goes
   * high when both the data register and the shift register are empty. The
   * ra8_fake_mmio host fault seam drives this real poll on the unit-test build, so
   * the success and timeout legs run on host (T1-01) rather than short-circuit. */
  const uint32_t mask = (1U << k_ra8_sci_csr_bit_tend);
  return ra8_hw_wait_flag_set32(&reg->CSR, mask, k_ra8_hw_budget_medium);
}

/**
 * @brief Build the CCR2 value with BRR programmed.
 *
 * @details HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189.
 * MDDR field reset value is 0xFF (modulation-disabled equivalent),
 * so we keep it at 0xFF and program BRR[15:8] only. CKS = 0,
 * BGDM = ABCS = ABCSE = ABCSE2 = 0 -- the 16x base-clock path.
 * @param[in] cfg See declaration: ``const ra8_sci_cfg_t* cfg``.
 * @return ::ra8_err_t outcome (or scalar return value).
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static uint32_t internal_ccr2(const ra8_sci_cfg_t* cfg)
{
  const uint8_t brr  = internal_brr(cfg->pclk_hz, cfg->baud);
  uint32_t      ccr2 = 0U;
  ccr2 |= ((uint32_t)brr << k_ra8_sci_ccr2_shift_brr);
  /* MDDR reset value -- keep modulation off. */
  ccr2 |= (k_ra8_sci_mddr_default << k_ra8_sci_ccr2_shift_mddr);
  return ccr2;
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Program CCR0..CCR4 + FCR for the requested UART config.
 *
 * @details
 * Performs the deterministic MMIO write sequence required between
 * MSTP-enable and the final TE/RE strobe: CCR0=0 (disable), FCR=0
 * (non-FIFO), CCR1/CCR3/CCR2 from the cached helpers, CCR4=0, then
 * the CFCLR / FFCLR latch clear. The write order is identical to
 * FSP r_sci_b_uart.c.
 *
 * @param[in,out] reg Channel register bank, non-NULL.
 * @param[in]     cfg Validated UART configuration.
 *
 * @pre Caller has enabled the module-stop clock for this channel.
 * @pre ``reg`` is the canonical bank pointer for the active channel.
 * @post CCR0=0 (TX/RX still disabled until ra8_sci_init re-strobes it).
 * @post FCR=0, CCR1/CCR2/CCR3 programmed, CCR4 cleared, flags cleared.
 *
 * @note Not thread-safe; called once during init under IRQ-masked context.
 * @since 0.1.0
 */
static void internal_program_ccr_bank(volatile r_sci_regs_t* reg, const ra8_sci_cfg_t* cfg)
{
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- disable
   * TX/RX/IE bits before reconfiguring CCR1..CCR4 and FCR. */
  reg->CCR0 = 0U;

  /* HUM Ch 38.2.11 "FCR : FIFO Control Register", p 2215 -- non-FIFO
   * polling mode for the bring-up demo: TFRST/RFRST cleared, all
   * trigger numbers reset to 0. */
  reg->FCR = 0U;

  /* HUM Ch 38.2.6 "CCR1 : Common Control Register 1", p 2185 */
  reg->CCR1 = internal_ccr1(cfg);

  /* HUM Ch 38.2.8 "CCR3 : Common Control Register 3", p 2203 -- mode
   * + framing must be programmed before TE/RE go high. */
  reg->CCR3 = internal_ccr3(cfg);

  /* HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189 -- BRR
   * derived from cfg->pclk_hz and cfg->baud. */
  reg->CCR2 = internal_ccr2(cfg);

  /* HUM Ch 38.2.9 "CCR4 : Common Control Register 4", p 2210 -- no
   * sample / transmit timing adjustment for async UART. */
  reg->CCR4 = 0U;

  /* HUM Ch 38.2.24 "CFCLR : Common Flag Clear Register", p 2238 +
   * HUM Ch 38.2.26 "FFCLR : FIFO Flag Clear Register", p 2239 -- drop
   * any latches inherited from a previous boot before TX/RX go live.
   * Mirrors FSP r_sci_b_uart.c. */
  internal_clear_csr_flags(reg);
}

ra8_err_t ra8_sci_init(uint8_t channel, const ra8_sci_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "sci_init: cfg");
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 445 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_mstp_table[channel]);
  if (mst_err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra8_log_error_val(s_tag, "sci_init: mstp enable failed", (uint32_t)mst_err);
    return k_ra8_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  internal_program_ccr_bank(reg, cfg);

  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- enable
   * transmitter and receiver. Interrupt-enable bits are toggled
   * separately by ra8_sci_attach_{rx,tx}_handler. */
  reg->CCR0 = (1U << k_ra8_sci_ccr0_bit_te) | (1U << k_ra8_sci_ccr0_bit_re);

  s_sci_state[channel].initialized = true;
  s_sci_state[channel].tx_buf      = nullptr;
  s_sci_state[channel].tx_len      = 0U;
  s_sci_state[channel].tx_idx      = 0U;
  s_sci_state[channel].rx_buf      = nullptr;
  s_sci_state[channel].rx_len      = 0U;
  s_sci_state[channel].rx_idx      = 0U;
  ra8_log_info_val(s_tag, "sci_init channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_deinit(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  /* Drop CCR0 and tear the async descriptor down atomically w.r.t. the
   * TXI/RXI ISR. Without the mask, a pending interrupt can still observe a
   * non-zero tx_len/rx_len after CCR0 is cleared and then dereference the
   * buffer pointer this path is nulling -- a NULL deref in interrupt
   * context. Mirrors the ra8_sci_abort teardown. */
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 */
  reg->CCR0                        = 0U;
  s_sci_state[channel].rx_fn       = nullptr;
  s_sci_state[channel].rx_ctx      = nullptr;
  s_sci_state[channel].tx_fn       = nullptr;
  s_sci_state[channel].tx_ctx      = nullptr;
  s_sci_state[channel].initialized = false;
  s_sci_state[channel].tx_buf      = nullptr;
  s_sci_state[channel].tx_len      = 0U;
  s_sci_state[channel].tx_idx      = 0U;
  s_sci_state[channel].rx_buf      = nullptr;
  s_sci_state[channel].rx_len      = 0U;
  s_sci_state[channel].rx_idx      = 0U;
  ra8_register_guard_exit(&guard);
  return ra8_mstp_disable(s_mstp_table[channel]);
}

/* ---- Polling TX / RX -------------------------------------------------- */

ra8_err_t ra8_sci_putc_polling(uint8_t channel, uint8_t byte)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- spin
   * until TDRE = 1 (transmit data register empty). */
  const uint32_t  mask = (1U << k_ra8_sci_csr_bit_tdre);
  const ra8_err_t werr = ra8_hw_wait_flag_set32(&reg->CSR, mask, k_ra8_hw_budget_medium);
  if (werr != k_ra8_ok) {
    return werr;
  }
  /* HUM Ch 38.2.3 "TDR : Transmit Data Register", p 2181 -- write to
   * TDAT[7:0] (low 8 bits of TDR) launches one frame in non-FIFO
   * 8-bit async mode. */
  reg->TDR = (uint32_t)byte;
  return k_ra8_ok;
}

ra8_err_t ra8_sci_getc_polling(uint8_t channel, uint8_t* out_byte)
{
  RA8_CHECK_NULL_PTR(out_byte, s_tag, "getc: out_byte");
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- spin
   * until RDRF = 1 (receive data full). */
  const uint32_t  mask = (1U << k_ra8_sci_csr_bit_rdrf);
  const ra8_err_t werr = ra8_hw_wait_flag_set32(&reg->CSR, mask, k_ra8_hw_budget_medium);
  if (werr != k_ra8_ok) {
    return werr;
  }
  /* HUM Ch 38.2.2 "RDR : Receive Data Register", p 2180 -- RDAT[7:0]
   * holds the byte just received in 8-bit async mode. */
  *out_byte = (uint8_t)(reg->RDR & k_ra8_sci_rdr_mask_data8);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_write_polling(uint8_t channel, const uint8_t* data, uint32_t len)
{
  if ((data == nullptr) && (len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    const ra8_err_t err = ra8_sci_putc_polling(channel, data[i]);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- TDRE
   * goes high as soon as TDR latches into the shifter, but the byte
   * may still be on the wire. Wait for TEND so the call only returns
   * after the last frame is fully transmitted. Mirrors FSP
   * r_sci_b_uart.c (`R_SCI_B_UART_Close` blocks on TEND for the
   * same reason before dropping TE). */
  if (len != 0U) {
    return internal_wait_tx_end(reg);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_sci_flush(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- block on
   * TEND so the shift register has fully drained before the caller
   * proceeds (typically into a panic_halt / WFI that would gate the
   * SCI clock and discard in-flight bytes). */
  return internal_wait_tx_end(reg);
}

/* ---- Interrupt handler attach ---------------------------------------- */

ra8_err_t ra8_sci_attach_rx_handler(uint8_t channel, ra8_sci_rx_fn_t fn, void* ctx)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* The RXI ISR reads rx_fn/rx_ctx and read-modify-writes CCR0; mask it
   * while we publish the handler and toggle RIE so neither a torn callback
   * pointer nor a lost CCR0 update is observable. */
  const uint32_t       rie = (1U << k_ra8_sci_ccr0_bit_rie);
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  s_sci_state[channel].rx_fn  = fn;
  s_sci_state[channel].rx_ctx = ctx;
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- toggle
   * RIE (bit 16). */
  if (fn != nullptr) {
    reg->CCR0 = reg->CCR0 | rie;
  } else {
    reg->CCR0 = reg->CCR0 & ~rie;
  }
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_attach_tx_handler(uint8_t channel, ra8_sci_tx_fn_t fn, void* ctx)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* The TXI ISR reads tx_fn/tx_ctx and read-modify-writes CCR0; mask it
   * while we publish the handler and toggle TIE. */
  const uint32_t       tie = (1U << k_ra8_sci_ccr0_bit_tie);
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  s_sci_state[channel].tx_fn  = fn;
  s_sci_state[channel].tx_ctx = ctx;
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- toggle
   * TIE (bit 20). */
  if (fn != nullptr) {
    reg->CCR0 = reg->CCR0 | tie;
  } else {
    reg->CCR0 = reg->CCR0 & ~tie;
  }
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}

/* ---- Error status ----------------------------------------------------- */

ra8_err_t ra8_sci_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "get_errors: out");
  volatile const r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t mask = k_ra8_sci_err_none;
  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- read the
   * three error flags out of the 32-bit status word. */
  const uint32_t csr = reg->CSR;
  if ((csr & (1U << k_ra8_sci_csr_bit_orer)) != 0U) {
    mask |= k_ra8_sci_err_overrun;
  }
  if ((csr & (1U << k_ra8_sci_csr_bit_fer)) != 0U) {
    mask |= k_ra8_sci_err_framing;
  }
  if ((csr & (1U << k_ra8_sci_csr_bit_per)) != 0U) {
    mask |= k_ra8_sci_err_parity;
  }
  *out_mask = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_sci_clear_errors(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.24 "CFCLR : Common Flag Clear Register", p 2238 --
   * write-1-to-clear lines for ORER / FER / PER. */
  reg->CFCLR = (1U << k_ra8_sci_cfclr_bit_orerc) | (1U << k_ra8_sci_cfclr_bit_ferc) |
               (1U << k_ra8_sci_cfclr_bit_perc);
  return k_ra8_ok;
}

/* ---- Runtime reconfigure --------------------------------------------- */

ra8_err_t ra8_sci_set_baud(uint8_t channel, uint32_t baud, uint32_t pclk_hz)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (baud == 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint8_t brr = internal_brr(pclk_hz, baud);
  /* Guard the CCR2 read-modify-write against any SCI ISR that stores to
   * this channel's control registers: an interrupt landing between the
   * CCR2 read and the write-back would otherwise drop the freshly merged
   * BRR field (lost update). The mask is a no-op when uncontended, so the
   * final CCR2 value is byte-identical to the unguarded path. */
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  /* HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189 -- BRR
   * lives in CCR2[15:8]; preserve the rest of CCR2. */
  uint32_t v = reg->CCR2;
  v &= ~k_ra8_sci_ccr2_mask_brr_field;
  v |= ((uint32_t)brr << k_ra8_sci_ccr2_shift_brr);
  reg->CCR2 = v;
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}

/* ---- Power transition ------------------------------------------------- */

ra8_err_t ra8_sci_enter_stop(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 */
  reg->CCR0 = 0U;
  return ra8_mstp_disable(s_mstp_table[channel]);
}

ra8_err_t ra8_sci_exit_stop(uint8_t channel)
{
  if (channel > k_ra8_sci_channel_max_index) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_enable(s_mstp_table[channel]);
}

/* ---- Async byte-stream TX / RX (FSP Read/Write parity) --------------- */

/**
 * @enum ra8_sci_baud_calc_const_t
 * @brief Constants used by ``ra8_sci_baud_calculate``.
 *
 * @details
 * HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189 Table 38.7:
 * the 16x base-clock formula divides PCLKB by ``32 * 2^(2n)`` where
 * ``n`` is the CKS divider (0..3). With ``n = 0`` the divisor is 32;
 * with ``n = 1`` it is 128; with ``n = 2`` it is 512; with ``n = 3``
 * it is 2048. ``k_ra8_sci_baud_brr_max`` is the 8-bit ceiling for BRR.
 */
typedef enum : uint16_t {
  k_ra8_sci_baud_brr_max    = 255U, /**< BRR is 8 bits wide.      */
  k_ra8_sci_baud_cks_max    = 3U,   /**< CKS field is 2 bits.     */
  k_ra8_sci_baud_div_step   = 4U,   /**< Multiplier per CKS step. */
  k_ra8_sci_baud_n0_divisor = 32U,  /**< 32 * 2^(2*0).            */
} ra8_sci_baud_calc_const_t;

ra8_err_t
ra8_sci_baud_calculate(uint32_t baud, uint32_t pclk_hz, uint16_t* brr_out, uint8_t* clk_div_out)
{
  RA8_CHECK_NULL_PTR(brr_out, s_tag, "baud_calc: brr_out");
  RA8_CHECK_NULL_PTR(clk_div_out, s_tag, "baud_calc: clk_div_out");
  if ((baud == 0U) || (pclk_hz == 0U)) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189 -- walk
   * CKS = 0..3 and pick the smallest divider that yields a BRR <= 255.
   * Mirrors the FSP `R_SCI_B_UART_BaudCalculate` outer loop
   * (r_sci_b_uart.c) but without the bit-rate-modulation pass
   * since the project always programs BRME=0. */
  uint32_t divisor = (uint32_t)k_ra8_sci_baud_n0_divisor;
  for (uint8_t n = 0U; n <= (uint8_t)k_ra8_sci_baud_cks_max; ++n) {
    const uint32_t denom = divisor * baud;
    if (denom == 0U) {              /* GCOVR_EXCL_BR_LINE -- baud * divisor cannot
                          overflow to zero for legal inputs. */
      return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE */
    }
    const uint32_t quotient = pclk_hz / denom;
    if (quotient > 0U) {
      const uint32_t candidate = quotient - 1U;
      if (candidate <= (uint32_t)k_ra8_sci_baud_brr_max) {
        *brr_out     = (uint16_t)candidate;
        *clk_div_out = n;
        return k_ra8_ok;
      }
    }
    divisor *= (uint32_t)k_ra8_sci_baud_div_step;
  }
  return k_ra8_err_invalid_arg;
}

ra8_err_t ra8_sci_write(uint8_t channel, const uint8_t* data, uint32_t len)
{
  if ((data == nullptr) && (len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_sci_state[channel].initialized) {
    return k_ra8_err_invalid_arg;
  }
  if (s_sci_state[channel].tx_len != 0U) {
    return k_ra8_err_busy;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }
  /* Publish the async TX descriptor and arm TIE atomically w.r.t. the
   * TXI ISR: it tests tx_len then dereferences tx_buf, so a torn publish
   * (or a CCR0 RMW racing the ISR's TIE clear) must not be observable. */
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  s_sci_state[channel].tx_buf = data;
  s_sci_state[channel].tx_len = len;
  s_sci_state[channel].tx_idx = 0U;
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- arm
   * TIE so the next TDRE event fires the dispatcher. Mirrors FSP
   * r_sci_b_uart.c which sets TE | TIE in a single store. */
  reg->CCR0 = reg->CCR0 | (1U << k_ra8_sci_ccr0_bit_tie);
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_read(uint8_t channel, uint8_t* buf, uint32_t len)
{
  if ((buf == nullptr) && (len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_sci_state[channel].initialized) {
    return k_ra8_err_invalid_arg;
  }
  if (s_sci_state[channel].rx_len != 0U) {
    return k_ra8_err_busy;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }
  /* Publish the async RX descriptor and arm RIE atomically w.r.t. the
   * RXI ISR (it tests rx_len then writes through rx_buf). */
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  s_sci_state[channel].rx_buf = buf;
  s_sci_state[channel].rx_len = len;
  s_sci_state[channel].rx_idx = 0U;
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- arm
   * RIE so the next RDRF event fires the dispatcher. Mirrors FSP
   * r_sci_b_uart.c which stashes p_rx_dest / rx_dest_bytes for
   * use by `rxi_isr`. */
  reg->CCR0 = reg->CCR0 | (1U << k_ra8_sci_ccr0_bit_rie);
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_abort(uint8_t channel, ra8_sci_dir_t direction)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((direction != k_ra8_sci_dir_tx) && (direction != k_ra8_sci_dir_rx) &&
      (direction != k_ra8_sci_dir_both)) {
    return k_ra8_err_invalid_arg;
  }
  /* Disarm and tear down the async descriptor atomically w.r.t. the
   * TXI/RXI ISR. Without the mask, the ISR can latch tx_len > 0, take an
   * interrupt while this path nulls tx_buf, then dereference the stale
   * NULL -- a use-after-free / NULL deref in interrupt context.
   * HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 */
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  if ((direction & k_ra8_sci_dir_tx) != 0U) {
    const uint32_t tie_teie     = (1U << k_ra8_sci_ccr0_bit_tie) | (1U << k_ra8_sci_ccr0_bit_teie);
    reg->CCR0                   = reg->CCR0 & ~tie_teie;
    s_sci_state[channel].tx_buf = nullptr;
    s_sci_state[channel].tx_len = 0U;
    s_sci_state[channel].tx_idx = 0U;
  }
  if ((direction & k_ra8_sci_dir_rx) != 0U) {
    const uint32_t rie          = (1U << k_ra8_sci_ccr0_bit_rie);
    reg->CCR0                   = reg->CCR0 & ~rie;
    s_sci_state[channel].rx_buf = nullptr;
    s_sci_state[channel].rx_len = 0U;
    s_sci_state[channel].rx_idx = 0U;
  }
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_read_stop(uint8_t channel, uint32_t* remaining)
{
  RA8_CHECK_NULL_PTR(remaining, s_tag, "read_stop: remaining");
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* Mirror FSP `R_SCI_B_UART_ReadStop` r_sci_b_uart.c: stash the
   * pre-stop count, zero state, then disarm RIE. */
  /* Snapshot the residual count, null the descriptor, and disarm RIE
   * atomically w.r.t. the RXI ISR so the reported remaining matches the
   * state we tear down (and the ISR cannot deref a half-nulled rx_buf). */
  const uint32_t       rie = (1U << k_ra8_sci_ccr0_bit_rie);
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  const uint32_t pending      = (s_sci_state[channel].rx_len > s_sci_state[channel].rx_idx)
                                  ? (s_sci_state[channel].rx_len - s_sci_state[channel].rx_idx)
                                  : 0U;
  *remaining                  = pending;
  s_sci_state[channel].rx_buf = nullptr;
  s_sci_state[channel].rx_len = 0U;
  s_sci_state[channel].rx_idx = 0U;
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 */
  reg->CCR0 = reg->CCR0 & ~rie;
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_receive_suspend(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- drop
   * RE to silence the RX shift register. FSP returns UNSUPPORTED here;
   * we approximate the feature by toggling CCR0.RE so RXI stops firing
   * and the FIFO/RDR stops accepting fresh frames. Guard the RMW against
   * the RXI ISR's own CCR0 update. */
  const uint32_t       re = (1U << k_ra8_sci_ccr0_bit_re);
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  reg->CCR0 = reg->CCR0 & ~re;
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}

ra8_err_t ra8_sci_receive_resume(uint8_t channel)
{
  volatile r_sci_regs_t* reg = internal_reg(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- guard the
   * RE RMW against the RXI ISR's concurrent CCR0 update. */
  const uint32_t       re = (1U << k_ra8_sci_ccr0_bit_re);
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  reg->CCR0 = reg->CCR0 | re;
  ra8_register_guard_exit(&guard);
  return k_ra8_ok;
}
