/**
 * @file ra8_dotf.h
 * @brief Decryption On The Fly (DOTF) HAL driver public API
 * @ingroup grp_hal_crypto
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Driver for the RA8D2 DOTF block (HUM Ch 45 p 3048..3050). DOTF
 * transparently decrypts read traffic on the AXI side of the OSPI /
 * xSPI controller using an AES core configured in CTR mode, allowing
 * encrypted code stored in external flash to execute in place from
 * the XiP window.
 *
 * The driver covers every register documented in HUM Ch 45 plus the
 * REG00 sub-fields cross-referenced from the FSP ``r_ospi_b`` driver
 * (key size, AES mode, side-channel countermeasure, self-test
 * trigger, IV staging via REG03). It also implements:
 *
 *  - Multi-region staging per channel: software keeps up to
 *    ``k_ra8_dotf_max_regions`` pre-validated regions, only one of
 *    which can be live at a time per HUM 45.3 ("one contiguous
 *    area"); the active region is selected via
 *    ``ra8_dotf_select_region`` for live key-region rotation.
 *  - Live key rotation: ``ra8_dotf_rotate_key`` quiesces the channel,
 *    re-stages the wrapped key handle from ra8_rsip, and re-arms the
 *    AES core in a single bounded sequence.
 *  - RSIP-key-injection wiring: ``ra8_dotf_install_key`` consumes a
 *    ``ra8_dotf_key_handle_t`` (an opaque cookie produced by
 *    ``ra8_rsip``'s key-injection layer) and stages the wrapped-key
 *    bytes into REG03 in big-endian order. The HAL never touches
 *    raw key material -- it only forwards the handle to RSIP.
 *  - Region overlap validation: ``ra8_dotf_set_region`` rejects any
 *    region that overlaps another channel's currently-armed region
 *    (HUM Ch 5 + Ch 45.3 implicitly require disjoint conversion
 *    areas because both channels share the same AXI bus and a
 *    single AES key cannot decrypt both halves correctly).
 *  - Both OSPI-channel pairings: DOTF0+XSPI0 (window
 *    ``0x8000_0000..0x9FFF_FFFF``) and DOTF1+XSPI1 (window
 *    ``0x7000_0000..0x7FFF_FFFF``) are first-class peers; every
 *    public API takes a ``channel`` argument and validates it.
 *  - Side-channel countermeasure tuning: REG00 bits 16/17 are
 *    exposed via ``ra8_dotf_set_sca_level``.
 *  - Self-test trigger: REG00 bit 20 is exposed via
 *    ``ra8_dotf_run_self_test``; the result is observable through
 *    ``ra8_dotf_get_status``.
 *  - Lifecycle (init / deinit / enter_stop / exit_stop), per-channel
 *    enable / disable, dispatch glue.
 *
 * @warning Misconfiguration of DOTF when executing from the encrypted
 *          XSPI window will cause the CPU to fetch garbage and almost
 *          certainly take a HardFault. The driver MUST be initialized
 *          and the conversion region MUST be programmed before
 *          control jumps into XiP code that lives in the encrypted
 *          area. The intended call order on a cold boot is:
 *          ``ra8_dotf_init()`` -> ``ra8_dotf_install_key(handle)`` ->
 *          ``ra8_dotf_set_iv(iv)`` -> ``ra8_dotf_set_region(region)``
 *          -> ``ra8_dotf_enable()`` -> jump into XiP code.
 *
 * @warning DOTF0 and DOTF1 share their MSTP bits with XSPI0 / XSPI1
 *          respectively (HUM Ch 11.2.7 MSTPCRB bits 16/17 cover the
 *          OSPI+DOTF pair). Tearing down DOTF therefore also tears
 *          down the matching xSPI controller -- callers running XiP
 *          must keep DOTF up for the lifetime of the XiP region.
 *
 * @par State Machine
 *
 * @dot
 * digraph ra8_dotf_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   RESET [label="RESET"];
 *   IDLE [label="IDLE"];
 *   KEY_STAGED [label="KEY_STAGED"];
 *   IV_STAGED [label="IV_STAGED"];
 *   REGION_SET [label="REGION_SET"];
 *   ARMED [label="ARMED"];
 *
 *   __start -> RESET;
 *   RESET -> IDLE [label="ra8_dotf_init"];
 *   IDLE -> KEY_STAGED [label="ra8_dotf_install_key"];
 *   KEY_STAGED -> IV_STAGED [label="ra8_dotf_set_iv"];
 *   IV_STAGED -> REGION_SET [label="ra8_dotf_set_region /\nselect_region"];
 *   REGION_SET -> ARMED [label="ra8_dotf_enable"];
 *   ARMED -> REGION_SET [label="ra8_dotf_disable"];
 *   ARMED -> ARMED [label="ra8_dotf_rotate_key\n(re-stage + re-arm)"];
 *   ARMED -> RESET [label="ra8_dotf_deinit"];
 * }
 * @enddot
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_dotf_regs.h"
#include "ra8_err.h"

/* =============================================================================
 * Configuration enums
 * =============================================================================
 */

