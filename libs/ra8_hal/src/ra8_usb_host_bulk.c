/**
 * @file ra8_usb_host_bulk.c
 * @brief USB host-mode bulk-transfer engine (polled, synchronous)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Host-mode bulk-transfer engine plus the host pipe-setup / target-select
 * helpers it builds on. Built on the same host signals the control engine
 * validated on hardware -- BRDY (a packet landed in the pipe buffer),
 * BEMP (the pipe buffer emptied onto the wire), and PIPECTR.PID for STALL
 * detection. Provides ``ra8_usb_host_set_target``,
 * ``ra8_usb_host_pipe_setup``, ``ra8_usb_host_bulk_out`` /
 * ``ra8_usb_host_bulk_in``, and ``ra8_usb_host_line_state``. Split out of
 * ``ra8_usb.c`` so every translation unit stays under the 1000-line cap;
 * the shared register helpers and host addressing enums it uses are
 * declared in ``ra8_usb_internal.h``. No FSP source ships in this tree.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_usb.h"
#include "ra8_usb_internal.h"
#include "ra8_usb_regs.h"

static const char* s_tag = "USB";

/* =============================================================================
 * Host-mode bulk-transfer engine (polled, synchronous)
 *
 * Built on the same host signals the control engine validated on hardware:
 * BRDY (a packet landed in the pipe buffer), BEMP (the pipe buffer emptied
 * onto the wire), and PIPECTR.PID for STALL detection. Pipes reuse the CFIFO
 * port (CFIFOSEL.CURPIPE selects the pipe; the data direction is fixed by
 * PIPECFG.DIR, so ISEL stays clear for non-DCP pipes).
 * =============================================================================
 */

/** @brief Spin bound for bulk-pipe waits (covers media access latency). */
typedef enum : uint32_t {
  /* Each spin performs two peripheral-bus reads (~150 ns/read on the FS
   * block), so 10M spins is roughly a 3 s ceiling -- enough for flash
   * media latency while keeping a failed phase visibly bounded. */
  k_ra8_usb_bulk_poll_limit = 10000000UL, /**< Spins per bulk stage wait. */
} ra8_usb_host_bulk_lim_t;

/**
 * @brief Spin until a pipe's W0C status bit asserts, with STALL detection.
 *
 * @details Bounded busy-wait over BRDYSTS/BEMPSTS for one pipe. While
 * waiting, watches the pipe's PIPECTR.PID field: a transition to either
 * STALL encoding (PID[1] set) means the device rejected the transfer, which
 * is reported distinctly from a timeout so MSC error handling can react.
 *
 * @param[in] reg      Selected controller register block.
 * @param[in] sts      Status register to watch (BRDYSTS or BEMPSTS).
 * @param[in] pipe_num Pipe whose bit (1 << pipe_num) is awaited.
 * @return Wait outcome.
 * @retval k_ra8_ok             The status bit asserted.
 * @retval k_ra8_err_hw_error   The device STALLed the endpoint.
 * @retval k_ra8_err_hw_timeout No event before the spin bound elapsed.
 * @pre The pipe is configured and its transfer has been armed.
 * @pre The matching ENB bit is set so the status can latch.
 * @post No register is modified by this function.
 * @post On STALL the pipe PID still reads the STALL encoding for the caller.
 * @note Bounded by ::k_ra8_usb_bulk_poll_limit (covers media access latency).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_host_wait_pipe(volatile r_usb_regs_t* reg, volatile const uint16_t* sts, uint8_t pipe_num)
{
  const uint16_t bit = (uint16_t)(1U << pipe_num);
  const uint8_t  idx = (uint8_t)(pipe_num - 1U);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_bulk_poll_limit; ++i) {
    if ((*sts & bit) != 0U) {
      return k_ra8_ok;
    }
    if ((reg->PIPECTR[idx] & (uint16_t)k_ra8_usb_pid_stall_bit) != 0U) {
      /* STALL bit is SIE-async; callers force PID=BUF before this spin, the fake cannot raise it. */
      return k_ra8_err_hw_error; /* GCOVR_EXCL_LINE */
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Implementation of `ra8_usb_host_set_target()`.
 * @details See the public header for the documented contract; programs the
 *          DEVADDn slot for @p dev_addr from the live RHST and retargets the
 *          DCP by loading DCPMAXP.DEVSEL while preserving MXPS.
 * @param[in] speed See header.
 * @param[in] dev_addr See header.
 * @return Result code.
 * @retval k_ra8_ok Target address applied.
 * @pre Module state is consistent.
 * @pre The DCP is idle (no SUREQ pending).
 * @post DCPMAXP.DEVSEL = @p dev_addr; DEVADDn carries the link speed.
 * @post Subsequent control transfers address @p dev_addr.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_host_set_target(ra8_usb_speed_t speed, uint8_t dev_addr)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (dev_addr > (uint8_t)k_ra8_usb_dev_addr_max) {
    return k_ra8_err_invalid_arg;
  }
  internal_host_program_devadd(reg, dev_addr);
  const uint16_t mxps = (uint16_t)(reg->DCPMAXP & (uint16_t)k_ra8_usb_dcpmaxp_mxps);
  reg->DCPMAXP =
    (uint16_t)((uint16_t)((uint16_t)dev_addr << (uint16_t)k_ra8_usb_devsel_shift) | mxps);
  return k_ra8_ok;
}

