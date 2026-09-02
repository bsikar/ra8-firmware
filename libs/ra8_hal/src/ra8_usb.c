/**
 * @file ra8_usb.c
 * @brief Native USB controller driver implementation (device + host)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Hand-written driver for the two RA8D2 USB controllers
 * (USBFS @ 0x40250000 -- HUM Ch 36, USBHS @ 0x40351000 -- HUM Ch 37).
 * The two instances share the FSP "USB2_B" register layout so this
 * file multiplexes them via a `ra8_usb_speed_t` argument and an
 * `priv_pick(speed)` helper. No FSP, CherryUSB, or TinyUSB
 * source ships in this tree -- this file is the native peripheral
 * driver, modelled on FSP's `r_usb_pdriver.c` /
 * `r_usb_preg_access.c` / `r_usb_preg_abs.c` (device) and
 * `r_usb_hreg_access.c` / `r_usb_hreg_abs.c` (host) flow.
 *
 * Mapping vs FSP (FSP function -> our entry point):
 *
 *  - `hw_usb_pmodule_init`          -> `ra8_usb_device_init`
 *  - `hw_usb_hmodule_init`          -> `ra8_usb_host_init`
 *  - `hw_usb_pclear_dprpu/_pset_dprpu` -> `ra8_usb_device_attach`
 *  - `hw_usb_set_uact / _clear_uact`-> `ra8_usb_host_set_uact`
 *  - `usb_hstd_bus_reset` (set/clr) -> `ra8_usb_host_bus_reset`
 *  - `usb_hstd_setup_command`       -> `ra8_usb_host_setup_request`
 *  - `usb_pstd_save_request`        -> `ra8_usb_read_setup_if_valid` /
 *                                      `ra8_usb_read_setup_unconditional`
 *  - `hw_usb_pcontrol_dcpctr_pid` + `hw_usb_pset_ccpl` ->
 *    `ra8_usb_control_response`
 *  - `usb_pstd_set_pipe_table`      -> `ra8_usb_configure_endpoint`
 *  - `usb_pstd_write_fifo` (CFIFO)  -> `ra8_usb_queue_in`
 *  - `usb_pstd_read_fifo`  (CFIFO)  -> `ra8_usb_queue_out`
 *  - `usb_pstd_interrupt_handler`   -> `ra8_usb_dispatch`
 *
 * Intentional gaps (deferred work, with FSP file:line + reason):
 *
 *  - HS PHY power-down / PLL bring-up (`r_usb_preg_access.c`)
 *    -- the EK-RA8D2 boots its HS PHY from the same 48 MHz clock the
 *    bootloader leaves running, so the driver assumes the PHY is
 *    already powered. Adding a full HS PHY sequence is tracked
 *    against the next iteration.
 *  - DMA glue (`r_usb_dma.c`) -- this driver runs in CPU-FIFO mode
 *    only.
 *  - OTG / role-swap paths (`r_usb_hdriver.c`) -- not ported. Host
 *    and device modes are independently selected at init time.
 *  - USB hubs -- the host-side starter targets a single attached
 *    device. Hub class enumeration is out of scope.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_usb.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_mstp.h"
#include "ra8_usb_internal.h"
#include "ra8_usb_regs.h"

static const char* s_tag = "USB";

/* =============================================================================
 * Tunables / sizing
 * =============================================================================
 */

/**
 * @enum ra8_usb_internal_lim32_t
 * @brief 32-bit driver-wide bounds (don't fit in uint16_t).
 *
 * @details ``frdy_poll_limit`` is sized for the worst-case wait
 * between two consecutive DCP IN chunks. The DCP is single-buffered:
 * after pushing chunk N, FRDY does NOT re-assert until the host has
 * actually pulled chunk N off the wire (one full IN token + data +
 * ACK round-trip on USB-FS, ~50 us). The original 1000-spin limit
 * (~1 us at 1 GHz) timed out unconditionally on every multi-chunk
 * EP0 IN, so 75-byte CONFIGURATION descriptors stalled at chunk 1.
 * 10 million spins == ~10 ms ceiling at 1 GHz, well above the
 * USB-FS host's IN re-issue cadence; the loop exits early on the
 * first FRDY=1 sample so the typical post-host-pull wait is still
 * sub-100 us. Synchronous polling is acceptable because the dispatch
 * loop runs in a dedicated ThreadX worker, not in NVIC context.
 */
