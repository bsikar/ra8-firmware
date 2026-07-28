/**
 * @file board_periph_usbhs_host.c
 * @brief On-chip USBHS host controller model (0x40351000) for the board emulator
 *
 * @details
 * Models the RA8D2 USB 2.0 High-Speed controller (USBHS, base 0x40351000, HUM
 * Ch 37) in HOST role -- the missing half of the chip-internal USB self-loop.
 * board_usb.c already models the USBFS controller (0x40250000) in DEVICE role;
 * this block models the second on-chip controller acting as the HOST that
 * enumerates and talks to that device, exactly as the EK-RA8D2's two USB jacks
 * do when cabled to each other on the bench. Together they close the loop the
 * @c usb_selftest_* / @c dfu_selftest_* apps exercise, entirely in-process.
 *
 * The firmware's first-party host driver (libs/ra8_hal ra8_usb_host_*) is polled:
 * it programs the host registers here and spins on their status bits (SACK on
 * INTSTS1, BRDY/BEMP on BRDYSTS/BEMPSTS, SUREQ self-clear on DCPCTR) rather than
 * taking a host interrupt. So this model is purely reactive -- it answers the
 * firmware's register reads and, on the writes that launch a transfer (SUREQ,
 * a bulk BVAL, a CCPL), calls the board_usb.c bridge to drive the DEVICE model:
 * deliver the SETUP + raise the device CTRT, stage the host's OUT bytes into the
 * device's OUT pipe + raise BRDY, or collect the device's IN response. The
 * device firmware's own ISR runs (preempting the host's spin via SysTick/PendSV,
 * the higher-priority device worker) and fills its side of the FIFO, which this
 * model reflects back to the host as the descriptor / echo bytes. No host
 * interrupt, no second Unicorn core -- one image, both stacks, a modelled cable.
 *
 * This block only owns its window when ra8_emulator is started with @c --usbhs-loop
 * (::board_periph_block_t::loop_only): without the flag the window falls through
 * to the sparse fallback, so the existing @c ra8_usb_host_* function-intercept
 * seam (main.c, the virtual HID keyboard / MSC disk for host-only apps) is
 * untouched. A self-loop app opts in through its hil.conf's @c HIL_EMU_ARGS.
 *
 * Register offsets and bit fields come from ra8_usb_regs.h (the same
 * first-party HUM transcription the driver uses). The few host-role fields not
 * carried there (SYSSTS0.LNST line state, DVSTCTR0.USBRST/UACT/RHST, PLLSTA
 * lock) are defined locally and flagged with a [CONFIRM] note where the value
 * comes from general USB2_B knowledge rather than a direct HUM transcription.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_periph_block.h"
#include "board_usb.h"
#include "ra8_usb_regs.h"

/* =============================================================================
 * Window geometry + model sizing.
 * =============================================================================
 */

/** @brief USBHS host register-window geometry and staging-buffer caps. */
typedef enum : uint64_t {
  k_usbhs_base       = 0x40351000UL, /**< USBHS base (HUM Ch 37, p 2059).           */
  k_usbhs_span       = 0x200UL,      /**< Modelled window (covers DEVADD / LPSTS).  */
  k_usbhs_words      = 0x200UL / 2U, /**< 16-bit register-shadow word count.        */
  k_usbhs_dev_span   = 0x100UL,      /**< Device-model span; PHY page lies past it. */
  k_usbhs_pipes      = 10UL,         /**< DCP (0) + PIPE1..PIPE9.                   */
  k_usbhs_pkt_cap    = 512UL,        /**< Per-pipe staging cap (HS bulk MPS).       */
  k_usbhs_din_cap    = 512UL,        /**< DCP IN staging cap (full config desc).    */
  k_usbhs_setup_len  = 8UL,          /**< SETUP packet length.                      */
  k_usbhs_report_ord = 155UL,        /**< End-of-run report slot (after USB-FS).    */
} usbhs_geom_t;

/* =============================================================================
 * Host-role register bit/field values not carried by ra8_usb_regs.h.
 * =============================================================================
 */

/** @brief SYSSTS0 / DVSTCTR0 host-role line-state fields (HUM Ch 37.2.3 / 37.2.5). */
typedef enum : uint16_t {
  k_usbhs_lnst_j  = 0x0001U, /**< SYSSTS0.LNST J-state: an FS device is attached.
                                    [CONFIRM] general USB2_B LNST encoding.        */
  k_usbhs_rhst_fs = 0x0002U, /**< DVSTCTR0.RHST = full speed. [CONFIRM] driver
                                    comment (01=LS,10=FS,11=HS) in ra8_usb_host_ctrl.c. */
} usbhs_lines_t;

/** @brief DVSTCTR0 host-control bit positions (HUM Ch 37.2.5, p 2065). */
typedef enum : uint8_t {
  k_usbhs_dvstctr_usbrst = 6U, /**< USBRST: drive bus reset. [CONFIRM] HUM Ch 37.2.5. */
} usbhs_dvstctr_bit_t;

