/**
 * @file ra8_devcfg.h
 * @brief Per-device (per-unit) configuration record -- schema, storage seam,
 *        two-copy resolution.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * ## Why this module exists
 *
 * A shipped product carries bytes that **differ per unit** and that a
 * Root-of-Trust image signature structurally cannot cover: the e-paper panel
 * VCOM printed on its flex tail, a serial number, the panel LUT id, touch
 * calibration, a device-key identity. One signature covers one byte sequence;
 * per-unit values differ per unit, so the config region needs **its own
 * integrity check** and must live **outside the A/B code banks** so neither an
 * update nor a rollback can erase it.
 *
 * This module owns the record: a versioned, CRC-protected schema; a
 * two-copy, header-last commit that survives a torn write; a boot-time
 * resolver that picks the surviving copy or reports UNPROVISIONED; and a
 * dependency-injection seam so the backing store is a RAM mock under host
 * test and the extra-MRAM (data-flash) window on silicon.
 *
 * ## Storage backing
 *
 * The record lives in the extra-MRAM option-setting window at
 * ``k_ra8_flash_extra_start`` (0x02E07600), which no DFU slot program or
 * erase touches, so it survives an A/B firmware update and a rollback alike.
 * Two ::k_ra8_devcfg_slot_bytes copies sit at ::k_ra8_devcfg_copy0_off and
 * ::k_ra8_devcfg_copy1_off -- the span ``0x40 .. 0x1BF`` that
 * ``ra8_epd_cal.h`` already reserves for exactly this record, clear of the
 * DFU anti-rollback counter at offset 0 and the standalone VCOM blob at
 * 0x200. The production binding is ::ra8_devcfg_default_store; host tests
 * inject their own ::ra8_devcfg_store_t.
 *
 * @note The window is one-time-programmable on this silicon -- there is no
 *       rewritable data-flash to erase and re-use (HUM Ch 59.7.4.5). The
 *       two-copy scheme's primary job here is torn-write atomicity; each
 *       commit programs a fresh copy slot, so re-provisioning is bounded by
 *       the reserved slot count. Extending that ring is a
 *       ::k_ra8_devcfg_slot_bytes schema decision, not a redesign.
 *
 * ## Boot resolution order
 *
 * ``ra8_devcfg_load`` probes both copies and applies, in order:
 *  1. Validate copy 0 (magic, schema known, ``record_len`` sane, CRC-32).
 *  2. Validate copy 1.
 *  3. Both valid -> the higher ``seq`` wins (a tie takes copy 0). One valid
 *     -> use it. Neither -> **UNPROVISIONED**.
 *  4. UNPROVISIONED is not papered over: ``ra8_devcfg_get_vcom_mv`` refuses,
 *     ``ra8_devcfg_is_blank`` reports true, and the caller must not drive the
 *     panel (INV-VCOM-1). The rest of the system boots normally.
 *
 * There is deliberately **no compile-time production fallback for VCOM** -- a
 * build-time default is exactly the plausible-looking wrong value that damages
 * panels cumulatively and irreversibly.
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

/**
 * @enum ra8_devcfg_layout_t
 * @brief Record geometry, magic, schema version and extra-MRAM placement.
 *
 * @details
 * Placement offsets (``copy0/copy1``) are relative to
 * ``k_ra8_flash_extra_start``. The active record is
 * ::k_ra8_devcfg_record_len bytes (header + body); each copy occupies a
 * ::k_ra8_devcfg_slot_bytes slot, whose ::k_ra8_devcfg_sig_headroom tail is
 * reserved for a future factory signature under Root-of-Trust.
 *
 * @invariant ``copy1_off - copy0_off >= slot_bytes`` (copies never overlap).
 * @invariant Both slots end below the standalone VCOM blob at 0x200 and the
 *            OTP guard boundary ``k_ra8_flash_extra_locked_start``.
 *
 * @see ra8_devcfg_default_store
 */
