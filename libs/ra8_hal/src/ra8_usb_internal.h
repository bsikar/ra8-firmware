/**
 * @file ra8_usb_internal.h
 * @brief Cross-TU surface for the ra8_usb driver split.
 * @ingroup grp_hal_usb
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Not part of the public API. The native USB controller driver
 * (HUM Ch 36 USBFS, Ch 37 USBHS) was partitioned across several
 * translation units so every file stays under the 1000-line cap:
 *
 *  - ``ra8_usb.c``           -- shared low-level register helpers
 *                              (the symbols promoted below) plus the
 *                              CFIFO byte-mover.
 *  - ``ra8_usb_phy.c``       -- USBHS embedded-PHY bring-up sequence.
 *  - ``ra8_usb_device.c``    -- device-mode lifecycle, status and the
 *                              endpoint/pipe configuration entry points.
 *  - ``ra8_usb_xfer.c``      -- device-mode data path (queue IN/OUT, DCP
 *                              control data, SETUP drain, control resp).
 *  - ``ra8_usb_irq.c``       -- IRQ dispatch / callbacks / power +
 *                              host-mode bring-up.
 *  - ``ra8_usb_host_ctrl.c`` -- host-mode control-transfer engine.
 *  - ``ra8_usb_host_bulk.c`` -- host-mode bulk-transfer engine.
 *
 * This header declares every symbol referenced by more than one of
 * those TUs: the shared bound enums, the byte-mask helpers, the host
 * addressing-field enums, and the promoted register helpers (which were
 * TU-private statics before the split). Production code keeps calling
 * the public ``ra8_usb_*`` API in ``ra8_usb.h``; the symbols here are an
 * implementation detail of the driver split only. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_usb.h"
#include "ra8_usb_regs.h"

/**
 * @enum ra8_usb_internal_lim_t
 * @brief Driver-wide bounds that aren't part of the public API.
 */
typedef enum : uint16_t {
  k_ra8_usb_max_pipe_num    = 9U,    /**< PIPE1..PIPE9 + DCP at 0.      */
  k_ra8_usb_max_ep_addr     = 15U,   /**< USB EP number is 4 bits.      */
  k_ra8_usb_max_address     = 127U,  /**< 7-bit USB address.            */
  k_ra8_usb_dcp_max_packet  = 64U,   /**< EP0 default packet size.      */
  k_ra8_usb_pipe_max_packet = 1024U, /**< Max packet ceiling for pipes. */
} ra8_usb_internal_lim_t;

/**
 * @enum ra8_usb_byte_mask_t
 * @brief Byte-extraction masks shared across the FIFO byte path.
 */
typedef enum : uint16_t {
  k_ra8_usb_byte_mask = 0x00FFU, /**< Low byte of a 16-bit FIFO word. */
  k_ra8_usb_byte_bits = 8U,      /**< Bits per byte (shift constant). */
} ra8_usb_byte_mask_t;

/** @brief Bit masks used by the host control-transfer engine. */
typedef enum : uint16_t {
  k_ra8_usb_dcp_pipe0_bit = 0x0001U, /**< BRDYSTS/BEMPSTS DCP (pipe 0) bit. */
  k_ra8_usb_setup_dir_in  = 0x0080U, /**< bmRequestType device-to-host bit. */
  k_ra8_usb_dcpmaxp_mxps  = 0x007FU, /**< DCPMAXP MXPS (max packet) field.  */
  k_ra8_usb_rhst_mask     = 0x0007U, /**< DVSTCTR0.RHST connected-speed.    */
  k_ra8_usb_lnst_mask     = 0x0003U, /**< SYSSTS0.LNST line-state field.    */
} ra8_usb_host_ctrl_bits_t;

