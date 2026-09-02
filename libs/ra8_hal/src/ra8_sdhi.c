/**
 * @file ra8_sdhi.c
 * @brief SD/MMC Host Interface driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 SDHI block. Covers lifecycle + status + IRQ +
 * power-transition surface plus the command primitive set:
 * `ra8_sdhi_send_command` (load SD_ARG + SD_CMD, poll SD_INFO1.RSPEND,
 * copy SD_RSP*) and `ra8_sdhi_set_clock` for runtime bus-rate change.
 * Bus-width control is `ra8_sdhi_set_bus_width` (host-side SD_OPTION
 * WIDTH / WIDTH8) plus `ra8_sdhi_set_bus_width_4bit` (the CMD55 + ACMD6
 * card-side negotiation for SD) and `ra8_sdhi_set_bus_width_8bit` (the
 * CMD6 SWITCH negotiation for eMMC). The default stays at the
 * conservative 1-bit mode; SDR/DDR speed-class tuning still belongs to
 * the consumer.
 *
 * The bring-up sequence in ::ra8_sdhi_init mirrors FSP r_sdhi.c:
 *   1. clear MSTP gate (`R_BSP_MODULE_START`)
 *   2. clear stale SD_INFO1
 *   3. SOFT_RST = 0; SOFT_RST = 1   (assert / release reset)
 *   4. SD_CLK_CTRL = 0x20            (8x divisor, auto-control off)
 *   5. SDIO_MODE / SD_DMAEN / SDIF_MODE / EXT_SWAP -> 0
 *   6. SD_OPTION = 0xC0E0            (1-bit bus WIDTH=1, default timeouts)
 *   7. SD_INFO1_MASK / SD_INFO2_MASK -> 0 (caller-supplied IRQs land
 *      via ::ra8_sdhi_attach_handler)
 *
 * Per HUM Ch 47.2.16 the 1-bit encoding is WIDTH=1 / WIDTH8=0
 * (0xC0E0); the literal 0x40E0 used by earlier scaffold revisions is
 * actually the *4-bit* encoding (WIDTH=0 / WIDTH8=0) and was a latent
 * mislabel -- see ::k_ra8_sdhi_option_bus_1bit.
 *
 * Every register access carries a HUM Ch 47 "SD/MMC Host Interface
 * (SDHI)" citation (HUM pages 3122-3179, Chapter 47 covers the full
 * SDHI block).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_sdhi.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_sdhi_regs.h"

static const char* s_tag = "SDHI";

/**
 * @var s_sdhi_mstp_table
 * @brief Per-instance MSTP id lookup.
 */
static const ra8_mstp_t s_sdhi_mstp_table[k_ra8_sdhi_instance_count] = {
  k_ra8_mstp_sdhi0,
  k_ra8_mstp_sdhi1,
};

static ra8_sdhi_event_fn_t s_sdhi_fn;
static void*               s_sdhi_ctx;

/**
 * @brief SDHI register-bring-up constants mirrored from FSP r_sdhi.c.
 *
 * @details
 * These values match `SDHI_PRV_SD_CLK_CTRL_DEFAULT` and the soft-reset
 * toggle used in `r_sdhi_hw_cfg()`. CLKSEL = 0x20 selects PCLKB / 64
 * -- the slowest divider available, suitable for the 400 kHz
 * identification phase. The SD_OPTION default (1-bit bus, TOP=0xE max
 * timeout, CTOP=0) is built from the named field selectors in
 * ::ra8_sdhi_option_bits_t (::k_ra8_sdhi_option_bus_1bit) rather than a
 * bare literal.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_soft_rst_assert         = 0x00000000U, /**< drive RST low                */
  k_ra8_sdhi_soft_rst_release        = 0x00000001U, /**< release RST                  */
  k_ra8_sdhi_clk_ctrl_default        = 0x00000020U, /**< CLKSEL=64, CLKEN/CLKCTRLEN=0 */
  k_ra8_sdhi_clk_ctrl_clken_mask     = 0x00000100U, /**< SD_CLK_CTRL.CLKEN bit-8      */
  k_ra8_sdhi_clk_ctrl_clkctrlen_mask = 0x00000200U, /**< SD_CLK_CTRL.CLKCTRLEN bit-9  */
  k_ra8_sdhi_clk_ctrl_setting_mask   = 0x000000FFU, /**< CLKSEL[7:0]                  */
  k_ra8_sdhi_info1_rspend_mask       = 0x00000001U, /**< SD_INFO1.RSPEND bit-0        */
} ra8_sdhi_init_const_t;

