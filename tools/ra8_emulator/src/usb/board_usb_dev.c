/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_usb_dev.c
 * @brief USBFS register + CFIFO model (see board_usb_internal.h)
 *
 * @details
 * The 16-bit reflect-on-read register shadow, the INTSTS0 composition, the
 * CFIFO port/CFIFOCTR staging model and the MMIO window entry points --
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

/** @brief INTSTS0 value the device reads: event bits OR computed fields. */
uint16_t usb_intsts0(void)
{
  uint16_t v = s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_intsts0)];
  v &= (uint16_t)((1U << k_ra8_int0_bit_brdy) | (1U << k_ra8_int0_bit_nrdy) |
                  (1U << k_ra8_int0_bit_bemp) | (1U << k_ra8_int0_bit_ctrt) |
                  (1U << k_ra8_int0_bit_dvst) | (1U << k_ra8_int0_bit_vbse));
  v = (uint16_t)(v | (s_usb.ctsq & (uint16_t)k_ra8_intsts0_mask_ctsq));
  v = (uint16_t)(v | (s_usb.dvsq & (uint16_t)k_ra8_intsts0_mask_dvsq));
  v = (uint16_t)(v | (uint16_t)k_ra8_intsts0_mask_vbsts); /* VBUS always present. */
  if (s_usb.setup_valid) {
    v = (uint16_t)(v | (uint16_t)k_ra8_intsts0_mask_valid);
  }
  return v;
}

/** @brief Set an INTSTS0 event bit in the shadow (host asserts it). */
void usb_intsts0_set(uint8_t bit)
{
  const uint32_t w = usb_word((uint64_t)k_ra8_usb_off_intsts0);
  s_usb.reg[w]     = (uint16_t)(s_usb.reg[w] | (uint16_t)(1U << bit));
}

/** @brief Raise the device controller interrupt (USBFS_INT, or USBHS after a role swap). */
void usb_raise_irq(uc_engine* uc)
{
  if (s_raise != nullptr) {
    s_raise(uc, s_dev_irq_event);
    s_usb_irqs++;
  }
}

/* =============================================================================
 * CFIFO routing -- map CFIFOSEL (CURPIPE + ISEL) to the active staging buffer.
 * =============================================================================
 */

/** @brief Currently-selected CFIFO pipe number (CFIFOSEL.CURPIPE[3:0]). */
static uint8_t cfifo_pipe(void)
{
  const uint16_t sel = s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_cfifosel)];
  return (uint8_t)(sel & (uint16_t)k_ra8_fifosel_curpipe);
}

/** @brief True when CFIFOSEL selects the IN direction (device writes / fills). */
static bool cfifo_is_in(void)
{
  const uint16_t sel = s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_cfifosel)];
  return (sel & (uint16_t)k_ra8_fifosel_isel) != 0U;
}

/** @brief The device-IN staging buffer CFIFOSEL currently points at. */
static usb_in_buf_t* cfifo_in_buf(void)
{
  const uint8_t p = cfifo_pipe();
  return (p == 0U) ? &s_usb.dcp_in : &s_usb.pipe_in[p % k_usb_pipe_count];
}

/** @brief The device-OUT staging buffer CFIFOSEL currently points at. */
static usb_out_buf_t* cfifo_out_buf(void)
{
  const uint8_t p = cfifo_pipe();
  return (p == 0U) ? &s_usb.dcp_out : &s_usb.pipe_out[p % k_usb_pipe_count];
}

/** @brief Bytes still available for the device to read on the selected OUT buf. */
static uint16_t cfifo_dtln(void)
{
  if (cfifo_is_in()) {
    return 0U;
  }
  const usb_out_buf_t* b = cfifo_out_buf();
  return (uint16_t)(b->len - b->rd);
}