/** @brief Offsets / shifts for the device-address (DEVADDn) registers. */
typedef enum : uint32_t {
  k_ra8_usb_devadd0_off   = 0x00D0U, /**< DEVADD0 byte offset from base.  */
  k_ra8_usb_usbspd_shift  = 6U,      /**< DEVADDn.USBSPD field position.  */
  k_ra8_usb_devadd_stride = 2U,      /**< Bytes between DEVADDn slots.    */
  k_ra8_usb_dev_addr_max  = 10U,     /**< Highest DEVADDn slot (DEVADDA). */
} ra8_usb_host_devadd_t;

/** @brief Field layout helpers for host-mode device addressing. */
typedef enum : uint16_t {
  k_ra8_usb_devsel_shift      = 12U,     /**< DCPMAXP/PIPEMAXP DEVSEL pos. */
  k_ra8_usb_devsel_field_mask = 0x000FU, /**< DEVSEL width once shifted.   */
  k_ra8_usb_pipemaxp_mxps     = 0x07FFU, /**< PIPEMAXP MXPS field (the USBHS
                                             instance carries 11 bits, HUM Ch
                                             37.2.36; FS uses the low 9).    */
  k_ra8_usb_pid_stall_bit     = 0x0002U, /**< PID[1]: set for either STALL. */
} ra8_usb_host_addr_bits_t;

