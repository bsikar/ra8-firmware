/**
 * @file ra8_translate.h
 * @brief Freestanding per-chapter translation sidecar (`TRN1`) C ABI contract.
 * @ingroup grp_translate
 *
 * @details
 * Exposes the versioned binary chapter translation sidecar contract (`TRN1`),
 * keyed by the source chapter SHA-256 content digest and translation pipeline
 * model versions so chapter updates or model upgrades invalidate stale caches
 * deterministically with zero dynamic memory allocation.
 *
 * All public functions are `task-safe-reentrant`: they borrow immutable inputs
 * for the duration of the call, write only caller-owned output storage on
 * success, retain no caller pointers after return, and preserve incoming output
 * values on every non-success return.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_translate_ctc.h"
#include "ra8_translate_detector.h"
#include "ra8_translate_ocr.h"
#include "ra8_translate_overlay.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Wire constants and bounds for the `TRN1` translation sidecar format. */
typedef enum : uint32_t {
  k_ra8_translate_sidecar_magic            = 0x314E5254U, /**< Little-endian "TRN1".         */
  k_ra8_translate_sidecar_version          = 1U,          /**< Supported format revision.    */
  k_ra8_translate_sidecar_header_bytes     = 96U,         /**< Fixed wire header size.       */
  k_ra8_translate_sidecar_page_bytes       = 12U,         /**< Fixed wire PageEntry size.    */
  k_ra8_translate_sidecar_bubble_bytes     = 44U,         /**< Fixed wire BubbleRecord size. */
  k_ra8_translate_sidecar_max_page_bubbles = 1024U,       /**< Maximum bubbles on one page.  */
  k_ra8_translate_max_confidence_bps       = 10000U,      /**< 100.00% in integer basis pts. */
  k_ra8_translate_chapter_hash_bytes       = 32U,         /**< SHA-256 content key length.   */
  k_ra8_translate_glossary_magic           = 0x314C4754U, /**< Little-endian "TGL1".         */
  k_ra8_translate_glossary_version         = 1U,          /**< Supported glossary revision.  */
  k_ra8_translate_glossary_header_bytes    = 32U,         /**< Fixed wire TGL1 header size.  */
} ra8_translate_const_t;

/** @brief Translation source script / language identifier. */
typedef enum : uint8_t {
  k_ra8_translate_lang_unknown  = 0U, /**< Language unspecified or unknown. */
  k_ra8_translate_lang_korean   = 1U, /**< Korean (Hangul / mixed Hanja).   */
  k_ra8_translate_lang_japanese = 2U, /**< Japanese (Kana / Kanji).         */
  k_ra8_translate_lang_chinese  = 3U, /**< Chinese (Han script).            */
} ra8_translate_lang_t;

/** @brief Speech or narration bubble contour shape. */
typedef enum : uint8_t {
  k_ra8_translate_shape_rect         = 0U, /**< Axis-aligned rectangle.          */
  k_ra8_translate_shape_ellipse      = 1U, /**< Elliptical speech bubble.        */
  k_ra8_translate_shape_rounded_rect = 2U, /**< Rounded rectangle with radius.   */
  k_ra8_translate_shape_cloud        = 3U, /**< Scalloped / thought cloud shape. */
} ra8_translate_shape_t;

/** @brief Source OCR text orientation inside a detected bubble. */
typedef enum : uint8_t {
  k_ra8_translate_orient_horizontal  = 0U, /**< Left-to-right horizontal lines.  */
  k_ra8_translate_orient_vertical_rl = 1U, /**< Vertical columns, right-to-left. */
  k_ra8_translate_orient_vertical_lr = 2U, /**< Vertical columns, left-to-right. */
} ra8_translate_orientation_t;

/** @brief Bitmask flags for `ra8_translate_sidecar_bubble_t::bubble_flags`. */
typedef enum : uint8_t {
  k_ra8_translate_bflag_glossary = 1U << 0U, /**< Term matched in series glossary. */
} ra8_translate_bubble_flag_t;

static_assert(sizeof(ra8_translate_lang_t) == 1U, "ra8_translate_lang_t width");
static_assert(sizeof(ra8_translate_shape_t) == 1U, "ra8_translate_shape_t width");
static_assert(sizeof(ra8_translate_orientation_t) == 1U, "ra8_translate_orientation_t width");
static_assert(sizeof(ra8_translate_bubble_flag_t) == 1U, "ra8_translate_bubble_flag_t width");

/**
 * @brief Cache invalidation key binding a sidecar to a chapter and pipeline.
 * @details Any difference in chapter hash, model version, glossary revision, or
 * source language marks an existing sidecar stale. Reserved bytes must be zero.
 * @since 0.1.0
 */
typedef struct {
  uint8_t              chapter_hash[k_ra8_translate_chapter_hash_bytes]; /**< Chapter SHA-256. */
  uint32_t             detector_model_version;                           /**< Detector .npub.  */
  uint32_t             ocr_model_version;                                /**< OCR .npub.       */
  uint32_t             nmt_model_version;                                /**< Marian NMT ver.  */
  uint32_t             glossary_revision;                                /**< Series glossary. */
  ra8_translate_lang_t source_lang;                                      /**< Chapter script.  */
  uint8_t              reserved[3];                                      /**< Zero padding.    */
} ra8_translate_sidecar_key_t;

