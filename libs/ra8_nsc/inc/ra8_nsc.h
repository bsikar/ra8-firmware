/**
 * @file ra8_nsc.h
 * @brief Non-Secure Callable veneers -- the only NS->S gateway
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * The NSC layer is the *only* gateway through which Non-Secure code can
 * reach Secure-side resources. Every function declared here is a veneer
 * that:
 *
 * 1. Lives in the ``.gnu.sgstubs`` output section, so the CPU accepts an
 *    NS->S transition at these entry points and nowhere else.
 * 2. Validates every argument against secure-world policy. NS code is
 *    untrusted: an "obviously safe" pointer may aim into the Secure
 *    region, and a length may be ``UINT32_MAX``.
 * 3. Calls the underlying secure-side driver.
 * 4. Returns through the ``cmse_nonsecure_entry`` epilogue, which clears
 *    caller-saved registers so no Secure state rides back out.
 *
 * @par Build modes:
 * ``RA8_NSC_VENEER`` (see ``ra8_nsc_veneer.h``) expands to the real
 * ``cmse_nonsecure_entry`` attribute -- the thing that actually emits the
 * secure gateway -- only in a Secure-world compile, i.e. with both
 * ``RA8_TRUSTZONE_ENABLE`` and ``-mcmse`` in effect. Non-Secure TUs and
 * the host unit tests take the plain-declaration branch and call these as
 * ordinary C functions; the range checks compile to no-ops there, which
 * is correct because those builds have no S/NS boundary to police.
 * ``ra8_nsc_veneer.h`` is the single authority for that macro and
 * ``#undef``s before defining, so include order cannot silently drop the
 * cmse attribute.
 *
 * @par Validation policy:
 * - **Raw pointers do cross, and every one is range-checked before it is
 *   dereferenced.** ``RA8_NSC_CHECK_NS_RANGE_R`` /
 *   ``RA8_NSC_CHECK_NS_RANGE_RW`` wrap
 *   ``cmse_check_address_range`` (the Armv8-M ``TT`` instruction), which
 *   asks the SAU/MPU whether an NS caller could legitimately touch
 *   ``[ptr, ptr+len)``. Ordering is load-bearing: validate a pointer,
 *   then dereference it, then use that value to size the next check.
 *   Reversing those steps turns a veneer into an oracle that reads Secure
 *   memory one word at a time -- see ``ra8_nsc_eth_recv()``.
 * - **Out-of-range lengths are rejected, never clamped.** A silent clamp
 *   would hand the caller a short result it did not ask for; the veneers
 *   return ``k_ra8_err_invalid_arg``.
 * - **NUL-terminated strings are checked against the copy cap, not their
 *   length** -- the length cannot be known without first reading NS
 *   memory. The veneer validates the longest prefix it may touch, then
 *   bounded-copies into secure scratch. See ``ra8_nsc_log_emit()``.
 * - **No Secure-side state leaks.** Read paths copy only the bytes asked
 *   for. Status veneers return packed bit-masks, not addresses.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_nsc_veneer.h"

/* =============================================================================
 * XSPI (external octa-SPI flash) veneers
 * =============================================================================
 */