/**
 * @brief Resolve the per-speed register pointer.
 *
 * @details Promoted from a TU-private static so every TU in the driver
 * split can map a ::ra8_usb_speed_t to its controller register block.
 *
 * @param[in] speed Controller selector (FS / HS).
 * @return The selected register block, or nullptr for an unknown speed.
 * @retval nullptr ``speed`` is neither FS nor HS.
 * @pre USB module pointers are populated.
 * @pre ``speed`` is a ::ra8_usb_speed_t value.
 * @post No state mutated; the helper is pure.
 * @post Returned pointer identifies the requested controller.
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
RA8_PRIV volatile r_usb_regs_t* internal_pick(ra8_usb_speed_t speed);

/**
 * @brief Resolve the per-speed MSTP id.
 *
 * @details Promoted from a TU-private static so the lifecycle and power
 * TUs share one speed-to-MSTP mapping.
 *
 * @param[in] speed Controller selector (FS / HS).
 * @return Module-stop id for the selected controller.
 * @retval k_ra8_mstp_usbhs ``speed == k_ra8_usb_speed_hs``.
 * @retval k_ra8_mstp_usbfs Any other speed value.
 * @pre Module state is consistent.
 * @pre ``speed`` is a ::ra8_usb_speed_t value.
 * @post No state mutated; the helper is pure.
 * @post Returned id matches the requested controller.
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_mstp_t internal_mstp(ra8_usb_speed_t speed);

/**
 * @brief Apply a generic read-modify-write to a 16-bit register.
 *
 * @details Promoted from a TU-private static. Clears ``clr_mask`` then
 * sets ``set_mask`` in one read / modify / store cycle.
 *
 * @param[in,out] reg      Target register pointer; must not be nullptr.
 * @param[in]     set_mask Bits to set.
 * @param[in]     clr_mask Bits to clear (applied before the set).
 * @pre ``reg`` is non-null and points at a live register.
 * @pre Caller serialises access to the register.
 * @post ``*reg`` reflects ``(old & ~clr_mask) | set_mask``.
 * @post No other register is touched.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV void internal_rmw16(volatile uint16_t* reg, uint16_t set_mask, uint16_t clr_mask);

/**
 * @brief Detect whether reg points at the USBHS (IP1) register block.
 *
 * @details Promoted from a TU-private static so the FIFO byte-mover,
 * the host control engine, and the bulk engine share one IP predicate.
 *
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
RA8_PRIV bool internal_is_hs(volatile r_usb_regs_t* reg);

/**
 * @brief Set CFIFOSEL.MBW + CURPIPE + ISEL for the given pipe / direction.
 *
 * @details Promoted from a TU-private static. Picks MBW=32 for USBHS
 * (FSP USB1_CFIFO_MBW) and MBW=16 for USBFS, then waits for the
 * CURPIPE/ISEL readback to settle.
 *
 * @param[in] reg       USB instance register block.
 * @param[in] pipe_num  CURPIPE value (0 = DCP, 1..n = data pipe).
 * @param[in] is_in_dir true = device-to-host (write), false = the other way.
 * @pre reg != NULL.
 * @pre Caller holds the DCP / pipe lock.
 * @post CFIFOSEL = MBW(speed) | (is_in_dir ? ISEL : 0) | pipe_num.
 * @post Subsequent CFIFO accesses must use the matching width.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void internal_select_cfifo(volatile r_usb_regs_t* reg, uint16_t pipe_num, bool is_in_dir);

/**
 * @brief Spin until `CFIFOCTR.FRDY` asserts or a deadline elapses.
 *
 * @details Promoted from a TU-private static. Bounded busy-wait shared
 * by every CFIFO read / write path.
 *
 * @param[in] reg USB instance register block.
 * @return ra8_ok on FRDY, k_ra8_err_hw_timeout otherwise.
 * @retval k_ra8_ok FRDY asserted within the bound.
 * @retval k_ra8_err_hw_timeout FRDY never asserted.
 * @pre Module state is consistent.
 * @pre CFIFOSEL has been programmed for the target pipe.
 * @post No register is modified.
 * @post On timeout the caller aborts the transfer.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_wait_frdy(volatile r_usb_regs_t* reg);

/**
 * @brief Set DCPCTR PID field while preserving the rest of the register.
 *
 * @details Promoted from a TU-private static so the device, control,
 * and host engines drive the DCP PID through one accessor.
 *
 * @param[in,out] reg DCP register block.
 * @param[in]     pid PID encoding to write (NAK / BUF / STALL).
 * @pre Module state is consistent.
 * @pre reg is non-null and points at a powered controller.
 * @post DCPCTR.PID == ``pid``; the other DCPCTR fields are unchanged.
 * @post No other register is touched.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV void internal_dcp_pid(volatile r_usb_regs_t* reg, ra8_usb_pid_t pid);

/**
 * @brief Set PIPECTR[idx] PID field.
 *
 * @details Promoted from a TU-private static so the pipe-config,
 * device-data, and host engines share one pipe PID accessor.
 *
 * @param[in,out] reg      Controller register block.
 * @param[in]     pipe_num Pipe index 1..9.
 * @param[in]     pid      PID encoding to write (NAK / BUF / STALL).
 * @pre Module state is consistent.
 * @pre pipe_num in [1,9].
 * @post PIPECTR[pipe_num-1].PID == ``pid``.
 * @post No other register is touched.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_PRIV void internal_pipe_pid(volatile r_usb_regs_t* reg, uint8_t pipe_num, ra8_usb_pid_t pid);

/**
 * @brief Push a byte buffer into the CFIFO data port.
 *
 * @details Promoted from a TU-private static. Dispatches to the
 * speed-appropriate access width (USBHS 32-bit, USBFS 16-bit).
 *
 * @param[in] reg  USB instance register block.
 * @param[in] data Source byte pointer (may be NULL when len == 0).
 * @param[in] len  Number of bytes to push.
 * @pre Caller selected DCP / pipe with internal_select_cfifo and FRDY=1.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by len bytes.
 * @post FIFO ready for caller's BVAL commit.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void internal_fifo_write(volatile r_usb_regs_t* reg, const uint8_t* data, uint16_t len);

/**
 * @brief Drain the CFIFO data port into a buffer.
 *
 * @details Promoted from a TU-private static. Dispatches to the
 * speed-appropriate access width (USBHS 32-bit, USBFS 16-bit).
 *
 * @param[in]  reg  USB instance register block.
 * @param[out] data Destination byte pointer (may be NULL when len == 0).
 * @param[in]  len  Number of bytes to drain.
 * @pre Caller selected DCP / pipe with internal_select_cfifo and FRDY=1.
 * @pre data != NULL when len > 0.
 * @post DTLN advanced by len bytes.
 * @post data[0..len-1] holds the received payload bytes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void internal_fifo_read(volatile r_usb_regs_t* reg, uint8_t* data, uint16_t len);

/**
 * @brief Push a single DCP IN chunk: wait FRDY, write FIFO, pulse BVAL.
 *
 * @details Promoted from a TU-private static so both the device DCP-IN
 * data path and the host control-write data path share one chunk push.
 *
 * @param[in,out] reg DCP register block (chip or host shim).
 * @param[in]     p   Source byte pointer; must hold at least ``n`` bytes.
 * @param[in]     n   Chunk size in bytes; must be > 0 and <= the DCP MPS.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Chunk queued; BVAL pulsed.
 * @retval k_ra8_err_hw_timeout FRDY never asserted within the bound.
 * @pre ``reg`` was returned by ``internal_pick()`` and is non-NULL.
 * @pre CFIFO is already selected on DCP (CURPIPE=0) in IN direction.
 * @post On success, the controller buffer holds the new chunk and BVAL pulsed.
 * @post On error, no PID transition has been performed.
 * @note Not thread-safe; the caller holds the DCP lock.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_dcp_push_chunk(volatile r_usb_regs_t* reg,
                                           const uint8_t*         p,
                                           uint16_t               n);

/**
 * @brief Compute the PIPEBUF word for a bulk pipe (2*MPS region).
 *
 * @details Promoted from a TU-private static so the device-side and
 * host-side pipe configurators pack PIPEBUF the same way.
 *
 * @param[in] pipe_num   PIPE number 1..9.
 * @param[in] max_packet Pipe MPS (bytes).
 * @return PIPEBUF word ready to be written to register ``PIPEBUF``.
 * @retval 0..0xFFFF Packed BUFNMB/BUFSIZE word (no error condition).
 * @pre ``pipe_num`` is in the 1..9 range (caller-checked).
 * @pre ``max_packet`` is the pipe's MPS (caller validated <= 1024).
 * @post No global state is touched; the helper is pure.
 * @post Returned word satisfies HUM Ch 37.2.35 PIPEBUF layout.
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
RA8_PRIV uint16_t internal_pipebuf_word(uint8_t pipe_num, uint16_t max_packet);

/**
 * @brief Pack PIPECFG fields for a configured non-control pipe.
 *
 * @details Promoted from a TU-private static so the device-side and
 * host-side pipe configurators encode PIPECFG the same way.
 *
 * @param[in] ep_addr Endpoint address (low nibble = EP number).
 * @param[in] dir     Pipe direction (::k_ra8_usb_ep_dir_in or _out).
 * @param[in] type    Pipe type (bulk / interrupt / iso).
 * @param[in] dblb_in Double-bank bulk IN pipes (host mode true,
 *                    device mode false; ignored for OUT / non-bulk).
 * @return PIPECFG word ready to write to ``PIPECFG``.
 * @retval 0..0xFFFF Packed configuration word; no error condition.
 * @pre ``ep_addr`` low nibble is the EP number (caller validated 1..15).
 * @pre ``dir`` / ``type`` are valid enum values.
 * @post No global state is touched; the helper is pure.
 * @post For bulk pipes SHTNAK is set; DBLB only when ``dblb_in`` is
 *       true with ``dir == IN``.
 * @note Pure / thread-safe.
 * @since 0.1.0
 */