/**
 * @enum ra8_dotf_key_size_t
 * @brief AES key sizes supported by the DOTF AES core.
 *
 * @details
 * HUM Ch 45.1 p 3048 ("AES core function: Block size 128-bit; Key
 * size: 128-bit, 192-bit, 256-bit"). The encoded values are the raw
 * REG00 patterns that select each size; the driver simply ORs them
 * into the assembled REG00 word.
 */
typedef enum : uint32_t {
  k_ra8_dotf_key_size_128 = k_ra8_dotf_reg00_key_size_128, /**< 128-bit AES. */
  k_ra8_dotf_key_size_192 = k_ra8_dotf_reg00_key_size_192, /**< 192-bit AES. */
  k_ra8_dotf_key_size_256 = k_ra8_dotf_reg00_key_size_256, /**< 256-bit AES. */
} ra8_dotf_key_size_t;

/**
 * @enum ra8_dotf_sca_level_t
 * @brief Side-channel countermeasure tuning levels.
 *
 * @details
 * HUM Ch 45.1 p 3048 ("Tamper Resistance: Countermeasures available
 * for side-channel attacks, including SPA/DPA and timing attacks").
 * The HUM does not expose the raw bit names; the driver maps three
 * coarse levels onto REG00 bits 17:16 per the FSP reference:
 *
 *  - off:      bit16=0, bit17=0  (no SCA -- not recommended)
 *  - standard: bit16=1, bit17=0  (default after ra8_dotf_init)
 *  - max:      bit16=1, bit17=1  (paranoid -- higher latency)
 */
typedef enum : uint8_t {
  k_ra8_dotf_sca_off      = 0U, /**< Side-channel countermeasures disabled. */
  k_ra8_dotf_sca_standard = 1U, /**< Default level, bit 16 set.             */
  k_ra8_dotf_sca_max      = 2U, /**< Maximum level, bits 16 + 17 set.       */
} ra8_dotf_sca_level_t;

/* =============================================================================
 * Region descriptor
 * =============================================================================
 */

/**
 * @struct ra8_dotf_region_t
 * @brief Conversion-area descriptor for ``ra8_dotf_set_region``.
 *
 * @details
 * Per HUM Ch 45.3 p 3049 the start and end addresses must be 4 KB
 * aligned and ``start_addr <= end_addr`` is mandatory. The driver
 * rejects misaligned, inverted, out-of-window or overlap-with-other-
 * channel regions before touching hardware.
 *
 * cppcheck cannot see test code, so it complains about every field
 * being unused; ``ra8_dotf_set_region`` reads them all.
 */
typedef struct {
  uint32_t start_addr; /**< First byte of the encrypted region (incl).      */
  uint32_t end_addr;   /**< Last byte of the encrypted region (incl).       */
  uint8_t  key_index;  /**< RSIP key index (forwarded to install_key flow). */
  uint8_t  region_id;  /**< 0..k_ra8_dotf_max_regions-1 staging slot.       */
} ra8_dotf_region_t;

/* =============================================================================
 * Key handle (opaque to ra8_rsip)
 * =============================================================================
 */