typedef enum : uint32_t {
  k_ra8_devcfg_magic        = 0x52413843U, /**< "RA8C" record marker.            */
  k_ra8_devcfg_schema_ver   = 1U,          /**< Current schema version.          */
  k_ra8_devcfg_hdr_bytes    = 32U,         /**< Header page (written LAST).      */
  k_ra8_devcfg_body_bytes   = 96U,         /**< Per-unit payload bytes.          */
  k_ra8_devcfg_record_len   = 128U,        /**< Active record = header + body.   */
  k_ra8_devcfg_slot_bytes   = 192U,        /**< Reserved slot pitch per copy.    */
  k_ra8_devcfg_sig_headroom = 64U,         /**< slot - record: future signature. */
  k_ra8_devcfg_page_bytes   = 32U,         /**< Extra-MRAM program page.         */
  k_ra8_devcfg_copy0_off    = 0x00000040U, /**< Copy 0 offset in extra-MRAM.     */
  k_ra8_devcfg_copy1_off    = 0x00000100U, /**< Copy 1 offset in extra-MRAM.     */
} ra8_devcfg_layout_t;

/**
 * @enum ra8_devcfg_field_len_t
 * @brief Fixed byte lengths of the variable-width body fields.
 */
typedef enum : uint8_t {
  k_ra8_devcfg_serial_len       = 16U, /**< Unit serial, NUL-padded.     */
  k_ra8_devcfg_panel_serial_len = 12U, /**< Panel identity, NUL-padded.  */
  k_ra8_devcfg_panel_lut_len    = 8U,  /**< Panel LUT id, e.g. "M641".   */
  k_ra8_devcfg_touch_cal_len    = 36U, /**< ``ra8_touch_cal_save`` blob. */
  k_ra8_devcfg_hdr_rsvd_len     = 12U, /**< Header reserved padding.     */
  k_ra8_devcfg_body_rsvd_len    = 10U, /**< Body reserved padding.       */
} ra8_devcfg_field_len_t;

/**
 * @enum ra8_devcfg_off_t
 * @brief Byte offset of every field inside a serialised record.
 *
 * @details
 * The record is packed byte-at-a-time in an explicit little-endian order so a
 * blob written on the host deserialises identically on the target -- the
 * on-storage format is a property of the record, not of the compiler. The
 * header occupies ``[0, hdr_bytes)`` and is programmed LAST; the CRC-32 covers
 * the body span ``[hdr_bytes, record_len)`` only.
 */
typedef enum : uint8_t {
  k_ra8_devcfg_off_magic        = 0U,   /**< u32 magic.                     */
  k_ra8_devcfg_off_schema       = 4U,   /**< u16 schema_version.            */
  k_ra8_devcfg_off_reclen       = 6U,   /**< u16 record_len.                */
  k_ra8_devcfg_off_seq          = 8U,   /**< u32 monotonic sequence.        */
  k_ra8_devcfg_off_crc          = 12U,  /**< u32 CRC-32 of the body.        */
  k_ra8_devcfg_off_flags        = 16U,  /**< u32 ::ra8_devcfg_flags_t.      */
  k_ra8_devcfg_off_hdr_rsvd     = 20U,  /**< 12 reserved header bytes.      */
  k_ra8_devcfg_off_serial       = 32U,  /**< char[16] serial (body start).  */
  k_ra8_devcfg_off_panel_serial = 48U,  /**< char[12] panel serial.         */
  k_ra8_devcfg_off_panel_lut    = 60U,  /**< char[8] panel LUT id.          */
  k_ra8_devcfg_off_touch_cal    = 68U,  /**< uint8[36] touch-cal blob.      */
  k_ra8_devcfg_off_mfg_date     = 104U, /**< u32 packed YYYYMMDD.           */
  k_ra8_devcfg_off_key_id       = 108U, /**< u32 device-key identifier.     */
  k_ra8_devcfg_off_hw_rev       = 112U, /**< u16 board revision.            */
  k_ra8_devcfg_off_fixture      = 114U, /**< u16 provisioning fixture id.   */
  k_ra8_devcfg_off_vcom         = 116U, /**< u16 panel VCOM magnitude (mV). */
  k_ra8_devcfg_off_body_rsvd    = 118U, /**< 10 reserved body bytes.        */
} ra8_devcfg_off_t;

/**
 * @enum ra8_devcfg_vcom_range_t
 * @brief Plausible-range guard for the stored VCOM magnitude, millivolts.
 *
 * @details
 * A secondary sanity guard behind the explicit ::k_ra8_devcfg_flag_vcom_valid
 * bit: it rejects the two real-world poison values 0 (controller still
 * booting) and 0xFFFF (blank / failed read) and any wildly out-of-band number.
 * The window brackets the typical e-paper VCOM span (approximately -0.5 V to
 * -4.0 V). These bounds are an extensible default, not a panel datasheet
 * value; a panel-specific window belongs in the BSP descriptor.
 *
 * @invariant ``vcom_min_mv`` is non-zero, so 0 can never be in range.
 * @invariant ``vcom_min_mv <= vcom_max_mv``.
 */
