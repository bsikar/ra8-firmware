/**
 * @file ra_i3c.h
 * @brief I3C Bus Interface driver -- CCC engine + private read/write
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * This module exposes the RA8D2 I3C0 controller as a master capable
 * of issuing the standard MIPI Common Command Codes (CCCs), private
 * read / write transfers in SDR mode, and reading inbound
 * In-Band-Interrupt (IBI) descriptors from the IBI queue.  The
 * implementation mirrors FSP ``r_i3c`` -- every transfer is built as
 * a 32-bit (or two-word) command descriptor that is pushed into
 * NCMDQP, with payload bytes flowing through NTDTBP0 and IBI events
 * arriving via NIBIQP.  See HUM Ch 40 "I3C Bus Interface (I3C)" pp
 * 2445-2701 for the register-level reference.
 *
 * Public API:
 * - lifecycle / status / IRQ:  ``ra_i3c_init``, ``_deinit``,
 *   ``_get_status``, ``_clear_status``, ``_attach_handler``,
 *   ``_dispatch``, ``_enter_stop``, ``_exit_stop``,
 *   ``_set_address``, ``_bus_enable``
 * - dynamic address assignment: ``ra_i3c_dynamic_address_assign``
 *   (ENTDAA), ``ra_i3c_set_dynamic_address`` (SETDASA),
 *   ``ra_i3c_reset_dynamic_addresses`` (broadcast RSTDAA)
 * - generic CCC: ``ra_i3c_send_ccc`` / ``ra_i3c_recv_ccc``
 * - private SDR: ``ra_i3c_write`` / ``ra_i3c_read``
 * - IBI inbound: ``ra_i3c_ibi_read``
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @typedef ra_i3c_event_fn_t
 * @brief I3C event callback fired by ``ra_i3c_dispatch``.
 *
 * @param ctx       Opaque user context registered with
 *                  ``ra_i3c_attach_handler``.
 * @param status_mask Snapshot of the INST register at dispatch time.
 */
typedef void (*ra_i3c_event_fn_t)(void* ctx, uint32_t status_mask);

/**
 * @struct ra_i3c_daa_target_t
 * @brief One target entry filled in by ENTDAA.
 *
 * @details
 * Each row in the array passed to ``ra_i3c_dynamic_address_assign``
 * is filled with the 48-bit Provisional ID, the Bus Characteristic
 * Register, and the Device Characteristic Register read during the
 * "address phase" of ENTDAA, plus the dynamic address that the
 * controller assigned.  Caller must pre-populate ``dynamic_address``
 * with the address that this target should receive.
 *
 * cppcheck cannot see test consumers that read every field, so
 * ``unusedStructMember`` is suppressed via a paired begin/end.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t pid[6];          /**< 48-bit Provisional ID (BE order). */
  uint8_t bcr;             /**< Bus Characteristic Register byte. */
  uint8_t dcr;             /**< Device Characteristic Register byte. */
  uint8_t dynamic_address; /**< Address assigned to this target. */
} ra_i3c_daa_target_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_i3c_ibi_type_t
 * @brief Type encoded in an IBI status descriptor (bit 31 IBI_ST,
 *        bits 1:0 of the IBI ID).
 */
typedef enum : uint8_t {
  k_ra_i3c_ibi_type_interrupt = 0U, /**< Slave-initiated IBI. */
  k_ra_i3c_ibi_type_hot_join  = 1U, /**< Hot-join request. */
  k_ra_i3c_ibi_type_master    = 2U, /**< Mastership request. */
} ra_i3c_ibi_type_t;

/**
 * @struct ra_i3c_ibi_t
 * @brief Decoded IBI inbound descriptor.
 *
 * @details
 * Returned by ``ra_i3c_ibi_read`` -- the 32-bit IBI status
 * descriptor word at NIBIQP is split into the originating address,
 * the type, the payload length, and any payload bytes that were
 * appended to the descriptor (read from the same FIFO).
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint8_t           address;     /**< Originating dynamic address. */
  ra_i3c_ibi_type_t type;        /**< IBI / HJ / MR classification. */
  uint8_t           payload_len; /**< Bytes that follow in payload[]. */
  uint8_t           payload[8];  /**< Up to 8 bytes of payload. */
  uint8_t           last;        /**< 1 if this is the final fragment. */
} ra_i3c_ibi_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Initialise the I3C controller.
 *
 * @details
 * Powers the I3C MSTP gate, runs the FSP-mirrored reset sequence,
 * and zeros every status / mask / address register so the bus is
 * left in a deterministic idle state.
 *
 * @retval k_ra_ok            Controller ready for ``ra_i3c_bus_enable``.
 * @retval k_ra_err_*         MSTP enable failed; see ``ra_mstp.h``.
 * @pre  MSTP module is initialised.
 * @pre  No other module owns the I3C peripheral.
 * @post CECTL.CLKE == 1 and INIE == INSTE == 0.
 * @post BCTL.BUSE == 0.
 * @note Not thread-safe; call from single-threaded init.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_init(void);

/**
 * @brief Tear down the I3C controller.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_deinit(void);

/**
 * @brief Programme the active 7-bit master/slave device address.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_set_address(uint32_t addr);

/**
 * @brief Enable / disable bus-master operation via BCTL.BUSE.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_bus_enable(bool enable);

/**
 * @brief Read the INST status register.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_get_status(uint32_t* out_mask);

/**
 * @brief Clear INST status bits.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_clear_status(uint32_t mask);

/**
 * @brief Attach an I3C event callback.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_attach_handler(ra_i3c_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch an I3C event -- snapshot status + fire callback.
 * @since 0.1.0
 */