/**
 * @brief Bounded spin budget for ::ra8_sdhi_send_command response wait.
 *
 * @details
 * 2M spins on a 1 GHz CPU is ~2 ms of wall clock -- a sane SDHI
 * command-response timeout on real silicon. Host tests never race this
 * budget: they assert RSPEND synchronously from the ra8_fake_mmio
 * poll-hook, which runs inline on each iteration of this loop, so the
 * flag is observed on the very next poll regardless of host load.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_cmd_spin = 2000000U, /**< RA8 SDHI cmd spin. */
} ra8_sdhi_timing_t;

ra8_err_t ra8_sdhi_init(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B" p 444 */
  const ra8_err_t mst_err = ra8_mstp_enable(s_sdhi_mstp_table[instance]);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "sdhi_init: mstp enable");
  /* GCOVR_EXCL_BR_STOP */

  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3129 */
  /* Drop any stale flags before reset so the card-detection latch is
   * not interpreted as an event by the consumer. */
  reg->SD_INFO1 = 0U;

  /* HUM Ch 47.2.31 "SOFT_RST : Software Reset Register" p 3148 */
  /* Mirror FSP r_sdhi_hw_cfg(): assert reset (SDRST=0), release
   * reset (SDRST=1). This brings every status flag and bus-state
   * machine to a known idle state. */
  reg->SOFT_RST = k_ra8_sdhi_soft_rst_assert;
  reg->SOFT_RST = k_ra8_sdhi_soft_rst_release;

  /* HUM Ch 47.2.18 "SD_CLK_CTRL : SD Clock Control Register" p 3138 */
  /* Slowest CLKSEL (PCLKB/64) with CLKEN/CLKCTRLEN cleared; the
   * card-stack consumer will retune for 25 MHz / 50 MHz once
   * identification completes. */
  reg->SD_CLK_CTRL = k_ra8_sdhi_clk_ctrl_default;

  /* HUM Ch 47.2.27 "SDIO_MODE : SDIO Mode Control Register" p 3144 */
  /* HUM Ch 47.2.30 "SD_DMAEN : DMA Mode Enable Register" p 3147 */
  /* HUM Ch 47.2.32 "SDIF_MODE : SD Interface Mode Setting" p 3148 */
  /* HUM Ch 47.2.33 "EXT_SWAP : Swap Control Register" p 3149 */
  /* Disable SDIO mode, DMA, MMC test-CRC mask, and endian-swap so
   * the FIFO interface starts in the canonical big-endian-aware
   * configuration. */
  reg->SDIO_MODE = 0U;
  reg->SD_DMAEN  = 0U;
  reg->SDIF_MODE = 0U;
  reg->EXT_SWAP  = 0U;

  /* HUM Ch 47.2.16 "SD_OPTION : SD Card Access Control Option Register" p 3139-3140 */
  /* 1-bit-wide bus (WIDTH=1, WIDTH8=0), default timeout counters. The
   * consumer flips WIDTH/WIDTH8 via ::ra8_sdhi_set_bus_width_4bit once
   * the card acknowledges ACMD6. */
  reg->SD_OPTION = (uint32_t)k_ra8_sdhi_option_bus_1bit;

  /* HUM Ch 47.2.17 "SD_INFO2_MASK : SD Card Interrupt Mask 2" p 3137 */
  /* HUM Ch 47.2.16 "SD_INFO1_MASK : SD Card Interrupt Mask 1" p 3136 */
  /* All interrupts masked off until ::ra8_sdhi_attach_handler installs
   * a callback. */
  reg->SD_INFO1_MASK = 0U;
  reg->SD_INFO2_MASK = 0U;
  reg->SD_INFO2      = 0U;

  ra8_log_info_val(s_tag, "sdhi_init inst", (uint32_t)instance);
  return k_ra8_ok;
}

ra8_err_t ra8_sdhi_deinit(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 47.2.31 "SOFT_RST : Software Reset Register" p 3148 */
  /* Hold the IP in reset before clearing per-register state and
   * gating the module clock. */
  reg->SOFT_RST = k_ra8_sdhi_soft_rst_assert;

  /* HUM Ch 47.2.15 "SD_INFO1 / SD_INFO2 / masks" p 3140 */
  reg->SD_INFO1      = 0U;
  reg->SD_INFO2      = 0U;
  reg->SD_INFO1_MASK = 0U;
  reg->SD_INFO2_MASK = 0U;
  reg->SD_CLK_CTRL   = 0U;
  return ra8_mstp_disable(s_sdhi_mstp_table[instance]);
}