/**
 * @struct ra8_dotf_key_handle_t
 * @brief Wrapped-key handle handed to DOTF by ra8_rsip.
 *
 * @details
 * The HAL never sees raw key bits. ``ra8_rsip``'s key-injection layer
 * (HUM Ch 52 p 3302..3307) wraps a plaintext or OEM-protected key
 * into a vendor-defined "wrapped key" envelope and produces this
 * handle. The DOTF driver consumes the handle, stages the wrapped
 * payload into REG03 in big-endian word order, and arms the AES
 * core. Clearing ``words[0]`` to zero indicates an empty / cleared
 * handle -- the driver treats that as "no key staged" and refuses
 * to enable the channel.
 *
 * cppcheck cannot see tests/ so it flags every field as unused; all
 * fields are read in the driver.
 */
typedef struct {
  ra8_dotf_key_size_t size;      /**< 128 / 192 / 256 selector for REG00.   */
  uint8_t             key_index; /**< RSIP key slot this handle came from.  */
  uint8_t             valid;     /**< Non-zero if the wrapped key is real.  */
  uint32_t            words[8];  /**< Wrapped-key payload (up to 256 bits). */
} ra8_dotf_key_handle_t;

/**
 * @typedef ra8_dotf_event_fn_t
 * @brief DOTF fault / status event callback.
 * @param[in] ctx     Caller context.
 * @param[in] channel Channel index 0..1 that raised the event.
 */
typedef void (*ra8_dotf_event_fn_t)(void* ctx, uint8_t channel);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Power on the DOTF block and reset both channels.
 *
 * @details
 * Clears ``MSTPB16`` (DOTF0 + XSPI0) and ``MSTPB17`` (DOTF1 + XSPI1)
 * via ``ra8_mstp_enable``, then writes ``0`` to each channel's REG00
 * and clears CONVAREAST / CONVAREAD to their reset values. Also
 * scrubs every staged region slot, the IV cache, and the bound key
 * handles.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                 DOTF clocked and zeroed.
 * @retval k_ra8_err_hw_init_failed MSTP enable failed for either id.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 *
 * @post Both DOTF channels are reachable but disabled.
 * @post REG00 of every channel reads ``0``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_dotf_set_region
 * @see ra8_dotf_enable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_init(void);

/**
 * @brief Disable DOTF and gate the OSPI clock.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre IRQs masked or single-threaded teardown context.
 * @post Both DOTF channels are disabled and ``MSTPB16/17`` are set.
 *
 * @warning Will also gate the matching XSPI controller -- callers
 *          must have stopped all XiP traffic first.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_deinit(void);

/* =============================================================================
 * Per-region configuration (multi-region staging)
 * =============================================================================
 */

