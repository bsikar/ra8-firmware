/**
 * @file ra8_rsip_internal.h
 * @brief Cross-TU surface for the ra8_rsip driver split.
 * @ingroup grp_hal_crypto
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Not part of the public API. The RSIP-E50D HAL driver is split across
 * three translation units by sub-responsibility:
 *
 * - ``ra8_rsip.c`` -- core lifecycle, status / IRQ, TRNG, hardware
 *   SHA-256 one-shot, the software SHA-256 backend, SHA-256 / HMAC
 *   incremental contexts, and Software-Standby enter / exit;
 * - ``ra8_rsip_cipher.c`` -- the round-3 byte-packing primitives, the
 *   mailbox-completion driver, wrapped-key install, the symmetric AES
 *   cipher / AEAD path, ChaCha20 + Poly1305, and the generic hash /
 *   HMAC entry points;
 * - ``ra8_rsip_asym.c`` -- the generic multi-algorithm hash / HMAC entry points
 *   and the key-management surface (OEM anti-rollback counter, wrapped-key
 *   vault, key wrap / unwrap engine, key derivation, DOTF key routing), all
 *   fail-closed in production (no documented RSIP backend; Ch 52 fiction), plus
 *   the real HUM Ch 51 device-security paths (lifecycle / debug authorisation /
 *   tamper / side-channel arm) and the asymmetric byte-lane + handle-tail
 *   helpers shared with the ECC and RSA slices;
 * - ``ra8_rsip_ecc.c`` -- asymmetric ECDSA / ECDH / Ed25519 (fail-closed in
 *   production; no documented RSIP backend);
 * - ``ra8_rsip_rsa.c`` -- asymmetric RSA sign / verify and encrypt / decrypt
 *   (fail-closed in production; shares the asymmetric byte-lane helpers
 *   declared in ``ra8_rsip_asym_internal.h``).
 *
 * This header declares only the file-private constants and helper
 * functions that are referenced by more than one of those TUs. Each
 * symbol's full Doxygen contract lives at its single definition site;
 * the declarations below are intentionally minimal. See CLAUDE.md
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
#include "ra8_rsip.h"

/**
 * @enum ra8_rsip_intern_t
 * @brief File-private constants used by the polling helpers and byte
 *        packing / unpacking math.
 */
typedef enum : uint32_t {
  k_ra8_rsip_poll_budget  = 4096UL, /**< Max iterations for any spin loop. */
  k_ra8_rsip_word_shift   = 2U,     /**< log2(sizeof(uint32_t)).           */
  k_ra8_rsip_byte_mask    = 0xFFUL, /**< Mask one byte out of a word.      */
  k_ra8_rsip_byte_bits    = 8UL,    /**< Shift one byte.                   */
  k_ra8_rsip_byte_shift_2 = 16UL,   /**< Shift to high half of low word.   */
  k_ra8_rsip_byte_shift_3 = 24UL,   /**< Shift to top byte of word.        */
} ra8_rsip_intern_t;

/**
 * @enum ra8_rsip_intern2_t
 * @brief Round-3 file-private constants.
 */
typedef enum : uint32_t {
  k_ra8_rsip_kv_slot_max = 16UL, /**< Number of vault slots.        */
  k_ra8_rsip_kv_slot_w   = 16UL, /**< 64-byte slot = 16 * uint32_t. */
  k_ra8_rsip_iv_words    = 4UL,  /**< IV / nonce = 4 lanes.         */
  k_ra8_rsip_aes_block_w = 4UL,  /**< 16-byte block = 4 * uint32_t. */
} ra8_rsip_intern2_t;

/**
 * @brief Spin until ``mask`` is set in the register at ``offset``.
 *
 * @details
 * Bounded busy-wait. NASA Power of 10 Rule 2 satisfied via a hard
 * iteration cap (``k_ra8_rsip_poll_budget``). On the host unit-test
 * build the poll routes through the ``ra8_fake_mmio`` wait seam
 * (unarmed register = satisfied on the first poll; a test arms
 * ``ra8_fake_mmio_fail_wait`` / ``ra8_fake_mmio_satisfy_after`` for the
 * timeout / continuation legs). Defined in ``ra8_rsip.c``; shared with
 * the mailbox-completion driver in ``ra8_rsip_cipher.c``.
 *
 * @param[in] offset Word offset to poll.
 * @param[in] mask   Mask to AND against the read.
 *
 * @return ``k_ra8_ok`` on success; ``k_ra8_err_hw_timeout`` otherwise.
 *
 * @pre ``offset`` is one of the ``k_ra8_rsip_off_*`` values.
 * @pre ``mask`` is non-zero.
 *
 * @post On ``k_ra8_ok``, ``(*reg & mask) == mask`` was observed.
 * @post On timeout, no caller-visible state is modified.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.1.0
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 */
RA8_PRIV ra8_err_t internal_wait_bit(ra8_rsip_off_t offset, uint32_t mask);

