/**
 * @file ra8_epd_cal.h
 * @brief Per-device e-paper panel calibration (VCOM) -- record, storage seam,
 *        resolution policy
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * ## Why this module exists
 *
 * Every electrophoretic panel is manufactured with its own **VCOM** --
 * the common-electrode bias that centres the drive waveform on that
 * particular sheet of film. The correct value is measured at the factory
 * and **printed on that panel's flex cable** (e.g. ``-1.53V``). It is
 * per-unit data, in exactly the same sense as a board's MAC address or a
 * sensor's trim constants.
 *
 * Driving a panel at the wrong VCOM is not a cosmetic problem:
 *
 *  - **Immediately**, contrast drops and ghosting appears.
 *  - **Cumulatively**, the net DC bias left across the film degrades it
 *    permanently. The damage is not recoverable in software.
 *
 * That makes VCOM the one value in this system where a *plausible-looking
 * wrong number is worse than no number at all*, and it drives every design
 * decision below.
 *
 * ## Rule 1: VCOM is never a source constant
 *
 * A hardcoded VCOM is correct for exactly the one panel the author had on
 * the bench. Anyone who clones this firmware onto a panel calibrated
 * differently gets a wrong value and quietly damages their hardware --
 * and so does the original owner the day they replace a cracked panel.
 * There is therefore **no default VCOM anywhere in this tree**, and no
 * code path that invents one.
 *
 * ## Rule 2: resolution order, with an explicit dead end
 *
 * ``ra8_epd_cal_resolve`` consults sources in descending order of
 * authority and stops at the first that yields an **in-range** value:
 *
 *  1. **The controller's own persisted value.** The IT8951 driver board
 *     powers up with whatever VCOM its own configuration holds, which for
 *     a vendor-provisioned board is the right one. Read it back with the
 *     VCOM command and range-check it. This is the source that makes a
 *     stock Waveshare HAT work out of the box.
 *  2. **The per-device configuration record** (::ra8_epd_cal_record_t)
 *     read through the injected ::ra8_epd_cal_store_t. This is the
 *     authority once a panel has been provisioned on this device, and the
 *     only one that survives a controller that forgets.
 *  3. **An explicit provisioning value** supplied by the operator for
 *     this boot -- the number read off the panel's flex cable.
 *
 * If none of those produces a trusted value, resolution **fails**
 * (``k_ra8_err_not_found``, ``source == k_ra8_epd_cal_src_none``) and the
 * caller must **refuse to drive the panel**. A blank screen is a support
 * call; a DC-biased panel is landfill.
 *
 * Resolving a value is necessary but not sufficient: ``ra8_epd_cal_apply``
 * writes it and then **reads it back and compares**, and the e-paper
 * driver refuses every display command until a VCOM has been echoed back
 * unchanged. A link that accepts writes and changes nothing is the failure
 * mode that otherwise looks exactly like success.
 *
 * ## The one build-flag exception, and why it is not a hole
 *
 * ``-DRA8_BENCH_VCOM_MV=<mv>`` adds a fourth, lowest-authority source so
 * first light on a bench panel is not blocked behind provisioned storage.
 * It is off unless typed on the compiler command line, it is consulted
 * only after all three real sources decline (so it can never override a
 * genuine calibration), it is range-checked like everything else, it logs
 * loudly on every boot that uses it, and it is a compile **error** when
 * ``RA8_PRODUCTION_BUILD`` is defined. That last guard is what keeps it
 * from becoming the shipped hardcoded VCOM this module exists to prevent.
 *
 * ## Rule 3: the record must outlive firmware updates
 *
 * The device carries an A/B-bank DFU bootloader (``libs/ra8_dfu``) with
 * Root-of-Trust-signed images (``ra8_rot_verify_image``). Two consequences
 * follow, and they are the whole reason this record has its own integrity
 * check rather than leaning on the image signature:
 *
 *  - **Per-device bytes cannot live inside the signed image.** One
 *    signature authenticates one exact byte sequence. A value that
 *    differs per unit cannot be covered by an image signature shared
 *    across units without re-signing per device, which defeats the point
 *    of having a single signed release. So the record is *outside* the
 *    signature's scope and must carry **its own** integrity check --
 *    hence the CRC-32 trailer and the schema version below.
 *  - **The record must live outside both code banks.** Slot A
 *    (``0x02020000``) and Slot B (``0x02090000``) are erased and
 *    reprogrammed wholesale by DFU, and a rollback re-points the boot at
 *    an older image. Anything stored in either bank is destroyed by an
 *    update or reverted by a rollback. The record therefore belongs in
 *    **extra-MRAM (data flash) at ``k_ra8_flash_extra_start``**, which no
 *    DFU path erases -- the same region and the same reasoning as the
 *    anti-rollback counter in ``ra8_dfu_antirollback.c``. Its slot is
 *    ::k_ra8_epd_cal_extra_mram_offset, placed clear of that counter.
 *
 * Because the record is unsigned, its integrity check is a **corruption**
 * check, not an authenticity one: it detects a torn write or a bit flip,
 * not an attacker with physical write access to data flash. That is the
 * correct threat model here -- an attacker who can rewrite data flash can
 * equally attack the panel directly -- but it is stated rather than
 * assumed.
 *
 * ## Storage layout (::k_ra8_epd_cal_blob_size bytes)
 *
 * @verbatim
 *   offset  size  field
 *   ------  ----  ------------------------------------------------
 *   0       4     magic   = 'E','V','C','M' (ASCII)
 *   4       1     version = ::k_ra8_epd_cal_schema_version
 *   5       1     reserved (zeroed)
 *   6       2     payload_len = bytes of payload (2 for schema 1)
 *   8       2     vcom_mv, little-endian magnitude in millivolts
 *   10      18    reserved (zeroed, room for later schema growth)
 *   28      4     crc32   = IEEE 802.3 polynomial over bytes 0..27
 * @endverbatim
 *
 * ``payload_len`` plus ``version`` are what make forward migration
 * possible: a future schema appends fields into the reserved span and
 * bumps both, and this reader rejects what it does not understand rather
 * than misreading it.
 *
 * The blob is 32 bytes so it maps exactly onto one extra-MRAM erase block.
 *
 * ## Dependency inversion
 *
 * This module includes no HAL header. It reaches the controller and the
 * non-volatile store only through injected function-pointer seams
 * (::ra8_epd_cal_panel_ops_t, ::ra8_epd_cal_store_t) -- NASA Power-of-10
 * Rule 9 deviation, documented in CLAUDE.md. Production binds
 * ``ra8_epaper_get_vcom`` / ``ra8_epaper_set_vcom`` and an extra-MRAM
 * store; host tests bind mocks and drive every branch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_epd_cal_limits_t
 * @brief Sizing and schema constants for the calibration record.
 */