/** @brief PLLSTA lock flag reported so the firmware's PHY PLL wait completes. */
typedef enum : uint16_t {
  k_usbhs_plllock = 0x0001U, /**< PLLSTA.PLLLOCK (HUM Ch 37.2.4 "PLLSTA" p 2064). */
} usbhs_pllsta_bit_t;

/** @brief DCP (pipe 0) status bit shared by BRDYSTS / BEMPSTS / NRDYSTS. */
typedef enum : uint16_t {
  k_usbhs_dcp_bit = 0x0001U, /**< BRDYSTS/BEMPSTS/NRDYSTS DCP (pipe 0) bit. */
} usbhs_status_bit_t;

/** @brief Standard USB chapter-9 SETUP field values the model classifies on. */
typedef enum : uint8_t {
  k_usbhs_setup_dir_in    = 0x80U, /**< bmRequestType device-to-host bit (USB 2.0 9.3). */
  k_usbhs_breq_set_addr   = 0x05U, /**< SET_ADDRESS bRequest (USB 2.0 9.4).             */
  k_usbhs_breq_set_config = 0x09U, /**< SET_CONFIGURATION bRequest (USB 2.0 9.4).       */
} usbhs_usb_std_t;

/** @brief Small shared literals (avoid bare numeric literals). */
typedef enum : uint32_t {
  k_usbhs_byte_bits  = 8U,    /**< Bits per byte.                          */
  k_usbhs_byte_mask  = 0xFFU, /**< One-byte mask.                          */
  k_usbhs_epnum_mask = 0x0FU, /**< PIPECFG endpoint-number field.          */
  k_usbhs_cfifo_h    = 2U,    /**< CFIFOH alias offset from CFIFO (+0x2).  */
  k_usbhs_cfifo_hh   = 3U,    /**< CFIFOHH alias offset from CFIFO (+0x3). */
} usbhs_lit_t;

/** @brief SETUP-packet byte offsets (USB 2.0 sec 9.3, little-endian fields). */
typedef enum : uint8_t {
  k_usbhs_setup_bmrt    = 0U, /**< bmRequestType.     */
  k_usbhs_setup_breq    = 1U, /**< bRequest.          */
  k_usbhs_setup_wval_lo = 2U, /**< wValue low byte.   */
  k_usbhs_setup_wval_hi = 3U, /**< wValue high byte.  */
  k_usbhs_setup_widx_lo = 4U, /**< wIndex low byte.   */
  k_usbhs_setup_widx_hi = 5U, /**< wIndex high byte.  */
  k_usbhs_setup_wlen_lo = 6U, /**< wLength low byte.  */
  k_usbhs_setup_wlen_hi = 7U, /**< wLength high byte. */
} usbhs_setup_idx_t;

/* =============================================================================
 * Model state.
 * =============================================================================
 */

/**
 * @brief Aggregate USBHS host-controller model state.
 *
 * @details The 16-bit @c reg shadow answers the plain register reads/writes; the
 * staging buffers carry the bytes crossing the modelled cable. @c din holds the
 * device's control-IN response (or a synthetic status ZLP); @c pout / @c pin
 * hold the per-pipe bulk OUT / IN payloads. @c pipe_ep / @c pipe_maxp / @c
 * pipe_pid mirror the per-pipe config the host programs through PIPESEL/PIPECTR.
 */
typedef struct {
  uint16_t reg[k_usbhs_words]; /**< 16-bit reflect-on-read register shadow. */

  uint8_t  din[k_usbhs_din_cap]; /**< DCP IN staging (device -> host control data). */
  uint16_t din_len;              /**< Bytes staged in @c din.                       */
  uint16_t din_rd;               /**< Read cursor as the host drains @c din.        */
  bool     din_ready;            /**< din holds an unread packet (DCP BRDY source). */

  uint8_t setup[k_usbhs_setup_len]; /**< Latched 8-byte SETUP packet.          */
  bool    ctrl_read;                /**< Last SETUP was a control read.        */
  uint8_t ctrl_breq;                /**< Last SETUP bRequest.                  */
  bool    ctrl_status_pending;      /**< Control-write status awaits dev CCPL. */

  uint8_t  pout[k_usbhs_pipes][k_usbhs_pkt_cap]; /**< Per-pipe OUT staging (host->dev).      */
  uint16_t pout_len[k_usbhs_pipes];              /**< Bytes accumulated per OUT pipe.        */
  bool     pout_wait[k_usbhs_pipes];             /**< OUT pipe awaiting device drain (BEMP). */

  uint8_t  pin[k_usbhs_pipes][k_usbhs_pkt_cap]; /**< Per-pipe IN staging (dev->host). */
  uint16_t pin_len[k_usbhs_pipes];              /**< Bytes staged per IN pipe.        */
  uint16_t pin_rd[k_usbhs_pipes];               /**< Read cursor per IN pipe.         */
  bool     pin_ready[k_usbhs_pipes];            /**< Pipe holds an unread packet.     */

  uint8_t  pipe_ep[k_usbhs_pipes];   /**< Device endpoint number per host pipe. */
  uint16_t pipe_maxp[k_usbhs_pipes]; /**< PIPEMAXP shadow per host pipe.        */
  uint16_t pipe_pid[k_usbhs_pipes];  /**< PIPECTR PID shadow per host pipe.     */

  bool prev_usbrst; /**< Previous DVSTCTR0.USBRST level (reset-edge detect). */

  uint32_t setups;   /**< SETUP packets bridged to the device. */
  uint32_t bulk_out; /**< Bulk-OUT packets delivered.          */
  uint32_t bulk_in;  /**< Bulk-IN packets collected.           */
} usbhs_state_t;

