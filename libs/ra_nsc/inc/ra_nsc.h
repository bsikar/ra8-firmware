/**
 * @file ra_nsc.h
 * @brief Non-Secure Callable veneer scaffold
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * scaffold for the TrustZone Non-Secure Callable layer.
 * The NSC layer is the *only* gateway through which Non-Secure
 * code can reach Secure-side resources partitions
 * the firmware. Each function in this header is a veneer that:
 *
 * 1. Lives in a special ``.gnu.sgstubs`` linker section so the
 * CPU can transition from NS to S only at these entry points.
 * 2. Validates every argument against the secure-world's policy
 * (NS code is untrusted; even an "obviously safe" pointer
 * could fall in the secure region).
 * 3. Calls the underlying Ring-3 driver in the secure world.
 * 4. Returns through the matching ``__cmse_nonsecure_entry``
 * epilogue which sanitises caller-saved registers.
 *
 * ** status:** every veneer here is a plain C function
 * with the right shape and policy logic, but **no** ``cmse``
 * annotation. The single-world build runs through them as
 * ordinary calls. adds:
 *
 * @code
 * __attribute__((cmse_nonsecure_entry))
 * @endcode
 *
 * to each prototype + definition and routes them through the
 * SAU + IDAU. The argument validation already in place is
 * exactly what the partition needs, so nothing in this header
 * has to change shape to go live.
 *
 * @par TrustZone Safety:
 * Each veneer documents what it validates, what state it touches,
 * and what assumptions hold. The validation policy follows three
 * rules:
 *
 * - **No raw pointers cross the boundary.** All buffer veneers
 * take ``uintptr_t`` style addresses but treat them as
 * opaque indices into well-known windows; the secure side
 * resolves the actual pointer.
 * - **Length bounds are enforced by the secure side.** The NS
 * caller may try to pass ``UINT32_MAX``; the veneer clamps
 * and rejects.
 * - **No secure-side state leaks.** Read paths copy only the
 * bytes asked for. Status veneers return packed bit-masks,
 * not addresses or pointers.
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
#include "ra_nsc_veneer.h"

/* =============================================================================
 * XSPI (external octa-SPI flash) veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: read ``len`` bytes from external XSPI flash.
 *
 * @details
 * Wraps the secure-side ``ra_xspi_*`` driver for Non-Secure callers
 * (application). The veneer enforces:
 *
 * - ``flash_off + len`` must fit in the configured flash window.
 * - ``len`` <= ``k_ra_nsc_xspi_max_read``.
 * - ``ns_dst`` must point into the NS region; this is checked
 * at runtime by reading TT (``cmse_check_address_range``).
 * In the check is a stub that always passes -- the
 * callsite is documented for the retrofit.
 *
 * @param[in] flash_off Byte offset inside the XSPI flash window.
 * @param[out] ns_dst Destination buffer in Non-Secure RAM.
 * @param[in] len Bytes to copy. Must be > 0.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bytes copied from flash.
 * @retval k_ra_err_null_ptr ``ns_dst`` was NULL.
 * @retval k_ra_err_invalid_arg ``len`` zero / range out of bounds.
 * @retval k_ra_err_timeout Flash COMSTT poll never cleared.
 *
 * @pre ``ns_dst`` is non-NULL and points into the NS data region.
 * @pre ``ra_xspi_init`` has run on the secure side.
 *
 * @post On success, ``ns_dst[0..len-1]`` contains flash bytes.
 *
 * @par TrustZone Safety:
 * - **Validates:** flash_off + len fits the flash window;
 * ns_dst is within the NS region (cmse_check).
 * - **Trusts:** the secure ra_xspi state machine.
 * - **Denies:** writes to flash, reads outside the configured
 * window, reads larger than k_ra_nsc_xspi_max_read.
 *
 * @note stub. The retrofit just adds the
 * ``__attribute__((cmse_nonsecure_entry))`` and the
 * ``cmse_check_address_range`` call site.
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_xspi_read(uint32_t flash_off,
                                                      uint8_t* ns_dst,
                                                      uint32_t len);

/**
 * @brief NSC veneer: query secure-side XSPI status.
 *
 * @param[in] instance XSPI instance number.
 * @param[out] out_mask Status bits; OR of ra_xspi status values.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Status returned.
 * @retval k_ra_err_null_ptr ``out_mask`` was NULL.
 * @retval k_ra_err_invalid_arg Bad instance number.
 *
 * @pre ``out_mask`` non-NULL.
 *
 * @post No secure state is modified; only a 32-bit value crosses
 * the boundary.
 *
 * @par TrustZone Safety:
 * - **Validates:** instance < num_instances; ``out_mask`` is in NS.
 * - **Trusts:** secure-side ra_xspi_get_status.
 * - **Denies:** any reachability to the actual status register
 * -- the value is copied through the veneer, not aliased.
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_xspi_status(uint8_t instance, uint32_t* out_mask);

/* =============================================================================
 * Ethernet veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: hand a frame to the secure ESWM transmit path.
 *
 * @param[in] ns_frame Pointer to a complete ethernet frame in NS RAM.
 * @param[in] len Frame length in bytes.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Frame queued.
 * @retval k_ra_err_null_ptr ``ns_frame`` NULL.
 * @retval k_ra_err_invalid_arg Length out of range.
 * @retval k_ra_err_no_mem TX ring full.
 *
 * @pre ``ns_frame`` non-NULL and points into NS data region.
 * @pre Length <= ``k_ra_nsc_eth_frame_max``.
 *
 * @post On success, the frame is owned by the secure TX descriptor
 * ring; the NS caller may free / overwrite ``ns_frame``.
 *
 * @par TrustZone Safety:
 * - **Validates:** length cap; ns_frame is in NS region.
 * - **Trusts:** ESWM driver descriptor management.
 * - **Denies:** raw pointer pass-through -- the secure side copies
 * bytes into its own descriptor before the IRQ context fires.
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_eth_send(const uint8_t* ns_frame, uint16_t len);

/**
 * @brief NSC veneer: pull the next received frame to NS memory.
 *
 * @param[out] ns_buf Destination buffer.
 * @param[in,out] inout_len On entry: capacity. On exit: bytes written.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Bytes copied.
 * @retval k_ra_err_no_data No frame ready.
 * @retval k_ra_err_null_ptr ``ns_buf`` / ``inout_len`` NULL.
 * @retval k_ra_err_invalid_arg Capacity zero / too small.
 *
 * @pre ``ns_buf`` and ``inout_len`` non-NULL.
 *
 * @post On success, ``*inout_len`` holds the actual byte count.
 *
 * @par TrustZone Safety:
 * - **Validates:** capacity bound; both pointers in NS region.
 * - **Trusts:** ESWM RX descriptor management.
 * - **Denies:** secure-side scratch leakage -- bytes are copied,
 * not aliased.
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_eth_recv(uint8_t* ns_buf, uint16_t* inout_len);

/* =============================================================================
 * Logging veneer
 * =============================================================================
 */