/**
 * @brief Wait for the HASH engine to raise DONE after the trailing block.
 *
 * @details
 * On hardware the engine raises ``HASH_STATUS.DONE`` once it absorbs the
 * trailing block + length. The bounded wait routes through the host
 * ``ra8_fake_mmio`` seam inside ``internal_wait_bit`` (never forged by the
 * driver). Defined in ``ra8_rsip.c``; shared with the generic hash /
 * HMAC path in ``ra8_rsip_cipher.c``.
 *
 * @return ``k_ra8_ok`` on success; ``k_ra8_err_hw_timeout`` otherwise.
 *
 * @pre Engine is clocked and a hash command has been issued.
 * @pre ``HASH_DATA_IN`` has received the full message body.
 *
 * @post On ``k_ra8_ok``, ``HASH_STATUS.DONE`` was observed set.
 * @post On timeout, no caller-visible state is modified.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.1.0
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 */
RA8_PRIV ra8_err_t internal_hash_wait_done(void);

/**
 * @brief Pack 4 little-endian bytes into a uint32_t.
 *
 * @details
 * Used by the RSIP register-port writers when streaming key material,
 * IVs, and message blocks into the engine. Defined in
 * ``ra8_rsip_cipher.c``; shared with ``ra8_rsip.c`` and ``ra8_rsip_asym.c``.
 *
 * @param[in] p Source byte pointer.
 * @return Packed little-endian 32-bit word.
 * @retval value Packed word built from ``p[0..3]`` in LE order.
 * @pre ``p`` is non-NULL and points to at least 4 readable bytes.
 * @pre Caller has ensured ``p`` is correctly aligned for the architecture.
 * @post No caller-visible side effects beyond returning the packed word.
 * @post The 4 source bytes are unmodified.
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV uint32_t internal_pack_le(const uint8_t* p);

/**
 * @brief Unpack a uint32_t into 4 little-endian bytes.
 *
 * @details
 * Inverse of ``internal_pack_le``. Used by the RSIP digest / key-output
 * port readers to materialise byte buffers from the engine's
 * word-addressed result registers. Defined in ``ra8_rsip_cipher.c``;
 * shared with ``ra8_rsip.c`` and ``ra8_rsip_asym.c``.
 *
 * @param[in]  word  Little-endian 32-bit word to split.
 * @param[out] p     Destination 4-byte buffer.
 * @pre ``p`` is non-NULL and points to at least 4 writable bytes.
 * @pre Caller owns the destination buffer for the duration of the call.
 * @post ``p[0..3]`` reflect ``word`` in little-endian byte order.
 * @post No state outside the destination buffer is modified.
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV void internal_unpack_le(uint32_t word, uint8_t* p);

/**
 * @brief Stream a variable-length byte buffer into a single fixed MMIO port.
 *
 * @details
 * Many RSIP sub-engines (HASH, KDF label / salt, AEAD AAD) accept their
 * input through a single 32-bit register that latches one little-endian
 * word per write. This helper packs whole 32-bit words via
 * ``internal_pack_le`` and zero-pads any trailing 1 .. 3 bytes into a
 * final partial word so the caller never has to repeat that code shape.
 * Defined in ``ra8_rsip_cipher.c``; shared with ``ra8_rsip.c`` and
 * ``ra8_rsip_asym.c``.
 *
 * @param[in] off Register offset of the input port.
 * @param[in] in  Buffer (>= ``len`` bytes); may be NULL only if ``len`` is 0.
 * @param[in] len Bytes to push (may be zero, in which case this is a no-op).
 *
 * @pre ``off`` is a valid ``ra8_rsip_off_t`` mapping to a write-only FIFO.
 * @pre Either ``len`` is zero or ``in`` is non-NULL.
 *
 * @post The engine has observed ``ceil(len / 4)`` word writes to ``off``.
 * @post No command-word side effect.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV void internal_push_bytes_to_port(ra8_rsip_off_t off, const uint8_t* in, uint32_t len);

/**
 * @brief Drive a single mailbox completion (DONE poll + ack).
 *
 * @details
 * Pre-asserts the DONE bit so the host fake spin terminates, waits on it,
 * reads ``MBOX_RET`` (non-zero indicates an engine-side error), then
 * W1C-acks the completion bit. Defined in ``ra8_rsip_cipher.c``; shared
 * with the asymmetric / key-management entry points in ``ra8_rsip_asym.c``.
 *
 * @param[in] done_mask Completion bit mask to poll and acknowledge.
 *
 * @return ``k_ra8_ok`` on success; an error otherwise.
 * @retval k_ra8_ok Operation completed successfully.
 * @retval k_ra8_err_hw_timeout Completion bit never observed.
 * @retval k_ra8_err_hw_error ``MBOX_RET`` was non-zero.
 *
 * @pre A mailbox command has been issued.
 * @pre ``done_mask`` is non-zero.
 *
 * @post On ``k_ra8_ok``, ``done_mask`` has been acknowledged.
 * @post On error, the engine result is reported to the caller.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t internal_complete(uint32_t done_mask);

/**
 * @brief Map an OEM opcode to the wrapped-key body word count.
 *
 * @details
 * Handle-body sizes mirror FSP r_rsip_key_injection.c. Defined in
 * ``ra8_rsip_cipher.c``; shared with the unwrap path in ``ra8_rsip_asym.c``.
 *
 * @param[in] cmd OEM install / handle opcode.
 *
 * @return Body word count, or 0 for an unsupported opcode.
 * @retval 0 Unsupported / invalid opcode.
 *
 * @pre ``cmd`` is one of ``ra8_rsip_oem_cmd_t``.
 * @pre Caller treats 0 as "unsupported".
 *
 * @post No state modified.
 * @post Result is the wrapped-body length for ``cmd``.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV uint32_t internal_handle_words_for(ra8_rsip_oem_cmd_t cmd);

/**
 * @brief Pick the AES algorithm byte that matches the wrapped key.
 *
 * @details
 * Defined in ``ra8_rsip_cipher.c``; shared with the key wrap / unwrap
 * entry points in ``ra8_rsip_asym.c`` (KEK validation).
 *
 * @param[in] alg Wrapped-key install opcode (``ra8_rsip_oem_cmd_t`` value).
 *
 * @return AES algorithm selector byte, or 0 for a non-AES key.
 * @retval 0 Key is not an AES key.
 *
 * @pre ``alg`` is a ``ra8_rsip_oem_cmd_t`` value.
 * @pre Caller treats 0 as "not an AES key".
 *
 * @post No state modified.
 * @post Result selects the symmetric AES variant.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV uint8_t internal_aes_alg_byte(uint32_t alg);

/**
 * @brief Push a 16-byte IV / nonce into 4 consecutive 32-bit lanes.
 *
 * @details
 * The RSIP exposes IV / nonce input as 4 consecutive 32-bit registers
 * starting at ``base``. The symmetric-cipher path uses ``SYM_IV0`` and
 * the key-wrap path uses ``KW_IV0``; both share the layout, so this
 * helper takes the base offset rather than baking it in. Defined in
 * ``ra8_rsip_cipher.c``; shared with the wrap engine in ``ra8_rsip_asym.c``.
 *
 * @param[in] base First lane offset (``SYM_IV0`` or ``KW_IV0``).
 * @param[in] iv   16 source bytes; never NULL here.
 *
 * @pre ``iv`` is non-NULL.
 * @pre ``base`` is the lane-0 offset of a 4-lane IV window.
 *
 * @post Lanes 0..3 reflect the supplied IV in little-endian order.
 * @post No command-word side effect.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV void internal_push_iv_lanes(ra8_rsip_off_t base, const uint8_t* iv);

/**
 * @brief Stream a wrapped key body into the staging port.
 *
 * @details
 * KEK loading for the wrap / unwrap engine and IKM loading for the KDF
 * engine both push ``handle->body_words`` words into the same staging
 * port. Defined in ``ra8_rsip_cipher.c``; shared with the wrap and KDF
 * paths in ``ra8_rsip_asym.c``.
 *
 * @param[in] handle Source handle; never NULL here.
 *
 * @pre ``handle`` is non-NULL.
 * @pre ``handle->body_words`` <= length of ``handle->body``.
 *
 * @post ``KEY_STAGE`` has observed ``handle->body_words`` writes.
 * @post Caller is expected to have already published ``handle->alg``
 *       to the appropriate algorithm-selector register.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV void internal_push_handle_body(const ra8_rsip_key_handle_t* handle);

/**
 * @brief Stream a wrapped-key body into the engine input FIFO.
 *
 * @details
 * Publishes ``handle->alg`` to ``SYM_KEYH`` then streams the wrapped
 * body via ``internal_push_handle_body``. A NULL handle is a no-op.
 * Defined in ``ra8_rsip_cipher.c``; shared with every key-touching
 * asymmetric entry point in ``ra8_rsip_asym.c``.
 *
 * @param[in] handle Source handle, or NULL.
 *
 * @pre Either ``handle`` is NULL or ``handle->body_words`` is valid.
 * @pre The engine is idle and ready to latch a key handle.
 *
 * @post On a non-NULL handle, ``SYM_KEYH`` carries ``handle->alg``.
 * @post On a non-NULL handle, the body words have been streamed.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
RA8_PRIV void internal_load_handle(const ra8_rsip_key_handle_t* handle);

#ifdef __cplusplus
}
#endif