static usbhs_state_t s_hs;

/* =============================================================================
 * Small register / pipe helpers (defined before the dispatchers that use them).
 * =============================================================================
 */

/** @brief 16-bit shadow word index for a window byte offset. */
static uint32_t hs_word(uint64_t off)
{
  return (uint32_t)((off & ~(uint64_t)1U) / 2U);
}

/** @brief Read a 16-bit shadow register at window byte offset @p off. */
static uint16_t hs_reg(uint16_t off)
{
  return s_hs.reg[hs_word((uint64_t)off)];
}

/** @brief Write a 16-bit shadow register at window byte offset @p off. */
static void hs_set(uint16_t off, uint16_t value)
{
  s_hs.reg[hs_word((uint64_t)off)] = value;
}

/** @brief OR @p bits into the 16-bit shadow register at @p off. */
static void hs_or(uint16_t off, uint16_t bits)
{
  hs_set(off, (uint16_t)(hs_reg(off) | bits));
}

/** @brief Currently-selected CFIFO pipe (CFIFOSEL.CURPIPE[3:0], modulo pipe count). */
static uint8_t hs_curpipe(void)
{
  const uint16_t sel = hs_reg((uint16_t)k_ra8_usb_off_cfifosel) & (uint16_t)k_ra8_fifosel_curpipe;
  return (uint8_t)(sel % (uint8_t)k_usbhs_pipes);
}

/** @brief The host pipe currently selected by PIPESEL (modulo pipe count). */
static uint8_t hs_pipesel(void)
{
  return (uint8_t)(hs_reg((uint16_t)k_ra8_usb_off_pipesel) % (uint8_t)k_usbhs_pipes);
}

/** @brief True when @p off is inside the PIPECTR[9] array window. */
static bool hs_is_pipectr(uint64_t off)
{
  const uint64_t lo = (uint64_t)k_ra8_usb_off_pipectr;
  const uint64_t hi = lo + ((uint64_t)k_ra8_usb_pipectr_count * 2U);
  return (off >= lo) && (off < hi);
}

/** @brief PIPECTR array index (PIPE1..PIPE9, modulo pipe count) for @p off. */
static uint8_t hs_pipectr_index(uint64_t off)
{
  const uint8_t p = (uint8_t)(((off - (uint64_t)k_ra8_usb_off_pipectr) / 2U) + 1U);
  return (uint8_t)(p % (uint8_t)k_usbhs_pipes);
}

/** @brief Bytes still available for the host to read on the selected IN staging. */
static uint16_t hs_cfifo_dtln(void)
{
  const uint8_t p = hs_curpipe();
  if (p == 0U) {
    return (uint16_t)(s_hs.din_len - s_hs.din_rd);
  }
  return (uint16_t)(s_hs.pin_len[p] - s_hs.pin_rd[p]);
}

/* =============================================================================
 * CFIFO data port -- move payload bytes between the host and the staging buffers.
 * =============================================================================
 */

/** @brief Append @p size bytes of @p value (LSB-first) to the selected OUT pipe. */
static void hs_cfifo_write(uint32_t value, unsigned size)
{
  const uint8_t p = hs_curpipe();
  for (unsigned i = 0U; (i < size) && (s_hs.pout_len[p] < (uint16_t)k_usbhs_pkt_cap); i++) {
    s_hs.pout[p][s_hs.pout_len[p]] =
      (uint8_t)((value >> (i * (unsigned)k_usbhs_byte_bits)) & (uint32_t)k_usbhs_byte_mask);
    s_hs.pout_len[p]++;
  }
}

/** @brief Read @p size bytes (LSB-first) from the selected IN staging buffer. */
static uint32_t hs_cfifo_read(unsigned size)
{
  const uint8_t  p    = hs_curpipe();
  uint8_t*       data = (p == 0U) ? s_hs.din : s_hs.pin[p];
  uint16_t*      rd   = (p == 0U) ? &s_hs.din_rd : &s_hs.pin_rd[p];
  const uint16_t len  = (p == 0U) ? s_hs.din_len : s_hs.pin_len[p];
  uint32_t       v    = 0U;
  for (unsigned i = 0U; (i < size) && (*rd < len); i++) {
    v |= (uint32_t)((uint32_t)data[*rd] << (i * (unsigned)k_usbhs_byte_bits));
    (*rd)++;
  }
  return v;
}

/* =============================================================================
 * Bulk pipe helpers.
 * =============================================================================
 */