/**
 * @brief Validate the argument set for ::ra8_usb_host_pipe_setup.
 *
 * @details Range-checks the pipe number, device address, endpoint number,
 * and max-packet size against the controller limits so the setup body can
 * stay small enough for the complexity gate.
 *
 * @param[in] pipe_num   Controller pipe (1..::k_ra8_usb_max_pipe_num).
 * @param[in] dev_addr   Target device address (0..::k_ra8_usb_dev_addr_max).
 * @param[in] ep_num     Device endpoint number (1..15).
 * @param[in] max_packet Endpoint wMaxPacketSize (1..PIPEMAXP MXPS range).
 * @return Validation outcome.
 * @retval k_ra8_ok              All arguments are in range.
 * @retval k_ra8_err_invalid_arg Any argument is out of range.
 * @pre None (pure argument validation).
 * @pre Caller passes the same values it will program.
 * @post No state is modified.
 * @post On k_ra8_ok the values are safe to write into PIPECFG/PIPEMAXP.
 * @note Helper split out for the clang-tidy size/complexity gate.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_host_pipe_args_ok(uint8_t pipe_num, uint8_t dev_addr, uint8_t ep_num, uint16_t max_packet)
{
  if (pipe_num == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (pipe_num > (uint8_t)k_ra8_usb_max_pipe_num) {
    return k_ra8_err_invalid_arg;
  }
  if (dev_addr > (uint8_t)k_ra8_usb_dev_addr_max) {
    return k_ra8_err_invalid_arg;
  }
  if (ep_num == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (ep_num > (uint8_t)k_ra8_pipecfg_epnum_mask) {
    return k_ra8_err_invalid_arg;
  }
  if (max_packet == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (max_packet > (uint16_t)k_ra8_usb_pipemaxp_mxps) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_host_pipe_setup()`.
 * @details See the public header for the documented contract; configures a
 *          bulk pipe against an attached device's endpoint. In host mode
 *          PIPECFG.DIR keeps its transmit/receive sense: receiving
 *          (device-to-host IN) is DIR=0 and transmitting (host-to-device
 *          OUT) is DIR=1, so the device-mode encoder is reused with the
 *          mapped direction. PIPEnCTR shares the DCPCTR SQCLR bit layout.
 * @param[in] speed See header.
 * @param[in] pipe_num See header.
 * @param[in] dev_addr See header.
 * @param[in] ep_num See header.
 * @param[in] device_to_host See header.
 * @param[in] max_packet See header.
 * @return Result code.
 * @retval k_ra8_ok Pipe configured, DATA0 forced, parked NAK.
 * @pre Module state is consistent.
 * @pre SET_CONFIGURATION has reset the device endpoint to DATA0.
 * @post The pipe targets @p dev_addr endpoint @p ep_num, PID = NAK.
 * @post The pipe's BRDY/NRDY/BEMP status bits are cleared.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_host_pipe_setup(ra8_usb_speed_t speed,
                                  uint8_t         pipe_num,
                                  uint8_t         dev_addr,
                                  uint8_t         ep_num,
                                  bool            device_to_host,
                                  uint16_t        max_packet)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t arg_err = internal_host_pipe_args_ok(pipe_num, dev_addr, ep_num, max_packet);
  if (arg_err != k_ra8_ok) {
    return arg_err;
  }
  internal_pipe_quiesce(reg, pipe_num);
  const ra8_usb_ep_dir_t cfg_dir = device_to_host ? k_ra8_usb_ep_dir_out : k_ra8_usb_ep_dir_in;
  reg->PIPESEL                   = pipe_num;
  reg->PIPECFG  = internal_pipecfg_word(ep_num, cfg_dir, k_ra8_usb_ep_type_bulk, true);
  reg->PIPEBUF  = internal_pipebuf_word(pipe_num, max_packet);
  reg->PIPEMAXP = (uint16_t)((uint16_t)((uint16_t)dev_addr << (uint16_t)k_ra8_usb_devsel_shift) |
                             (uint16_t)(max_packet & (uint16_t)k_ra8_usb_pipemaxp_mxps));
  reg->PIPEPERI = 0U;
  reg->PIPESEL  = 0U;
  internal_rmw16(&reg->PIPECTR[(uint8_t)(pipe_num - 1U)],
                 (uint16_t)(1U << k_ra8_dcpctr_bit_sqclr),
                 0U);
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  reg->BRDYSTS            = (uint16_t)~pipe_bit;
  reg->NRDYSTS            = (uint16_t)~pipe_bit;
  reg->BEMPSTS            = (uint16_t)~pipe_bit;
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_usb_host_bulk_out()`.
 * @details See the public header for the documented contract; pushes one
 *          packet (up to the pipe MPS) through the pipe buffer via
 *          ::ra8_usb_queue_in (wait FRDY, FIFO write, BVAL, PID=BUF) and
 *          waits for the buffer-empty (BEMP) event that marks the packet
 *          transmitted and ACKed, then parks the pipe NAK.
 * @param[in] speed See header.
 * @param[in] pipe_num See header.
 * @param[in] data See header.
 * @param[in] len See header.
 * @return Result code.
 * @retval k_ra8_ok Packet transmitted and acknowledged.
 * @pre Module state is consistent.
 * @pre The pipe was configured by ::ra8_usb_host_pipe_setup.
 * @post The pipe PID is NAK and its BEMP status is cleared.
 * @post On k_ra8_ok the device has ACKed the packet.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t
ra8_usb_host_bulk_out(ra8_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "host_bulk_out: data");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (pipe_num == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (pipe_num > (uint8_t)k_ra8_usb_max_pipe_num) {
    return k_ra8_err_invalid_arg;
  }
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  reg->BEMPSTS            = (uint16_t)~pipe_bit;
  reg->BEMPENB            = (uint16_t)(reg->BEMPENB | pipe_bit);
  const ra8_err_t qerr    = ra8_usb_queue_in(speed, pipe_num, data, len);
  if (qerr != k_ra8_ok) {
    return qerr;
  }
  const ra8_err_t werr = internal_host_wait_pipe(reg, &reg->BEMPSTS, pipe_num);
  internal_pipe_pid(reg, pipe_num, k_ra8_pid_nak);
  reg->BEMPSTS = (uint16_t)~pipe_bit;
  return werr;
}

/**
 * @brief Receive one bulk packet from an armed IN pipe into @p dst.
 *
 * @details Waits for the pipe BRDY, selects the pipe on the CFIFO port,
 * reads DTLN bytes (clamped to @p room; any overflow remainder is dropped
 * with BCLR, and a zero-length packet is released with BCLR), then clears
 * the BRDY status for the next packet.
 *
 * @param[in]  reg        Selected controller register block.
 * @param[in]  pipe_num   Armed IN pipe (PID = BUF).
 * @param[out] dst        Destination for this packet's bytes.
 * @param[in]  room       Bytes available at @p dst.
 * @param[out] out_dtln   Receives the packet length the device sent.
 * @param[out] out_copied Receives the bytes actually copied to @p dst.
 * @return Packet outcome.
 * @retval k_ra8_ok             One packet consumed.
 * @retval k_ra8_err_hw_error   The device STALLed the endpoint.
 * @retval k_ra8_err_hw_timeout No packet before the spin bound.
 * @pre The pipe PID is BUF and BRDYENB carries the pipe bit.
 * @pre @p dst holds at least @p room bytes.
 * @post The pipe buffer is released and BRDY is cleared.
 * @post @p out_dtln / @p out_copied describe the packet.
 * @note Helper split out for the clang-tidy size/complexity gate.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_bulk_rx_packet(volatile r_usb_regs_t* reg,
                                              uint8_t                pipe_num,
                                              uint8_t*               dst,
                                              uint16_t               room,
                                              uint16_t*              out_dtln,
                                              uint16_t*              out_copied)
{
  const ra8_err_t werr = internal_host_wait_pipe(reg, &reg->BRDYSTS, pipe_num);
  if (werr != k_ra8_ok) {
    return werr;
  }
  /* Acknowledge the BRDY edge BEFORE draining the buffer: at high speed
   * the device's next packet (e.g. the CSW right behind a full data
   * packet) lands while the FIFO is being read, and clearing the status
   * afterwards wipes that new edge (observed as the follow-up packet
   * stuck with BSTS=1 while the wait times out). */
  reg->BRDYSTS = (uint16_t) ~(uint16_t)(1U << pipe_num);
  internal_select_cfifo(reg, (uint16_t)pipe_num, false);
  const ra8_err_t ferr = internal_wait_frdy(reg);
  if (ferr != k_ra8_ok) {
    return ferr;
  }
  const uint16_t dtln = (uint16_t)(reg->CFIFOCTR & (uint16_t)k_ra8_fifoctr_dtln);
  uint16_t       copy = dtln;
  if (copy > room) {
    copy = room;
  }
  if (copy > 0U) {
    internal_fifo_read(reg, dst, copy);
  }
  if (copy < dtln) {
    reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
  }
  if (dtln == 0U) {
    reg->CFIFOCTR = (uint16_t)k_ra8_fifoctr_bclr;
  }
  *out_dtln   = dtln;
  *out_copied = copy;
  return k_ra8_ok;
}