/**
 * @brief Stage one DOTF region in the channel's region table.
 *
 * @details
 * Validates 4 KB alignment, ``start <= end``, that the region falls
 * entirely inside the matching channel's XSPI window
 * (``[0x8000_0000..0x9FFF_FFFF]`` for DOTF0, ``[0x7000_0000..
 * 0x7FFF_FFFF]`` for DOTF1), and that the region does NOT overlap
 * the other channel's currently-armed region. On success the
 * descriptor is copied into slot ``region->region_id`` of the
 * channel's staging table; the slot becomes "live" only when
 * ``ra8_dotf_select_region(channel, region_id)`` is called.
 *
 * @param[in] channel Channel index 0..1.
 * @param[in] region  Non-NULL region descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Region staged.
 * @retval k_ra8_err_null_ptr        ``region`` is NULL.
 * @retval k_ra8_err_invalid_arg     ``channel`` or ``region_id`` out of
 *                                   range, addresses not 4 KB aligned,
 *                                   start > end, or region escapes the
 *                                   matching XSPI window.
 * @retval k_ra8_err_conflict        Region overlaps the other channel's
 *                                   currently-armed region.
 *
 * @pre ``ra8_dotf_init`` has run.
 * @pre Channel ``channel`` may be enabled or disabled (the slot table
 *      is software-only until ``select_region`` is called).
 *
 * @post Slot ``region->region_id`` of channel ``channel`` mirrors
 *       ``*region`` and is marked valid.
 * @post The hardware CONVAREAST / CONVAREAD registers are NOT updated
 *       until ``ra8_dotf_select_region`` runs.
 *
 * @warning HUM 45.3.1 / 45.3.2 say: "Set before the AXI transfer
 *          request and do not make any further changes." Any change
 *          to the live region MUST go through ``ra8_dotf_disable`` ->
 *          ``ra8_dotf_select_region`` -> ``ra8_dotf_enable``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_set_region(uint8_t channel, const ra8_dotf_region_t* region);

/**
 * @brief Promote a staged region into the live CONVAREAST / CONVAREAD pair.
 *
 * @details
 * Atomically writes ``CONVAREAD`` then ``CONVAREAST`` (in that order
 * per the FSP reference comment "Set the end and start area for DOTF
 * conversion in that order to ensure that end address is always
 * higher than start address.", ``r_ospi_b.c``). Caller is
 * expected to have called ``ra8_dotf_disable`` first; the function
 * does NOT clear REG00 itself so the AES core stays primed for the
 * subsequent ``ra8_dotf_enable`` call.
 *
 * @param[in] channel    Channel index 0..1.
 * @param[in] region_id  Slot index in the channel's region table.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Region promoted.
 * @retval k_ra8_err_invalid_arg     Out-of-range channel or region_id.
 * @retval k_ra8_err_invalid_state   The slot has not been staged.
 *
 * @pre ``ra8_dotf_set_region(channel, ..., region_id=region_id)``
 *      previously returned ``k_ra8_ok``.
 * @pre Channel is currently disabled (HUM 45.3.1 p 3049).
 *
 * @post CONVAREAST / CONVAREAD reflect the staged region.
 * @post The "active region" cache for ``channel`` is updated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_select_region(uint8_t channel, uint8_t region_id);

/**
 * @brief Read back the active region for a channel.
 *
 * @param[in]  channel Channel index 0..1.
 * @param[out] region  Receives a copy of the active descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Descriptor returned.
 * @retval k_ra8_err_null_ptr        ``region`` is NULL.
 * @retval k_ra8_err_invalid_arg     Channel out of range.
 * @retval k_ra8_err_invalid_state   No region active on ``channel``.
 *
 * @pre ``region`` is non-NULL.
 * @pre ``channel`` is in range.
 *
 * @post On success ``*region`` holds the live region descriptor.
 * @post Hardware state is unchanged.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_get_active_region(uint8_t channel, ra8_dotf_region_t* region);

/* =============================================================================
 * RSIP key + IV staging
 * =============================================================================
 */