typedef enum : uint32_t {
  k_ra8_usb_frdy_poll_limit = 10000000UL, /**< Spin-loops before timeout. */
  /* Short-bound FRDY poll used by queue_out's post-BCLR DBLB-second-
   * bank check. Must be tight (we're typically on the IRQ-time
   * drain path) but big enough to absorb the few cycles between BCLR
   * and the FIFO pointer flipping to the other bank when DBLB is on.
   * 256 iters x ~3 cycles == sub-microsecond at 1 GHz. */
  k_ra8_usb_dblb_frdy_poll_limit = 256UL, /**< RA8 USB dblb frdy poll limit. */
  /* CFIFOSEL CURPIPE/ISEL readback settle bound (a handful of bus
   * clocks; generous to stay fake-safe). */
  k_ra8_usb_fifosel_settle_limit = 1000UL, /**< RA8 USB fifosel settle limit. */
} ra8_usb_internal_lim32_t;
/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Resolve the per-speed register pointer.
 */
volatile r_usb_regs_t* priv_pick(ra8_usb_speed_t speed)
{
  if (speed == k_ra8_usb_speed_fs) {
    return ra8_usb_fs();
  }
  if (speed == k_ra8_usb_speed_hs) {
    return ra8_usb_hs();
  }
  return nullptr;
}

/**
 * @brief Resolve the per-speed MSTP id.
 *
 * @details See implementation.
 * @param[in] speed See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_mstp_t priv_mstp(ra8_usb_speed_t speed)
{
  return (speed == k_ra8_usb_speed_hs) ? k_ra8_mstp_usbhs : k_ra8_mstp_usbfs;
}

/**
 * @brief Apply a generic read-modify-write to a 16-bit register.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] set_mask See implementation.
 * @param[in] clr_mask See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void priv_rmw16(volatile uint16_t* reg, uint16_t set_mask, uint16_t clr_mask)
{
  const uint16_t old     = *reg;
  uint16_t       new_val = (uint16_t)(old & (uint16_t)~clr_mask);
  new_val                = (uint16_t)(new_val | set_mask);
  *reg                   = new_val;
}

/**
 * @brief Detect whether reg points at the USBHS (IP1) register block.
 * @details FSP gates `USB1_CFIFO_MBW = USB_MBW_32` on the same predicate
 *          (`p_utr->ip == USB_CFG_IP1`), and the FIFO write helpers below
 *          mirror that to keep CFIFOSEL.MBW and the CFIFO write width in
 *          agreement on each controller.
 * @param[in] reg USB instance register block pointer.
 * @return true if reg is the HS instance, false for FS.
 * @retval true USBHS (IP1) -- caller should use MBW=32 + 32-bit FIFO.
 * @retval false USBFS (IP0) -- caller should use MBW=16 + 16-bit FIFO.
 * @pre reg is a pointer returned by ra8_usb_fs() or ra8_usb_hs().
 * @pre USB module pointers are populated.
 * @post No state mutated.
 * @post Returned value reflects controller identity.
 * @note Pure function.
 * @since 0.1.0
 */
bool priv_is_hs(volatile const r_usb_regs_t* reg)
{
  return reg == ra8_usb_hs();
}

/**
 * @brief Set CFIFOSEL.MBW + CURPIPE + ISEL for the given pipe / direction.
 * @details Picks MBW=32 for USBHS (FSP USB1_CFIFO_MBW) and MBW=16 for
 *          USBFS (FSP USB0_CFIFO_MBW). The CFIFO data-port access
 *          width must match the MBW field on subsequent CFIFO accesses.
 * @param[in] reg USB instance register block.
 * @param[in] pipe_num CURPIPE value (0 = DCP, 1..n = data pipe).
 * @param[in] is_in_dir true = device-to-host (write), false = host-to-device.
 * @pre reg != NULL.
 * @pre Caller holds the DCP / pipe lock.
 * @post CFIFOSEL = MBW(speed) | (is_in_dir ? ISEL : 0) | pipe_num.
 * @post Subsequent CFIFO accesses must use the matching width.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void priv_select_cfifo(volatile r_usb_regs_t* reg, uint16_t pipe_num, bool is_in_dir)
{
  /* HUM Ch 36.2.7 / 37.2.8 CFIFOSEL p 1976 / 2071. The USBHS module
   * (IP1) requires MBW=32 -- FSP wires `USB1_CFIFO_MBW = USB_MBW_32`
   * unconditionally for this controller. With MBW=16 the SIE refuses
   * to arm an IN response (BSTS reads 0 and PID stays effectively
   * NAK on the wire even though DCPCTR.PID=BUF), and the host sees
   * unending NAKs / "device descriptor read/N, error -110". The
   * USBFS module (IP0) keeps MBW=16. */
  uint16_t sel = (uint16_t)(pipe_num & k_ra8_fifosel_curpipe);
  if (priv_is_hs(reg)) {
    sel = (uint16_t)(sel | k_ra8_fifosel_mbw_32);
  } else {
    sel = (uint16_t)(sel | k_ra8_fifosel_mbw_16);
  }
  if (is_in_dir) {
    sel = (uint16_t)(sel | k_ra8_fifosel_isel);
  }
  reg->CFIFOSEL = sel;
  /* FSP (usb_cstd_chg_curpipe) polls the CURPIPE/ISEL readback until the
   * window switch takes effect; without this a FIFO access issued right
   * after the select can land on the previous pipe's buffer (observed on
   * hardware as a bulk-OUT payload stuck with INBUFM set, never sent). */
  const uint16_t key = (uint16_t)(sel & (uint16_t)(k_ra8_fifosel_curpipe | k_ra8_fifosel_isel));
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_fifosel_settle_limit; ++i) {
    const uint16_t now =
      (uint16_t)(reg->CFIFOSEL & (uint16_t)(k_ra8_fifosel_curpipe | k_ra8_fifosel_isel));
    if (now == key) {
      return;
    }
  }
}