ra8_err_t ra8_sdhi_send_command(uint8_t instance, uint32_t cmd, uint32_t arg, uint32_t* out_rsp)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");

  /* HUM Ch 47.2.2 "SD_ARG / SD_ARG1 : SD Command Argument" p 3128 */
  /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3123 */
  /* Load SD_ARG, then SD_CMD kicks the command out on the bus.
   * FSP r_sdhi.c r_sdhi_command_send_no_wait() splits the argument
   * into the legacy SD_ARG1 (upper 16 bits) plus SD_ARG (lower 16
   * bits); on RA8D2 SD_ARG is a full 32-bit register so a single
   * write is sufficient. */
  reg->SD_ARG = arg;
  reg->SD_CMD = cmd;

  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3129 */
  /* Poll SD_INFO1.RSPEND (bit 0) with bounded spin budget. */
  for (uint32_t i = 0U; i < k_ra8_sdhi_cmd_spin; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_poll(&reg->SD_INFO1,
                           i,
                           (reg->SD_INFO1 & k_ra8_sdhi_info1_rspend_mask) != 0U)) {
#else
    if ((reg->SD_INFO1 & k_ra8_sdhi_info1_rspend_mask) != 0U) {
#endif
      if (out_rsp != nullptr) {
        /* HUM Ch 47.2.5 "SD_RSP10..SD_RSP76 : Response Registers" p 3132 */
        out_rsp[0] = reg->SD_RSP10;
        out_rsp[1] = reg->SD_RSP32;
        out_rsp[2] = reg->SD_RSP54;
        out_rsp[3] = reg->SD_RSP76;
      }
      reg->SD_INFO1 = reg->SD_INFO1 & ~k_ra8_sdhi_info1_rspend_mask;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

ra8_err_t ra8_sdhi_set_clock(uint8_t instance, uint32_t divider)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47.2.18 "SD_CLK_CTRL : SD Clock Control Register" p 3138 */
  /* Preserve CLKEN (bit 8) and CLKCTRLEN (bit 9) -- caller controls
   * only the CLKSEL[7:0] divider. FSP r_sdhi_set_clock() snapshots
   * CLKCTRLEN, OR's CLKEN back in, and writes the merged value;
   * mirror that semantics so a runtime divider change does not
   * silently disable the SD bus clock. */
  const uint32_t preserved =
    reg->SD_CLK_CTRL & (k_ra8_sdhi_clk_ctrl_clkctrlen_mask | k_ra8_sdhi_clk_ctrl_clken_mask);
  reg->SD_CLK_CTRL = (divider & k_ra8_sdhi_clk_ctrl_setting_mask) | preserved;
  return k_ra8_ok;
}

ra8_err_t ra8_sdhi_set_bus_width(uint8_t instance, ra8_sdhi_bus_width_t width)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "set_bus_width: instance out of range");

  /* Reject anything that is not one of the three valid lane counts.
   * This compound decision (3 conditions) is MC/DC-tested in
   * test_ra8_sdhi_width.c. */
  if ((width != k_ra8_sdhi_bus_width_1bit) && (width != k_ra8_sdhi_bus_width_4bit) &&
      (width != k_ra8_sdhi_bus_width_8bit)) {
    return k_ra8_err_invalid_arg;
  }

  /* Map the lane count onto the SD_OPTION.WIDTH / WIDTH8 selector per
   * the HUM Ch 47.2.16 truth table: 1-bit is WIDTH=1 (the initial
   * value below), 4-bit is both bits clear, 8-bit is WIDTH8 only. */
  uint32_t sel = (uint32_t)k_ra8_sdhi_option_width_bit;
  if (width == k_ra8_sdhi_bus_width_4bit) {
    sel = 0U;
  } else if (width == k_ra8_sdhi_bus_width_8bit) {
    sel = (uint32_t)k_ra8_sdhi_option_width8_bit;
  }

  /* HUM Ch 47.2.16 "SD_OPTION : SD Card Access Control Option Register" p 3139-3140 */
  /* Read-modify-write: clear WIDTH+WIDTH8, OR in the new selector, and
   * preserve TOP / CTOP / TOUTMASK / reserved bit 14. */
  reg->SD_OPTION = (reg->SD_OPTION & ~(uint32_t)k_ra8_sdhi_option_width_mask) | sel;
  return k_ra8_ok;
}