/** @brief Deliver the selected bulk pipe's accumulated OUT payload to the device. */
static void hs_bulk_out_commit(uc_engine* uc)
{
  const uint8_t p = hs_curpipe();
  if (p == 0U) {
    /* DCP OUT: a control-write data stage (e.g. a DFU_DNLOAD block). Deliver it
     * to the device and gate BEMP on the device draining it. A zero-length DCP
     * BVAL is instead the control-read OUT status ZLP, driven from the CCPL
     * path -- ignore it here. */
    if (s_hs.pout_len[0] > 0U) {
      board_usb_bridge_dcp_out(uc, s_hs.pout[0], s_hs.pout_len[0]);
      s_hs.pout_wait[0] = true;
      s_hs.pout_len[0]  = 0U;
    }
    return;
  }
  board_usb_bridge_bulk_out(uc, s_hs.pipe_ep[p], s_hs.pout[p], s_hs.pout_len[p]);
  s_hs.bulk_out++;
  /* DEFER the buffer-empty (BEMP) signal until the device firmware drains this
   * packet from its CFIFO: raising it now lets the polled host push the next
   * OUT packet immediately, overwriting the undrained bank (which wedged the
   * multi-packet WRITE(10) data stage -- the device saw only the last chunk and
   * never produced the CSW). hs_bempsts_read() raises BEMP on device drain. */
  s_hs.pout_wait[p] = true;
  s_hs.pout_len[p]  = 0U;
}

/** @brief BEMPSTS read: raise a pipe's BEMP once the device has drained its OUT packet. */
static uint16_t hs_bempsts_read(void)
{
  if (s_hs.pout_wait[0] && board_usb_bridge_dcp_out_consumed()) {
    s_hs.pout_wait[0] = false; /* DCP control-write data stage drained by device. */
    hs_or((uint16_t)k_ra8_usb_off_bempsts, (uint16_t)k_usbhs_dcp_bit);
  }
  for (uint8_t p = 1U; p < (uint8_t)k_usbhs_pipes; p++) {
    if (s_hs.pout_wait[p] && board_usb_bridge_bulk_out_consumed(s_hs.pipe_ep[p])) {
      s_hs.pout_wait[p] = false;
      hs_or((uint16_t)k_ra8_usb_off_bempsts, (uint16_t)(1U << p));
    }
  }
  return hs_reg((uint16_t)k_ra8_usb_off_bempsts);
}

/**
 * @brief Finish a no-data / control-write IN status stage.
 *
 * @details Presents the device's status ZLP as a zero-length DCP IN packet so
 * the polled host's status-stage BRDY wait (@c internal_host_dcp_in_wait)
 * returns, and -- for SET_CONFIGURATION -- advances the device model to the
 * Configured state. Shared by the immediate path (SIE-owned SET_ADDRESS, and
 * control writes that carried a data stage) and the deferred path that waits on
 * the device's DCPCTR.CCPL.
 *
 * @param[in,out] uc Unicorn engine (mark-configured pends the device DVST IRQ).
 */
static void hs_ctrl_status_complete(uc_engine* uc)
{
  s_hs.din_len   = 0U;
  s_hs.din_rd    = 0U;
  s_hs.din_ready = true;
  hs_or((uint16_t)k_ra8_usb_off_brdysts, (uint16_t)k_usbhs_dcp_bit);
  if (s_hs.ctrl_breq == (uint8_t)k_usbhs_breq_set_config) {
    board_usb_bridge_mark_configured(uc);
  }
}

/**
 * @brief Latch any device IN data the host is now polling for (BRDY sources).
 *
 * @details Called on every BRDYSTS read -- the register the polled host driver
 * spins on. First completes a deferred no-data control-write status stage once
 * the device has pulsed its CCPL (so the host does not race past a request the
 * device is still applying). Then reflects the device's control-IN response
 * into @c din and any armed bulk-IN pipe's echo into @c pin, setting the
 * matching BRDYSTS bit. The guards (@c din_ready / @c pin_ready, PID==BUF) latch
 * each packet once, only after the host armed the transfer and the device
 * produced the data.
 *
 * @param[in,out] uc Unicorn engine (bulk-IN collection pends the device BEMP).
 * @return The BRDYSTS value to report.
 */
static uint16_t hs_brdysts_read(uc_engine* uc)
{
  if (s_hs.ctrl_status_pending && board_usb_bridge_dev_took_ccpl()) {
    s_hs.ctrl_status_pending = false;
    hs_ctrl_status_complete(uc);
  }
  if (!s_hs.din_ready && board_usb_bridge_dcp_in_ready()) {
    s_hs.din_len   = board_usb_bridge_dcp_in_take(s_hs.din, (uint16_t)k_usbhs_din_cap);
    s_hs.din_rd    = 0U;
    s_hs.din_ready = true;
    hs_or((uint16_t)k_ra8_usb_off_brdysts, (uint16_t)k_usbhs_dcp_bit);
  }
  for (uint8_t p = 1U; p < (uint8_t)k_usbhs_pipes; p++) {
    const bool armed = (s_hs.pipe_pid[p] & (uint16_t)k_ra8_pid_mask) == (uint16_t)k_ra8_pid_buf;
    if (!armed || s_hs.pin_ready[p]) {
      continue;
    }
    if (board_usb_bridge_bulk_in_ready(s_hs.pipe_ep[p])) {
      s_hs.pin_len[p] =
        board_usb_bridge_bulk_in_take(uc, s_hs.pipe_ep[p], s_hs.pin[p], (uint16_t)k_usbhs_pkt_cap);
      s_hs.pin_rd[p]    = 0U;
      s_hs.pin_ready[p] = true;
      s_hs.bulk_in++;
      hs_or((uint16_t)k_ra8_usb_off_brdysts, (uint16_t)(1U << p));
    }
  }
  return hs_reg((uint16_t)k_ra8_usb_off_brdysts);
}