typedef enum : uint16_t {
  k_ra8_epd_cal_blob_size      = 32U, /**< Serialised record size in bytes.  */
  k_ra8_epd_cal_schema_version = 1U,  /**< Current on-flash schema version.  */
  k_ra8_epd_cal_payload_len    = 2U,  /**< Schema-1 payload bytes (vcom_mv). */
} ra8_epd_cal_limits_t;

/**
 * @enum ra8_epd_cal_layout_t
 * @brief Byte offsets inside the serialised record.
 */
typedef enum : uint8_t {
  k_ra8_epd_cal_off_magic       = 0U,  /**< 'EVCM' magic.           */
  k_ra8_epd_cal_off_version     = 4U,  /**< Schema version byte.    */
  k_ra8_epd_cal_off_reserved0   = 5U,  /**< One reserved zero byte. */
  k_ra8_epd_cal_off_payload_len = 6U,  /**< Payload length, LE16.   */
  k_ra8_epd_cal_off_vcom_mv     = 8U,  /**< VCOM magnitude, LE16.   */
  k_ra8_epd_cal_off_reserved1   = 10U, /**< Reserved growth span.   */
  k_ra8_epd_cal_off_crc32       = 28U, /**< CRC-32 trailer, LE32.   */
} ra8_epd_cal_layout_t;

/**
 * @enum ra8_epd_cal_magic_t
 * @brief Bytes of the record magic 'EVCM' (E-paper VCOM).
 */