static_assert(sizeof(ra8_translate_sidecar_key_t) == 52U, "ra8_translate_sidecar_key_t size");
static_assert(alignof(ra8_translate_sidecar_key_t) == 4U, "ra8_translate_sidecar_key_t align");
static_assert(offsetof(ra8_translate_sidecar_key_t, chapter_hash) == 0U, "key chapter_hash off");
static_assert(offsetof(ra8_translate_sidecar_key_t, detector_model_version) == 32U,
              "key detector_model_version off");
static_assert(offsetof(ra8_translate_sidecar_key_t, ocr_model_version) == 36U,
              "key ocr_model_version off");
static_assert(offsetof(ra8_translate_sidecar_key_t, nmt_model_version) == 40U,
              "key nmt_model_version off");
static_assert(offsetof(ra8_translate_sidecar_key_t, glossary_revision) == 44U,
              "key glossary_revision off");
static_assert(offsetof(ra8_translate_sidecar_key_t, source_lang) == 48U, "key source_lang off");
static_assert(offsetof(ra8_translate_sidecar_key_t, reserved) == 49U, "key reserved off");

/**
 * @brief Validated header view opened from an immutable `TRN1` buffer.
 * @since 0.1.0
 */
typedef struct {
  uint32_t                    total_bytes;      /**< Exact byte length of container. */
  uint32_t                    file_crc32;       /**< IEEE CRC-32 stored in header.   */
  ra8_translate_sidecar_key_t key;              /**< Cache validation key.           */
  uint16_t                    page_count;       /**< Number of PageEntry records.    */
  uint16_t                    reserved;         /**< Zero padding.                   */
  uint32_t                    bubble_count;     /**< Number of BubbleRecord items.   */
  uint32_t                    line_break_count; /**< Total u16 line-break offsets.   */
  uint32_t                    text_pool_bytes;  /**< Byte extent of UTF-8 text pool. */
} ra8_translate_sidecar_header_t;

static_assert(sizeof(ra8_translate_sidecar_header_t) == 76U, "ra8_translate_sidecar_header_t size");
static_assert(alignof(ra8_translate_sidecar_header_t) == 4U,
              "ra8_translate_sidecar_header_t align");
static_assert(offsetof(ra8_translate_sidecar_header_t, total_bytes) == 0U,
              "header total_bytes off");
static_assert(offsetof(ra8_translate_sidecar_header_t, file_crc32) == 4U, "header file_crc32 off");
static_assert(offsetof(ra8_translate_sidecar_header_t, key) == 8U, "header key off");
static_assert(offsetof(ra8_translate_sidecar_header_t, page_count) == 60U, "header page_count off");
static_assert(offsetof(ra8_translate_sidecar_header_t, reserved) == 62U, "header reserved off");
static_assert(offsetof(ra8_translate_sidecar_header_t, bubble_count) == 64U,
              "header bubble_count off");
static_assert(offsetof(ra8_translate_sidecar_header_t, line_break_count) == 68U,
              "header line_break_count off");
static_assert(offsetof(ra8_translate_sidecar_header_t, text_pool_bytes) == 72U,
              "header text_pool_bytes off");

/**
 * @brief Per-page slice descriptor inside a `TRN1` sidecar.
 * @since 0.1.0
 */
typedef struct {
  uint16_t page_index;   /**< 0-based page index within the chapter. */
  uint16_t bubble_count; /**< Bubbles present on this page.          */
  uint32_t first_bubble; /**< Start index in chapter bubble array.   */
  uint16_t page_width;   /**< Page raster width in pixels.           */
  uint16_t page_height;  /**< Page raster height in pixels.          */
} ra8_translate_sidecar_page_t;

static_assert(sizeof(ra8_translate_sidecar_page_t) == 12U, "ra8_translate_sidecar_page_t size");
static_assert(alignof(ra8_translate_sidecar_page_t) == 4U, "ra8_translate_sidecar_page_t align");
static_assert(offsetof(ra8_translate_sidecar_page_t, page_index) == 0U, "page page_index off");
static_assert(offsetof(ra8_translate_sidecar_page_t, bubble_count) == 2U, "page bubble_count off");
static_assert(offsetof(ra8_translate_sidecar_page_t, first_bubble) == 4U, "page first_bubble off");
static_assert(offsetof(ra8_translate_sidecar_page_t, page_width) == 8U, "page page_width off");
static_assert(offsetof(ra8_translate_sidecar_page_t, page_height) == 10U, "page page_height off");

/**
 * @brief Metadata and table offsets for one speech or narration bubble.
 * @since 0.1.0
 */
typedef struct {
  uint16_t                    x;                  /**< Box left pixel coordinate.        */
  uint16_t                    y;                  /**< Box top pixel coordinate.         */
  uint16_t                    w;                  /**< Box width in pixels.              */
  uint16_t                    h;                  /**< Box height in pixels.             */
  ra8_translate_shape_t       shape;              /**< Bubble contour shape.             */
  ra8_translate_orientation_t text_orientation;   /**< OCR text orientation.             */
  uint16_t                    corner_radius;      /**< Radius for rounded_rect shapes.   */
  uint16_t                    reading_order;      /**< 0-based page reading order index. */
  uint16_t                    ocr_confidence_bps; /**< Confidence in [0, 10000].         */
  uint16_t                    font_size_px;       /**< Fitted English font size (px).    */
  uint16_t                    line_count;         /**< Number of fitted line breaks.     */
  uint32_t                    first_line_break;   /**< Index into line-break table.      */
  uint32_t                    source_text_off;    /**< Byte offset in UTF-8 text pool.   */
  uint32_t                    source_text_len;    /**< UTF-8 source text length (bytes). */
  uint32_t                    english_text_off;   /**< Byte offset in UTF-8 text pool.   */
  uint32_t                    english_text_len;   /**< UTF-8 English length (bytes).     */
  uint8_t                     bubble_flags;       /**< `ra8_translate_bubble_flag_t`.    */
  uint8_t                     reserved[3];        /**< Zero padding.                     */
} ra8_translate_sidecar_bubble_t;