/** @brief Apply a BRDYSTS W0C write: a written 0 clears that bit, a 1 preserves it. */
static void hs_brdysts_write(uint16_t value)
{
  const uint16_t before  = hs_reg((uint16_t)k_ra8_usb_off_brdysts);
  const uint16_t cleared = (uint16_t)(before & (uint16_t)~value); /* 1 -> 0 transitions. */
  hs_set((uint16_t)k_ra8_usb_off_brdysts, (uint16_t)(before & value));
  if ((cleared & (uint16_t)k_usbhs_dcp_bit) != 0U) {
    s_hs.din_ready = false; /* host consumed the DCP IN packet; allow a re-latch. */
  }
  for (uint8_t p = 1U; p < (uint8_t)k_usbhs_pipes; p++) {
    if ((cleared & (uint16_t)(1U << p)) != 0U) {
      s_hs.pin_ready[p] = false; /* host acked this pipe's BRDY; allow the next. */
    }
  }
}

/* =============================================================================
 * Control-transfer bridge -- SETUP launch + status completion.
 * =============================================================================
 */

/** @brief Copy USBREQ..USBLENG from the shadow into the 8-byte SETUP buffer. */
static void hs_capture_setup(void)
{
  const uint16_t req             = hs_reg((uint16_t)k_ra8_usb_off_usbreq);
  const uint16_t val             = hs_reg((uint16_t)k_ra8_usb_off_usbval);
  const uint16_t indx            = hs_reg((uint16_t)k_ra8_usb_off_usbindx);
  const uint16_t leng            = hs_reg((uint16_t)k_ra8_usb_off_usbleng);
  s_hs.setup[k_usbhs_setup_bmrt] = (uint8_t)(req & (uint16_t)k_usbhs_byte_mask);
  s_hs.setup[k_usbhs_setup_breq] =
    (uint8_t)((req >> (uint16_t)k_usbhs_byte_bits) & (uint16_t)k_usbhs_byte_mask);
  s_hs.setup[k_usbhs_setup_wval_lo] = (uint8_t)(val & (uint16_t)k_usbhs_byte_mask);
  s_hs.setup[k_usbhs_setup_wval_hi] =
    (uint8_t)((val >> (uint16_t)k_usbhs_byte_bits) & (uint16_t)k_usbhs_byte_mask);
  s_hs.setup[k_usbhs_setup_widx_lo] = (uint8_t)(indx & (uint16_t)k_usbhs_byte_mask);
  s_hs.setup[k_usbhs_setup_widx_hi] =
    (uint8_t)((indx >> (uint16_t)k_usbhs_byte_bits) & (uint16_t)k_usbhs_byte_mask);
  s_hs.setup[k_usbhs_setup_wlen_lo] = (uint8_t)(leng & (uint16_t)k_usbhs_byte_mask);
  s_hs.setup[k_usbhs_setup_wlen_hi] =
    (uint8_t)((leng >> (uint16_t)k_usbhs_byte_bits) & (uint16_t)k_usbhs_byte_mask);
}

/** @brief Launch a SETUP (DCPCTR.SUREQ write): deliver it and latch SACK. */
static void hs_setup_launch(uc_engine* uc)
{
  hs_capture_setup();
  const uint16_t leng = hs_reg((uint16_t)k_ra8_usb_off_usbleng);
  s_hs.ctrl_breq      = s_hs.setup[k_usbhs_setup_breq];
  s_hs.ctrl_read =
    ((s_hs.setup[k_usbhs_setup_bmrt] & (uint8_t)k_usbhs_setup_dir_in) != 0U) && (leng != 0U);
  s_hs.ctrl_status_pending = false; /* fresh transfer: no deferred status yet. */
  /* Discard any device CCPL left over from the previous control transfer's
   * status stage (a control read leaves it unconsumed). Otherwise a deferred
   * no-data status stage would complete on that stale pulse instead of waiting
   * for THIS request's completion, re-introducing the host-races-device bug. */
  (void)board_usb_bridge_dev_took_ccpl();

  s_hs.din_ready = false; /* stale DCP IN state cleared for the fresh transfer. */
  s_hs.din_len   = 0U;
  s_hs.din_rd    = 0U;
  board_usb_bridge_deliver_setup(uc, s_hs.setup);
  s_hs.setups++;

  /* The SIE latches SACK when the device ACKs the SETUP; the host gates on it. */
  hs_or((uint16_t)k_ra8_usb_off_intsts1, (uint16_t)(1U << k_ra8_int1_bit_sack));
}