/**
 * @brief Bind a wrapped AES key handle to a DOTF channel.
 *
 * @details
 * Copies the handle into the driver's per-channel slot and stages the
 * wrapped-key payload into REG03 in big-endian word order. The
 * channel is left disabled; the AES core only goes hot once
 * ``ra8_dotf_enable`` (or ``ra8_dotf_rotate_key``) is called. The HAL
 * does not unwrap the key -- that work belongs to ra8_rsip and runs
 * before this call. The handle's ``size`` field is mirrored into the
 * driver's REG00 cache so subsequent ``ra8_dotf_enable`` writes the
 * correct key-size bits.
 *
 * @param[in] channel Channel index 0..1.
 * @param[in] handle  Non-NULL handle. ``handle->valid`` must be non-0.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Key staged.
 * @retval k_ra8_err_null_ptr      ``handle`` is NULL.
 * @retval k_ra8_err_invalid_arg   Channel out of range or handle invalid.
 *
 * @pre ``handle`` is non-NULL and ``handle->valid != 0``.
 * @pre ``ra8_dotf_init`` has run.
 *
 * @post The channel's bound key handle equals ``*handle``.
 * @post REG03 has been written ``words_for_size(handle->size)`` times.
 *
 * @warning Do NOT call this while the channel is enabled. Re-keying
 *          a live channel must go through ``ra8_dotf_rotate_key``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_install_key(uint8_t channel, const ra8_dotf_key_handle_t* handle);

/**
 * @brief Atomically rotate the key bound to a channel.
 *
 * @details
 * Quiesces the channel (writes ``0`` to REG00), re-stages the new
 * wrapped key + IV, then re-arms the AES core with the cached key-
 * size and SCA settings. Used by the bootloader to swap signing keys
 * during anti-rollback handling.
 *
 * Sequence:
 *  1. Save current REG00 enable state.
 *  2. Write 0 to REG00 (disable AES).
 *  3. Replace the bound key handle.
 *  4. Re-stage the IV via REG03.
 *  5. Re-arm REG00 with the new key-size + SCA + enable bits.
 *
 * @param[in] channel    Channel index 0..1.
 * @param[in] new_handle Non-NULL replacement key.
 * @param[in] iv_words   ``k_ra8_dotf_iv_word_count`` words of new IV
 *                       (or NULL to reuse the current IV).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Rotation complete.
 * @retval k_ra8_err_null_ptr        ``new_handle`` is NULL.
 * @retval k_ra8_err_invalid_arg     Channel out of range or handle invalid.
 * @retval k_ra8_err_invalid_state   No region active on the channel.
 *
 * @pre ``ra8_dotf_install_key`` was called at least once.
 * @pre A region is currently armed (``select_region`` ran).
 *
 * @post The new key is in REG03, REG00 is re-armed with enable bit.
 * @post The active key handle reflects ``*new_handle``.
 *
 * @warning Outstanding XiP fetches between step 2 and step 5 will
 *          observe garbage. Callers must temporarily steer
 *          instruction fetches away from the encrypted window
 *          (typically by jumping into SRAM) before invoking this.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_rotate_key(uint8_t                      channel,
                                            const ra8_dotf_key_handle_t* new_handle,
                                            const uint32_t*              iv_words);

/**
 * @brief Stage the AES counter-mode IV for a channel via REG03.
 *
 * @details
 * Per HUM Ch 45.1 p 3048 the AES counter is
 * ``{IV[127:28], Address[31:4]}``. Software still loads a full
 * 128-bit IV; the hardware silently overwrites the bottom 28 bits
 * with the AXI address bits at decryption time. The four 32-bit
 * words are written into REG03 in big-endian byte order, mirroring
 * the FSP reference (``r_ospi_b.c``).
 *
 * @param[in] channel  Channel index 0..1.
 * @param[in] iv_words Pointer to ``k_ra8_dotf_iv_word_count`` words.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Words written.
 * @retval k_ra8_err_null_ptr      ``iv_words`` is NULL.
 * @retval k_ra8_err_invalid_arg   Channel out of range.
 *
 * @pre ``iv_words`` is non-NULL and points to >= 4 valid words.
 * @pre Channel is currently disabled.
 *
 * @post The IV cache for ``channel`` mirrors the new value.
 * @post REG03 has been written 4 times in big-endian word order.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_set_iv(uint8_t channel, const uint32_t* iv_words);

/* =============================================================================
 * Per-channel enable / disable
 * =============================================================================
 */

/**
 * @brief Enable AES decryption for one channel.
 *
 * @details
 * Writes the cached REG00 word (default ``0x2200_0000`` ORed with
 * the bound key-size bits, the SCA bits, and the enable bit). The
 * channel must have a key staged and a region selected.
 *
 * @param[in] channel Channel index 0..1.
 * @return ``ra8_err_t`` error code.
 *
 * @pre ``ra8_dotf_set_region`` + ``ra8_dotf_select_region`` have been
 *      called for ``channel``.
 * @pre ``ra8_dotf_install_key`` has been called for ``channel``.
 *
 * @post REG00 reflects the assembled key-size + SCA + enable pattern.
 * @post Subsequent reads in the conversion area go through the AES
 *       core.
 *
 * @warning If the conversion region overlaps live XiP code the CPU
 *          will fault on the next instruction fetch. Callers must
 *          ensure no fetches target the encrypted window until the
 *          key is in place.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_enable(uint8_t channel);

/**
 * @brief Disable AES decryption for one channel (transparent bypass).
 *
 * @param[in] channel Channel index 0..1.
 * @return ``ra8_err_t`` error code.
 *
 * @pre ``ra8_dotf_init`` has run.
 * @post REG00 reads ``0`` and the conversion area passes through.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_disable(uint8_t channel);

/* =============================================================================
 * REG00 sub-field tuning
 * =============================================================================
 */