ra8_err_t ra8_sdhi_set_bus_width_4bit(uint8_t instance, uint16_t rca)
{
  volatile const r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "set_bus_width_4bit: instance out of range");

  uint32_t       rsp[4]    = {0U, 0U, 0U, 0U};
  const uint32_t cmd55_arg = (uint32_t)rca << (uint32_t)k_ra8_sdhi_rca_arg_shift;

  /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3123 */
  /* CMD55 APP_CMD prefix (arg = RCA<<16) -- required before any ACMDxx. */
  const ra8_err_t e55 =
    ra8_sdhi_send_command(instance, (uint32_t)k_ra8_sdhi_cmd_app_cmd, cmd55_arg, rsp);
  RA8_RETURN_ON_ERROR(e55, s_tag, "acmd6: CMD55 timeout");
  const uint32_t cmd55_rsp = rsp[0];

  /* HUM Ch 47.2.2 "SD_ARG : SD Command Argument" p 3124 */
  /* ACMD6 SET_BUS_WIDTH -- arg bits[1:0] = 0b10 selects the 4-bit bus. */
  const ra8_err_t e6 = ra8_sdhi_send_command(instance,
                                             (uint32_t)k_ra8_sdhi_cmd_set_bus_width,
                                             (uint32_t)k_ra8_sdhi_acmd6_arg_4bit,
                                             rsp);
  RA8_RETURN_ON_ERROR(e6, s_tag, "acmd6: ACMD6 timeout");
  const uint32_t acmd6_rsp = rsp[0];

  /* Card acknowledges only when CMD55 echoed APP_CMD AND the ACMD6 R1
   * response carries no error bits. This compound decision (2
   * conditions) is MC/DC-tested in test_ra8_sdhi_width.c. On any
   * rejection the host stays at the 1-bit default. */
  const bool app_cmd_ready = (cmd55_rsp & (uint32_t)k_ra8_sdhi_r1_app_cmd_mask) != 0U;
  const bool acmd6_clean   = (acmd6_rsp & (uint32_t)k_ra8_sdhi_r1_error_mask) == 0U;
  if ((!app_cmd_ready) || (!acmd6_clean)) {
    return k_ra8_err_not_supported;
  }

  /* Card switched to 4-bit -- widen the host side to match. */
  return ra8_sdhi_set_bus_width(instance, k_ra8_sdhi_bus_width_4bit);
}

ra8_err_t ra8_sdhi_set_bus_width_8bit(uint8_t instance)
{
  volatile const r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "set_bus_width_8bit: instance out of range");

  uint32_t rsp[4] = {0U, 0U, 0U, 0U};

  /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3123 */
  /* HUM Ch 47.2.2 "SD_ARG : SD Command Argument"  p 3124 */
  /* eMMC CMD6 SWITCH -- write EXT_CSD[183] BUS_WIDTH = 2 (8-bit). This
   * is a native command with no CMD55 app prefix (unlike SD ACMD6). */
  const ra8_err_t e6 = ra8_sdhi_send_command(instance,
                                             (uint32_t)k_ra8_sdhi_cmd_emmc_switch,
                                             (uint32_t)k_ra8_sdhi_cmd6_arg_8bit,
                                             rsp);
  RA8_RETURN_ON_ERROR(e6, s_tag, "switch8: CMD6 timeout");

  /* The device accepts only when the R1b response carries no error /
   * status-violation bits; otherwise the host stays at its current
   * width. An SD card, which cannot do 8-bit, is rejected here. */
  if ((rsp[0] & (uint32_t)k_ra8_sdhi_r1_error_mask) != 0U) {
    return k_ra8_err_not_supported;
  }

  /* Device switched to 8-bit -- widen the host side to match. */
  return ra8_sdhi_set_bus_width(instance, k_ra8_sdhi_bus_width_8bit);
}

ra8_err_t ra8_sdhi_get_status(uint8_t instance, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  volatile const r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3129 */
  *out_mask = reg->SD_INFO1;
  return k_ra8_ok;
}