/**
 * @brief NSC veneer: read ``len`` bytes from external XSPI flash.
 *
 * @details
 * Wraps the secure-side ``ra8_xspi_*`` driver for Non-Secure callers
 * (application). The veneer enforces:
 *
 * - ``flash_off + len`` must fit in the configured flash window.
 * - ``len`` <= ``k_ra8_nsc_xspi_max_read``.
 * - ``ns_dst`` must point into the NS region; this is checked
 * at runtime by reading TT (``cmse_check_address_range``).
 * A rejected range returns ``k_ra8_err_invalid_arg`` before the
 * secure-side XSPI driver can dereference the pointer.
 *
 * @param[in] flash_off Byte offset inside the XSPI flash window.
 * @param[out] ns_dst Destination buffer in Non-Secure RAM.
 * @param[in] len Bytes to copy. Must be > 0.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bytes copied from flash.
 * @retval k_ra8_err_null_ptr ``ns_dst`` was NULL.
 * @retval k_ra8_err_invalid_arg ``len`` zero / range out of bounds.
 * @retval k_ra8_err_timeout Flash COMSTT poll never cleared.
 *
 * @pre ``ns_dst`` is non-NULL and points into the NS data region.
 * @pre ``ra8_xspi_init`` has run on the secure side.
 *
 * @post On success, ``ns_dst[0..len-1]`` contains flash bytes.
 *
 * @par TrustZone Safety:
 * - **Validates:** flash_off + len fits the flash window;
 * ns_dst is within the NS region (cmse_check).
 * - **Trusts:** the secure ra8_xspi state machine.
 * - **Denies:** writes to flash, reads outside the configured
 * window, reads larger than k_ra8_nsc_xspi_max_read.
 *
 * @note Host and single-world builds intentionally compile the range check
 * to a no-op; Secure-world ``-mcmse`` builds execute the TT-based check.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_xspi_read(uint32_t flash_off,
                                                         uint8_t* ns_dst,
                                                         uint32_t len);

/**
 * @brief NSC veneer: query secure-side XSPI status.
 *
 * @param[in] instance XSPI instance number.
 * @param[out] out_mask Status bits; OR of ra8_xspi status values.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Status returned.
 * @retval k_ra8_err_null_ptr ``out_mask`` was NULL.
 * @retval k_ra8_err_invalid_arg Bad instance number.
 *
 * @pre ``out_mask`` non-NULL.
 *
 * @post No secure state is modified; only a 32-bit value crosses
 * the boundary.
 *
 * @par TrustZone Safety:
 * - **Validates:** instance < num_instances; ``out_mask`` is in NS.
 * - **Trusts:** secure-side ra8_xspi_get_status.
 * - **Denies:** any reachability to the actual status register
 * -- the value is copied through the veneer, not aliased.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_xspi_status(uint8_t instance, uint32_t* out_mask);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Frame queued.
 * @retval k_ra8_err_null_ptr ``ns_frame`` NULL.
 * @retval k_ra8_err_invalid_arg Length out of range.
 * @retval k_ra8_err_no_mem TX ring full.
 *
 * @pre ``ns_frame`` non-NULL and points into NS data region.
 * @pre Length <= ``k_ra8_nsc_eth_frame_max``.
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
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_eth_send(const uint8_t* ns_frame, uint16_t len);

/**
 * @brief NSC veneer: pull the next received frame to NS memory.
 *
 * @param[out] ns_buf Destination buffer.
 * @param[in,out] inout_len On entry: capacity. On exit: bytes written.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Bytes copied.
 * @retval k_ra8_err_no_data No frame ready.
 * @retval k_ra8_err_null_ptr ``ns_buf`` / ``inout_len`` NULL.
 * @retval k_ra8_err_invalid_arg Capacity zero / too small.
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
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_eth_recv(uint8_t* ns_buf, uint16_t* inout_len);

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
 * to a small secure-side scratch area, calls ``ra8_log_info``,
 * and returns. Strings longer than the scratch are truncated.
 *
 * @param[in] tag Module tag (short string).
 * @param[in] message Free-form message text.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Log line emitted.
 * @retval k_ra8_err_null_ptr ``tag`` / ``message`` was NULL.
 *
 * @pre ``tag`` and ``message`` are NS pointers.
 *
 * @post Log scratch contains the truncated copy; ITM stim port
 * has been written.
 *
 * @par TrustZone Safety:
 * - **Validates:** both pointers are in NS region; length is
 * capped by the scratch buffer size.
 * - **Trusts:** secure ra8_log driver.
 * - **Denies:** direct ITM stim writes from NS world.
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_log_emit(const char* tag, const char* message);

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
 * - ``ra8_mstp_init`` -- module-stop ref count baseline
 * - ``ra8_pwr_init`` -- LPM + CGC wrapper baseline
 * - ``ra8_isr_init`` -- ICU IELSR allocator
 * - ``ra8_dma_init`` -- DMAC substrate
 *
 * Once this returns success, NS code can call any of the other
 * NSC veneers (``ra8_nsc_eth_*``, ``ra8_nsc_xspi_*``, ``ra8_nsc_log_*``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Substrate up.
 * @retval k_ra8_err_hw_init_failed One of the substrate inits failed.
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
 * (idempotent fast-path returns k_ra8_ok without re-init).
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_periph_init(void);

/* =============================================================================
 * Watchdog veneers (Secure-owned WDT; NS runs the ThreadX supervisor)
 * =============================================================================
 */