static_assert(sizeof(ra8_translate_sidecar_bubble_t) == 44U, "ra8_translate_sidecar_bubble_t size");
static_assert(alignof(ra8_translate_sidecar_bubble_t) == 4U,
              "ra8_translate_sidecar_bubble_t align");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, x) == 0U, "bubble x off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, y) == 2U, "bubble y off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, w) == 4U, "bubble w off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, h) == 6U, "bubble h off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, shape) == 8U, "bubble shape off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, text_orientation) == 9U,
              "bubble text_orientation off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, corner_radius) == 10U,
              "bubble corner_radius off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, reading_order) == 12U,
              "bubble reading_order off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, ocr_confidence_bps) == 14U,
              "bubble ocr_confidence_bps off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, font_size_px) == 16U,
              "bubble font_size_px off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, line_count) == 18U, "bubble line_count off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, first_line_break) == 20U,
              "bubble first_line_break off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, source_text_off) == 24U,
              "bubble source_text_off off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, source_text_len) == 28U,
              "bubble source_text_len off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, english_text_off) == 32U,
              "bubble english_text_off off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, english_text_len) == 36U,
              "bubble english_text_len off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, bubble_flags) == 40U,
              "bubble bubble_flags off");
static_assert(offsetof(ra8_translate_sidecar_bubble_t, reserved) == 41U, "bubble reserved off");