typedef enum : uint16_t {
  k_ra8_devcfg_vcom_min_mv = 500U,  /**< Lowest accepted VCOM magnitude, mV.  */
  k_ra8_devcfg_vcom_max_mv = 4000U, /**< Highest accepted VCOM magnitude, mV. */
} ra8_devcfg_vcom_range_t;

/**
 * @enum ra8_devcfg_flags_t
 * @brief Per-record status flags stored in the header ``flags`` word.
 *
 * @details
 * ::k_ra8_devcfg_flag_vcom_valid is an EXPLICIT validity bit rather than a
 * sentinel value. "Zero means unset" is exactly how a plausible-looking wrong
 * VCOM gets written, and a wrong VCOM degrades the panel cumulatively and
 * irreversibly, so the validity of the VCOM field is stated, never inferred.
 */
typedef enum : uint32_t {
  k_ra8_devcfg_flag_provisioned = 0x00000001U, /**< Line self-test passed.      */
  k_ra8_devcfg_flag_vcom_valid  = 0x00000002U, /**< ``panel_vcom_mv`` is real.  */
  k_ra8_devcfg_flag_touch_valid = 0x00000004U, /**< ``touch_cal`` blob is real. */
} ra8_devcfg_flags_t;

/**
 * @struct ra8_devcfg_body_t
 * @brief Decoded per-unit payload (the 96-byte record body).
 *
 * @details
 * The in-memory form of the serialised body. ``touch_cal`` carries the exact
 * 36-byte blob produced by ``ra8_touch_cal_save`` -- this record is that
 * blob's durable home.
 *
 * @note ``device_key_id`` is an IDENTIFIER ONLY, never key material: there is
 *       no working key-wrapping engine on this silicon and the debug port is
 *       open (#244), so a private key here would be plaintext to anyone with
 *       physical access. Adding real key storage later is a ``schema_version``
 *       bump, not a redesign.
 *
 * @invariant ``panel_vcom_mv`` is a VCOM **magnitude** in millivolts; VCOM is
 *            negative and the sign is implicit (2300 => -2.30 V), matching the
 *            IT8951 ``0x0039`` command encoding.
 *
 * @see ra8_devcfg_get_body
 */
typedef struct {
  char     serial[k_ra8_devcfg_serial_len];             /**< Unit serial, NUL-padded.        */
  char     panel_serial[k_ra8_devcfg_panel_serial_len]; /**< Panel identity, NUL-padded.     */
  char     panel_lut_id[k_ra8_devcfg_panel_lut_len];    /**< Panel LUT id from GET_DEV_INFO. */
  uint8_t  touch_cal[k_ra8_devcfg_touch_cal_len];       /**< ra8_touch_cal_save() blob.      */
  uint32_t mfg_date;                                    /**< Packed YYYYMMDD.                */
  uint32_t device_key_id;                               /**< Key identifier; NEVER key data. */
  uint16_t hw_rev;                                      /**< Board revision.                 */
  uint16_t fixture_id;                                  /**< Provisioning-line fixture id.   */
  uint16_t panel_vcom_mv;                               /**< VCOM magnitude, mV (sign -ve).  */
} ra8_devcfg_body_t;

/**
 * @struct ra8_devcfg_record_t
 * @brief Decoded record: the body plus the header discriminators.
 *
 * @details
 * The resolved output of ``ra8_devcfg_load`` and the input a provisioning
 * writer hands ``ra8_devcfg_commit``. ``seq`` and ``schema_version`` come from
 * the header; ``flags`` is the header ``flags`` word.
 *
 * @invariant ``schema_version <= k_ra8_devcfg_schema_ver`` for an accepted
 *            record.
 *
 * @see ra8_devcfg_load
 * @see ra8_devcfg_commit
 */
typedef struct {
  ra8_devcfg_body_t body;           /**< Per-unit payload.                    */
  uint32_t          flags;          /**< ::ra8_devcfg_flags_t bit-set.        */
  uint32_t          seq;            /**< Monotonic sequence of this record.   */
  uint16_t          schema_version; /**< Schema the record was written under. */
} ra8_devcfg_record_t;