/**
 * @brief Update the side-channel countermeasure level for a channel.
 *
 * @param[in] channel Channel index 0..1.
 * @param[in] level   ``k_ra8_dotf_sca_off`` / ``standard`` / ``max``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Cached and (if armed) written through.
 * @retval k_ra8_err_invalid_arg   Channel out of range or unknown level.
 *
 * @pre ``ra8_dotf_init`` has run.
 *
 * @post REG00 SCA bits reflect ``level``; if the channel was already
 *       enabled, the AES core is updated in place (single REG00 write).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_set_sca_level(uint8_t channel, ra8_dotf_sca_level_t level);

/**
 * @brief Update the cached AES key size for a channel.
 *
 * @param[in] channel Channel index 0..1.
 * @param[in] size    ``k_ra8_dotf_key_size_*``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Size cached; if armed, REG00 updated.
 * @retval k_ra8_err_invalid_arg   Channel out of range or unknown size.
 *
 * @pre ``ra8_dotf_init`` has run.
 *
 * @post REG00 key-size bits reflect ``size`` (only if armed).
 * @post Subsequent ``ra8_dotf_enable`` calls write the new size.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_set_key_size(uint8_t channel, ra8_dotf_key_size_t size);

/**
 * @brief Trigger the built-in self-test (REG00 bit 20) for a channel.
 *
 * @details
 * HUM Ch 45.1 p 3048 ("Supports self-test function"). Sets bit 20 of
 * REG00, polls for completion by re-reading REG00 a bounded number
 * of times, and returns the post-test snapshot via ``out_status``.
 * The bit auto-clears in real hardware; in the simulator the bit
 * remains set after the spin (the host has no way to model AES
 * timing) -- callers MUST treat ``out_status`` as opaque diagnostic
 * data, not a pass/fail indicator on the host.
 *
 * @param[in]  channel    Channel index 0..1.
 * @param[out] out_status Non-NULL diagnostic snapshot.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Self-test triggered.
 * @retval k_ra8_err_null_ptr      ``out_status`` is NULL.
 * @retval k_ra8_err_invalid_arg   Channel out of range.
 *
 * @pre ``ra8_dotf_init`` has run.
 * @pre Channel is currently disabled.
 *
 * @post REG00 bit 20 was set at least once.
 * @post On return, REG00 has been restored to its pre-test value.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_run_self_test(uint8_t channel, uint32_t* out_status);

/* =============================================================================
 * Status accessors
 * =============================================================================
 */

/**
 * @brief Read REG00 (raw control / status snapshot).
 *
 * @param[in]  channel  Channel index 0..1.
 * @param[out] out_mask Receives REG00 contents.
 * @return ``ra8_err_t`` error code.
 *
 * @pre ``out_mask`` is non-NULL.
 * @pre ``channel`` is in range.
 * @post On success, ``*out_mask`` reflects the live REG00 value.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_get_status(uint8_t channel, uint32_t* out_mask);

/**
 * @brief Clear REG00 (force the channel into bypass).
 *
 * @param[in] channel Channel index 0..1.
 * @return ``ra8_err_t`` error code.
 *
 * @pre ``channel`` is in range.
 * @post REG00 reads ``0``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_clear_status(uint8_t channel);

/* =============================================================================
 * IRQ attach / dispatch
 * =============================================================================
 */

/**
 * @brief Register a fault / event callback (shared across both channels).
 *
 * @details
 * DOTF on its own does not raise an IRQ -- faults surface through the
 * matching xSPI controller's INTS register. The platform IRQ glue
 * forwards the channel index to ``ra8_dotf_dispatch`` which fires this
 * callback. Useful for telemetry on key-mismatch / illegal-region
 * hits during bring-up.
 *
 * @param[in] fn  Callback fired on dispatch.
 * @param[in] ctx Context forwarded to the callback.
 * @return ``ra8_err_t`` error code.
 *
 * @pre Called with IRQs masked or before dispatch ever fires.
 * @post Subsequent dispatches will fire ``fn(ctx, channel)``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_attach_handler(ra8_dotf_event_fn_t fn, void* ctx);

/**
 * @brief Dispatch a DOTF event from the IRQ glue.
 *
 * @param[in] channel Channel index 0..1 that raised the event. Out of
 *                    range channels are dropped silently.
 * @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_dotf_dispatch(uint8_t channel);

/* =============================================================================
 * Power transition
 * =============================================================================
 */

/**
 * @struct ra8_dotf_open_cfg_t
 * @brief One-shot DOTF bring-up descriptor consumed by ``ra8_dotf_open``.
 *
 * @details
 * Bundles the typical cold-boot sequence (init -> install_key -> set_iv ->
 * set_region -> enable) into a single descriptor so the bootloader can
 * cross from "DOTF off" to "XiP armed" with a single call. Per-field
 * semantics mirror the underlying setters.
 *
 * cppcheck cannot see tests/ so it flags every member as unused; all
 * fields are read by ``ra8_dotf_open`` in ``ra8_dotf.c``.
 */