/**
 * @brief NSC veneer: arm the Secure WDT with the e-reader configuration.
 *
 * @details
 * The WDT (HUM Ch 27) is Secure-owned; NS cannot programme WDTCR/WDTRR. This
 * veneer forwards to ``ra8_wdt_init`` with a fixed Secure-side configuration
 * (fully-open refresh window, expiry to NMI). In register-start mode that arms
 * the down-counter, so NS must call this immediately before starting the
 * ThreadX supervisor thread that will refresh it.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok WDT configured and armed.
 * @retval k_ra8_err_invalid_arg The fixed configuration was rejected by the driver.
 *
 * @pre Called from NS world after ``ra8_nsc_periph_init`` succeeded.
 * @pre The WDT has not already been armed this boot.
 *
 * @post The WDT down-counter is running and expects a refresh within the timeout.
 * @post WDTCR reflects the fixed Secure configuration.
 *
 * @par TrustZone Safety:
 * - **Validates:** nothing -- parameterless call.
 * - **Trusts:** the Secure clock tree is up (``ra8_nsc_periph_init`` ran).
 * - **Denies:** any NS write to WDTCR/WDTRR (only this veneer arms it).
 * @since 0.1.0
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_wdt_start(void);

/**
 * @brief NSC veneer: refresh the Secure WDT down-counter.
 *
 * @details
 * Forwards to ``ra8_wdt_refresh_deferred`` (WDTRR heartbeat). Installed as the
 * ThreadX supervisor's ``ra8_wdt_sup_refresh_fn_t`` hook via
 * ``ra8_wdt_supervisor_set_refresh_hook`` so the WDT is kicked only when every
 * registered NS thread has checked in within its deadline. Returns ``void`` to
 * match the hook signature.
 *
 * @return Nothing.
 * @note This function does not return a value.
 *
 * @pre ``ra8_nsc_wdt_start`` has armed the WDT.
 * @pre Called from the NS supervisor thread on its refresh cadence.
 *
 * @post The WDT down-counter has been reloaded.
 * @post No Secure state other than the WDT refresh register is touched.
 *
 * @note Not thread-safe; call only from the single NS watchdog supervisor
 *       thread on its refresh cadence, never concurrently.
 *
 * @par TrustZone Safety:
 * - **Validates:** nothing -- parameterless call.
 * - **Trusts:** the NS supervisor's all-threads-alive decision.
 * - **Denies:** any direct NS WDTRR write.
 * @since 0.1.0
 */
RA8_NSC_VENEER void ra8_nsc_wdt_refresh(void);

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
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Digest written.
 * @retval k_ra8_err_null_ptr ``ns_chal`` / ``ns_digest`` NULL.
 * @retval k_ra8_err_invalid_arg ``slot`` >= 8.
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
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_key_vault_challenge(uint16_t       slot,
                                                                   const uint8_t* ns_chal,
                                                                   uint8_t*       ns_digest);

/* The sealed-key-import and TRNG NSC veneers (ra8_nsc_key_import /
 * ra8_nsc_trng_read) were declared here but never defined -- a phantom NS->S
 * entry point that misrepresents the trust-boundary surface. They are removed
 * until a real definition exists (enforced by
 * scripts/checks/check_nsc_veneer_defs.py). The secure-side backing code lives
 * in libs/ra8_secure_app/src/{key_import,secure_trng}.c; re-add each
 * declaration in the
 * same change that adds its RA8_NSC_VENEER definition. */

/* =============================================================================
 * Constants surfaced to NS callers
 * =============================================================================
 */

/**
 * @enum ra8_nsc_limits_t
 * @brief Boundary-policy limits exposed to NS callers.
 *
 * @details
 * These are the hard caps the veneers enforce. NS code can read
 * them directly so it never has to make a call that the secure
 * side will reject.
 */
typedef enum : uint32_t {
  k_ra8_nsc_xspi_max_read   = 4096U, /**< Max bytes per ra8_nsc_xspi_read. */
  k_ra8_nsc_eth_frame_max   = 1518U, /**< Max ethernet frame bytes.        */
  k_ra8_nsc_log_msg_max_len = 128U,  /**< Truncated copy size for logs.    */
} ra8_nsc_limits_t;

/* =============================================================================
 * OTA bank-commit veneers (forwarded to libs/ra8_secure_app/src/ota_commit.c)
 * =============================================================================
 */

/**
 * @brief NSC veneer: commit OTA target bank as the boot bank.
 * @param[in] target_bank Target bank id (k_ra8_ota_bank_a or _b).
 * @return ra8_err_t code.
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_ota_commit(uint8_t target_bank);

/**
 * @brief NSC veneer: write the flash bank-config word.
 * @param[in] raw_value Raw value forwarded to the secure side.
 * @return ra8_err_t code.
 */
[[nodiscard]] RA8_NSC_VENEER ra8_err_t ra8_nsc_flash_bank_config(uint32_t raw_value);

#ifdef __cplusplus
}
#endif