/**
 * @brief Spin until `CFIFOCTR.FRDY` asserts or a deadline elapses.
 *
 * @return ra8_ok on FRDY, k_ra8_err_hw_timeout otherwise.
 *
 * @details Runs the real bounded FRDY poll on every build. On the host
 * unit-test build each poll's loop-exit decision is routed through the
 * ra8_fake_mmio fault seam keyed on CFIFOCTR: first-poll success when no
 * fault is armed, or a test-armed retry / timeout leg (T1-01).
 * @param[in] reg Selected controller register block (non-NULL).
 * @retval k_ra8_ok FRDY observed before the deadline.
 * @retval k_ra8_err_hw_timeout FRDY never asserted within the budget.
 * @pre The CFIFO window is selected on the intended pipe.
 * @pre @p reg points at a live controller register block.
 * @post On k_ra8_ok the CFIFO port is ready for a read/write access.
 * @post No register is modified by this function.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra8_err_t priv_wait_frdy(volatile r_usb_regs_t* reg)
{
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979.
   * Loop bound is large (~10 ms ceiling at 1 GHz) because the DCP
   * is single-buffered: between consecutive EP0 IN chunks FRDY stays
   * low until the host actually pulls the previous chunk off the
   * wire. See ra8_usb_internal_lim32_t for the rationale. */
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_usb_frdy_poll_limit; ++i) {
    const bool frdy = ((reg->CFIFOCTR & k_ra8_fifoctr_frdy) != 0U);
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* Host MMIO fault seam, keyed on CFIFOCTR (the polled register). */
    if (ra8_fake_mmio_wait_eval(&reg->CFIFOCTR, i, frdy)) {
      return k_ra8_ok;
    }
#else
    if (frdy) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Set DCPCTR PID field to a specific value while preserving the
 *        rest of the register.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] pid See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void priv_dcp_pid(volatile r_usb_regs_t* reg, ra8_usb_pid_t pid)
{
  /* HUM Ch 36.2.21 "DCPCTR : DCP Control Register", p 1999 */
  priv_rmw16(&reg->DCPCTR, pid, k_ra8_pid_mask);
}

/**
 * @brief Set PIPECTR[idx] PID field.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] pipe_num See implementation.
 * @param[in] pid See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void priv_pipe_pid(volatile r_usb_regs_t* reg, uint8_t pipe_num, ra8_usb_pid_t pid)
{
  /* HUM Ch 36.2.27 "PIPEnCTR : PIPE n Control Register", p 2005 */
  const uint8_t idx = (uint8_t)(pipe_num - 1U);
  priv_rmw16(&reg->PIPECTR[idx], pid, k_ra8_pid_mask);
}

/* =============================================================================
 * Pipe-config word packing + quiesce (shared device/host helpers)
 * =============================================================================
 */