/**
 * @brief Validate a `TRN1` sidecar buffer and decode its header.
 *
 * @details Calling context: `task-safe-reentrant`. Borrows @p bytes for the
 * duration of the call and writes caller-owned @p out_header only on success.
 * Null @p out_header or non-empty null @p bytes returns `k_ra8_err_null_ptr`;
 * length mismatches return `k_ra8_err_invalid_size`; CRC mismatches return
 * `k_ra8_err_crc_mismatch`; unsupported format version returns
 * `k_ra8_err_not_supported`; malformed tables return `k_ra8_err_invalid_arg`.
 *
 * @param[in] bytes Immutable serialized `TRN1` byte buffer (may be NULL only if @p len == 0).
 * @param[in] len Byte length of @p bytes.
 * @param[out] out_header Non-null caller storage receiving validated header fields.
 * @return ::k_ra8_ok on valid container, or explicit `ra8_err_t` failure code.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_open(const uint8_t*                  bytes,
                                     size_t                          len,
                                     ra8_translate_sidecar_header_t* out_header);

/**
 * @brief Check whether a validated `TRN1` header matches an expected cache key.
 *
 * @details Calling context: `task-safe-reentrant`. Borrows both structs for the
 * call and writes @p out_matches only when inputs are valid. Returns
 * `k_ra8_err_invalid_arg` if @p header or @p expected has non-zero reserved
 * bytes, out-of-range language enum, or zero model version.
 *
 * @param[in] header Non-null validated header from ::ra8_translate_sidecar_open.
 * @param[in] expected Non-null key describing current chapter bytes and models.
 * @param[out] out_matches Non-null caller boolean receiving key-match status.
 * @return ::k_ra8_ok on comparison completion, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_key_matches(const ra8_translate_sidecar_header_t* header,
                                            const ra8_translate_sidecar_key_t*    expected,
                                            bool*                                 out_matches);

/**
 * @brief Read one validated page descriptor by index.
 *
 * @details Calling context: `task-safe-reentrant`. Validates @p bytes and writes
 * @p out_page only on success. Returns `k_ra8_err_range_check_failed` when
 * @p page_index is out of bounds.
 *
 * @param[in] bytes Validated `TRN1` byte buffer.
 * @param[in] len Byte length of @p bytes.
 * @param[in] page_index Page index in `[0, header.page_count)`.
 * @param[out] out_page Non-null caller storage receiving the page descriptor.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_page_at(const uint8_t*                bytes,
                                        size_t                        len,
                                        uint16_t                      page_index,
                                        ra8_translate_sidecar_page_t* out_page);

/**
 * @brief Read one validated bubble record by chapter-global index.
 *
 * @details Calling context: `task-safe-reentrant`. Validates @p bytes and writes
 * @p out_bubble only on success. Returns `k_ra8_err_range_check_failed` when
 * @p bubble_index is out of bounds.
 *
 * @param[in] bytes Validated `TRN1` byte buffer.
 * @param[in] len Byte length of @p bytes.
 * @param[in] bubble_index Bubble index in `[0, header.bubble_count)`.
 * @param[out] out_bubble Non-null caller storage receiving the bubble metadata.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_bubble_at(const uint8_t*                  bytes,
                                          size_t                          len,
                                          uint32_t                        bubble_index,
                                          ra8_translate_sidecar_bubble_t* out_bubble);

/**
 * @brief Borrow a validated bubble's UTF-8 source text span inside @p bytes.
 *
 * @details Calling context: `task-safe-reentrant`. Returned @p out_ptr borrows
 * directly from @p bytes and remains valid only while @p bytes lives; an empty
 * string returns a non-null pointer within @p bytes text pool with `*out_len == 0`.
 *
 * @param[in] bytes Validated `TRN1` byte buffer.
 * @param[in] len Byte length of @p bytes.
 * @param[in] bubble_index Bubble index in `[0, header.bubble_count)`.
 * @param[out] out_ptr Non-null out-pointer receiving span base inside @p bytes.
 * @param[out] out_len Non-null out-length receiving UTF-8 byte count.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_source_text(const uint8_t*  bytes,
                                            size_t          len,
                                            uint32_t        bubble_index,
                                            const uint8_t** out_ptr,
                                            uint32_t*       out_len);

/**
 * @brief Borrow a validated bubble's UTF-8 English text span inside @p bytes.
 *
 * @details Calling context: `task-safe-reentrant`. Returned @p out_ptr borrows
 * directly from @p bytes and remains valid only while @p bytes lives.
 *
 * @param[in] bytes Validated `TRN1` byte buffer.
 * @param[in] len Byte length of @p bytes.
 * @param[in] bubble_index Bubble index in `[0, header.bubble_count)`.
 * @param[out] out_ptr Non-null out-pointer receiving span base inside @p bytes.
 * @param[out] out_len Non-null out-length receiving UTF-8 byte count.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_english_text(const uint8_t*  bytes,
                                             size_t          len,
                                             uint32_t        bubble_index,
                                             const uint8_t** out_ptr,
                                             uint32_t*       out_len);

/**
 * @brief Copy a validated bubble's fitted line-break offsets into caller storage.
 *
 * @details Calling context: `task-safe-reentrant`. Writes neither @p out_breaks
 * nor @p out_count on failure. A null @p out_breaks returns `k_ra8_err_null_ptr`.
 * Insufficient @p break_cap returns `k_ra8_err_invalid_size`.
 *
 * @param[in] bytes Validated `TRN1` byte buffer.
 * @param[in] len Byte length of @p bytes.
 * @param[in] bubble_index Bubble index in `[0, header.bubble_count)`.
 * @param[out] out_breaks Non-null caller array receiving cumulative end byte offsets.
 * @param[in] break_cap Element capacity of @p out_breaks.
 * @param[out] out_count Non-null storage receiving copied line-break count.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_line_breaks(const uint8_t* bytes,
                                            size_t         len,
                                            uint32_t       bubble_index,
                                            uint16_t*      out_breaks,
                                            uint16_t       break_cap,
                                            uint16_t*      out_count);

/**
 * @brief Compute the exact byte length needed to serialize a chapter sidecar.
 *
 * @details Calling context: `task-safe-reentrant`. Checks section-size arithmetic
 * without overflow and writes @p out_bytes only on success.
 *
 * @param[in] page_count Number of pages.
 * @param[in] bubble_count Total number of bubbles.
 * @param[in] line_break_count Total number of `uint16_t` line-break entries.
 * @param[in] text_pool_bytes Byte length of concatenated UTF-8 text pool.
 * @param[out] out_bytes Non-null storage receiving exact serialized byte count.
 * @return ::k_ra8_ok on success, `k_ra8_err_invalid_size` on `uint32_t` / `size_t` overflow.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_required_bytes(uint16_t page_count,
                                               uint32_t bubble_count,
                                               uint32_t line_break_count,
                                               uint32_t text_pool_bytes,
                                               size_t*  out_bytes);

/**
 * @brief Serialize and CRC-stamp a chapter translation sidecar into @p out_buf.
 *
 * @details Calling context: `task-safe-reentrant`. Validates all arguments and
 * sidecar invariants before writing any byte to @p out_buf or @p out_written.
 * Input slice pointers (@p pages, @p bubbles, @p line_breaks, @p text_pool) may
 * be NULL only when their corresponding element or byte count is zero.
 * Insufficient @p out_cap returns `k_ra8_err_invalid_size`.
 *
 * @param[in] key Non-null cache invalidation key.
 * @param[in] pages Page descriptors (`page_count` entries).
 * @param[in] page_count Number of pages.
 * @param[in] bubbles Bubble descriptors (`bubble_count` entries).
 * @param[in] bubble_count Total number of bubbles.
 * @param[in] line_breaks Cumulative line-break end offsets (`line_break_count` entries).
 * @param[in] line_break_count Total entries in @p line_breaks.
 * @param[in] text_pool UTF-8 text pool bytes (`text_pool_bytes` bytes).
 * @param[in] text_pool_bytes Byte extent of @p text_pool.
 * @param[out] out_buf Non-null caller output buffer receiving serialized bytes.
 * @param[in] out_cap Capacity of @p out_buf in bytes.
 * @param[out] out_written Non-null storage receiving exact byte count written.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_sidecar_write(const ra8_translate_sidecar_key_t*    key,
                                      const ra8_translate_sidecar_page_t*   pages,
                                      uint16_t                              page_count,
                                      const ra8_translate_sidecar_bubble_t* bubbles,
                                      uint32_t                              bubble_count,
                                      const uint16_t*                       line_breaks,
                                      uint32_t                              line_break_count,
                                      const uint8_t*                        text_pool,
                                      uint32_t                              text_pool_bytes,
                                      uint8_t*                              out_buf,
                                      size_t                                out_cap,
                                      size_t*                               out_written);

/** @brief Decision reason emitted by ::ra8_translate_lang_select. */
typedef enum : uint8_t {
  k_ra8_translate_lang_reason_unknown          = 0U, /**< No distinguishing script and no hint. */
  k_ra8_translate_lang_reason_script_confirmed = 1U, /**< Script matches per-title hint.        */
  k_ra8_translate_lang_reason_deferred_to_hint = 2U, /**< Kanji-only / neutral text used hint.  */
  k_ra8_translate_lang_reason_script_override  = 3U, /**< Script overrode empty/other hint.     */
} ra8_translate_lang_reason_t;

