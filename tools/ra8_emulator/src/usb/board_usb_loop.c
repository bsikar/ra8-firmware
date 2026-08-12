/**
 * @file board_usb_loop.c
 * @brief Self-loop (loop-cable) transport (see board_usb_internal.h)
 *
 * @details
 * The board_usb_loop_* family the register-level USBHS host model drives
 * when the firmware itself is the bus host (self-loop bench), plus the
 * latch/reset state -- moved verbatim out of board_usb.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdio.h>
#include <string.h>

#include "board_usb_internal.h"
#include "ra8_elc_regs.h"
#include "ra8_usb_regs.h"

/* Loop-cable (self-loop) state -- see the board_usb_loop_* block below. */
bool            s_loop_latched;                   /**< Firmware host owns the bus.   */
static bool     s_loop_pending_cfg;               /**< SET_CONFIGURATION in flight.  */
static uint16_t s_loop_dcp_rd;                    /**< Host cursor into dcp_in.      */
static uint16_t s_loop_pipe_rd[k_usb_pipe_count]; /**< Host cursors into pipe_in.    */
uint32_t        s_loop_setups;                    /**< SETUPs the fw host delivered. */
uint32_t        s_loop_bulk_out_pkts;             /**< Bulk-OUT packets delivered.   */
uint32_t        s_loop_bulk_in_pkts;              /**< Bulk-IN packets drained.      */

/* =============================================================================
 * Loop-cable transport -- the USBHS HOST-mode model (board_usb_host.c) drives
 * this device through these calls when the firmware itself is the bus host
 * (self-loop bench: USBHS J7 host cabled to USBFS J11 device). Each helper
 * mirrors the corresponding built-in virtual-host step so the device-side
 * firmware sees identical register semantics either way.
 * =============================================================================
 */

/** @brief SETUP-word decode helpers + the DCP status bit for the loop path. */
typedef enum : uint16_t {
  k_loop_req_bm_mask     = 0x00FFU, /**< bmRequestType byte of USBREQ.    */
  k_ra8_usb_dcp_brdy_bit = 0x0001U, /**< BRDYSTS bit 0: the DCP (pipe 0). */
} loop_req_mask_t;

/** @brief Reset one pipe's IN/OUT staging plus the host-side read cursor. */
static void loop_reset_pipe(uint8_t pipe)
{
  s_usb.pipe_in[pipe].len    = 0U;
  s_usb.pipe_in[pipe].valid  = false;
  s_usb.pipe_out[pipe].len   = 0U;
  s_usb.pipe_out[pipe].rd    = 0U;
  s_usb.pipe_out[pipe].ready = false;
  s_loop_pipe_rd[pipe]       = 0U;
}

/** @brief Reset the DCP staging (both directions) plus the host read cursor. */
static void loop_reset_dcp(void)
{
  s_usb.dcp_in.len    = 0U;
  s_usb.dcp_in.valid  = false;
  s_usb.dcp_out.len   = 0U;
  s_usb.dcp_out.rd    = 0U;
  s_usb.dcp_out.ready = false;
  s_loop_dcp_rd       = 0U;
}

void board_usb_loop_latch(void)
{
  if (s_loop_latched) {
    return;
  }
  s_loop_latched = true;
  usb_log_line("self-loop latched: firmware host owns the bus (virtual host parked)");
}

bool board_usb_loop_attached(void)
{
  return host_device_attached();
}

void board_usb_loop_bus_reset(uc_engine* uc)
{
  loop_reset_dcp();
  for (uint32_t p = 0U; p < (uint32_t)k_usb_pipe_count; p++) {
    loop_reset_pipe((uint8_t)p);
  }
  s_usb.setup_valid = false;
  s_usb.ctsq        = (uint16_t)k_ra8_ctsq_idle;
  s_usb.dvsq        = (uint16_t)k_ra8_dvsq_default;
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_dvst);
  usb_log_line("loop: bus reset -> DVSQ Default");
  usb_raise_irq(uc);
}