/**
 * @brief NSC veneer: emit a log line via the secure ITM channel.
 *
 * @details
 * NS code cannot reach the ITM stim ports directly because they
 * live in the System control space (0xE0000000) which lives in
 * the secure region. The veneer copies the (tag, message) pair
 * to a small secure-side scratch area, calls ``ra_log_info``,
 * and returns. Strings longer than the scratch are truncated.
 *
 * @param[in] tag Module tag (short string).
 * @param[in] message Free-form message text.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Log line emitted.
 * @retval k_ra_err_null_ptr ``tag`` / ``message`` was NULL.
 *
 * @pre ``tag`` and ``message`` are NS pointers.
 *
 * @post Log scratch contains the truncated copy; ITM stim port
 * has been written.
 *
 * @par TrustZone Safety:
 * - **Validates:** both pointers are in NS region; length is
 * capped by the scratch buffer size.
 * - **Trusts:** secure ra_log driver.
 * - **Denies:** direct ITM stim writes from NS world.
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_log_emit(const char* tag, const char* message);

/* =============================================================================
 * Peripheral init veneer (boot-time only)
 * =============================================================================
 */

/**
 * @brief NSC veneer: bring up the secure-side peripheral substrate.
 *
 * @details
 * Called once by NS code at boot. The veneer kicks off the
 * substrate dance that NS code cannot do directly:
 *
 * - ``ra_mstp_init`` -- module-stop ref count baseline
 * - ``ra_pwr_init`` -- LPM + CGC wrapper baseline
 * - ``ra_isr_init`` -- ICU IELSR allocator
 * - ``ra_dma_init`` -- DMAC substrate
 *
 * Once this returns success, NS code can call any of the other
 * NSC veneers (``ra_nsc_eth_*``, ``ra_nsc_xspi_*``, ``ra_nsc_log_*``).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Substrate up.
 * @retval k_ra_err_hw_init_failed One of the substrate inits failed.
 *
 * @pre Called from NS world after vector_table init.
 * @pre IRQs masked or single-threaded boot context.
 *
 * @post All Ring-3 substrate modules are initialized on the
 * secure side.
 *
 * @par TrustZone Safety:
 * - **Validates:** nothing -- this is a parameterless call.
 * - **Trusts:** the boot ROM has already configured the SAU.
 * - **Denies:** repeat invocation past the first success
 * (idempotent fast-path returns k_ra_ok without re-init).
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_periph_init(void);

/* =============================================================================
 * Key vault veneer
 * =============================================================================
 */

