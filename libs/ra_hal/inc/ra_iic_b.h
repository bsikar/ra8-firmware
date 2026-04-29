/**
 * @file ra_iic_b.h
 * @brief IIC_B (I3C unified IP, I2C-only mode) master driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling-mode master driver for the RA8D2 I3C peripheral operated in
 * I2C compatibility mode (HUM Ch 40 "I3C Bus Interface (I3C)",
 * p 2445-2701). The peripheral name in FSP and in this codebase is
 * ``IIC_B`` -- it replaces the legacy IIC block that older RA parts
 * carried.
 *
 * The public surface is intentionally small for the first pass:
 *
 * - ``ra_iic_b_init``           configure + MSTP enable
 * - ``ra_iic_b_deinit``         disable + MSTP release
 * - ``ra_iic_b_write``          polling write to a 7-bit target
 * - ``ra_iic_b_read``           polling read from a 7-bit target
 * - ``ra_iic_b_scan``           probe a 7-bit target without payload
 * - ``ra_iic_b_attach_handler`` register completion / error callback
 *
 * NSC veneer wrappers (``ra_iic_init`` / ``ra_iic_write`` /
 * ``ra_iic_read``) are kept as thin pass-throughs so that the existing
 * cross-world communications surface in ``libs/ra_nsc`` keeps
 * compiling without churn during the retrofit.
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

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra_iic_b_speed_t
 * @brief Supported bus speeds (I2C compatibility mode).
 *
 * @details
 * Per HUM Ch 40.1.1 Table 40.1 "I2C specifications", p 2445.
 */
typedef enum : uint32_t {
  k_ra_iic_b_speed_standard  = 100000U,  /**< 100 kHz Sm. */
  k_ra_iic_b_speed_fast      = 400000U,  /**< 400 kHz Fm. */
  k_ra_iic_b_speed_fast_plus = 1000000U, /**< 1 MHz Fm+.  */
} ra_iic_b_speed_t;

/**
 * @struct ra_iic_b_cfg_t
 * @brief Configuration descriptor for ``ra_iic_b_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra_iic_b_init`` in
 * ``libs/ra_hal/src/ra_iic_b.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t bus_hz;   /**< Target I2C clock rate in Hz. */
  uint32_t pclka_hz; /**< Current PCLKA / I3CCLK frequency for STDBR calc. */
} ra_iic_b_cfg_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @enum ra_iic_b_err_mask_t
 * @brief Error-mask bits returned by ``ra_iic_b_get_errors``.
 */
typedef enum : uint8_t {
  k_ra_iic_b_err_none     = 0x00U, /**< No latched error. */
  k_ra_iic_b_err_arb_lost = 0x01U, /**< BST.ALF set.      */
  k_ra_iic_b_err_nack     = 0x02U, /**< BST.NACKDF set.   */
  k_ra_iic_b_err_timeout  = 0x04U, /**< BST.TODF set.     */
} ra_iic_b_err_mask_t;

/**
 * @typedef ra_iic_b_complete_fn_t
 * @brief Transfer-complete / error callback signature.
 *
 * @param[in] ctx      Caller-supplied context.
 * @param[in] err_mask OR of ``k_ra_iic_b_err_*`` bits; zero on success.
 */