/** @brief Handle a DCPCTR.CCPL rising edge: drive the control-transfer status stage. */
static void hs_ccpl_status(uc_engine* uc)
{
  if (s_hs.ctrl_read) {
    /* Control-read OUT status: let the device close its side, then report the
     * buffer empty so the host's best-effort BEMP wait returns promptly. */
    board_usb_bridge_ctrl_status(uc);
    hs_or((uint16_t)k_ra8_usb_off_bempsts, (uint16_t)k_usbhs_dcp_bit);
    return;
  }
  /* A host-to-device control write the DEVICE must apply -- every one except
   * SET_ADDRESS, whose USBADDR latch and IN-ZLP status stage the SIE owns:
   * DEFER the host's status-stage BRDY until the device firmware has actually
   * processed the request and pulsed DCPCTR.CCPL. Completing it synchronously
   * races the polled host past a request the device is still applying:
   *  - no-data (SET_CONFIGURATION): it would issue GET_MAX_LUN and the first
   *    BOT CBW before the MSC class had activated and armed its bulk endpoints;
   *  - with-data (DFU_DNLOAD): it would issue the next SETUP -- clearing the
   *    device DCP-OUT staging -- before the device had drained the block, so
   *    the firmware image never lands. The device pulses CCPL only after it
   *    drains the data stage and applies the request, which is exactly the
   *    completion to wait on. hs_brdysts_read() finishes the stage on it. */
  if (s_hs.ctrl_breq != (uint8_t)k_usbhs_breq_set_addr) {
    s_hs.ctrl_status_pending = true;
    return;
  }
  /* SET_ADDRESS: SIE-owned, no device CCPL to wait on -- complete now. */
  hs_ctrl_status_complete(uc);
}

/** @brief Apply a DCPCTR write: SUREQ launches a SETUP, CCPL drives the status. */
static void hs_dcpctr_write(uc_engine* uc, uint16_t value)
{
  const uint16_t before = hs_reg((uint16_t)k_ra8_usb_off_dcpctr);
  const uint16_t sureq  = (uint16_t)(1U << k_ra8_dcpctr_bit_sureq);
  const uint16_t ccpl   = (uint16_t)(1U << k_ra8_dcpctr_bit_ccpl);
  if ((value & sureq) != 0U) {
    hs_setup_launch(uc);
  }
  if (((value & ccpl) != 0U) && ((before & ccpl) == 0U)) {
    hs_ccpl_status(uc);
  }
  /* SUREQ self-clears once the SIE has delivered the token; never latch it. */
  hs_set((uint16_t)k_ra8_usb_off_dcpctr, (uint16_t)(value & (uint16_t)~sureq));
}

/* =============================================================================
 * CFIFOCTR write (BCLR / BVAL) + bus-reset edge.
 * =============================================================================
 */

/** @brief Apply a CFIFOCTR write: BCLR flushes the buffer, BVAL commits a bulk OUT. */
static void hs_cfifoctr_write(uc_engine* uc, uint16_t value)
{
  const uint8_t p = hs_curpipe();
  if ((value & (uint16_t)k_ra8_fifoctr_bclr) != 0U) {
    if (p == 0U) {
      s_hs.din_len   = 0U;
      s_hs.din_rd    = 0U;
      s_hs.din_ready = false;
    } else {
      s_hs.pout_len[p] = 0U;
      s_hs.pin_rd[p]   = s_hs.pin_len[p];
    }
  }
  if ((value & (uint16_t)k_ra8_fifoctr_bval) != 0U) {
    hs_bulk_out_commit(uc);
  }
}

/** @brief Apply a DVSTCTR0 write, bridging a host bus-reset edge onto the device. */
static void hs_dvstctr0_write(uc_engine* uc, uint16_t value)
{
  const bool usbrst = (value & (uint16_t)(1U << k_usbhs_dvstctr_usbrst)) != 0U;
  hs_set((uint16_t)k_ra8_usb_off_dvstctr0, value);
  if (s_hs.prev_usbrst && !usbrst) {
    board_usb_bridge_bus_reset(uc); /* USBRST released: device returns to Default. */
  }
  s_hs.prev_usbrst = usbrst;
}

/* =============================================================================
 * MMIO read / write dispatch.
 * =============================================================================
 */