/**
 * @typedef ra8_devcfg_read_fn_t
 * @brief Backing-store read (dependency-injection seam).
 *
 * @details
 * Reads ``len`` bytes at ``offset`` (relative to the devcfg region base) into
 * ``dst``. On silicon the base is ``k_ra8_flash_extra_start``; the read must
 * tolerate a blank (never-programmed) window, which reads back as 0xFF bytes.
 * Any non-``k_ra8_ok`` return makes the resolver treat that copy as absent and
 * fall through -- it never fabricates a record.
 *
 * @param[in]  offset Byte offset into the devcfg region.
 * @param[out] dst    Destination buffer; non-NULL, at least ``len`` bytes.
 * @param[in]  len    Bytes to read.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           ``dst`` holds ``len`` bytes.
 * @retval k_ra8_err_null_ptr ``dst`` was NULL.
 * @retval other              Backing-store fault; the copy is treated absent.
 *
 * @note Thread safety is the implementation's responsibility; the boot-path
 *       caller is single-threaded.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_devcfg_read_fn_t)(uint32_t offset, uint8_t* dst, uint32_t len);

/**
 * @typedef ra8_devcfg_write_fn_t
 * @brief Backing-store write (dependency-injection seam).
 *
 * @details
 * Programs ``len`` bytes at ``offset``. The caller (``ra8_devcfg_commit``)
 * honours the header-last commit order -- body bytes first, then the header
 * page -- so a torn write leaves the header invalid and the other copy wins.
 * The implementation chunks ``len`` into ::k_ra8_devcfg_page_bytes program
 * pages as its backing requires.
 *
 * @param[in] offset Byte offset into the devcfg region.
 * @param[in] src    Source buffer; non-NULL, at least ``len`` bytes.
 * @param[in] len    Bytes to write.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           ``len`` bytes committed durably.
 * @retval k_ra8_err_null_ptr ``src`` was NULL.
 * @retval other              Program fault; the caller must not assume persistence.
 *
 * @note Thread safety is the implementation's responsibility.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_devcfg_write_fn_t)(uint32_t offset, const uint8_t* src, uint32_t len);

/**
 * @struct ra8_devcfg_store_t
 * @brief Dependency-injection vtable for the record backing store.
 *
 * @details
 * The single seam between the hardware-agnostic record logic and the durable
 * medium (NASA Power-of-10 Rule 9 deviation: function pointers enable
 * Dependency Inversion and host mock injection). Production binds
 * ::ra8_devcfg_default_store (extra-MRAM); host tests bind a RAM mock. Both
 * members must be non-NULL for ``ra8_devcfg_load`` / ``ra8_devcfg_commit`` to
 * proceed. This mirrors the ``ra8_epd_cal_store_t`` and
 * ``ra8_rot_antirollback_store_t`` seams; callers reach the backing only
 * through this vtable, never by naming a backend symbol.
 *
 * @invariant ``read`` and ``write`` are either both bound to a real backing or
 *            both bound to a mock (never one of each).
 *
 * @par Example:
 * @code
 * const ra8_devcfg_store_t* store = ra8_devcfg_default_store();
 * if (ra8_devcfg_load(store) == k_ra8_ok) {
 *   uint16_t mv = 0U;
 *   if (ra8_devcfg_get_vcom_mv(&mv) == k_ra8_ok) { drive_panel(mv); }
 * }
 * @endcode
 *
 * @see ra8_devcfg_load
 * @see ra8_devcfg_default_store
 */
typedef struct {
  ra8_devcfg_read_fn_t  read;  /**< Backing read; non-NULL.  */
  ra8_devcfg_write_fn_t write; /**< Backing write; non-NULL. */
} ra8_devcfg_store_t;