/**
 * @brief Consume bulk-IN packets until short packet, fill, or error.
 *
 * @details Inner receive loop of ::ra8_usb_host_bulk_in, split out for the
 * complexity gate. The pipe must already be armed (PID = BUF).
 *
 * @param[in]  reg      Selected controller register block.
 * @param[in]  pipe_num Armed IN pipe.
 * @param[out] buf      Destination buffer.
 * @param[in]  max_len  Capacity of @p buf in bytes.
 * @param[in]  mps      Pipe max packet size (short-packet threshold).
 * @param[out] out_rx   Receives the byte count gathered.
 * @return First packet error, or k_ra8_ok at transfer end.
 * @retval k_ra8_ok Transfer ended by short packet or byte count.
 * @pre The pipe PID is BUF and BRDYENB carries the pipe bit.
 * @pre @p buf holds at least @p max_len bytes.
 * @post @p out_rx holds the byte count even on error (partial count).
 * @post The pipe PID is unchanged (caller parks it).
 * @note Helper split out for the clang-tidy size/complexity gate.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_host_bulk_rx_loop(volatile r_usb_regs_t* reg,
                                            uint8_t                pipe_num,
                                            uint8_t*               buf,
                                            uint16_t               max_len,
                                            uint16_t               mps,
                                            uint16_t*              out_rx)
{
  uint16_t  rx   = 0U;
  ra8_err_t err  = k_ra8_ok;
  bool      done = false;
  while (!done) {
    uint16_t dtln   = 0U;
    uint16_t copied = 0U;
    err             = internal_host_bulk_rx_packet(reg,
                                       pipe_num,
                                       &buf[rx],
                                       (uint16_t)(max_len - rx),
                                       &dtln,
                                       &copied);
    if (err != k_ra8_ok) {
      done = true;
    } else {
      rx = (uint16_t)(rx + copied);
      if (dtln < mps) {
        done = true;
      }
      if (rx >= max_len) {
        done = true;
      }
    }
  }
  *out_rx = rx;
  return err;
}

/**
 * @brief Implementation of `ra8_usb_host_bulk_in()`.
 * @details See the public header for the documented contract; arms the IN
 *          pipe (PID=BUF so the SIE issues IN tokens) and consumes packets
 *          via ::internal_host_bulk_rx_packet until a short packet ends the
 *          transfer or @p max_len bytes have been gathered, then parks the
 *          pipe NAK.
 * @param[in] speed See header.
 * @param[in] pipe_num See header.
 * @param[out] buf See header.
 * @param[in] max_len See header.
 * @param[out] out_received See header.
 * @return Result code.
 * @retval k_ra8_ok Transfer ended by short packet or byte count.
 * @pre Module state is consistent.
 * @pre The pipe was configured by ::ra8_usb_host_pipe_setup.
 * @post The pipe PID is NAK; @p out_received holds the byte count.
 * @post On error the partial count received so far is still reported.
 * @note Not thread-safe.
 * @since 0.1.0
 */