typedef void (*ra_iic_b_complete_fn_t)(void* ctx, uint8_t err_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the IIC_B channel and bring the bus up.
 *
 * @param[in] channel Channel index (only ``0`` is valid on RA8D2).
 * @param[in] cfg     Configuration descriptor.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Channel initialised, BCTL.BUSE = 1.
 * @retval k_ra_err_null_ptr    ``cfg`` is NULL.
 * @retval k_ra_err_invalid_arg ``channel`` out of range or
 *                              ``cfg->bus_hz`` zero.
 * @retval k_ra_err_hw_timeout  RSTCTL.RI3CRST didn't self-clear.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra_mstp_init`` has been called.
 * @post On success ``BCTL.BUSE`` is set and the channel is ready to
 *       service ``ra_iic_b_write`` / ``ra_iic_b_read``.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_b_init(uint8_t channel, const ra_iic_b_cfg_t* cfg);

/**
 * @brief Tear down the IIC_B channel.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Channel torn down, MSTP gated.
 * @retval k_ra_err_invalid_arg ``channel`` out of range.
 *
 * @pre Caller is not in the middle of a transfer.
 * @post BCTL.BUSE cleared and I3C MSTP bit ref-released.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_b_deinit(uint8_t channel);

/**
 * @brief Update the bus clock without tearing the channel down.
 *
 * @param[in] channel  Channel index.
 * @param[in] bus_hz   New bus clock in Hz (non-zero).
 * @param[in] pclka_hz Current PCLKA frequency in Hz.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              STDBR programmed.
 * @retval k_ra_err_invalid_arg Channel / clock out of range.
 *
 * @pre Channel previously initialised.
 * @post STDBR.SBRLO / SBRHO reflect the new divider.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_b_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclka_hz);

/* =============================================================================
 * Polling transfers
 * =============================================================================
 */

/**
 * @brief Polling write of ``len`` bytes to a 7-bit target address.
 *
 * @details
 * Mirrors FSP's ``R_IIC_B_MASTER_Write`` flow without the DTC fast
 * path: issue START, send ``(target_7b<<1)|W``, push payload bytes
 * into ``NTDTBP0`` waiting for ``NTST.TDBEF0`` between each one,
 * wait for ``BST.TENDF`` after the last byte, then issue STOP.
 *
 * @param[in] channel   Channel index.
 * @param[in] target_7b 7-bit slave address.
 * @param[in] data      Buffer to send (must be non-NULL even when
 *                      ``len`` is zero).
 * @param[in] len       Byte count.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Transfer succeeded; STOP issued.
 * @retval k_ra_err_null_ptr    ``data`` is NULL or channel invalid.
 * @retval k_ra_err_hw_timeout  START / TDBEF0 / TENDF poll timed out.
 * @retval k_ra_err_hw_error    NACK / arbitration-lost detected.
 *
 * @pre Channel previously initialised.
 * @post BST status flags cleared; channel ready for the next API call.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_iic_b_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len);

/**
 * @brief Polling read of ``len`` bytes from a 7-bit target.
 *
 * @details
 * Mirrors FSP's ``R_IIC_B_MASTER_Read`` minus DTC: issue START, send
 * ``(target_7b<<1)|R``, dummy-read ``NTDTBP0`` to clock the first
 * byte, then loop reading ``NTDTBP0`` after each ``NTST.RDBFF0``.
 * On the last byte set ``ACKCTL.ACKT`` (with ACKTWP) so the master
 * NACKs and the slave releases the bus, then issue STOP.
 *
 * @param[in]  channel   Channel index.
 * @param[in]  target_7b 7-bit slave address.
 * @param[out] out       Destination buffer (non-NULL).
 * @param[in]  len       Byte count (non-zero).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Transfer succeeded; STOP issued.
 * @retval k_ra_err_null_ptr    ``out`` is NULL or channel invalid.
 * @retval k_ra_err_invalid_arg ``len`` is zero.
 * @retval k_ra_err_hw_timeout  Bus / data-buffer poll timed out.
 * @retval k_ra_err_hw_error    NACK / arbitration-lost detected.
 *
 * @pre Channel previously initialised.
 * @post BST status flags cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_iic_b_read(uint8_t channel, uint8_t target_7b, uint8_t* out, uint32_t len);

/**
 * @brief Probe whether a 7-bit address ACKs.
 *
 * @details
 * Issues START, writes the address byte, waits for the ACK / NACK
 * status, and issues STOP. Equivalent to a single ``i2cdetect``
 * sweep entry.
 *
 * @param[in]  channel   Channel index.
 * @param[in]  target_7b 7-bit slave address.
 * @param[out] out_acked Set to ``true`` when the slave ACKs, ``false``
 *                       on NACK.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Probe completed (ACK or NACK).
 * @retval k_ra_err_null_ptr    ``out_acked`` is NULL or channel invalid.
 * @retval k_ra_err_hw_timeout  Status poll timed out.
 *
 * @pre Channel previously initialised.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_b_scan(uint8_t channel, uint8_t target_7b, bool* out_acked);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read latched error flags from BST (AL / NACKDF / TODF).
 *
 * @param[in]  channel  Channel index.
 * @param[out] out_mask OR of ``k_ra_iic_b_err_*`` bits.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              ``out_mask`` populated.
 * @retval k_ra_err_null_ptr    ``out_mask`` is NULL.
 * @retval k_ra_err_invalid_arg ``channel`` out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_b_get_errors(uint8_t channel, uint8_t* out_mask);

/**
 * @brief Clear latched error flags in BST.
 *
 * @param[in] channel Channel index.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Error bits W0C cleared.
 * @retval k_ra_err_invalid_arg Channel out of range.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_b_clear_errors(uint8_t channel);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach a completion / error callback for the channel.
 *
 * @param[in] channel Channel index.
 * @param[in] fn      Callback fired from the dispatch helpers, or
 *                    NULL to detach.
 * @param[in] ctx     Context pointer passed to the callback.
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok              Callback registered.
 * @retval k_ra_err_invalid_arg Channel out of range.
 *
 * @pre Channel previously initialised.
 * @post BIE / NTIE bits track ``fn`` non-null status.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_iic_b_attach_handler(uint8_t channel, ra_iic_b_complete_fn_t fn, void* ctx);

/**
 * @brief Dispatch the bus-error IRQ source.
 *
 * @details
 * Test-callable shim that mirrors the ERI handler in FSP's
 * ``r_iic_b_master``: it samples BST, masks the latched error bits
 * back to ``k_ra_iic_b_err_*``, clears them, and fires the registered
 * callback with that mask if it is non-zero.
 *
 * @param[in] channel Channel index.
 *
 * @since 0.1.0
 */
void ra_iic_b_dispatch_eri(uint8_t channel);

/* =============================================================================
 * Legacy NSC-veneer entry points -- thin pass-throughs
 * =============================================================================
 *
 * The Ring 4 NSC veneers (``ra_nsc_iic_init`` etc.) call ``ra_iic_init``
 * / ``ra_iic_write`` / ``ra_iic_read`` directly. We keep those names
 * one-to-one with the IIC_B implementations so the cross-world
 * surface does not have to be re-wired in the same diff.
 */

/** @brief Type alias used by the NSC veneer surface. */
typedef ra_iic_b_cfg_t ra_iic_cfg_t;

/**
 * @brief NSC pass-through: forwards to ``ra_iic_b_init``.
 *
 * @param[in] channel Channel index.
 * @param[in] cfg     Configuration descriptor.
 *
 * @return Forwarded ``ra_err_t``.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_init(uint8_t channel, const ra_iic_cfg_t* cfg);

/**
 * @brief NSC pass-through: forwards to ``ra_iic_b_write``.
 *
 * @param[in] channel   Channel index.
 * @param[in] target_7b 7-bit slave address.
 * @param[in] data      Send buffer.
 * @param[in] len       Byte count.
 *
 * @return Forwarded ``ra_err_t``.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_iic_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len);

/**
 * @brief NSC pass-through: forwards to ``ra_iic_b_read``.
 *
 * @param[in]  channel   Channel index.
 * @param[in]  target_7b 7-bit slave address.
 * @param[out] out       Receive buffer.
 * @param[in]  len       Byte count.
 *
 * @return Forwarded ``ra_err_t``.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_iic_read(uint8_t channel, uint8_t target_7b, uint8_t* out, uint32_t len);

#ifdef __cplusplus
}
#endif