typedef enum : uint8_t {
  k_ra8_epd_cal_magic_b0 = 0x45U, /**< 'E' */
  k_ra8_epd_cal_magic_b1 = 0x56U, /**< 'V' */
  k_ra8_epd_cal_magic_b2 = 0x43U, /**< 'C' */
  k_ra8_epd_cal_magic_b3 = 0x4DU, /**< 'M' */
} ra8_epd_cal_magic_t;

/**
 * @enum ra8_epd_cal_storage_t
 * @brief Placement of the record in non-volatile storage.
 *
 * @details
 * The offset is relative to ``k_ra8_flash_extra_start`` (the extra-MRAM
 * option-setting window base, ``0x02E07600`` -- HUM Ch 59.7.4.5 Table 59.15
 * p 3592). No DFU path erases extra-MRAM, so the record survives an A/B
 * firmware update and a rollback alike.
 * @note The window is one-time-programmable option-setting memory, not a
 *       rewritable data-flash; a real rewritable-medium home for this record is
 *       a bench question tracked by #315.
 *
 * ## Why this is not at the obvious ``0x40``
 *
 * Block 0 of the window is already claimed by the DFU anti-rollback
 * counter and block 1 is left spare for it to grow into, which makes
 * ``0x40`` look like the natural next slot. It is not available: the
 * general per-device configuration record specified alongside this module
 * reserves ``0x40 .. 0x1BF`` for its two sequence-numbered 192-byte
 * copies. Landing this blob at ``0x40`` would have put a VCOM record
 * exactly on top of copy 0 of that record -- two writers, one address,
 * discovered on the first provisioned unit rather than in review. This
 * blob therefore starts after that reservation, at ``0x200``.
 *
 * ## This placement is transitional by design
 *
 * The per-device record is the intended long-term home for VCOM: one
 * record, one CRC, one signature, all the per-unit bytes together. When it
 * lands, the production binding of ::ra8_epd_cal_store_t becomes a reader
 * over *that* record and this standalone blob is retired -- the resolution
 * policy in this module, which is the part that matters, does not change,
 * because the store is an injected seam and callers of
 * ``ra8_epd_cal_resolve`` cannot tell which backing answered. That
 * substitutability is the whole reason the seam exists.
 *
 * @invariant The offset is a multiple of ::k_ra8_epd_cal_blob_size so the
 *            record occupies exactly one 32-byte erase block.
 * @invariant The offset lies outside the per-device record's reserved
 *            ``0x40 .. 0x1BF`` span.
 *
 * @see ra8_epd_cal_store_t
 */
typedef enum : uint32_t {
  k_ra8_epd_cal_extra_mram_offset = 0x200U, /**< Byte offset into extra-MRAM. */
} ra8_epd_cal_storage_t;

/**
 * @enum ra8_epd_cal_source_t
 * @brief Which authority supplied the resolved VCOM.
 *
 * @details
 * Reported by ``ra8_epd_cal_resolve`` so the caller can log provenance and
 * so a caller that requires provisioned-on-this-device calibration can
 * reject a merely controller-reported value.
 *
 * @see ra8_epd_cal_resolve
 */
typedef enum : uint8_t {
  k_ra8_epd_cal_src_none        = 0U, /**< No trusted value -- do NOT drive. */
  k_ra8_epd_cal_src_panel       = 1U, /**< Read back from the controller.    */
  k_ra8_epd_cal_src_record      = 2U, /**< From the per-device NV record.    */
  k_ra8_epd_cal_src_provisioned = 3U, /**< Operator-supplied this boot.      */
  k_ra8_epd_cal_src_bench       = 4U, /**< ``RA8_BENCH_VCOM_MV``; see below. */
} ra8_epd_cal_source_t;

/**
 * @struct ra8_epd_cal_limits_mv_t
 * @brief The panel's documented VCOM window, in millivolt magnitudes.
 *
 * @details
 * Range-checking is what turns "a number came back" into "a number worth
 * trusting". A controller that has not finished booting reports ``0``; a
 * corrupted read reports ``0xFFFF``; both must be rejected rather than
 * programmed. The window is panel data -- state it from the panel's
 * datasheet in the BSP descriptor, not from a guess here.
 *
 * @invariant ``min_mv <= max_mv``.
 * @invariant ``min_mv`` is non-zero, so zero can never be in range.
 *
 * @par Example:
 * @code
 * // Waveshare 6 inch HD (IT8951): panels ship labelled around -1.5 V.
 * const ra8_epd_cal_limits_mv_t limits = {.min_mv = 200U, .max_mv = 4000U};
 * @endcode
 *
 * @see ra8_epd_cal_vcom_in_range
 */