/**
 * @brief NSC veneer: SHA-256(key XOR challenge) for a stored slot.
 *
 * @details
 * The only operation the Non-Secure world can perform on the
 * secure key vault. The raw key never leaves the secure world;
 * this veneer copies the challenge into secure scratch, XORs it
 * with the slot key, hashes the result, and copies the 32-byte
 * digest back to NS memory. NS callers can then verify the
 * digest matches their expected value (e.g., for HMAC or PBKDF2
 * style derivation) without ever seeing the key.
 *
 * @param[in] slot Vault slot index 0..7.
 * @param[in] ns_chal 32-byte challenge in NS memory.
 * @param[out] ns_digest 32-byte digest buffer in NS memory.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Digest written.
 * @retval k_ra_err_null_ptr ``ns_chal`` / ``ns_digest`` NULL.
 * @retval k_ra_err_invalid_arg ``slot`` >= 8.
 *
 * @pre PAL has been initialized; the secure key vault has been
 * programmed with at least one key in the requested slot.
 *
 * @post ``ns_digest[0..31]`` holds SHA-256(key XOR challenge).
 *
 * @par TrustZone Safety:
 * - **Validates:** slot in range; both NS pointers in NS region
 * (cmse_check); buffer lengths fit 32-byte windows.
 * - **Trusts:** the secure key vault's static slot array.
 * - **Denies:** raw key access from NS. Only the digest crosses.
 *
 * @note Thread safety: not thread-safe; the SHA-256 sponge is
 * single-instance.
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_key_vault_challenge(uint16_t       slot,
                                                                const uint8_t* ns_chal,
                                                                uint8_t*       ns_digest);

/* =============================================================================
 * Sealed-key import veneer
 * =============================================================================
 */

