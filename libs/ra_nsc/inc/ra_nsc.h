/**
 * @file ra_nsc.h
 * @brief Non-Secure Callable veneer scaffold
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * Wave 7.3 scaffold for the TrustZone Non-Secure Callable layer.
 * The NSC layer is the *only* gateway through which Non-Secure
 * code can reach Secure-side resources after Wave 9 partitions
 * the firmware. Each function in this header is a veneer that:
 *
 *  1. Lives in a special ``.gnu.sgstubs`` linker section so the
 *     CPU can transition from NS to S only at these entry points.
 *  2. Validates every argument against the secure-world's policy
 *     (NS code is untrusted; even an "obviously safe" pointer
 *     could fall in the secure region).
 *  3. Calls the underlying Ring-3 driver in the secure world.
 *  4. Returns through the matching ``__cmse_nonsecure_entry``
 *     epilogue which sanitises caller-saved registers.
 *
 * **Wave 7.3 status:** every veneer here is a plain C function
 * with the right shape and policy logic, but **no** ``cmse``
 * annotation. The single-world build runs through them as
 * ordinary calls. Wave 9.2 adds:
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
 *   - **No raw pointers cross the boundary.** All buffer veneers
 *     take ``uintptr_t`` style addresses but treat them as
 *     opaque indices into well-known windows; the secure side
 *     resolves the actual pointer.
 *   - **Length bounds are enforced by the secure side.** The NS
 *     caller may try to pass ``UINT32_MAX``; the veneer clamps
 *     and rejects.
 *   - **No secure-side state leaks.** Read paths copy only the
 *     bytes asked for. Status veneers return packed bit-masks,
 *     not addresses or pointers.
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
 * (Wave 9 application). The veneer enforces:
 *
 *   - ``flash_off + len`` must fit in the configured flash window.
 *   - ``len`` <= ``k_ra_nsc_xspi_max_read``.
 *   - ``ns_dst`` must point into the NS region; this is checked
 *     at runtime in Wave 9 by reading TT (``cmse_check_address_range``).
 *     In Wave 7.3 the check is a stub that always passes -- the
 *     callsite is documented for the Wave 9.2 retrofit.
 *
 * @param[in]  flash_off Byte offset inside the XSPI flash window.
 * @param[out] ns_dst    Destination buffer in Non-Secure RAM.
 * @param[in]  len       Bytes to copy. Must be > 0.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Bytes copied.
 * @retval k_ra_err_null_ptr     ``ns_dst`` was NULL.
 * @retval k_ra_err_invalid_arg  ``len`` zero / range out of bounds.
 * @retval k_ra_err_not_supported Underlying ra_xspi flash-read API
 *                                still pending Wave 5.1c.
 *
 * @pre ``ns_dst`` is non-NULL and points into the NS data region.
 * @pre ``ra_xspi_init`` has run on the secure side.
 *
 * @post On success, ``ns_dst[0..len-1]`` contains flash bytes.
 *
 * @par TrustZone Safety:
 *  - **Validates:** flash_off + len fits the flash window;
 *    ns_dst is within the NS region (Wave 9.2 cmse_check).
 *  - **Trusts:** the secure ra_xspi state machine.
 *  - **Denies:** writes to flash, reads outside the configured
 *    window, reads larger than k_ra_nsc_xspi_max_read.
 *
 * @note Wave 7.3 stub. The Wave 9.2 retrofit just adds the
 *       ``__attribute__((cmse_nonsecure_entry))`` and the
 *       ``cmse_check_address_range`` call site.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_xspi_read(uint32_t flash_off,
                                                      uint8_t* ns_dst,
                                                      uint32_t len);

/**
 * @brief NSC veneer: query secure-side XSPI status.
 *
 * @param[in]  instance XSPI instance number.
 * @param[out] out_mask Status bits; OR of ra_xspi status values.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok               Status returned.
 * @retval k_ra_err_null_ptr     ``out_mask`` was NULL.
 * @retval k_ra_err_invalid_arg  Bad instance number.
 *
 * @pre ``out_mask`` non-NULL.
 *
 * @post No secure state is modified; only a 32-bit value crosses
 *       the boundary.
 *
 * @par TrustZone Safety:
 *  - **Validates:** instance < num_instances; ``out_mask`` is in NS.
 *  - **Trusts:** secure-side ra_xspi_get_status.
 *  - **Denies:** any reachability to the actual status register
 *    -- the value is copied through the veneer, not aliased.
 * @since 0.3.0
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
 * @param[in] len      Frame length in bytes.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Frame queued.
 * @retval k_ra_err_null_ptr       ``ns_frame`` NULL.
 * @retval k_ra_err_invalid_arg    Length out of range.
 * @retval k_ra_err_not_supported  ra_eth TX path still in scaffold.
 *
 * @pre ``ns_frame`` non-NULL and points into NS data region.
 * @pre Length <= ``k_ra_nsc_eth_frame_max``.
 *
 * @post On success, the frame is owned by the secure TX descriptor
 *       ring; the NS caller may free / overwrite ``ns_frame``.
 *
 * @par TrustZone Safety:
 *  - **Validates:** length cap; ns_frame is in NS region (Wave 9.2).
 *  - **Trusts:** ESWM driver descriptor management.
 *  - **Denies:** raw pointer pass-through -- the secure side copies
 *    bytes into its own descriptor before the IRQ context fires.
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_eth_send(const uint8_t* ns_frame, uint16_t len);

/**
 * @brief NSC veneer: pull the next received frame to NS memory.
 *
 * @param[out]    ns_buf    Destination buffer.
 * @param[in,out] inout_len On entry: capacity. On exit: bytes written.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Bytes copied.
 * @retval k_ra_err_no_data        No frame ready.
 * @retval k_ra_err_null_ptr       ``ns_buf`` / ``inout_len`` NULL.
 * @retval k_ra_err_invalid_arg    Capacity zero / too small.
 * @retval k_ra_err_not_supported  ra_eth RX path still in scaffold.
 *
 * @pre ``ns_buf`` and ``inout_len`` non-NULL.
 *
 * @post On success, ``*inout_len`` holds the actual byte count.
 *
 * @par TrustZone Safety:
 *  - **Validates:** capacity bound; both pointers in NS region.
 *  - **Trusts:** ESWM RX descriptor management.
 *  - **Denies:** secure-side scratch leakage -- bytes are copied,
 *    not aliased.
 * @since 0.3.0
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
 * @param[in] tag     Module tag (short string).
 * @param[in] message Free-form message text.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok           Log line emitted.
 * @retval k_ra_err_null_ptr ``tag`` / ``message`` was NULL.
 *
 * @pre ``tag`` and ``message`` are NS pointers.
 *
 * @post Log scratch contains the truncated copy; ITM stim port
 *       has been written.
 *
 * @par TrustZone Safety:
 *  - **Validates:** both pointers are in NS region; length is
 *    capped by the scratch buffer size.
 *  - **Trusts:** secure ra_log driver.
 *  - **Denies:** direct ITM stim writes from NS world.
 * @since 0.3.0
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
 *   - ``ra_mstp_init``      -- module-stop ref count baseline
 *   - ``ra_pwr_init``       -- LPM + CGC wrapper baseline
 *   - ``ra_isr_init``       -- ICU IELSR allocator
 *   - ``ra_dma_init``       -- DMAC substrate
 *
 * Once this returns success, NS code can call any of the other
 * NSC veneers (``ra_nsc_eth_*``, ``ra_nsc_xspi_*``, ``ra_nsc_log_*``).
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok                 Substrate up.
 * @retval k_ra_err_hw_init_failed One of the substrate inits failed.
 *
 * @pre Called from NS world after vector_table init.
 * @pre IRQs masked or single-threaded boot context.
 *
 * @post All Ring-3 substrate modules are initialised on the
 *       secure side.
 *
 * @par TrustZone Safety:
 *  - **Validates:** nothing -- this is a parameterless call.
 *  - **Trusts:** the boot ROM has already configured the SAU.
 *  - **Denies:** repeat invocation past the first success
 *    (idempotent fast-path returns k_ra_ok without re-init).
 * @since 0.3.0
 */
[[nodiscard]] RA_NSC_VENEER ra_err_t ra_nsc_periph_init(void);

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
  k_ra_nsc_xspi_max_read   = 4096U, /**< Max bytes per ra_nsc_xspi_read.  */
  k_ra_nsc_eth_frame_max   = 1518U, /**< Max ethernet frame bytes.        */
  k_ra_nsc_log_msg_max_len = 128U,  /**< Truncated copy size for logs.    */
} ra_nsc_limits_t;

#ifdef __cplusplus
}
#endif