RA8_PRIV uint16_t internal_pipecfg_word(uint8_t           ep_addr,
                                        ra8_usb_ep_dir_t  dir,
                                        ra8_usb_ep_type_t type,
                                        bool              dblb_in);

/**
 * @brief Quiesce the pipe so PIPECFG/PIPEMAXP/PIPEPERI become writable.
 *
 * @details Promoted from a TU-private static so both the device-side and
 * host-side pipe configurators park a pipe at PID=NAK before re-programming.
 *
 * @param[in,out] reg      Controller register window.
 * @param[in]     pipe_num Pipe index 1..9.
 * @pre reg is non-null and points at a powered controller.
 * @pre pipe_num in [1,9].
 * @post BRDY/NRDY/BEMPENB bit for pipe_num is cleared.
 * @post PIPECTR PID for pipe_num == NAK.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void internal_pipe_quiesce(volatile r_usb_regs_t* reg, uint8_t pipe_num);

/**
 * @brief USBHS embedded-PHY bring-up (HUM Figure 37.2 p 2121).
 *
 * @details Promoted from a TU-private static in ``ra8_usb_phy.c`` so both
 * the device-mode init (``ra8_usb_device.c``) and the host-mode init
 * (``ra8_usb_irq.c``) can run the UTMI PHY bring-up. Implements the
 * device-mode PHY bring-up flow from HUM Ch 37.3.3 + Figure 37.2.
 *
 * @param[in] reg HS register block pointer (must be ::ra8_usb_hs()).
 * @return ::ra8_err_t
 * @retval k_ra8_ok HS PHY locked and ready for ``DPRPU`` attach.
 * @retval k_ra8_err_hw_timeout PLLSTA.PLLLOCK never asserted.
 * @pre MSTPB12 ungated; USB60CLK = PLL2P / 4 = 60 MHz.
 * @pre Caller is single-threaded init context.
 * @post On success: PHY powered, PLL locked, USBE=1, SUSPENDM=1.
 * @post BUSWAIT programmed.
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_usbhs_phy_bringup(volatile r_usb_regs_t* reg);

/**
 * @brief Programme the post-SYSCFG common registers (FIFO, DCP, INTENB).
 *
 * @details Promoted from a TU-private static in ``ra8_usb_phy.c`` so the
 * device-mode init (``ra8_usb_device.c``) can apply the shared FS/HS tail
 * of device bring-up.
 *
 * @param[in] reg Selected USB instance register block.
 * @pre Caller has already enabled SYSCFG.USBE.
 * @pre reg is non-null.
 * @post All listed registers carry deterministic device-mode defaults.
 * @post INTENB1 / BRDYENB / NRDYENB / BEMPENB are cleared.
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_PRIV void internal_usb_init_common(volatile r_usb_regs_t* reg);

/**
 * @brief USBFS module bring-up (FSP `hw_usb_pmodule_init` IP0 branch).
 *
 * @details Promoted from a TU-private static in ``ra8_usb_phy.c`` so the
 * device-mode init (``ra8_usb_device.c``) can run the FS SYSCFG sequence.
 *
 * @param[in] reg FS register block pointer (must be ::ra8_usb_fs()).
 * @return ::ra8_err_t
 * @retval k_ra8_ok USBE asserted.
 * @pre MSTPB11 ungated; USB48CLK fed from PLL2P/5 = 48 MHz.
 * @pre Caller is single-threaded init context.
 * @post SYSCFG: SCKE=1, DRPD=0, USBE=1.
 * @post Loop iteration count is bounded.
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_usbfs_module_bringup(volatile r_usb_regs_t* reg);

/**
 * @brief Program DEVADDn.USBSPD with the connected device's link speed.
 *
 * @details Promoted from a TU-private static so the host control engine
 * (``ra8_usb_host_ctrl.c``) and the host bulk engine
 * (``ra8_usb_host_bulk.c``) share one DEVADDn programmer. The DEVADDn
 * registers sit past the modelled register window, so this addresses the
 * slot by raw offset (0xD0 + 2n) and copies DVSTCTR0.RHST into USBSPD[7:6].
 *
 * @param[in] reg      Selected controller register block (its base address).
 * @param[in] dev_addr Address slot to program (0..::k_ra8_usb_dev_addr_max).
 * @pre The bus reset has completed so RHST reflects the device speed.
 * @pre @p reg is non-NULL and @p dev_addr is within the DEVADD range.
 * @post DEVADDn.USBSPD matches the connected speed; the address is usable.
 * @post No other DEVADDn field is changed.
 * @note Single-device bring-up: no hub fields (UPPHUB/HUBPORT) are set.
 * @since 0.1.0
 */
RA8_PRIV void internal_host_program_devadd(volatile r_usb_regs_t* reg, uint8_t dev_addr);

#ifdef __cplusplus
}
#endif