ra8_err_t ra8_sdhi_clear_status(uint8_t instance, uint32_t mask)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "instance out of range");
  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3129 */
  /* SD_INFO1 bits are write-0-to-clear. Read-modify-write so unset
   * bits in `mask` continue to read 1 (canonical pattern from FSP
   * r_sdhi_access_irq_process()). */
  reg->SD_INFO1 = reg->SD_INFO1 & ~mask;
  return k_ra8_ok;
}

ra8_err_t ra8_sdhi_attach_handler(ra8_sdhi_event_fn_t fn, void* ctx)
{
  s_sdhi_fn  = fn;
  s_sdhi_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_sdhi_dispatch(uint8_t instance)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3129 */
  const uint32_t            mask = reg->SD_INFO1;
  const ra8_sdhi_event_fn_t fn   = s_sdhi_fn;
  void* const               ctx  = s_sdhi_ctx;
  reg->SD_INFO1                  = 0U;
  if (fn != nullptr) {
    fn(ctx, instance, mask);
  }
}

ra8_err_t ra8_sdhi_enter_stop(uint8_t instance)
{
  if (instance >= k_ra8_sdhi_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_disable(s_sdhi_mstp_table[instance]);
}

ra8_err_t ra8_sdhi_exit_stop(uint8_t instance)
{
  if (instance >= k_ra8_sdhi_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_mstp_enable(s_sdhi_mstp_table[instance]);
}

/**
 * @brief Bounded spin budget for the per-word BRE / BWE poll.
 *
 * @details
 * One iteration of the FIFO-drain loop polls SD_INFO2.BRE (or BWE)
 * before each 4-byte access. On real hardware the flag asserts
 * within microseconds; the budget is sized so the loop tolerates a
 * 1ms scheduler glitch (roughly 1 GHz / 1 us = 1M spins) before
 * returning ``k_ra8_err_hw_timeout``.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_fifo_spin = 1000000U, /**< RA8 SDHI FIFO spin. */
} ra8_sdhi_fifo_timing_t;

/**
 * @brief Byte / shift constants for the 32-bit FIFO word splitter.
 *
 * @details
 * ``SD_BUF0`` is a 32-bit register; the polled drain reassembles
 * each word into four little-endian bytes in the destination
 * buffer. These constants name the per-byte mask and shift
 * amounts so the loop body avoids inline magic numbers.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_byte_mask = 0xFFU, /**< low-byte select mask  */
  k_ra8_sdhi_shift_b1  = 8U,    /**< bits 15..8  -> byte 1 */
  k_ra8_sdhi_shift_b2  = 16U,   /**< bits 23..16 -> byte 2 */
  k_ra8_sdhi_shift_b3  = 24U,   /**< bits 31..24 -> byte 3 */
} ra8_sdhi_byte_split_t;

