/**
 * @file ra_sdhi.c
 * @brief SD/MMC Host Interface driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 SDHI block. Covers lifecycle + status + IRQ +
 * power-transition surface plus the command primitive set:
 * `ra_sdhi_send_command` (load SD_ARG + SD_CMD, poll SD_INFO1.RSPEND,
 * copy SD_RSP*) and `ra_sdhi_set_clock` for runtime bus-rate change.
 * Block data transfer (SD_BUF FIFO + DMA bounce buffer) is left to
 * the first card-stack consumer because the protocol decisions
 * (4-bit / 8-bit, SDR/DDR) belong to the consumer.
 *
 * The bring-up sequence in ::ra_sdhi_init mirrors FSP r_sdhi.c:
 *   1. clear MSTP gate (`R_BSP_MODULE_START`)
 *   2. clear stale SD_INFO1
 *   3. SOFT_RST = 0; SOFT_RST = 1   (assert / release reset)
 *   4. SD_CLK_CTRL = 0x20            (8x divisor, auto-control off)
 *   5. SDIO_MODE / SD_DMAEN / SDIF_MODE / EXT_SWAP -> 0
 *   6. SD_OPTION = 0x40E0            (1-bit wide bus, default timeouts)
 *   7. SD_INFO1_MASK / SD_INFO2_MASK -> 0 (caller-supplied IRQs land
 *      via ::ra_sdhi_attach_handler)
 *
 * Every register access carries a HUM Ch 47 "SD/MMC Host Interface
 * (SDHI)" citation (HUM pages 3122-3179, Chapter 47 covers the full
 * SDHI block).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sdhi.h"

#include <stdint.h>

#include "ra8d2_mstp_regs.h"
#include "ra8d2_sdhi_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "SDHI";

/**
 * @var s_sdhi_mstp_table
 * @brief Per-instance MSTP id lookup.
 */
static const ra_mstp_t s_sdhi_mstp_table[k_ra_sdhi_instance_count] = {
  k_ra_mstp_sdhi0,
  k_ra_mstp_sdhi1,
};

static ra_sdhi_event_fn_t s_sdhi_fn;
static void*              s_sdhi_ctx;

/**
 * @brief SDHI register-bring-up constants mirrored from FSP r_sdhi.c.
 *
 * @details
 * These values match `SDHI_PRV_SD_OPTION_DEFAULT`,
 * `SDHI_PRV_SD_CLK_CTRL_DEFAULT` and the soft-reset toggle used in
 * `r_sdhi_hw_cfg()`. CLKSEL = 0x20 selects PCLKB / 64 -- the slowest
 * divider available, suitable for the 400 kHz identification phase.
 * SD_OPTION 0x40E0 = WIDTH=1 (1-bit bus), TOP=0xE (max timeout),
 * CTOP=0 (default card-detect counter).
 */
typedef enum : uint32_t {
  k_ra_sdhi_soft_rst_assert         = 0x00000000U, /**< drive RST low */
  k_ra_sdhi_soft_rst_release        = 0x00000001U, /**< release RST */
  k_ra_sdhi_clk_ctrl_default        = 0x00000020U, /**< CLKSEL=64, CLKEN/CLKCTRLEN=0 */
  k_ra_sdhi_clk_ctrl_clken_mask     = 0x00000100U, /**< SD_CLK_CTRL.CLKEN bit-8 */
  k_ra_sdhi_clk_ctrl_clkctrlen_mask = 0x00000200U, /**< SD_CLK_CTRL.CLKCTRLEN bit-9 */
  k_ra_sdhi_clk_ctrl_setting_mask   = 0x000000FFU, /**< CLKSEL[7:0] */
  k_ra_sdhi_option_default          = 0x000040E0U, /**< 1-bit bus, default timeouts */
  k_ra_sdhi_info1_rspend_mask       = 0x00000001U, /**< SD_INFO1.RSPEND bit-0 */
} ra_sdhi_init_const_t;