/**
 * @brief Compute the PIPEBUF word for a bulk pipe (2*MPS region).
 *
 * @details
 * Statically partition the controller's internal FIFO RAM among the
 * bulk pipes. Reserve blocks 0..7 (64-byte units) for the DCP (per
 * FSP/Renesas examples), then pack user pipes in pipe-number order
 * with 2*MPS per pipe -- IN bulk pipes use both as a double-buffer
 * (PIPECFG.DBLB set), OUT bulk pipes use only the first block (DBLB
 * clear). Uniform 2*MPS keeps the BUFNMB arithmetic simple and wastes
 * at most one block per OUT pipe.
 *
 *   BUFNMB  = 8 + (pipe_num - 1) * (2 * mps_blocks)
 *   BUFSIZE = (2 * mps_blocks) - 1
 *
 *   HS MPS=512 -> 2*mps_blocks = 16, BUFSIZE = 15
 *   FS MPS=64  -> 2*mps_blocks =  2, BUFSIZE =  1
 *
 * @param[in] pipe_num   PIPE number 1..9.
 * @param[in] max_packet Pipe MPS (bytes).
 *
 * @return PIPEBUF word ready to be written to register ``PIPEBUF``.
 * @retval 0..0xFFFF Packed BUFNMB/BUFSIZE word (no error condition; the
 *                   helper is total over its enum-checked inputs).
 *
 * @pre ``pipe_num`` is in the 1..9 range (caller-checked).
 * @pre ``max_packet`` is the pipe's MPS (caller validated <= 1024).
 * @post No global state is touched; the helper is pure.
 * @post Returned word satisfies HUM Ch 37.2.35 PIPEBUF layout.
 *
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
uint16_t priv_pipebuf_word(uint8_t pipe_num, uint16_t max_packet)
{
  const uint16_t mps_blocks = (uint16_t)(((uint32_t)max_packet + (k_ra8_pipebuf_block_bytes - 1U)) /
                                         k_ra8_pipebuf_block_bytes);
  const uint16_t blocks_per_pipe = (uint16_t)(mps_blocks * 2U);
  const uint16_t bufsize_field   = (uint16_t)(blocks_per_pipe - 1U);
  const uint16_t bufnmb_field =
    (uint16_t)(8U + (uint16_t)((uint16_t)(pipe_num - 1U) * blocks_per_pipe));
  return (uint16_t)((bufsize_field << k_ra8_pipebuf_bufsize_shift) |
                    (bufnmb_field & k_ra8_pipebuf_bufnmb_mask));
}

/**
 * @brief Pack PIPECFG fields for a configured non-control pipe.
 *
 * @details Encodes endpoint number, direction (DIR), pipe type (TYPE),
 * and for bulk pipes the SHTNAK flag plus (optionally, IN only) the
 * DBLB flag into the PIPECFG word. HUM Ch 36.2.24 PIPECFG. Bulk OUT is
 * always single-buffered, otherwise the controller fills both banks
 * with host data and the one-bank-per-call ra8_usb_queue_out drainer
 * wedges the data phase (GitHub issue #6). Bulk IN double-banking is
 * the caller's choice: HOST mode wants it so queue_in can push a
 * data + ZLP pair back-to-back without the second push hitting a
 * full-bank FRDY stall; DEVICE mode must run single-banked because the
 * free-bank handshake after an MPS-exact fill is unreliable (staging
 * the BOT CSW behind a 512-byte data phase on a 512-MPS HS pipe
 * FRDY-times-out and the transport wedges, observed live vs macOS).
 *
 * @param[in] ep_addr Endpoint address (low nibble = EP number; bit 7
 *                    direction; the helper reads only the EP number).
 * @param[in] dir     Pipe direction (::k_ra8_usb_ep_dir_in or _out).
 * @param[in] type    Pipe type (bulk / interrupt / iso).
 * @param[in] dblb_in Double-bank bulk IN pipes (host mode true,
 *                    device mode false; ignored for OUT / non-bulk).
 *
 * @return PIPECFG word ready to write to ``PIPECFG``.
 * @retval 0..0xFFFF Packed configuration word; no error condition.
 *
 * @pre ``ep_addr`` low nibble is the EP number (caller validated 1..15).
 * @pre ``dir`` / ``type`` are valid enum values.
 * @post No global state is touched; the helper is pure.
 * @post For bulk pipes SHTNAK is set; DBLB only when ``dblb_in`` is
 *       true with ``dir == IN`` (nested ifs, no compound decision).
 *
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
uint16_t
priv_pipecfg_word(uint8_t ep_addr, ra8_usb_ep_dir_t dir, ra8_usb_ep_type_t type, bool dblb_in)
{
  uint16_t cfg = (uint16_t)((uint16_t)ep_addr & k_ra8_pipecfg_epnum_mask);
  if (dir == k_ra8_usb_ep_dir_in) {
    cfg = (uint16_t)(cfg | k_ra8_pipecfg_dir_in);
  }
  if (type == k_ra8_usb_ep_type_bulk) {
    cfg = (uint16_t)(cfg | k_ra8_pipecfg_type_bulk);
    cfg = (uint16_t)(cfg | k_ra8_pipecfg_shtnak);
    /* Device OUT (receive): double-buffer so the host's next packet lands
     * in bank B while the ISR drains bank A. Single-banked OUT NAKs (and
     * wedges) the host's next packet during the per-packet re-arm of a
     * sustained multi-packet WRITE data phase. PIPEBUF already reserves
     * 2*MPS, and internal_irq_complete_out drains every ready bank per
     * ISR. Device IN stays single-banked (the free-bank handshake after
     * an MPS-exact fill is unreliable -- see header). HUM Ch 36.2.24. */
    if (dir == k_ra8_usb_ep_dir_out) {
      cfg = (uint16_t)(cfg | k_ra8_pipecfg_dblb);
    }
    /* DBLB on bulk IN only when the caller asks (host-mode IN). */
    if (dblb_in) {
      if (dir == k_ra8_usb_ep_dir_in) {
        cfg = (uint16_t)(cfg | k_ra8_pipecfg_dblb);
      }
    }
  } else if (type == k_ra8_usb_ep_type_intr) {
    cfg = (uint16_t)(cfg | k_ra8_pipecfg_type_intr);
  } else {
    cfg = (uint16_t)(cfg | k_ra8_pipecfg_type_iso);
  }
  return cfg;
}
/**
 * @brief Quiesce the pipe so PIPECFG/PIPEMAXP/PIPEPERI become writable.
 * @details Clears BRDYENB/NRDYENB/BEMPENB for this pipe and forces
 *          PID=NAK (HUM Ch 36.2.24 NOTE 1, p 1996; mirrors STAR
 *          rx_usb_hw.c::internal_usb_quiesce_pipe).
 * @param[in,out] reg Controller register window.
 * @param[in] pipe_num Pipe index 1..9.
 * @pre reg is non-null and points at a powered controller.
 * @pre pipe_num in [1,9].
 * @post BRDY/NRDY/BEMPENB bit for pipe_num is cleared.
 * @post PIPECTR PID for pipe_num == NAK.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void priv_pipe_quiesce(volatile r_usb_regs_t* reg, uint8_t pipe_num)
{
  const uint16_t pipe_bit = (uint16_t)(1U << pipe_num);
  reg->BRDYENB            = (uint16_t)(reg->BRDYENB & (uint16_t)~pipe_bit);
  reg->NRDYENB            = (uint16_t)(reg->NRDYENB & (uint16_t)~pipe_bit);
  reg->BEMPENB            = (uint16_t)(reg->BEMPENB & (uint16_t)~pipe_bit);
  priv_pipe_pid(reg, pipe_num, k_ra8_pid_nak);
}

/* =============================================================================
 * CFIFO byte mover (shared device/host data path)
 * =============================================================================
 */