/** @brief OCR recognizer profile bound to a resolved source language. */
typedef enum : uint8_t {
  k_ra8_translate_ocr_none     = 0U, /**< No recognizer resolved yet. */
  k_ra8_translate_ocr_korean   = 1U, /**< Korean OCR recognizer.      */
  k_ra8_translate_ocr_japanese = 2U, /**< Japanese OCR recognizer.    */
  k_ra8_translate_ocr_chinese  = 3U, /**< Chinese OCR recognizer.     */
} ra8_translate_ocr_model_t;

/** @brief NMT translator profile bound to a resolved source language. */
typedef enum : uint8_t {
  k_ra8_translate_nmt_none  = 0U, /**< No NMT model resolved yet. */
  k_ra8_translate_nmt_ko_en = 1U, /**< Korean-to-English NMT.     */
  k_ra8_translate_nmt_ja_en = 2U, /**< Japanese-to-English NMT.   */
  k_ra8_translate_nmt_zh_en = 3U, /**< Chinese-to-English NMT.    */
} ra8_translate_nmt_model_t;

static_assert(sizeof(ra8_translate_lang_reason_t) == 1U, "ra8_translate_lang_reason_t width");
static_assert(sizeof(ra8_translate_ocr_model_t) == 1U, "ra8_translate_ocr_model_t width");
static_assert(sizeof(ra8_translate_nmt_model_t) == 1U, "ra8_translate_nmt_model_t width");

/**
 * @brief Codepoint counts across Unicode CJK script buckets for one OCR sample.
 * @since 0.1.0
 */
typedef struct {
  uint32_t hangul_count; /**< Hangul syllables and Jamo codepoints.        */
  uint32_t kana_count;   /**< Hiragana, Katakana, and halfwidth Kana runs. */
  uint32_t han_count;    /**< Shared CJK Kanji / Hanzi / Hanja ideographs. */
  uint32_t other_count;  /**< Punctuation, digits, Latin, and symbols.     */
} ra8_translate_script_counts_t;

static_assert(sizeof(ra8_translate_script_counts_t) == 16U, "ra8_translate_script_counts_t size");
static_assert(alignof(ra8_translate_script_counts_t) == 4U, "ra8_translate_script_counts_t align");
static_assert(offsetof(ra8_translate_script_counts_t, hangul_count) == 0U,
              "counts hangul_count off");
static_assert(offsetof(ra8_translate_script_counts_t, kana_count) == 4U, "counts kana_count off");
static_assert(offsetof(ra8_translate_script_counts_t, han_count) == 8U, "counts han_count off");
static_assert(offsetof(ra8_translate_script_counts_t, other_count) == 12U,
              "counts other_count off");

/**
 * @brief Resolved translation language, reason, pipeline profile, and script counts.
 * @since 0.1.0
 */
typedef struct {
  ra8_translate_lang_t          language;    /**< Selected chapter source language. */
  ra8_translate_lang_reason_t   reason;      /**< Selection decision rationale.     */
  ra8_translate_ocr_model_t     ocr_model;   /**< Recognizer profile for title.     */
  ra8_translate_nmt_model_t     nmt_model;   /**< Translator profile for title.     */
  uint8_t                       reserved[4]; /**< Zero padding.                     */
  ra8_translate_script_counts_t counts;      /**< Per-script sample counts.         */
} ra8_translate_lang_selection_t;

static_assert(sizeof(ra8_translate_lang_selection_t) == 24U, "ra8_translate_lang_selection_t size");
static_assert(alignof(ra8_translate_lang_selection_t) == 4U,
              "ra8_translate_lang_selection_t align");
static_assert(offsetof(ra8_translate_lang_selection_t, language) == 0U, "sel language off");
static_assert(offsetof(ra8_translate_lang_selection_t, reason) == 1U, "sel reason off");
static_assert(offsetof(ra8_translate_lang_selection_t, ocr_model) == 2U, "sel ocr_model off");
static_assert(offsetof(ra8_translate_lang_selection_t, nmt_model) == 3U, "sel nmt_model off");
static_assert(offsetof(ra8_translate_lang_selection_t, reserved) == 4U, "sel reserved off");
static_assert(offsetof(ra8_translate_lang_selection_t, counts) == 8U, "sel counts off");

/**
 * @brief Parse a per-title MDL/BCP-47 language metadata string into ::ra8_translate_lang_t.
 *
 * @details Calling context: `task-safe-reentrant`. Borrows @p hint for the
 * duration of the call and writes @p out_lang only on success.
 *
 * @param[in] hint Metadata language tag bytes (NULL allowed only if @p hint_len == 0).
 * @param[in] hint_len Byte length of @p hint.
 * @param[out] out_lang Non-null storage receiving parsed language enum.
 * @return ::k_ra8_ok on success, `k_ra8_err_null_ptr` on null pointer contract violation.
 * @since 0.1.0
 */
ra8_err_t
ra8_translate_lang_from_hint(const uint8_t* hint, size_t hint_len, ra8_translate_lang_t* out_lang);