/** @brief Read one USBHS host register at window byte offset @p off. */
static uint64_t hs_read(uc_engine* uc, uint64_t off, unsigned size)
{
  switch ((uint16_t)off) {
    case (uint16_t)k_ra8_usbhs_off_pllsta:
      return (uint64_t)k_usbhs_plllock; /* PHY PLL reported locked so init proceeds. */
    case (uint16_t)k_ra8_usb_off_syssts0:
      return board_usb_dev_attached() ? (uint64_t)k_usbhs_lnst_j : 0U;
    case (uint16_t)k_ra8_usb_off_dvstctr0: {
      uint16_t v = hs_reg((uint16_t)k_ra8_usb_off_dvstctr0);
      if (board_usb_dev_attached()) {
        v = (uint16_t)(v | (uint16_t)k_usbhs_rhst_fs); /* connected FS device speed. */
      }
      return (uint64_t)v;
    }
    case (uint16_t)k_ra8_usb_off_cfifo:
      return (uint64_t)hs_cfifo_read(size);
    case (uint16_t)k_ra8_usb_off_cfifoctr:
      return (uint64_t)((uint16_t)k_ra8_fifoctr_frdy |
                        (hs_cfifo_dtln() & (uint16_t)k_ra8_fifoctr_dtln));
    case (uint16_t)k_ra8_usb_off_brdysts:
      return (uint64_t)hs_brdysts_read(uc);
    case (uint16_t)k_ra8_usb_off_bempsts:
      return (uint64_t)hs_bempsts_read();
    case (uint16_t)k_ra8_usb_off_pipemaxp:
      return (uint64_t)s_hs.pipe_maxp[hs_pipesel()];
    default:
      break;
  }
  if (hs_is_pipectr(off)) {
    return (uint64_t)s_hs.pipe_pid[hs_pipectr_index(off)];
  }
  return (uint64_t)hs_reg((uint16_t)off);
}

/** @brief Write one USBHS host register at window byte offset @p off. */
static void hs_write(uc_engine* uc, uint64_t off, unsigned size, uint64_t value)
{
  const uint16_t v16 = (uint16_t)value;
  switch ((uint16_t)off) {
    case (uint16_t)k_ra8_usb_off_cfifo:
    case (uint16_t)((uint16_t)k_ra8_usb_off_cfifo + (uint16_t)k_usbhs_cfifo_h):  /* CFIFOH tail. */
    case (uint16_t)((uint16_t)k_ra8_usb_off_cfifo + (uint16_t)k_usbhs_cfifo_hh): /* CFIFOHH tail. */
      hs_cfifo_write((uint32_t)value, size); /* full width: HS CFIFO is 32-bit (MBW=32). */
      return;
    case (uint16_t)k_ra8_usb_off_cfifoctr:
      hs_cfifoctr_write(uc, v16);
      return;
    case (uint16_t)k_ra8_usb_off_dcpctr:
      hs_dcpctr_write(uc, v16);
      return;
    case (uint16_t)k_ra8_usb_off_dvstctr0:
      hs_dvstctr0_write(uc, v16);
      return;
    case (uint16_t)k_ra8_usb_off_brdysts:
      hs_brdysts_write(v16);
      return;
    case (uint16_t)k_ra8_usb_off_bempsts:
    case (uint16_t)k_ra8_usb_off_nrdysts:
    case (uint16_t)k_ra8_usb_off_intsts0:
    case (uint16_t)k_ra8_usb_off_intsts1:
      hs_set((uint16_t)off, (uint16_t)(hs_reg((uint16_t)off) & v16)); /* W0C. */
      return;
    case (uint16_t)k_ra8_usb_off_pipecfg:
      s_hs.pipe_ep[hs_pipesel()] = (uint8_t)(v16 & (uint16_t)k_usbhs_epnum_mask);
      hs_set((uint16_t)off, v16);
      return;
    case (uint16_t)k_ra8_usb_off_pipemaxp:
      s_hs.pipe_maxp[hs_pipesel()] = v16;
      return;
    default:
      break;
  }
  if (hs_is_pipectr(off)) {
    s_hs.pipe_pid[hs_pipectr_index(off)] = v16;
    return;
  }
  hs_set((uint16_t)off, v16);
}

/* =============================================================================
 * Role-routed public entry points (see the role-routing section of board_usb.h).
 * =============================================================================
 */

uint64_t board_usbhs_host_reg_read(uc_engine* uc, uint64_t off, unsigned size)
{
  return hs_read(uc, off, size);
}

void board_usbhs_host_reg_write(uc_engine* uc, uint64_t off, unsigned size, uint64_t value)
{
  hs_write(uc, off, size, value);
}

uint32_t board_usbhs_host_shadow_handoff(uint16_t* dst_words, uint32_t word_capacity)
{
  if (dst_words == nullptr) {
    return 0U;
  }
  uint32_t n = (uint32_t)k_usbhs_words;
  if (n > word_capacity) {
    n = word_capacity;
  }
  (void)memcpy(dst_words, s_hs.reg, (size_t)n * sizeof(uint16_t));
  /* The embedded-PHY page past the device-model span (LPSTS.SUSPENDM at 0x102)
   * is per-WINDOW state, not per-role: it stays with this physical controller
   * so the device firmware's PHY configuration survives the rebinding. */
  uint16_t phy_page[k_usbhs_words] = {};
  (void)memcpy(&phy_page[(uint32_t)k_usbhs_dev_span / 2U],
               &s_hs.reg[(uint32_t)k_usbhs_dev_span / 2U],
               ((size_t)k_usbhs_words - ((size_t)k_usbhs_dev_span / 2U)) * sizeof(uint16_t));
  s_hs = (usbhs_state_t){}; /* fresh start behind the USBFS window (Config B). */
  (void)memcpy(s_hs.reg, phy_page, sizeof(phy_page));
  return n;
}