typedef struct {
  uint8_t               channel;      /**< 0 = DOTF0, 1 = DOTF1.          */
  ra8_dotf_key_handle_t key;          /**< RSIP-wrapped key handle.       */
  uint32_t              iv_words[4];  /**< 128-bit IV in word order.      */
  ra8_dotf_region_t     region;       /**< Initial conversion region.     */
  ra8_dotf_sca_level_t  sca_level;    /**< Side-channel level.            */
  bool                  enable_after; /**< true => arm AES after staging. */
} ra8_dotf_open_cfg_t;

/**
 * @brief One-shot DOTF bring-up: init + install_key + set_iv + set_region (+ enable).
 *
 * @details
 * Convenience entry point for the bootloader: drives
 * ``ra8_dotf_init`` (idempotent re-init is allowed), ``ra8_dotf_install_key``,
 * ``ra8_dotf_set_iv``, ``ra8_dotf_set_region``, ``ra8_dotf_select_region``,
 * ``ra8_dotf_set_sca_level`` and (optionally) ``ra8_dotf_enable`` from a
 * single descriptor. Used during cold boot when XiP code lives behind
 * the AES core and must be armed before the first instruction fetch
 * into the encrypted window.
 *
 * @param[in] cfg Non-NULL bring-up descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                  Channel armed (or staged + idle).
 * @retval k_ra8_err_null_ptr        ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg     Channel out of range or descriptor bad.
 * @retval k_ra8_err_hw_init_failed  MSTP enable failed.
 *
 * @pre IRQs masked or single-threaded boot context.
 * @pre ``cfg->key.valid != 0``.
 *
 * @post Channel ``cfg->channel`` is staged + (if ``enable_after``) armed.
 * @post REG00 / REG03 / CONVAREAST / CONVAREAD reflect the descriptor.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_dotf_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_open(const ra8_dotf_open_cfg_t* cfg);

/**
 * @brief Tear down DOTF and release all hardware state.
 *
 * @details
 * Companion to ``ra8_dotf_open``: disables both channels and gates the
 * shared OSPI MSTP bits via ``ra8_dotf_deinit``. Provided as a thin
 * symmetric helper so callers do not have to mix ``open`` / ``deinit``
 * vocabulary.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre All XiP traffic is quiesced.
 * @post Both DOTF channels disabled and gated.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_dotf_open
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_close(void);

/**
 * @brief Stage a region from a (start, length) pair instead of a struct.
 *
 * @details
 * Thin convenience wrapper around ``ra8_dotf_set_region`` for callers
 * that have a ``base + len`` pair handy. Internally constructs a
 * ``ra8_dotf_region_t`` with ``region_id = 0``, ``key_index = 0`` and
 * forwards. The end address is derived as ``start + len - 1`` and is
 * subject to the same 4 KB alignment constraints as the struct API.
 *
 * @param[in] channel Channel index 0..1.
 * @param[in] start   First byte of the encrypted region.
 * @param[in] len     Length of the region in bytes (must be > 0 and 4 KB aligned).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Region staged in slot 0.
 * @retval k_ra8_err_invalid_arg   Channel out of range or alignment bad.
 *
 * @pre ``ra8_dotf_init`` (or ``ra8_dotf_open``) has run.
 * @pre ``len > 0`` and ``(start | len) % 4096 == 0``.
 *
 * @post Slot 0 of the channel mirrors the constructed region.
 * @post Hardware CONVAREAST / CONVAREAD are NOT updated until
 *       ``ra8_dotf_select_region`` is called.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_dotf_set_region
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_set_region_window(uint8_t channel, uint32_t start, uint32_t len);

/**
 * @brief Park the DOTF block prior to entering a low-power mode.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre All XiP traffic from the matching xSPI is quiesced.
 * @post DOTF MSTP bits are set; AES core is gated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_enter_stop(void);

/**
 * @brief Bring the DOTF block back from low-power mode.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre System clocks have been restored.
 * @post DOTF MSTP bits are cleared but channels remain disabled.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dotf_exit_stop(void);

#ifdef __cplusplus
}
#endif