/**
 * @brief Bounded spin budget for ::ra_sdhi_send_command response wait.
 *
 * @details
 * The host test fires a 100 us SIGALRM to set RSPEND asynchronously;
 * the budget must be large enough that the loop is still spinning
 * when the signal lands under macOS / Linux scheduler jitter. 2M
 * spins on a 1 GHz CPU is still only ~2 ms of wall clock -- a sane
 * SDHI command-response timeout on real silicon.
 */
typedef enum : uint32_t {
  k_ra_sdhi_cmd_spin = 2000000U,
} ra_sdhi_timing_t;

ra_err_t ra_sdhi_init(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra_err_t mst_err = ra_mstp_enable(s_sdhi_mstp_table[instance]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "sdhi_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3140 */
  /* Drop any stale flags before reset so the card-detection latch is
   * not interpreted as an event by the consumer. */
  reg->SD_INFO1 = 0U;

  /* HUM Ch 47.2.31 "SOFT_RST : Software Reset Register" p 3173 */
  /* Mirror FSP r_sdhi_hw_cfg(): assert reset (SDRST=0), release
   * reset (SDRST=1). This brings every status flag and bus-state
   * machine to a known idle state. */
  reg->SOFT_RST = k_ra_sdhi_soft_rst_assert;
  reg->SOFT_RST = k_ra_sdhi_soft_rst_release;

  /* HUM Ch 47.2.18 "SD_CLK_CTRL : SD Clock Control Register" p 3145 */
  /* Slowest CLKSEL (PCLKB/64) with CLKEN/CLKCTRLEN cleared; the
   * card-stack consumer will retune for 25 MHz / 50 MHz once
   * identification completes. */
  reg->SD_CLK_CTRL = k_ra_sdhi_clk_ctrl_default;

  /* HUM Ch 47.2.27 "SDIO_MODE : SDIO Mode Control Register" p 3169 */
  /* HUM Ch 47.2.30 "SD_DMAEN : DMA Mode Enable Register" p 3172 */
  /* HUM Ch 47.2.32 "SDIF_MODE : SD Interface Mode Setting" p 3174 */
  /* HUM Ch 47.2.33 "EXT_SWAP : Swap Control Register" p 3175 */
  /* Disable SDIO mode, DMA, MMC test-CRC mask, and endian-swap so
   * the FIFO interface starts in the canonical big-endian-aware
   * configuration. */
  reg->SDIO_MODE = 0U;
  reg->SD_DMAEN  = 0U;
  reg->SDIF_MODE = 0U;
  reg->EXT_SWAP  = 0U;

  /* HUM Ch 47.2.20 "SD_OPTION : SD Card Access Control Option" p 3148 */
  /* 1-bit-wide bus, default timeout counters. The consumer flips
   * WIDTH/WIDTH8 once the card reports its supported bus widths. */
  reg->SD_OPTION = k_ra_sdhi_option_default;

  /* HUM Ch 47.2.17 "SD_INFO2_MASK : SD Card Interrupt Mask 2" p 3144 */
  /* HUM Ch 47.2.16 "SD_INFO1_MASK : SD Card Interrupt Mask 1" p 3142 */
  /* All interrupts masked off until ::ra_sdhi_attach_handler installs
   * a callback. */
  reg->SD_INFO1_MASK = 0U;
  reg->SD_INFO2_MASK = 0U;
  reg->SD_INFO2      = 0U;

  ra_log_info_val(s_tag, "sdhi_init inst", (uint32_t)instance);
  return k_ra_ok;
}

ra_err_t ra_sdhi_deinit(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 47.2.31 "SOFT_RST : Software Reset Register" p 3173 */
  /* Hold the IP in reset before clearing per-register state and
   * gating the module clock. */
  reg->SOFT_RST = k_ra_sdhi_soft_rst_assert;

  /* HUM Ch 47.2.15 "SD_INFO1 / SD_INFO2 / masks" p 3140 */
  reg->SD_INFO1      = 0U;
  reg->SD_INFO2      = 0U;
  reg->SD_INFO1_MASK = 0U;
  reg->SD_INFO2_MASK = 0U;
  reg->SD_CLK_CTRL   = 0U;
  return ra_mstp_disable(s_sdhi_mstp_table[instance]);
}

ra_err_t ra_sdhi_send_command(uint8_t   instance,
                              uint32_t  cmd,
                              uint32_t  arg,
                              uint32_t* out_rsp) // NOLINT(readability-non-const-parameter)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 47.2.2 "SD_ARG / SD_ARG1 : SD Command Argument" p 3128 */
  /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3125 */
  /* Load SD_ARG, then SD_CMD kicks the command out on the bus.
   * FSP r_sdhi.c r_sdhi_command_send_no_wait() splits the argument
   * into the legacy SD_ARG1 (upper 16 bits) plus SD_ARG (lower 16
   * bits); on RA8D2 SD_ARG is a full 32-bit register so a single
   * write is sufficient. */
  reg->SD_ARG = arg;
  reg->SD_CMD = cmd;

  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3140 */
  /* Poll SD_INFO1.RSPEND (bit 0) with bounded spin budget. */
  for (uint32_t i = 0U; i < k_ra_sdhi_cmd_spin; ++i) {
    if ((reg->SD_INFO1 & k_ra_sdhi_info1_rspend_mask) != 0U) {
      if (out_rsp != nullptr) {
        /* HUM Ch 47.2.5 "SD_RSP10..SD_RSP76 : Response Registers" p 3132 */
        out_rsp[0] = reg->SD_RSP10;
        out_rsp[1] = reg->SD_RSP32;
        out_rsp[2] = reg->SD_RSP54;
        out_rsp[3] = reg->SD_RSP76;
      }
      reg->SD_INFO1 = reg->SD_INFO1 & ~k_ra_sdhi_info1_rspend_mask;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_sdhi_set_clock(uint8_t instance, uint32_t divider)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47.2.18 "SD_CLK_CTRL : SD Clock Control Register" p 3145 */
  /* Preserve CLKEN (bit 8) and CLKCTRLEN (bit 9) -- caller controls
   * only the CLKSEL[7:0] divider. FSP r_sdhi_set_clock() snapshots
   * CLKCTRLEN, OR's CLKEN back in, and writes the merged value;
   * mirror that semantics so a runtime divider change does not
   * silently disable the SD bus clock. */
  const uint32_t preserved =
    reg->SD_CLK_CTRL & (k_ra_sdhi_clk_ctrl_clkctrlen_mask | k_ra_sdhi_clk_ctrl_clken_mask);
  reg->SD_CLK_CTRL = (divider & k_ra_sdhi_clk_ctrl_setting_mask) | preserved;
  return k_ra_ok;
}

ra_err_t ra_sdhi_get_status(uint8_t instance, uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3140 */
  *out_mask = reg->SD_INFO1;
  return k_ra_ok;
}

ra_err_t ra_sdhi_clear_status(uint8_t instance, uint32_t mask)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  RA_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3140 */
  /* SD_INFO1 bits are write-0-to-clear. Read-modify-write so unset
   * bits in `mask` continue to read 1 (canonical pattern from FSP
   * r_sdhi_access_irq_process()). */
  reg->SD_INFO1 = reg->SD_INFO1 & ~mask;
  return k_ra_ok;
}

ra_err_t ra_sdhi_attach_handler(ra_sdhi_event_fn_t fn, void* ctx)
{
  s_sdhi_fn  = fn;
  s_sdhi_ctx = ctx;
  return k_ra_ok;
}

void ra_sdhi_dispatch(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra_sdhi(instance);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3140 */
  const uint32_t           mask = reg->SD_INFO1;
  const ra_sdhi_event_fn_t fn   = s_sdhi_fn;
  void* const              ctx  = s_sdhi_ctx;
  reg->SD_INFO1                 = 0U;
  if (fn != nullptr) {
    fn(ctx, instance, mask);
  }
}

ra_err_t ra_sdhi_enter_stop(uint8_t instance)
{
  if (instance >= k_ra_sdhi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_disable(s_sdhi_mstp_table[instance]);
}

ra_err_t ra_sdhi_exit_stop(uint8_t instance)
{
  if (instance >= k_ra_sdhi_instance_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_sdhi_mstp_table[instance]);
}