void ra_i3c_dispatch(void);

/**
 * @brief Put I3C into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_exit_stop(void);

/**
 * @brief Issue ENTDAA (broadcast 0x07) to enumerate I3C targets.
 *
 * @details
 * Builds an "address-assignment" command descriptor for the ENTDAA
 * CCC, pushes it through NCMDQP, and reads back PID/BCR/DCR for
 * each target via NTDTBP0.  Each target row in @p targets must have
 * its ``dynamic_address`` field pre-populated with the address that
 * the controller should hand out.  The PID/BCR/DCR fields are
 * filled from the responses.  See HUM Ch 40 "Enter Dynamic Address
 * Assignment" pp 2445-2701.
 *
 * @param[in,out] targets Caller-allocated array; rows updated in place.
 * @param[in]     target_count Number of valid rows in @p targets.
 *                  Must be in the range 1 .. k_ra_i3c_max_targets.
 *
 * @retval k_ra_ok                Command queued; FIFOs read.
 * @retval k_ra_err_null_ptr      @p targets was NULL.
 * @retval k_ra_err_invalid_arg   @p target_count was 0 or out of range.
 * @pre  ``ra_i3c_init`` succeeded.
 * @pre  Bus is enabled (``ra_i3c_bus_enable(true)``).
 * @post NCMDQP holds the ENTDAA descriptor.
 * @post @p targets reflect responses received before STOP.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_dynamic_address_assign(ra_i3c_daa_target_t* targets,
                                                     uint8_t              target_count);

/**
 * @brief Issue SETDASA (direct 0x87) to assign one target a dynamic address.
 *
 * @param[in] static_addr  7-bit static address currently used by the target.
 * @param[in] dynamic_addr 7-bit dynamic address to assign.
 *
 * @retval k_ra_ok               Descriptor queued.
 * @retval k_ra_err_invalid_arg  Address out of 7-bit range.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_set_dynamic_address(uint8_t static_addr, uint8_t dynamic_addr);

/**
 * @brief Issue RSTDAA (broadcast 0x06) to clear all dynamic addresses.
 * @retval k_ra_ok               Descriptor queued.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_reset_dynamic_addresses(void);

/**
 * @brief Generic CCC send (broadcast or directed write).
 *
 * @details
 * For broadcast CCCs (opcode < 0x80), @p target_addr is ignored.
 * For directed CCCs (opcode >= 0x80), @p target_addr selects the
 * target.  Up to ``k_ra_i3c_immediate_max_bytes`` of payload may be
 * passed; longer payloads are pushed through NTDTBP0 word-by-word.
 *
 * @param[in] ccc          Common Command Code opcode.
 * @param[in] target_addr  Target dynamic address (directed CCCs only).
 * @param[in] payload      Optional payload buffer; may be NULL when len==0.
 * @param[in] len          Payload byte count.
 * @retval k_ra_ok               Descriptor queued.
 * @retval k_ra_err_invalid_arg  Address out of range, or len exceeds queue.
 * @retval k_ra_err_null_ptr     payload NULL but len > 0.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_i3c_send_ccc(uint8_t ccc, uint8_t target_addr, const uint8_t* payload, uint8_t len);

/**
 * @brief Generic CCC receive (directed read).
 *
 * @param[in]  ccc         Direct CCC opcode (must have bit 7 set).
 * @param[in]  target_addr Target dynamic address.
 * @param[out] buf         Caller buffer to receive bytes from NTDTBP0.
 * @param[in]  max_len     Maximum bytes to drain.
 * @param[out] got_len     Number of bytes actually drained.
 * @retval k_ra_ok               Read complete.
 * @retval k_ra_err_invalid_arg  Non-direct CCC, or max_len == 0.
 * @retval k_ra_err_null_ptr     Output pointers NULL.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_i3c_recv_ccc(uint8_t ccc, uint8_t target_addr, uint8_t* buf, uint8_t max_len, uint8_t* got_len);

/**
 * @brief Private write (SDR) to a 7-bit dynamic-addressed target.
 *
 * @param[in] target_addr 7-bit target dynamic address.
 * @param[in] data        Bytes to push through NTDTBP0.
 * @param[in] len         Byte count.  Up to 4 bytes use immediate-data
 *                        transfer; longer transfers use the regular
 *                        FIFO descriptor.
 * @retval k_ra_ok               Descriptor + payload queued.
 * @retval k_ra_err_invalid_arg  Address out of range.
 * @retval k_ra_err_null_ptr     data NULL but len > 0.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_write(uint8_t target_addr, const uint8_t* data, uint32_t len);

/**
 * @brief Private read (SDR) from a 7-bit dynamic-addressed target.
 *
 * @param[in]  target_addr 7-bit target dynamic address.
 * @param[out] buf         Caller buffer; bytes drained from NTDTBP0.
 * @param[in]  len         Byte count to read.
 * @retval k_ra_ok               Read complete.
 * @retval k_ra_err_invalid_arg  Address out of range, or len == 0.
 * @retval k_ra_err_null_ptr     buf NULL.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_read(uint8_t target_addr, uint8_t* buf, uint32_t len);

/**
 * @brief Drain one inbound IBI from NIBIQP.
 *
 * @param[out] out_ibi Caller-supplied descriptor; populated when
 *                     ``k_ra_ok`` is returned.
 * @retval k_ra_ok               One IBI dequeued.
 * @retval k_ra_err_no_data      IBI queue empty.
 * @retval k_ra_err_null_ptr     out_ibi NULL.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_i3c_ibi_read(ra_i3c_ibi_t* out_ibi);

#ifdef __cplusplus
}
#endif