/**
 * @brief Load and resolve the device configuration record from both copies.
 *
 * @details
 * Probes copy 0 and copy 1 through ``store``; validates each (magic, schema
 * known, ``record_len`` sane, CRC-32); selects the valid copy, or the higher
 * ``seq`` when both are valid; and caches the winner for
 * ``ra8_devcfg_get_vcom_mv`` / ``ra8_devcfg_get_body`` / ``ra8_devcfg_is_blank``.
 * A per-copy read fault or blank window marks that copy absent rather than
 * failing the load, so a fresh unit resolves cleanly to UNPROVISIONED. This
 * call is read-only: it never programs the backing.
 *
 * @param[in] store Backing-store vtable; non-NULL, both members non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    A valid record is loaded and cached.
 * @retval k_ra8_err_null_ptr          ``store`` or a member is NULL.
 * @retval k_ra8_err_validation_failed Neither copy is valid -- the unit is
 *                                     UNPROVISIONED. Not a soft failure: the
 *                                     panel must not be driven and provisioning
 *                                     mode becomes reachable.
 *
 * @pre ``store->read`` is callable for the duration of the call.
 * @pre The boot path is single-threaded.
 * @post On ``k_ra8_ok`` the cached state is "loaded" and the record is decoded.
 * @post On ``k_ra8_err_validation_failed`` the cached state is "unprovisioned".
 *
 * @note Not thread-safe: mutates module-static cache state. Call from the
 *       single-threaded boot path.
 * @see ra8_devcfg_get_vcom_mv
 * @see ra8_devcfg_commit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_devcfg_load(const ra8_devcfg_store_t* store);

/**
 * @brief Fetch the validated panel VCOM magnitude in millivolts.
 *
 * @details
 * The entry point the e-paper driver uses. It succeeds only when a record
 * loaded cleanly, ::k_ra8_devcfg_flag_vcom_valid is set, and the stored value
 * lies inside ``[k_ra8_devcfg_vcom_min_mv, k_ra8_devcfg_vcom_max_mv]``. Any
 * error means INV-VCOM-1 forbids driving the panel -- leave the rail off and
 * the controller in reset. Refusing to draw is recoverable; a wrong VCOM is
 * not, because panel damage accumulates with time under bias.
 *
 * @param[out] out_mv Receives the magnitude (sign is implicitly negative).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Value validated and safe to program.
 * @retval k_ra8_err_null_ptr          ``out_mv`` is NULL.
 * @retval k_ra8_err_not_initialized   ``ra8_devcfg_load`` has not succeeded.
 * @retval k_ra8_err_validation_failed No valid VCOM: unprovisioned, the valid
 *                                     flag is clear, or the value is out of
 *                                     range. **Refuse the panel.**
 *
 * @pre ``out_mv`` is a writable ``uint16_t``.
 * @pre ``ra8_devcfg_load`` was called on this boot.
 * @post On ``k_ra8_ok`` ``*out_mv`` is in the plausible range.
 * @post On any error ``*out_mv`` is left unchanged.
 *
 * @note Thread-safe for concurrent readers once ``ra8_devcfg_load`` completed
 *       (reads immutable cache); not safe against a concurrent load.
 * @see ra8_devcfg_load
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_devcfg_get_vcom_mv(uint16_t* out_mv);

/**
 * @brief Expose the decoded body of the loaded record.
 *
 * @details
 * Gives consumers read access to the per-unit fields (serial, touch
 * calibration, panel identity) after a successful ``ra8_devcfg_load``. The
 * returned pointer aliases module-static cache storage and stays valid until
 * the next ``ra8_devcfg_load`` / ``ra8_devcfg_reset``; the caller must not
 * modify or free it.
 *
 * @param[out] out_body Receives a pointer to the cached body; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  ``*out_body`` points at the cached body.
 * @retval k_ra8_err_null_ptr        ``out_body`` is NULL.
 * @retval k_ra8_err_not_initialized No record is loaded (never loaded or
 *                                   UNPROVISIONED).
 *
 * @pre ``out_body`` is a writable pointer slot.
 * @pre ``ra8_devcfg_load`` returned ``k_ra8_ok`` this boot.
 * @post On ``k_ra8_ok`` ``*out_body`` is non-NULL and read-only.
 * @post On any error ``*out_body`` is left unchanged.
 *
 * @note Thread-safe for readers once loaded; not safe against a concurrent load.
 * @see ra8_devcfg_load
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_devcfg_get_body(const ra8_devcfg_body_t** out_body);

/**
 * @brief Report whether neither record copy is valid (the provisioning gate).
 *
 * @details
 * True exactly when the most recent ``ra8_devcfg_load`` found neither copy
 * valid (UNPROVISIONED), and also before any load has run. One half of the
 * provisioning-mode entry condition; the other half (a factory token) lives
 * with the writer (#317). Blank-alone is not a sufficient gate while the debug
 * port is open (#244).
 *
 * @return bool True iff the unit is UNPROVISIONED (or load never ran).
 * @retval true  Both copies were invalid, or ``ra8_devcfg_load`` never ran.
 * @retval false A valid record is loaded.
 *
 * @pre None (safe to call before ``ra8_devcfg_load``; reports true).
 * @post No state is mutated.
 *
 * @note Thread-safe for readers once loaded.
 * @see ra8_devcfg_load
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_devcfg_is_blank(void);

/**
 * @brief Commit a new record to the stale copy slot with header-last ordering.
 *
 * @details
 * The provisioning-writer primitive (consumed by #317). Serialises ``rec`` (its
 * body, ``flags`` and a ``seq`` one past the newest existing copy), then
 * programs the target slot -- the older / invalid copy, so the newest good
 * record is never overwritten -- body bytes first and the header page LAST. A
 * power cut before the header lands leaves that slot header-invalid, so the
 * other copy still resolves; there is never a window with zero valid records.
 * The caller must ``ra8_devcfg_load`` again to adopt the committed record.
 *
 * @param[in] store Backing-store vtable; non-NULL, both members non-NULL.
 * @param[in] rec   Record to persist (``rec->seq`` is ignored and recomputed);
 *                  non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Body and header programmed to the target slot.
 * @retval k_ra8_err_null_ptr ``store``, a member, or ``rec`` is NULL.
 * @retval other              A backing write faulted; persistence is not assured.
 *
 * @pre ``store->write`` is callable and targets storage outside both DFU banks.
 * @pre The boot / provisioning path is single-threaded.
 * @post On ``k_ra8_ok`` the target slot holds a CRC-valid record with a ``seq``
 *       strictly greater than every previously valid copy.
 * @post On error the target slot may be partially written; the other copy is
 *       untouched and still resolves.
 *
 * @note Not thread-safe: programs the backing store. Call from a single-threaded
 *       provisioning context.
 * @see ra8_devcfg_load
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_devcfg_commit(const ra8_devcfg_store_t*  store,
                                          const ra8_devcfg_record_t* rec);

/**
 * @brief Drop the cached record so a later load re-resolves from scratch.
 *
 * @details
 * Returns the module to the "never loaded" state. Used by the re-provisioning
 * flow (write a new record, then reset and reload) and by host tests that need
 * a pristine cache between cases. Touches no backing store.
 *
 * @return void
 *
 * @pre None -- safe to call at any time, including before the first load.
 * @pre The boot / provisioning path is single-threaded.
 * @post ``ra8_devcfg_is_blank`` reports true and ``ra8_devcfg_get_vcom_mv``
 *       reports ``k_ra8_err_not_initialized`` until the next successful load.
 * @post No backing store is accessed.
 *
 * @note Not thread-safe: mutates module-static cache state.
 * @see ra8_devcfg_load
 * @since 0.1.0
 */