bool board_usb_loop_setup(uc_engine* uc, uint16_t req, uint16_t val, uint16_t indx, uint16_t leng)
{
  const uint8_t bm   = (uint8_t)(req & (uint16_t)k_loop_req_bm_mask);
  const uint8_t breq = (uint8_t)(req >> k_usb_byte_bits);
  /* A new SETUP supersedes whatever control transfer preceded it: drop stale
   * DCP staging and consume a stale CCPL (the fw host is strictly serial, so
   * any completion still latched here belongs to the finished transfer). */
  loop_reset_dcp();
  (void)host_take_ccpl();
  s_loop_pending_cfg                                   = false;
  s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbreq)]  = req;
  s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbval)]  = val;
  s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbindx)] = indx;
  s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbleng)] = leng;
  s_loop_setups++;
  char line[k_usb_log_width];
  (void)snprintf(line,
                 sizeof(line),
                 "loop: SETUP bm=0x%02X bReq=0x%02X wVal=0x%04X wLen=%u",
                 (unsigned)bm,
                 (unsigned)breq,
                 (unsigned)val,
                 (unsigned)leng);
  usb_log_line(line);
  if ((bm == (uint8_t)k_usb_dir_host_to_device) && (breq == (uint8_t)k_usb_req_set_address)) {
    /* The device SIE latches USBADDR and runs the whole status stage itself
     * (mirrors ::host_apply_no_data): the host needs no device CCPL. */
    s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_usbaddr)] =
      (uint16_t)(val & (uint16_t)k_ra8_usbaddr_addr_mask);
    s_usb.dvsq = (uint16_t)k_ra8_dvsq_address;
    usb_intsts0_set((uint8_t)k_ra8_int0_bit_dvst);
    usb_raise_irq(uc);
    return true;
  }
  if ((bm == (uint8_t)k_usb_dir_host_to_device) && (breq == (uint8_t)k_usb_req_set_config)) {
    s_loop_pending_cfg = true; /* configured-state advance rides the CCPL. */
  }
  /* For an OUT-data request the payload arrives via board_usb_loop_ctrl_out. */
  s_usb.setup_valid = true;
  if ((bm & (uint8_t)k_usb_dir_device_to_host) != 0U) {
    s_usb.ctsq = (uint16_t)k_ra8_ctsq_rdds;
  } else if (leng != 0U) {
    s_usb.ctsq = (uint16_t)k_ra8_ctsq_wrds;
  } else {
    s_usb.ctsq = (uint16_t)k_ra8_ctsq_wrnd;
  }
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_ctrt);
  usb_raise_irq(uc);
  return false;
}

bool board_usb_loop_take_ccpl(uc_engine* uc)
{
  if (!host_take_ccpl()) {
    return false;
  }
  if (s_loop_pending_cfg) {
    s_loop_pending_cfg = false;
    host_mark_configured(uc);
  }
  return true;
}

uint16_t board_usb_loop_ctrl_in_avail(void)
{
  if (!s_usb.dcp_in.valid) {
    return 0U;
  }
  return (uint16_t)(s_usb.dcp_in.len - s_loop_dcp_rd);
}

uint16_t board_usb_loop_ctrl_in_read(uint8_t* dst, uint16_t cap)
{
  const uint16_t avail = board_usb_loop_ctrl_in_avail();
  const uint16_t n     = (avail < cap) ? avail : cap;
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = s_usb.dcp_in.data[s_loop_dcp_rd + i];
  }
  s_loop_dcp_rd = (uint16_t)(s_loop_dcp_rd + n);
  if ((n > 0U) && (s_loop_dcp_rd >= s_usb.dcp_in.len)) {
    usb_detect_class(s_usb.dcp_in.data, s_usb.dcp_in.len);
    s_usb.dcp_in.len   = 0U;
    s_usb.dcp_in.valid = false;
    s_loop_dcp_rd      = 0U;
  }
  return n;
}