typedef struct {
  uint16_t min_mv; /**< Lowest accepted VCOM magnitude, millivolts.  */
  uint16_t max_mv; /**< Highest accepted VCOM magnitude, millivolts. */
} ra8_epd_cal_limits_mv_t;

/**
 * @struct ra8_epd_cal_record_t
 * @brief Decoded per-device calibration record.
 *
 * @details
 * The in-memory form of the serialised blob. ``schema_version`` is carried
 * through so a caller can tell a freshly written record from one migrated
 * off an older schema.
 *
 * @invariant ``vcom_mv`` is the panel's VCOM **magnitude** in millivolts
 *            (VCOM itself is negative; the sign is implicit).
 * @invariant ``schema_version <= ::k_ra8_epd_cal_schema_version``.
 *
 * @par Example:
 * @code
 * ra8_epd_cal_record_t rec = {.vcom_mv = 1530U,
 *                             .schema_version = k_ra8_epd_cal_schema_version};
 * @endcode
 *
 * @see ra8_epd_cal_serialize
 * @see ra8_epd_cal_deserialize
 */
typedef struct {
  uint16_t vcom_mv;        /**< VCOM magnitude in millivolts (e.g. 1530). */
  uint8_t  schema_version; /**< Schema the record was written under.      */
} ra8_epd_cal_record_t;

/**
 * @typedef ra8_epd_cal_nv_read_fn_t
 * @brief Read the serialised record out of non-volatile storage (DI seam).
 *
 * @details
 * Production reads ::k_ra8_epd_cal_blob_size bytes from extra-MRAM at
 * ``k_ra8_flash_extra_start + k_ra8_epd_cal_extra_mram_offset``; host
 * tests read a RAM fixture. Returning any non-``k_ra8_ok`` code makes the
 * resolver treat the record source as absent and fall through -- it never
 * fabricates a value.
 *
 * @param[in]  ctx Implementation context (may be NULL).
 * @param[out] dst Destination buffer; non-NULL, at least ``len`` bytes.
 * @param[in]  len Bytes to read; always ::k_ra8_epd_cal_blob_size.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           ``dst`` holds ``len`` bytes.
 * @retval k_ra8_err_null_ptr ``dst`` was NULL.
 * @retval other              Backing-store fault; treated as "no record".
 *
 * @note Thread safety is the implementation's responsibility; the
 *       boot-path caller is single-threaded.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_epd_cal_nv_read_fn_t)(void* ctx, uint8_t* dst, size_t len);

/**
 * @typedef ra8_epd_cal_nv_write_fn_t
 * @brief Durably store the serialised record (DI seam).
 *
 * @details
 * Called only from the explicit provisioning path
 * (``ra8_epd_cal_provision``), never from the boot-time read path -- a
 * boot must not silently rewrite calibration.
 *
 * @param[in] ctx Implementation context (may be NULL).
 * @param[in] src Serialised record; non-NULL, ``len`` bytes.
 * @param[in] len Bytes to write; always ::k_ra8_epd_cal_blob_size.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Record committed durably.
 * @retval k_ra8_err_null_ptr ``src`` was NULL.
 * @retval other              Program fault; the caller must not assume
 *                            the record persisted.
 *
 * @note Thread safety is the implementation's responsibility.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_epd_cal_nv_write_fn_t)(void* ctx, const uint8_t* src, size_t len);

/**
 * @struct ra8_epd_cal_store_t
 * @brief Injected non-volatile store for the calibration record.
 *
 * @details
 * A store with a NULL ``read`` is legal and simply means "no per-device
 * record on this build" -- the resolver skips that source rather than
 * failing, so a board without data flash still resolves from the
 * controller or from provisioning.
 *
 * @invariant ``read`` and ``write`` are either NULL or callable for the
 *            lifetime of every resolve / provision call.
 *
 * @par Example:
 * @code
 * const ra8_epd_cal_store_t store = {.read = my_read, .write = my_write,
 *                                    .ctx = nullptr};
 * @endcode
 *
 * @see ra8_epd_cal_resolve
 */