/**
 * @brief Issue a data-transfer command and wait for RSPEND.
 *
 * @details
 * Loads SD_ARG with ``arg``, writes ``cmd`` (bare command index,
 * 0..63) to SD_CMD, then polls SD_INFO1.RSPEND with the existing
 * ``k_ra8_sdhi_cmd_spin`` budget. The hardware infers the data-phase
 * direction from the command index for CMD17/18/24/25, so the caller
 * does not need to pre-encode response-type or data-direction bits.
 * Mirrors the FSP r_sdhi_command_send_no_wait() prologue at
 * r_sdhi.c minus the IRQ-mask bookkeeping that the polled API
 * does not need.
 *
 * @param[in] reg SDHI register window pointer.
 * @param[in] cmd Bare SD command index (e.g. ::k_ra8_sdhi_cmd_read_single_block).
 * @param[in] arg 32-bit command argument.
 *
 * @return ``k_ra8_ok`` on RSPEND, ``k_ra8_err_hw_timeout`` otherwise.
 * @retval k_ra8_ok             RSPEND asserted; the flag has been cleared.
 * @retval k_ra8_err_hw_timeout RSPEND never asserted within
 *                              ::k_ra8_sdhi_cmd_spin polls.
 *
 * @pre ``reg`` points at a mapped SDHI register window.
 * @pre The card is out of the busy state from any prior command.
 *
 * @post SD_ARG and SD_CMD hold ``arg`` and ``cmd``.
 * @post On success SD_INFO1.RSPEND is cleared, so the next command
 *       observes a fresh flag.
 *
 * @note Not thread-safe; the polled SDHI API assumes one caller.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_sdhi_send(volatile r_sdhi_regs_t* reg, uint32_t cmd, uint32_t arg)
{
  /* HUM Ch 47.2.2 "SD_ARG : SD Command Argument" p 3124 */
  /* HUM Ch 47.2.1 "SD_CMD : Command Type Register"  p 3123 */
  reg->SD_ARG = arg;
  reg->SD_CMD = cmd;

  /* HUM Ch 47.2.15 "SD_INFO1 : SD Card Interrupt Flag Register 1" p 3129 */
  for (uint32_t i = 0U; i < k_ra8_sdhi_cmd_spin; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    if (ra8_fake_mmio_poll(&reg->SD_INFO1,
                           i,
                           (reg->SD_INFO1 & k_ra8_sdhi_info1_rspend_mask) != 0U)) {
#else
    if ((reg->SD_INFO1 & k_ra8_sdhi_info1_rspend_mask) != 0U) {
#endif
      reg->SD_INFO1 = reg->SD_INFO1 & ~k_ra8_sdhi_info1_rspend_mask;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Common data-phase setup shared by read_block / write_block.
 *
 * @details
 * Mirrors FSP ``r_sdhi_read_write_common`` (r_sdhi.c). For
 * multi-block transfers, SD_STOP.SEC must be set so the IP stops the
 * data phase after SD_SECCNT blocks; for single-block transfers
 * SD_STOP must be cleared. SD_SIZE is always 512 bytes for SD card
 * sector access.
 *
 * @param[in] reg         SDHI register window pointer.
 * @param[in] block_count Number of 512-byte blocks in this transfer.
 *
 * @pre ``reg`` points at a mapped SDHI register window.
 * @pre ``block_count`` is at least 1.
 *
 * @post SD_STOP.SEC reflects single- vs multi-block, and SD_SECCNT
 *       holds ``block_count`` for the multi-block case.
 * @post SD_SIZE is ::k_ra8_sdhi_block_bytes.
 *
 * @note Not thread-safe; the polled SDHI API assumes one caller.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_sdhi_setup_xfer(volatile r_sdhi_regs_t* reg, uint32_t block_count)
{
  /* HUM Ch 47.2.4 "SD_STOP : Data Stop Register"     p 3125 */
  /* HUM Ch 47.2.6 "SD_SECCNT : Block Count Register" p 3126 */
  if (block_count > 1U) {
    reg->SD_STOP   = k_ra8_sdhi_stop_seccnt_en;
    reg->SD_SECCNT = block_count;
  } else {
    reg->SD_STOP = 0U;
  }

  /* HUM Ch 47.2.19 "SD_SIZE : Transfer Data Length Register" p 3139 */
  reg->SD_SIZE = k_ra8_sdhi_block_bytes;
}

/**
 * @brief Drain ``words`` 32-bit FIFO words from SD_BUF0 into ``buf``.
 *
 * @details
 * Polls SD_INFO2.BRE before each 4-byte read, returning
 * ``k_ra8_err_hw_timeout`` if the BRE flag never asserts inside the
 * ``k_ra8_sdhi_fifo_spin`` budget. The split into a helper keeps
 * ::ra8_sdhi_read_block under the NASA Rule 4 (60 statements) limit.
 *
 * @param[in]  reg   SDHI register window pointer.
 * @param[out] buf   Destination byte buffer, at least ``words * 4`` bytes.
 * @param[in]  words Number of 32-bit FIFO words to drain.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             All ``words`` words were read into ``buf``.
 * @retval k_ra8_err_hw_timeout SD_INFO2.BRE never asserted within
 *                              ::k_ra8_sdhi_fifo_spin polls.
 *
 * @pre ``reg`` and ``buf`` are non-NULL.
 * @pre A read data phase is already armed (see ::internal_sdhi_setup_xfer).
 *
 * @post On success ``buf[0 .. words * 4 - 1]`` holds the FIFO payload,
 *       little-endian per word.
 * @post On timeout ``buf`` holds only the words drained before the stall.
 *
 * @note Not thread-safe; the polled SDHI API assumes one caller.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_sdhi_drain(volatile r_sdhi_regs_t* reg, uint8_t* buf, uint32_t words)
{
  uint8_t* cursor = buf;
  for (uint32_t w = 0U; w < words; ++w) {
    uint32_t spin = 0U;
    for (; spin < k_ra8_sdhi_fifo_spin; ++spin) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
      if (ra8_fake_mmio_poll(&reg->SD_INFO2,
                             spin,
                             (reg->SD_INFO2 & k_ra8_sdhi_info2_bre_mask) != 0U)) {
#else
      if ((reg->SD_INFO2 & k_ra8_sdhi_info2_bre_mask) != 0U) {
#endif
        break;
      }
    }
    if (spin >= k_ra8_sdhi_fifo_spin) {
      return k_ra8_err_hw_timeout;
    }
    const uint32_t word = reg->SD_BUF0;
    cursor[0]           = (uint8_t)(word & k_ra8_sdhi_byte_mask);
    cursor[1]           = (uint8_t)((word >> k_ra8_sdhi_shift_b1) & k_ra8_sdhi_byte_mask);
    cursor[2]           = (uint8_t)((word >> k_ra8_sdhi_shift_b2) & k_ra8_sdhi_byte_mask);
    cursor[3]           = (uint8_t)((word >> k_ra8_sdhi_shift_b3) & k_ra8_sdhi_byte_mask);
    cursor += k_ra8_sdhi_fifo_word_bytes;
  }
  return k_ra8_ok;
}

/**
 * @brief Push ``words`` 32-bit FIFO words from ``buf`` into SD_BUF0.
 *
 * @details
 * Polls SD_INFO2.BWE before each 4-byte write, returning
 * ``k_ra8_err_hw_timeout`` if the BWE flag never asserts inside the
 * ``k_ra8_sdhi_fifo_spin`` budget. Mirror image of
 * ::internal_sdhi_drain, also factored out to keep
 * ::ra8_sdhi_write_block under the NASA Rule 4 limit.
 *
 * @param[in] reg   SDHI register window pointer.
 * @param[in] buf   Source byte buffer, at least ``words * 4`` bytes.
 * @param[in] words Number of 32-bit FIFO words to push.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             All ``words`` words were written to SD_BUF0.
 * @retval k_ra8_err_hw_timeout SD_INFO2.BWE never asserted within
 *                              ::k_ra8_sdhi_fifo_spin polls.
 *
 * @pre ``reg`` and ``buf`` are non-NULL.
 * @pre A write data phase is already armed (see ::internal_sdhi_setup_xfer).
 *
 * @post On success the controller has accepted ``words * 4`` bytes.
 * @post ``buf`` is not modified.
 *
 * @note Not thread-safe; the polled SDHI API assumes one caller.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_sdhi_fill(volatile r_sdhi_regs_t* reg, const uint8_t* buf, uint32_t words)
{
  const uint8_t* cursor = buf;
  for (uint32_t w = 0U; w < words; ++w) {
    uint32_t spin = 0U;
    for (; spin < k_ra8_sdhi_fifo_spin; ++spin) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
      if (ra8_fake_mmio_poll(&reg->SD_INFO2,
                             spin,
                             (reg->SD_INFO2 & k_ra8_sdhi_info2_bwe_mask) != 0U)) {
#else
      if ((reg->SD_INFO2 & k_ra8_sdhi_info2_bwe_mask) != 0U) {
#endif
        break;
      }
    }
    if (spin >= k_ra8_sdhi_fifo_spin) {
      return k_ra8_err_hw_timeout;
    }
    const uint32_t word = (uint32_t)cursor[0] | ((uint32_t)cursor[1] << k_ra8_sdhi_shift_b1) |
                          ((uint32_t)cursor[2] << k_ra8_sdhi_shift_b2) |
                          ((uint32_t)cursor[3] << k_ra8_sdhi_shift_b3);
    reg->SD_BUF0        = word;
    cursor += k_ra8_sdhi_fifo_word_bytes;
  }
  return k_ra8_ok;
}

/**
 * @brief Tear down the data phase: optional CMD12 + flag cleanup.
 *
 * @details
 * Issues CMD12 STOP_TRANSMISSION when ``block_count > 1`` to close
 * the open-ended multi-block transfer the way FSP does at
 * r_sdhi.c, then zeroes SD_INFO1 / SD_INFO2 so the next caller
 * sees a clean slate. Factored out so the public read/write helpers
 * stay under the NASA Rule 4 statement limit.
 *
 * @param[in] reg         SDHI register window pointer.
 * @param[in] block_count Block count of the transfer being torn down.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Data phase closed and flags cleared.
 * @retval k_ra8_err_hw_timeout CMD12 STOP_TRANSMISSION never saw RSPEND.
 *
 * @pre ``reg`` points at a mapped SDHI register window.
 * @pre The data phase this call closes has already drained or filled.
 *
 * @post SD_INFO1 and SD_INFO2 are zeroed.
 * @post CMD12 has been issued when ``block_count > 1``.
 *
 * @note Not thread-safe; the polled SDHI API assumes one caller.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_sdhi_finish_xfer(volatile r_sdhi_regs_t* reg, uint32_t block_count)
{
  if (block_count > 1U) {
    /* HUM Ch 47.2.1 "SD_CMD : Command Type Register" p 3123 */
    const ra8_err_t stop_err =
      internal_sdhi_send(reg, (uint32_t)k_ra8_sdhi_cmd_stop_transmission, 0U);
    RA8_RETURN_ON_ERROR(stop_err, s_tag, "block_xfer: CMD12 timeout");
  }
  reg->SD_INFO1 = 0U;
  reg->SD_INFO2 = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_sdhi_read_block(uint8_t instance, uint32_t lba, uint8_t* buf, uint32_t block_count)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "read_block: buf must not be nullptr");
  if (block_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "read_block: instance out of range");

  internal_sdhi_setup_xfer(reg, block_count);

  const uint32_t  cmd     = (block_count > 1U) ? (uint32_t)k_ra8_sdhi_cmd_read_multi_block
                                               : (uint32_t)k_ra8_sdhi_cmd_read_single_block;
  const ra8_err_t cmd_err = internal_sdhi_send(reg, cmd, lba);
  RA8_RETURN_ON_ERROR(cmd_err, s_tag, "read_block: RSPEND timeout");

  /* HUM Ch 47.2.21 "SD_BUF0 : SD Buffer Register" p 3143 */
  const uint32_t  total_words = block_count * k_ra8_sdhi_words_per_block;
  const ra8_err_t drain_err   = internal_sdhi_drain(reg, buf, total_words);
  RA8_RETURN_ON_ERROR(drain_err, s_tag, "read_block: BRE timeout");

  return internal_sdhi_finish_xfer(reg, block_count);
}