/**
 * @enum ra8_usb_fifo_shift_t
 * @brief Byte-shift constants for packing CFIFO writes.
 */
typedef enum : uint8_t {
  k_ra8_usb_shift_b1 = 8U,  /**< Shift for byte 1. */
  k_ra8_usb_shift_b2 = 16U, /**< Shift for byte 2. */
  k_ra8_usb_shift_b3 = 24U, /**< Shift for byte 3. */
} ra8_usb_fifo_shift_t;

/**
 * @typedef ra8_usb_cfifo32_t
 * @brief 32-bit view of the CFIFO data port, permitted to alias the 16-bit
 *        ``CFIFO`` register lane.
 * @details
 * The CFIFO data port is physically 32 bits wide at CFIFO+0, and at MBW=32 a
 * single 32-bit access is what advances the FIFO read/write pointer (HUM Ch
 * 37.2.7 "CFIFO Port Register"). The register map (``r_usb_regs_t``) declares
 * only the low 16-bit ``CFIFO`` half, so draining/filling a 32-bit word is a
 * legitimate width-pun of that MMIO address. Marking the access type
 * ``may_alias`` tells the optimiser the 32-bit read/write aliases the register
 * storage, so it stays a single 32-bit load/store under strict aliasing at -O2
 * -- without it GCC assumes the 32-bit access is independent of the 16-bit
 * struct member (undefined behaviour, flagged by ``-Wstrict-aliasing``) and may
 * reorder or elide it.
 * @note Used only for the 32-bit CFIFO fills/drains in this file.
 * @since 0.1.0
 */
typedef uint32_t __attribute__((may_alias)) /* ATTR-OK: cppcheck 2.13 rejects the C23 spelling */
ra8_usb_cfifo32_t;

