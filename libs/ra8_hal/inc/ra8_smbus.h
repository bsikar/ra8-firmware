/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_smbus.h
 * @brief SMBus 3.2 protocol layer over an injected I2C bus seam
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the controller-side SMBus 3.2 protocol layer specified in
 * the SMBus 3.2 specification (SBS-IF, December 2018):
 *
 * - section 6.5.1  Quick Command          (not yet wrapped here -- use
 *                                          the bus driver's scan instead)
 * - section 6.5.2  Send Byte
 * - section 6.5.3  Receive Byte
 * - section 6.5.4  Write Byte / Write Word
 * - section 6.5.5  Read Byte / Read Word
 * - section 6.5.7  Block Write
 * - section 6.5.8  Block Read
 * - section 6.5.13 Host Notify / SMBALERT# (alert response address 0x0C)
 * - section 5.4    Packet Error Code (PEC, CRC-8 polynomial 0x07, init 0)
 *
 * SMBus is a strict subset of I2C with extra protocol rules:
 *
 *   1. fixed bus speeds: 10, 100, 400, 1000 kHz
 *   2. mandatory STOP after every transaction (no bus parking)
 *   3. PEC byte may be appended to detect single-bit errors end-to-end
 *   4. block transfers carry a leading byte-count field
 *
 * This module owns the protocol framing (count byte, PEC, address
 * stuffing for ARA) and delegates raw byte movement to the injected
 * I2C bus seam (``ra8_i2c_bus_ops_t``) supplied at init -- IIC_B in
 * I2C-compat mode today, RIIC on a future board revision. The bus
 * drivers are **not** modified -- SMBus is layered on top, and the
 * app owns bus bring-up/teardown (typically binding the seam via
 * ``ra8_io_i2c_bus_as_ops()``).
 *
 * Inclusive terminology: "controller" / "peripheral" replaces the
 * legacy controller/peripheral wording in the SMBus spec (the spec itself uses the older terminology).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra8_smbus_limits_t
 * @brief Spec-mandated buffer ceilings.
 *
 * @details
 * SMBus 3.2 section 6.5.7 / 6.5.8 caps a block payload at 255 bytes
 * (the count byte is uint8_t). The on-the-wire frame is therefore at
 * most ``addr + count + 255 data + PEC`` = 258 bytes. Callers that
 * stack-allocate scratch buffers should size them with this enum.
 */
typedef enum : uint16_t {
  k_ra8_smbus_block_max     = 255U, /**< Max data bytes per block xfer.   */
  k_ra8_smbus_frame_max     = 258U, /**< Max wire bytes incl. PEC + addr. */
  k_ra8_smbus_alert_addr_7b = 0x0CU /**< Alert Response Address (ARA).    */
} ra8_smbus_limits_t;

/**
 * @struct ra8_smbus_cfg_t
 * @brief Configuration descriptor for ``ra8_smbus_init``.
 *
 * @details
 * Carries the injected I2C bus seam plus the SMBus-specific
 * ``pec_enabled`` flag. SMBus 3.2 makes PEC optional but strongly
 * recommended for safety-critical traffic (battery / power ICs).
 */
typedef struct {
  ra8_i2c_bus_ops_t bus;         /**< Injected I2C transfer seam (app-bound). */
  bool              pec_enabled; /**< Append + verify PEC on every xfer.      */
} ra8_smbus_cfg_t;

/**
 * @typedef ra8_smbus_alert_fn_t
 * @brief SMBALERT# notification callback.
 *
 * @param[in] ctx       Caller-supplied context.
 * @param[in] target_7b 7-bit address of the peripheral that asserted
 *                      ALERT, as recovered from the ARA read.
 * @param[in] status    Status byte returned in the ARA payload.
 */
typedef void (*ra8_smbus_alert_fn_t)(void* ctx, uint8_t target_7b, uint8_t status);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the SMBus layer over the injected bus seam.
 *
 * @details
 * Latches ``cfg->bus`` and the policy bits (PEC enable) for later
 * transfers. The bus peripheral behind the seam is initialised by the
 * app before this call -- this layer never brings hardware up or down.
 *
 * @param[in] cfg Configuration descriptor.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              SMBus layer ready.
 * @retval k_ra8_err_null_ptr    ``cfg`` is NULL.
 * @retval k_ra8_err_invalid_arg ``cfg->bus`` seam incomplete (a NULL op).
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre The bus peripheral behind ``cfg->bus`` is initialised.
 * @post On success the layer accepts SMBus transactions.
 *
 * @note Thread safety: not thread-safe.
 *
 * @see ra8_i2c_bus_ops_t
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_smbus_init(const ra8_smbus_cfg_t* cfg);

/**
 * @brief Tear the SMBus layer down and release the driver slot.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok               Layer released.
 * @retval k_ra8_err_not_initialized ``ra8_smbus_init`` never ran.
 *
 * @pre Caller is not in the middle of a transaction.
 * @post The layer rejects transactions until the next init; the app
 *       keeps owning the bus peripheral behind the injected seam.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_smbus_deinit(void);

/* =============================================================================
 * Byte protocols (SMBus 3.2 sec 6.5.2 / 6.5.3 / 6.5.4 / 6.5.5)
 * =============================================================================
 */

/**
 * @brief Send Byte transaction (SMBus 3.2 section 6.5.2).
 *
 * @details
 * Wire format with PEC disabled:
 *   ``S | (addr<<1)|0 | data | P``
 * With PEC enabled:
 *   ``S | (addr<<1)|0 | data | PEC | P``
 *
 * The PEC is computed over the address byte and the data byte using
 * CRC-8 polynomial 0x07, init 0 (SMBus 3.2 section 5.4).
 *
 * @param[in] target_7b 7-bit peripheral address.
 * @param[in] data      Single data byte to transmit.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Byte delivered.
 * @retval k_ra8_err_not_initialized Init not run.
 * @retval Forwarded codes from the bus seam write.
 *
 * @pre ``ra8_smbus_init`` previously succeeded.
 * @post STOP is issued, bus is released.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_smbus_send_byte(uint8_t target_7b, uint8_t data);

/**
 * @brief Receive Byte transaction (SMBus 3.2 section 6.5.3).
 *
 * @details
 * Wire format with PEC disabled:
 *   ``S | (addr<<1)|1 | data | P``
 * With PEC enabled:
 *   ``S | (addr<<1)|1 | data | PEC | P``
 *
 * @param[in]  target_7b 7-bit peripheral address.
 * @param[out] out_data  Destination byte (non-NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok                Byte received (and PEC matched if enabled).
 * @retval k_ra8_err_null_ptr      ``out_data`` is NULL.
 * @retval k_ra8_err_not_initialized Init not run.
 * @retval k_ra8_err_crc_mismatch  PEC verification failed.
 * @retval Forwarded codes from the bus seam read.
 *
 * @pre ``ra8_smbus_init`` previously succeeded.
 * @post STOP is issued, bus is released.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_smbus_receive_byte(uint8_t target_7b, uint8_t* out_data);

/**
 * @brief Write Byte Data: register-indexed byte write (SMBus 3.2 sec 6.5.4).
 *
 * @details
 * Wire format with PEC disabled:
 *   ``S | addr_w | cmd | data | P``
 * With PEC enabled the PEC is appended before the STOP.
 *
 * @param[in] target_7b 7-bit peripheral address.
 * @param[in] cmd       Command / register index byte.
 * @param[in] data      Data byte to write.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Byte written.
 * @retval k_ra8_err_not_initialized Init not run.
 * @retval Forwarded codes from the bus seam write.
 *
 * @pre ``ra8_smbus_init`` previously succeeded.
 * @post STOP is issued.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_smbus_write_byte_data(uint8_t target_7b, uint8_t cmd, uint8_t data);

/**
 * @brief Read Byte Data: register-indexed byte read (SMBus 3.2 sec 6.5.5).
 *
 * @details
 * Wire format with PEC disabled:
 *   ``S | addr_w | cmd | Sr | addr_r | data | P``
 * With PEC enabled:
 *   ``S | addr_w | cmd | Sr | addr_r | data | PEC | P``
 *
 * The PEC is computed over ``addr_w | cmd | addr_r | data``.
 *
 * @param[in]  target_7b 7-bit peripheral address.
 * @param[in]  cmd       Command / register index byte.
 * @param[out] out_data  Destination byte (non-NULL).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok                Byte read (and PEC matched if enabled).
 * @retval k_ra8_err_null_ptr      ``out_data`` NULL.
 * @retval k_ra8_err_not_initialized Init not run.
 * @retval k_ra8_err_crc_mismatch  PEC verification failed.
 * @retval Forwarded codes from the bus seam write / read.
 *
 * @pre ``ra8_smbus_init`` previously succeeded.
 * @post STOP is issued.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_smbus_read_byte_data(uint8_t target_7b, uint8_t cmd, uint8_t* out_data);

/* =============================================================================
 * Block protocols (SMBus 3.2 sec 6.5.7 / 6.5.8)
 * =============================================================================
 */

/**
 * @brief Block Write (SMBus 3.2 section 6.5.7).
 *
 * @details
 * Wire format with PEC disabled:
 *   ``S | addr_w | cmd | count | data[0..count-1] | P``
 * With PEC enabled the PEC byte is inserted before the STOP. The
 * count byte is mandatory and must equal ``len`` (1..255).
 *
 * @param[in] target_7b 7-bit peripheral address.
 * @param[in] cmd       Command / register index byte.
 * @param[in] data      Payload buffer (non-NULL when ``len > 0``).
 * @param[in] len       Number of bytes (1..255).
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok              Block written.
 * @retval k_ra8_err_null_ptr    ``data`` NULL with non-zero ``len``.
 * @retval k_ra8_err_invalid_arg ``len`` is 0 or > 255.
 * @retval k_ra8_err_not_initialized Init not run.
 * @retval Forwarded codes from the bus seam write.
 *
 * @pre ``ra8_smbus_init`` previously succeeded.
 * @post STOP is issued.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_smbus_block_write(uint8_t target_7b, uint8_t cmd, const uint8_t* data, uint8_t len);

/**
 * @brief Block Read (SMBus 3.2 section 6.5.8).
 *
 * @details
 * Wire format with PEC disabled:
 *   ``S | addr_w | cmd | Sr | addr_r | count | data[0..count-1] | P``
 * With PEC enabled the PEC trails the data run.
 *
 * @param[in]  target_7b 7-bit peripheral address.
 * @param[in]  cmd       Command / register index byte.
 * @param[out] buf       Destination buffer (non-NULL, capacity ``cap``).
 * @param[in]  cap       Capacity of ``buf`` (1..255).
 * @param[out] out_len   Bytes actually returned by the peripheral
 *                       (the count field). Always populated on
 *                       ``k_ra8_ok`` and on ``k_ra8_err_invalid_size``.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok                Block received and PEC matched if on.
 * @retval k_ra8_err_null_ptr      ``buf`` / ``out_len`` NULL.
 * @retval k_ra8_err_invalid_arg   ``cap`` is 0 or > 255.
 * @retval k_ra8_err_invalid_size  Returned count exceeds ``cap``.
 * @retval k_ra8_err_not_initialized Init not run.
 * @retval k_ra8_err_crc_mismatch  PEC verification failed.
 * @retval Forwarded codes from the bus seam write / read.
 *
 * @pre ``ra8_smbus_init`` previously succeeded.
 * @post STOP is issued regardless of outcome.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_smbus_block_read(uint8_t target_7b, uint8_t cmd, uint8_t* buf, uint8_t cap, uint8_t* out_len);

/* =============================================================================
 * SMBALERT# / Host Notify (SMBus 3.2 sec 6.5.13)
 * =============================================================================
 */

/**
 * @brief Register a callback fired when ``ra8_smbus_alert_dispatch``
 *        successfully reads the Alert Response Address.
 *
 * @details
 * SMBus 3.2 section 6.5.13 specifies that a peripheral asserts
 * SMBALERT# (an open-drain side-band line, *not* SDA/SCL) and the
 * controller responds by reading from address 0x0C (ARA). The
 * peripheral that won arbitration writes its 7-bit address back into
 * the LSBs of the response byte. Because SMBALERT# is a board-level
 * GPIO interrupt, this driver does not own the IRQ wiring -- the
 * caller arms ``ra8_icu`` to call ``ra8_smbus_alert_dispatch()`` from
 * the SMBALERT# ISR, and the dispatch helper performs the ARA read
 * and fires the registered callback.
 *
 * @param[in] fn  Callback to fire, or NULL to detach.
 * @param[in] ctx Context pointer passed back unchanged.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok               Callback registered or detached.
 * @retval k_ra8_err_not_initialized Init not run.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_smbus_alert_register_callback(ra8_smbus_alert_fn_t fn, void* ctx);

/**
 * @brief Dispatch a single SMBALERT# event: read the ARA and fire the
 *        registered callback.
 *
 * @details
 * Reads one byte from address ``k_ra8_smbus_alert_addr_7b`` (0x0C). The
 * returned byte's upper 7 bits are the 7-bit address of the peripheral
 * that asserted ALERT (SMBus 3.2 section 6.5.13). The LSB is the
 * device status. If no callback has been registered the read still
 * happens (so the bus line is released) but no further action is
 * taken.
 *
 * Test-callable: unit tests invoke this directly to simulate an ALERT
 * event.
 *
 * @return ``ra8_err_t``.
 * @retval k_ra8_ok                Dispatch completed (callback fired
 *                                if registered).
 * @retval k_ra8_err_not_initialized Init not run.
 * @retval Forwarded codes from the bus seam read.
 *
 * @pre ``ra8_smbus_init`` previously succeeded.
 * @post Bus is released.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_smbus_alert_dispatch(void);

/* =============================================================================
 * PEC helpers (exposed for unit tests)
 * =============================================================================
 */

/**
 * @brief Compute CRC-8/SMBus over a buffer.
 *
 * @details
 * Polynomial 0x07, initial value 0x00, no reflection, no XOR-out
 * (SMBus 3.2 section 5.4). Provided as a public symbol so tests can
 * cross-check the framing; production code should not need to call
 * this directly.
 *
 * @param[in] data Pointer to bytes (non-NULL when ``len > 0``).
 * @param[in] len  Byte count.
 *
 * @return Computed CRC-8 value.
 * @retval 0x00 Either ``len == 0`` or the input bytes happen to hash to zero.
 * @retval other Computed CRC-8 over ``data[0..len-1]`` per SMBus 3.2 section 5.4.
 *
 * @pre ``data != NULL`` when ``len > 0``.
 * @pre ``len`` reflects the true buffer size (no aliasing past ``len``).
 * @post Function is pure -- no firmware state mutated.
 * @post Result depends only on ``data`` and ``len``.
 *
 * @note Thread-safe: pure function with no shared state.
 * @since 0.1.0
 */
uint8_t ra8_smbus_pec(const uint8_t* data, uint32_t len);

#ifdef __cplusplus
}
#endif