void ra8_devcfg_reset(void);

/**
 * @brief Return the production extra-MRAM-backed store binding.
 *
 * @details
 * Wires ::ra8_devcfg_read_fn_t / ::ra8_devcfg_write_fn_t to the extra-MRAM
 * (data-flash) window: reads dereference ``k_ra8_flash_extra_start + offset``
 * (a blank word reads back as 0xFF and does not fault -- #315), writes go
 * through ``ra8_flash_extra_mram_write`` in ::k_ra8_devcfg_page_bytes pages
 * (HUM Ch 59.7.4.5 "Program Command" Table 59.15 p 3592). Under
 * ``RA8_SIMULATOR_MODE`` both members address a RAM shadow so host tests
 * exercise the same control flow without MMIO. The returned pointer has static
 * lifetime; the caller must not free it.
 *
 * @return Pointer to the process-lifetime default store; never NULL.
 * @retval non-NULL The extra-MRAM (or, under simulation, RAM-shadow) store.
 *
 * @pre On silicon, ``ra8_flash_init`` has run before a commit.
 * @post The same pointer is returned on every call.
 * @post Both members of the returned store are non-NULL.
 *
 * @note Thread-safe (returns a pointer to immutable static data).
 * @see ra8_devcfg_load
 * @see ra8_devcfg_commit
 * @since 0.1.0
 */
const ra8_devcfg_store_t* ra8_devcfg_default_store(void);

#ifdef __cplusplus
}
#endif