/**
 * @brief HS-only: write the residual 0-3 bytes after 32-bit chunks.
 * @details FSP narrows CFIFOSEL.MBW to 16 then 8 for trailing
 *          halfword/byte. We save+restore MBW around these writes.
 * @param[in] reg HS register block.
 * @param[in] data Source byte pointer.
 * @param[in] len Total payload length.
 * @pre Caller already wrote (len & ~0x3) bytes via 32-bit access.
 * @pre CFIFOSEL.MBW currently == 32.
 * @post Tail bytes pushed; CFIFOSEL.MBW restored.
 * @post DTLN advanced by exactly (len & 0x3) bytes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_fifo_write_hs_tail(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  const uint16_t tail = (uint16_t)(len & 0x3U);
  if (tail == 0U) {
    return;
  }
  const uint16_t sel_save = reg->CFIFOSEL;
  const uint16_t sel_base = (uint16_t)(sel_save & (uint16_t)~(uint16_t)k_ra8_fifosel_mbw_msk);
  uint16_t       offset   = (uint16_t)(len & (uint16_t)~(uint16_t)0x3U);
  /* For USBHS in little-endian mode, FSP writes the 16-bit residual
   * halfword to CFIFOH (CFIFO base + 0x02) and the 8-bit residual
   * byte to CFIFOHH (CFIFO base + 0x03), NOT to CFIFO itself. See
   * FSP r_usb_reg_access.h: USB1_CFIFO16=USB_M1->CFIFOH and
   * USB1_CFIFO8=USB_M1->CFIFOHH (in USB_CFG_LITTLE branch). The
   * chip uses these address aliases to commit the trailing 0..3
   * bytes when MBW is narrowed; a write to CFIFO at offset +0x00
   * in MBW=16/8 mode is silently dropped on USBHS, leaving DTLN
   * unchanged at 4N (verified empirically: 18-byte push showed
   * DTLN=16 instead of 18, and Linux saw -EOVERFLOW). */
  volatile uint16_t* const cfifoh  = (volatile uint16_t*)((uintptr_t)&reg->CFIFO + 2U);
  volatile uint8_t* const  cfifohh = (volatile uint8_t*)((uintptr_t)&reg->CFIFO + 3U);
  if ((tail & 0x2U) != 0U) {
    reg->CFIFOSEL     = (uint16_t)(sel_base | k_ra8_fifosel_mbw_16);
    const uint16_t lo = (uint16_t)data[offset + 0U];
    const uint16_t hi = (uint16_t)data[offset + 1U];
    *cfifoh           = (uint16_t)(lo | (uint16_t)(hi << k_ra8_usb_byte_bits));
    offset            = (uint16_t)(offset + 2U);
  }
  if ((tail & 0x1U) != 0U) {
    reg->CFIFOSEL = (uint16_t)(sel_base | k_ra8_fifosel_mbw_8);
    *cfifohh      = data[offset];
  }
  reg->CFIFOSEL = sel_save;
}

/**
 * @brief HS-only: 32-bit CFIFO write loop for the head bytes.
 * @details Mirrors FSP hw_usb_write_fifo32: cast &CFIFO to uint32_t*
 *          and write `len/4` 32-bit words.
 * @param[in] reg HS register block.
 * @param[in] data Source byte pointer.
 * @param[in] len Total payload length; this helper writes only the
 *                head (len & ~0x3) bytes.
 * @pre CFIFOSEL.MBW == 32.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by exactly (len & ~0x3) bytes.
 * @post FIFO contains head bytes ready for BVAL commit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_fifo_write_hs_head(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  volatile ra8_usb_cfifo32_t* const cfifo32 = (volatile ra8_usb_cfifo32_t*)(uintptr_t)&reg->CFIFO;
  const uint16_t                    quads   = (uint16_t)(len >> 2U);
  for (uint16_t i = 0U; i < quads; ++i) {
    const uint32_t b0 = (uint32_t)data[(4U * i) + 0U];
    const uint32_t b1 = (uint32_t)data[(4U * i) + 1U];
    const uint32_t b2 = (uint32_t)data[(4U * i) + 2U];
    const uint32_t b3 = (uint32_t)data[(4U * i) + 3U];
    *cfifo32 =
      b0 | (b1 << k_ra8_usb_shift_b1) | (b2 << k_ra8_usb_shift_b2) | (b3 << k_ra8_usb_shift_b3);
  }
}

/**
 * @brief Push a byte buffer into the CFIFO data port.
 * @details Dispatches to the speed-appropriate access width: USBHS
 *          uses 32-bit writes (with FSP-style 16/8 narrowing for the
 *          trailing 0..3 bytes), USBFS uses 16-bit writes with a
 *          single-byte tail. Mirrors FSP `usb_pstd_write_fifo` for
 *          IP1 / IP0 respectively.
 * @param[in] reg USB instance register block.
 * @param[in] data Source byte pointer (may be NULL when len == 0).
 * @param[in] len Number of bytes to push.
 * @pre Caller selected DCP / pipe with priv_select_cfifo and
 *      observed FRDY=1.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by len bytes.
 * @post FIFO ready for caller's BVAL commit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void priv_fifo_write(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len)
{
  /* HUM Ch 36.2.5 / 37.2.7 CFIFO p 1973 / 2070. CFIFO access width
   * must match CFIFOSEL.MBW. USBHS = 32-bit, USBFS = 16-bit. */
  if (priv_is_hs(reg)) {
    internal_fifo_write_hs_head(reg, data, len);
    internal_fifo_write_hs_tail(reg, data, len);
    return;
  }
  /* USBFS / MBW=16 path. */
  const uint16_t even = (uint16_t)(len >> 1U);
  for (uint16_t i = 0U; i < even; ++i) {
    const uint16_t lo = (uint16_t)data[(2U * i) + 0U];
    const uint16_t hi = (uint16_t)data[(2U * i) + 1U];
    reg->CFIFO        = (uint16_t)(lo | (uint16_t)(hi << k_ra8_usb_byte_bits));
  }
  if ((len & 1U) != 0U) {
    /* Trailing odd byte: switch to MBW=8 and write one byte. A 16-bit
     * write here adds a phantom zero byte to DTLN and the host sees
     * EOVERFLOW on every odd-length descriptor. Mirrors FSP
     * hw_usb_write_fifo8 for USBFS. HUM Ch 36.2.6 CFIFOSEL.MBW. */
    const uint16_t sel_save = reg->CFIFOSEL;
    const uint16_t sel_8 =
      (uint16_t)((sel_save & (uint16_t)~(uint16_t)k_ra8_fifosel_mbw_msk) | k_ra8_fifosel_mbw_8);
    volatile uint8_t* const cfifo8 = (volatile uint8_t*)(uintptr_t)&reg->CFIFO;
    reg->CFIFOSEL                  = sel_8;
    *cfifo8                        = data[len - 1U];
    reg->CFIFOSEL                  = sel_save;
  }
}