ra8_err_t ra8_usb_host_bulk_in(ra8_usb_speed_t speed,
                               uint8_t         pipe_num,
                               uint8_t*        buf,
                               uint16_t        max_len,
                               uint16_t*       out_received)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "host_bulk_in: buf");
  RA8_CHECK_NULL_PTR(out_received, s_tag, "host_bulk_in: out_received");
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (pipe_num == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (pipe_num > (uint8_t)k_ra8_usb_max_pipe_num) {
    return k_ra8_err_invalid_arg;
  }
  reg->PIPESEL       = pipe_num;
  const uint16_t mps = (uint16_t)(reg->PIPEMAXP & (uint16_t)k_ra8_usb_pipemaxp_mxps);
  reg->PIPESEL       = 0U;
  if (mps == 0U) {
    return k_ra8_err_invalid_state;
  }
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  /* Do NOT clear a pending BRDY here: at high speed the device can push
   * the next packet (e.g. the CSW right behind a full data packet) into
   * the pipe buffer while the previous transfer is winding down, and its
   * BRDY is already latched -- clearing it would strand that packet in
   * the buffer (observed as BSTS=1 with the wait timing out). */
  reg->BRDYENB = (uint16_t)(reg->BRDYENB | pipe_bit);
  internal_pipe_pid(reg, pipe_num, k_ra8_pid_buf);
  uint16_t        rx  = 0U;
  const ra8_err_t err = internal_host_bulk_rx_loop(reg, pipe_num, buf, max_len, mps, &rx);
  internal_pipe_pid(reg, pipe_num, k_ra8_pid_nak);
  *out_received = rx;
  return err;
}

/**
 * @brief Implementation of `ra8_usb_host_line_state()`.
 * @details Pure MMIO read of SYSSTS0.LNST; no state is modified.
 * @param[in] speed See header.
 * @return LNST[1:0] (0 = SE0 / nothing attached, 1 = J-state).
 * @retval 0 Invalid speed, controller not powered, or SE0.
 * @pre Module state is consistent.
 * @pre The controller is clocked for a meaningful read.
 * @post No register is modified.
 * @post Caller-visible state matches the documented contract.
 * @note Safe to call from any context.
 * @since 0.1.0
 */
uint16_t ra8_usb_host_line_state(ra8_usb_speed_t speed)
{
  volatile r_usb_regs_t* reg = internal_pick(speed);
  if (reg == nullptr) {
    return 0U;
  }
  return (uint16_t)(reg->SYSSTS0 & (uint16_t)k_ra8_usb_lnst_mask);
}
