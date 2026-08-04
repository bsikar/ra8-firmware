/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_usb_bridge.c
 * @brief USBHS-host <-> USBFS-device bridge (see board_usb_internal.h)
 *
 * @details
 * The chip-internal self-loop surface: role declaration + swap, the device
 * register accessors the emulated USBHS host controller drives, the
 * control/bulk hand-off primitives and the level-triggered DCP-OUT pump --
 * moved verbatim out of board_usb.c.
 *
 *
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "board_usb_internal.h"
#include "ra8_elc_regs.h"
#include "ra8_usb_regs.h"

/* =============================================================================
 * Chip-internal USBHS-host <-> USBFS-device bridge (see board_usb.h).
 *
 * The emulated USBHS host controller (board_periph_usbhs_host.c) calls these to
 * drive THIS device model from the real firmware's host-side register accesses,
 * reusing the same device-poking primitives the virtual host uses. This closes
 * the RA8D2's on-chip HS-host -> FS-device loop entirely in-process.
 * =============================================================================
 */

void board_usb_set_external_host(bool present)
{
  s_external_host = present;
}

bool board_usb_roles_swapped(void)
{
  return s_roles_swapped;
}

void board_usb_roles_swap(uc_engine* uc)
{
  (void)uc;
  if (s_roles_swapped) {
    return; /* one-shot: both triggers (FS DCFM=1, HS DPRPU=1) fire in Config B. */
  }
  /* The device firmware's PHY + module-init writes landed in the HS window's
   * shadow while its role was still unknowable (the HS host bring-up drives the
   * very same device-polarity PHY sequence first). Adopt that accumulated state
   * as the device model's register file -- the models swap windows, the
   * firmware-visible register contents must not change -- and reset the host
   * model for its fresh life behind the FS window (which is still virgin: the
   * FS host driver's first window write is the SYSCFG.DCFM=1 that triggers
   * this swap, and the HS DPRPU=1 trigger fires before any FS host activity). */
  (void)board_usbhs_host_shadow_handoff(s_usb.reg, (uint32_t)k_usb_reg_words);
  s_roles_swapped = true;
  s_dev_irq_event = (uint16_t)k_ra8_elc_event_usbhs_int_resume;
  usb_log_line("role swap: FS window = host model, HS window = device model (Config B)");
}

uint64_t board_usb_dev_reg_read(uc_engine* uc, uint64_t off, unsigned size)
{
  (void)uc;
  return (uint64_t)usb_reg_read(off, size);
}

void board_usb_dev_reg_write(uc_engine* uc, uint64_t off, unsigned size, uint64_t value)
{
  (void)uc;
  usb_reg_write(off, (uint32_t)value, size);
}

bool board_usb_dev_attached(void)
{
  return host_device_attached();
}

void board_usb_bridge_bus_reset(uc_engine* uc)
{
  s_usb.dvsq = (uint16_t)k_ra8_dvsq_default;
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_dvst);
  usb_log_line("bridge: HS host bus reset -> device Default");
  usb_raise_irq(uc);
}

void board_usb_bridge_deliver_setup(uc_engine* uc, const uint8_t* setup)
{
  if (setup == nullptr) {
    return;
  }
  const usb_setup_step_t s = {
    .bm_request_type = setup[0],
    .b_request       = setup[1],
    .w_value         = (uint16_t)((uint16_t)setup[2] | (uint16_t)((uint16_t)setup[3] << 8)),
    .w_index         = (uint16_t)((uint16_t)setup[4] | (uint16_t)((uint16_t)setup[5] << 8)),
    .w_length        = (uint16_t)((uint16_t)setup[6] | (uint16_t)((uint16_t)setup[7] << 8)),
    .name            = "bridge SETUP",
  };
  host_deliver_setup(uc, &s);
  /* SET_ADDRESS is latched by the SIE on hardware; mirror that so the device
   * advances to the Address state (host_apply_no_data self-guards on bRequest). */
  host_apply_no_data(uc, &s);
}

bool board_usb_bridge_dcp_in_ready(void)
{
  return s_usb.dcp_in.valid && host_dcp_pid_buf();
}

uint16_t board_usb_bridge_dcp_in_take(uint8_t* buf, uint16_t cap)
{
  if (buf == nullptr) {
    return 0U;
  }
  uint16_t n = s_usb.dcp_in.len;
  if (n > cap) {
    n = cap;
  }
  (void)memcpy(buf, s_usb.dcp_in.data, n);
  usb_detect_class(s_usb.dcp_in.data, s_usb.dcp_in.len);
  usb_log_count("bridge control-IN: device returned", (unsigned)s_usb.dcp_in.len);
  if (s_trace) {
    /* Payload head, --trace only: enough to identify a descriptor / status. */
    char           line[k_usb_log_width];
    int            w = 0;
    const uint16_t limit =
      (n < (uint16_t)k_usb_trace_dump_max) ? n : (uint16_t)k_usb_trace_dump_max;
    for (uint16_t i = 0U; (i < limit) && (w >= 0) && ((size_t)w < sizeof(line)); i++) {
      w += snprintf(&line[w], sizeof(line) - (size_t)w, "%02X ", (unsigned)s_usb.dcp_in.data[i]);
    }
    (void)fprintf(stderr, "  [usb] bridge control-IN bytes: %s\n", line);
  }
  s_usb.dcp_in.len   = 0U;
  s_usb.dcp_in.valid = false;
  return n;
}

void board_usb_bridge_ctrl_status(uc_engine* uc)
{
  s_usb.ctsq        = (uint16_t)k_ra8_ctsq_rdss;
  s_usb.setup_valid = false;
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_ctrt);
  usb_raise_irq(uc);
}