/**
 * @brief NSC veneer: import a sealed key blob and vend an opaque handle.
 *
 * @details
 * Validates ``ns_blob`` and ``ns_handle`` against the NS region,
 * copies the blob into a small secure scratch buffer, then forwards
 * to ``ra_key_import_seal`` on the secure side. On success the
 * 32-bit opaque handle is written back to NS memory; the slot
 * index never crosses the boundary.
 *
 * @param[in]  ns_blob   Pointer to a sealed blob in NS RAM.
 * @param[in]  blob_len  Must equal ``k_ra_nsc_key_import_blob_bytes``.
 * @param[out] ns_handle Destination for the opaque handle.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                Handle written.
 * @retval k_ra_err_null_ptr      Either pointer was NULL.
 * @retval k_ra_err_invalid_size  ``blob_len`` mismatched.
 * @retval k_ra_err_invalid_arg   MAC verification failed.
 * @retval k_ra_err_no_mem        No free vault slots.
 *
 * @pre Both pointers in NS region.
 * @pre ``ra_key_import_reset`` ran during secure boot.
 *
 * @post On success ``*ns_handle`` is non-zero.
 *
 * @par TrustZone Safety:
 * - **Validates:** blob_len equals the published constant; both
 *   pointers in NS region.
 * - **Trusts:** ``ra_key_import_seal`` and the underlying vault.
 * - **Denies:** raw key access from NS; pointer pass-through.
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_key_import(const uint8_t* ns_blob,
                                                       uint32_t       blob_len,
                                                       uint32_t*      ns_handle);

/* =============================================================================
 * TRNG veneer
 * =============================================================================
 */

/**
 * @brief NSC veneer: pull TRNG-quality entropy into an NS buffer.
 *
 * @details
 * Forwards to ``ra_secure_trng_read`` after copying the result
 * through a secure-side scratch buffer. NS pointers are never
 * dereferenced from secure code.
 *
 * @param[out] ns_dst Destination buffer in NS RAM.
 * @param[in]  len    Number of entropy bytes, ``1..k_ra_nsc_trng_max_bytes``.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Bytes written.
 * @retval k_ra_err_null_ptr     ``ns_dst`` was NULL.
 * @retval k_ra_err_invalid_arg  ``len`` zero or above the cap.
 *
 * @pre ``ns_dst`` non-NULL and points into NS region.
 *
 * @post ``ns_dst[0..len-1]`` contains entropy.
 *
 * @par TrustZone Safety:
 * - **Validates:** len cap; ns_dst in NS region.
 * - **Trusts:** secure-side TRNG core.
 * - **Denies:** direct RSIP register access from NS.
 * @since 0.1.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_trng_read(uint8_t* ns_dst, uint32_t len);

/* =============================================================================
 * Constants surfaced to NS callers
 * =============================================================================
 */

/**
 * @enum ra_nsc_limits_t
 * @brief Boundary-policy limits exposed to NS callers.
 *
 * @details
 * These are the hard caps the veneers enforce. NS code can read
 * them directly so it never has to make a call that the secure
 * side will reject.
 */
typedef enum : uint32_t {
  k_ra_nsc_xspi_max_read         = 4096U, /**< Max bytes per ra_nsc_xspi_read. */
  k_ra_nsc_eth_frame_max         = 1518U, /**< Max ethernet frame bytes. */
  k_ra_nsc_log_msg_max_len       = 128U,  /**< Truncated copy size for logs. */
  k_ra_nsc_key_import_blob_bytes = 36U,   /**< Sealed blob: 32-byte key + 4-byte MAC. */
  k_ra_nsc_trng_max_bytes        = 256U,  /**< Max bytes per ra_nsc_trng_read. */
} ra_nsc_limits_t;

/* =============================================================================
 * OTA bank-commit veneers (forwarded to src/secure_app/ota_commit.c)
 * =============================================================================
 */

/**
 * @brief NSC veneer: commit OTA target bank as the boot bank.
 * @param[in] target_bank Target bank id (k_ra_ota_bank_a or _b).
 * @return ra_err_t code.
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_ota_commit(uint8_t target_bank);

/**
 * @brief NSC veneer: write the flash bank-config word.
 * @param[in] raw_value Raw value forwarded to the secure side.
 * @return ra_err_t code.
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_flash_bank_config(uint32_t raw_value);

#ifdef __cplusplus
}
#endif
