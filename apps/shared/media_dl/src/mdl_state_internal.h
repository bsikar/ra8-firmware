/**
 * @file mdl_state_internal.h
 * @brief Module-private validation shared by the state model and codec.
 * @details Declares bounded validation, exact decimal conversion, and streamed parsing seams.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mdl_state.h"
#include "ra8_attributes.h"

/** @brief Largest complete serialized record including its terminator. */
typedef enum : uint16_t {
  k_mdl_state_line_max = (uint16_t)k_mdl_url_max + (uint16_t)k_mdl_title_max +
                         (uint16_t)k_mdl_chapter_id_max + 192U, /**< Serialized line cap. */
} mdl_state_internal_limit_t;

/**
 * @brief Copy one optional value into a bounded state field.
 * @details Copies exact validated text while permitting an absent optional value.
 * @param[out] dst Destination buffer.
 * @param[in] cap Destination capacity.
 * @param[in] val Optional NUL-terminated value.
 * @pre @p dst is non-NULL and @p cap is nonzero.
 * @pre Non-NULL @p val fits including its terminator.
 * @post A non-NULL value is copied with NUL termination; NULL leaves @p dst unchanged.
 * @post No byte beyond @p cap is modified.
 * @note Validation belongs to the caller.
 * @since 0.1.0
 */
RA8_PRIV void priv_mdl_state_set_opt(char* dst, size_t cap, const char* val);

/**
 * @brief Validate one delimiter-free bounded state field.
 * @details Measures through a fixed cap and rejects TAB, CR, and LF record delimiters.
 * @param[in] text Candidate NUL-terminated field.
 * @param[in] cap Persisted field capacity.
 * @return Whether the complete value fits and contains no record delimiter.
 * @retval false NULL, unterminated, over-capacity, or delimiter-bearing input.
 * @pre @p cap is positive. @pre Candidate storage is readable through the terminator or cap.
 * @post Input remains unchanged. @post Success guarantees safe exact serialization.
 * @note Empty fields are valid.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_state_field_valid(const char* text, size_t cap);

/**
 * @brief Validate a bounded relative path stored in state.
 * @details Applies field bounds plus absolute-path and traversal rejection.
 * @param[in] path Candidate path.
 * @param[in] cap Persisted field capacity.
 * @return Whether the path is relative, bounded, and non-traversing.
 * @retval false Empty, absolute, delimiter-bearing, or traversing path.
 * @pre @p cap is positive. @pre Candidate storage is readable through the terminator or cap.
 * @post Input remains unchanged. @post Success is safe beneath the series root.
 * @note Both slash variants are treated conservatively.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_state_relative_path_valid(const char* path, size_t cap);

/**
 * @brief Validate all persisted state bounds and cross-field invariants.
 * @details Checks schema, counts, strings, chapter/page relationships, and finite numbers.
 * @param[in] st State model to inspect.
 * @return Whether @p st can be serialized or accepted after parsing.
 * @retval false Any persisted invariant is violated.
 * @pre @p st is non-NULL.
 * @pre The complete fixed-layout object is readable.
 * @post No state is modified.
 * @post Success qualifies the object for the current writer.
 * @note Validation does not touch the filesystem.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_state_valid(const mdl_state_t* st);

/**
 * @brief Convert one exact bounded decimal rational to binary64.
 * @details Uses fixed-capacity integer division and nearest-even rounding without libc conversion.
 * @param[in] mantissa Unsigned decimal significand (at most 17 digits).
 * @param[in] decimal_scale Signed power of ten applied to @p mantissa.
 * @param[in] negative Whether to set the binary64 sign bit.
 * @param[out] out Converted finite binary64 value.
 * @return Whether the exact value rounds to a non-underflowing finite binary64.
 * @retval false Scale, capacity, overflow, or nonzero underflow is invalid.
 * @pre @p out is non-NULL and @p decimal_scale is in [-400, 400].
 * @pre @p mantissa carries at most 17 decimal digits.
 * @post Success is correctly rounded to nearest, ties to even without locale or libc conversion.
 * @post Failure publishes no numeric result contract.
 * @note Signed zero is preserved.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mdl_state_decimal_to_binary64(uint64_t mantissa,
                                                 int32_t  decimal_scale,
                                                 bool     negative,
                                                 double*  out);

/**
 * @brief Parse one exact state payload from an open portable file.
 * @details Streams a declared payload extent and migrates supported schemas transactionally.
 * @param[in,out] storage Initialized storage scratch binding.
 * @param[in,out] file Open file containing the payload.
 * @param[in] offset First payload byte.
 * @param[in] length Exact payload byte count.
 * @param[in] max_schema_version Highest schema authorized by the containing format.
 * @param[out] st State model receiving the parsed and migrated payload.
 * @return Canonical parse/read status.
 * @retval k_ra8_err_invalid_state Malformed, truncated, trailing, or unsupported payload.
 * @pre All pointers are non-NULL and @p file is open for reading.
 * @pre Offset and length identify the authenticated payload extent.
 * @pre @p max_schema_version is a supported nonzero schema version.
 * @post Success replaces @p st with the complete migrated/current state.
 * @post Failure leaves @p st initialized empty.
 * @note Parsing uses bounded caller-owned scratch.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_state_parse_file(mdl_storage_t* storage,
                                             fw_fs_file_t*  file,
                                             uint64_t       offset,
                                             uint64_t       length,
                                             uint16_t       max_schema_version,
                                             mdl_state_t*   st);