/** @brief Service a CFIFO data-port read: drain one unit (1..4 bytes) from the OUT buffer. */
static uint32_t cfifo_read_port(unsigned size)
{
  usb_out_buf_t* b = cfifo_out_buf();
  uint32_t       v = 0U;
  for (unsigned i = 0U; (i < size) && (b->rd < b->len); i++) {
    v |= (uint32_t)((uint32_t)b->data[b->rd] << (i * k_usb_byte_bits));
    b->rd++;
  }
  return v;
}

/** @brief Service a CFIFO data-port write: append one unit (1..4 bytes) to the IN buffer. */
static void cfifo_write_port(uint32_t value, unsigned size)
{
  usb_in_buf_t* b = cfifo_in_buf();
  for (unsigned i = 0U; (i < size) && (b->len < (uint16_t)sizeof(b->data)); i++) {
    b->data[b->len] = (uint8_t)((value >> (i * k_usb_byte_bits)) & k_usb_byte_mask);
    b->len++;
  }
}

/** @brief Apply a CFIFOCTR write: BCLR clears the buffer, BVAL commits an IN. */
static void cfifoctr_write(uint16_t value)
{
  if ((value & (uint16_t)k_ra8_fifoctr_bclr) != 0U) {
    if (cfifo_is_in()) {
      usb_in_buf_t* b = cfifo_in_buf();
      b->len          = 0U;
      b->valid        = false;
    } else {
      usb_out_buf_t* b = cfifo_out_buf();
      b->rd            = b->len; /* drop the remainder of the OUT buffer. */
    }
  }
  if ((value & (uint16_t)k_ra8_fifoctr_bval) != 0U) {
    if (cfifo_is_in()) {
      cfifo_in_buf()->valid = true; /* IN buffer committed; ready for the host. */
    }
  }
}

/* =============================================================================
 * Register read / write dispatch (USBFS window at 0x40250000).
 * =============================================================================
 */

/** @brief CFIFOCTR read value: FRDY always ready, DTLN = OUT bytes available. */
static uint16_t cfifoctr_read(void)
{
  return (uint16_t)((uint16_t)k_ra8_fifoctr_frdy | (cfifo_dtln() & (uint16_t)k_ra8_fifoctr_dtln));
}

/** @brief Read one device-controller register; @p off is the byte offset into the window. */
uint32_t usb_reg_read(uint64_t off, unsigned size)
{
  switch ((uint16_t)off) {
    case (uint16_t)k_ra8_usb_off_intsts0:
      return usb_intsts0();
    case (uint16_t)k_ra8_usb_off_cfifoctr:
      return cfifoctr_read();
    case (uint16_t)k_ra8_usb_off_cfifo:
    case (uint16_t)((uint16_t)k_ra8_usb_off_cfifo + (uint16_t)k_usb_cfifo_h):  /* CFIFOH tail.  */
    case (uint16_t)((uint16_t)k_ra8_usb_off_cfifo + (uint16_t)k_usb_cfifo_hh): /* CFIFOHH tail. */
      /* Full access width: the HS instance drains its CFIFO 32 bits at a time
       * (MBW=32) with 16/8-bit residual reads through the CFIFOH / CFIFOHH
       * aliases (HUM Ch 37.2.7 "CFIFO, DnFIFO : FIFO Port Register" p 2069-2070); the FS
       * instance uses 16-bit accesses at the base port. */
      return cfifo_read_port(size);
    case (uint16_t)k_ra8_usb_off_syssts0:
      return (uint16_t)0x0003U; /* LNST = J-state: device pull-up seen. */
    case (uint16_t)k_ra8_usb_off_dvstctr0: {
      /* HUM Ch 36.2.5 "DVSTCTR0 : Device State Control Register 0", p 1971:
       * RHST[2:0] reports the settled link speed once the host's bus reset
       * completes; the device firmware (internal_dvst_track_speed in the DCD)
       * re-aims the USBX framework at it. The modelled loop is a full-speed
       * link (the FS controller's ceiling caps it in either polarity), so
       * report FS from the moment the device has left Powered. */
      uint16_t v = s_usb.reg[usb_word(off)];
      if (s_usb.dvsq != (uint16_t)k_ra8_dvsq_powered) {
        v = (uint16_t)(v | (uint16_t)k_usb_rhst_fs);
      }
      return v;
    }
    case (uint16_t)k_ra8_usb_off_frmnum:
      return s_usb.reg[usb_word((uint64_t)k_ra8_usb_off_frmnum)];
    default:
      return s_usb.reg[usb_word(off)];
  }
}