/**
 * @brief HS-only: 32-bit CFIFO read loop for the head bytes.
 * @details Mirrors FSP hw_usb_read_fifo32: cast &CFIFO to uint32_t* and
 *          read `len/4` 32-bit words into the destination buffer
 *          (little-endian byte order).
 * @param[in]  reg  HS register block.
 * @param[out] data Destination byte pointer.
 * @param[in]  len  Total payload length; reads only the head (len & ~0x3) bytes.
 * @pre CFIFOSEL.MBW == 32.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by exactly (len & ~0x3) bytes.
 * @post data[0..(len&~0x3)-1] holds the received bytes in LE order.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fifo_read_hs_head(volatile r_usb_regs_t* reg, uint8_t* data, uint16_t len)
{
  volatile const ra8_usb_cfifo32_t* const cfifo32 =
    (volatile ra8_usb_cfifo32_t*)(uintptr_t)&reg->CFIFO;
  const uint16_t quads = (uint16_t)(len >> 2U);
  for (uint16_t i = 0U; i < quads; ++i) {
    const uint32_t word = *cfifo32;
    data[(4U * i) + 0U] = (uint8_t)(word & k_ra8_usb_byte_mask);
    data[(4U * i) + 1U] = (uint8_t)((word >> k_ra8_usb_shift_b1) & k_ra8_usb_byte_mask);
    data[(4U * i) + 2U] = (uint8_t)((word >> k_ra8_usb_shift_b2) & k_ra8_usb_byte_mask);
    data[(4U * i) + 3U] = (uint8_t)((word >> k_ra8_usb_shift_b3) & k_ra8_usb_byte_mask);
  }
}

/**
 * @brief HS-only: read trailing 1..3 bytes from CFIFOH / CFIFOHH aliases.
 * @details Mirrors FSP hw_usb_read_fifo16 / hw_usb_read_fifo8 (little-endian).
 *          On USBHS a narrow read must go to CFIFOH (+0x02) / CFIFOHH (+0x03);
 *          reading CFIFO itself at MBW=16/8 does not advance the read pointer.
 * @param[in]  reg  HS register block.
 * @param[out] data Destination byte pointer.
 * @param[in]  len  Total payload length; reads only the tail (len & 0x3) bytes.
 * @pre CFIFOSEL.MBW == 32 on entry (restored on exit).
 * @pre data != NULL when len > 0.
 * @post Tail bytes written; CFIFOSEL.MBW restored.
 * @post DTLN advanced by exactly (len & 0x3) bytes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_fifo_read_hs_tail(volatile r_usb_regs_t* reg, uint8_t* data, uint16_t len)
{
  const uint16_t tail = (uint16_t)(len & 0x3U);
  if (tail == 0U) {
    return;
  }
  /* HUM Ch 37.2.7 CFIFO: at MBW=32 the FIFO read pointer only advances
   * on 32-bit reads at CFIFO+0. Narrow CFIFOH/CFIFOHH reads at MBW=16/8
   * do not advance the pointer reliably (a 3-byte bulk-OUT packet drains
   * as bytes [0],[1],[0] when mixed). For the trailing 1..3 bytes of a
   * non-quadword packet we keep MBW=32 and consume one final 32-bit
   * word, extracting only the `tail` low-order bytes (the FIFO presents
   * unused-byte slots as don't-care). One read, one pointer advance,
   * no MBW transition. */
  const uint32_t word     = *(volatile ra8_usb_cfifo32_t*)(uintptr_t)&reg->CFIFO;
  const uint16_t off_base = (uint16_t)(len & (uint16_t)~(uint16_t)0x3U);
  const uint8_t  bytes[4] = {
    (uint8_t)(word & k_ra8_usb_byte_mask),
    (uint8_t)((word >> k_ra8_usb_shift_b1) & k_ra8_usb_byte_mask),
    (uint8_t)((word >> k_ra8_usb_shift_b2) & k_ra8_usb_byte_mask),
    (uint8_t)((word >> k_ra8_usb_shift_b3) & k_ra8_usb_byte_mask),
  };
  for (uint16_t i = 0U; i < tail; ++i) {
    data[off_base + i] = bytes[i];
  }
}