bool board_usb_bridge_dev_took_ccpl(void)
{
  return host_take_ccpl();
}

void board_usb_bridge_mark_configured(uc_engine* uc)
{
  host_mark_configured(uc);
}

void board_usb_bridge_bulk_out(uc_engine* uc, uint8_t dev_pipe, const uint8_t* data, uint16_t len)
{
  if (data == nullptr) {
    return;
  }
  usb_out_buf_t* b = &s_usb.pipe_out[dev_pipe % k_usb_pipe_count];
  uint16_t       n = len;
  if (n > (uint16_t)sizeof(b->data)) {
    n = (uint16_t)sizeof(b->data);
  }
  (void)memcpy(b->data, data, n);
  b->len           = n;
  b->rd            = 0U;
  b->ready         = true;
  const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_brdysts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << dev_pipe));
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_brdy);
  usb_raise_irq(uc);
}

bool board_usb_bridge_bulk_out_consumed(uint8_t dev_pipe)
{
  const usb_out_buf_t* b = &s_usb.pipe_out[dev_pipe % k_usb_pipe_count];
  /* The device firmware drains the staged OUT packet through its CFIFO, which
   * advances @c rd to @c len. Until then the packet is still in flight, so the
   * host must not push the next one (it would overwrite this bank -- the exact
   * failure that wedged a multi-packet WRITE(10) data stage). */
  return b->ready && (b->rd >= b->len);
}

bool board_usb_bridge_bulk_in_ready(uint8_t dev_pipe)
{
  const usb_in_buf_t* b = &s_usb.pipe_in[dev_pipe % k_usb_pipe_count];
  return b->valid && (b->len > 0U);
}

uint16_t board_usb_bridge_bulk_in_take(uc_engine* uc, uint8_t dev_pipe, uint8_t* buf, uint16_t cap)
{
  if (buf == nullptr) {
    return 0U;
  }
  usb_in_buf_t* b = &s_usb.pipe_in[dev_pipe % k_usb_pipe_count];
  uint16_t      n = b->len;
  if (n > cap) {
    n = cap;
  }
  (void)memcpy(buf, b->data, n);
  b->len           = 0U;
  b->valid         = false;
  const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_bempsts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << dev_pipe));
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_bemp);
  usb_raise_irq(uc);
  return n;
}

void board_usb_bridge_dcp_out(uc_engine* uc, const uint8_t* data, uint16_t len)
{
  (void)uc;
  if (data == nullptr) {
    return;
  }
  uint16_t n = len;
  if (n > (uint16_t)sizeof(s_dcp_hold)) {
    n = (uint16_t)sizeof(s_dcp_hold);
  }
  /* Hold the bytes as "in flight on the wire": the device has almost certainly
   * not armed its DCP for OUT yet, and its arm-time BCLR would discard them.
   * bridge_pump_device() lands them once the device is ready. */
  (void)memcpy(s_dcp_hold, data, n);
  s_dcp_hold_len     = n;
  s_dcp_hold_pending = true;
}

bool board_usb_bridge_dcp_out_consumed(void)
{
  /* Still on the wire (not yet handed to the device): not consumed. */
  if (s_dcp_hold_pending) {
    return false;
  }
  return s_usb.dcp_out.ready && (s_usb.dcp_out.rd >= s_usb.dcp_out.len);
}

/**
 * @brief Re-assert a device level-triggered condition the polled host awaits.
 *
 * @details In the chip-internal self-loop the polled host issues a control-write
 * data stage (e.g. a DFU_DNLOAD block) on the DCP before the device firmware --
 * running on the same core off its own IRQ -- has armed the DCP to receive it.
 * On real silicon the SIE keeps re-issuing the OUT token until the device ACKs;
 * here the one-shot BRDY the delivery pended is cleared by the device's own
 * ``ra8_usb_dcp_out_arm`` (W0C) and never re-raised, so the receive stalls. This
 * pump, run each emulation chunk while the external host drives the device,
 * re-raises the device DCP BRDY whenever the device has armed the DCP for OUT
 * (PID=BUF and BRDYENB.PIPE0) and undrained OUT data is still staged -- exactly
 * the SIE's OUT-retry, so the device reads the block on its next BRDY ISR.
 *
 * @param[in,out] uc Unicorn engine (to pend the device USB interrupt).
 */
void bridge_pump_device(uc_engine* uc)
{
  if (!s_dcp_hold_pending) {
    return;
  }
  const uint16_t dcpctr  = s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_dcpctr)];
  const uint16_t brdyenb = s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_brdyenb)];
  const bool     armed   = ((dcpctr & (uint16_t)k_ra8_pid_mask) == (uint16_t)k_ra8_pid_buf) &&
                           ((brdyenb & (uint16_t)k_usb_dcp_pipe_bit) != 0U);
  if (!armed) {
    return; /* device has not armed its DCP for OUT yet; keep the bytes on the wire. */
  }
  /* The device is ready (its arm-time BCLR already ran on an empty bank): land
   * the held control-OUT packet into the DCP and raise its BRDY so the device's
   * receive ISR drains it, exactly as an OUT token the device finally ACKs. */
  (void)memcpy(s_usb.dcp_out.data, s_dcp_hold, s_dcp_hold_len);
  s_usb.dcp_out.len   = s_dcp_hold_len;
  s_usb.dcp_out.rd    = 0U;
  s_usb.dcp_out.ready = true;
  s_dcp_hold_pending  = false;
  const uint32_t w    = usb_word((uint64_t)k_ra8_usb_off_brdysts);
  s_usb.reg[w]        = (uint16_t)(s_usb.reg[w] | (uint16_t)k_usb_dcp_pipe_bit);
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_brdy);
  usb_raise_irq(uc);
}