/**
 * @brief Select source language and OCR/NMT profiles from hint plus OCR UTF-8 script ranges.
 *
 * @details Calling context: `task-safe-reentrant`. Validates @p hint enum,
 * UTF-8 well-formedness of @p sample_utf8, and output pointer before writing
 * @p out_selection. Kanji-only or script-neutral text defers to @p hint.
 *
 * @param[in] hint Per-title hint from MDL metadata.
 * @param[in] sample_utf8 UTF-8 sample OCR text (NULL allowed only if @p sample_len == 0).
 * @param[in] sample_len Byte length of @p sample_utf8.
 * @param[out] out_selection Non-null caller storage receiving language and model profile.
 * @return ::k_ra8_ok on success, `k_ra8_err_null_ptr`, or `k_ra8_err_invalid_arg` on invalid UTF-8
 *         or invalid @p hint discriminant.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_lang_select(ra8_translate_lang_t            hint,
                                    const uint8_t*                  sample_utf8,
                                    size_t                          sample_len,
                                    ra8_translate_lang_selection_t* out_selection);

/**
 * @brief Initialize an empty serialized `TGL1` per-series glossary container.
 *
 * @details Calling context: `task-safe-reentrant`. Writes CRC-stamped empty
 * container bytes into @p out_buf and writes @p out_len only on success.
 *
 * @param[in] lang Source script / language for the series glossary.
 * @param[in] revision Initial monotonic revision number.
 * @param[out] out_buf Non-null destination buffer receiving `TGL1` bytes.
 * @param[in] out_cap Capacity of @p out_buf in bytes (`>= 32`).
 * @param[out] out_len Non-null storage receiving bytes written.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_glossary_init(ra8_translate_lang_t lang,
                                      uint32_t             revision,
                                      uint8_t*             out_buf,
                                      size_t               out_cap,
                                      size_t*              out_len);

/**
 * @brief Insert or update a glossary entry and write the new `TGL1` container.
 *
 * @details Calling context: `task-safe-reentrant`. Validates @p tgl_buf, bumps
 * revision, and serializes updated CRC-stamped `TGL1` bytes into @p out_buf.
 * Caller storage @p out_buf must not alias @p tgl_buf.
 *
 * @param[in] tgl_buf Existing validated `TGL1` container bytes.
 * @param[in] tgl_len Byte length of @p tgl_buf.
 * @param[in] src_utf8 Non-empty UTF-8 source term bytes.
 * @param[in] src_len Byte length of @p src_utf8.
 * @param[in] eng_utf8 Non-empty UTF-8 English translation bytes.
 * @param[in] eng_len Byte length of @p eng_utf8.
 * @param[out] out_buf Non-null destination buffer for updated `TGL1`.
 * @param[in] out_cap Capacity of @p out_buf in bytes.
 * @param[out] out_len Non-null storage receiving bytes written.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_glossary_upsert(const uint8_t* tgl_buf,
                                        size_t         tgl_len,
                                        const uint8_t* src_utf8,
                                        size_t         src_len,
                                        const uint8_t* eng_utf8,
                                        size_t         eng_len,
                                        uint8_t*       out_buf,
                                        size_t         out_cap,
                                        size_t*        out_len);

/**
 * @brief Look up an exact UTF-8 source term in a `TGL1` container zero-copy.
 *
 * @details Calling context: `task-safe-reentrant`. Returned @p out_eng_ptr
 * borrows directly from @p tgl_buf while @p tgl_buf lives. Returns
 * `k_ra8_err_not_found` when absent without writing output pointers.
 *
 * @param[in] tgl_buf Validated `TGL1` container bytes.
 * @param[in] tgl_len Byte length of @p tgl_buf.
 * @param[in] src_utf8 UTF-8 source term to search.
 * @param[in] src_len Byte length of @p src_utf8.
 * @param[out] out_eng_ptr Non-null out-pointer receiving English term slice.
 * @param[out] out_eng_len Non-null storage receiving English byte length.
 * @return ::k_ra8_ok when present, `k_ra8_err_not_found` when missing, or error.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_glossary_lookup(const uint8_t*  tgl_buf,
                                        size_t          tgl_len,
                                        const uint8_t*  src_utf8,
                                        size_t          src_len,
                                        const uint8_t** out_eng_ptr,
                                        size_t*         out_eng_len);

/**
 * @brief Replace known glossary stems (and attached honorifics) with placeholders.
 *
 * @details Calling context: `task-safe-reentrant`. Emits UTF-8 masked source text
 * containing deterministic `__Gdd__` tokens and an opaque substitution plan into
 * caller buffers only when both output capacities suffice.
 *
 * @param[in] tgl_buf Validated `TGL1` container bytes.
 * @param[in] tgl_len Byte length of @p tgl_buf.
 * @param[in] src_utf8 UTF-8 input bubble text (NULL allowed only if @p src_len == 0).
 * @param[in] src_len Byte length of @p src_utf8.
 * @param[out] out_masked Non-null destination buffer for masked UTF-8 text.
 * @param[in] masked_cap Capacity of @p out_masked in bytes.
 * @param[out] out_masked_len Non-null storage receiving masked byte length.
 * @param[out] out_plan Non-null destination buffer for serialized plan bytes.
 * @param[in] plan_cap Capacity of @p out_plan in bytes.
 * @param[out] out_plan_len Non-null storage receiving serialized plan length.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_glossary_mask(const uint8_t* tgl_buf,
                                      size_t         tgl_len,
                                      const uint8_t* src_utf8,
                                      size_t         src_len,
                                      uint8_t*       out_masked,
                                      size_t         masked_cap,
                                      size_t*        out_masked_len,
                                      uint8_t*       out_plan,
                                      size_t         plan_cap,
                                      size_t*        out_plan_len);

/**
 * @brief Restore `__Gdd__` placeholders back to canonical English and honorific policy.
 *
 * @details Calling context: `task-safe-reentrant`. Restores every token in
 * @p eng_utf8 from @p tgl_buf and @p plan_bytes into @p out_restored without
 * mutating outputs on any error.
 *
 * @param[in] tgl_buf Validated `TGL1` container bytes.
 * @param[in] tgl_len Byte length of @p tgl_buf.
 * @param[in] plan_bytes Plan buffer produced by ::ra8_translate_glossary_mask.
 * @param[in] plan_len Byte length of @p plan_bytes.
 * @param[in] eng_utf8 UTF-8 decoded English containing `__Gdd__` placeholders.
 * @param[in] eng_len Byte length of @p eng_utf8.
 * @param[out] out_restored Non-null destination buffer for restored UTF-8 English.
 * @param[in] restored_cap Capacity of @p out_restored in bytes.
 * @param[out] out_restored_len Non-null storage receiving restored byte length.
 * @return ::k_ra8_ok on success, or explicit error status.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_glossary_restore(const uint8_t* tgl_buf,
                                         size_t         tgl_len,
                                         const uint8_t* plan_bytes,
                                         size_t         plan_len,
                                         const uint8_t* eng_utf8,
                                         size_t         eng_len,
                                         uint8_t*       out_restored,
                                         size_t         restored_cap,
                                         size_t*        out_restored_len);

/** @brief Tokenization model family stored in a `TVOC` container. */
typedef enum ra8_translate_tok_model_e : uint8_t {
  k_ra8_translate_tok_unigram = 0U,
  k_ra8_translate_tok_bpe     = 1U,
} ra8_translate_tok_model_t;