/** @brief Write one device-controller register; @p off is the byte offset into the window. */
void usb_reg_write(uint64_t off, uint32_t value, unsigned size)
{
  const uint16_t v16 = (uint16_t)value;
  switch ((uint16_t)off) {
    case (uint16_t)k_ra8_usb_off_cfifo:
    case (uint16_t)((uint16_t)k_ra8_usb_off_cfifo + (uint16_t)k_usb_cfifo_h):  /* CFIFOH tail.  */
    case (uint16_t)((uint16_t)k_ra8_usb_off_cfifo + (uint16_t)k_usb_cfifo_hh): /* CFIFOHH tail. */
      /* Full access width: the HS instance fills its CFIFO 32 bits at a time
       * (MBW=32) with 16/8-bit residual writes through the CFIFOH / CFIFOHH
       * aliases (HUM Ch 37.2.7 "CFIFO, DnFIFO : FIFO Port Register" p 2069-2070); truncating to
       * 16 bits here zeroed bytes 2-3 of every descriptor word in Config B. */
      cfifo_write_port(value, size);
      return;
    case (uint16_t)k_ra8_usb_off_cfifoctr:
      cfifoctr_write(v16);
      return;
    case (uint16_t)k_ra8_usb_off_intsts0:
      /* W0C on the event bits: a written 0 clears, a 1 preserves. */
      s_usb.reg[usb_word(off)] &= v16;
      if ((v16 & (uint16_t)k_ra8_intsts0_mask_valid) == 0U) {
        s_usb.setup_valid = false;
      }
      return;
    default:
      s_usb.reg[usb_word(off)] = v16;
      return;
  }
}

uint64_t board_usb_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled)
{
  (void)uc;
  if ((addr < (uint64_t)k_usb_base) || (addr >= ((uint64_t)k_usb_base + (uint64_t)k_usb_span))) {
    *handled = false;
    return 0U;
  }
  *handled = true;
  if (s_roles_swapped) {
    /* Config B: the polled ra8_usb_host_* driver owns the USBFS controller. */
    return board_usbhs_host_reg_read(uc, addr - (uint64_t)k_usb_base, size);
  }
  return (uint64_t)usb_reg_read(addr - (uint64_t)k_usb_base, size);
}

void board_usb_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value, bool* handled)
{
  if ((addr < (uint64_t)k_usb_base) || (addr >= ((uint64_t)k_usb_base + (uint64_t)k_usb_span))) {
    *handled = false;
    return;
  }
  *handled           = true;
  const uint64_t off = addr - (uint64_t)k_usb_base;
  /* Role declaration (HUM Ch 36.2.1 SYSCFG.DCFM, p 1966): the firmware writes
   * DCFM=1 to select controller (host) function on THIS instance. Under the
   * self-loop bridge that pins the FS window to the host model -- Config B. */
  if (s_external_host && !s_roles_swapped && (off == (uint64_t)k_ra8_usb_off_syscfg) &&
      (((uint16_t)value & (uint16_t)(1U << k_ra8_syscfg_bit_dcfm)) != 0U)) {
    board_usb_roles_swap(uc);
  }
  if (s_roles_swapped) {
    board_usbhs_host_reg_write(uc, off, size, value);
    return;
  }
  usb_reg_write(off, (uint32_t)value, size);
}