/* =============================================================================
 * Block entry points, reset, report, self-registration.
 * =============================================================================
 */

/**
 * @brief MMIO read handler: dispatch inside the USBHS controller window.
 *
 * @details The embedded HS PHY page is role-agnostic and always answered here:
 * PLLSTA reports the UTMI PLL locked so either role's PHY bring-up completes,
 * and offsets past the device model's 0x100-byte span (LPSTS at 0x102) reflect
 * the local shadow. Everything else routes by the self-loop polarity: to the
 * host model (Config A, default) or to the device model after a role swap
 * (Config B: the DCD drives this controller in device role).
 *
 * @param[in,out] uc   Unicorn engine (forwarded to the routed model).
 * @param[in]     addr Absolute MMIO address inside the window.
 * @param[in]     size Access width in bytes.
 * @return The register value, zero-extended to 64 bits.
 */
static uint64_t usbhs_block_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  const uint64_t off = addr - (uint64_t)k_usbhs_base;
  if ((uint16_t)off == (uint16_t)k_ra8_usbhs_off_pllsta) {
    return (uint64_t)
      k_usbhs_plllock; /* HUM Ch 37.2.4 "PLLSTA : PLL Status Register" p 2064 -- role-agnostic. */
  }
  if (board_usb_roles_swapped()) {
    if (off >= (uint64_t)k_usbhs_dev_span) {
      return (uint64_t)hs_reg((uint16_t)off); /* PHY page (LPSTS): local shadow. */
    }
    return board_usb_dev_reg_read(uc, off, size);
  }
  return hs_read(uc, off, size);
}

/**
 * @brief MMIO write handler: dispatch inside the USBHS controller window.
 *
 * @details Watches for the firmware's device-role declaration -- a SYSCFG
 * write with DPRPU=1 (the D+ pull-up only a device asserts, HUM Ch 37.2.1) --
 * and swaps the self-loop window<->model bindings on it (Config B). PHY-page
 * writes past the device model's span (LPSTS) always land in the local
 * shadow; everything else routes by the current polarity.
 *
 * @param[in,out] uc    Unicorn engine (forwarded to the routed model).
 * @param[in]     addr  Absolute MMIO address inside the window.
 * @param[in]     size  Access width in bytes.
 * @param[in]     value Value the firmware wrote.
 */
static void usbhs_block_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  const uint64_t off = addr - (uint64_t)k_usbhs_base;
  /* Role declaration (HUM Ch 37.2.1 SYSCFG.DPRPU, p 2060): only the device
   * role pulls D+ up. A DPRPU=1 write to THIS instance pins it to the device
   * model -- the FS controller must then be the loop's host (Config B). */
  if (!board_usb_roles_swapped() && ((uint16_t)off == (uint16_t)k_ra8_usb_off_syscfg) &&
      (((uint16_t)value & (uint16_t)(1U << k_ra8_syscfg_bit_dprpu)) != 0U)) {
    board_usb_roles_swap(uc);
  }
  if (board_usb_roles_swapped()) {
    if (off >= (uint64_t)k_usbhs_dev_span) {
      hs_set((uint16_t)off, (uint16_t)value); /* PHY page (LPSTS): local shadow. */
      return;
    }
    board_usb_dev_reg_write(uc, off, size, value);
    return;
  }
  hs_write(uc, off, size, value);
}

/** @brief Reset the USBHS host model to power-on state. */
static void usbhs_reset(void)
{
  s_hs = (usbhs_state_t){};
}

/** @brief End-of-run summary: enumeration + bulk traffic bridged to the device. */
static void usbhs_report(void)
{
  if (s_hs.setups == 0U) {
    return; /* the loop's host side never came up this run; stay silent. */
  }
  (void)fprintf(stderr,
                "  %s host   : %u SETUP(s), %u bulk-OUT, %u bulk-IN (chip-internal loop)\n",
                board_usb_roles_swapped() ? "USB-FS" : "USB-HS",
                s_hs.setups,
                s_hs.bulk_out,
                s_hs.bulk_in);
}

/** @brief USBHS host block descriptor (loop-only: owns 0x40351000 with --usbhs-loop). */
static const board_periph_block_t k_usbhs_block = {
  .base      = (uint64_t)k_usbhs_base,
  .span      = (uint64_t)k_usbhs_span,
  .order     = (uint32_t)k_usbhs_report_ord,
  .read      = usbhs_block_read,
  .write     = usbhs_block_write,
  .tick      = nullptr,
  .reset     = usbhs_reset,
  .report    = usbhs_report,
  .name      = "USBHS-host",
  .observe   = false,
  .device    = k_board_block_dev_any,
  .loop_only = true,
};

/** @brief Register the USBHS host block before main (host constructor). */
[[gnu::constructor]] static void usbhs_block_register(void)
{
  board_periph_register_block(&k_usbhs_block);
}