typedef struct {
  ra8_epd_cal_nv_read_fn_t  read;  /**< Record read seam; may be NULL.  */
  ra8_epd_cal_nv_write_fn_t write; /**< Record write seam; may be NULL. */
  void*                     ctx;   /**< Opaque context for both.        */
} ra8_epd_cal_store_t;

/**
 * @typedef ra8_epd_cal_panel_get_fn_t
 * @brief Read the controller's current VCOM, in millivolts (DI seam).
 *
 * @details Production binds ``ra8_epaper_get_vcom``.
 *
 * @param[in]  ctx    Implementation context (may be NULL).
 * @param[out] out_mv Receives the VCOM magnitude; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           ``*out_mv`` holds the controller's VCOM.
 * @retval k_ra8_err_null_ptr ``out_mv`` was NULL.
 * @retval other              Bus fault; treated as "no value".
 *
 * @note Not thread-safe; boot-path use only.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_epd_cal_panel_get_fn_t)(void* ctx, uint16_t* out_mv);

/**
 * @typedef ra8_epd_cal_panel_set_fn_t
 * @brief Programme the controller's VCOM, in millivolts (DI seam).
 *
 * @details Production binds ``ra8_epaper_set_vcom``.
 *
 * @param[in] ctx Implementation context (may be NULL).
 * @param[in] mv  VCOM magnitude to programme; already range-checked.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Controller accepted the new VCOM.
 * @retval other    Bus fault; the previous VCOM remains in effect.
 *
 * @note Not thread-safe; boot-path use only.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_epd_cal_panel_set_fn_t)(void* ctx, uint16_t mv);

/**
 * @struct ra8_epd_cal_panel_ops_t
 * @brief Injected controller access for VCOM.
 *
 * @details
 * A NULL ``get`` disables the controller-reported source; a NULL ``set``
 * makes ``ra8_epd_cal_apply`` fail rather than pretend to have programmed
 * the panel.
 *
 * @invariant Members are either NULL or callable for the call's lifetime.
 *
 * @par Example:
 * @code
 * const ra8_epd_cal_panel_ops_t panel = {.get = my_get, .set = my_set};
 * @endcode
 *
 * @see ra8_epd_cal_resolve
 */
typedef struct {
  ra8_epd_cal_panel_get_fn_t get; /**< VCOM read seam; may be NULL.  */
  ra8_epd_cal_panel_set_fn_t set; /**< VCOM write seam; may be NULL. */
  void*                      ctx; /**< Opaque context for both.      */
} ra8_epd_cal_panel_ops_t;

/**
 * @struct ra8_epd_cal_cfg_t
 * @brief Everything ``ra8_epd_cal_resolve`` needs.
 *
 * @details
 * Follows the repo's config-struct convention: one struct, passed to the
 * entry point, carrying both the injected seams and the panel's data.
 *
 * ``provisioned_mv`` is the escape hatch for a device whose controller has
 * forgotten and whose record is blank -- the operator types in the number
 * printed on the flex cable and the app passes it here, typically together
 * with a ``ra8_epd_cal_provision`` call to make it durable. It is
 * deliberately an explicit, caller-supplied field rather than a built-in
 * default: there is no value this module is entitled to invent.
 *
 * @invariant ``limits`` satisfies ::ra8_epd_cal_limits_mv_t's invariants.
 * @invariant ``provisioned_mv`` is zero when ``has_provisioned`` is false.
 *
 * @par Example:
 * @code
 * const ra8_epd_cal_cfg_t cfg = {
 *   .limits = {.min_mv = 200U, .max_mv = 4000U},
 *   .panel  = panel_ops,
 *   .store  = nv_store,
 *   .has_provisioned = false,
 *   .provisioned_mv  = 0U,
 * };
 * @endcode
 *
 * @see ra8_epd_cal_resolve
 */
typedef struct {
  ra8_epd_cal_limits_mv_t limits;          /**< Panel's documented VCOM window. */
  ra8_epd_cal_panel_ops_t panel;           /**< Controller access seam.         */
  ra8_epd_cal_store_t     store;           /**< Per-device record store seam.   */
  uint16_t                provisioned_mv;  /**< Operator value for this boot.   */
  bool                    has_provisioned; /**< Whether the field above is set. */
} ra8_epd_cal_cfg_t;

