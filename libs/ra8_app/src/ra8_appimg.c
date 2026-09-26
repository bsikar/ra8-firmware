/**
 * @file ra8_appimg.c
 * @brief `.ra8app` header parsing, span reporting and capability checking (#662).
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / UI] {World: NS}
 *
 * @details
 * Default-deny throughout: each entry point proves its inputs before it writes
 * an output, and every refusal leaves the caller's output untouched or zeroed.
 * No allocation, no I/O and no cryptography live here -- the file is the format
 * and nothing more.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_appimg.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Report whether a fixed-width text field terminates inside its width.
 * @param[in] field Field base pointer.
 * @param[in] cap   Field width in bytes, including the NUL.
 * @return true when a NUL appears at or before `cap - 1`.
 */
RA8_INTERNAL static bool
internal_field_terminated(const char* field, size_t cap)
{
  for (size_t i = 0U; i < cap; ++i) {
    if (field[i] == '\0') {
      return true;
    }
  }
  return false;
}

/**
 * @brief Prove the fixed content fields of a candidate header.
 * @param[in] hdr Candidate header copied out of the file image.
 * @return k_ra8_ok when magic, version, capabilities and text fields are sound.
 */
RA8_INTERNAL static ra8_err_t
internal_check_identity(const ra8_appimg_header_t* hdr)
{
  if ((hdr->magic != (uint32_t)k_ra8_appimg_magic) ||
      (hdr->format_version != (uint32_t)k_ra8_appimg_format_version)) {
    return k_ra8_err_validation_failed;
  }
  if ((hdr->capabilities & ~(uint32_t)k_ra8_appimg_cap_known) != 0U) {
    return k_ra8_err_validation_failed;
  }
  if (!internal_field_terminated(hdr->app_id, (size_t)k_ra8_appimg_app_id_cap) ||
      !internal_field_terminated(hdr->display_name, (size_t)k_ra8_appimg_name_cap)) {
    return k_ra8_err_validation_failed;
  }
  if (hdr->app_id[0] == '\0') {
    return k_ra8_err_validation_failed;
  }
  if (hdr->min_api_version > (uint32_t)k_ra8_appimg_api_version_current) {
    return k_ra8_err_not_supported;
  }
  return k_ra8_ok;
}

/**
 * @brief Prove the declared sizes of a candidate header against the file.
 * @param[in] hdr Candidate header copied out of the file image.
 * @param[in] len Total readable file length, bytes.
 * @return k_ra8_ok when every declared length fits its cap and the file.
 */
RA8_INTERNAL static ra8_err_t
internal_check_sizes(const ra8_appimg_header_t* hdr, size_t len)
{
  if ((hdr->code_size == 0U) || (hdr->code_size > (uint32_t)k_ra8_appimg_segment_max) ||
      (hdr->data_size > (uint32_t)k_ra8_appimg_segment_max)) {
    return k_ra8_err_out_of_range;
  }
  if ((hdr->stack_size < (uint32_t)k_ra8_appimg_stack_min) ||
      (hdr->stack_size > (uint32_t)k_ra8_appimg_segment_max)) {
    return k_ra8_err_out_of_range;
  }
  if (hdr->entry_offset >= hdr->code_size) {
    return k_ra8_err_out_of_range;
  }
  const size_t payload = (size_t)hdr->code_size + (size_t)hdr->data_size;
  if (len < (sizeof(ra8_appimg_header_t) + payload)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_appimg_parse(const uint8_t* bytes, size_t len, ra8_appimg_header_t* out)
{
  if ((bytes == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (len < sizeof(ra8_appimg_header_t)) {
    return k_ra8_err_invalid_size;
  }

  ra8_appimg_header_t candidate = {};
  (void)memcpy(&candidate, bytes, sizeof(candidate));

  const ra8_err_t identity = internal_check_identity(&candidate);
  if (identity != k_ra8_ok) {
    return identity;
  }
  const ra8_err_t sizes = internal_check_sizes(&candidate, len);
  if (sizes != k_ra8_ok) {
    return sizes;
  }

  *out = candidate;
  return k_ra8_ok;
}

ra8_err_t
ra8_appimg_signed_span(const ra8_appimg_header_t* header, size_t len, ra8_appimg_span_t* out)
{
  if (out != nullptr) {
    *out = (ra8_appimg_span_t){};
  }
  if ((header == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (len < sizeof(ra8_appimg_header_t)) {
    return k_ra8_err_invalid_size;
  }

  out->offset = 0U;
  out->length = (uint32_t)offsetof(ra8_appimg_header_t, signature);
  return k_ra8_ok;
}

ra8_err_t
ra8_appimg_payload_span(const ra8_appimg_header_t* header, size_t len, ra8_appimg_span_t* out)
{
  if (out != nullptr) {
    *out = (ra8_appimg_span_t){};
  }
  if ((header == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const size_t payload = (size_t)header->code_size + (size_t)header->data_size;
  if (len < (sizeof(ra8_appimg_header_t) + payload)) {
    return k_ra8_err_invalid_size;
  }

  out->offset = (uint32_t)sizeof(ra8_appimg_header_t);
  out->length = (uint32_t)payload;
  return k_ra8_ok;
}

ra8_err_t
ra8_appimg_capabilities_permitted(const ra8_appimg_header_t* header, uint32_t granted)
{
  if (header == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((granted & ~(uint32_t)k_ra8_appimg_cap_known) != 0U) {
    return k_ra8_err_validation_failed;
  }
  if ((header->capabilities & ~granted) != 0U) {
    return k_ra8_err_access_denied;
  }
  return k_ra8_ok;
}