/** @brief SentencePiece vocabulary entry classification. */
typedef enum ra8_translate_tok_piece_kind_e : uint8_t {
  k_ra8_translate_tok_piece_normal       = 0U,
  k_ra8_translate_tok_piece_unknown      = 1U,
  k_ra8_translate_tok_piece_control      = 2U,
  k_ra8_translate_tok_piece_user_defined = 3U,
  k_ra8_translate_tok_piece_byte         = 4U,
  k_ra8_translate_tok_piece_unused       = 5U,
} ra8_translate_tok_piece_kind_t;

/** @brief Normalization and byte-fallback flags in a `TVOC` header. */
typedef enum ra8_translate_tok_flag_e : uint8_t {
  k_ra8_translate_tok_flag_add_dummy_prefix = 0x01U,
  k_ra8_translate_tok_flag_remove_extra_ws  = 0x02U,
  k_ra8_translate_tok_flag_escape_ws        = 0x04U,
  k_ra8_translate_tok_flag_byte_fallback    = 0x08U,
  k_ra8_translate_tok_flag_norm_fullwidth   = 0x10U,
} ra8_translate_tok_flag_t;

/** @brief Per-call encode control token flags. */
typedef enum ra8_translate_tok_enc_flag_e : uint8_t {
  k_ra8_translate_tok_enc_none    = 0x00U,
  k_ra8_translate_tok_enc_add_bos = 0x01U,
  k_ra8_translate_tok_enc_add_eos = 0x02U,
} ra8_translate_tok_enc_flag_t;

/** @brief Sentinel ID constants for `TVOC` special token slots. */
typedef enum ra8_translate_tok_const_e : uint32_t {
  k_ra8_translate_tok_id_none = 0xFFFFFFFFU,
} ra8_translate_tok_const_t;

static_assert(sizeof(ra8_translate_tok_model_t) == 1U, "ra8_translate_tok_model_t width");
static_assert(sizeof(ra8_translate_tok_piece_kind_t) == 1U, "ra8_translate_tok_piece_kind_t width");
static_assert(sizeof(ra8_translate_tok_flag_t) == 1U, "ra8_translate_tok_flag_t width");
static_assert(sizeof(ra8_translate_tok_enc_flag_t) == 1U, "ra8_translate_tok_enc_flag_t width");
static_assert(sizeof(ra8_translate_tok_const_t) == 4U, "ra8_translate_tok_const_t width");

/** @brief Validated `TVOC` SentencePiece vocabulary container header. */
typedef struct ra8_translate_vocab_header_s {
  uint32_t total_bytes;
  uint32_t container_crc32;
  uint32_t vocab_size;
  uint32_t unk_id;
  uint32_t bos_id;
  uint32_t eos_id;
  uint32_t pad_id;
  float    min_normal_score;
  uint8_t  model_type;
  uint8_t  flags;
  uint8_t  reserved[2];
} ra8_translate_vocab_header_t;

static_assert(sizeof(ra8_translate_vocab_header_t) == 36U, "ra8_translate_vocab_header_t size");
static_assert(alignof(ra8_translate_vocab_header_t) == 4U, "ra8_translate_vocab_header_t align");
static_assert(offsetof(ra8_translate_vocab_header_t, total_bytes) == 0U, "vocab total_bytes off");
static_assert(offsetof(ra8_translate_vocab_header_t, container_crc32) == 4U, "vocab crc off");
static_assert(offsetof(ra8_translate_vocab_header_t, vocab_size) == 8U, "vocab size off");
static_assert(offsetof(ra8_translate_vocab_header_t, unk_id) == 12U, "vocab unk_id off");
static_assert(offsetof(ra8_translate_vocab_header_t, bos_id) == 16U, "vocab bos_id off");
static_assert(offsetof(ra8_translate_vocab_header_t, eos_id) == 20U, "vocab eos_id off");
static_assert(offsetof(ra8_translate_vocab_header_t, pad_id) == 24U, "vocab pad_id off");
static_assert(offsetof(ra8_translate_vocab_header_t, min_normal_score) == 28U,
              "vocab min_normal_score off");