void board_usb_loop_ctrl_in_flush(void)
{
  s_usb.dcp_in.len   = 0U;
  s_usb.dcp_in.valid = false;
  s_loop_dcp_rd      = 0U;
}

void board_usb_loop_ctrl_out(uc_engine* uc, const uint8_t* data, uint16_t len)
{
  uint16_t n = len;
  if (n > (uint16_t)sizeof(s_usb.dcp_out.data)) {
    n = (uint16_t)sizeof(s_usb.dcp_out.data);
  }
  (void)memcpy(s_usb.dcp_out.data, data, n);
  s_usb.dcp_out.len   = n;
  s_usb.dcp_out.rd    = 0U;
  s_usb.dcp_out.ready = true;
  const uint32_t w    = usb_word((uint64_t)k_ra8_usb_off_brdysts);
  s_usb.reg[w]        = (uint16_t)(s_usb.reg[w] | (uint16_t)k_ra8_usb_dcp_brdy_bit);
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_brdy);
  usb_raise_irq(uc);
}

void board_usb_loop_status_out_zlp(uc_engine* uc)
{
  s_usb.ctsq        = (uint16_t)k_ra8_ctsq_rdss;
  s_usb.setup_valid = false;
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_ctrt);
  usb_raise_irq(uc);
}

void board_usb_loop_bulk_out(uc_engine* uc, uint8_t ep, const uint8_t* data, uint16_t len)
{
  usb_out_buf_t* b = &s_usb.pipe_out[ep % k_usb_pipe_count];
  uint16_t       n = len;
  if (n > (uint16_t)sizeof(b->data)) {
    n = (uint16_t)sizeof(b->data);
  }
  (void)memcpy(b->data, data, n);
  b->len           = n;
  b->rd            = 0U;
  b->ready         = true;
  const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_brdysts);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << (ep % k_usb_pipe_count)));
  usb_intsts0_set((uint8_t)k_ra8_int0_bit_brdy);
  s_loop_bulk_out_pkts++;
  usb_raise_irq(uc);
}

uint16_t board_usb_loop_bulk_in_avail(uint8_t ep)
{
  const usb_in_buf_t* b = &s_usb.pipe_in[ep % k_usb_pipe_count];
  if (!b->valid) {
    return 0U;
  }
  return (uint16_t)(b->len - s_loop_pipe_rd[ep % k_usb_pipe_count]);
}

uint16_t board_usb_loop_bulk_in_read(uc_engine* uc, uint8_t ep, uint8_t* dst, uint16_t cap)
{
  const uint8_t  pipe  = (uint8_t)(ep % k_usb_pipe_count);
  usb_in_buf_t*  b     = &s_usb.pipe_in[pipe];
  const uint16_t avail = board_usb_loop_bulk_in_avail(ep);
  const uint16_t n     = (avail < cap) ? avail : cap;
  for (uint16_t i = 0U; i < n; i++) {
    dst[i] = b->data[s_loop_pipe_rd[pipe] + i];
  }
  s_loop_pipe_rd[pipe] = (uint16_t)(s_loop_pipe_rd[pipe] + n);
  if ((n > 0U) && (s_loop_pipe_rd[pipe] >= b->len)) {
    b->len               = 0U;
    b->valid             = false;
    s_loop_pipe_rd[pipe] = 0U;
    s_loop_bulk_in_pkts++;
    /* Transmit buffer drained: raise the device's BEMP for this pipe so its
     * firmware can queue the next packet (mirrors ::host_echo_read_in). */
    const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_bempsts);
    s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << pipe));
    usb_intsts0_set((uint8_t)k_ra8_int0_bit_bemp);
    usb_raise_irq(uc);
  }
  return n;
}

void board_usb_loop_bulk_in_flush(uint8_t ep)
{
  const uint8_t pipe        = (uint8_t)(ep % k_usb_pipe_count);
  s_usb.pipe_in[pipe].len   = 0U;
  s_usb.pipe_in[pipe].valid = false;
  s_loop_pipe_rd[pipe]      = 0U;
}