ra8_err_t
ra8_sdhi_write_block(uint8_t instance, uint32_t lba, const uint8_t* buf, uint32_t block_count)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "write_block: buf must not be nullptr");
  if (block_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "write_block: instance out of range");

  internal_sdhi_setup_xfer(reg, block_count);

  const uint32_t  cmd     = (block_count > 1U) ? (uint32_t)k_ra8_sdhi_cmd_write_multi_block
                                               : (uint32_t)k_ra8_sdhi_cmd_write_single_block;
  const ra8_err_t cmd_err = internal_sdhi_send(reg, cmd, lba);
  RA8_RETURN_ON_ERROR(cmd_err, s_tag, "write_block: RSPEND timeout");

  /* HUM Ch 47.2.21 "SD_BUF0 : SD Buffer Register" p 3143 */
  const uint32_t  total_words = block_count * k_ra8_sdhi_words_per_block;
  const ra8_err_t fill_err    = internal_sdhi_fill(reg, buf, total_words);
  RA8_RETURN_ON_ERROR(fill_err, s_tag, "write_block: BWE timeout");

  return internal_sdhi_finish_xfer(reg, block_count);
}

ra8_err_t ra8_sdhi_attach_dma(uint8_t instance, uint8_t enable)
{
  volatile r_sdhi_regs_t* reg = ra8_sdhi(instance);
  RA8_CHECK_NULL_PTR(reg, s_tag, "attach_dma: instance out of range");

  /* HUM Ch 47.2.30 "SD_DMAEN : DMA Mode Enable Register"  p 3147 */
  /* HUM Ch 47.2.17 "SD_INFO2_MASK : SD Card Interrupt Mask 2" p 3137 */
  /* FSP r_sdhi_transfer_read / r_sdhi_transfer_write set the BREM
   * and BWEM mask bits whenever DMAEN is asserted, so that the
   * DMA-driven path does not race with the polled BRE / BWE wait
   * the PIO path uses. Mirror that pairing here. */
  if (enable != 0U) {
    reg->SD_INFO2_MASK |= k_ra8_sdhi_info2_brem_bwem;
    reg->SD_DMAEN = k_ra8_sdhi_dmaen_set;
  } else {
    reg->SD_DMAEN = 0U;
    reg->SD_INFO2_MASK &= ~k_ra8_sdhi_info2_brem_bwem;
  }
  return k_ra8_ok;
}