/**
 * @struct ra8_epd_cal_result_t
 * @brief Outcome of ``ra8_epd_cal_resolve``.
 *
 * @invariant ``source == ::k_ra8_epd_cal_src_none`` if and only if
 *            ``vcom_mv == 0`` and resolution failed.
 * @invariant When ``source`` is not ``none``, ``vcom_mv`` lies inside the
 *            configured limits.
 *
 * @par Example:
 * @code
 * ra8_epd_cal_result_t r = {};
 * if (ra8_epd_cal_resolve(&cfg, &r) != k_ra8_ok) { return; } // do NOT drive
 * @endcode
 *
 * @see ra8_epd_cal_resolve
 */
typedef struct {
  uint16_t             vcom_mv; /**< Resolved VCOM magnitude, millivolts. */
  ra8_epd_cal_source_t source;  /**< Which authority supplied it.         */
} ra8_epd_cal_result_t;

/* ===========================================================================
 * Record serialisation
 * ===========================================================================
 */

/**
 * @brief Range-check a candidate VCOM against a panel's documented window.
 *
 * @details
 * The single decision every source's value passes through. Rejects the two
 * failure signatures that matter in practice -- ``0`` from a controller
 * that has not finished booting, and ``0xFFFF`` from blank flash or a
 * failed read -- by requiring a non-zero ``min_mv`` in the limits.
 *
 * @param[in] mv     Candidate VCOM magnitude in millivolts.
 * @param[in] limits Panel's documented window; non-NULL.
 *
 * @return Whether ``mv`` may be programmed.
 * @retval true  ``limits->min_mv <= mv <= limits->max_mv``.
 * @retval false ``mv`` is outside the window, or ``limits`` is NULL.
 *
 * @pre  ``limits`` is NULL or readable.
 * @pre  ``limits->min_mv <= limits->max_mv``.
 * @post No state mutated.
 * @post Return depends solely on the arguments.
 *
 * @note Pure function; thread-safe.
 *
 * @see ra8_epd_cal_resolve
 *
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_epd_cal_vcom_in_range(uint16_t mv, const ra8_epd_cal_limits_mv_t* limits);

/**
 * @brief Serialise a record into its 32-byte on-flash form.
 *
 * @details
 * Writes magic, schema version, payload length and the VCOM field, zeroes
 * every reserved byte, then appends the CRC-32 (IEEE 802.3) of everything
 * preceding the trailer. Reserved bytes are zeroed rather than left
 * undefined so the CRC is reproducible for a given record.
 *
 * @param[in]  rec      Record to serialise; non-NULL.
 * @param[out] dst      Destination buffer; non-NULL.
 * @param[in]  dst_size Capacity of ``dst``; must be >=
 *                      ::k_ra8_epd_cal_blob_size.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               ``dst`` holds a sealed record.
 * @retval k_ra8_err_null_ptr     ``rec`` or ``dst`` is NULL.
 * @retval k_ra8_err_invalid_size ``dst_size`` too small.
 * @retval k_ra8_err_invalid_arg  ``rec->vcom_mv`` is zero.
 *
 * @pre  ``dst`` has room for ::k_ra8_epd_cal_blob_size bytes.
 * @pre  ``rec->vcom_mv`` was range-checked by the caller.
 * @post On success ``ra8_epd_cal_deserialize`` round-trips ``dst``.
 * @post On failure ``dst`` is untouched.
 *
 * @note Thread-safe (no shared state).
 *
 * @see ra8_epd_cal_deserialize
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epd_cal_serialize(const ra8_epd_cal_record_t* rec, uint8_t* dst, size_t dst_size);

/**
 * @brief Parse and integrity-check a serialised record.
 *
 * @details
 * Validation order is magic, then schema version, then payload length,
 * then CRC-32. Each rejection is distinct so a caller can tell "never
 * provisioned" (blank flash fails the magic check) from "provisioned but
 * corrupted" (magic passes, CRC fails) -- the two demand different
 * operator responses.
 *
 * A record whose ``version`` exceeds ::k_ra8_epd_cal_schema_version is
 * rejected rather than best-effort parsed: a newer writer may have
 * redefined the payload, and misreading calibration is exactly the failure
 * this module exists to prevent.
 *
 * @param[in]  src      Serialised record; non-NULL.
 * @param[in]  src_size Bytes available at ``src``; must be >=
 *                      ::k_ra8_epd_cal_blob_size.
 * @param[out] out_rec  Receives the decoded record; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Record valid; ``*out_rec`` populated.
 * @retval k_ra8_err_null_ptr        ``src`` or ``out_rec`` is NULL.
 * @retval k_ra8_err_invalid_size    ``src_size`` too small.
 * @retval k_ra8_err_not_found       Magic absent -- never provisioned.
 * @retval k_ra8_err_not_supported   Schema version newer than this build.
 * @retval k_ra8_err_validation_failed Payload length wrong for the schema.
 * @retval k_ra8_err_crc_mismatch    CRC trailer does not match the body.
 *
 * @pre  ``src`` holds at least ``src_size`` readable bytes.
 * @pre  ``out_rec`` is writable.
 * @post On success ``out_rec->vcom_mv`` is non-zero.
 * @post On failure ``*out_rec`` is untouched.
 *
 * @note Thread-safe (no shared state).
 *
 * @see ra8_epd_cal_serialize
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epd_cal_deserialize(const uint8_t* src, size_t src_size, ra8_epd_cal_record_t* out_rec);

/* ===========================================================================
 * Resolution + application
 * ===========================================================================
 */