/**
 * @brief Drain the CFIFO data port into a buffer.
 * @details Dispatches to the speed-appropriate access width: USBHS requires
 *          32-bit reads (MBW=32) -- a 16-bit read does not advance the FIFO
 *          read pointer (HUM Ch 37.2.7 p 2070, FSP hw_usb_read_fifo32).
 *          USBFS keeps MBW=16. Mirrors FSP `usb_pstd_read_fifo` for IP1/IP0.
 * @param[in]  reg  USB instance register block.
 * @param[out] data Destination byte pointer (may be NULL when len == 0).
 * @param[in]  len  Number of bytes to drain.
 * @pre Caller selected DCP / pipe with priv_select_cfifo and FRDY=1.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by len bytes.
 * @post data[0..len-1] holds the received payload bytes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void priv_fifo_read(volatile r_usb_regs_t* reg, uint8_t* data, uint16_t len)
{
  /* HUM Ch 37.2.7 "CFIFO : CFIFO Port Register" p 2070 */
  if (priv_is_hs(reg)) {
    internal_fifo_read_hs_head(reg, data, len);
    internal_fifo_read_hs_tail(reg, data, len);
    return;
  }
  /* USBFS / MBW=16 path. */
  const uint16_t even = (uint16_t)(len >> 1U);
  for (uint16_t i = 0U; i < even; ++i) {
    const uint16_t word = reg->CFIFO;
    data[(2U * i) + 0U] = (uint8_t)(word & k_ra8_usb_byte_mask);
    data[(2U * i) + 1U] = (uint8_t)((word >> k_ra8_usb_byte_bits) & k_ra8_usb_byte_mask);
  }
  if ((len & 1U) != 0U) {
    /* Trailing odd byte: switch to MBW=8 and read one byte so DTLN
     * advances by exactly 1. Mirrors FSP hw_usb_read_fifo8 for USBFS. */
    const uint16_t sel_save = reg->CFIFOSEL;
    const uint16_t sel_8 =
      (uint16_t)((sel_save & (uint16_t)~(uint16_t)k_ra8_fifosel_mbw_msk) | k_ra8_fifosel_mbw_8);
    volatile const uint8_t* const cfifo8 = (volatile uint8_t*)(uintptr_t)&reg->CFIFO;
    reg->CFIFOSEL                        = sel_8;
    data[len - 1U]                       = *cfifo8;
    reg->CFIFOSEL                        = sel_save;
  }
}

/* =============================================================================
 * Shared DCP chunk push (device DCP-IN + host control-write data path)
 * =============================================================================
 */

/**
 * @brief Push a single DCP IN chunk: wait for FRDY, write FIFO, pulse BVAL.
 *
 * @details Bounded helper extracted from ``ra8_usb_dcp_in_data`` so the
 * top-level function stays under the clang-tidy
 * ``readability-function-size`` threshold. Performs exactly one
 * controller-buffer transfer cycle.
 *
 * @param[in,out] reg DCP register block (chip or host shim).
 * @param[in]     p   Source byte pointer; must hold at least ``n`` bytes.
 * @param[in]     n   Chunk size in bytes; must be > 0 and <= the DCP MPS.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            Chunk queued; BVAL pulsed.
 * @retval k_ra8_err_timeout   FRDY never asserted within the bound.
 *
 * @pre ``reg`` was returned by ``priv_pick()`` and is non-NULL.
 * @pre CFIFO is already selected on DCP (CURPIPE=0) in IN direction.
 * @post On success, the controller buffer holds the new chunk and BVAL
 *       has been pulsed.
 * @post On error, no PID transition has been performed.
 *
 * @note Not thread-safe; the parent function holds the DCP lock.
 * @since 0.1.0
 */
ra8_err_t priv_dcp_push_chunk(volatile r_usb_regs_t* reg, const uint8_t* p, uint16_t n)
{
  const ra8_err_t ready = priv_wait_frdy(reg);
  RA8_RETURN_ON_ERROR(ready, s_tag, "dcp_in_data: FRDY timeout (chunk)");
  priv_fifo_write(reg, p, n);
  /* HUM Ch 36.2.8 "CFIFOCTR : CFIFO Port Control Register", p 1979.
   *
   * 2026-05-20 attempt: FSP r_usb_plibusbip.c only sets BVAL on the
   * short last chunk (n < MPS), letting hardware auto-flag full-MPS
   * chunks. Tried that here; usb_cdc_echo dropped off the bus and
   * the chip didn't enumerate. Our synchronous multi-chunk push (vs
   * FSP's BRDY-IRQ-per-chunk) apparently requires the explicit BVAL
   * pulse on every chunk to commit each one before the next FRDY
   * poll. Keep the unconditional write. */
  reg->CFIFOCTR = k_ra8_fifoctr_bval;
  return k_ra8_ok;
}