static_assert(offsetof(ra8_translate_vocab_header_t, model_type) == 32U, "vocab model_type off");
static_assert(offsetof(ra8_translate_vocab_header_t, flags) == 33U, "vocab flags off");
static_assert(offsetof(ra8_translate_vocab_header_t, reserved) == 34U, "vocab reserved off");

/**
 * @brief Validate a serialized `TVOC` SentencePiece vocabulary container.
 *
 * @details Calling context: `task-safe-reentrant`. Verifies header fields,
 * CRC-32 checksum, piece UTF-8 validity, and sorted lookup permutation before
 * writing @p out_header.
 *
 * @param[in] tvoc_buf Serialized `TVOC` container bytes.
 * @param[in] tvoc_len Byte length of @p tvoc_buf.
 * @param[out] out_header Non-null destination for validated vocabulary metadata.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_tok_open(const uint8_t*                tvoc_buf,
                                 size_t                        tvoc_len,
                                 ra8_translate_vocab_header_t* out_header);

/**
 * @brief Inspect metadata and borrowed UTF-8 piece slice for one vocabulary ID.
 *
 * @details Calling context: `task-safe-reentrant`. Returned @p out_piece_ptr
 * borrows directly from @p tvoc_buf while @p tvoc_buf remains live.
 *
 * @param[in] tvoc_buf Validated `TVOC` container bytes.
 * @param[in] tvoc_len Byte length of @p tvoc_buf.
 * @param[in] token_id Token identifier in `[0, vocab_size)`.
 * @param[out] out_piece_ptr Non-null out-pointer receiving UTF-8 piece base.
 * @param[out] out_piece_len Non-null storage receiving piece byte length.
 * @param[out] out_kind Non-null storage receiving ::ra8_translate_tok_piece_kind_t.
 * @param[out] out_score Non-null storage receiving Unigram log-prob or BPE rank score.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_tok_piece_info(const uint8_t*                  tvoc_buf,
                                       size_t                          tvoc_len,
                                       uint32_t                        token_id,
                                       const uint8_t**                 out_piece_ptr,
                                       uint16_t*                       out_piece_len,
                                       ra8_translate_tok_piece_kind_t* out_kind,
                                       float*                          out_score);

/**
 * @brief Compute conservative scratch buffer size required by ::ra8_translate_tok_encode.
 *
 * @details Calling context: `task-safe-reentrant`. Pure arithmetic bound based
 * on input UTF-8 byte length and model family.
 *
 * @param[in] text_len Input UTF-8 byte length in bytes.
 * @param[in] model_type Tokenizer model family (Unigram or BPE).
 * @param[out] out_work_bytes Non-null storage receiving required scratch byte count.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_tok_work_bytes(size_t                    text_len,
                                       ra8_translate_tok_model_t model_type,
                                       size_t*                   out_work_bytes);

/**
 * @brief Encode UTF-8 text into SentencePiece token IDs using caller scratch storage.
 *
 * @details Calling context: `task-safe-reentrant`. Allocates zero heap memory;
 * all normalization and Viterbi/BPE state lives in @p work_buf. Outputs
 * @p out_ids and @p out_id_count are untouched if any error occurs.
 *
 * @param[in] tvoc_buf Validated `TVOC` container bytes.
 * @param[in] tvoc_len Byte length of @p tvoc_buf.
 * @param[in] text_utf8 UTF-8 input string (NULL allowed only if @p text_len == 0).
 * @param[in] text_len Byte length of @p text_utf8.
 * @param[in] enc_flags Bitmask of ::ra8_translate_tok_enc_flag_t values.
 * @param[in,out] work_buf Non-null scratch buffer sized at least ::ra8_translate_tok_work_bytes.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[out] out_ids Non-null destination array receiving encoded token IDs.
 * @param[in] id_cap Element capacity of @p out_ids.
 * @param[out] out_id_count Non-null storage receiving token count written.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_tok_encode(const uint8_t* tvoc_buf,
                                   size_t         tvoc_len,
                                   const uint8_t* text_utf8,
                                   size_t         text_len,
                                   uint8_t        enc_flags,
                                   uint8_t*       work_buf,
                                   size_t         work_cap,
                                   uint32_t*      out_ids,
                                   size_t         id_cap,
                                   size_t*        out_id_count);

/**
 * @brief Decode SentencePiece token IDs back to UTF-8 text transactionally.
 *
 * @details Calling context: `task-safe-reentrant`. Strips control/unused tokens,
 * renders unknown tokens as SentencePiece unk_surface, reassembles contiguous
 * byte fallback tokens into valid UTF-8, and unescapes leading dummy metaspace.
 *
 * @param[in] tvoc_buf Validated `TVOC` container bytes.
 * @param[in] tvoc_len Byte length of @p tvoc_buf.
 * @param[in] ids Input token ID array (NULL allowed only if @p id_count == 0).
 * @param[in] id_count Element count of @p ids.
 * @param[out] out_utf8 Non-null destination buffer for decoded UTF-8 bytes.
 * @param[in] out_cap Byte capacity of @p out_utf8.
 * @param[out] out_len Non-null storage receiving decoded byte length.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_tok_decode(const uint8_t*  tvoc_buf,
                                   size_t          tvoc_len,
                                   const uint32_t* ids,
                                   size_t          id_count,
                                   uint8_t*        out_utf8,
                                   size_t          out_cap,
                                   size_t*         out_len);

#ifdef __cplusplus
}
#endif