/**
 * @brief Resolve the VCOM to drive this panel at, or fail safe.
 *
 * @details
 * Walks the sources documented at the top of this file -- controller,
 * then per-device record, then operator-provisioned value -- and returns
 * the first that yields a value inside ``cfg->limits``. Every source is
 * optional: a NULL seam or a failing read simply moves to the next.
 *
 * **This function never invents a value.** When no source yields an
 * in-range number it returns ``k_ra8_err_not_found`` with
 * ``out_result->source == ::k_ra8_epd_cal_src_none`` and
 * ``vcom_mv == 0``, and the caller **must not drive the panel**. That is
 * the whole point: an unpowered panel is recoverable, a panel driven at a
 * guessed bias is not.
 *
 * @param[in]  cfg        Seams, limits and any provisioning value; non-NULL.
 * @param[out] out_result Receives the resolved value and its provenance;
 *                        non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               A trusted VCOM was resolved.
 * @retval k_ra8_err_null_ptr     ``cfg`` or ``out_result`` is NULL.
 * @retval k_ra8_err_invalid_arg  ``cfg->limits`` is self-inconsistent
 *                                (zero or inverted window).
 * @retval k_ra8_err_not_found    No source produced an in-range value --
 *                                refuse to drive the panel.
 *
 * @pre  The controller has been initialised if ``cfg->panel.get`` is bound.
 * @pre  ``cfg->limits`` states the panel's documented VCOM window.
 * @post On success ``out_result->vcom_mv`` is inside ``cfg->limits``.
 * @post On failure ``out_result->source`` is ::k_ra8_epd_cal_src_none.
 *
 * @note Not thread-safe; boot-path use only.
 * @warning Success means "a value worth trusting was found", not "the
 *          panel is now driving at it". Call ``ra8_epd_cal_apply`` to
 *          programme it.
 *
 * @par Example:
 * @code
 * ra8_epd_cal_result_t r = {};
 * if (ra8_epd_cal_resolve(&cfg, &r) != k_ra8_ok) {
 *   ra8_log_error("APP", "no trusted VCOM -- panel left dark");
 *   return k_ra8_err_not_found;   // fail safe
 * }
 * (void)ra8_epd_cal_apply(&cfg, &r);
 * @endcode
 *
 * @see ra8_epd_cal_apply
 * @see ra8_epd_cal_provision
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epd_cal_resolve(const ra8_epd_cal_cfg_t* cfg,
                                            ra8_epd_cal_result_t*    out_result);

/**
 * @brief Programme a resolved VCOM onto the controller and confirm it took.
 *
 * @details
 * Re-checks the value against ``cfg->limits`` before writing it, writes it
 * through ``cfg->panel.set``, then reads it straight back through
 * ``cfg->panel.get`` and requires an exact match. The pre-write re-check is
 * deliberate belt-and-braces -- ``result`` is caller-owned memory between
 * ``resolve`` and here, and this is the last point before a number reaches
 * the panel.
 *
 * **The readback is the load-bearing part.** A dead link, a controller
 * that ignores the command, or a bus stuck returning zeroes all accept the
 * write and change nothing; without the compare that is indistinguishable
 * from success, and the film would sit biased at the controller's own
 * unknown value. The readback is compared for equality and deliberately
 * not re-range-checked -- the written value was range-checked a few lines
 * earlier, so an equal readback is in range by construction and a second
 * test would be unreachable code wearing a safety check's clothes. Range
 * validation of a value the controller *reports* belongs where such a
 * value is adopted rather than confirmed, which is the resolve-from-
 * controller source, and it is applied there.
 *
 * A ``cfg`` with no ``panel.get`` binding is rejected rather than
 * write-and-hope: an unconfirmable bias is not one to leave on a panel.
 *
 * @param[in] cfg    Seams and limits; non-NULL, with **both** ``panel.set``
 *                   and ``panel.get`` bound.
 * @param[in] result A successful ``ra8_epd_cal_resolve`` outcome; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                     Controller accepted the VCOM and
 *                                      echoed it back unchanged.
 * @retval k_ra8_err_null_ptr           ``cfg`` or ``result`` is NULL.
 * @retval k_ra8_err_invalid_state      ``result->source`` is ``none``.
 * @retval k_ra8_err_not_supported      ``cfg->panel.set`` or ``cfg->panel.get``
 *                                      is not bound.
 * @retval k_ra8_err_range_check_failed ``result->vcom_mv`` is out of range.
 * @retval k_ra8_err_hw_error           The readback transaction failed.
 * @retval k_ra8_err_validation_failed  The controller echoed a different
 *                                      value. **Do not drive the panel.**
 * @retval other                        Forwarded from ``cfg->panel.set``.
 *
 * @pre  ``result`` came from a successful ``ra8_epd_cal_resolve``.
 * @pre  ``cfg->panel.set`` and ``cfg->panel.get`` are bound to the controller.
 * @post On success the controller reports ``result->vcom_mv`` as its VCOM.
 * @post On failure the caller must leave the panel dark -- the bias in
 *       effect is unknown, which is precisely the state to refuse.
 *
 * @note Not thread-safe; boot-path use only.
 *
 * @see ra8_epd_cal_resolve
 * @see ra8_epaper_set_vcom
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epd_cal_apply(const ra8_epd_cal_cfg_t*    cfg,
                                          const ra8_epd_cal_result_t* result);

/**
 * @brief Write a per-device calibration record to non-volatile storage.
 *
 * @details
 * The explicit provisioning path: range-checks ``vcom_mv``, serialises a
 * sealed record and commits it through ``cfg->store.write``. Called by a
 * provisioning app or a service menu after the operator enters the value
 * printed on the panel's flex cable -- never automatically from a boot
 * path, so calibration cannot drift without someone asking for it.
 *
 * @param[in] cfg     Seams and limits; non-NULL.
 * @param[in] vcom_mv VCOM magnitude to persist, in millivolts.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                     Record committed durably.
 * @retval k_ra8_err_null_ptr           ``cfg`` is NULL.
 * @retval k_ra8_err_not_supported      ``cfg->store.write`` is not bound.
 * @retval k_ra8_err_range_check_failed ``vcom_mv`` outside ``cfg->limits``.
 * @retval other                        Forwarded from ``cfg->store.write``.
 *
 * @pre  ``vcom_mv`` was read off this panel's own flex cable.
 * @pre  ``cfg->store.write`` targets storage outside both DFU code banks.
 * @post On success a later ``ra8_epd_cal_resolve`` finds the record.
 * @post On failure nothing was persisted.
 *
 * @note Not thread-safe.
 * @warning Provisioning the wrong number is exactly as damaging as having
 *          none. Confirm the value against the panel's flex cable.
 *
 * @see ra8_epd_cal_resolve
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epd_cal_provision(const ra8_epd_cal_cfg_t* cfg, uint16_t vcom_mv);

#ifdef __cplusplus
}
#endif
